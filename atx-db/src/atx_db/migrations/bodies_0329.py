"""0329 build ledger, unkeyed bulk tables, A9 unit-correction ledger, ``adj_close_basis``.

1. ``build_runs`` / ``build_batches`` (index §4 M3, node 1.3's batch ledger): one ``build_runs``
   row per (stage, run_key) with its spec and status (open | published | abandoned); one
   ``build_batches`` row per COMMITTED batch, written in that batch's own transaction, so a kill
   loses at most the batch in flight and a rerun skips every ledgered batch.
2. ``equity_bar_unit_corrections``: ledger of the A9 vendor-shares x1000 correction, one row per
   (price line, run of bars) the batch stage ``bars_unit_correction`` (built in 1.3, run in 1.8)
   scaled or kept, with the unit basis and its evidence. 0327 keeps only the read-only unit
   inventory and its abort rule; no migration rewrites ``equity_daily_bars``.
3. R-4 / M5: the VARCHAR sha256 PRIMARY KEY of the 13 bulk analytic tables in
   :data:`UNKEYED_BULK_TABLES` is dropped, and their secondary ART indexes with it. ART is not
   buffer-managed (a 32M-key VARCHAR ART does not fit 512 MB), and DuckDB cannot rename an
   indexed table, so a bulk table with one can never be published by swap. The id columns stay
   (NOT NULL) and ``table_catalog`` keeps every natural key; uniqueness becomes a publish-time
   ``GROUP BY key HAVING count(*) > 1`` check plus a critical DQC. Physical order follows
   ``(security_id, date)`` at load time (zone maps instead of an index).
4. ``adj_close_basis`` (C-36, C-61) on ``market_daily_metrics`` and ``equity_price_metrics``,
   folded into the same rebuild: the adjusted-close basis of the row (the writers'
   ``ADJ_CLOSE_BASIS`` = ``_vendor_artifact.REPAIR_VERSION``). NULL = a row of unknown basis
   (written before the column, or by a writer that does not store it yet).
5. Catalog rows for the three new tables and the two new columns, and the
   ``equity_daily_bars`` share-unit field text that 0327 no longer owns.

Every table is rebuilt by create/copy/swap (the live catalog DDL minus the key, a row-count
proof, drop, rename): DuckDB 1.5.5 cannot replay a WAL ``ALTER`` of a ``DEFAULT now()`` table.
A table above :data:`MAX_REBUILD_ROWS` makes 0329 raise before anything is written. This is
a per-table limit, not an aggregate transaction bound: the migration is one transaction
and several allowed tables could exceed 2M copied rows together. Production holds these
tables empty (0.1 / 1.2 receipts); a populated rehearsal must separately bound the sum. No
table created here has a ``DEFAULT now()`` column, a PRIMARY KEY or an index. The schema
contract pin is re-persisted by swap.
"""

from __future__ import annotations

import re

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0327 import (
    _columns,
    _constraints,
    _describe_fields,
    _index_sql,
    _primary_key,
    _quoted,
    _row_count,
    refresh_schema_contract_pin_by_swap,
)

_SCRATCH_SUFFIX = "__rebuild_0329"
#: M2 bound: 0329 copies each rebuilt table inside its one migration transaction.
MAX_REBUILD_ROWS = 2_000_000

#: Bulk analytic table -> its VARCHAR sha256 id (the dropped single-column PRIMARY KEY).
UNKEYED_BULK_TABLES: dict[str, str] = {
    "fundamental_fact_revisions": "fact_revision_id",
    "fundamental_statement_points": "statement_point_id",
    "fundamental_periods": "fundamental_period_id",
    "fundamental_ttm_points": "ttm_point_id",
    "fundamental_calendar_map": "calendar_map_id",
    "fundamental_calendar_ttm": "calendar_ttm_id",
    "fundamental_standardized": "standardized_id",
    "fundamental_standardization_exception": "exception_id",
    "derived_metric_values": "derived_value_id",
    "market_daily_metrics": "market_daily_id",
    "equity_price_metrics": "metric_id",
    "forward_returns_survivorship_safe": "forward_return_id",
    "daily_adjustment_factors": "daily_adjustment_id",
}
ADJ_CLOSE_BASIS_COLUMNS: dict[str, tuple[tuple[str, str], ...]] = {
    "market_daily_metrics": (("adj_close_basis", "VARCHAR"),),
    "equity_price_metrics": (("adj_close_basis", "VARCHAR"),),
}

