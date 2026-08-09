"""Day-level market-adjusted SEC filing-reaction factor."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT SEC filing reaction v1"
FACTOR_ID = "earnings_sec_filing_reaction"
SUE_FACTOR_ID = "earnings_standardized_unexpected_eps"


def _filing_reaction_factor(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT market-adjusted SEC filing reaction','fundamental_earnings',
            'First complete trading-session return strictly after the SEC filed date, less the cross-sectional median return; this is not an intraday earnings-announcement return.',
            'post_filing_session_total_return - cross_sectional_median_return',
            ?,1,7,'{"method":"market_median","by":["trade_date"]}',
            'return','higher_is_better','zscore',true,
            'Visible only after the post-filing trading session closes; companyfacts timestamps are day-level synthetic 22:00 values.',
            'atx_db.filing_reaction','atx-db',?,
            '{"method":"winsorize_then_zscore_cs","winsor_limits":[0.01,0.01]}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, f'["factor:{SUE_FACTOR_ID}","market:post_filing_session_return"]', SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dep_type, dep_name, dep_factor in (
        ("factor", SUE_FACTOR_ID, SUE_FACTOR_ID),
        ("market", "post_filing_session_return", None),
    ):
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','filing_reaction',?,?,?)),?,?,?,?,?,NULL,
                      1,?,7,true,?)
            """,
            [FACTOR_ID, dep_type, dep_name, FACTOR_ID, dep_type, dep_name, dep_factor,
             None if dep_type == "factor" else dep_name, f"{dep_type}:{dep_name}", SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=203, name="sec_filing_reaction_factor", up=_filing_reaction_factor)]
