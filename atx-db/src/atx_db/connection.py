from __future__ import annotations

import contextlib
import os
from collections.abc import Iterator
from pathlib import Path

import duckdb

DB_PATH_ENV = "ATX_DB_PATH"
DATA_DIR_ENV = "ATX_DB_DATA_DIR"


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
) -> duckdb.DuckDBPyConnection:
    """Open a raw DuckDB connection with the warehouse-wide UTC clock contract."""

    con = duckdb.connect(str(path), read_only=read_only)
    con.execute("SET TimeZone='UTC'")
    return con


class DuckDBStore:
    """Persistence boundary used by ATX dataset loaders and read paths."""

    def __init__(self, path: Path | str = DEFAULT_DB_PATH, *, read_only: bool = False) -> None:
        self.path = Path(path)
        self.read_only = read_only
        self.connection: duckdb.DuckDBPyConnection | None = None
        self._initialized = False

    def __enter__(self) -> DuckDBStore:
        if not self.read_only:
            self.path.parent.mkdir(parents=True, exist_ok=True)
        self.connection = open_duckdb_connection(self.path, read_only=self.read_only)
        self._configure_session(self.connection)
        if not self.read_only:
            self.initialize()
        return self

    def _configure_session(self, con: duckdb.DuckDBPyConnection) -> None:
        """Use UTC semantics and an absolute spill directory beside the DB file.

        DuckDB's default temp_directory is derived from the (possibly relative) DB
        path and resolves to an invalid location on Windows (e.g. ``\\.tmp``), so any
        query large enough to spill to disk fails with an IO error. Heavy refreshes
        over the widened universe (millions of rows) hit this; an explicit absolute
        temp directory keeps them robust. Best-effort: never fail open over a setting.
        """
        con.execute("SET TimeZone='UTC'")
        if str(self.path) == ":memory:":
            return
        temp_dir = (self.path.resolve().parent / f".{self.path.name}.duckdb_tmp").as_posix()
        with contextlib.suppress(Exception):
            con.execute("SET temp_directory = ?", [temp_dir])

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self.connection is not None:
            self.connection.close()
            self.connection = None

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
        con = self.con
        con.execute("BEGIN TRANSACTION")
        try:
            yield con
        except Exception:
            con.execute("ROLLBACK")
            raise
        else:
            con.execute("COMMIT")


@contextlib.contextmanager
def connect(path: Path | str = DEFAULT_DB_PATH, *, read_only: bool = False) -> Iterator[DuckDBStore]:
    with DuckDBStore(path, read_only=read_only) as store:
        yield store
