"""Promote quarterly profitability as a decile-preserving router secondary key."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db decile-preserving conditional router v4"
FACTOR_ID = "composite_operating_profitability_or_net_issuance"
PRIMARY_FACTOR_ID = "profitability_operating_profitability"
FALLBACK_FACTOR_ID = "financing_low_net_share_issuance"
SECONDARY_FACTOR_ID = (
    "profitability_quarterly_operating_profitability_lagged_assets"
)


def _factor_edge(conn: duckdb.DuckDBPyConnection, dependency: str) -> None:
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','conditional_router_v4',?,?)),?,
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


def _decile_preserving_secondary(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        UPDATE factor_definition
        SET factor_name = 'Decile-preserving profitability router',
            description = 'Operating profitability or, when absent, low net share issuance determines the primary decile. Quarterly operating profits-to-lagged-assets orders names only within that decile, preserving the production top and bottom baskets.',
            expression = 'lexicographic_decile(coalesce(operating_profitability,low_net_share_issuance),quarterly_operating_profitability_lagged_assets)',
            input_ids_json = '["factor:profitability_operating_profitability","factor:financing_low_net_share_issuance","factor:profitability_quarterly_operating_profitability_lagged_assets"]',
            available_at_policy = 'The primary route uses visible operating profitability or visible net issuance. When quarterly profitability is visible on the same date it is used only as an intra-decile secondary key; otherwise primary within-decile order is retained.',
            source = ?,
            standardization_spec_json = '{"method":"primary_decile_then_secondary_rank_then_zscore_cs","primary_bucket_count":10,"primary":"profitability_operating_profitability","fallback":"financing_low_net_share_issuance","secondary":"profitability_quarterly_operating_profitability_lagged_assets","secondary_scope":"within_primary_decile","missing_secondary":"retain_primary_within_decile_rank","top_bottom_membership_preserved":true,"return_fitted_weights":false}'
        WHERE factor_id = ?
        """,
        [SOURCE, FACTOR_ID],
    )
    conn.execute("DELETE FROM factor_dependency_edges WHERE factor_id=?", [FACTOR_ID])
    for dependency in (PRIMARY_FACTOR_ID, FALLBACK_FACTOR_ID, SECONDARY_FACTOR_ID):
        _factor_edge(conn, dependency)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=223,
        name="decile_preserving_quarterly_profitability_router",
        up=_decile_preserving_secondary,
    )
]
