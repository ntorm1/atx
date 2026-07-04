"""PF2-S2 migration governance tests."""

from __future__ import annotations

import datetime as dt
import os
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
    assert "idx_migration_backup_registry_backup_path" in indexes


def test_verify_schema_uses_persisted_contract_and_catches_drift(tmp_store):
    from db.migration_admin import SchemaVerificationError, verify_schema

    assert verify_schema(tmp_store.con) == ()
    tmp_store.con.execute("CREATE TABLE uncatalogued_drift_probe (id INTEGER)")

    with pytest.raises(SchemaVerificationError, match="uncatalogued_drift_probe"):
        verify_schema(tmp_store.con)


def test_backup_database_and_restore_roundtrip(tmp_path):
    from db.connection import DuckDBStore
    from db.migration_admin import backup_database, checkpoint, restore_database

    db_path = tmp_path / "restore_roundtrip.duckdb"
    backup_dir = tmp_path / "backups"
    with DuckDBStore(db_path) as store:
        store.con.execute("CREATE TABLE restore_probe (id INTEGER)")
        store.con.execute("INSERT INTO restore_probe VALUES (1)")
        checkpoint(store.con)

    artifact = backup_database(
        db_path,
        "unit-restore",
        run_id="roundtrip-test",
        versions_before=(100, 101),
        backup_dir=backup_dir,
    )
    con = duckdb.connect(str(db_path))
    try:
        con.execute("INSERT INTO restore_probe VALUES (2)")
        con.execute("CHECKPOINT")
    finally:
        con.close()

    assert artifact.backup_path.exists()
    assert artifact.backup_path.parent == backup_dir
    assert artifact.byte_size > 0
    assert len(artifact.sha256) == 64

    restore_database(artifact.backup_path, db_path, wal_backup_path=artifact.wal_backup_path)
    con = duckdb.connect(str(db_path))
    try:
        rows = con.execute("SELECT id FROM restore_probe ORDER BY id").fetchall()
    finally:
        con.close()
    assert rows == [(1,)]


def test_run_governed_migrations_restores_on_verify_failure(tmp_path):
    from db.connection import DuckDBStore
    from db.migration_admin import run_governed_migrations

    db_path = tmp_path / "restore_on_verify_failure.duckdb"
    backup_dir = tmp_path / "backups"
    with DuckDBStore(db_path):
        pass

    def create_bad_post_backup_table(conn):
        conn.execute("CREATE TABLE injected_after_backup (id INTEGER)")

    def fail_verify(conn):
        conn.execute("SELECT count(*) FROM injected_after_backup")
        raise RuntimeError("forced verify failure")

    with pytest.raises(RuntimeError, match="forced verify failure"):
        run_governed_migrations(
            db_path,
            backup_dir=backup_dir,
            before_verify=create_bad_post_backup_table,
            verify_func=fail_verify,
        )

    con = duckdb.connect(str(db_path))
    try:
        table_count = con.execute(
            """
            SELECT count(*)
            FROM duckdb_tables()
            WHERE table_name = 'injected_after_backup'
            """
        ).fetchone()[0]
        lock_count = con.execute(
            """
            SELECT count(*)
            FROM migration_apply_lock
            WHERE lock_name = 'schema_migrations'
            """
        ).fetchone()[0]
    finally:
        con.close()
    assert table_count == 0
    assert lock_count == 0


def test_governed_forward_migration_on_populated_pre_s2_db(tmp_path, monkeypatch):
    from db.connection import DuckDBStore
    from db.migration_admin import run_governed_migrations, verify_schema
    import db.migrations as migrations

    db_path = tmp_path / "pre_s2_populated.duckdb"
    backup_dir = tmp_path / "backups"
    pre_s2_migrations = [migration for migration in migrations.MIGRATIONS if migration.version < 100]

    with monkeypatch.context() as patch:
        patch.setattr(migrations, "MIGRATIONS", pre_s2_migrations)
        with DuckDBStore(db_path) as store:
            store.con.execute(
                """
                INSERT OR REPLACE INTO source_systems (
                    source_system_id, name, base_url, license_note, cadence,
                    requires_key, metadata_json, created_at, updated_at
                )
                VALUES ('s2_forward_fixture', 'S2 Forward Fixture', NULL, NULL, 'manual', false, '{}', now(), now())
                """
            )

    result = run_governed_migrations(db_path, backup_dir=backup_dir)
    expected_forward_versions = tuple(
        migration.version for migration in migrations.MIGRATIONS if migration.version >= 100
    )
    assert result.applied_versions == expected_forward_versions
    assert result.backup is not None
    assert result.backup.backup_path.exists()

    with DuckDBStore(db_path) as store:
        assert verify_schema(store.con) == ()
        row = store.con.execute(
            "SELECT name FROM source_systems WHERE source_system_id = 's2_forward_fixture'"
        ).fetchone()
        registry = store.con.execute(
            """
            SELECT versions_before, versions_after
            FROM migration_backup_registry
            WHERE backup_id = ?
            """,
            [result.backup.backup_id],
        ).fetchone()

    assert row == ("S2 Forward Fixture",)
    assert registry is not None
    assert "99" in registry[0]
    assert str(expected_forward_versions[-1]) in registry[1]


