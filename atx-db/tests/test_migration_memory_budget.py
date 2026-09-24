"""Small persistent governance checks, without bootstrapping the whole warehouse."""

from __future__ import annotations

import hashlib
import json

import duckdb
import pytest

import atx_db.migration_admin as admin
import atx_db.migrations as migrations


def _prepare_small_schema(con, _path):
    con.execute("""
        CREATE TABLE IF NOT EXISTS earnings (
            issuer VARCHAR PRIMARY KEY, diluted_eps DOUBLE NOT NULL
        );
        INSERT INTO earnings VALUES ('CVX', 2.25) ON CONFLICT DO NOTHING;
        CREATE TABLE IF NOT EXISTS schema_migrations (
            version VARCHAR PRIMARY KEY, description VARCHAR NOT NULL,
            checksum VARCHAR, applied_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS migration_apply_lock (
            lock_name VARCHAR PRIMARY KEY, holder_run_id VARCHAR NOT NULL,
            heartbeat_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE TABLE IF NOT EXISTS migration_backup_registry (
            backup_id VARCHAR PRIMARY KEY, run_id VARCHAR, label VARCHAR,
            database_path VARCHAR, backup_path VARCHAR, wal_backup_path VARCHAR,
            sha256 VARCHAR, byte_size BIGINT, versions_before VARCHAR,
            versions_after VARCHAR, created_at TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS table_catalog (table_name VARCHAR PRIMARY KEY);
        CREATE TABLE IF NOT EXISTS schema_contract (
            table_name VARCHAR, column_name VARCHAR, data_type VARCHAR,
            nullable BOOLEAN, is_natural_key BOOLEAN, is_pit_column BOOLEAN,
            declared_in VARCHAR, PRIMARY KEY (table_name, column_name)
        );
        INSERT OR IGNORE INTO table_catalog VALUES
            ('earnings'), ('schema_migrations'), ('migration_apply_lock'),
            ('migration_backup_registry'), ('table_catalog'), ('schema_contract');
        INSERT OR IGNORE INTO schema_contract
            SELECT table_name, column_name, data_type, is_nullable,
                   false, false, 'schema_py'
            FROM duckdb_columns()
            WHERE database_name = current_database() AND schema_name = 'main'
              AND table_name IN (SELECT table_name FROM table_catalog);
    """)


def _add_migration_marker(con):
    con.execute("ALTER TABLE earnings ADD COLUMN migrated BOOLEAN DEFAULT true")
    con.execute("""
        INSERT INTO schema_contract VALUES
            ('earnings', 'migrated', 'BOOLEAN', true, false, false, 'migration')
    """)


@pytest.mark.parametrize("failure", [None, "schema", "checksum", "checkpoint"])
def test_governed_connections_are_bounded_and_preserve_recovery(tmp_path, monkeypatch, failure):
    path = tmp_path / "governed.duckdb"
    backup_dir = tmp_path / "backups"
    original_connect = duckdb.connect
    with original_connect(str(path), config={"memory_limit": "512MB", "threads": "1"}) as con:
        _prepare_small_schema(con, path)
        expected_memory = con.execute("SELECT current_setting('memory_limit')").fetchone()[0]
        assert admin.verify_schema(con) == ()

    observations = []

    def observe_connect(*args, **kwargs):
        con = original_connect(*args, **kwargs)
        # Observe immediately, before governance has issued any session SET.
        observations.append((kwargs.get("config"), con.execute("""
            SELECT current_setting('memory_limit'), current_setting('threads'),
                   current_setting('preserve_insertion_order'), current_setting('temp_directory')
        """).fetchone()))
        return con

    monkeypatch.setattr(duckdb, "connect", observe_connect)
    monkeypatch.setattr(admin, "_prepare_base_schema", _prepare_small_schema)
    monkeypatch.setattr(migrations, "MIGRATIONS", [
        migrations.Migration(1, "add_migration_marker", _add_migration_marker),
    ])

    def inject_failure(con):
        # Real schema verification and checksum verification remain enabled.
        if failure == "schema":
            con.execute("ALTER TABLE earnings ADD COLUMN unexpected INTEGER")
        elif failure == "checksum":
            con.execute("UPDATE schema_migrations SET checksum = 'tampered'")

    original_checkpoint = admin.checkpoint
    checkpoints = 0

    def fail_final_checkpoint(con):
        nonlocal checkpoints
        checkpoints += 1
        if checkpoints == 2:
            assert con.execute("SELECT count(*) FROM migration_apply_lock").fetchone() == (0,)
            assert con.execute("SELECT migrated FROM earnings").fetchone() == (True,)
            raise MemoryError("forced final checkpoint allocation failure")
        original_checkpoint(con)

    if failure == "checkpoint":
        monkeypatch.setattr(admin, "checkpoint", fail_final_checkpoint)

    if failure is None:
        result = admin.run_governed_migrations(path, backup_dir=backup_dir)
        assert result.applied_versions == (1,)
        assert result.versions_before == ()
        assert result.versions_after == (1,)
        assert result.backup is not None
        assert hashlib.sha256(result.backup.backup_path.read_bytes()).hexdigest() == result.backup.sha256
    else:
        exception, message = {
            "schema": (admin.SchemaVerificationError, "unexpected"),
            "checksum": (RuntimeError, "Migration checksum verification failed"),
            "checkpoint": (MemoryError, "forced final checkpoint allocation failure"),
        }[failure]
        with pytest.raises(exception, match=message):
            admin.run_governed_migrations(path, backup_dir=backup_dir, before_verify=inject_failure)

    # Two opens on success; recovery adds a third, bounded lock-cleanup open.
    expected_temp = (path.parent / f".{path.name}.duckdb_tmp").as_posix()
    expected_config = {
        "memory_limit": "512MB", "threads": "1", "preserve_insertion_order": "false",
        "temp_directory": expected_temp,
    }
    assert observations == [
        (expected_config, (expected_memory, 1, False, expected_temp))
    ] * (2 if failure is None else 3)

    with original_connect(str(path), config=expected_config) as con:
        assert con.execute("SELECT issuer, diluted_eps FROM earnings").fetchall() == [("CVX", 2.25)]
        assert con.execute("SELECT count(*) FROM migration_apply_lock").fetchone() == (0,)
        assert admin.verify_schema(con) == ()
        migrations.verify_migration_checksums(con)
        if failure is None:
            assert con.execute("SELECT migrated FROM earnings").fetchone() == (True,)
            registry = con.execute(
                "SELECT versions_before, versions_after FROM migration_backup_registry"
            ).fetchone()
            assert tuple(map(json.loads, registry)) == ([], [1])
        else:
            assert con.execute("SELECT count(*) FROM schema_migrations").fetchone() == (0,)
            assert con.execute("SELECT count(*) FROM migration_backup_registry").fetchone() == (0,)
            assert [row[0] for row in con.execute("DESCRIBE earnings").fetchall()] == ["issuer", "diluted_eps"]
    backups = list(backup_dir.glob("*.bak"))
    assert len(backups) == 1
    with original_connect(str(backups[0]), read_only=True, config=expected_config) as con:
        assert con.execute("SELECT issuer, diluted_eps FROM earnings").fetchall() == [("CVX", 2.25)]
        assert con.execute("SELECT count(*) FROM schema_migrations").fetchone() == (0,)
        # The retained backup remains the pre-apply artifact; only the restored
        # target has its stale sentinel cleared.
        assert con.execute("SELECT count(*) FROM migration_apply_lock").fetchone() == (1,)
