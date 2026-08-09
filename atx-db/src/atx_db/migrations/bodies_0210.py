"""Production point-in-time Piotroski financial-strength factors."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT Piotroski F-score v1"
FACTOR_ID = "quality_piotroski_f_score"
HIGH_BOOK_TO_MARKET_FACTOR_ID = "quality_piotroski_high_book_to_market"
BOOK_TO_MARKET_FACTOR_ID = "value_book_to_market"
NET_ISSUANCE_FACTOR_ID = "financing_low_net_share_issuance"
METRIC_DEPENDENCIES = (
    "net_income",
    "operating_cash_flow",
    "revenue",
    "gross_profit",
    "total_assets",
    "current_assets",
    "current_liabilities",
    "lt_debt",
)


def _insert_factor_edge(
    conn: duckdb.DuckDBPyConnection,
    factor_id: str,
    dependency: str,
) -> None:
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','piotroski_factor',?,?)),?,
                  'factor',?,?,NULL,NULL,1,?,1100,true,?)
        """,
        [
            factor_id,
            dependency,
            factor_id,
            dependency,
            dependency,
            f"factor:{dependency}",
            SOURCE,
        ],
    )


def _insert_metric_edge(
    conn: duckdb.DuckDBPyConnection,
    factor_id: str,
    metric: str,
) -> None:
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','piotroski_metric',?,?)),?,
                  'metric',?,NULL,?,NULL,1,?,1100,true,?)
        """,
        [factor_id, metric, factor_id, metric, metric, f"metric:{metric}", SOURCE],
    )


def _production_piotroski(conn: duckdb.DuckDBPyConnection) -> None:
    main_inputs = (
        '["factor:value_book_to_market",'
        '"factor:financing_low_net_share_issuance",'
        '"metric:net_income","metric:operating_cash_flow","metric:revenue",'
        '"metric:gross_profit","metric:total_assets","metric:current_assets",'
        '"metric:current_liabilities","metric:lt_debt"]'
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT Piotroski F-score','fundamental_quality',
            'Complete-case annual 0-9 Piotroski financial-strength score using beginning assets for ROA and turnover, average assets for leverage, and split-adjusted net issuance for the equity-offering signal.',
            'F_ROA + F_CFO + F_DELTA_ROA + F_ACCRUAL + F_DELTA_LEVER + F_DELTA_LIQUID + EQ_OFFER + F_DELTA_MARGIN + F_DELTA_TURN',
            ?,1,1100,'{"method":"none","by":[]}','score','higher_is_better','zscore',true,
            'Annual statement inputs and prior periods must be visible by the governed monthly decision; missing signals are not imputed.',
            'atx_db.piotroski','atx-db',?,
            '{"method":"zscore_cs","raw_range":[0,9],"annual_duration_days":[330,380],"complete_case":true,"equity_offer_proxy":"split_adjusted_net_share_issuance_le_zero"}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, main_inputs, SOURCE],
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT Piotroski F-score within high book-to-market','fundamental_quality',
            'Piotroski F-score restricted to securities at or above the point-in-time 80th percentile of book-to-market, matching the original conditional research design.',
            'quality_piotroski_f_score where percent_rank(value_book_to_market) >= 0.80',
            ?,1,1100,'{"method":"universe_condition","factor":"value_book_to_market","minimum_percentile":0.8}',
            'score','higher_is_better','zscore',true,
            'Book-to-market membership and every annual input must be visible at the governed monthly decision.',
            'atx_db.piotroski','atx-db',?,
            '{"method":"zscore_cs_within_condition","raw_range":[0,9],"condition":"book_to_market_percentile_ge_0.80","complete_case":true}',
            DATE '1900-01-01',NULL
        )
        """,
        [
            HIGH_BOOK_TO_MARKET_FACTOR_ID,
            '["factor:quality_piotroski_f_score","factor:value_book_to_market"]',
            SOURCE,
        ],
    )

    for factor_id in (FACTOR_ID, HIGH_BOOK_TO_MARKET_FACTOR_ID):
        conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [factor_id])
    for dependency in (BOOK_TO_MARKET_FACTOR_ID, NET_ISSUANCE_FACTOR_ID):
        _insert_factor_edge(conn, FACTOR_ID, dependency)
    for metric in METRIC_DEPENDENCIES:
        _insert_metric_edge(conn, FACTOR_ID, metric)
    for dependency in (FACTOR_ID, BOOK_TO_MARKET_FACTOR_ID):
        _insert_factor_edge(conn, HIGH_BOOK_TO_MARKET_FACTOR_ID, dependency)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=210, name="production_piotroski_f_score", up=_production_piotroski)]
