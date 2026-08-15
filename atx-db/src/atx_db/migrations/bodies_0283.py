"""Recreate the queue accession index without a uniqueness constraint."""

from __future__ import annotations

import duckdb

from ._runner import Migration


def _filing_context_queue_nonunique_accession_index(
    conn: duckdb.DuckDBPyConnection,
) -> None:
    conn.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_filing_context_backfill_accession
            ON filing_context_backfill_queue(security_id,accession_number);

        INSERT OR REPLACE INTO quality_check_registry (
            check_name,dataset_id,table_name,severity,threshold_value,
            comparator,enabled,failure_status,source,updated_at
        ) VALUES (
            'duplicate_filing_context_backfill_accessions',
            'filing_context_backfill_queue',
            'filing_context_backfill_queue',
            'critical',0.0,'eq',true,'failed','atx_fundamentals_provider',now()
        );
        """
    )


MIGRATIONS = [
    Migration(
        version=283,
        name="filing_context_queue_nonunique_accession_index",
        up=_filing_context_queue_nonunique_accession_index,
    )
]
