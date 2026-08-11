"""Align decile-preserving router construction with its governed panel cohort."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

SOURCE = "atx-db governed decile-preserving conditional router v5"
FACTOR_ID = "composite_operating_profitability_or_net_issuance"


def _governed_router_cohort(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        UPDATE factor_definition
        SET description = 'On the governed liquid-universe decision-date cohort, operating profitability or low net share issuance determines the primary decile. Quarterly operating profits-to-lagged-assets orders names only within that decile.',
            available_at_policy = 'Upstream factor dates are normalized to max(as_of_date, available_at date), filtered through point-in-time liquid-universe membership, and deduplicated before primary deciles are formed. Visible quarterly profitability is then an intra-decile secondary key.',
            source = ?,
            standardization_spec_json = '{"method":"governed_primary_decile_then_secondary_rank_then_zscore_cs","universe_id":"us_common_equity_liquid_v1","decision_date":"max_factor_as_of_and_available_at_date","primary_bucket_count":10,"primary":"profitability_operating_profitability","fallback":"financing_low_net_share_issuance","secondary":"profitability_quarterly_operating_profitability_lagged_assets","secondary_scope":"within_primary_decile","missing_secondary":"retain_primary_within_decile_rank","return_fitted_weights":false}'
        WHERE factor_id = ?
        """,
        [SOURCE, FACTOR_ID],
    )
    conn.execute(
        """
        UPDATE factor_dependency_edges
        SET source = ?
        WHERE factor_id = ?
        """,
        [SOURCE, FACTOR_ID],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=224,
        name="governed_decile_router_cohort",
        up=_governed_router_cohort,
    )
]
