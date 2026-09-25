"""Separate research store (ruling RX6).

Research outputs -- monthly PIT panels, later feature versions, evaluation
results and the qualification ledger -- live in their own DuckDB file,
``<data dir>/research/research.duckdb`` by default, never in the governed
warehouse. The warehouse is attached ``READ_ONLY`` under an alias and put on the
connection's ``search_path`` after the research catalog, so the shared warehouse
builders (FQ1 state selection, lineage proofs, owner bridge) read their
unqualified warehouse tables unchanged while every write lands in the research
file. A research table must therefore never reuse a warehouse table name; all of
them are prefixed ``research_``.

The store has its own versioned bootstrap (``research_store_migrations``):
migrations are append-only, applied once each in their own transaction, and
never edited after release (a changed body is a new version).
"""

from __future__ import annotations

import contextlib
import datetime as dt
import os
import re
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import duckdb

from ..connection import resolve_data_dir, resolve_default_db_path

WAREHOUSE_ALIAS = "wh"
RESEARCH_DB_PATH_ENV = "ATX_RESEARCH_DB_PATH"
_ALIAS = re.compile(r"^[a-z][a-z0-9_]{0,30}$")
_CATALOG = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,62}$")
_MEMORY = re.compile(r"^[1-9][0-9]{0,5}(MB|GB)$")


def default_research_db_path() -> Path:
    """``$ATX_RESEARCH_DB_PATH``, else ``<data dir>/research/research.duckdb``."""
    configured = os.environ.get(RESEARCH_DB_PATH_ENV)
    if configured:
        return Path(os.path.expandvars(configured)).expanduser()
    return resolve_data_dir() / "research" / "research.duckdb"


