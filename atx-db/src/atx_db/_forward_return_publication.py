"""Persistent, bounded publication of the complete survivorship return panel.

The caller owns the sole writer. Input snapshots and the final unindexed sort
are spillable CTAS operations; expanded result inserts and indexed shadow inserts
have explicit row caps. Never wrap this workflow in an outer transaction.
"""

from __future__ import annotations

import datetime as dt
import logging
from collections.abc import Sequence
from uuid import uuid4

from ._bulk_publication import publish_validated_shadow
from .connection import DuckDBStore

_LOG = logging.getLogger(__name__)
_LIVE = "forward_returns_survivorship_safe"
_FORMATION_BATCH_ROWS = 100_000
_SHADOW_BATCH_ROWS = 100_000
_CHECKPOINT_ROWS = 400_000
# This identifies the bounded SQL calculation below, including its selected bar
# basis, terminal stitching, and observed trading-calendar endpoint rule. The
# formation filter only selects WHICH anchor rows are computed; every published
# row is the identical per-row calculation, so the version is shared.
CALCULATION_VERSION = "forward_return_publication_v1"
# every_session: every positive selected bar is an anchor (the daily panel).
# month_end_next_session: only the first observed session after the last observed
# session of each closed calendar month (entry after a month-end decision).
FORMATION_RULES = ("every_session", "month_end_next_session")


def _count(store: DuckDBStore, table: str) -> int:
    row = store.con.execute(f"SELECT count(*) FROM {table}").fetchone()
    assert row is not None
    return int(row[0])


class _Build:
    """Own exactly this attempt's relations, never names left by another attempt."""

    def __init__(self, store: DuckDBStore) -> None:
        self.store = store
        self.prefix = f"_ss_forward_{uuid4().hex}"
        self.owned: list[str] = []
        self.pending_rows = 0
        self.recyclable = (
            not str(store.path).startswith(":memory:")
            and store.path.is_file()
            and store.analytical_memory_limit is not None
            and store.analytical_threads is not None
        )
        self.preserve_order = True
        if self.recyclable:
            row = store.con.execute("""
                SELECT EXISTS (
                    SELECT 1 FROM duckdb_tables() WHERE temporary AND NOT internal
                ) OR EXISTS (
                    SELECT 1 FROM duckdb_views() WHERE temporary AND NOT internal
                ) OR EXISTS (
                    SELECT 1 FROM duckdb_functions()
                    WHERE NOT internal AND database_name <> current_database()
                ) OR EXISTS (
                    SELECT 1 FROM duckdb_databases()
                    WHERE NOT internal AND database_name <> current_database()
                )
            """).fetchone()
            if row is None or row[0]:
                raise RuntimeError(
                    "forward-return publication cannot recycle a connection with "
                    "caller-owned temporary objects, functions, or attached databases"
                )
            settings = store.con.execute("SELECT current_setting('preserve_insertion_order')").fetchone()
            assert settings is not None
            self.preserve_order = bool(settings[0])

    def name(self, suffix: str) -> str:
        return f"{self.prefix}_{suffix}"

    def create(self, suffix: str, sql: str, params: Sequence[object] = ()) -> str:
        name = self.name(suffix)
        self.store.con.execute(sql, list(params))
        self.owned.append(name)  # Only a successful CREATE gives us ownership.
        return name

    def drop(self, table: str) -> None:
        self.store.con.execute(f"DROP TABLE {table}")
        self.owned.remove(table)

    def checkpoint(self, rows: int) -> None:
        self.pending_rows += rows
        if self.recyclable and self.pending_rows >= _CHECKPOINT_ROWS:
            self.store.close()  # CHECKPOINT after committed work, before closing.
            self.store.reopen()  # Replays the caller's recorded memory/thread limits.
            self.store.con.execute("SET preserve_insertion_order = ?", [self.preserve_order])
            self.pending_rows = 0

    def cleanup(self) -> None:
        for table in reversed(self.owned):
            try:
                self.store.con.execute(f"DROP TABLE IF EXISTS {table}")
            except Exception:
                # Preserve the original failure (or successful publication). A dead
                # connection/process can leave this exact attempt's named artifacts;
                # retries allocate new names and never sweep a shared prefix.
                _LOG.warning("forward-return staging cleanup failed for %s", table, exc_info=True)


