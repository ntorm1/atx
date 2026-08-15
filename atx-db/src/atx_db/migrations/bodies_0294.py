"""Indexes for filing-package taxonomy reference edges."""

from __future__ import annotations

import duckdb

from ._runner import Migration


def _xbrl_filing_taxonomy_reference_edge_indexes(
    conn: duckdb.DuckDBPyConnection,
) -> None:
    conn.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_xbrl_filing_taxonomy_accession
            ON xbrl_filing_taxonomy_packages(
                security_id,accession_number,is_latest_revision,package_key
            );
        CREATE INDEX IF NOT EXISTS idx_xbrl_filing_taxonomy_package
            ON xbrl_filing_taxonomy_packages(package_revision_id);
        """
    )


MIGRATIONS = [
    Migration(
        version=294,
        name="xbrl_filing_taxonomy_reference_edge_indexes",
        up=_xbrl_filing_taxonomy_reference_edge_indexes,
    )
]
