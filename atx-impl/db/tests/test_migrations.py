"""Test the versioned migration framework."""

from __future__ import annotations


def test_migrations_recorded_after_bootstrap(tmp_store):
    """After bootstrap, schema_migrations should contain versions 1, 2, and 3."""
    rows = tmp_store.con.execute(
        "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$' ORDER BY 1"
    ).fetchall()
    versions = [row[0] for row in rows]
    assert 1 in versions, f"Migration 0001 not recorded; found: {versions}"
    assert 2 in versions, f"Migration 0002 not recorded; found: {versions}"
    assert 3 in versions, f"Migration 0003 not recorded; found: {versions}"


def test_apply_pending_idempotent(tmp_store):
    """Calling apply_pending_migrations again returns [] — already up to date."""
    from db.migrations import apply_pending_migrations

    result = apply_pending_migrations(tmp_store.con)
    assert result == [], f"Expected [] but got {result}"


def test_migration_0002_columns_exist(tmp_store):
    """Columns added by migration 0002 (schema_evolution_alters) must be present."""
    cols_query = """
        SELECT column_name
        FROM information_schema.columns
        WHERE table_schema = 'main'
          AND table_name = ?
    """
    # equity_daily_bars gains vendor_security_id and available_at
    bar_cols = {
        row[0]
        for row in tmp_store.con.execute(cols_query, ["equity_daily_bars"]).fetchall()
    }
    assert "vendor_security_id" in bar_cols, "equity_daily_bars.vendor_security_id missing"
    assert "available_at" in bar_cols, "equity_daily_bars.available_at missing"
    assert "run_id" in bar_cols, "equity_daily_bars.run_id missing"

    # etl_job_definitions gains max_retries and retry_delay_seconds
    job_cols = {
        row[0]
        for row in tmp_store.con.execute(cols_query, ["etl_job_definitions"]).fetchall()
    }
    assert "max_retries" in job_cols, "etl_job_definitions.max_retries missing"
    assert "retry_delay_seconds" in job_cols, "etl_job_definitions.retry_delay_seconds missing"


def test_migrations_ordered_ascending():
    """MIGRATIONS registry must be in ascending version order."""
    from db.migrations import MIGRATIONS

    versions = [m.version for m in MIGRATIONS]
    assert versions == sorted(versions), f"Migrations not in ascending order: {versions}"


def test_migrations_unique_versions():
    """Each migration version must be unique."""
    from db.migrations import MIGRATIONS

    versions = [m.version for m in MIGRATIONS]
    assert len(versions) == len(set(versions)), f"Duplicate migration versions: {versions}"


def test_migration_description_recorded(tmp_store):
    """The description (name) column should be stored alongside the version."""
    rows = tmp_store.con.execute(
        "SELECT version, description FROM schema_migrations WHERE version ~ '^[0-9]+$' ORDER BY version"
    ).fetchall()
    by_version = {int(row[0]): row[1] for row in rows}
    assert by_version.get(1) == "baseline_schema", f"Expected baseline_schema, got {by_version.get(1)!r}"
    assert by_version.get(2) == "schema_evolution_alters", (
        f"Expected schema_evolution_alters, got {by_version.get(2)!r}"
    )
    assert by_version.get(3) == "reference_classifications", (
        f"Expected reference_classifications, got {by_version.get(3)!r}"
    )


def test_migration_0003_tables_exist(tmp_store):
    """Tables introduced by migration 0003 must exist after bootstrap."""
    tables_query = """
        SELECT table_name
        FROM information_schema.tables
        WHERE table_schema = 'main'
    """
    table_names = {row[0] for row in tmp_store.con.execute(tables_query).fetchall()}
    for expected in ("taxonomy", "taxonomy_node", "entity_classification", "taxonomy_mapping"):
        assert expected in table_names, f"Table '{expected}' missing after bootstrap"