def _build_shadow(build: _Build, ordered: str, source: str, physical: str) -> str:
    store = build.store
    ddl_row = store.con.execute("""
        SELECT sql FROM duckdb_tables()
        WHERE database_name = current_database() AND schema_name = current_schema()
          AND table_name = ? AND NOT temporary
    """, [_LIVE]).fetchone()
    if ddl_row is None or not ddl_row[0]:
        raise RuntimeError("forward-return publication requires a physical live table")
    # Clone authoritative column types, defaults, NOT NULL, PK and CHECK clauses.
    # Explicit secondary indexes are separate catalog objects, removed by 0317.
    declaration = str(ddl_row[0]).partition("(")[2]
    if not declaration:
        raise RuntimeError("cannot read the forward-return physical table declaration")
    shadow = build.create("shadow", f"CREATE TABLE {build.name('shadow')} ({declaration}")
    for prefix in range(256):
        # Open first/last bounds also retain non-SHA IDs from unrelated sources.
        predicates: list[str] = []
        bounds: list[object] = []
        if prefix:
            predicates.append("forward_return_id >= ?")
            bounds.append(f"{prefix:02x}")
        if prefix < 255:
            predicates.append("forward_return_id < ?")
            bounds.append(f"{prefix + 1:02x}")
        predicate = " AND ".join(predicates) or "TRUE"
        cursor: str | None = None
        while True:
            after = " AND forward_return_id > ?" if cursor is not None else ""
            params = [*bounds, *(() if cursor is None else (cursor,))]
            candidates = f"""
                SELECT {physical} FROM {ordered} WHERE {predicate}{after}
                UNION ALL
                SELECT {physical} FROM {_LIVE} WHERE source <> ? AND {predicate}{after}
            """
            candidate_params = [*params, source, *params]
            row = store.con.execute(f"""
                SELECT count(*), max(forward_return_id) FROM (
                    SELECT forward_return_id FROM ({candidates}) candidates
                    ORDER BY forward_return_id LIMIT {_SHADOW_BATCH_ROWS}
                ) batch
            """, candidate_params).fetchone()
            assert row is not None
            rows = int(row[0])
            if not rows:
                break
            end = str(row[1])
            store.con.execute(f"""
                INSERT INTO {shadow} ({physical})
                SELECT * FROM ({candidates}) candidates
                WHERE forward_return_id <= ? ORDER BY forward_return_id
            """, [*candidate_params, end])
            # The PK rejects duplicate IDs even across sources or batch boundaries.
            build.checkpoint(rows)
            cursor = end
    return shadow


