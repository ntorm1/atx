from __future__ import annotations

import contextlib
import hashlib
import itertools
import os
import time
from collections.abc import Iterator
from pathlib import Path

import duckdb

DB_PATH_ENV = "ATX_DB_PATH"
DATA_DIR_ENV = "ATX_DB_DATA_DIR"

#: Shared DuckDB spill root (index §4 M7). DuckDB is never handed this directory itself: every
#: instance spills into a private directory below it (:func:`private_temp_directory`, ruling C-56).
DEFAULT_TEMP_DIR = Path("C:/atx/atx-db/data/tmp/duckdb")
DEFAULT_MEMORY_LIMIT = "384MB"
DEFAULT_THREADS = 1
DEFAULT_MAX_TEMP_DIRECTORY_SIZE = "40GB"

#: The config type DuckDB's ``connect`` accepts (dict values are invariant for mypy).
DuckDBConfig = dict[str, str | bool | int | float | list[str]]


def bounded_config(memory_limit: str = "384MB", threads: int = 1, *,
                   temp_directory: Path = DEFAULT_TEMP_DIR,
                   max_temp_directory_size: str = "40GB") -> DuckDBConfig:
    """Config applied at duckdb.connect(), so WAL replay and initialize() run inside the budget.

    ``temp_directory`` must belong to one DuckDB instance: DuckDB names its spill files the same
    in every instance and, at close, deletes the ``duckdb_temp_*`` files of a directory it did not
    create (ruling C-56). Pass :func:`private_temp_directory` -- :class:`DuckDBStore` and
    :func:`open_duckdb_connection` always do; the shared default is only safe for a sole instance.
    """
    return {"memory_limit": memory_limit, "threads": threads, "preserve_insertion_order": False,
            "temp_directory": str(temp_directory), "max_temp_directory_size": max_temp_directory_size}


def spill_root() -> Path:
    """:data:`DEFAULT_TEMP_DIR`; off Windows (where it is not absolute) ``<data dir>/tmp/duckdb``."""
    if DEFAULT_TEMP_DIR.is_absolute():
        return DEFAULT_TEMP_DIR
    return resolve_data_dir() / "tmp" / "duckdb"


_IN_MEMORY_STORES = itertools.count()


def private_temp_directory(path: Path | str) -> Path:
    """This process's spill directory for the database at ``path`` (not created here).

    DuckDB creates the directory on its first spill and removes it at close, but it does not
    create missing parents, so :func:`_ensure_spill_root` runs before every connect. One DuckDB
    instance serves every in-process connection to a file, and DuckDB refuses a second in-process
    connection whose connect config differs, so the name is stable per (process, database file):
    ``conn-<pid>-<digest of the absolute path>``. Each ``:memory:`` database is its own instance
    and gets a unique name. Only a killed process leaves its (pid-named) directory behind.
    """
    text = str(path)
    if text in ("", ":memory:"):
        key = f"mem-{next(_IN_MEMORY_STORES)}-{time.monotonic_ns()}"
    else:
        absolute = os.path.normcase(os.path.abspath(text))
        key = hashlib.sha256(absolute.encode("utf-8")).hexdigest()[:16]
    return spill_root() / f"conn-{os.getpid()}-{key}"


def _ensure_spill_root() -> None:
    # Best-effort: a missing root only fails a query that actually spills, and loudly.
    with contextlib.suppress(OSError):
        spill_root().mkdir(parents=True, exist_ok=True)


def resolve_data_dir() -> Path:
    """Return the writable directory for warehouse runtime state."""
    configured_data_dir = os.environ.get(DATA_DIR_ENV)
    if configured_data_dir:
        return Path(os.path.expandvars(configured_data_dir)).expanduser()

    configured_path = os.environ.get(DB_PATH_ENV)
    if configured_path:
        return Path(os.path.expandvars(configured_path)).expanduser().parent

    # Source checkouts keep operator-managed state in <project>/data.
    project_root = Path(__file__).resolve().parents[2]
    if (project_root / "pyproject.toml").is_file():
        return project_root / "data"

    if os.name == "nt":
        data_root = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    else:
        data_root = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share"))
    return data_root / "atx-db"


def resolve_default_db_path() -> Path:
    """Return the configured warehouse path without placing data in site-packages."""
    configured_path = os.environ.get(DB_PATH_ENV)
    if configured_path:
        return Path(os.path.expandvars(configured_path)).expanduser()
    return resolve_data_dir() / "warehouse.duckdb"


DEFAULT_DB_PATH = resolve_default_db_path()


