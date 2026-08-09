"""Same-sign revenue-confirmed SUE sleeve."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db PIT SUE revenue agreement v1"
FACTOR_ID = "earnings_sue_revenue_agreement"
INPUTS = (
    "earnings_standardized_unexpected_eps",
    "earnings_standardized_unexpected_revenue",
)


def _earnings_revenue_agreement_factor(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        INSERT OR REPLACE INTO factor_definition (
            factor_id,factor_name,family,description,expression,input_ids_json,
            direction,lookback_days,neutralization_spec_json,unit,sign,scale,
            is_point_in_time_safe,available_at_policy,declared_in,owner,source,
            standardization_spec_json,valid_from,valid_to
        ) VALUES (
            ?,'PIT SUE with same-sign revenue confirmation','fundamental_earnings',
            'High-conviction standardized unexpected EPS sleeve retained only when independently standardized unexpected revenue has the same sign.',
            'zscore(sue | sue*revenue_surprise>0)',?,1,150,
            '{"method":"none","by":[]}','normalized_score',
            'higher_is_better','zscore',true,
            'Visible at the later upstream available_at; absent rather than neutral when signs disagree or revenue history is unavailable.',
            'atx_db.earnings_revenue_agreement','atx-db',?,
            '{"method":"same_sign_gate_then_zscore_cs","gate":"sue*revenue_surprise>0"}',
            DATE '1900-01-01',NULL
        )
        """,
        [
            FACTOR_ID,
            '["factor:earnings_standardized_unexpected_eps","factor:earnings_standardized_unexpected_revenue"]',
            SOURCE,
        ],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for input_factor in INPUTS:
        conn.execute(
            """
            INSERT INTO factor_dependency_edges (
                dependency_id,factor_id,dependency_type,dependency_name,
                dependency_factor_id,dependency_metric_id,dependency_source_id,
                dependency_depth,expression,lookback_days,is_direct,source
            ) VALUES (sha256(concat_ws('|','earnings_revenue_agreement',?,?)),?,
                      'factor',?,?,NULL,NULL,1,?,150,true,?)
            """,
            [
                FACTOR_ID,
                input_factor,
                FACTOR_ID,
                input_factor,
                input_factor,
                f"factor:{input_factor}",
                SOURCE,
            ],
        )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=207,
        name="earnings_revenue_agreement_factor",
        up=_earnings_revenue_agreement_factor,
    )
]
