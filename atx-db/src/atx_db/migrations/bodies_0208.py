"""TTM cash-flow profitability and cash-flow-statement accrual quality."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT cash-flow profitability v1"
FACTORS = (
    (
        "profitability_operating_cash_flow_to_assets",
        "PIT operating cash-flow profitability",
        "fundamental_profitability",
        "Trailing-twelve-month operating cash flow scaled by average total assets, using only values visible at a governed monthly universe decision.",
        "operating_cash_flow_ttm/average_total_assets",
    ),
    (
        "quality_low_total_accruals",
        "PIT low total accruals",
        "fundamental_quality",
        "Negative-oriented cash-flow-statement total accruals: TTM operating cash flow less TTM net income, scaled by average total assets.",
        "(operating_cash_flow_ttm-net_income_ttm)/average_total_assets",
    ),
)
METRICS = ("net_income_ttm", "operating_cash_flow_ttm", "total_assets")


def _cash_flow_profitability_factors(conn: duckdb.DuckDBPyConnection) -> None:
    for factor_id, factor_name, family, description, expression in FACTORS:
        conn.execute(
            """
            INSERT OR REPLACE INTO factor_definition (
                factor_id,factor_name,family,description,expression,input_ids_json,
                direction,lookback_days,neutralization_spec_json,unit,sign,scale,
                is_point_in_time_safe,available_at_policy,declared_in,owner,source,
                standardization_spec_json,valid_from,valid_to
            ) VALUES (?,?,?,?,?,
                '["metric:net_income_ttm","metric:operating_cash_flow_ttm","metric:total_assets","universe:us_common_equity_liquid_v1"]',
                1,550,'{"method":"none","by":[]}','normalized_score',
                'higher_is_better','zscore',true,
                'Visible at monthly close after both TTM inputs, current/prior assets, and the governed universe decision are visible.',
                'atx_db.cash_flow_profitability','atx-db',?,
                '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"currency":"USD","asset_match_days":31}',
                DATE '1900-01-01',NULL)
            """,
            [factor_id, factor_name, family, description, expression, SOURCE],
        )
        conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [factor_id])
        for metric in METRICS:
            conn.execute(
                """
                INSERT INTO factor_dependency_edges (
                    dependency_id,factor_id,dependency_type,dependency_name,
                    dependency_factor_id,dependency_metric_id,dependency_source_id,
                    dependency_depth,expression,lookback_days,is_direct,source
                ) VALUES (sha256(concat_ws('|','cash_flow_profitability',?,?)),?,
                          'metric',?,NULL,?,NULL,1,?,550,true,?)
                """,
                [factor_id, metric, factor_id, metric, metric, f"metric:{metric}", SOURCE],
            )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=208,
        name="cash_flow_profitability_factors",
        up=_cash_flow_profitability_factors,
    )
]
