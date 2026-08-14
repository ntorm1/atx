"""Govern twin profitability-price momentum."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT twin profitability-price momentum v1"
FACTOR_ID = "momentum_twin_profitability_trend_price_12_1"
TREND_FACTOR_ID = "profitability_quarterly_gross_profitability_trend_8q"


def _twin_momentum(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT twin profitability-trend and price momentum','fundamental_momentum',
            'Same-direction confirmation between a governed PIT eight-quarter gross-profitability trend and split-adjusted 12-1 price momentum.',
            'same_sign(F,P) * sign(F) * least(abs(F),abs(P)), F=2*rank_cs(profitability_trend)-1, P=2*rank_cs(return[t-252,t-21])-1',
            '["factor:profitability_quarterly_gross_profitability_trend_8q","market:equity_daily_bars"]',
            1,1000,
            '{"method":"same_direction_rank_confirmation","missing_policy":"drop","disagreement_policy":"zero"}',
            'rank_confirmation','higher_is_better','zscore',true,
            'The governed profitability parent and canonical price endpoints must be visible at formation; the latest 21 sessions are excluded.',
            'atx_db.twin_momentum','atx-db',?,
            '{"method":"rank_confirmation_then_winsorize_zscore_cs","skip_sessions":21,"lookback_sessions":252,"maximum_reference_staleness_days":7,"winsor_limits":[0.01,0.01],"return_fitted_parameters":false}',
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
        ) VALUES (sha256(concat_ws('|','twin_momentum',?,?,?)),
                  ?,?,?,?,?,?,1,?,1000,true,?)
        """,
        [
            (
                FACTOR_ID, "factor", TREND_FACTOR_ID, FACTOR_ID, "factor",
                TREND_FACTOR_ID, TREND_FACTOR_ID, None, None,
                f"factor:{TREND_FACTOR_ID}", SOURCE,
            ),
            (
                FACTOR_ID, "market", "equity_daily_bars", FACTOR_ID, "market",
                "equity_daily_bars", None, None, "equity_daily_bars",
                "market:equity_daily_bars", SOURCE,
            ),
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=264, name="twin_profitability_price_momentum", up=_twin_momentum)]
