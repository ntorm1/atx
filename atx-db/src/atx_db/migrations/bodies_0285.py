"""Durable filing-context backfill execution attempts."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _filing_context_backfill_attempts(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS filing_context_backfill_attempts (
            attempt_id VARCHAR PRIMARY KEY,
            queue_id VARCHAR NOT NULL,
            queue_build_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            priority_tier VARCHAR NOT NULL,
            priority_rank BIGINT NOT NULL,
            attempt_number INTEGER NOT NULL,
            max_attempts INTEGER NOT NULL,
            status VARCHAR NOT NULL,
            is_retryable BOOLEAN NOT NULL,
            estimated_request_count INTEGER NOT NULL,
            actual_request_count INTEGER,
            contexts_loaded BIGINT,
            dimensions_loaded BIGINT,
            facts_loaded BIGINT,
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

        CREATE UNIQUE INDEX IF NOT EXISTS idx_filing_context_attempt_sequence
            ON filing_context_backfill_attempts(queue_id,attempt_number);
        CREATE INDEX IF NOT EXISTS idx_filing_context_attempt_status
            ON filing_context_backfill_attempts(status,started_at);
        CREATE INDEX IF NOT EXISTS idx_filing_context_attempt_accession
            ON filing_context_backfill_attempts(security_id,accession_number,attempt_number);
        CREATE INDEX IF NOT EXISTS idx_filing_context_attempt_run
            ON filing_context_backfill_attempts(run_id,started_at);
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,
            pit_notes,updated_at
        ) VALUES (
            'filing_context_backfill_attempts','control',
            'filing_context_backfill_attempt','attempt_id',
            'Append-only execution ledger for bounded SEC filing-instance backfill attempts.',
            '["attempt_id"]',
            'available_at is the attempt claim time; is_latest_revision identifies the latest attempt for a stable queue accession.',
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
                "bad_filing_context_backfill_attempt_rows",
                "filing_context_backfill_attempts",
                "filing_context_backfill_attempts",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "duplicate_filing_context_backfill_attempt_sequences",
                "filing_context_backfill_attempts",
                "filing_context_backfill_attempts",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "stale_running_filing_context_backfill_attempts",
                "filing_context_backfill_attempts",
                "filing_context_backfill_attempts",
                "warning",
                0.0,
                "eq",
                True,
                "warning",
                "atx_fundamentals_provider",
            ),
            (
                "exhausted_filing_context_backfill_attempts",
                "filing_context_backfill_attempts",
                "filing_context_backfill_attempts",
                "warning",
                0.0,
                "eq",
                True,
                "warning",
                "atx_fundamentals_provider",
            ),
        ],
    )
    _catalog_fields_for_tables(conn, ("filing_context_backfill_attempts",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=285,
        name="filing_context_backfill_attempts",
        up=_filing_context_backfill_attempts,
    )
]
