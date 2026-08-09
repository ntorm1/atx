"""Point-in-time rank-standardized QMJ profitability composite."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT QMJ profitability v1"
FACTOR_ID = "quality_qmj_profitability"
PIOTROSKI_FACTOR_ID = "quality_piotroski_f_score"
BOOK_TO_MARKET_FACTOR_ID = "value_book_to_market"


def _factor_edge(
    conn: duckdb.DuckDBPyConnection,
    dependency: str,
) -> None:
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','qmj_profitability',?,?)),?,
                  'factor',?,?,NULL,NULL,1,?,1100,true,?)
        """,
        [
            FACTOR_ID,
            dependency,
            FACTOR_ID,
            dependency,
            dependency,
            f"factor:{dependency}",
            SOURCE,
        ],
    )


def _qmj_profitability(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT QMJ profitability','fundamental_quality',
            'Equal-weight QMJ-style profitability composite of six cross-sectionally rank-standardized annual measures. Reported operating cash flow replaces the synthetic cash-flow proxy; weights are literature-specified and never return-fitted.',
            'zscore(mean(zrank(gross_profit/total_assets),zrank(net_income/book_equity),zrank(net_income/total_assets),zrank(operating_cash_flow/total_assets),zrank(gross_profit/revenue),zrank((operating_cash_flow-net_income)/total_assets)))',
            '["factor:quality_piotroski_f_score","factor:value_book_to_market"]',
            1,1100,'{"method":"none","by":[]}','score','higher_is_better','zscore',true,
            'All six annual inputs must be visible at the governed monthly decision; book equity must have the same fiscal period as the other annual inputs; missing components are not imputed.',
            'atx_db.qmj_profitability','atx-db',?,
            '{"method":"equal_weight_rank_z_then_composite_z","component_count":6,"rank_method":"average","complete_case":true,"return_fitted_weights":false,"cash_flow":"reported_operating_cash_flow"}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dependency in (PIOTROSKI_FACTOR_ID, BOOK_TO_MARKET_FACTOR_ID):
        _factor_edge(conn, dependency)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=213,
        name="qmj_profitability",
        up=_qmj_profitability,
    )
]
