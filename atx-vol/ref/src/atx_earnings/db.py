from __future__ import annotations

import hashlib
import json
import sqlite3
from collections.abc import Iterable, Iterator, Sequence
from contextlib import contextmanager
from datetime import date, datetime
from pathlib import Path
from typing import Any

from .models import EarningsEvent, UniverseMembership, iso_datetime, normalize_symbol

SCHEMA_VERSION = 2


class EarningsDatabase:
    """SQLite store whose raw source history is append-only.

    A source snapshot is the atomic PIT unit.  A later empty snapshot is meaningful:
    it records that an event previously present on that calendar date disappeared.
    Failed fetches are retained for operations telemetry but never enter PIT queries.
    """

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.connection = sqlite3.connect(self.path)
        self.connection.row_factory = sqlite3.Row
        self.connection.execute("PRAGMA foreign_keys = ON")
        self.connection.execute("PRAGMA journal_mode = WAL")
        self.connection.execute("PRAGMA synchronous = NORMAL")
        self.connection.execute("PRAGMA busy_timeout = 30000")
        self.initialize()

    def close(self) -> None:
        self.connection.close()

    def __enter__(self) -> EarningsDatabase:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    @contextmanager
    def transaction(self) -> Iterator[sqlite3.Connection]:
        self.connection.execute("BEGIN IMMEDIATE")
        try:
            yield self.connection
        except Exception:
            self.connection.rollback()
            raise
        else:
            self.connection.commit()

    def initialize(self) -> None:
        self.connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS schema_metadata (
                key TEXT PRIMARY KEY,
                value TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS source_snapshots (
                snapshot_id INTEGER PRIMARY KEY,
                source TEXT NOT NULL,
                scope TEXT NOT NULL,
                scope_key TEXT NOT NULL,
                requested_at TEXT NOT NULL,
                observed_at TEXT NOT NULL,
                coverage_start TEXT,
                coverage_end TEXT,
                status TEXT NOT NULL CHECK (status IN ('COMPLETE', 'FAILED')),
                row_count INTEGER NOT NULL DEFAULT 0,
                payload_sha256 TEXT,
                raw_payload TEXT,
                error TEXT,
                UNIQUE(source, scope, scope_key, observed_at)
            );
            CREATE INDEX IF NOT EXISTS idx_snapshot_pit
                ON source_snapshots(scope, scope_key, source, observed_at, status);

            CREATE TABLE IF NOT EXISTS earnings_observations (
                snapshot_id INTEGER NOT NULL REFERENCES source_snapshots(snapshot_id),
                source_record_key TEXT NOT NULL,
                symbol TEXT NOT NULL,
                company_name TEXT,
                event_date TEXT NOT NULL,
                session TEXT NOT NULL,
                date_status TEXT NOT NULL,
                fiscal_quarter_ending TEXT,
                eps_estimate REAL,
                reported_eps REAL,
                surprise_percent REAL,
                estimator_count INTEGER,
                eps_prior_year REAL,
                prior_year_report_date TEXT,
                currency TEXT,
                raw_json TEXT NOT NULL,
                PRIMARY KEY(snapshot_id, source_record_key)
            );
            CREATE INDEX IF NOT EXISTS idx_earnings_symbol_date
                ON earnings_observations(symbol, event_date, snapshot_id);

            CREATE TABLE IF NOT EXISTS universe_memberships (
                snapshot_id INTEGER NOT NULL REFERENCES source_snapshots(snapshot_id),
                symbol TEXT NOT NULL,
                valid_from TEXT NOT NULL,
                valid_to TEXT,
                company_name TEXT,
                sector TEXT,
                sub_industry TEXT,
                cik TEXT,
                PRIMARY KEY(snapshot_id, symbol, valid_from)
            );
            CREATE INDEX IF NOT EXISTS idx_universe_validity
                ON universe_memberships(snapshot_id, valid_from, valid_to, symbol);

            CREATE TABLE IF NOT EXISTS reference_builds (
                build_id INTEGER PRIMARY KEY,
                as_of_start TEXT NOT NULL,
                as_of_end TEXT NOT NULL,
                known_at TEXT NOT NULL,
                horizon_days INTEGER NOT NULL,
                max_events INTEGER NOT NULL,
                created_at TEXT NOT NULL,
                universe_snapshot_id INTEGER NOT NULL REFERENCES source_snapshots(snapshot_id),
                source_snapshot_ids TEXT NOT NULL
            );

            CREATE TABLE IF NOT EXISTS reference_rows (
                build_id INTEGER NOT NULL REFERENCES reference_builds(build_id),
                as_of_date TEXT NOT NULL,
                symbol TEXT NOT NULL,
                company_name TEXT,
                sector TEXT,
                event_1_date TEXT, event_1_session TEXT, event_1_status TEXT,
                event_1_fiscal_quarter_ending TEXT, event_1_eps_estimate REAL,
                event_1_reported_eps REAL, event_1_surprise_percent REAL,
                event_1_estimator_count INTEGER, event_1_source TEXT,
                event_1_observed_at TEXT,
                event_2_date TEXT, event_2_session TEXT, event_2_status TEXT,
                event_2_fiscal_quarter_ending TEXT, event_2_eps_estimate REAL,
                event_2_reported_eps REAL, event_2_surprise_percent REAL,
                event_2_estimator_count INTEGER, event_2_source TEXT,
                event_2_observed_at TEXT,
                event_3_date TEXT, event_3_session TEXT, event_3_status TEXT,
                event_3_fiscal_quarter_ending TEXT, event_3_eps_estimate REAL,
                event_3_reported_eps REAL, event_3_surprise_percent REAL,
                event_3_estimator_count INTEGER, event_3_source TEXT,
                event_3_observed_at TEXT,
                event_4_date TEXT, event_4_session TEXT, event_4_status TEXT,
                event_4_fiscal_quarter_ending TEXT, event_4_eps_estimate REAL,
                event_4_reported_eps REAL, event_4_surprise_percent REAL,
                event_4_estimator_count INTEGER, event_4_source TEXT,
                event_4_observed_at TEXT,
                PRIMARY KEY(build_id, as_of_date, symbol)
            );
            CREATE INDEX IF NOT EXISTS idx_reference_pair
                ON reference_rows(as_of_date, symbol, build_id);

            CREATE TABLE IF NOT EXISTS job_runs (
                job_run_id INTEGER PRIMARY KEY,
                job_type TEXT NOT NULL,
                started_at TEXT NOT NULL,
                finished_at TEXT,
                status TEXT NOT NULL CHECK(status IN ('RUNNING', 'SUCCESS', 'PARTIAL', 'FAILED')),
                details_json TEXT NOT NULL DEFAULT '{}',
                error TEXT
            );

            DROP VIEW IF EXISTS earnings_reference_latest;
            CREATE VIEW earnings_reference_latest AS
            WITH ranked AS (
                SELECT rr.*, rb.known_at, rb.horizon_days,
                       ROW_NUMBER() OVER (
                         PARTITION BY rr.as_of_date, rr.symbol
                         ORDER BY rb.known_at DESC, rr.build_id DESC
                       ) AS _rank
                FROM reference_rows rr
                JOIN reference_builds rb USING(build_id)
            )
            SELECT * FROM ranked WHERE _rank = 1;
            """
        )
        # Additive migration for databases created by schema v1.
        _ensure_column(self.connection, "earnings_observations", "reported_eps", "REAL")
        _ensure_column(self.connection, "earnings_observations", "surprise_percent", "REAL")
        for slot in range(1, 5):
            _ensure_column(self.connection, "reference_rows", f"event_{slot}_reported_eps", "REAL")
            _ensure_column(
                self.connection, "reference_rows", f"event_{slot}_surprise_percent", "REAL"
            )
        self.connection.execute(
            "INSERT OR REPLACE INTO schema_metadata(key, value) VALUES('schema_version', ?)",
            (str(SCHEMA_VERSION),),
        )
        self.connection.commit()

    def add_earnings_snapshot(
        self,
        *,
        source: str,
        calendar_date: date,
        events: Sequence[EarningsEvent],
        requested_at: datetime | str,
        observed_at: datetime | str,
        raw_payload: str | bytes | None = None,
    ) -> int:
        for event in events:
            if event.event_date != calendar_date:
                raise ValueError(
                    f"event {event.source_record_key} has {event.event_date}, "
                    f"outside atomic snapshot {calendar_date}"
                )
        payload_text = _payload_text(raw_payload)
        with self.transaction() as con:
            cur = con.execute(
                """INSERT INTO source_snapshots(
                       source, scope, scope_key, requested_at, observed_at,
                       coverage_start, coverage_end, status, row_count,
                       payload_sha256, raw_payload)
                   VALUES (?, 'earnings_date', ?, ?, ?, ?, ?, 'COMPLETE', ?, ?, ?)""",
                (
                    source,
                    calendar_date.isoformat(),
                    iso_datetime(requested_at),
                    iso_datetime(observed_at),
                    calendar_date.isoformat(),
                    calendar_date.isoformat(),
                    len(events),
                    _sha256(payload_text),
                    payload_text,
                ),
            )
            snapshot_id = _lastrowid(cur)
            con.executemany(
                """INSERT INTO earnings_observations(
                       snapshot_id, source_record_key, symbol, company_name, event_date,
                       session, date_status, fiscal_quarter_ending, eps_estimate,
                       reported_eps, surprise_percent, estimator_count, eps_prior_year,
                       prior_year_report_date, currency, raw_json)
                   VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                [
                    (
                        snapshot_id,
                        event.source_record_key,
                        event.symbol,
                        event.company_name,
                        event.event_date.isoformat(),
                        event.session.value,
                        event.date_status.value,
                        _date_text(event.fiscal_quarter_ending),
                        event.eps_estimate,
                        event.reported_eps,
                        event.surprise_percent,
                        event.estimator_count,
                        event.eps_prior_year,
                        _date_text(event.prior_year_report_date),
                        event.currency,
                        json.dumps(event.raw, sort_keys=True, separators=(",", ":")),
                    )
                    for event in events
                ],
            )
        return snapshot_id

    def add_failed_snapshot(
        self,
        *,
        source: str,
        scope: str,
        scope_key: str,
        requested_at: datetime | str,
        observed_at: datetime | str,
        error: str,
    ) -> int:
        with self.transaction() as con:
            cur = con.execute(
                """INSERT INTO source_snapshots(
                       source, scope, scope_key, requested_at, observed_at, status, error)
                   VALUES (?, ?, ?, ?, ?, 'FAILED', ?)""",
                (
                    source,
                    scope,
                    scope_key,
                    iso_datetime(requested_at),
                    iso_datetime(observed_at),
                    error[:4000],
                ),
            )
        return _lastrowid(cur)

    def add_universe_snapshot(
        self,
        *,
        source: str,
        universe: str,
        memberships: Sequence[UniverseMembership],
        requested_at: datetime | str,
        observed_at: datetime | str,
        coverage_start: date,
        raw_payload: str | bytes | None = None,
    ) -> int:
        payload_text = _payload_text(raw_payload)
        with self.transaction() as con:
            cur = con.execute(
                """INSERT INTO source_snapshots(
                       source, scope, scope_key, requested_at, observed_at,
                       coverage_start, status, row_count, payload_sha256, raw_payload)
                   VALUES (?, 'universe', ?, ?, ?, ?, 'COMPLETE', ?, ?, ?)""",
                (
                    source,
                    universe,
                    iso_datetime(requested_at),
                    iso_datetime(observed_at),
                    coverage_start.isoformat(),
                    len(memberships),
                    _sha256(payload_text),
                    payload_text,
                ),
            )
            snapshot_id = _lastrowid(cur)
            con.executemany(
                "INSERT INTO universe_memberships VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                [
                    (
                        snapshot_id,
                        m.symbol,
                        m.valid_from.isoformat(),
                        _date_text(m.valid_to),
                        m.company_name,
                        m.sector,
                        m.sub_industry,
                        m.cik,
                    )
                    for m in memberships
                ],
            )
        return snapshot_id

    def events_between(
        self,
        start: date,
        end: date,
        *,
        known_at: datetime | str,
        sources: Sequence[str] | None = None,
        symbols: Sequence[str] | None = None,
    ) -> list[sqlite3.Row]:
        params: list[Any] = [start.isoformat(), end.isoformat(), iso_datetime(known_at)]
        source_clause = ""
        if sources:
            source_clause = f" AND source IN ({','.join('?' for _ in sources)})"
            params.extend(sources)
        symbol_clause = ""
        if symbols:
            symbol_clause = f" AND eo.symbol IN ({','.join('?' for _ in symbols)})"
            params.extend(normalize_symbol(s) for s in symbols)
        return list(
            self.connection.execute(
                f"""
                WITH candidate AS (
                    SELECT *, ROW_NUMBER() OVER (
                        PARTITION BY source, scope_key
                        ORDER BY observed_at DESC, snapshot_id DESC
                    ) AS rank
                    FROM source_snapshots
                    WHERE scope = 'earnings_date'
                      AND scope_key BETWEEN ? AND ?
                      AND observed_at <= ?
                      AND status = 'COMPLETE'
                      {source_clause}
                )
                SELECT eo.*, c.source, c.observed_at
                FROM candidate c
                JOIN earnings_observations eo USING(snapshot_id)
                WHERE c.rank = 1 {symbol_clause}
                ORDER BY eo.symbol, eo.event_date, c.source
                """,
                params,
            )
        )

    def latest_universe_snapshot(
        self, *, universe: str, known_at: datetime | str, source: str | None = None
    ) -> sqlite3.Row | None:
        sql = """SELECT * FROM source_snapshots
                 WHERE scope='universe' AND scope_key=? AND status='COMPLETE'
                   AND observed_at <= ?"""
        params: list[Any] = [universe, iso_datetime(known_at)]
        if source:
            sql += " AND source=?"
            params.append(source)
        sql += " ORDER BY observed_at DESC, snapshot_id DESC LIMIT 1"
        return self.connection.execute(sql, params).fetchone()

    def universe_intervals(
        self, *, universe: str, known_at: datetime | str, source: str | None = None
    ) -> tuple[int, list[sqlite3.Row]]:
        snapshot = self.latest_universe_snapshot(
            universe=universe, known_at=known_at, source=source
        )
        if snapshot is None:
            raise LookupError(
                f"no {universe!r} universe snapshot known at {iso_datetime(known_at)}"
            )
        snapshot_id = int(snapshot["snapshot_id"])
        rows = list(
            self.connection.execute(
                """SELECT * FROM universe_memberships
                   WHERE snapshot_id=? ORDER BY symbol, valid_from""",
                (snapshot_id,),
            )
        )
        return snapshot_id, rows

    def universe_on(
        self, as_of_date: date, *, universe: str, known_at: datetime | str
    ) -> list[sqlite3.Row]:
        snapshot = self.latest_universe_snapshot(universe=universe, known_at=known_at)
        if snapshot is None:
            return []
        day = as_of_date.isoformat()
        return list(
            self.connection.execute(
                """SELECT * FROM universe_memberships
                   WHERE snapshot_id=? AND valid_from <= ?
                     AND (valid_to IS NULL OR valid_to > ?)
                   ORDER BY symbol""",
                (snapshot["snapshot_id"], day, day),
            )
        )

    def begin_job(self, job_type: str, *, started_at: datetime | str) -> int:
        cur = self.connection.execute(
            "INSERT INTO job_runs(job_type, started_at, status) VALUES(?, ?, 'RUNNING')",
            (job_type, iso_datetime(started_at)),
        )
        self.connection.commit()
        return _lastrowid(cur)

    def finish_job(
        self,
        job_run_id: int,
        *,
        status: str,
        finished_at: datetime | str,
        details: dict[str, Any] | None = None,
        error: str | None = None,
    ) -> None:
        self.connection.execute(
            """UPDATE job_runs SET status=?, finished_at=?, details_json=?, error=?
               WHERE job_run_id=?""",
            (
                status,
                iso_datetime(finished_at),
                json.dumps(details or {}, sort_keys=True),
                error,
                job_run_id,
            ),
        )
        self.connection.commit()

    def iter_latest_reference(
        self, *, as_of_date: date | None = None, symbol: str | None = None
    ) -> Iterable[sqlite3.Row]:
        clauses: list[str] = []
        params: list[str] = []
        if as_of_date:
            clauses.append("as_of_date=?")
            params.append(as_of_date.isoformat())
        if symbol:
            clauses.append("symbol=?")
            params.append(normalize_symbol(symbol))
        where = " WHERE " + " AND ".join(clauses) if clauses else ""
        return self.connection.execute(
            "SELECT * FROM earnings_reference_latest" + where + " ORDER BY as_of_date, symbol",
            params,
        )

    def iter_reference_as_known_at(
        self,
        known_at: datetime | str,
        *,
        as_of_date: date | None = None,
        symbol: str | None = None,
    ) -> Iterable[sqlite3.Row]:
        """Return wide rows without using knowledge acquired after ``known_at``."""
        clauses = ["rb.known_at <= ?"]
        params: list[str] = [iso_datetime(known_at)]
        if as_of_date:
            clauses.append("rr.as_of_date=?")
            params.append(as_of_date.isoformat())
        if symbol:
            clauses.append("rr.symbol=?")
            params.append(normalize_symbol(symbol))
        where = " AND ".join(clauses)
        return self.connection.execute(
            f"""
            WITH ranked AS (
                SELECT rr.*, rb.known_at, rb.horizon_days,
                       ROW_NUMBER() OVER (
                         PARTITION BY rr.as_of_date, rr.symbol
                         ORDER BY rb.known_at DESC, rr.build_id DESC
                       ) AS _rank
                FROM reference_rows rr
                JOIN reference_builds rb USING(build_id)
                WHERE {where}
            )
            SELECT * FROM ranked WHERE _rank=1 ORDER BY as_of_date, symbol
            """,
            params,
        )


def _date_text(value: date | None) -> str | None:
    return value.isoformat() if value else None


def _payload_text(payload: str | bytes | None) -> str | None:
    if isinstance(payload, bytes):
        return payload.decode("utf-8", errors="replace")
    return payload


def _sha256(payload: str | None) -> str | None:
    return hashlib.sha256(payload.encode()).hexdigest() if payload is not None else None


def _lastrowid(cursor: sqlite3.Cursor) -> int:
    if cursor.lastrowid is None:
        raise RuntimeError("SQLite did not return a row id")
    return cursor.lastrowid


def _ensure_column(
    connection: sqlite3.Connection, table: str, column: str, column_type: str
) -> None:
    existing = {row[1] for row in connection.execute(f"PRAGMA table_info({table})")}
    if column not in existing:
        connection.execute(f"ALTER TABLE {table} ADD COLUMN {column} {column_type}")
