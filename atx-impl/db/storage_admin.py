from __future__ import annotations

import datetime as dt
import hashlib
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from .connection import DuckDBStore
from .migration_admin import BackupArtifact, backup_database, checkpoint, record_backup
from .warehouse import json_dumps, now_utc_naive


@dataclass(frozen=True)
class StorageStatsResult:
    checked_at: dt.datetime
    db_size_bytes: int
    wal_size_bytes: int
    table_count: int


@dataclass(frozen=True)
class StorageCompactionResult:
    backup: BackupArtifact
    checkpointed: bool
    vacuumed: bool


def _wal_path(db_path: Path) -> Path:
    return Path(f"{db_path}.wal")


def _safe_label(value: str) -> str:
    return "".join(ch if ch.isalnum() or ch in "-_" else "-" for ch in value).strip("-") or "backup"


def _numeric_versions(store: DuckDBStore) -> tuple[int, ...]:
    try:
        rows = store.con.execute(
            """
            SELECT CAST(version AS INTEGER)
            FROM schema_migrations
            WHERE version ~ '^[0-9]+$'
            ORDER BY CAST(version AS INTEGER)
            """
        ).fetchall()
    except Exception:
        return ()
    return tuple(int(row[0]) for row in rows)


def _directory_digest(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    total = 0
    for file_path in sorted(item for item in path.rglob("*") if item.is_file()):
        rel = file_path.relative_to(path).as_posix()
        digest.update(rel.encode("utf-8"))
        digest.update(b"\0")
        data = file_path.read_bytes()
        digest.update(data)
        total += len(data)
    return digest.hexdigest(), total


def _export_database_backup(
    store: DuckDBStore,
    *,
    backup_dir: Path | str | None,
    label: str,
) -> BackupArtifact:
    created_at = now_utc_naive()
    dest_dir = Path(backup_dir) if backup_dir is not None else store.path.parent
    dest_dir.mkdir(parents=True, exist_ok=True)
    stamp = created_at.strftime("%Y%m%d-%H%M%S")
    export_dir = dest_dir / f"{store.path.name}.{_safe_label(label)}.{stamp}.export.bak"
    if export_dir.exists():
        export_dir = dest_dir / f"{store.path.name}.{_safe_label(label)}.{stamp}.{uuid.uuid4().hex[:8]}.export.bak"
    escaped = str(export_dir).replace("'", "''")
    store.con.execute(f"EXPORT DATABASE '{escaped}'")
    sha256, byte_size = _directory_digest(export_dir)
    artifact = BackupArtifact(
        backup_id=uuid.uuid4().hex,
        run_id=f"storage-compact-{uuid.uuid4()}",
        label=label,
        database_path=store.path,
        backup_path=export_dir,
        wal_backup_path=None,
        sha256=sha256,
        byte_size=byte_size,
        versions_before=_numeric_versions(store),
        versions_after=None,
        created_at=created_at,
    )
    record_backup(store.con, artifact)
    return artifact


def _table_names(store: DuckDBStore) -> list[str]:
    return [
        str(row[0])
        for row in store.con.execute(
            """
            SELECT table_name
            FROM duckdb_tables()
            WHERE schema_name = 'main'
              AND coalesce(internal, false) = false
            ORDER BY table_name
            """
        ).fetchall()
    ]


def _quote_ident(identifier: str) -> str:
    return '"' + identifier.replace('"', '""') + '"'


def _table_row_count(store: DuckDBStore, table_name: str) -> int | None:
    try:
        return int(store.con.execute(f"SELECT count(*) FROM {_quote_ident(table_name)}").fetchone()[0])
    except Exception:
        return None


def record_storage_stats(
    store: DuckDBStore,
    *,
    checked_at: dt.datetime | None = None,
    details: dict[str, Any] | None = None,
) -> StorageStatsResult:
    """Record DB/WAL size and per-table row counts into ``warehouse_storage_stats``."""

    db_path = store.path
    if str(db_path) == ":memory:":
        db_size = 0
        wal_size = 0
    else:
        db_size = db_path.stat().st_size if db_path.exists() else 0
        wal = _wal_path(db_path)
        wal_size = wal.stat().st_size if wal.exists() else 0
    measured_at = checked_at or now_utc_naive()
    tables = _table_names(store)
    payload = json_dumps(details or {})
    for table_name in tables:
        row_count = _table_row_count(store, table_name)
        store.con.execute(
            """
            INSERT INTO warehouse_storage_stats (
                storage_stat_id, checked_at, db_path, db_size_bytes,
                wal_size_bytes, table_name, row_count, byte_count,
                details_json, source_loaded_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, NULL, ?, ?)
            """,
            [
                str(uuid.uuid4()),
                measured_at,
                str(db_path),
                db_size,
                wal_size,
                table_name,
                row_count,
                payload,
                measured_at,
            ],
        )
    if not tables:
        store.con.execute(
            """
            INSERT INTO warehouse_storage_stats (
                storage_stat_id, checked_at, db_path, db_size_bytes,
                wal_size_bytes, table_name, row_count, byte_count,
                details_json, source_loaded_at
            )
            VALUES (?, ?, ?, ?, ?, NULL, NULL, NULL, ?, ?)
            """,
            [
                str(uuid.uuid4()),
                measured_at,
                str(db_path),
                db_size,
                wal_size,
                payload,
                measured_at,
            ],
        )
    return StorageStatsResult(
        checked_at=measured_at,
        db_size_bytes=db_size,
        wal_size_bytes=wal_size,
        table_count=len(tables),
    )


def checkpoint_and_compact(
    store: DuckDBStore,
    *,
    backup_dir: Path | str | None = None,
    label: str = "storage-compact",
) -> StorageCompactionResult:
    """Run PF2-S2-backed backup, CHECKPOINT, VACUUM, CHECKPOINT for a file DB."""

    if str(store.path) == ":memory:":
        raise ValueError("storage compaction requires a file-backed DuckDB database")
    checkpoint(store.con)
    try:
        artifact = backup_database(
            store.path,
            label,
            conn=store.con,
            backup_dir=backup_dir,
            register=True,
        )
    except PermissionError:
        artifact = _export_database_backup(store, backup_dir=backup_dir, label=label)
    checkpoint(store.con)
    store.con.execute("VACUUM")
    checkpoint(store.con)
    return StorageCompactionResult(
        backup=artifact,
        checkpointed=True,
        vacuumed=True,
    )
