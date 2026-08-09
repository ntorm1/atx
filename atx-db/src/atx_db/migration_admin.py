"""PF2-S2 migration governance helpers.

These functions keep live migration applies on the same rails as the offline
tests: checkpoint before copying the DB, preserve a restorable artifact, apply
under the migration lock, then verify the persisted schema contract.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import shutil
import uuid
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Callable, Sequence

import duckdb

from .schema_contract import ColumnSpec, DriftRow, detect_schema_drift


@dataclass(frozen=True)
class BackupArtifact:
    backup_id: str
    run_id: str
    label: str
    database_path: Path
    backup_path: Path
    wal_backup_path: Path | None
    sha256: str
    byte_size: int
    versions_before: tuple[int, ...]
    versions_after: tuple[int, ...] | None = None
    created_at: dt.datetime | None = None


@dataclass(frozen=True)
class GovernedMigrationResult:
    run_id: str
    db_path: Path
    backup: BackupArtifact | None
    applied_versions: tuple[int, ...]
    versions_before: tuple[int, ...]
    versions_after: tuple[int, ...]
    restored: bool = False


@dataclass(frozen=True)
class RetentionResult:
    deleted: tuple[Path, ...]
    kept: tuple[Path, ...]
    refused_unregistered: tuple[Path, ...]
    refused_in_flight: tuple[Path, ...]


class SchemaVerificationError(RuntimeError):
    """Raised when the live schema diverges from the persisted S1 contract."""

    def __init__(self, drift: Sequence[DriftRow]) -> None:
        self.drift = tuple(drift)
        sample = "; ".join(
            f"{row.drift_type}:{row.table_name}"
            + (f".{row.column_name}" if row.column_name else "")
            for row in self.drift[:10]
        )
        suffix = f" ({sample})" if sample else ""
        super().__init__(f"Schema contract verification failed with {len(self.drift)} drift rows{suffix}")


class _ConnectionStore:
    def __init__(self, con: duckdb.DuckDBPyConnection, path: Path | None = None) -> None:
        self.connection = con
        self.path = path

    @property
    def con(self) -> duckdb.DuckDBPyConnection:
        return self.connection


def _utc_now() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)


def _safe_label(label: str) -> str:
    slug = re.sub(r"[^A-Za-z0-9_.-]+", "-", label.strip())
    return slug.strip("-") or "migration"


def _timestamp_label(value: dt.datetime) -> str:
    return value.strftime("%Y%m%d-%H%M%S")


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _wal_path(db_path: Path) -> Path:
    return Path(f"{db_path}.wal")


def _numeric_versions(conn: duckdb.DuckDBPyConnection) -> tuple[int, ...]:
    try:
        rows = conn.execute(
            """
            SELECT CAST(version AS INTEGER)
            FROM schema_migrations
            WHERE version ~ '^[0-9]+$'
            ORDER BY CAST(version AS INTEGER)
            """
        ).fetchall()
    except Exception:
        return ()
    return tuple(row[0] for row in rows)


def _table_exists(conn: duckdb.DuckDBPyConnection, table_name: str) -> bool:
    row = conn.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE schema_name = 'main'
          AND table_name = ?
        """,
        [table_name],
    ).fetchone()
    return bool(row and row[0])


def checkpoint(conn: duckdb.DuckDBPyConnection) -> None:
    """Force DuckDB to checkpoint the database before a file-level backup."""
    conn.execute("CHECKPOINT")


