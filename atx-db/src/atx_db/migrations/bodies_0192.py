"""Availability-safe 13F amendment backtest table."""

from __future__ import annotations

import duckdb

from ..connection import DuckDBStore
from ..thirteenf_backtest import ensure_thirteenf_backtest_schema
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _thirteenf_backtest_schema(conn: duckdb.DuckDBPyConnection) -> None:
    store = DuckDBStore(":memory:")
    store.connection = conn
    ensure_thirteenf_backtest_schema(store)
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        ) VALUES (
            'thirteenf_amendment_backtest_trades', 'atx_warehouse',
            '13F amendment signal backtest trades',
            'Availability-safe fixed-horizon price outcomes for consensus amendment signals.',
            'signal_id,price_source,horizon_trading_days',
            'thirteenf_amendment_backtest_trades', 'entry_date', 'entry_available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        ) VALUES (
            'thirteenf_amendment_backtest_trades', 'research', '13f_signal_trade',
            'signal_id,price_source,horizon_trading_days',
            'Fixed-horizon long and short returns entered on the first fully available bar after signal publication.',
            '["trade_id"]',
            'Entry is strictly after signal_available_at; incomplete forward windows are retained and flagged.', now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("thirteenf_amendment_backtest_trades",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(version=192, name="thirteenf_amendment_backtest_schema", up=_thirteenf_backtest_schema)
]
