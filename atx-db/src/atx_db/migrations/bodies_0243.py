"""Govern point-in-time comprehensive net external financing."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT net external financing v1"
FACTOR_ID = "financing_low_external_financing"
PARENT_FACTOR_ID = "investment_conservative_asset_growth"


def _external_financing(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT low net external financing','fundamental_financing',
            'Negative annual net cash provided by financing activities scaled by beginning total assets. Capital distributions rank above net capital raising.',
            '-financing_cash_flow/prior_total_assets',
            '["metric:financing_cash_flow","factor:investment_conservative_asset_growth"]',
            1,550,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Annual financing cash flow must be an exact-accession 330-400 day duration visible at the governed month end; beginning assets and formation policy inherit from the PIT asset-growth parent; no value is imputed.',
            'atx_db.external_financing','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"annual_duration_days":[330,400],"missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    dependencies = (
        ("metric", "financing_cash_flow", None, "financing_cash_flow"),
        ("factor", PARENT_FACTOR_ID, PARENT_FACTOR_ID, None),
    )
    for dependency_type, name, dependency_factor_id, dependency_metric_id in dependencies:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','external_financing',?,?)),?,?,?, ?,?,NULL,
                      1,?,550,true,?)
            """,
            [
                FACTOR_ID,
                name,
                FACTOR_ID,
                dependency_type,
                name,
                dependency_factor_id,
                dependency_metric_id,
                f"{dependency_type}:{name}",
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=243, name="external_financing_factor", up=_external_financing)]
