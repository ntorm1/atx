"""Close PIT-column gaps on the 13F amendment research surfaces."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

TABLES = (
    "thirteenf_amendment_backtest_trades",
    "thirteenf_amendment_corrections",
    "thirteenf_amendment_market_regimes",
    "thirteenf_amendment_rates",
    "thirteenf_consensus_amendment_signals",
    "thirteenf_consensus_signal_outcomes",
    "thirteenf_effective_positions",
    "thirteenf_signal_instrument_candidates",
)


def _thirteenf_pit_contract_close(conn: duckdb.DuckDBPyConnection) -> None:
    for table in TABLES:
        conn.execute(f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS as_of_date DATE")
    for table in (
        "thirteenf_amendment_backtest_trades",
        "thirteenf_consensus_amendment_signals",
        "thirteenf_consensus_signal_outcomes",
    ):
        conn.execute(f"ALTER TABLE {table} ADD COLUMN IF NOT EXISTS available_at TIMESTAMP")

    conn.execute(
        """
        UPDATE thirteenf_amendment_backtest_trades
        SET as_of_date = coalesce(as_of_date, entry_date),
            available_at = coalesce(
                available_at,
                greatest(signal_available_at, entry_available_at,
                         coalesce(exit_available_at, entry_available_at))
            )
        """
    )
    for table in (
        "thirteenf_amendment_corrections",
        "thirteenf_amendment_market_regimes",
        "thirteenf_amendment_rates",
        "thirteenf_effective_positions",
    ):
        conn.execute(f"UPDATE {table} SET as_of_date = coalesce(as_of_date, report_period)")
    conn.execute(
        """
        UPDATE thirteenf_consensus_amendment_signals
        SET as_of_date = coalesce(as_of_date, report_period),
            available_at = coalesce(available_at, signal_available_at)
        """
    )
    conn.execute(
        """
        UPDATE thirteenf_consensus_signal_outcomes
        SET as_of_date = coalesce(as_of_date, report_period),
            available_at = coalesce(available_at, greatest(signal_available_at, source_loaded_at))
        """
    )
    conn.execute(
        """
        UPDATE thirteenf_signal_instrument_candidates
        SET as_of_date = coalesce(as_of_date, CAST(available_at AS DATE))
        """
    )
    _catalog_fields_for_tables(conn, TABLES)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=269, name="thirteenf_pit_contract_close", up=_thirteenf_pit_contract_close)]
