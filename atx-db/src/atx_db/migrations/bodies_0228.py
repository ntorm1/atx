"""Govern low quarterly operating working-capital accruals output."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT low quarterly working-capital accruals v1"
FACTOR_ID = "quality_low_quarterly_operating_working_capital_accruals"
CASH_FACTOR_ID = "profitability_quarterly_cash_operating_profitability_lagged_assets"


def _quarterly_working_capital_accruals(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT low quarterly operating working-capital accruals',
            'fundamental_quality',
            'Negatively oriented quarterly receivables plus inventory changes minus deferred-revenue and payable changes, scaled by one-quarter-lagged assets.',
            '-(dAR+dInventory-dDeferredRevenue-dAP)/one_quarter_lagged_total_assets',
            '["factor:profitability_quarterly_cash_operating_profitability_lagged_assets"]',
            -1,330,'{"method":"none","by":[]}','normalized_score',
            'lower_is_better','zscore',true,
            'Uses the exact visible balance-change decomposition and lagged-assets denominator persisted in the governed quarterly cash-profitability row.',
            'atx_db.quarterly_working_capital_accruals','atx-db',?,
            '{"method":"negate_then_winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"maximum_absolute_raw_value":5.0,"missing_balance_changes":"inherited_zero_from_claq","return_fitted_parameters":false}',
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
        ) VALUES (sha256(concat_ws('|','quarterly_wc_accruals',?,?)),?,
                  'factor',?,?,NULL,NULL,1,?,330,true,?)
        """,
        [
            FACTOR_ID,
            CASH_FACTOR_ID,
            FACTOR_ID,
            CASH_FACTOR_ID,
            CASH_FACTOR_ID,
            f"factor:{CASH_FACTOR_ID}",
            SOURCE,
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=228,
        name="low_quarterly_working_capital_accruals",
        up=_quarterly_working_capital_accruals,
    )
]
