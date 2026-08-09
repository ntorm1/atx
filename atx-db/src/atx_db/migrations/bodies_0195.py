"""Maximum-age policy for production PIT fundamental signals."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _pit_fundamental_signal_freshness_policy(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        UPDATE factor_definition
        SET description =
                'Annual gross profit divided by same-filing total assets, published by the monthly rebalance close and no more than 550 days old.',
            expression =
                'method:pit_annual_divide|numerator:gross_profit|denominator:total_assets|annual_days:330-380|max_age_days:550',
            updated_at = now()
        WHERE factor_id = 'profitability_gross_profitability'
        """
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=195,
        name="pit_fundamental_signal_freshness_policy",
        up=_pit_fundamental_signal_freshness_policy,
    )
]