def open_duckdb_connection(
    path: Path | str,
    *,
    read_only: bool = False,
    memory_limit: str = DEFAULT_MEMORY_LIMIT,
    threads: int = DEFAULT_THREADS,
) -> duckdb.DuckDBPyConnection:
    """Open a raw DuckDB connection, bounded at connect, with the warehouse-wide UTC clock contract.

    Uses the same connect config as a default :class:`DuckDBStore` on the same file, so the two
    can share DuckDB's in-process instance.
    """
    _ensure_spill_root()
    config = bounded_config(memory_limit, threads, temp_directory=private_temp_directory(path))
    con = duckdb.connect(str(path), read_only=read_only, config=config)
    con.execute("SET TimeZone='UTC'")
    return con


class DuckDBStore:
    """Persistence boundary used by ATX dataset loaders and read paths.

    Every connection it opens carries :func:`bounded_config` in the connect config, so WAL
    replay, ``initialize()`` and migrations run inside ``memory_limit``/``threads`` (index §4 M7).
    """

    def __init__(self, path: Path | str = DEFAULT_DB_PATH, *, memory_limit: str = DEFAULT_MEMORY_LIMIT,
                 threads: int = DEFAULT_THREADS, read_only: bool = False) -> None:
        if isinstance(threads, bool) or not isinstance(threads, int) or threads < 1:
            raise ValueError("threads must be a positive integer")
        self.path = Path(path)
        self.read_only = read_only
        self.memory_limit = memory_limit
        self.threads = threads
        self.max_temp_directory_size = DEFAULT_MAX_TEMP_DIRECTORY_SIZE
        # Fixed for the store's lifetime: its connections are sequential (close/reopen, failure
        # recovery), and a stable name keeps the connect config identical across them.
        self.temp_directory = private_temp_directory(path)
        self.connection: duckdb.DuckDBPyConnection | None = None
        self._initialized = False
        # Recorded by cli._configure_analytical_session() (or any caller) so a
        # close()/reopen() cycle -- e.g. around the activation ladder's
        # reconciliation stage, which yields the file to a sharded subprocess --
        # can restore the same memory_limit/threads tuning instead of silently
        # reverting to the store's default budget on the new connection.
        self.analytical_memory_limit: str | None = None
        self.analytical_threads: int | None = None

    def _connect(self) -> duckdb.DuckDBPyConnection:
        """Connect with the budget in the connect config (DuckDB replays a WAL inside ``connect``).

        A recorded analytical budget wins over the store's default one.
        """
        memory_limit, threads = self.memory_limit, self.threads
        if self.analytical_memory_limit is not None and self.analytical_threads is not None:
            memory_limit, threads = self.analytical_memory_limit, self.analytical_threads
        _ensure_spill_root()
        config = bounded_config(memory_limit, threads, temp_directory=self.temp_directory,
                                max_temp_directory_size=self.max_temp_directory_size)
        return duckdb.connect(str(self.path), read_only=self.read_only, config=config)

    def __enter__(self) -> DuckDBStore:
        if not self.read_only:
            self.path.parent.mkdir(parents=True, exist_ok=True)
        self.connection = self._connect()
        self._configure_session(self.connection)
        if not self.read_only:
            self.initialize()
        return self

    def _configure_session(self, con: duckdb.DuckDBPyConnection) -> None:
        """Use UTC semantics and this store's private absolute spill directory.

        Connections opened by the store already carry the directory in their connect config;
        this also covers callers that open a connection themselves and hand it to the store.
        DuckDB's default temp_directory is derived from the (possibly relative) DB path and
        resolves to an invalid location on Windows (e.g. ``\\.tmp``), so a spilling query
        would fail with an IO error. Best-effort: never fail open over a setting (DuckDB
        refuses to switch a directory it has already spilled into).
        """
        con.execute("SET TimeZone='UTC'")
        _ensure_spill_root()
        with contextlib.suppress(Exception):
            con.execute("SET temp_directory = ?", [str(self.temp_directory)])

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self.connection is not None:
            self.connection.close()
            self.connection = None

    def close(self) -> None:
        """Release the connection without discarding the store's configuration.

        ``self.path``/``self.read_only`` and any recorded analytical session
        settings (``analytical_memory_limit``/``analytical_threads``) survive
        this call, so ``reopen()`` can bring a replacement connection back to
        the same tuning.
        """
        if self.connection is not None:
            if not self.read_only:
                self.connection.execute("CHECKPOINT")
            self.connection.close()
            self.connection = None

    def reopen(self) -> None:
        """Reacquire a configured connection after ``close()``.

        Replays the base session setup (UTC timezone, temp directory) and the budget at
        connect: the store's default one, or -- if ``cli._configure_analytical_session``
        was applied before the close -- the recorded analytical ``memory_limit``/``threads``
        (with ``preserve_insertion_order`` off). A fresh DuckDB connection does not inherit
        session-level ``SET`` state from the one it replaces, and opening may recover a WAL
        before any SET statement could run.
        """
        if self.connection is None:
            self.connection = self._connect()
            self._configure_session(self.connection)
            if self.analytical_memory_limit is not None and self.analytical_threads is not None:
                self.connection.execute("PRAGMA disable_progress_bar")

    def recover_failed_connection(self) -> None:
        """Replace an unusable persistent connection for failure ledgering only.

        Do not CHECKPOINT an invalidated connection, initialize the schema, or
        retry the failed workload. Supply the existing analytical budget before
        opening the database so WAL recovery also runs within that budget.
        """
        if str(self.path) == ":memory:":
            raise RuntimeError("cannot recover an in-memory warehouse without losing its ledger")
        if self.analytical_memory_limit is None or self.analytical_threads is None:
            raise RuntimeError("cannot recover without a recorded analytical memory/thread budget")
        previous = self.connection
        self.connection = None
        if previous is not None:
            previous.close()  # Raw close: close() above issues SQL that may itself fail.
        con = self._connect()
        try:
            self._configure_session(con)
            con.execute("PRAGMA disable_progress_bar")
        except Exception:
            with contextlib.suppress(Exception):
                con.close()
            raise
        self.connection = con

    @property
    def con(self) -> duckdb.DuckDBPyConnection:
        if self.connection is None:
            raise RuntimeError("DuckDBStore is not open; use it as a context manager")
        return self.connection

    def _schema_is_current(self, con: duckdb.DuckDBPyConnection) -> bool:
        """True if this warehouse is already built to the head schema version.

        Bootstrapping the full schema (CREATE IF NOT EXISTS for ~60 tables/indexes/
        views + catalog seed) costs ~1-2s even when everything already exists. Many
        dataset loaders call initialize() on every run, so without this guard a single
        pipeline pays that cost dozens of times. If schema_migrations already records
        the highest known migration version, the schema is current and we can skip the
        whole rebuild. Any schema change must bump a migration version (S0 discipline),
        so a stale warehouse falls through to the full idempotent path below.
        """
        from .migrations import MIGRATIONS, verify_migration_checksums

        if not MIGRATIONS:
            return False
        target = max(m.version for m in MIGRATIONS)
        try:
            row = con.execute(
                "SELECT max(CAST(version AS INTEGER)) FROM schema_migrations "
                "WHERE version ~ '^[0-9]+$'"
            ).fetchone()
        except Exception:
            return False  # schema_migrations absent -> fresh/legacy db, full init needed
        if row is not None and row[0] is not None and int(row[0]) >= target:
            verify_migration_checksums(con)
            return True
        return False

    def initialize(self) -> None:
        if self._initialized:
            return
        con = self.con
        if self._schema_is_current(con):
            self._initialized = True
            return
        # PF2-S1 S1-2: these three tables are created directly here (not via schema.py or
        # a migration), so they must all exist BEFORE apply_pending_migrations() runs --
        # migration 0097 reconciles the live schema into a complete manifest
        # (schema_contract.py::build_contract_manifest), and a table created only AFTER
        # migrations apply would be invisible to that reconciliation. dataset_watermarks/
        # security_identifiers used to be created after apply_pending_migrations(); moved
        # up here for that reason. Neither depends on anything ensure_quant_schema or the
        # migrations create.
        con.execute(
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
        con.execute(
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
        con.execute(
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
        from .schema import ensure_quant_schema

        ensure_quant_schema(self)

        # Apply any pending versioned migrations (idempotent; no-op if up to date).
        from .migrations import apply_pending_migrations

        apply_pending_migrations(self.con)
        self._initialized = True

    @contextlib.contextmanager
    def transaction(self) -> Iterator[duckdb.DuckDBPyConnection]:
        """Commit on success; roll back on any exit by exception.

        Rolls back on ``BaseException`` too: ``sec_http.SecBlockedError`` derives from it so
        per-item ``except Exception`` handlers cannot absorb a host-wide SEC block. The
        original exception stays primary -- a failed ROLLBACK (e.g. on an invalidated
        connection) is attached as a note instead of replacing it.
        """
        con = self.con
        con.execute("BEGIN TRANSACTION")
        try:
            yield con
        except BaseException as exc:
            try:
                con.execute("ROLLBACK")
            except Exception as rollback_error:
                exc.add_note(f"ROLLBACK also failed: {rollback_error}")
            raise
        else:
            con.execute("COMMIT")


@contextlib.contextmanager
def connect(path: Path | str = DEFAULT_DB_PATH, *, read_only: bool = False,
            memory_limit: str = DEFAULT_MEMORY_LIMIT, threads: int = DEFAULT_THREADS) -> Iterator[DuckDBStore]:
    with DuckDBStore(path, memory_limit=memory_limit, threads=threads, read_only=read_only) as store:
        yield store