BUILD_RUNS_DDL = """
    CREATE TABLE IF NOT EXISTS build_runs (
        stage VARCHAR NOT NULL, run_key VARCHAR NOT NULL, spec_json VARCHAR NOT NULL,
        status VARCHAR NOT NULL,            -- 'open' | 'published' | 'abandoned'
        created_at TIMESTAMP NOT NULL, published_at TIMESTAMP, code_sha VARCHAR, note VARCHAR)
"""
BUILD_BATCHES_DDL = """
    CREATE TABLE IF NOT EXISTS build_batches (
        stage VARCHAR NOT NULL, run_key VARCHAR NOT NULL, batch_id INTEGER NOT NULL,
        lo VARCHAR, hi VARCHAR, input_sha256 VARCHAR,
        rows_in BIGINT NOT NULL, rows_out BIGINT NOT NULL,
        started_at TIMESTAMP NOT NULL, finished_at TIMESTAMP NOT NULL,
        process_id INTEGER, note VARCHAR)   -- a row exists only for a COMMITTED batch
"""
UNIT_CORRECTIONS_DDL = """
    CREATE TABLE IF NOT EXISTS equity_bar_unit_corrections (   -- ledger for the A9 vendor-shares x1000 correction
        security_id VARCHAR NOT NULL, run_start DATE NOT NULL, run_end DATE NOT NULL,
        unit_basis VARCHAR NOT NULL, multiplier DOUBLE NOT NULL, evidence VARCHAR NOT NULL, created_at TIMESTAMP NOT NULL)
"""
#: (column, type, nullable) of each ledger table, checked after CREATE IF NOT EXISTS: a table left
#: by an unreleased 0327 body (the old correction ledger) has another shape and fails loud.
LEDGER_SHAPES: dict[str, tuple[tuple[str, str, bool], ...]] = {
    "build_runs": (
        ("stage", "VARCHAR", False), ("run_key", "VARCHAR", False), ("spec_json", "VARCHAR", False),
        ("status", "VARCHAR", False), ("created_at", "TIMESTAMP", False), ("published_at", "TIMESTAMP", True),
        ("code_sha", "VARCHAR", True), ("note", "VARCHAR", True),
    ),
    "build_batches": (
        ("stage", "VARCHAR", False), ("run_key", "VARCHAR", False), ("batch_id", "INTEGER", False),
        ("lo", "VARCHAR", True), ("hi", "VARCHAR", True), ("input_sha256", "VARCHAR", True),
        ("rows_in", "BIGINT", False), ("rows_out", "BIGINT", False), ("started_at", "TIMESTAMP", False),
        ("finished_at", "TIMESTAMP", False), ("process_id", "INTEGER", True), ("note", "VARCHAR", True),
    ),
    "equity_bar_unit_corrections": (
        ("security_id", "VARCHAR", False), ("run_start", "DATE", False), ("run_end", "DATE", False),
        ("unit_basis", "VARCHAR", False), ("multiplier", "DOUBLE", False), ("evidence", "VARCHAR", False),
        ("created_at", "TIMESTAMP", False),
    ),
}


