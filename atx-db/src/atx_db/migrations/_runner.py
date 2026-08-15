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
from collections.abc import Callable, Iterator
from dataclasses import dataclass

import duckdb

# Exact legacy fingerprints produced before imported runtime callables were excluded
# from migration checksums. These four migrations instantiate DuckDBStore, whose
# session configuration legitimately changed when the warehouse standardized on UTC.
# Accepting only the persisted, audited predecessors lets existing warehouses cross
# the checksum-algorithm boundary without accepting arbitrary historical drift.
_ACCEPTED_LEGACY_CHECKSUMS: dict[int, frozenset[str]] = {
    100: frozenset({"43ada89b7dfd768ef0957ee38b8488ff5737e7ab1e45044533da29cbde806a76"}),
    189: frozenset(
        {
            "6e144c6c12c410089bce8f06da14feb0d2d0c49b684b663931244674731a5b57",
            "bc16d80f8047189c86d961300f96f8b2dd91ab75f7b440c2e9056b243b907731",
        }
    ),
    190: frozenset(
        {
            "2385d2c4e0918080c49493060b3f74647bfc53375e13dfd4173c65619452ba40",
            "88b8c0431c2281325b6f6107c14b0b10caad271d6543374516772a72ddc83729",
        }
    ),
    191: frozenset(
        {
            "3f396fe07286e7fc11979343490dfcf4d6e9e6b266153481f90c9a3262f0ffaf",
            "84fed9e68cfd1ac2ba75e3d6248dc7eba41f089b1c010d31737d778ce01c98e8",
        }
    ),
    192: frozenset(
        {
            "5f6e97a020d021d52dcb9389a43357568f6b5a642551abe031b4e54abcb695ce",
            "996798594e9b7983d328704291bbc28a0a94160d8a4719e7374fcae9c45f6e59",
        }
    ),
}


@dataclass(frozen=True)
class Migration:
    version: int
    name: str
    up: Callable[[duckdb.DuckDBPyConnection], None]


MIGRATIONS: list[Migration] = []



def _migration_version_text(version: int) -> str:
    return str(version).zfill(4)


def _migration_source_checksum_with_scope(
    migration: Migration, *, migration_local_helpers_only: bool
) -> str:
    parts: list[tuple[str, str]] = []
    seen: set[int] = set()
    migration_module = migration.up.__module__

    def normalize_source(func: Callable[..., object]) -> str:
        source = inspect.getsource(func)
        return textwrap.dedent(source).replace("\r\n", "\n").replace("\r", "\n").strip()

    def direct_module_helpers(
        func: Callable[..., object], source: str
    ) -> dict[str, Callable[..., object]]:
        try:
            tree = ast.parse(source)
        except SyntaxError:
            return {}
        helpers: dict[str, Callable[..., object]] = {}
        for node in ast.walk(tree):
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
                name = node.func.id
                helper_globals = (
                    func.__globals__
                    if migration_local_helpers_only
                    else migration.up.__globals__
                )
                helper = helper_globals.get(name)
                if not callable(helper) or getattr(helper, "__name__", "") == func.__name__:
                    continue
                if (
                    migration_local_helpers_only
                    and getattr(helper, "__module__", None) != migration_module
                ):
                    continue
                helpers[name] = helper
        return helpers

    def collect(name: str, func: Callable[..., object], depth: int) -> None:
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
        for helper_name, helper in sorted(direct_module_helpers(func, source).items()):
            collect(helper_name, helper, depth + 1)

    collect(migration.up.__name__, migration.up, 0)
    payload = "\n\n".join(
        f"# symbol:{name}\n{source}" for name, source in sorted(parts, key=lambda item: item[0])
    )
    return hashlib.sha256(f"{payload}\n".encode()).hexdigest()


def _migration_source_checksum(migration: Migration) -> str:
    """Hash a migration body and helpers defined in the migration's own module.

    Imported runtime classes and functions are deliberately excluded. Their source can
    evolve without changing the already-applied DDL/DML body, and including them made
    unrelated connection/session changes invalidate historical migrations.
    """

    return _migration_source_checksum_with_scope(migration, migration_local_helpers_only=True)


def _legacy_migration_source_checksum(migration: Migration) -> str:
    """Return the pre-UTC checksum shape for compatibility verification only."""

    return _migration_source_checksum_with_scope(migration, migration_local_helpers_only=False)


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
        accepted_checksums = {
            expected_checksum,
            _legacy_migration_source_checksum(migration),
            *_ACCEPTED_LEGACY_CHECKSUMS.get(version_int, ()),
        }
        if stored_checksum not in accepted_checksums:
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
    table_exists_row = conn.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE table_name = 'migration_apply_lock'
        """
    ).fetchone()
    table_exists = int(table_exists_row[0]) if table_exists_row is not None else 0
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
def acquire_apply_lock(conn: duckdb.DuckDBPyConnection, run_id: str) -> Iterator[None]:
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
