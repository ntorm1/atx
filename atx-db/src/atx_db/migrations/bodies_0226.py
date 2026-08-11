"""Govern quarterly cash operating profits-to-lagged-assets output."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT quarterly cash profitability v1"
FACTOR_ID = "profitability_quarterly_cash_operating_profitability_lagged_assets"
OPERATING_FACTOR_ID = "profitability_quarterly_operating_profitability_lagged_assets"
DEPENDENCY_METRICS = ("ar", "inventory", "deferred_revenue", "ap")


def _quarterly_cash_profitability(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT quarterly cash operating profits-to-lagged assets',
            'fundamental_profitability',
            'Quarterly operating profit minus receivables and inventory changes plus deferred-revenue and payable changes, scaled by one-quarter-lagged assets.',
            '(quarterly_operating_profit-dAR-dInventory+dDeferredRevenue+dAP)/one_quarter_lagged_total_assets',
            '["factor:profitability_quarterly_operating_profitability_lagged_assets","metric:ar","metric:inventory","metric:deferred_revenue","metric:ap"]',
            1,330,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Uses a governed quarterly operating-profitability decision and only current/prior balance facts visible by that same monthly close.',
            'atx_db.quarterly_cash_profitability','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"balance_change_period":"one_quarter","missing_balance_changes":"zero","maximum_absolute_raw_value":5.0,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','quarterly_cash_profitability',?,?)),?,
                  'factor',?,?,NULL,NULL,1,?,330,true,?)
        """,
        [
            FACTOR_ID,
            OPERATING_FACTOR_ID,
            FACTOR_ID,
            OPERATING_FACTOR_ID,
            OPERATING_FACTOR_ID,
            f"factor:{OPERATING_FACTOR_ID}",
            SOURCE,
        ],
    )
    for metric in DEPENDENCY_METRICS:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','quarterly_cash_profitability',?,?)),?,
                      'metric',?,NULL,?,NULL,1,?,330,true,?)
            """,
            [FACTOR_ID, metric, FACTOR_ID, metric, metric, f"metric:{metric}", SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=226,
        name="quarterly_cash_profitability_lagged_assets",
        up=_quarterly_cash_profitability,
    )
]
