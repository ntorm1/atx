"""Govern point-in-time financing-side net operating assets."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT financing-side net operating assets v1"
FACTOR_ID = "quality_net_operating_assets"
METRICS = ("total_assets", "total_liabilities", "cash_st_inv", "st_debt", "lt_debt")


def _net_operating_assets(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT low net operating assets','fundamental_quality',
            'Negative financing-side net operating assets scaled by prior annual assets. Low cumulative accounting-minus-cash value is preferred under the Hirshleifer-Hou-Teoh-Zhang sustainability hypothesis.',
            '-((total_assets-cash_st_inv-total_liabilities+st_debt+lt_debt)/prior_total_assets)',
            '["metric:total_assets","metric:total_liabilities","metric:cash_st_inv","metric:st_debt","metric:lt_debt","universe:us_common_equity_liquid_v1"]',
            1,550,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Every same-accession current component and the consecutive prior-year asset denominator must be visible by the governed monthly close; no component is imputed.',
            'atx_db.net_operating_assets','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"annual_period_gap_days":[300,430],"short_term_debt_policy":"canonical_prioritized_current_debt","missing_components_imputed":false,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for metric in METRICS:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','net_operating_assets',?,?)),?,
                      'metric',?,NULL,?,NULL,1,?,550,true,?)
            """,
            [FACTOR_ID, metric, FACTOR_ID, metric, metric, f"metric:{metric}", SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=242, name="net_operating_assets_factor", up=_net_operating_assets)
]
