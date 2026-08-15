"""Drop the queue's conflicting unique accession index in its own transaction."""

from __future__ import annotations

import duckdb

from ._runner import Migration


def _filing_context_queue_publication_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute("DROP INDEX IF EXISTS idx_filing_context_backfill_accession")


MIGRATIONS = [
    Migration(
        version=282,
        name="drop_unique_filing_context_queue_index",
        up=_filing_context_queue_publication_indexes,
    )
]