def backup_database(
    db_path: Path | str,
    label: str,
    *,
    conn: duckdb.DuckDBPyConnection | None = None,
    run_id: str | None = None,
    versions_before: Sequence[int] | None = None,
    backup_dir: Path | str | None = None,
    created_at: dt.datetime | None = None,
    register: bool = True,
) -> BackupArtifact:
    """Copy the DuckDB file and any WAL to timestamped ``.bak`` artifacts."""
    source = Path(db_path)
    if str(source) == ":memory:":
        raise ValueError("Cannot file-backup an in-memory DuckDB database")
    if not source.exists():
        raise FileNotFoundError(source)

    created = created_at or _utc_now()
    label_slug = _safe_label(label)
    stamp = _timestamp_label(created)
    dest_dir = Path(backup_dir) if backup_dir is not None else source.parent
    dest_dir.mkdir(parents=True, exist_ok=True)

    backup_path = dest_dir / f"{source.name}.{label_slug}.{stamp}.bak"
    if backup_path.exists():
        backup_path = dest_dir / f"{source.name}.{label_slug}.{stamp}.{uuid.uuid4().hex[:8]}.bak"
    shutil.copy2(source, backup_path)

    wal_backup_path: Path | None = None
    wal_path = _wal_path(source)
    if wal_path.exists():
        wal_backup_path = dest_dir / f"{wal_path.name}.{label_slug}.{stamp}.bak"
        if wal_backup_path.exists():
            wal_backup_path = dest_dir / f"{wal_path.name}.{label_slug}.{stamp}.{uuid.uuid4().hex[:8]}.bak"
        shutil.copy2(wal_path, wal_backup_path)

    artifact = BackupArtifact(
        backup_id=uuid.uuid4().hex,
        run_id=run_id or f"migration-backup-{uuid.uuid4()}",
        label=label_slug,
        database_path=source,
        backup_path=backup_path,
        wal_backup_path=wal_backup_path,
        sha256=_sha256_file(backup_path),
        byte_size=backup_path.stat().st_size,
        versions_before=tuple(versions_before if versions_before is not None else ()),
        created_at=created,
    )
    if conn is not None and register:
        record_backup(conn, artifact)
    return artifact


def record_backup(conn: duckdb.DuckDBPyConnection, artifact: BackupArtifact) -> bool:
    """Persist a backup registry row when migration 0100 has created the table."""
    if not _table_exists(conn, "migration_backup_registry"):
        return False
    conn.execute(
        """
        INSERT OR REPLACE INTO migration_backup_registry (
            backup_id, run_id, label, database_path, backup_path, wal_backup_path,
            sha256, byte_size, versions_before, versions_after, created_at
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        [
            artifact.backup_id,
            artifact.run_id,
            artifact.label,
            str(artifact.database_path),
            str(artifact.backup_path),
            str(artifact.wal_backup_path) if artifact.wal_backup_path is not None else None,
            artifact.sha256,
            artifact.byte_size,
            json.dumps(list(artifact.versions_before)),
            json.dumps(list(artifact.versions_after)) if artifact.versions_after is not None else None,
            artifact.created_at or _utc_now(),
        ],
    )
    return True


def complete_backup_record(
    conn: duckdb.DuckDBPyConnection,
    backup_id: str,
    versions_after: Sequence[int],
) -> None:
    if not _table_exists(conn, "migration_backup_registry"):
        return
    conn.execute(
        """
        UPDATE migration_backup_registry
        SET versions_after = ?
        WHERE backup_id = ?
        """,
        [json.dumps(list(versions_after)), backup_id],
    )


def _load_persisted_contract(conn: duckdb.DuckDBPyConnection) -> dict[str, list[ColumnSpec]]:
    if not _table_exists(conn, "schema_contract"):
        raise RuntimeError("schema_contract table is missing; PF2-S1 must be applied before S2 verification")
    rows = conn.execute(
        """
        SELECT table_name, column_name, data_type, nullable, is_natural_key, is_pit_column, declared_in
        FROM schema_contract
        ORDER BY table_name, column_name
        """
    ).fetchall()
    if not rows:
        raise RuntimeError("schema_contract table is empty; cannot verify warehouse schema")

    manifest: dict[str, list[ColumnSpec]] = {}
    for table_name, column_name, data_type, nullable, is_natural_key, is_pit_column, declared_in in rows:
        manifest.setdefault(table_name, []).append(
            ColumnSpec(
                name=column_name,
                data_type=data_type,
                nullable=bool(nullable),
                is_natural_key=bool(is_natural_key),
                is_pit_column=bool(is_pit_column),
                declared_in=declared_in,
            )
        )
    return manifest


def verify_schema(conn: duckdb.DuckDBPyConnection) -> tuple[DriftRow, ...]:
    """Verify live tables/columns against the persisted PF2-S1 schema contract."""
    drift = tuple(detect_schema_drift(_ConnectionStore(conn), _load_persisted_contract(conn)))
    if drift:
        raise SchemaVerificationError(drift)
    return drift


def restore_database(
    bak_path: Path | str,
    target: Path | str,
    *,
    wal_backup_path: Path | str | None = None,
    clear_locks: bool = True,
) -> Path:
    """Restore ``target`` from a primary DB backup and optional WAL backup."""
    backup = Path(bak_path)
    target_path = Path(target)
    if not backup.exists():
        raise FileNotFoundError(backup)
    target_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(backup, target_path)

    target_wal = _wal_path(target_path)
    if wal_backup_path is not None:
        wal_backup = Path(wal_backup_path)
        if not wal_backup.exists():
            raise FileNotFoundError(wal_backup)
        shutil.copy2(wal_backup, target_wal)
    elif target_wal.exists():
        target_wal.unlink()

    if clear_locks:
        con = duckdb.connect(str(target_path))
        try:
            if _table_exists(con, "migration_apply_lock"):
                con.execute("DELETE FROM migration_apply_lock WHERE lock_name = 'schema_migrations'")
                checkpoint(con)
        finally:
            con.close()
    return target_path


def _prepare_base_schema(conn: duckdb.DuckDBPyConnection, db_path: Path) -> None:
    from .schema import ensure_quant_schema

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS dataset_runs (
            run_id VARCHAR PRIMARY KEY,
            dataset_id VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            started_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP,
            rows_loaded BIGINT,
            source VARCHAR,
            params_json VARCHAR,
            error_message VARCHAR
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS dataset_watermarks (
            dataset_id VARCHAR NOT NULL,
            watermark_name VARCHAR NOT NULL,
            watermark_value VARCHAR NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (dataset_id, watermark_name)
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS security_identifiers (
            symbol VARCHAR NOT NULL,
            id_type VARCHAR NOT NULL,
            id_value VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (symbol, id_type, id_value)
        )
        """
    )
    ensure_quant_schema(_ConnectionStore(conn, db_path))


