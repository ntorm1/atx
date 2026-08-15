"""Governed standard-taxonomy package catalog and filing dependency edges."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _xbrl_taxonomy_package_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_standard_taxonomy_package_revisions (
            package_revision_id VARCHAR PRIMARY KEY,
            package_key VARCHAR NOT NULL,
            authority VARCHAR NOT NULL,
            taxonomy_family VARCHAR NOT NULL,
            taxonomy_version VARCHAR NOT NULL,
            source_url VARCHAR NOT NULL,
            sha256 VARCHAR NOT NULL,
            byte_count BIGINT NOT NULL,
            cache_path VARCHAR NOT NULL,
            materialized_path VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            fetched_at TIMESTAMP NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE TABLE IF NOT EXISTS xbrl_filing_taxonomy_packages (
            filing_package_edge_id VARCHAR PRIMARY KEY,
            security_id VARCHAR NOT NULL,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            extension_schema_url VARCHAR NOT NULL,
            import_namespace VARCHAR,
            import_url VARCHAR NOT NULL,
            package_revision_id VARCHAR NOT NULL,
            package_key VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE INDEX IF NOT EXISTS idx_xbrl_taxonomy_package_key
            ON xbrl_standard_taxonomy_package_revisions(
                package_key,is_latest_revision,available_at
            );
        CREATE INDEX IF NOT EXISTS idx_xbrl_taxonomy_package_source
            ON xbrl_standard_taxonomy_package_revisions(source_url,sha256);
        CREATE INDEX IF NOT EXISTS idx_xbrl_filing_taxonomy_accession
            ON xbrl_filing_taxonomy_packages(
                security_id,accession_number,is_latest_revision,package_key
            );
        CREATE INDEX IF NOT EXISTS idx_xbrl_filing_taxonomy_package
            ON xbrl_filing_taxonomy_packages(package_revision_id);
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,
            pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "xbrl_standard_taxonomy_package_revisions",
                "raw",
                "standard_taxonomy_package_revision",
                "package_revision_id",
                "Content-addressed official FASB/SEC taxonomy ZIP revisions used for offline DTS resolution.",
                '["package_revision_id"]',
                "available_at is first verified package capture time; use package_key plus is_latest_revision for the active local revision.",
            ),
            (
                "xbrl_filing_taxonomy_packages",
                "lineage",
                "filing_taxonomy_dependency",
                "filing_package_edge_id",
                "Revision-aware edge from a filing extension-schema import to the exact local taxonomy package revision.",
                '["filing_package_edge_id"]',
                "available_at is when the verified filing-to-package mapping became known; historical edge revisions remain queryable.",
            ),
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name,dataset_id,table_name,severity,threshold_value,
            comparator,enabled,failure_status,source,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,now())
        """,
        [
            (
                "bad_xbrl_standard_taxonomy_package_revisions",
                "xbrl_standard_taxonomy_packages",
                "xbrl_standard_taxonomy_package_revisions",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "duplicate_latest_xbrl_standard_taxonomy_packages",
                "xbrl_standard_taxonomy_packages",
                "xbrl_standard_taxonomy_package_revisions",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "bad_xbrl_filing_taxonomy_edges",
                "xbrl_filing_taxonomy_packages",
                "xbrl_filing_taxonomy_packages",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "orphan_xbrl_filing_taxonomy_edges",
                "xbrl_filing_taxonomy_packages",
                "xbrl_filing_taxonomy_packages",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
        ],
    )
    _catalog_fields_for_tables(
        conn,
        (
            "xbrl_standard_taxonomy_package_revisions",
            "xbrl_filing_taxonomy_packages",
        ),
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=291,
        name="xbrl_taxonomy_package_catalog",
        up=_xbrl_taxonomy_package_catalog,
    )
]
