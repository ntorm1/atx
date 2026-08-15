"""Processor-ready lineage for normalized official taxonomy archives."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _xbrl_processor_taxonomy_package_lineage(
    conn: duckdb.DuckDBPyConnection,
) -> None:
    conn.execute(
        """
        ALTER TABLE xbrl_standard_taxonomy_package_revisions
            ADD COLUMN IF NOT EXISTS package_format VARCHAR;
        ALTER TABLE xbrl_standard_taxonomy_package_revisions
            ADD COLUMN IF NOT EXISTS processor_package_path VARCHAR;
        ALTER TABLE xbrl_standard_taxonomy_package_revisions
            ADD COLUMN IF NOT EXISTS processor_package_sha256 VARCHAR;
        ALTER TABLE xbrl_standard_taxonomy_package_revisions
            ADD COLUMN IF NOT EXISTS processor_package_byte_count BIGINT;

        UPDATE xbrl_standard_taxonomy_package_revisions
        SET package_format=coalesce(package_format,'unclassified')
        WHERE package_format IS NULL;
        """
    )
    _catalog_fields_for_tables(
        conn,
        ("xbrl_standard_taxonomy_package_revisions",),
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=292,
        name="xbrl_processor_taxonomy_package_lineage",
        up=_xbrl_processor_taxonomy_package_lineage,
    )
]
