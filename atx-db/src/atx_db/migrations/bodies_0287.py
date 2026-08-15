"""Structured Arelle processor runs and validation findings."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _xbrl_processor_runs_and_findings(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_processor_runs (
            processor_run_id VARCHAR PRIMARY KEY,
            processor VARCHAR NOT NULL,
            processor_version VARCHAR NOT NULL,
            validation_profile VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            instance_format VARCHAR NOT NULL,
            entrypoint_json VARCHAR NOT NULL,
            command_json VARCHAR NOT NULL,
            internet_connectivity VARCHAR NOT NULL,
            processor_cache_dir VARCHAR,
            status VARCHAR NOT NULL,
            exit_code INTEGER,
            finding_count BIGINT,
            error_count BIGINT,
            warning_count BIGINT,
            inconsistency_count BIGINT,
            started_at TIMESTAMP NOT NULL,
            completed_at TIMESTAMP,
            error_type VARCHAR,
            error_message VARCHAR,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE TABLE IF NOT EXISTS xbrl_processor_findings (
            finding_id VARCHAR PRIMARY KEY,
            processor_run_id VARCHAR NOT NULL,
            processor VARCHAR NOT NULL,
            processor_version VARCHAR NOT NULL,
            validation_profile VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            severity VARCHAR NOT NULL,
            message_code VARCHAR NOT NULL,
            message VARCHAR NOT NULL,
            message_attributes_json VARCHAR NOT NULL,
            references_json VARCHAR NOT NULL,
            ordinal BIGINT NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE INDEX IF NOT EXISTS idx_xbrl_processor_run_accession
            ON xbrl_processor_runs(
                security_id,accession_number,validation_profile,is_latest_revision
            );
        CREATE INDEX IF NOT EXISTS idx_xbrl_processor_run_status
            ON xbrl_processor_runs(status,started_at);
        CREATE INDEX IF NOT EXISTS idx_xbrl_processor_finding_accession
            ON xbrl_processor_findings(
                security_id,accession_number,severity,message_code
            );
        CREATE INDEX IF NOT EXISTS idx_xbrl_processor_finding_run
            ON xbrl_processor_findings(processor_run_id,ordinal);
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
                "xbrl_processor_runs",
                "control",
                "xbrl_processor_validation_run",
                "processor_run_id",
                "Versioned external XBRL processor execution manifest per filing accession.",
                '["processor_run_id"]',
                "available_at is processor completion/claim knowledge time; latest revision is scoped by accession, processor, and validation profile.",
            ),
            (
                "xbrl_processor_findings",
                "quality",
                "xbrl_processor_finding",
                "finding_id",
                "Structured Arelle validation messages with exact message and reference payloads.",
                '["finding_id"]',
                "available_at is inherited from the immutable processor run that emitted the finding.",
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
                "bad_xbrl_processor_run_rows",
                "xbrl_processor_runs",
                "xbrl_processor_runs",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "duplicate_latest_xbrl_processor_runs",
                "xbrl_processor_runs",
                "xbrl_processor_runs",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "bad_xbrl_processor_finding_rows",
                "xbrl_processor_findings",
                "xbrl_processor_findings",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "orphan_xbrl_processor_findings",
                "xbrl_processor_findings",
                "xbrl_processor_findings",
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
        ("xbrl_processor_runs", "xbrl_processor_findings"),
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=287,
        name="xbrl_processor_runs_and_findings",
        up=_xbrl_processor_runs_and_findings,
    )
]
