"""Durable, failure-isolated taxonomy package capture attempts."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _xbrl_taxonomy_package_capture_attempts(
    conn: duckdb.DuckDBPyConnection,
) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_taxonomy_package_capture_attempts (
            attempt_id VARCHAR PRIMARY KEY,
            run_id VARCHAR NOT NULL,
            package_key VARCHAR NOT NULL,
            authority VARCHAR NOT NULL,
            taxonomy_family VARCHAR NOT NULL,
            taxonomy_version VARCHAR NOT NULL,
            source_url VARCHAR NOT NULL,
            source_kind VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            failure_stage VARCHAR,
            cache_hit BOOLEAN,
            network_request_count BIGINT,
            sha256 VARCHAR,
            byte_count BIGINT,
            package_revision_id VARCHAR,
            package_format VARCHAR,
            processor_package_path VARCHAR,
            processor_package_sha256 VARCHAR,
            processor_package_byte_count BIGINT,
            error_type VARCHAR,
            error_message VARCHAR,
            started_at TIMESTAMP NOT NULL,
            completed_at TIMESTAMP NOT NULL,
            available_at TIMESTAMP NOT NULL,
            as_of_date DATE NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE INDEX IF NOT EXISTS idx_xbrl_taxonomy_capture_attempt_package
            ON xbrl_taxonomy_package_capture_attempts(
                package_key,is_latest_revision,completed_at
            );
        CREATE INDEX IF NOT EXISTS idx_xbrl_taxonomy_capture_attempt_run
            ON xbrl_taxonomy_package_capture_attempts(run_id,status);
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,
            pit_notes,updated_at
        ) VALUES (
            'xbrl_taxonomy_package_capture_attempts','audit',
            'taxonomy_package_capture_attempt','attempt_id',
            'Append-only per-package capture outcome with exact publisher request, cache, processor-package, and failure lineage.',
            '["attempt_id"]',
            'available_at is when the package outcome became known; use package_key plus is_latest_revision for the current capture state.',
            now()
        )
        """
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
                "bad_xbrl_taxonomy_package_capture_attempts",
                "xbrl_taxonomy_package_capture_attempts",
                "xbrl_taxonomy_package_capture_attempts",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "duplicate_latest_xbrl_taxonomy_package_capture_attempts",
                "xbrl_taxonomy_package_capture_attempts",
                "xbrl_taxonomy_package_capture_attempts",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "failed_latest_xbrl_taxonomy_package_capture_attempts",
                "xbrl_taxonomy_package_capture_attempts",
                "xbrl_taxonomy_package_capture_attempts",
                "warning",
                0.0,
                "eq",
                True,
                "warning",
                "atx_fundamentals_provider",
            ),
        ],
    )
    _catalog_fields_for_tables(
        conn,
        ("xbrl_taxonomy_package_capture_attempts",),
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=295,
        name="xbrl_taxonomy_package_capture_attempts",
        up=_xbrl_taxonomy_package_capture_attempts,
    )
]
