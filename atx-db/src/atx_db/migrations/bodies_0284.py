"""Upstream source watermark for filing-context queue freshness."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _filing_context_queue_input_watermark(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        ALTER TABLE filing_context_backfill_builds
            ADD COLUMN IF NOT EXISTS input_max_source_loaded_at TIMESTAMP;

        UPDATE table_catalog
        SET description='Atomic queue refresh manifests linked to an exact reconciliation publication checksum and all queue-input source watermarks.',
            pit_notes='The input watermark covers reconciliation serving, SEC submissions metadata, and existing filing contexts; any newer input invalidates the published plan.',
            updated_at=now()
        WHERE table_name='filing_context_backfill_builds';
        """
    )
    _catalog_fields_for_tables(conn, ("filing_context_backfill_builds",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=284,
        name="filing_context_queue_input_watermark",
        up=_filing_context_queue_input_watermark,
    )
]
