"""Versioned migration framework for the atx-impl DuckDB warehouse.

Migrations are ordered, idempotent, and tracked in the schema_migrations table.
Call apply_pending_migrations(conn) after ensure_quant_schema to bring the schema
up to date. It is safe to call multiple times; only unapplied migrations run.
"""

from __future__ import annotations

import ast
import contextlib
import hashlib
import inspect
import textwrap
import uuid

import duckdb
from dataclasses import dataclass
from typing import Callable


@dataclass(frozen=True)
class Migration:
    version: int
    name: str
    up: Callable[[duckdb.DuckDBPyConnection], None]



def _migration_version_text(version: int) -> str:
    return str(version).zfill(4)


def _migration_source_checksum(migration: Migration) -> str:
    """Stable sha256 digest of a migration's ``up`` source plus direct helper sources."""
    parts: list[tuple[str, str]] = []
    seen: set[int] = set()

    def normalize_source(func: Callable) -> str:
        source = inspect.getsource(func)
        return textwrap.dedent(source).replace("\r\n", "\n").replace("\r", "\n").strip()

    def direct_module_helper_names(source: str) -> set[str]:
        try:
            tree = ast.parse(source)
        except SyntaxError:
            return set()
        names: set[str] = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
                name = node.func.id
                helper = migration.up.__globals__.get(name)
                if callable(helper) and getattr(helper, "__name__", "") != migration.up.__name__:
                    names.add(name)
        return names

    def collect(name: str, func: Callable, depth: int) -> None:
        identity = id(func)
        if identity in seen:
            return
        seen.add(identity)
        try:
            source = normalize_source(func)
        except (OSError, TypeError):
            source = repr(func)
        parts.append((name, source))
        if depth >= 4:
            return
        for helper_name in sorted(direct_module_helper_names(source)):
            helper = migration.up.__globals__.get(helper_name)
            if callable(helper):
                collect(helper_name, helper, depth + 1)

    collect(migration.up.__name__, migration.up, 0)
    payload = "\n\n".join(
        f"# symbol:{name}\n{source}" for name, source in sorted(parts, key=lambda item: item[0])
    )
    return hashlib.sha256(f"{payload}\n".encode("utf-8")).hexdigest()


def _migration_by_version() -> dict[int, Migration]:
    _validate_migration_registry()
    return {migration.version: migration for migration in MIGRATIONS}


def _validate_migration_registry() -> None:
    versions = [migration.version for migration in MIGRATIONS]
    if versions != sorted(versions):
        raise RuntimeError(f"MIGRATIONS must be sorted ascending: {versions}")
    duplicates = sorted({version for version in versions if versions.count(version) > 1})
    if duplicates:
        formatted = ", ".join(_migration_version_text(version) for version in duplicates)
        raise RuntimeError(f"MIGRATIONS contains duplicate versions: {formatted}")


def verify_migration_checksums(
    conn: duckdb.DuckDBPyConnection, *, allow_missing: bool = False
) -> None:
    """Verify the append-only invariant for every applied numeric migration."""
    migrations_by_version = _migration_by_version()
    rows = conn.execute(
        """
        SELECT CAST(version AS INTEGER) AS version_int, version, checksum
        FROM schema_migrations
        WHERE version ~ '^[0-9]+$'
        ORDER BY version_int
        """
    ).fetchall()

    failures: list[str] = []
    for version_int, _version_text, stored_checksum in rows:
        migration = migrations_by_version.get(version_int)
        display_version = _migration_version_text(version_int)
        if migration is None:
            failures.append(f"{display_version}: no migration source is registered")
            continue
        expected_checksum = _migration_source_checksum(migration)
        if stored_checksum in (None, ""):
            if allow_missing:
                continue
            failures.append(f"{display_version}: missing stored checksum")
            continue
        if stored_checksum != expected_checksum:
            failures.append(
                f"{display_version}: stored checksum {stored_checksum} "
                f"does not match current source {expected_checksum}"
            )

    if failures:
        raise RuntimeError(
            "Migration checksum verification failed: " + "; ".join(failures)
        )


def _backfill_missing_migration_checksums(conn: duckdb.DuckDBPyConnection) -> None:
    migrations_by_version = _migration_by_version()
    rows = conn.execute(
        """
        SELECT CAST(version AS INTEGER) AS version_int, version
        FROM schema_migrations
        WHERE version ~ '^[0-9]+$'
          AND (checksum IS NULL OR checksum = '')
        ORDER BY version_int
        """
    ).fetchall()
    updates = [
        (_migration_source_checksum(migrations_by_version[version_int]), version_text)
        for version_int, version_text in rows
        if version_int in migrations_by_version
    ]
    if updates:
        conn.executemany(
            """
            UPDATE schema_migrations
            SET checksum = ?
            WHERE version = ?
              AND (checksum IS NULL OR checksum = '')
            """,
            updates,
        )


