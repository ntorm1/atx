"""Govern sales-adjusted quarterly inventory growth."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT quarterly abnormal inventory growth v1"
FACTOR_ID = "investment_low_quarterly_abnormal_inventory_growth"
CASH_FACTOR_ID = "profitability_quarterly_cash_operating_profitability_lagged_assets"
OPERATING_FACTOR_ID = "profitability_quarterly_operating_profitability_lagged_assets"


def _quarterly_abnormal_inventory_growth(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT low quarterly abnormal inventory growth','fundamental_investment',
            'Negatively oriented same-quarter year-over-year inventory growth in excess of sales growth.',
            '-((inventory_t/inventory_t_4-1)-(revenue_t/revenue_t_4-1))',
            '["factor:profitability_quarterly_cash_operating_profitability_lagged_assets","factor:profitability_quarterly_operating_profitability_lagged_assets"]',
            -1,500,'{"method":"none","by":[]}','normalized_score',
            'lower_is_better','zscore',true,
            'Uses the closest governed prior report 330-400 days before the current period and requires both factor rows to be visible by the current monthly decision.',
            'atx_db.quarterly_abnormal_inventory_growth','atx-db',?,
            '{"method":"negate_then_winsorize_then_zscore_cs","winsor_limits":[0.01,0.01],"period_gap_days":[330,400],"maximum_absolute_component_growth":10.0,"maximum_absolute_raw_value":10.0,"return_fitted_parameters":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dependency_factor_id in (CASH_FACTOR_ID, OPERATING_FACTOR_ID):
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (
                sha256(concat_ws('|','quarterly_abnormal_inventory',?,?)),?,
                'factor',?,?,NULL,NULL,1,?,500,true,?
            )
            """,
            [
                FACTOR_ID,
                dependency_factor_id,
                FACTOR_ID,
                dependency_factor_id,
                dependency_factor_id,
                f"factor:{dependency_factor_id}",
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=230,
        name="quarterly_abnormal_inventory_growth",
        up=_quarterly_abnormal_inventory_growth,
    )
]
