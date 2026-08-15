"""Generalize taxonomy edges from schema imports to filing-package references."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _xbrl_filing_taxonomy_reference_edges(
    conn: duckdb.DuckDBPyConnection,
) -> None:
    conn.execute(
        """
        DROP INDEX IF EXISTS idx_xbrl_filing_taxonomy_accession;
        DROP INDEX IF EXISTS idx_xbrl_filing_taxonomy_package;
        """
    )
    columns = {
        str(row[1])
        for row in conn.execute(
            "PRAGMA table_info('xbrl_filing_taxonomy_packages')"
        ).fetchall()
    }
    for old_name, new_name in (
        ("extension_schema_url", "source_document_url"),
        ("import_namespace", "reference_namespace"),
        ("import_url", "reference_url"),
    ):
        if old_name in columns and new_name not in columns:
            conn.execute(
                f"""
                ALTER TABLE xbrl_filing_taxonomy_packages
                    RENAME COLUMN {old_name} TO {new_name}
                """
            )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,
            pit_notes,updated_at
        ) VALUES (
            'xbrl_filing_taxonomy_packages','lineage',
            'filing_taxonomy_dependency','filing_package_edge_id',
            'Revision-aware edge from a filing-package taxonomy reference to the exact official package revision.',
            '["filing_package_edge_id"]',
            'available_at is when the verified filing-to-package mapping became known; historical edge revisions remain queryable.',
            now()
        )
        """
    )
    conn.execute(
        """
        UPDATE table_catalog
        SET description=(
                'Content-addressed official FASB, SEC, and XBRL US taxonomy '
                'archive revisions used for offline DTS resolution.'
            ),
            updated_at=now()
        WHERE table_name='xbrl_standard_taxonomy_package_revisions'
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name,dataset_id,table_name,severity,threshold_value,
            comparator,enabled,failure_status,source,updated_at
        ) VALUES (
            'duplicate_latest_xbrl_filing_taxonomy_edges',
            'xbrl_filing_taxonomy_packages',
            'xbrl_filing_taxonomy_packages',
            'critical',0.0,'eq',true,'failed',
            'atx_fundamentals_provider',now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("xbrl_filing_taxonomy_packages",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=293,
        name="xbrl_filing_taxonomy_reference_edges",
        up=_xbrl_filing_taxonomy_reference_edges,
    )
]
