"""Roll back the quarterly-ROE router overlay after tail-performance validation."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db conditional OP/issuance router v3"
FACTOR_ID = "composite_operating_profitability_or_net_issuance"
PRIMARY_FACTOR_ID = "profitability_operating_profitability"
FALLBACK_FACTOR_ID = "financing_low_net_share_issuance"


def _factor_edge(conn: duckdb.DuckDBPyConnection, dependency: str) -> None:
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','conditional_router_v3',?,?)),?,
                  'factor',?,?,NULL,NULL,1,?,550,true,?)
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


def _restore_primary_else_fallback(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        UPDATE factor_definition
        SET factor_name = 'Conditional operating profitability or net issuance',
            description = 'Operating profitability when available; otherwise low net share issuance. The quarterly-ROE overlay was rejected because it weakened long-short tail performance and raised turnover.',
            expression = 'coalesce(profitability_operating_profitability,financing_low_net_share_issuance)',
            input_ids_json = '["factor:profitability_operating_profitability","factor:financing_low_net_share_issuance"]',
            available_at_policy = 'Uses operating profitability when visible at the monthly decision; otherwise uses visible split-adjusted net share issuance.',
            source = ?,
            standardization_spec_json = '{"method":"conditional_primary_else_fallback_then_zscore_cs","primary":"profitability_operating_profitability","fallback":"financing_low_net_share_issuance","rejected_overlay":"profitability_q_factor_roe","rejection_reason":"weaker_decile_spread_and_higher_turnover"}'
        WHERE factor_id = ?
        """,
        [SOURCE, FACTOR_ID],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dependency in (PRIMARY_FACTOR_ID, FALLBACK_FACTOR_ID):
        _factor_edge(conn, dependency)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=217,
        name="restore_primary_fallback_router",
        up=_restore_primary_else_fallback,
    )
]
