"""Mid-cap audit field for 13F amendment backtests."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _thirteenf_backtest_market_cap(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        ALTER TABLE thirteenf_amendment_backtest_trades
        ADD COLUMN IF NOT EXISTS entry_market_cap_usd DOUBLE
        """
    )
    _catalog_fields_for_tables(conn, ("thirteenf_amendment_backtest_trades",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=193,
        name="thirteenf_backtest_market_cap",
        up=_thirteenf_backtest_market_cap,
    )
]