def test_recover_from_wal_failure_restores_backup_and_reapplies(tmp_path):
    from db.connection import DuckDBStore
    from db.migration_admin import backup_database, checkpoint, recover_from_wal_failure

    db_path = tmp_path / "wal_recovery.duckdb"
    backup_dir = tmp_path / "backups"
    with DuckDBStore(db_path) as store:
        store.con.execute(
            """
            INSERT OR REPLACE INTO source_systems (
                source_system_id, name, base_url, license_note, cadence,
                requires_key, metadata_json, created_at, updated_at
            )
            VALUES ('wal_recovery_before', 'Before recovery', NULL, NULL, 'manual', false, '{}', now(), now())
            """
        )
        checkpoint(store.con)

    artifact = backup_database(
        db_path,
        "pre-wal-failure",
        run_id="wal-recovery-test",
        versions_before=(100, 101),
        backup_dir=backup_dir,
    )

    con = duckdb.connect(str(db_path))
    try:
        con.execute(
            """
            INSERT OR REPLACE INTO source_systems (
                source_system_id, name, base_url, license_note, cadence,
                requires_key, metadata_json, created_at, updated_at
            )
            VALUES ('wal_recovery_after', 'After backup mutation', NULL, NULL, 'manual', false, '{}', now(), now())
            """
        )
        con.execute("CHECKPOINT")
    finally:
        con.close()
    failed_wal = tmp_path / "wal_recovery.duckdb.wal"
    failed_wal.write_text("failed wal fixture", encoding="utf-8")

    result = recover_from_wal_failure(db_path, artifact.backup_path, backup_dir=backup_dir)
    assert result.backup is not None

    con = duckdb.connect(str(db_path))
    try:
        rows = con.execute(
            """
            SELECT source_system_id
            FROM source_systems
            WHERE source_system_id LIKE 'wal_recovery_%'
            ORDER BY source_system_id
            """
        ).fetchall()
    finally:
        con.close()
    assert rows == [("wal_recovery_before",)]
    assert not failed_wal.exists()
    assert list(tmp_path.glob("wal_recovery.duckdb.wal.failed-*.bak"))


def test_enforce_backup_retention_prunes_only_registered_completed_backups(tmp_path):
    from db.connection import DuckDBStore
    from db.migration_admin import (
        backup_database,
        complete_backup_record,
        enforce_backup_retention,
        record_backup,
    )

    db_path = tmp_path / "retention.duckdb"
    backup_dir = tmp_path / "backups"
    now = dt.datetime(2026, 7, 4, 12, 0, 0)
    with DuckDBStore(db_path) as store:
        store.con.execute("CHECKPOINT")

    old = backup_database(
        db_path,
        "old-complete",
        run_id="retention-test",
        versions_before=(100,),
        backup_dir=backup_dir,
        created_at=now - dt.timedelta(days=5),
    )
    with duckdb.connect(str(db_path)) as con:
        record_backup(con, old)
        complete_backup_record(con, old.backup_id, (100, 101))

    latest = backup_database(
        db_path,
        "latest-complete",
        run_id="retention-test",
        versions_before=(100,),
        backup_dir=backup_dir,
        created_at=now - dt.timedelta(days=4),
    )
    with duckdb.connect(str(db_path)) as con:
        record_backup(con, latest)
        complete_backup_record(con, latest.backup_id, (100, 101))

    in_flight = backup_database(
        db_path,
        "in-flight",
        run_id="retention-test",
        versions_before=(100,),
        backup_dir=backup_dir,
        created_at=now - dt.timedelta(days=6),
    )
    with duckdb.connect(str(db_path)) as con:
        record_backup(con, in_flight)

    for path, age_days in (
        (old.backup_path, 5),
        (latest.backup_path, 4),
        (in_flight.backup_path, 6),
    ):
        stamp = (now - dt.timedelta(days=age_days)).timestamp()
        os.utime(path, (stamp, stamp))

    rogue = backup_dir / "rogue.bak"
    rogue.write_text("not registered", encoding="utf-8")
    os.utime(rogue, ((now - dt.timedelta(days=7)).timestamp(),) * 2)

    with duckdb.connect(str(db_path)) as con:
        result = enforce_backup_retention(
            backup_dir,
            conn=con,
            keep_latest=1,
            min_age=dt.timedelta(days=1),
            now=now,
        )

    assert old.backup_path in result.deleted
    assert not old.backup_path.exists()
    assert latest.backup_path in result.kept
    assert latest.backup_path.exists()
    assert in_flight.backup_path in result.refused_in_flight
    assert in_flight.backup_path.exists()
    assert rogue in result.refused_unregistered
    assert rogue.exists()