def run_governed_migrations(
    db_path: Path | str,
    *,
    label: str = "pre-migrate",
    backup_dir: Path | str | None = None,
    verify_func: Callable[[duckdb.DuckDBPyConnection], object] = verify_schema,
    before_verify: Callable[[duckdb.DuckDBPyConnection], None] | None = None,
) -> GovernedMigrationResult:
    """Run checkpoint + backup + locked migration apply + schema verify.

    If apply or verification fails after a backup is created, the target database
    is restored from the pre-flight backup before the original error is reraised.
    """
    from .connection import DuckDBStore
    from .migrations import (
        apply_pending_migrations,
        claim_apply_lock,
        release_apply_lock,
        verify_migration_checksums,
    )

    target = Path(db_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    run_id = f"warehouse-migrate-{uuid.uuid4()}"
    backup: BackupArtifact | None = None
    con: duckdb.DuckDBPyConnection | None = duckdb.connect(str(target))
    lock_claimed = False
    try:
        DuckDBStore(target)._configure_session(con)
        _prepare_base_schema(con, target)
        claim_apply_lock(con, run_id)
        lock_claimed = True
        versions_before = _numeric_versions(con)
        checkpoint(con)
        con.close()
        con = None

        backup = backup_database(
            target,
            label,
            conn=None,
            run_id=run_id,
            versions_before=versions_before,
            backup_dir=backup_dir,
        )

        con = duckdb.connect(str(target))
        DuckDBStore(target)._configure_session(con)
        holder = con.execute(
            """
            SELECT holder_run_id
            FROM migration_apply_lock
            WHERE lock_name = 'schema_migrations'
            """
        ).fetchone()
        if holder != (run_id,):
            raise RuntimeError(f"migration apply lock was lost before apply; expected {run_id}, got {holder}")

        applied = tuple(apply_pending_migrations(con, run_id=run_id, acquire_lock=False))
        versions_after = _numeric_versions(con)
        backup = replace(backup, versions_after=versions_after)
        record_backup(con, backup)
        if before_verify is not None:
            before_verify(con)
        verify_func(con)
        verify_migration_checksums(con)
        complete_backup_record(con, backup.backup_id, versions_after)
        release_apply_lock(con, run_id)
        lock_claimed = False
        checkpoint(con)
        return GovernedMigrationResult(
            run_id=run_id,
            db_path=target,
            backup=backup,
            applied_versions=applied,
            versions_before=versions_before,
            versions_after=versions_after,
            restored=False,
        )
    except Exception:
        if con is not None and lock_claimed and backup is None:
            try:
                release_apply_lock(con, run_id)
            except Exception:
                pass
            lock_claimed = False
        if con is not None:
            con.close()
            con = None
        if backup is not None:
            restore_database(
                backup.backup_path,
                target,
                wal_backup_path=backup.wal_backup_path,
                clear_locks=True,
            )
        raise
    finally:
        if con is not None:
            con.close()


def recover_from_wal_failure(
    db_path: Path | str,
    backup_path: Path | str,
    *,
    wal_backup_path: Path | str | None = None,
    backup_dir: Path | str | None = None,
) -> GovernedMigrationResult:
    """Restore a pre-migration backup, preserve the failed WAL, then re-apply.

    This codifies the S5g/S5k recovery lesson: restore the pre-flight DB copy,
    quarantine the failed WAL artifact, and re-run forward migrations where DDL
    schema changes and index creation live in separate migration numbers.
    """
    target = Path(db_path)
    failed_wal = _wal_path(target)
    if failed_wal.exists():
        quarantine = failed_wal.with_name(f"{failed_wal.name}.failed-{_timestamp_label(_utc_now())}.bak")
        shutil.move(str(failed_wal), str(quarantine))
    restore_database(backup_path, target, wal_backup_path=wal_backup_path, clear_locks=True)
    return run_governed_migrations(target, label="wal-recovery", backup_dir=backup_dir)


def _registered_backups(conn: duckdb.DuckDBPyConnection) -> dict[Path, tuple[Path | None, bool]]:
    if not _table_exists(conn, "migration_backup_registry"):
        return {}
    rows = conn.execute(
        """
        SELECT backup_path, wal_backup_path, versions_after
        FROM migration_backup_registry
        """
    ).fetchall()
    return {
        Path(backup_path): (
            Path(wal_backup_path) if wal_backup_path else None,
            versions_after is None,
        )
        for backup_path, wal_backup_path, versions_after in rows
    }


def enforce_backup_retention(
    directory: Path | str,
    *,
    conn: duckdb.DuckDBPyConnection,
    keep_latest: int,
    min_age: dt.timedelta,
    now: dt.datetime | None = None,
) -> RetentionResult:
    """Prune registered, completed ``.bak`` files while preserving everything else."""
    if keep_latest < 0:
        raise ValueError("keep_latest must be non-negative")
    root = Path(directory)
    current = now or _utc_now()
    registered = _registered_backups(conn)
    all_baks = sorted(root.glob("*.bak"), key=lambda path: path.stat().st_mtime, reverse=True)
    primary_baks = [path for path in all_baks if path in registered]
    protected = set(primary_baks[:keep_latest])

    deleted: list[Path] = []
    kept: list[Path] = []
    refused_unregistered: list[Path] = []
    refused_in_flight: list[Path] = []

    for path in all_baks:
        if path not in registered:
            refused_unregistered.append(path)
            continue
        wal_path, in_flight = registered[path]
        if path in protected:
            kept.append(path)
            continue
        if in_flight:
            refused_in_flight.append(path)
            continue
        file_time = dt.datetime.fromtimestamp(path.stat().st_mtime, dt.timezone.utc).replace(tzinfo=None)
        if current - file_time < min_age:
            kept.append(path)
            continue
        path.unlink()
        deleted.append(path)
        if wal_path is not None and wal_path.exists():
            wal_path.unlink()
            deleted.append(wal_path)

    return RetentionResult(
        deleted=tuple(deleted),
        kept=tuple(kept),
        refused_unregistered=tuple(refused_unregistered),
        refused_in_flight=tuple(refused_in_flight),
    )


def prune_backups(*args, **kwargs) -> RetentionResult:
    """Alias for callers that think in backup-pruning terms."""
    return enforce_backup_retention(*args, **kwargs)
