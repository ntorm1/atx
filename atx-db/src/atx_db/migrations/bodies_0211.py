"""Continuous rank-standardized annual financial strength."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT continuous financial strength v1"
FACTOR_ID = "quality_continuous_financial_strength"
HIGH_BOOK_TO_MARKET_FACTOR_ID = (
    "quality_continuous_financial_strength_high_book_to_market"
)
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


def _factor_edge(
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
        ) VALUES (sha256(concat_ws('|','continuous_strength_factor',?,?)),?,
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


def _metric_edge(
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
        ) VALUES (sha256(concat_ws('|','continuous_strength_metric',?,?)),?,
                  'metric',?,NULL,?,NULL,1,?,1100,true,?)
        """,
        [factor_id, metric, factor_id, metric, metric, f"metric:{metric}", SOURCE],
    )


def _continuous_financial_strength(conn: duckdb.DuckDBPyConnection) -> None:
    inputs = (
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
            ?,'PIT continuous financial strength','fundamental_quality',
            'Equal-weight mean of rank-standardized continuous representations of the nine annual Piotroski dimensions. Uses a fixed literature-specified transform and no return-fitted weights.',
            'mean(zrank(ROA),zrank(CFOA),zrank(delta_ROA),zrank(low_accruals),zrank(low_delta_leverage),zrank(delta_liquidity),zrank(low_net_issuance),zrank(delta_gross_margin),zrank(delta_asset_turnover))',
            ?,1,1100,'{"method":"none","by":[]}','score','higher_is_better','zscore',true,
            'All current, prior, and beginning-period inputs must be visible at the governed monthly decision; missing components are not imputed.',
            'atx_db.continuous_financial_strength','atx-db',?,
            '{"method":"equal_weight_rank_z_then_composite_z","component_count":9,"rank_method":"average","complete_case":true,"return_fitted_weights":false}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, inputs, SOURCE],
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT continuous financial strength within high book-to-market',
            'fundamental_quality',
            'Continuous financial strength restricted to the point-in-time top book-to-market quintile.',
            'quality_continuous_financial_strength where percent_rank(value_book_to_market) >= 0.80',
            '["factor:quality_continuous_financial_strength","factor:value_book_to_market"]',
            1,1100,'{"method":"universe_condition","factor":"value_book_to_market","minimum_percentile":0.8}',
            'score','higher_is_better','zscore',true,
            'Book-to-market membership and all continuous component inputs must be visible at the governed monthly decision.',
            'atx_db.continuous_financial_strength','atx-db',?,
            '{"method":"zscore_cs_within_condition","condition":"book_to_market_percentile_ge_0.80","complete_case":true}',
            DATE '1900-01-01',NULL
        )
        """,
        [HIGH_BOOK_TO_MARKET_FACTOR_ID, SOURCE],
    )
    for factor_id in (FACTOR_ID, HIGH_BOOK_TO_MARKET_FACTOR_ID):
        conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [factor_id])
    for dependency in (BOOK_TO_MARKET_FACTOR_ID, NET_ISSUANCE_FACTOR_ID):
        _factor_edge(conn, FACTOR_ID, dependency)
    for metric in METRIC_DEPENDENCIES:
        _metric_edge(conn, FACTOR_ID, metric)
    for dependency in (FACTOR_ID, BOOK_TO_MARKET_FACTOR_ID):
        _factor_edge(conn, HIGH_BOOK_TO_MARKET_FACTOR_ID, dependency)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=211,
        name="continuous_financial_strength",
        up=_continuous_financial_strength,
    )
]
