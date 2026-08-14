"""Govern the cash-profitability level and first-change composite."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT cash-profitability level-growth composite v1"
FACTOR_ID = "composite_cash_profitability_level_growth"
CASH_FACTOR_ID = "profitability_cash_operating_profitability"


def _cash_profitability_level_growth(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT cash profitability level and growth','fundamental_composite',
            'Untuned equal-weight blend of cash operating profitability and its first annual change.',
            '0.5*z(profitability_cash_operating_profitability)+0.5*z(delta_1y(profitability_cash_operating_profitability))',
            '["factor:profitability_cash_operating_profitability"]',
            1,980,'{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Current and prior annual cash-profitability observations must both be known at formation; annual period ends must be 300 to 430 days apart.',
            'atx_db.cash_profitability_growth','atx-db',?,
            '{"method":"equal_weight_level_first_change_then_winsorize_zscore_cs","weights":{"level":0.5,"first_annual_change":0.5},"winsor_limits":[0.01,0.01],"weights_fitted_to_returns":false,"missing_components_imputed":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    conn.execute(
        """INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','cash_profitability_level_growth',?,?)),
                  ?,'factor',?,?,NULL,NULL,1,?,980,true,?)""",
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
        version=254,
        name="cash_profitability_level_growth_composite",
        up=_cash_profitability_level_growth,
    )
]
