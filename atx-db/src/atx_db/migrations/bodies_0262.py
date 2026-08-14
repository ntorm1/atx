"""Govern price-controlled fundamental momentum."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT price-controlled fundamental momentum v1"
FACTOR_ID = "earnings_sue_price_momentum_residual_12_1"
SUE_FACTOR_ID = "earnings_standardized_unexpected_eps"


def _fundamental_momentum(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT SUE residual to 12-1 price momentum','fundamental_earnings',
            'Cross-sectional SUE rank residual after controlling for split-adjusted price momentum from session t-252 through t-21.',
            'residual(rank_cs(SUE) ~ 1 + rank_cs(split_adjusted_return[t-252,t-21]))',
            '["factor:earnings_standardized_unexpected_eps","market:equity_daily_bars"]',
            1,400,
            '{"method":"cross_sectional_ols","response":"sue_rank","covariates":["price_momentum_12_1_rank"],"intercept":true,"missing_control_policy":"drop"}',
            'rank_residual','higher_is_better','zscore',true,
            'The governed SUE parent and canonical bars through the formation close must be visible. Price momentum excludes the latest 21 trading sessions and uses only sessions t-252 through t-21.',
            'atx_db.fundamental_momentum','atx-db',?,
            '{"method":"rank_ols_residual_then_winsorize_zscore_cs","skip_sessions":21,"lookback_sessions":252,"maximum_reference_staleness_days":7,"winsor_limits":[0.01,0.01],"return_fitted_parameters":true}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    conn.executemany(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','fundamental_momentum',?,?,?)),
                  ?,?,?,?,?,?,1,?,400,true,?)
        """,
        [
            (
                FACTOR_ID,
                "factor",
                SUE_FACTOR_ID,
                FACTOR_ID,
                "factor",
                SUE_FACTOR_ID,
                SUE_FACTOR_ID,
                None,
                None,
                f"factor:{SUE_FACTOR_ID}",
                SOURCE,
            ),
            (
                FACTOR_ID,
                "market",
                "equity_daily_bars",
                FACTOR_ID,
                "market",
                "equity_daily_bars",
                None,
                None,
                "equity_daily_bars",
                "market:equity_daily_bars",
                SOURCE,
            ),
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=262, name="fundamental_momentum_price_control", up=_fundamental_momentum)
]
