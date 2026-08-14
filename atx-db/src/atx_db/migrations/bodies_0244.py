"""Govern point-in-time net long-term-debt financing."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT net debt financing v1"
FACTOR_ID = "financing_low_net_debt_financing"
PARENT_FACTOR_ID = "investment_conservative_asset_growth"


def _net_debt_financing(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT low net debt financing','fundamental_financing',
            'Negative net long-term debt proceeds, defined as issuance plus signed repayments, scaled by beginning total assets.',
            '-(lt_debt_issued+signed_lt_debt_repaid)/prior_total_assets',
            '["metric:lt_debt_issued","metric:lt_debt_repaid","factor:investment_conservative_asset_growth"]',
            1,550,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Both exact-accession 330-400 day debt-flow components must be visible at the governed month end; issuance must be nonnegative, signed repayment nonpositive, and no value is imputed.',
            'atx_db.net_debt_financing','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"annual_duration_days":[330,400],"sign_policy":"issuance_nonnegative_repayment_nonpositive","missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    dependencies = (
        ("factor", PARENT_FACTOR_ID, PARENT_FACTOR_ID, None),
        ("metric", "lt_debt_issued", None, "lt_debt_issued"),
        ("metric", "lt_debt_repaid", None, "lt_debt_repaid"),
    )
    for dependency_type, name, dependency_factor_id, dependency_metric_id in dependencies:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','net_debt_financing',?,?)),?,?,?, ?,?,NULL,
                      1,?,550,true,?)
            """,
            [
                FACTOR_ID,name,FACTOR_ID,dependency_type,name,
                dependency_factor_id,dependency_metric_id,
                f"{dependency_type}:{name}",SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=244, name="net_debt_financing_factor", up=_net_debt_financing)]
