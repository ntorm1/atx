"""Versioned migration framework for the atx-impl DuckDB warehouse.

Migrations are ordered, idempotent, and tracked in the schema_migrations table.
Call apply_pending_migrations(conn) after ensure_quant_schema to bring the schema
up to date. It is safe to call multiple times; only unapplied migrations run.
"""

from __future__ import annotations

import duckdb
from dataclasses import dataclass
from typing import Callable


@dataclass(frozen=True)
class Migration:
    version: int
    name: str
    up: Callable[[duckdb.DuckDBPyConnection], None]


def _noop(conn: duckdb.DuckDBPyConnection) -> None:
    """No-op body — baseline schema is already created by ensure_quant_schema."""


def _schema_evolution_alters(conn: duckdb.DuckDBPyConnection) -> None:
    """Apply the ALTER TABLE evolution statements that were previously in _ensure_schema_evolution.

    All statements use ADD COLUMN IF NOT EXISTS so they are idempotent.
    """
    for statement in (
        "ALTER TABLE raw_source_files ADD COLUMN IF NOT EXISTS metadata_json VARCHAR",
        "ALTER TABLE security_identifier_history ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE security_identifier_history ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE exchange_listings ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE exchange_listings ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE equity_daily_bars ADD COLUMN IF NOT EXISTS vendor_security_id VARCHAR",
        "ALTER TABLE equity_daily_bars ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE equity_daily_bars ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE corporate_actions ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE corporate_actions ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE sec_company_facts ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE sec_company_facts ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE fundamental_points ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE fundamental_points ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE macro_observations ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE universe_memberships ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE universe_memberships ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE feature_values ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE feature_values ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE etl_job_definitions ADD COLUMN IF NOT EXISTS max_retries INTEGER DEFAULT 0",
        "ALTER TABLE etl_job_definitions ADD COLUMN IF NOT EXISTS retry_delay_seconds DOUBLE DEFAULT 0",
        "ALTER TABLE etl_job_runs ADD COLUMN IF NOT EXISTS attempt_count INTEGER DEFAULT 0",
        "ALTER TABLE etl_job_runs ADD COLUMN IF NOT EXISTS max_retries INTEGER DEFAULT 0",
        "ALTER TABLE etl_job_runs ADD COLUMN IF NOT EXISTS retry_delay_seconds DOUBLE DEFAULT 0",
    ):
        conn.execute(statement)


def _reference_classifications(conn: duckdb.DuckDBPyConnection) -> None:
    """Create the S1 reference-classification tables if they don't already exist.

    These tables are also created by ensure_quant_schema (CREATE TABLE IF NOT EXISTS),
    so this migration is a no-op for databases that run ensure_quant_schema first.
    It exists so the migration log has a versioned record of when the tables were introduced.
    """
    for statement in (
        """
        CREATE TABLE IF NOT EXISTS taxonomy (
            taxonomy_id VARCHAR PRIMARY KEY,
            code VARCHAR NOT NULL UNIQUE,
            name VARCHAR NOT NULL,
            provider VARCHAR,
            version VARCHAR,
            is_hierarchical BOOLEAN NOT NULL DEFAULT true,
            description VARCHAR,
            source VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS taxonomy_node (
            node_id VARCHAR PRIMARY KEY,
            taxonomy_id VARCHAR NOT NULL,
            node_code VARCHAR NOT NULL,
            node_label VARCHAR NOT NULL,
            parent_node_id VARCHAR,
            level INTEGER NOT NULL,
            sort_order INTEGER
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS entity_classification (
            classification_id VARCHAR PRIMARY KEY,
            security_id VARCHAR NOT NULL,
            taxonomy_id VARCHAR NOT NULL,
            node_id VARCHAR NOT NULL,
            node_code VARCHAR NOT NULL,
            is_primary BOOLEAN NOT NULL DEFAULT false,
            valid_from DATE NOT NULL,
            valid_to DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR,
            source VARCHAR NOT NULL
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS taxonomy_mapping (
            mapping_id VARCHAR PRIMARY KEY,
            from_taxonomy_id VARCHAR NOT NULL,
            from_node_code VARCHAR NOT NULL,
            to_taxonomy_id VARCHAR NOT NULL,
            to_node_code VARCHAR NOT NULL,
            relationship VARCHAR NOT NULL,
            confidence DOUBLE,
            source VARCHAR
        )
        """,
        "CREATE INDEX IF NOT EXISTS idx_entity_classification_security ON entity_classification(security_id)",
        "CREATE INDEX IF NOT EXISTS idx_entity_classification_taxonomy_code ON entity_classification(taxonomy_id, node_code)",
        "CREATE INDEX IF NOT EXISTS idx_taxonomy_node_taxonomy_parent ON taxonomy_node(taxonomy_id, parent_node_id)",
    ):
        conn.execute(statement)


# Ordered registry of all migrations. Add new entries at the END only.
MIGRATIONS: list[Migration] = [
    Migration(
        version=1,
        name="baseline_schema",
        up=_noop,
    ),
    Migration(
        version=2,
        name="schema_evolution_alters",
        up=_schema_evolution_alters,
    ),
    Migration(
        version=3,
        name="reference_classifications",
        up=_reference_classifications,
    ),
]


def apply_pending_migrations(conn: duckdb.DuckDBPyConnection) -> list[int]:
    """Apply any MIGRATIONS whose version is not yet recorded in schema_migrations.

    Runs each migration inside a transaction. Inserts a tracking row on success.
    Returns the list of version numbers that were applied (empty list if all up to date).
    Must be called after ensure_quant_schema so that schema_migrations exists.

    The schema_migrations table was created by ensure_quant_schema with columns:
        version VARCHAR PRIMARY KEY, description VARCHAR NOT NULL,
        checksum VARCHAR, applied_at TIMESTAMP NOT NULL DEFAULT now()
    We use (version, description) and cast version int to VARCHAR for storage.
    """
    # Fetch already-applied versions as integers
    rows = conn.execute(
        "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
    ).fetchall()
    applied: set[int] = {row[0] for row in rows}

    applied_now: list[int] = []
    for migration in sorted(MIGRATIONS, key=lambda m: m.version):
        if migration.version in applied:
            continue
        # Run inside a transaction so a failure rolls back cleanly
        conn.execute("BEGIN TRANSACTION")
        try:
            migration.up(conn)
            conn.execute(
                """
                INSERT INTO schema_migrations (version, description, applied_at)
                VALUES (?, ?, CURRENT_TIMESTAMP)
                """,
                [str(migration.version).zfill(4), migration.name],
            )
            conn.execute("COMMIT")
        except Exception:
            conn.execute("ROLLBACK")
            raise
        applied_now.append(migration.version)

    return applied_now
