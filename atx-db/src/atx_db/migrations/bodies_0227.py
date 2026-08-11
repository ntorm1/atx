"""Promote quarterly cash profitability as the production router secondary key."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db governed cash-decile router v6"
FACTOR_ID = "composite_operating_profitability_or_net_issuance"
PRIMARY_FACTOR_ID = "profitability_operating_profitability"
FALLBACK_FACTOR_ID = "financing_low_net_share_issuance"
SECONDARY_FACTOR_ID = (
    "profitability_quarterly_cash_operating_profitability_lagged_assets"
)


def _factor_edge(conn: duckdb.DuckDBPyConnection, dependency: str) -> None:
    conn.execute(
        """
        INSERT INTO factor_dependency_edges (
            dependency_id,factor_id,dependency_type,dependency_name,
            dependency_factor_id,dependency_metric_id,dependency_source_id,
            dependency_depth,expression,lookback_days,is_direct,source
        ) VALUES (sha256(concat_ws('|','conditional_router_v6',?,?)),?,
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


def _cash_profitability_secondary(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        UPDATE factor_definition
        SET factor_name = 'Cash-decile-preserving profitability router',
            description = 'On the governed liquid-universe decision-date cohort, operating profitability or low net share issuance determines the primary decile. Quarterly cash operating profits-to-lagged-assets orders names only within that decile.',
            expression = 'lexicographic_decile(coalesce(operating_profitability,low_net_share_issuance),quarterly_cash_operating_profitability_lagged_assets)',
            input_ids_json = '["factor:profitability_operating_profitability","factor:financing_low_net_share_issuance","factor:profitability_quarterly_cash_operating_profitability_lagged_assets"]',
            available_at_policy = 'Upstream dates are normalized to max(as_of_date, available_at date) and filtered through point-in-time liquid-universe membership before primary deciles are formed. Visible quarterly cash profitability is then an intra-decile secondary key.',
            source = ?,
            standardization_spec_json = '{"method":"governed_primary_decile_then_cash_secondary_rank_then_zscore_cs","universe_id":"us_common_equity_liquid_v1","decision_date":"max_factor_as_of_and_available_at_date","primary_bucket_count":10,"primary":"profitability_operating_profitability","fallback":"financing_low_net_share_issuance","secondary":"profitability_quarterly_cash_operating_profitability_lagged_assets","secondary_scope":"within_primary_decile","missing_secondary":"retain_primary_within_decile_rank","top_bottom_membership_preserved":true,"return_fitted_weights":false}'
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
        version=227,
        name="quarterly_cash_profitability_router",
        up=_cash_profitability_secondary,
    )
]
