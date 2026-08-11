"""Govern quarterly operating profits-to-lagged-assets output."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT quarterly operating profitability v1"
FACTOR_ID = "profitability_quarterly_operating_profitability_lagged_assets"
DEPENDENCY_METRICS = (
    "revenue",
    "cogs",
    "gross_profit",
    "sga",
    "rd_expense",
    "total_assets",
)


def _quarterly_operating_profitability(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT quarterly operating profits-to-lagged assets',
            'fundamental_profitability',
            'Latest SEC-visible quarterly revenue minus COGS minus SG&A plus R&D, scaled by one-quarter-lagged total assets.',
            '(coalesce(revenue-cogs,gross_profit)-sga+coalesce(rd_expense,0))/one_quarter_lagged_total_assets',
            '["metric:revenue","metric:cogs","metric:gross_profit","metric:sga","metric:rd_expense","metric:total_assets"]',
            1,330,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Quarterly duration inputs and lagged assets must be SEC-visible by the monthly close; current quarter age is at most 200 days.',
            'atx_db.quarterly_operating_profitability','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"quarter_duration_days":[70,115],"lagged_assets_gap_days":[60,130],"gross_profit_fallback":"reported_gross_profit","missing_rd_expense":"zero","return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for metric in DEPENDENCY_METRICS:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','quarterly_op_lagged_assets',?,?)),?,
                      'metric',?,NULL,?,NULL,1,?,330,true,?)
            """,
            [
                FACTOR_ID,
                metric,
                FACTOR_ID,
                metric,
                metric,
                f"metric:{metric}",
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=221,
        name="quarterly_operating_profitability_lagged_assets",
        up=_quarterly_operating_profitability,
    )
]
