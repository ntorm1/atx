"""Immutable SEC 8-K earnings-release document receipts."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _sec_earnings_release_receipts(conn: duckdb.DuckDBPyConnection) -> None:
    """Add only the source-owned receipt surface; do not alter raw facts."""

    # Retain raw SEC's acceptance string for newly loaded metadata. Existing
    # normalized rows remain deliberately unqualified until a pinned archive
    # recovery supplies the original offset-bearing value.
    conn.execute("ALTER TABLE sec_submissions ADD COLUMN IF NOT EXISTS acceptance_datetime_raw VARCHAR")
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS sec_earnings_release_receipts (
            receipt_id VARCHAR PRIMARY KEY,
            cik VARCHAR NOT NULL,
            source_security_id VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            document_name VARCHAR,
            filing_date DATE,
            report_date DATE,
            acceptance_datetime TIMESTAMP,
            raw_acceptance_timestamp VARCHAR,
            acceptance_utc_offset VARCHAR,
            timestamp_zone_status VARCHAR NOT NULL,
            available_at TIMESTAMP,
            index_url VARCHAR,
            document_url VARCHAR,
            document_sha256 VARCHAR,
            outcome VARCHAR NOT NULL,
            rejection_reason VARCHAR,
            source_url VARCHAR NOT NULL,
            retrieval_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            CHECK (outcome IN ('accepted', 'rejected', 'fetch_failed')),
            CHECK (outcome = 'accepted' OR rejection_reason IS NOT NULL),
            CHECK (
                (outcome = 'accepted' AND document_name IS NOT NULL AND document_sha256 IS NOT NULL
                 AND available_at IS NOT NULL AND rejection_reason IS NULL)
                OR (outcome IN ('rejected', 'fetch_failed') AND rejection_reason IS NOT NULL)
            )
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
        ) VALUES (
            'sec_earnings_release_receipts', 'bronze', 'sec_earnings_release_receipt',
            'cik,accession_number,document_name,document_sha256',
            'Immutable public-SEC archive fetch, rejection, timestamp and SHA receipts for 8-K Item 2.02 earnings-release exhibits.',
            '["receipt_id"]',
            'Raw acceptance timestamp and explicit offset are retained. A timestamp_zone_unknown value never establishes exact UTC; with filing_date the conservative filing-date-plus-46-hour daily availability floor remains valid.', now()
        )
        """
    )
    conn.execute(
        """
        UPDATE table_catalog
        SET pit_notes = concat_ws(' ', nullif(pit_notes, ''),
            'Raw acceptance_datetime_raw preserves the original SEC offset-bearing source string for new loads; legacy normalized rows need archive recovery.'),
            updated_at = now()
        WHERE table_name = 'sec_submissions'
        """
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_sec_earnings_release_receipts_candidate "
        "ON sec_earnings_release_receipts(cik, accession_number, outcome)"
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_sec_earnings_release_receipts_sha "
        "ON sec_earnings_release_receipts(document_sha256)"
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(version=320, name="sec_earnings_release_receipts", up=_sec_earnings_release_receipts)
]
