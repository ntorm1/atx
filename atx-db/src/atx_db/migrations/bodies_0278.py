"""Serving publication parity and upstream-freshness manifests."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _fundamental_reconciliation_publish_watermarks(
    conn: duckdb.DuckDBPyConnection,
) -> None:
    conn.execute(
        """
        ALTER TABLE fundamental_reconciliation_builds
            ADD COLUMN IF NOT EXISTS is_full_refresh BOOLEAN;
        ALTER TABLE fundamental_reconciliation_builds
            ADD COLUMN IF NOT EXISTS published_row_count BIGINT;
        ALTER TABLE fundamental_reconciliation_builds
            ADD COLUMN IF NOT EXISTS published_max_available_at TIMESTAMP;
        ALTER TABLE fundamental_reconciliation_builds
            ADD COLUMN IF NOT EXISTS published_content_hash UBIGINT;
        ALTER TABLE fundamental_reconciliation_builds
            ADD COLUMN IF NOT EXISTS input_max_source_loaded_at TIMESTAMP;
        """
    )
    conn.execute(
        """
        UPDATE table_catalog
        SET description = 'Atomic refresh manifests with scoped source parity, published-table checksum parity, and upstream freshness watermarks.',
            pit_notes = 'Full builds record the upstream input watermark; every completed build records the global row count, available_at watermark, and order-independent content hash.',
            updated_at = now()
        WHERE table_name = 'fundamental_reconciliation_builds'
        """
    )
    _catalog_fields_for_tables(conn, ("fundamental_reconciliation_builds",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=278,
        name="fundamental_reconciliation_publish_watermarks",
        up=_fundamental_reconciliation_publish_watermarks,
    )
]
