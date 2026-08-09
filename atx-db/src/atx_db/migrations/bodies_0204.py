"""SUE and SEC filing-reaction confirmation factor."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT earnings confirmation composite v1"
FACTOR_ID = "earnings_sue_filing_confirmation"
INPUTS = ("earnings_standardized_unexpected_eps", "earnings_sec_filing_reaction")


def _earnings_confirmation_factor(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT SUE and SEC filing-reaction confirmation','fundamental_earnings',
            'Equal-weight intersection of standardized unexpected EPS and the market-adjusted first complete session after the SEC filed date.',
            'zscore(0.5*sue + 0.5*sec_filing_reaction)',?,1,150,
            '{"method":"market_median","by":["filing_reaction_trade_date"]}',
            'normalized_score','higher_is_better','zscore',true,
            'Visible at the later upstream available_at; both SUE and filing reaction are required.',
            'atx_db.earnings_confirmation','atx-db',?,
            '{"method":"equal_weight_then_zscore_cs","weights":[0.5,0.5]}',
            DATE '1900-01-01',NULL
        )
        """,
        [FACTOR_ID, '["factor:earnings_standardized_unexpected_eps","factor:earnings_sec_filing_reaction"]', SOURCE],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for input_factor in INPUTS:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','earnings_confirmation',?,?)),?,
                      'factor',?,?,NULL,NULL,1,?,150,true,?)
            """,
            [FACTOR_ID, input_factor, FACTOR_ID, input_factor, input_factor,
             f"factor:{input_factor}", SOURCE],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=204, name="earnings_confirmation_factor", up=_earnings_confirmation_factor)]
