"""Persist immutable-source cache usage on filing-context attempts."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _filing_context_attempt_source_cache_lineage(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        ALTER TABLE filing_context_backfill_attempts
            ADD COLUMN IF NOT EXISTS source_artifact_count INTEGER;
        ALTER TABLE filing_context_backfill_attempts
            ADD COLUMN IF NOT EXISTS source_cache_hit_count INTEGER;

        UPDATE filing_context_backfill_attempts
        SET source_artifact_count=actual_request_count,
            source_cache_hit_count=0
        WHERE status='succeeded'
          AND source_artifact_count IS NULL;
        """
    )
    _catalog_fields_for_tables(conn, ("filing_context_backfill_attempts",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=286,
        name="filing_context_attempt_source_cache_lineage",
        up=_filing_context_attempt_source_cache_lineage,
    )
]
