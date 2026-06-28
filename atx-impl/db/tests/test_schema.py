"""Test that bootstrapping a fresh temp warehouse creates the expected core tables."""

from __future__ import annotations


EXPECTED_CORE_TABLES = {
    "securities",
    "equity_daily_bars",
    "fundamental_points",
    "etl_job_definitions",
    "schema_migrations",
    "provider_parity_matrix",
}


def test_core_tables_exist(tmp_store):
    """After bootstrap, all expected core tables must exist."""
    rows = tmp_store.con.execute(
        """
        SELECT table_name
        FROM information_schema.tables
        WHERE table_schema = 'main'
          AND table_type = 'BASE TABLE'
        """
    ).fetchall()
    existing = {row[0] for row in rows}
    missing = EXPECTED_CORE_TABLES - existing
    assert not missing, f"Missing expected tables: {sorted(missing)}"


def test_schema_migrations_table_exists(tmp_store):
    """schema_migrations table must exist after bootstrap."""
    count = tmp_store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = 'schema_migrations'"
    ).fetchone()[0]
    assert count == 1


def test_securities_table_columns(tmp_store):
    """securities table should have the primary key column security_id."""
    rows = tmp_store.con.execute(
        """
        SELECT column_name
        FROM information_schema.columns
        WHERE table_schema = 'main'
          AND table_name = 'securities'
        """
    ).fetchall()
    cols = {row[0] for row in rows}
    assert "security_id" in cols
    assert "primary_symbol" in cols
    assert "active" in cols


def test_etl_job_definitions_table_exists(tmp_store):
    rows = tmp_store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = 'etl_job_definitions'"
    ).fetchone()[0]
    assert rows == 1
