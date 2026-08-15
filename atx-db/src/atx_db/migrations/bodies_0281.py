"""Reconciliation-driven filing-context backfill queue and build manifests."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _filing_context_backfill_queue_release(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS filing_context_backfill_queue (
            queue_id VARCHAR PRIMARY KEY,
            build_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            form VARCHAR,
            filing_date DATE,
            report_date DATE,
            acceptance_datetime TIMESTAMP,
            primary_document VARCHAR,
            is_xbrl BOOLEAN,
            is_inline_xbrl BOOLEAN,
            has_existing_filing_context BOOLEAN NOT NULL,
            expected_instance_format VARCHAR,
            filing_directory_url VARCHAR NOT NULL,
            filing_index_url VARCHAR NOT NULL,
            primary_document_url VARCHAR,
            filing_size_bytes BIGINT,
            estimated_request_count INTEGER NOT NULL,
            affected_reconciliation_count BIGINT NOT NULL,
            affected_error_rule_count BIGINT NOT NULL,
            affected_rule_count BIGINT NOT NULL,
            affected_period_count BIGINT NOT NULL,
            mismatch_count BIGINT NOT NULL,
            diagnostic_difference_count BIGINT NOT NULL,
            unverified_reconciled_count BIGINT NOT NULL,
            max_absolute_difference DOUBLE,
            max_abs_residual_percent DOUBLE,
            priority_tier VARCHAR NOT NULL,
            priority_score BIGINT NOT NULL,
            priority_rank BIGINT NOT NULL,
            queue_status VARCHAR NOT NULL,
            blocked_reason VARCHAR,
            source_max_available_at TIMESTAMP NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE TABLE IF NOT EXISTS filing_context_backfill_builds (
            build_id VARCHAR PRIMARY KEY,
            status VARCHAR NOT NULL,
            input_reconciliation_build_id VARCHAR,
            input_reconciliation_content_hash UBIGINT,
            source_gap_row_count BIGINT,
            queue_row_count BIGINT,
            ready_row_count BIGINT,
            blocked_row_count BIGINT,
            published_max_available_at TIMESTAMP,
            published_content_hash UBIGINT,
            started_at TIMESTAMP NOT NULL,
            completed_at TIMESTAMP,
            error_message VARCHAR,
            run_id VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE UNIQUE INDEX IF NOT EXISTS idx_filing_context_backfill_accession
            ON filing_context_backfill_queue(security_id,accession_number);
        CREATE INDEX IF NOT EXISTS idx_filing_context_backfill_priority
            ON filing_context_backfill_queue(queue_status,priority_rank);
        CREATE INDEX IF NOT EXISTS idx_filing_context_backfill_symbol
            ON filing_context_backfill_queue(symbol,filing_date);
        CREATE INDEX IF NOT EXISTS idx_filing_context_backfill_build_status
            ON filing_context_backfill_builds(status,started_at);
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "filing_context_backfill_queue",
                "control",
                "filing_context_backfill_candidate",
                "security_id,accession_number",
                "Prioritized SEC filing instances whose absence prevents same-context accounting verification.",
                '["queue_id"]',
                "available_at is the latest affected reconciliation evidence time; each refresh atomically republishes the current queue.",
            ),
            (
                "filing_context_backfill_builds",
                "control",
                "filing_context_backfill_build",
                "build_id",
                "Atomic queue refresh manifests linked to an exact reconciliation publication checksum.",
                '["build_id"]',
                "Build timestamps and the input reconciliation build identify when a current operational plan was derived.",
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
                "bad_filing_context_backfill_queue_rows",
                "filing_context_backfill_queue",
                "filing_context_backfill_queue",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "filing_context_backfill_queue_manifest_parity",
                "filing_context_backfill_queue",
                "filing_context_backfill_queue",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "filing_context_backfill_queue_freshness",
                "filing_context_backfill_queue",
                "filing_context_backfill_queue",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_fundamentals_provider",
            ),
            (
                "blocked_filing_context_backfill_queue_rows",
                "filing_context_backfill_queue",
                "filing_context_backfill_queue",
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
        (
            "filing_context_backfill_queue",
            "filing_context_backfill_builds",
        ),
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=281,
        name="filing_context_backfill_queue",
        up=_filing_context_backfill_queue_release,
    )
]
