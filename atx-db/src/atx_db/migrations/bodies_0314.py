"""Remove unbounded secondary ART indexes before physical bulk publication."""

from __future__ import annotations

import duckdb

from ._runner import Migration


def _bounded_physical_publication(conn: duckdb.DuckDBPyConnection) -> None:
    """Keep logical keys while permitting an atomic shadow-table rename.

    DuckDB does not allow renaming a table with dependent ART indexes.  These
    five indexes are non-unique performance indexes; the bar table has no
    logical key constraint, and `equity_price_metrics.metric_id` retains its
    required primary key.
    """
    for index_name in (
        "idx_equity_daily_bars_security_date",
        "idx_equity_daily_bars_symbol_date",
        "idx_equity_price_metrics_key",
        "idx_equity_price_metrics_date",
        "idx_equity_price_metrics_asof",
    ):
        conn.execute(f"DROP INDEX IF EXISTS {index_name}")


MIGRATIONS = [
    Migration(version=314, name="bounded_physical_publication", up=_bounded_physical_publication)
]