def refresh_forward_return_publication(
    store: DuckDBStore,
    *,
    source: str,
    run_id: str | None,
    price_basis: str,
    cutoff: dt.datetime | None,
    columns: Sequence[str],
    horizons: Sequence[int],
    calendar_id: str,
    calendar_source: str,
    formation: str = "every_session",
) -> int:
    """Build every horizon and publish one source, retaining other sources.

    ``formation`` filters the anchor (``as_of_date``) rows only; see ``FORMATION_RULES``.
    Endpoints, terminal stitching and exclusions still use the full bar/calendar
    snapshot, so a filtered row equals the every-session row on the same anchor.

    Connection lifetime is bounded only for persistent callers with recorded
    analytical settings. In-memory and unconfigured callers retain their entire
    existing connection/session; they still use the same bounded insert batches.
    """
    if price_basis not in {"adjusted_close", "close"}:
        raise ValueError("price_basis must be adjusted_close or close")
    if formation not in FORMATION_RULES:
        raise ValueError(f"formation must be one of {FORMATION_RULES}")
    if not horizons or any(int(h) != h or h < 1 for h in horizons) or len(set(horizons)) != len(horizons):
        raise ValueError("horizons must be distinct positive session counts")
    build = _Build(store)
    try:
        stamp = store.con.execute("SELECT current_timestamp::TIMESTAMP").fetchone()
        assert stamp is not None
        described = store.con.execute(f"DESCRIBE {_LIVE}").fetchall()
        physical = ", ".join(f'"{row[0]}"' for row in described)
        metadata = [name for name in ("source_loaded_at", "updated_at")
                    if name in {row[0] for row in described}]
        required = {"price_basis", "calculation_version"}
        if not required.issubset({row[0] for row in described}):
            raise RuntimeError("forward-return publication requires migration 0325")
        insert_columns = ", ".join([*columns, "price_basis", "calculation_version", *metadata])
        bars = build.create("bars", _BARS_SQL.format(name=build.name("bars"), price_basis=price_basis),
                            [cutoff, cutoff])
        calendar = build.create("calendar", _CALENDAR_SQL.format(name=build.name("calendar")),
                                [calendar_id, calendar_source, cutoff, cutoff])
        terminals = build.create("terminals", _TERMINALS_SQL.format(name=build.name("terminals")),
                                 [cutoff, cutoff])
        terminal_prices = build.create(
            "terminal_prices", _TERMINAL_PRICES_SQL.format(
                name=build.name("terminal_prices"), terminals=terminals, bars=bars,
            ),
        )
        formations = bars if formation == "every_session" else build.create(
            "formations", _MONTH_END_FORMATIONS_SQL.format(
                name=build.name("formations"), bars=bars, calendar=calendar,
            ),
        )
        stage = build.create("stage", f"CREATE TABLE {build.name('stage')} AS SELECT * FROM {_LIVE} WHERE FALSE")
        formations_count = _count(store, formations)
        rows = 0
        for lower in range(1, formations_count + 1, _FORMATION_BATCH_ROWS):
            upper = lower + _FORMATION_BATCH_ROWS - 1
            scope = store.con.execute(f"""
                SELECT min(security_id), max(security_id) FROM {formations}
                WHERE formation_number BETWEEN ? AND ?
            """, [lower, upper]).fetchone()
            assert scope is not None
            for horizon in horizons:
                inserted = store.con.execute(
                    _RESULT_SQL.format(
                        stage=stage, insert_columns=insert_columns, bars=bars,
                        formations=formations,
                        calendar=calendar, terminal_prices=terminal_prices,
                        metadata_select=", ?" * len(metadata),
                    ),
                    [horizon, *scope, lower, upper, source, horizon, source, horizon, run_id,
                     price_basis, CALCULATION_VERSION, *([stamp[0]] * len(metadata))],
                ).fetchone()
                assert inserted is not None
                inserted_rows = int(inserted[0])
                rows += inserted_rows
                build.checkpoint(inserted_rows)
        if _count(store, stage) != rows:
            raise RuntimeError("forward-return stage count differs from committed result batches")
        # Release all input snapshots before sorting the expanded panel. This CTAS
        # has no ART index and uses DuckDB's external sort/spill implementation.
        for table in dict.fromkeys((formations, terminal_prices, terminals, calendar, bars)):
            build.drop(table)
        ordered = build.create("ordered", f"""
            CREATE TABLE {build.name('ordered')} AS
            SELECT * FROM {stage} ORDER BY forward_return_id
        """)
        if _count(store, ordered) != rows:
            raise RuntimeError("forward-return ordered stage lost rows")
        build.drop(stage)
        build.checkpoint(rows)
        shadow = _build_shadow(build, ordered, source, physical)
        expected = store.con.execute(f"SELECT count(*) FROM {_LIVE} WHERE source <> ?", [source]).fetchone()
        actual = store.con.execute(f"""
            SELECT count(*), count(*) FILTER (WHERE source = ?) FROM {shadow}
        """, [source]).fetchone()
        if expected is None or actual is None or tuple(actual) != (int(expected[0]) + rows, rows):
            raise RuntimeError("forward-return shadow count differs from complete publication inputs")
        # The helper validates the entire physical column/default/key contract and
        # uses a short transaction; dependent views keep their public table name.
        publish_validated_shadow(store, live_table=_LIVE, shadow_table=shadow)
        build.owned.remove(shadow)  # This name is now the published live table.
        return rows
    finally:
        build.cleanup()


_BARS_SQL = """
CREATE TABLE {name} AS
                WITH eligible AS (
                    SELECT *, coalesce(available_at,
                        CAST(trade_date AS TIMESTAMP) + INTERVAL '22 hours') AS price_available_at
                    FROM equity_daily_bars
                    WHERE (?::TIMESTAMP IS NULL OR coalesce(available_at,
                        CAST(trade_date AS TIMESTAMP) + INTERVAL '22 hours') <= ?)
                ), chosen AS (
                    SELECT security_id, symbol, trade_date,
                           {price_basis} AS price, price_available_at,
                           row_number() OVER (
                               PARTITION BY security_id, trade_date
                               ORDER BY price_available_at DESC, source ASC,
                                        vendor_security_id ASC NULLS LAST, symbol ASC,
                                        adjusted_close DESC NULLS LAST, close DESC NULLS LAST,
                                        source_loaded_at DESC
                           ) AS pick
                    FROM eligible
                )
                SELECT security_id, symbol, trade_date, price, price_available_at,
                       row_number() OVER (ORDER BY security_id, trade_date) AS formation_number
                FROM chosen WHERE pick = 1 AND price > 0 AND isfinite(price)
                ORDER BY security_id, trade_date
"""

_CALENDAR_SQL = """
CREATE TABLE {name} AS
                SELECT trade_date, row_number() OVER (ORDER BY trade_date) AS session_number
                FROM (
                    SELECT DISTINCT trade_date FROM trading_calendar
                    WHERE calendar_id = ? AND source = ? AND is_open
                      AND (?::TIMESTAMP IS NULL OR trade_date <= CAST(? AS DATE))
                )
"""

