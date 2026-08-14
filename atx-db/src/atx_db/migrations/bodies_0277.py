"""Materialized reconciliation serving surface and refresh manifests."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin
from .bodies_0267 import _seed_public_contract


def _fundamental_reconciliation_serving_release(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_reconciliation_serving AS
        SELECT *
        FROM v_fundamental_reconciliation_contextual
        WHERE false;

        CREATE TABLE IF NOT EXISTS fundamental_reconciliation_builds (
            build_id VARCHAR PRIMARY KEY,
            status VARCHAR NOT NULL,
            scope_json VARCHAR NOT NULL,
            source_row_count BIGINT,
            serving_row_count BIGINT,
            source_max_available_at TIMESTAMP,
            serving_max_available_at TIMESTAMP,
            started_at TIMESTAMP NOT NULL,
            completed_at TIMESTAMP,
            error_message VARCHAR,
            run_id VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_fundamental_reconciliation_serving_id ON fundamental_reconciliation_serving(reconciliation_id)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_reconciliation_serving_security_period ON fundamental_reconciliation_serving(security_id,period_end,basis)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_reconciliation_serving_rule_period ON fundamental_reconciliation_serving(rule_id,period_end,basis)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_reconciliation_serving_status ON fundamental_reconciliation_serving(status,is_hard_failure,is_latest_revision)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_reconciliation_serving_pit ON fundamental_reconciliation_serving(available_at,is_latest_revision)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_reconciliation_builds_status ON fundamental_reconciliation_builds(status,started_at)",
    ):
        conn.execute(statement)
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "fundamental_reconciliation_serving",
                "api",
                "fundamental_reconciliation",
                "security_id,rule_id,basis,period_end,available_at",
                "Indexed materialization of contextual accounting reconciliation for low-latency delivery.",
                '["reconciliation_id"]',
                "Rows retain source available_at and revision chains; build manifests prove serving freshness.",
            ),
            (
                "fundamental_reconciliation_builds",
                "control",
                "fundamental_reconciliation_build",
                "build_id",
                "Atomic refresh manifests with source/serving row and watermark parity.",
                '["build_id"]',
                "started_at/completed_at bound each materialization attempt; only completed builds are publishable.",
            ),
        ],
    )
    _catalog_fields_for_tables(
        conn,
        (
            "fundamental_reconciliation_serving",
            "fundamental_reconciliation_builds",
        ),
    )
    _seed_public_contract(conn)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=277,
        name="materialized_fundamental_reconciliation_serving",
        up=_fundamental_reconciliation_serving_release,
    )
]