def _ensure_apply_lock_table(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS migration_apply_lock (
            lock_name VARCHAR PRIMARY KEY,
            holder_run_id VARCHAR NOT NULL,
            heartbeat_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )


def release_apply_lock(conn: duckdb.DuckDBPyConnection, run_id: str) -> None:
    """Release the singleton migration apply lock held by ``run_id``."""
    table_exists = conn.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE table_name = 'migration_apply_lock'
        """
    ).fetchone()[0]
    if not table_exists:
        return

    row = conn.execute(
        """
        SELECT holder_run_id
        FROM migration_apply_lock
        WHERE lock_name = 'schema_migrations'
        """
    ).fetchone()
    if row is None:
        return
    holder = row[0]
    if holder != run_id:
        raise RuntimeError(
            "migration apply lock is held "
            f"by run_id {holder}; refusing release by run_id {run_id}"
        )
    conn.execute(
        """
        DELETE FROM migration_apply_lock
        WHERE lock_name = 'schema_migrations'
          AND holder_run_id = ?
        """,
        [run_id],
    )


def claim_apply_lock(conn: duckdb.DuckDBPyConnection, run_id: str) -> None:
    """Persistently claim the migration apply sentinel row."""
    _ensure_apply_lock_table(conn)
    try:
        conn.execute(
            """
            INSERT INTO migration_apply_lock (lock_name, holder_run_id, heartbeat_at)
            VALUES ('schema_migrations', ?, CURRENT_TIMESTAMP)
            """,
            [run_id],
        )
    except Exception as exc:
        row = conn.execute(
            """
            SELECT holder_run_id, heartbeat_at
            FROM migration_apply_lock
            WHERE lock_name = 'schema_migrations'
            """
        ).fetchone()
        holder, heartbeat = row if row is not None else ("unknown", "unknown")
        raise RuntimeError(
            "migration apply lock is already held "
            f"by run_id {holder} heartbeat_at {heartbeat}; aborting"
        ) from exc


@contextlib.contextmanager
def acquire_apply_lock(conn: duckdb.DuckDBPyConnection, run_id: str):
    """Acquire the migration apply sentinel row or fail fast with the holder."""
    claim_apply_lock(conn, run_id)
    try:
        yield
    finally:
        release_apply_lock(conn, run_id)



def _apply_pending_migrations_unlocked(conn: duckdb.DuckDBPyConnection) -> list[int]:
    rows = conn.execute(
        "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
    ).fetchall()
    applied: set[int] = {row[0] for row in rows}

    verify_migration_checksums(conn, allow_missing=100 not in applied)

    applied_now: list[int] = []
    for migration in sorted(MIGRATIONS, key=lambda m: m.version):
        if migration.version in applied:
            continue
        checksum = _migration_source_checksum(migration)
        # Run inside a transaction so a failure rolls back cleanly.
        conn.execute("BEGIN TRANSACTION")
        try:
            migration.up(conn)
            conn.execute(
                """
                INSERT INTO schema_migrations (version, description, checksum, applied_at)
                VALUES (?, ?, ?, CURRENT_TIMESTAMP)
                """,
                [_migration_version_text(migration.version), migration.name, checksum],
            )
            conn.execute("COMMIT")
        except Exception:
            conn.execute("ROLLBACK")
            raise
        applied_now.append(migration.version)

    return applied_now


def apply_pending_migrations(
    conn: duckdb.DuckDBPyConnection,
    *,
    run_id: str | None = None,
    acquire_lock: bool = True,
) -> list[int]:
    """Apply any MIGRATIONS whose version is not yet recorded in schema_migrations.

    Runs each migration inside a transaction. Inserts a tracking row on success.
    Returns the list of version numbers that were applied (empty list if all up to date).
    Must be called after ensure_quant_schema so that schema_migrations exists.

    The schema_migrations table was created by ensure_quant_schema with columns:
        version VARCHAR PRIMARY KEY, description VARCHAR NOT NULL,
        checksum VARCHAR, applied_at TIMESTAMP NOT NULL DEFAULT now()
    We cast version int to zero-padded VARCHAR for storage.
    """
    if not acquire_lock:
        return _apply_pending_migrations_unlocked(conn)

    lock_run_id = run_id or f"migration-apply-{uuid.uuid4()}"
    with acquire_apply_lock(conn, lock_run_id):
        return _apply_pending_migrations_unlocked(conn)
