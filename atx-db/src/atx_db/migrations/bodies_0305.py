"""Public delisting evidence streams with explicit precedence."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _delisting_evidence(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS delisting_evidence (
            evidence_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            evidence_kind VARCHAR NOT NULL,
            evidence_rank INTEGER NOT NULL,
            delist_date DATE NOT NULL,
            reason_category VARCHAR NOT NULL,
            reason_confidence VARCHAR NOT NULL,
            delist_code VARCHAR NOT NULL,
            evidence_source_table VARCHAR NOT NULL,
            source_event_id VARCHAR,
            as_of_date DATE NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            available_at TIMESTAMP NOT NULL,
            details_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        );

        CREATE INDEX IF NOT EXISTS idx_delisting_evidence_security
            ON delisting_evidence(security_id, delist_date, evidence_rank);
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
                "delisting_evidence",
                "reference",
                "delisting_evidence",
                "security_id,delist_date,evidence_kind",
                "One row per independent piece of public evidence that a security stopped "
                "trading: SEC Form 25/25-NSE, a Nasdaq Trader delete action, SEC Form 15, or "
                "a last-trade gap in the ticker-history archive. evidence_rank encodes "
                "precedence (1 = strongest); delisting_events keeps the minimum rank per "
                "(security_id, delist_date).",
                '["security_id","delist_date","evidence_kind"]',
                "available_at is the filing acceptance time for SEC evidence, the Nasdaq "
                "file creation time for delete actions, and last trade_date + 22 hours for "
                "archive inference. It is never a wall-clock read. is_latest_revision is "
                "expected to always be true: refresh_delisting_evidence fully replaces a "
                "source's evidence rows on every run rather than maintaining a revision "
                "chain, so this column carries no information today but keeps the table's "
                "shape consistent with every other fact table under the current PIT regime.",
            )
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id,source_system_id,name,description,grain,primary_table,
            pit_column,available_at_column,updated_at
        ) VALUES (?,?,?,?,?,?,'as_of_date','available_at',now())
        """,
        [
            (
                "delisting_evidence",
                "atx_derived",
                "Public delisting evidence",
                "Attributed public evidence for delisting date and reason.",
                "security_id,delist_date,evidence_kind",
                "delisting_evidence",
            )
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO lake_partition_specs (
            object_name,partition_columns_json,watermark_column,updated_at
        ) VALUES (?,?,'available_at',now())
        """,
        [("delisting_evidence", '["as_of_date"]')],
    )
    _catalog_fields_for_tables(conn, ("delisting_evidence",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=305, name="delisting_evidence", up=_delisting_evidence)]
