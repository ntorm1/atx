"""XBRL package lineage and semantic processor outcomes."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _xbrl_processor_package_lineage(conn: duckdb.DuckDBPyConnection) -> None:
    statements = (
        "ALTER TABLE xbrl_processor_runs ADD COLUMN IF NOT EXISTS taxonomy_packages_json VARCHAR DEFAULT '[]'",
        "ALTER TABLE xbrl_processor_runs ADD COLUMN IF NOT EXISTS filing_archive_manifest_sha256 VARCHAR",
        "ALTER TABLE xbrl_processor_runs ADD COLUMN IF NOT EXISTS filing_archive_member_count INTEGER",
        "ALTER TABLE xbrl_processor_runs ADD COLUMN IF NOT EXISTS dts_resolution_status VARCHAR DEFAULT 'not_evaluated'",
        "ALTER TABLE xbrl_processor_runs ADD COLUMN IF NOT EXISTS validation_outcome VARCHAR DEFAULT 'not_evaluated'",
    )
    for statement in statements:
        conn.execute(statement)
    conn.execute(
        """
        CREATE TEMPORARY TABLE xbrl_processor_0288_backfill AS
        SELECT
            run.processor_run_id,
            coalesce(bool_or(finding.message_code='IOerror'),false) AS has_io_error
        FROM xbrl_processor_runs run
        LEFT JOIN xbrl_processor_findings finding
          ON finding.processor_run_id=run.processor_run_id
        GROUP BY run.processor_run_id
        """
    )
    conn.execute(
        """
        UPDATE xbrl_processor_runs run
        SET taxonomy_packages_json=coalesce(run.taxonomy_packages_json,'[]'),
            filing_archive_manifest_sha256=coalesce(
                run.filing_archive_manifest_sha256,
                json_extract_string(run.entrypoint_json,'$.archive_manifest_sha256')
            ),
            filing_archive_member_count=coalesce(
                run.filing_archive_member_count,
                try_cast(
                    json_extract_string(run.entrypoint_json,'$.archive_member_count')
                    AS INTEGER
                )
            ),
            dts_resolution_status=CASE
                WHEN run.status<>'succeeded' THEN 'not_evaluated'
                WHEN backfill.has_io_error THEN 'incomplete'
                ELSE 'resolved'
            END,
            validation_outcome=CASE
                WHEN run.status='running' THEN 'not_evaluated'
                WHEN run.status='failed' THEN 'processor_failed'
                WHEN run.status='unavailable' THEN 'processor_unavailable'
                WHEN backfill.has_io_error THEN 'incomplete_dts'
                WHEN coalesce(run.error_count,0)>0 THEN 'validation_errors'
                WHEN coalesce(run.warning_count,0)>0
                  OR coalesce(run.inconsistency_count,0)>0 THEN 'validation_issues'
                ELSE 'valid'
            END
        FROM xbrl_processor_0288_backfill backfill
        WHERE backfill.processor_run_id=run.processor_run_id
        """
    )
    conn.execute("DROP TABLE xbrl_processor_0288_backfill")
    _catalog_fields_for_tables(conn, ("xbrl_processor_runs",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=288,
        name="xbrl_processor_package_lineage",
        up=_xbrl_processor_package_lineage,
    )
]
