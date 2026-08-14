"""Canonical point-in-time shares and market capitalization on daily bars."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _canonical_daily_bar_market_cap(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        ALTER TABLE equity_daily_bars
        ADD COLUMN IF NOT EXISTS shares_outstanding BIGINT
        """
    )
    conn.execute(
        """
        ALTER TABLE equity_daily_bars
        ADD COLUMN IF NOT EXISTS market_cap_usd DOUBLE
        """
    )
    _catalog_fields_for_tables(conn, ("equity_daily_bars",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=263,
        name="canonical_daily_bar_market_cap",
        up=_canonical_daily_bar_market_cap,
    )
]
