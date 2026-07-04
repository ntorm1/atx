"""PF2-S2 migration governance tests."""

from __future__ import annotations

from dataclasses import replace

import duckdb
import pytest


def _numeric_migration_checksums(conn) -> dict[int, str | None]:
    rows = conn.execute(
        """
        SELECT CAST(version AS INTEGER), checksum
        FROM schema_migrations
        WHERE version ~ '^[0-9]+$'
        ORDER BY CAST(version AS INTEGER)
        """
    ).fetchall()
    return {version: checksum for version, checksum in rows}


def test_bootstrap_records_checksum_for_every_migration(tmp_store):
    from db.migrations import MIGRATIONS

    checksums = _numeric_migration_checksums(tmp_store.con)
    for migration in MIGRATIONS:
        checksum = checksums.get(migration.version)
        assert checksum is not None, f"Migration {migration.version:04d} has no checksum"
        assert len(checksum) == 64, f"Migration {migration.version:04d} checksum is not sha256"


def test_verify_migration_checksums_names_tampered_version(tmp_store, monkeypatch):
    import db.migrations as migrations

    target = next(migration for migration in migrations.MIGRATIONS if migration.version == 1)

    def tampered_up(conn):
        conn.execute("SELECT 1")

    patched = [
        replace(migration, up=tampered_up)
        if migration.version == target.version
        else migration
        for migration in migrations.MIGRATIONS
    ]
    monkeypatch.setattr(migrations, "MIGRATIONS", patched)

    with pytest.raises(RuntimeError, match="0001"):
        migrations.verify_migration_checksums(tmp_store.con)


def test_current_schema_initialize_verifies_checksums(tmp_store):
    from db.connection import DuckDBStore

    db_path = tmp_store.path
    tmp_store.con.execute(
        "UPDATE schema_migrations SET checksum = 'tampered' WHERE version = '0101'"
    )
    tmp_store.con.close()
    tmp_store.connection = None

    with pytest.raises(RuntimeError, match="0101"):
        with DuckDBStore(db_path):
            pass


def test_migration_checksum_includes_direct_helper_source(monkeypatch):
    import db.migrations as migrations

    migration_0100 = next(migration for migration in migrations.MIGRATIONS if migration.version == 100)
    original = migrations._migration_source_checksum(migration_0100)

    def changed_backfill(conn):
        conn.execute("SELECT 42")

    monkeypatch.setattr(migrations, "_backfill_missing_migration_checksums", changed_backfill)

    assert migrations._migration_source_checksum(migration_0100) != original


def test_apply_pending_migrations_clean_current_db_returns_empty(tmp_store):
    from db.migrations import apply_pending_migrations

    assert apply_pending_migrations(tmp_store.con) == []


def test_held_apply_lock_fails_fast(tmp_store):
    from db.migrations import acquire_apply_lock, apply_pending_migrations

    with acquire_apply_lock(tmp_store.con, "held-by-test"):
        with pytest.raises(RuntimeError, match="migration apply lock.*held-by-test"):
            apply_pending_migrations(tmp_store.con)


def test_apply_lock_release_is_holder_scoped(tmp_store):
    from db.migrations import acquire_apply_lock, release_apply_lock

    with acquire_apply_lock(tmp_store.con, "holder-a"):
        with pytest.raises(RuntimeError, match="holder-a"):
            release_apply_lock(tmp_store.con, "holder-b")
        holder = tmp_store.con.execute(
            "SELECT holder_run_id FROM migration_apply_lock WHERE lock_name = 'schema_migrations'"
        ).fetchone()
        assert holder == ("holder-a",)

    remaining = tmp_store.con.execute(
        "SELECT count(*) FROM migration_apply_lock WHERE lock_name = 'schema_migrations'"
    ).fetchone()[0]
    assert remaining == 0


def test_apply_lock_blocks_second_connection(tmp_store, tmp_path):
    from db.connection import DuckDBStore
    from db.migrations import acquire_apply_lock

    db_path = tmp_path / "lock_contention.duckdb"
    with DuckDBStore(db_path):
        pass
    con_a = duckdb.connect(str(db_path))
    con_b = duckdb.connect(str(db_path))
    try:
        with acquire_apply_lock(con_a, "holder-a"):
            with pytest.raises(RuntimeError, match="migration apply lock.*holder-a"):
                with acquire_apply_lock(con_b, "holder-b"):
                    pass
    finally:
        con_a.close()
        con_b.close()


def test_migration_governance_migrations_schema_catalog_and_indexes(tmp_store):
    from db.migrations import MIGRATIONS

    migrations_by_version = {migration.version: migration for migration in MIGRATIONS}
    assert migrations_by_version[100].name == "migration_governance_schema"
    assert migrations_by_version[101].name == "migration_governance_indexes"

    tables = {
        row[0]
        for row in tmp_store.con.execute(
            "SELECT table_name FROM duckdb_tables()"
        ).fetchall()
    }
    assert {"migration_apply_lock", "migration_backup_registry"}.issubset(tables)

    catalogued_tables = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT table_name
            FROM table_catalog
            WHERE table_name IN ('migration_apply_lock', 'migration_backup_registry')
            """
        ).fetchall()
    }
    assert catalogued_tables == {"migration_apply_lock", "migration_backup_registry"}

    for table_name in ("migration_apply_lock", "migration_backup_registry"):
        columns = {
            row[0]
            for row in tmp_store.con.execute(
                "SELECT column_name FROM duckdb_columns() WHERE table_name = ?",
                [table_name],
            ).fetchall()
        }
        fields = {
            row[0]
            for row in tmp_store.con.execute(
                "SELECT field_name FROM field_catalog WHERE table_name = ?",
                [table_name],
            ).fetchall()
        }
        assert columns.issubset(fields)

    indexes = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT index_name
            FROM duckdb_indexes()
            WHERE table_name IN ('migration_apply_lock', 'migration_backup_registry')
            """
        ).fetchall()
    }
    assert "idx_migration_apply_lock_heartbeat_at" in indexes
    assert "idx_migration_backup_registry_run_id" in indexes
    assert "idx_migration_backup_registry_created_at" in indexes