# Anchor = first observed session whose predecessor lies in an earlier calendar
# month, i.e. the entry session after the last observed session of a CLOSED month
# (a month is closed once a later session exists). Same bars, same numbering rule.
_MONTH_END_FORMATIONS_SQL = """
CREATE TABLE {name} AS
                WITH entries AS (
                    SELECT entry.trade_date
                    FROM {calendar} entry
                    JOIN {calendar} decision
                      ON decision.session_number = entry.session_number - 1
                    WHERE date_trunc('month', decision.trade_date)
                          <> date_trunc('month', entry.trade_date)
                )
                SELECT b.security_id, b.symbol, b.trade_date, b.price, b.price_available_at,
                       row_number() OVER (ORDER BY b.security_id, b.trade_date) AS formation_number
                FROM {bars} b JOIN entries e ON e.trade_date = b.trade_date
                ORDER BY b.security_id, b.trade_date
"""

_TERMINALS_SQL = """
CREATE TABLE {name} AS
                WITH revisions AS (
                    SELECT *, row_number() OVER (
                        PARTITION BY security_id, delist_date
                        ORDER BY available_at DESC, source_loaded_at DESC,
                                 terminal_return_id DESC
                    ) AS revision
                    FROM delisting_terminal_returns
                    WHERE (?::TIMESTAMP IS NULL OR available_at <= ?)
                )
                SELECT security_id, delist_date, terminal_return, terminal_return_source,
                       return_observation_id, available_at AS terminal_available_at,
                       isfinite(terminal_return) AND terminal_return >= -1 AS terminal_valid
                FROM revisions WHERE revision = 1
                QUALIFY row_number() OVER (
                    PARTITION BY security_id ORDER BY delist_date, terminal_return_id
                ) = 1
"""

_TERMINAL_PRICES_SQL = """
CREATE TABLE {name} AS
                SELECT t.*, b.trade_date AS last_price_date, b.price AS last_price,
                       b.price_available_at AS last_price_available_at
                FROM {terminals} t
                ASOF LEFT JOIN {bars} b
                  ON t.security_id = b.security_id AND t.delist_date > b.trade_date
"""

_RESULT_SQL = """
INSERT INTO {stage} ({insert_columns})
WITH legs AS (
                        SELECT f.security_id, f.symbol, f.trade_date AS as_of_date,
                               ending.trade_date AS forward_end_date,
                               CASE WHEN t.delist_date <= ending.trade_date
                                    THEN t.last_price / f.price - 1
                                    ELSE e.price / f.price - 1 END AS raw_forward_return,
                               CASE WHEN t.delist_date <= ending.trade_date
                                    THEN t.terminal_return END AS terminal_return,
                               CASE WHEN t.delist_date <= ending.trade_date
                                    THEN t.delist_date END AS delist_date,
                               CASE WHEN t.delist_date <= ending.trade_date
                                    THEN t.terminal_return_source END AS terminal_return_source,
                               CASE WHEN t.delist_date <= ending.trade_date
                                    THEN t.return_observation_id END AS return_observation_id,
                               greatest(f.price_available_at,
                                   CASE WHEN t.delist_date <= ending.trade_date
                                        THEN greatest(t.last_price_available_at,
                                                      t.terminal_available_at)
                                        ELSE e.price_available_at END) AS available_at
                        FROM {formations} f
                        JOIN {calendar} anchor ON anchor.trade_date = f.trade_date
                        JOIN {calendar} ending
                          ON ending.session_number = anchor.session_number + ?
                        LEFT JOIN (SELECT * FROM {bars} WHERE security_id BETWEEN ? AND ?) e
                          ON e.security_id = f.security_id AND e.trade_date = ending.trade_date
                        LEFT JOIN {terminal_prices} t ON t.security_id = f.security_id
                        WHERE f.formation_number BETWEEN ? AND ?
                          AND (t.delist_date IS NULL OR f.trade_date < t.delist_date)
                          -- Retain invalid selected evidence as an event boundary: never
                          -- resurrect an older return or use a post-terminal survivor leg.
                          AND (t.delist_date IS NULL OR t.delist_date > ending.trade_date
                               OR coalesce(t.terminal_valid, false))
                          AND ((t.delist_date <= ending.trade_date
                                AND t.last_price_date >= f.trade_date) OR e.price IS NOT NULL)
                    ), returns AS (
                        SELECT *, CASE WHEN terminal_return IS NOT NULL
                            THEN (1 + raw_forward_return) * (1 + terminal_return) - 1
                            ELSE raw_forward_return END AS forward_return
                        FROM legs
                    )
                    SELECT sha256(concat_ws('|', ?, security_id,
                                           CAST(as_of_date AS VARCHAR), CAST(? AS VARCHAR))),
                           ?, security_id, symbol, as_of_date, ?, forward_end_date,
                           raw_forward_return, terminal_return, forward_return,
                           terminal_return IS NOT NULL, terminal_return IS NOT NULL,
                           delist_date, terminal_return_source, return_observation_id,
                           true, available_at, ?, ?, ?{metadata_select}
                    FROM returns WHERE isfinite(forward_return)
"""