def _unkeyed_body(ddl: str, table: str, key: str) -> str:
    """The live DDL column list without the ``key`` PRIMARY KEY; ``key`` stays ``NOT NULL``.

    DuckDB renders a single-column key either inline (``id VARCHAR PRIMARY KEY``) or as a
    trailing table constraint (``..., PRIMARY KEY(id)`` with ``id VARCHAR`` bare, its NOT NULL
    implied). Exactly one of the two must match; the caller verifies the rebuilt shape.
    """
    prefix = f"CREATE TABLE {table}("
    if not ddl.startswith(prefix) or not ddl.endswith(");"):
        raise RuntimeError(f"0329 cannot derive the {table} DDL for a governed rebuild")
    body = ddl[len(prefix):-2]
    name = rf'(?:{re.escape(key)}|"{re.escape(key)}")'
    body, inline = re.subn(rf"(^|, )({name}) VARCHAR PRIMARY KEY(?=, |$)", r"\1\2 VARCHAR NOT NULL", body)
    body, trailing = re.subn(rf", PRIMARY KEY\({name}\)(?=, |$)", "", body)
    if trailing:  # the bare key column loses its implied NOT NULL with the constraint: state it
        body = re.sub(rf"(^|, )({name}) VARCHAR(?=, |$)", r"\1\2 VARCHAR NOT NULL", body)
    declared = re.findall(rf"(?:^|, ){name} VARCHAR NOT NULL(?=, |$)", body)
    if inline + trailing != 1 or len(declared) != 1 or "PRIMARY KEY" in body.upper():
        raise RuntimeError(f"0329 cannot remove the {table} primary key ({key}) from its DDL")
    return body


def _rebuild_unkeyed(
    conn: duckdb.DuckDBPyConnection, table: str, key: str, additions: tuple[tuple[str, str], ...] = ()
) -> bool:
    """Rebuild ``table`` without its PRIMARY KEY and indexes, appending ``additions``. False = nothing to do.

    Refuses an unexpected key, a partially widened table and a table above
    :data:`MAX_REBUILD_ROWS`, all before any write. Proves the row count and that every
    column shape and every non-key constraint is unchanged.
    """
    existing = _columns(conn, table)
    names = [str(row[0]) for row in existing]
    missing = tuple((name, data_type) for name, data_type in additions if name not in names)
    if missing and len(missing) != len(additions):
        raise RuntimeError(f"0329 found {table} partially widened: missing {[name for name, _ in missing]}")
    primary_key = _primary_key(conn, table)
    if primary_key not in ((), (key,)):
        raise RuntimeError(f"0329 refuses to rebuild {table}: primary key {primary_key}, expected ({key},)")
    indexes = _index_sql(conn, table)
    if not primary_key and not indexes and not missing:
        return False
    before = _row_count(conn, table)
    if before > MAX_REBUILD_ROWS:
        raise RuntimeError(
            f"0329 refuses to rebuild {table} in one migration transaction: {before:,} rows > "
            f"{MAX_REBUILD_ROWS:,} (M2). Production held it empty at 0.1/1.2; re-plan before migrating."
        )
    ddl = conn.execute(
        """
        SELECT sql FROM duckdb_tables()
        WHERE database_name = current_database() AND schema_name = current_schema() AND table_name = ?
        """,
        [table],
    ).fetchone()
    if ddl is None:
        raise RuntimeError(f"0329 cannot derive the {table} DDL for a governed rebuild")
    body = _unkeyed_body(str(ddl[0]), table, key) if primary_key else str(ddl[0])[len(f"CREATE TABLE {table}("):-2]
    added = "".join(f', "{name}" {data_type}' for name, data_type in missing)
    constraints = [row for row in _constraints(conn, table) if row[0] != "PRIMARY KEY"]
    scratch = f"{table}{_SCRATCH_SUFFIX}"
    conn.execute(f"DROP TABLE IF EXISTS {scratch}")
    conn.execute(f"CREATE TABLE {scratch}({body}{added});")
    conn.execute(f"INSERT INTO {scratch} ({_quoted(names)}) SELECT {_quoted(names)} FROM {table}")
    after = _row_count(conn, scratch)
    if after != before:
        raise RuntimeError(f"0329 {table} rebuild row-count proof failed: {before} source rows, {after} copied")
    conn.execute(f"DROP TABLE {table}")  # drops its PRIMARY KEY and secondary ART indexes with it
    conn.execute(f"ALTER TABLE {scratch} RENAME TO {table}")
    expected = [*existing, *((name, data_type, True, None) for name, data_type in missing)]
    if _columns(conn, table) != expected or _constraints(conn, table) != constraints or _index_sql(conn, table):
        raise RuntimeError(f"0329 {table} rebuild changed column shapes or non-key constraints, or kept an index")
    return True