def _create_panel_tables(con: duckdb.DuckDBPyConnection) -> None:
    """Version 1: monthly point-in-time metric panel (task R2a)."""
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_panel_runs (
            run_id VARCHAR PRIMARY KEY,
            status VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            universe_id VARCHAR NOT NULL,
            identity_basis VARCHAR NOT NULL,
            universe_basis VARCHAR NOT NULL,
            fundamental_availability_basis VARCHAR NOT NULL,
            market_availability_basis VARCHAR NOT NULL,
            query_version VARCHAR NOT NULL,
            spec_json VARCHAR NOT NULL,
            spec_sha256 VARCHAR NOT NULL,
            definitions_json VARCHAR NOT NULL,
            definitions_sha256 VARCHAR NOT NULL,
            code_sha256 VARCHAR NOT NULL,
            source_ids_json VARCHAR NOT NULL,
            calendar_sha256 VARCHAR NOT NULL,
            start_month DATE NOT NULL,
            end_month DATE NOT NULL,
            as_of_date DATE NOT NULL,
            run_at TIMESTAMP NOT NULL,
            warehouse_path VARCHAR NOT NULL,
            owner_bridge_json VARCHAR,
            diagnostic_json VARCHAR,
            blockers_json VARCHAR NOT NULL,
            panel_sha256 VARCHAR,
            created_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS research_panel_calendar (
            run_id VARCHAR NOT NULL,
            month_start DATE NOT NULL,
            expected_session DATE NOT NULL,
            last_observed_session DATE,
            formation_date DATE,
            cutoff TIMESTAMP,
            entry_date DATE,
            status VARCHAR NOT NULL,
            visible_members BIGINT,
            eligible_members BIGINT,
            valid_members BIGINT,
            cohort_reasons_json VARCHAR,
            cohort_sha256 VARCHAR,
            PRIMARY KEY (run_id, month_start)
        );
        CREATE TABLE IF NOT EXISTS research_panel_cohort (
            run_id VARCHAR NOT NULL,
            formation_date DATE NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            security_type VARCHAR,
            exchange_code VARCHAR,
            membership_reason VARCHAR,
            membership_cik VARCHAR,
            owner_cik VARCHAR,
            identity_basis VARCHAR NOT NULL,
            owner_link_method VARCHAR,
            owner_link_reason VARCHAR,
            cohort_reason VARCHAR NOT NULL
        );
        CREATE TABLE IF NOT EXISTS research_panel_values (
            run_id VARCHAR NOT NULL,
            formation_date DATE NOT NULL,
            security_id VARCHAR NOT NULL,
            feature_id VARCHAR NOT NULL,
            metric_code VARCHAR NOT NULL,
            metric_window VARCHAR NOT NULL,
            raw_value DOUBLE,
            reason VARCHAR NOT NULL,
            lineage_status VARCHAR,
            available_at TIMESTAMP NOT NULL,
            latest_input_clock TIMESTAMP,
            period_end DATE,
            fiscal_period_start DATE,
            fiscal_period_end DATE,
            value_origin VARCHAR,
            age_days INTEGER,
            max_age_days INTEGER,
            owner_cik VARCHAR,
            derived_value_id VARCHAR,
            derived_owner_security_id VARCHAR,
            lineage_digest VARCHAR,
            identity_basis VARCHAR NOT NULL,
            universe_basis VARCHAR NOT NULL,
            availability_basis VARCHAR NOT NULL
        );
        CREATE TABLE IF NOT EXISTS research_panel_coverage (
            run_id VARCHAR NOT NULL,
            formation_date DATE NOT NULL,
            feature_id VARCHAR NOT NULL,
            metric_code VARCHAR NOT NULL,
            metric_window VARCHAR NOT NULL,
            batch_ordinal INTEGER NOT NULL,
            status VARCHAR NOT NULL,
            valid_members BIGINT NOT NULL,
            selected_states BIGINT NOT NULL,
            values_emitted BIGINT NOT NULL,
            values_valid BIGINT NOT NULL,
            unmatched_owner_states BIGINT NOT NULL,
            reasons_json VARCHAR NOT NULL,
            values_sha256 VARCHAR NOT NULL,
            PRIMARY KEY (run_id, formation_date, feature_id)
        );
        CREATE TABLE IF NOT EXISTS research_panel_proofs (
            run_id VARCHAR NOT NULL,
            derived_value_id VARCHAR NOT NULL,
            derived_owner_security_id VARCHAR NOT NULL,
            metric_code VARCHAR NOT NULL,
            metric_window VARCHAR NOT NULL,
            root_available_at TIMESTAMP NOT NULL,
            selected_input_refs_hash VARCHAR,
            selected_cik VARCHAR,
            status VARCHAR NOT NULL,
            reason VARCHAR NOT NULL,
            leaf_ids_json VARCHAR NOT NULL,
            input_clocks_json VARCHAR NOT NULL,
            fiscal_ends_json VARCHAR NOT NULL,
            oldest_fiscal_end DATE,
            newest_fiscal_end DATE,
            latest_input_clock TIMESTAMP,
            proof_digest VARCHAR,
            proof_cutoff TIMESTAMP NOT NULL
        );
    """)


def _panel_scale_and_survivorship(con: duckdb.DuckDBPyConnection) -> None:
    """Version 2 (R2a fix round 1): slim proofs, eligible-member accounting, primary lines.

    * ``research_lineage_proofs``: one summary row per proved root per run (no
      per-leaf JSON; ``proof_digest`` content-addresses the full proof, which the
      deterministic resolver re-derives from the warehouse). No primary key: an
      ART index over ~1e8 keys would not fit the research memory budget; the
      builder inserts only roots that are not yet proved.
    * The value grid is dense over eligible members, so rows excluded before
      selection (``missing_owner_link``, ``secondary_issuer_line``) carry no
      state clock: ``available_at`` becomes nullable.
    """
    con.execute("""
        CREATE TABLE IF NOT EXISTS research_lineage_proofs (
            run_id VARCHAR NOT NULL,
            derived_value_id VARCHAR NOT NULL,
            derived_owner_security_id VARCHAR NOT NULL,
            metric_code VARCHAR NOT NULL,
            metric_window VARCHAR NOT NULL,
            root_available_at TIMESTAMP NOT NULL,
            root_as_of_date DATE,
            selected_cik VARCHAR,
            status VARCHAR NOT NULL,
            reason VARCHAR NOT NULL,
            proof_digest VARCHAR,
            oldest_fiscal_end DATE,
            newest_fiscal_end DATE,
            latest_input_clock TIMESTAMP,
            leaf_count INTEGER NOT NULL,
            method VARCHAR NOT NULL
        );
        ALTER TABLE research_panel_values ALTER COLUMN available_at DROP NOT NULL;
        ALTER TABLE research_panel_values ADD COLUMN IF NOT EXISTS feature_scope VARCHAR;
        ALTER TABLE research_panel_coverage ADD COLUMN IF NOT EXISTS eligible_members BIGINT;
        ALTER TABLE research_panel_coverage ADD COLUMN IF NOT EXISTS feature_scope VARCHAR;
        ALTER TABLE research_panel_calendar ADD COLUMN IF NOT EXISTS owner_unlinked_members BIGINT;
        ALTER TABLE research_panel_calendar ADD COLUMN IF NOT EXISTS owner_link_attrition DOUBLE;
        ALTER TABLE research_panel_calendar ADD COLUMN IF NOT EXISTS multi_line_issuers BIGINT;
        ALTER TABLE research_panel_cohort ADD COLUMN IF NOT EXISTS eligible BOOLEAN;
        ALTER TABLE research_panel_cohort ADD COLUMN IF NOT EXISTS issuer_lines INTEGER;
        ALTER TABLE research_panel_cohort ADD COLUMN IF NOT EXISTS primary_line BOOLEAN;
        ALTER TABLE research_panel_cohort ADD COLUMN IF NOT EXISTS primary_line_rule VARCHAR;
    """)


#: Append-only bootstrap: (version, name, body). Never edit a released body.
RESEARCH_STORE_MIGRATIONS: tuple[tuple[int, str, Callable[[duckdb.DuckDBPyConnection], None]], ...] = (
    (1, "monthly_pit_panel", _create_panel_tables),
    (2, "panel_scale_and_survivorship", _panel_scale_and_survivorship),
)
RESEARCH_STORE_VERSION = max(version for version, _, _ in RESEARCH_STORE_MIGRATIONS)


@dataclass(frozen=True)
class ResearchStoreStatus:
    path: str
    warehouse_path: str
    warehouse_alias: str
    versions: tuple[int, ...]


class ResearchStore:
    """Research DuckDB with the warehouse attached read-only.

    Duck-types the parts of :class:`atx_db.connection.DuckDBStore` the shared
    builders use (``con``, ``transaction()``). Use as a context manager.
    """

    def __init__(
        self,
        path: Path | str | None = None,
        *,
        warehouse_path: Path | str | None = None,
        memory_limit: str = "256MB",
        threads: int = 1,
        warehouse_alias: str = WAREHOUSE_ALIAS,
    ) -> None:
        self.path = Path(path) if path is not None else default_research_db_path()
        self.warehouse_path = Path(warehouse_path) if warehouse_path is not None else resolve_default_db_path()
        if not _MEMORY.fullmatch(memory_limit):
            raise ValueError("memory_limit must look like '256MB' or '1GB'")
        if isinstance(threads, bool) or not isinstance(threads, int) or not 1 <= threads <= 8:
            raise ValueError("threads must be an integer 1..8")
        if not _ALIAS.fullmatch(warehouse_alias):
            raise ValueError("invalid warehouse alias")
        self.memory_limit = memory_limit
        self.threads = threads
        self.warehouse_alias = warehouse_alias
        self.connection: duckdb.DuckDBPyConnection | None = None
        self.catalog: str | None = None

    # -- lifecycle ---------------------------------------------------------
    def __enter__(self) -> ResearchStore:
        self.open()
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def open(self) -> None:
        if self.connection is not None:
            return
        research = self.path.resolve()
        warehouse = self.warehouse_path.resolve()
        if research == warehouse:
            raise ValueError("the research store must not be the warehouse file")
        if not warehouse.is_file():
            raise FileNotFoundError(f"warehouse not found: {warehouse}")
        # DuckDB names the research catalog after the file stem; it goes on the
        # search_path unquoted, so it must be a plain identifier.
        if not _CATALOG.fullmatch(research.stem) or research.stem.lower() == self.warehouse_alias:
            raise ValueError(f"research store file stem must be a plain identifier: {research.name!r}")
        research.parent.mkdir(parents=True, exist_ok=True)
        temp_dir = (research.parent / f".{research.name}.duckdb_tmp").as_posix()
        con = duckdb.connect(str(research), config={
            "memory_limit": self.memory_limit,
            "threads": str(self.threads),
            "preserve_insertion_order": "false",
            "temp_directory": temp_dir,
        })
        try:
            con.execute("SET TimeZone='UTC'")
            con.execute("PRAGMA disable_progress_bar")
            literal = "'" + warehouse.as_posix().replace("'", "''") + "'"
            con.execute(f"ATTACH {literal} AS {self.warehouse_alias} (READ_ONLY)")
            self.catalog = str(con.execute("SELECT current_database()").fetchone()[0])
            if not _CATALOG.fullmatch(self.catalog):
                raise ValueError(f"unsupported research catalog name: {self.catalog!r}")
            # Research catalog first: writes and research tables resolve there;
            # unqualified warehouse tables resolve to the read-only attachment.
            con.execute(f"SET search_path = '{self.catalog}.main,{self.warehouse_alias}.main'")
            self.connection = con
            self.bootstrap()
        except Exception:
            self.connection = None
            with contextlib.suppress(Exception):
                con.close()
            raise

    def close(self) -> None:
        if self.connection is not None:
            with contextlib.suppress(Exception):
                self.connection.execute("CHECKPOINT")
            self.connection.close()
            self.connection = None

    @property
    def con(self) -> duckdb.DuckDBPyConnection:
        if self.connection is None:
            raise RuntimeError("ResearchStore is not open; use it as a context manager")
        return self.connection

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

    # -- bootstrap ---------------------------------------------------------
    def bootstrap(self) -> tuple[int, ...]:
        """Apply pending research-store migrations, each once, in order."""
        con = self.con
        con.execute(f"""
            CREATE TABLE IF NOT EXISTS "{self.catalog}".main.research_store_migrations (
                version INTEGER PRIMARY KEY,
                name VARCHAR NOT NULL,
                applied_at TIMESTAMP NOT NULL
            )
        """)
        applied = {int(row[0]) for row in con.execute(
            f'SELECT version FROM "{self.catalog}".main.research_store_migrations').fetchall()}
        unknown = applied - {version for version, _, _ in RESEARCH_STORE_MIGRATIONS}
        if unknown:
            raise RuntimeError(f"research store has unknown versions {sorted(unknown)}; code is older")
        for version, name, body in RESEARCH_STORE_MIGRATIONS:
            if version in applied:
                continue
            with self.transaction():
                body(con)
                con.execute(
                    f'INSERT INTO "{self.catalog}".main.research_store_migrations VALUES (?, ?, ?)',
                    [version, name, dt.datetime.now(dt.UTC).replace(tzinfo=None)],
                )
        return self.status().versions

    def status(self) -> ResearchStoreStatus:
        versions = tuple(int(row[0]) for row in self.con.execute(
            f'SELECT version FROM "{self.catalog}".main.research_store_migrations ORDER BY version').fetchall())
        return ResearchStoreStatus(str(self.path), str(self.warehouse_path), self.warehouse_alias, versions)

    def warehouse_has(self, table: str, column: str | None = None) -> bool:
        """True when the attached warehouse (not the research file) has the table/column."""
        if column is None:
            row = self.con.execute("""
                SELECT count(*) FROM duckdb_tables()
                WHERE database_name=? AND schema_name='main' AND table_name=?
            """, [self.warehouse_alias, table]).fetchone()
        else:
            row = self.con.execute("""
                SELECT count(*) FROM duckdb_columns()
                WHERE database_name=? AND schema_name='main' AND table_name=? AND column_name=?
            """, [self.warehouse_alias, table, column]).fetchone()
        return bool(row and row[0])


def open_research_store(path: Path | str | None = None, *, warehouse_path: Path | str | None = None,
                        **kwargs: Any) -> ResearchStore:
    """Open (and bootstrap) a research store; the caller closes it."""
    store = ResearchStore(path, warehouse_path=warehouse_path, **kwargs)
    store.open()
    return store


__all__ = [
    "RESEARCH_DB_PATH_ENV",
    "RESEARCH_STORE_MIGRATIONS",
    "RESEARCH_STORE_VERSION",
    "WAREHOUSE_ALIAS",
    "ResearchStore",
    "ResearchStoreStatus",
    "default_research_db_path",
    "open_research_store",
]