def _build_ledger_tables(conn: duckdb.DuckDBPyConnection) -> None:
    for ddl in (BUILD_RUNS_DDL, BUILD_BATCHES_DDL, UNIT_CORRECTIONS_DDL):
        conn.execute(ddl)
    for table, shape in LEDGER_SHAPES.items():
        live = tuple((str(name), str(data_type), bool(nullable)) for name, data_type, nullable, _ in _columns(conn, table))
        if live != shape or _constraints(conn, table) != sorted(
            ("NOT NULL", (name,), "") for name, _, nullable in shape if not nullable
        ) or _index_sql(conn, table):
            raise RuntimeError(f"0329 found {table} with an unexpected shape: {live}")
    tables = (
        ("build_runs", "control", "build_ledger", "stage,run_key",
         "Batch-ledger runs (M3): one row per build of a heavy stage with its canonical spec, status "
         "(open | published | abandoned) and code sha. A run writes into <table>__next staging tables through "
         "build_batches and is published by one short swap followed by CHECKPOINT.",
         "Control ledger; created_at/published_at are wall clocks of the build, not data availability."),
        ("build_batches", "control", "build_ledger", "stage,run_key,batch_id",
         "Committed batches of a build run: one row per batch, written in the batch's own transaction with "
         "its output, so a rerun skips every ledgered batch and a kill loses at most the batch in flight.",
         "Control ledger; started_at/finished_at are wall clocks of the batch, not data availability."),
        ("equity_bar_unit_corrections", "control", "equity_daily_bars", "security_id,run_start",
         "Ledger of the A9 TickerHistory vendor-shares correction (batch stage bars_unit_correction): one row "
         "per price line and run of bars with the stored unit basis (thousands | units | shares_unit_suspect), "
         "the multiplier applied to shares_outstanding (market_cap_usd recomputed) and the evidence. Migration "
         "0327 only gates on the read-only unit inventory; it corrects nothing.",
         "Control ledger; a corrected line's bars change value when the stage publishes (no bar revision "
         "rows). created_at is the stage's wall clock."),
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog
          (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?, now())
        """,
        [(name, layer, entity, grain, description, '["' + grain.replace(",", '","') + '"]', notes)
         for name, layer, entity, grain, description, notes in tables],
    )
    _catalog_fields_for_tables(conn, tuple(LEDGER_SHAPES))
    _describe_fields(conn, "build_runs", (
        ("stage", "Heavy stage name (e.g. companyfacts_load, bars_unit_correction)."),
        ("run_key", "Identity of one build of the stage, derived from its spec; with stage the natural key."),
        ("spec_json", "Canonical JSON spec of the build (inputs, parameters, code identity)."),
        ("status", "open (batches may still commit) | published (swapped live) | abandoned (never published)."),
        ("created_at", "Wall clock when the run was opened (set by the writer; no DEFAULT now())."),
        ("published_at", "Wall clock of the publishing swap; NULL unless status is published."),
        ("code_sha", "Git commit of the code that built the run; NULL when unknown."),
    ))
    _describe_fields(conn, "build_batches", (
        ("batch_id", "Ordinal of the batch within (stage, run_key); with them the natural key."),
        ("lo", "Inclusive lower bound of the batch key range as text (bucket, CIK, date); NULL = open."),
        ("hi", "Inclusive upper bound of the batch key range as text; NULL = open."),
        ("input_sha256", "SHA-256 of the batch's inputs, when the stage records one."),
        ("rows_in", "Input rows the batch read."),
        ("rows_out", "Rows the batch committed to its output (the M2 bound is ~2M)."),
        ("started_at", "Wall clock when the batch started."),
        ("finished_at", "Wall clock of the batch commit; a row exists only for a committed batch."),
        ("process_id", "OS process id of the worker that committed the batch."),
    ))
    _describe_fields(conn, "equity_bar_unit_corrections", (
        ("security_id", "Price line whose bars the row covers."),
        ("run_start", "First trade_date of the line's run of bars (inclusive); with security_id the natural key. "
                      "Whole lines are decided, and a run boundary never falls on a factor-step bar (0.13 D10)."),
        ("run_end", "Last trade_date of the run (inclusive)."),
        ("unit_basis", "Disposition of the line's stored vendor shares_outstanding: thousands (scaled) | units "
                       "(kept) | shares_unit_suspect (ruling C-81: a line of a thousands run with a row above the "
                       "1e8 stored ceiling; its share-derived values are withheld, never rescaled by guess). A9: the "
                       "input file format and the run's median must agree; any other doubt aborts."),
        ("multiplier", "Factor applied to shares_outstanding: 1000.0 for vendor thousands, 1.0 when kept or "
                       "shares_unit_suspect."),
        ("evidence", "The A9 evidence of the decision (input format, median verdict, rows above the 1e8 ceiling)."),
        ("created_at", "Wall clock when the stage wrote the row (set by the writer; no DEFAULT now())."),
    ))
    _describe_fields(conn, "equity_daily_bars", (
        ("shares_outstanding",
         "Shares outstanding. TickerHistory parquet deliveries state thousands: the unit-aware bulk loader scales "
         "them; rows of older parquet loads stay in vendor thousands until the batch stage bars_unit_correction "
         "scales them (ledger equity_bar_unit_corrections; read-only gate "
         "bodies_0327.ticker_history_unit_inventory)."),
        ("market_cap_usd",
         "shares_outstanding * close in USD; recomputed by bars_unit_correction for the lines it scales."),
    ))


def _unkeyed_bulk_tables(conn: duckdb.DuckDBPyConnection) -> None:
    for table, key in UNKEYED_BULK_TABLES.items():
        _rebuild_unkeyed(conn, table, key, ADJ_CLOSE_BASIS_COLUMNS.get(table, ()))
    _describe_fields(conn, "market_daily_metrics", (
        ("adj_close_basis",
         "Adjusted-close basis of adj_close and of every return and metric chained on it: the writer's "
         "market_daily.ADJ_CLOSE_BASIS (_vendor_artifact.REPAIR_VERSION, e.g. vendor_artifact_repair_v2). "
         "Readers of stored adj_close require the current value. NULL = unknown basis (a row written before "
         "the column, or by a writer that does not store it yet)."),
    ))
    _describe_fields(conn, "equity_price_metrics", (
        ("adj_close_basis",
         "Adjusted-close basis of adjusted_close and of every return and metric chained on it: "
         "equity_price_metrics.ADJ_CLOSE_BASIS (_vendor_artifact.REPAIR_VERSION, e.g. "
         "vendor_artifact_repair_v2). NULL = unknown basis (a row written before the column)."),
    ))


def _build_ledger_bulk_keys(conn: duckdb.DuckDBPyConnection) -> None:
    _build_ledger_tables(conn)
    _unkeyed_bulk_tables(conn)
    # The pin is re-persisted by swap, never in place: an in-place WAL replay corrupts the ART.
    refresh_schema_contract_pin_by_swap(conn)


MIGRATIONS = [Migration(version=329, name="build_ledger_bulk_keys", up=_build_ledger_bulk_keys)]
