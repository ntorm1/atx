"""Incremental daily TickerHistory3 date-partition ingestion (P14).

After the one-time full load (:mod:`atx_db.ticker_history_bulk`), the vendor
delivers one date partition per trading day (SpiderRock S3 TickerHistory3; the
real source is gated by ruling RX11 and never fetched here). This module
applies such a partition to ``equity_daily_bars`` without rebuilding the table.

Watermark
    One ``dataset_watermarks`` row per (source, partition date)
    (``partition:<source>:<date>``, the partition file's sha256 and what was
    applied) and one for the latest applied date (``latest_partition:<source>``).
    Partitions are applied in date order: a date before the latest applied one
    that was never applied is refused (:class:`OutOfOrderPartitionError`).
    Re-applying a partition with the recorded sha256 changes nothing.

Rows
    A partition holds exactly one trade date. Its rows are projected exactly as
    the bulk loader projects them (invalid OHLCV excluded, duplicate vendor keys
    quarantined). New keys are appended; a key already stored with different
    values (a re-delivered, corrected partition) is restated (below). Rows of
    vendor lines not yet in the table get the bulk loader's line id (current SEC
    ticker map, else ``TBLTICKERHISTORY-<vendor id>``).

Adjustment basis: stored bars are never rebased
    The vendor's ``cumulReturnFactor`` is backward-anchored: it is 1.0 on each
    line's latest date (measured on the retained file: 25,762 of 25,762 lines'
    last-date rows), so every dividend or split restates all earlier factors of
    the line by one multiple. The partition's ``returnFactor`` links its day to
    the line's previous one (``crf_prev = crf_day x returnFactor``, the vendor
    recurrence checked in :mod:`atx_db.ticker_history_quality`). Instead of
    rewriting history on the new basis, a new bar continues the stored factor
    ``f = adjusted_close / close`` of the line's latest stored bar through that
    recurrence: ``f_day = f_prev / returnFactor``. Every stored bar keeps the
    adjusted close it was written with (no UPDATE of ``equity_daily_bars``, and
    no stored value ever changes when later data arrives), day-over-day ratios
    of ``adjusted_close`` are the vendor's total returns across bulk and
    incremental bars alike, and the absolute level is relative to the line's
    anchor (factor 1.0 on the last date of the bulk file, or on a new line's
    first partition). A line's first bar takes the partition's own factor
    (``close x cumulReturnFactor``). A missing or invalid ``returnFactor`` keeps
    the previous factor (no event assumed) and a previous bar without a factor
    restarts at the partition's factor; both are counted as
    ``rebase_unverifiable_lines`` (warning). A new key dated before a line's
    latest stored bar (a re-delivered partition that adds a line; a gap fill)
    takes the neighbouring stored factor, so it adds no factor step, and is
    counted in ``gap_fill_rows`` (warning).

Rebase ledger
    For a line with no stored bar after the partition date, the vendor's
    restatement of its earlier adjusted history at this delivery is ``m =
    cumulReturnFactor x returnFactor`` (the previous delivery's factor is 1.0 on
    its own date). When ``|m - 1| > rebase_tolerance`` one row is appended to
    ``equity_adjustment_rebases``: (line, receipt clock, ``multiplier`` m,
    first/last affected trade date = the line's first stored date and the
    previous bar's date, detection evidence). A re-delivered latest partition
    whose factor changed adds a correcting row (m over the product already
    recorded for that date). The ledger is append-only history of when the
    vendor's basis moved; stored bars do not depend on it.
    :func:`bars_asof_sql` applies the multiplier at read time: its ``vendor``
    basis rescales each line by ``close / adjusted_close`` of its latest bar
    known at the cutoff. That scale equals the product of the ledger
    multipliers recorded since the line's anchor and known by then (up to
    returnFactors within the tolerance), and gives the vendor's own convention
    as of the cutoff (factor 1.0 on the line's latest known bar).

Series (P8 / R1d)
    ``run_id`` on a bar names its factor-basis series: a new bar of an existing
    line carries the ``run_id`` of the stored bar whose factor it continues,
    so the corporate-action and split-epoch readers, which take one
    (source, run_id) series per line, see incremental dates as the same
    series and its factor steps as events. A new line's bars carry the run of
    the partition that first delivered it. The partition's own run is in
    ``dataset_runs``, its watermark, its ledger rows and restated revisions.

Restatements and point-in-time reads
    ``equity_daily_bars`` keeps exactly one row per (source, security_id,
    trade_date). A key whose raw values changed in a re-delivered partition,
    or whose factor changed (a corrected ``returnFactor`` on a re-delivered
    latest date), is restated: the superseded row is appended to
    ``equity_daily_bar_revisions`` (``is_latest_revision = false``,
    ``superseded_at``, ``superseded_by_run_id``, ``revision_reason``), then a
    small keyed DELETE + INSERT of those rows (one trade date) replaces it,
    in the partition's transaction. The restated row keeps the stored factor
    unless its line has no later bar (then the recurrence above), keeps its
    series ``run_id``, and is available at ``greatest(receipt clock, the
    superseded row's available_at)`` (the superseded row's ``superseded_at``).
    :func:`bars_asof_sql` reconstructs the bars as known at a cutoff: the
    newest version available then. Both tables are formalized by migration
    0328 (:data:`REBASES_TABLE_DDL`, :data:`REVISIONS_TABLE_DDL`). On a
    warehouse below 0328 they are created only when the caller passes
    ``allow_unmigrated_revisions_table=True`` (non-production); otherwise a
    partition that needs one is refused before any write
    (:class:`RevisionsTableNotMigratedError`).

Durability (DuckDB 1.5 WAL replay)
    Everything from the restatement to the watermark, the ``dataset_runs`` row,
    the source-file receipt and the ``incremental_partition_applied`` quality
    row commits in one transaction; a CHECKPOINT follows and the WAL must then
    be empty. No keyed row of an indexed table is deleted and re-inserted: the
    watermark and the source-file receipt are updated in place (non-key
    columns) or inserted. A kill before COMMIT leaves nothing; a kill after it
    replays one committed partition, and a rerun reports it ``unchanged``.

Share units (A8)
    The partition's raw ``shares`` unit is decided by migration 0327's rule,
    read-only (``_share_unit_decision``: file-format evidence and the
    partition's own median must agree, or the partition is refused). The
    scaled counts then pass the bulk loader's magnitude and DEI check
    (:func:`atx_db.ticker_history_bulk.vendor_share_unit_check`).

Directory and scheduling
    :func:`ingest_directory_files` appends retained Nasdaq Trader directory and
    Adds/Deletes files under the A1 pinned-snapshot policy
    (:mod:`atx_db.symbol_directory`). :func:`refresh_daily_prices` is the hand-off
    hook for C12 scheduling. It applies given partitions in date order, then the
    directory files. :func:`pending_partitions` lists what is not yet applied.
    There is no scheduler and no network code here. A real fetcher
    (:class:`PartitionFetcher`) must identify itself with
    :data:`APPROVED_USER_AGENT`.
"""

from __future__ import annotations

import datetime as dt
import json
import logging
import uuid
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

from .connection import DuckDBStore
from .migrations.bodies_0327 import _share_unit_decision
from .symbol_directory import APPROVED_USER_AGENT
from .ticker_history import SOURCE_NAME, TBLTICKERHISTORY_ID_TYPE
from .ticker_history_bulk import (
    _RAW_CTE,
    SHARES_UNIT_BY_FORMAT,
    SHARES_UNIT_SCALE,
    _create_symbol_map,
    vendor_share_unit_check,
)
from .ticker_history_quality import source_diagnostics, source_format, source_provenance, stage_source
from .warehouse import file_sha256, now_utc_naive, quality_check

LOGGER = logging.getLogger(__name__)

__all__ = [
    "APPROVED_USER_AGENT",
    "BASIS_STORED",
    "BASIS_VENDOR",
    "DATASET_ID",
    "REBASES_TABLE",
    "REBASES_TABLE_DDL",
    "REBASE_TOLERANCE",
    "REVISIONS_TABLE",
    "REVISIONS_TABLE_DDL",
    "DailyRefreshRequest",
    "DailyRefreshResult",
    "DirectoryFile",
    "OutOfOrderPartitionError",
    "PartitionFetcher",
    "PartitionFile",
    "PartitionIngestOptions",
    "PartitionIngestResult",
    "RevisionsTableNotMigratedError",
    "bars_asof_sql",
    "ingest_directory_files",
    "ingest_ticker_history_partition",
    "partition_trade_dates",
    "pending_partitions",
    "refresh_daily_prices",
]

DATASET_ID = "tbltickerhistory_daily"
#: |m - 1| above this is a vendor factor rebase. The vendor's returnFactor is
#: single precision (~6e-8 relative); a 0.01 % dividend moves m by 1e-4. The
#: same tolerance decides whether a re-delivered bar's adjusted close changed.
REBASE_TOLERANCE = 1e-6
#: :func:`bars_asof_sql` bases: the vendor's convention as of the cutoff, or the stored (anchored) values.
BASIS_VENDOR = "vendor"
BASIS_STORED = "stored"
REVISIONS_TABLE = "equity_daily_bar_revisions"
REVISION_REASON_RESTATEMENT = "partition_restatement"
REBASES_TABLE = "equity_adjustment_rebases"
#: The ledger's ``detection_basis``: m from the vendor recurrence
#: ``crf_prev = crf_day x returnFactor`` against the stored previous bar.
REBASE_DETECTION_BASIS = "vendor_crf_return_factor_recurrence"
STATUS_APPLIED = "applied"
STATUS_UNCHANGED = "unchanged"

#: ``equity_daily_bars`` columns, in table order (as the bulk loader's shadow table).
BAR_COLUMNS = (
    "source", "security_id", "vendor_security_id", "symbol", "trade_date", "open", "high", "low", "close",
    "adjusted_close", "volume", "vwap", "dividend_amount", "split_factor", "is_adjusted", "available_at", "run_id",
    "source_loaded_at", "as_of_date", "is_latest_revision", "shares_outstanding", "market_cap_usd",
)
_COLUMNS = ", ".join(BAR_COLUMNS)
#: Raw values that make a stored row differ from a re-delivered one. The
#: adjusted close is compared separately, within ``rebase_tolerance``, after the
#: re-delivered row's factor is derived (see the module docstring).
_VALUE_COLUMNS = ("vendor_security_id", "symbol", "open", "high", "low", "close", "volume",
                  "shares_outstanding", "market_cap_usd")

#: The rebase ledger: one row per (line, vendor factor rebase): at
#: ``available_at`` the vendor restated the line's adjusted history dated
#: first..last_affected_date by ``multiplier``. Stored bars are never rebased
#: (see the module docstring). Formalized by migration 0328 (below it, created
#: only with ``allow_unmigrated_revisions_table``); 0328 froze this DDL. Key
#: (source, security_id, partition_date, available_at). No ``DEFAULT now()``
#: (DuckDB 1.5 WAL replay), no ART index.
REBASES_TABLE_DDL = f"""
CREATE TABLE IF NOT EXISTS {REBASES_TABLE} (
    source VARCHAR NOT NULL,
    security_id VARCHAR NOT NULL,
    partition_date DATE NOT NULL,
    available_at TIMESTAMP NOT NULL,
    multiplier DOUBLE NOT NULL,
    first_affected_date DATE NOT NULL,
    last_affected_date DATE NOT NULL,
    detection_basis VARCHAR NOT NULL,
    cumul_return_factor DOUBLE,
    return_factor DOUBLE,
    prior_close DOUBLE,
    prior_adjusted_close DOUBLE,
    run_id VARCHAR NOT NULL,
    source_loaded_at TIMESTAMP NOT NULL,
    CHECK (multiplier > 0 AND isfinite(multiplier)),
    CHECK (first_affected_date <= last_affected_date),
    CHECK (last_affected_date < partition_date)
)
"""

#: Superseded rows of restated keys (a re-delivered partition). Formalized by
#: migration 0328 (below it, created only with
#: ``allow_unmigrated_revisions_table``); 0328 froze this DDL. No
#: ``DEFAULT now()`` column (DuckDB 1.5 WAL replay), no ART index.
REVISIONS_TABLE_DDL = f"""
CREATE TABLE IF NOT EXISTS {REVISIONS_TABLE} (
    source VARCHAR NOT NULL,
    security_id VARCHAR NOT NULL,
    vendor_security_id VARCHAR,
    symbol VARCHAR NOT NULL,
    trade_date DATE NOT NULL,
    open DOUBLE,
    high DOUBLE,
    low DOUBLE,
    close DOUBLE,
    adjusted_close DOUBLE,
    volume BIGINT,
    vwap DOUBLE,
    dividend_amount DOUBLE,
    split_factor DOUBLE,
    is_adjusted BOOLEAN NOT NULL,
    available_at TIMESTAMP,
    run_id VARCHAR,
    source_loaded_at TIMESTAMP NOT NULL,
    as_of_date DATE,
    is_latest_revision BOOLEAN NOT NULL,
    shares_outstanding BIGINT,
    market_cap_usd DOUBLE,
    superseded_at TIMESTAMP NOT NULL,
    superseded_by_run_id VARCHAR NOT NULL,
    revision_reason VARCHAR NOT NULL,
    CHECK (NOT is_latest_revision),
    CHECK (revision_reason IN ('{REVISION_REASON_RESTATEMENT}')),
    CHECK (available_at IS NULL OR superseded_at >= available_at)
)
"""

_TEMPORARIES = (
    "ticker_history_source_rows", "ticker_history_source_keys", "broad_symbol_map",
    "_p14_existing_lines", "_p14_lines", "_p14_new", "_p14_span", "_p14_near", "_p14_stored", "_p14_calc",
    "_p14_restated",
)


class OutOfOrderPartitionError(ValueError):
    """A partition dated before the latest applied one, never applied itself."""


class RevisionsTableNotMigratedError(RuntimeError):
    """The rebase ledger or the revisions table is needed but not formalized (0328) and not allowed."""


@dataclass(frozen=True)
class PartitionIngestOptions:
    partition_path: Path
    #: Receipt clock of the partition's bytes: every new or restated row's
    #: ``available_at`` (floored at trade_date + 22h). None reads the retained
    #: file's ``<file>.receipt.json``, else its modification time (A1 rule).
    received_at: dt.datetime | None = None
    source: str = SOURCE_NAME
    run_id: str | None = None
    #: A real daily partition carries ~5-12K lines; fewer means a truncated file.
    minimum_partition_lines: int = 5_000
    shares_unit_min_pairs: int = 20
    rebase_tolerance: float = REBASE_TOLERANCE
    #: Create ``equity_adjustment_rebases`` / ``equity_daily_bar_revisions`` when
    #: missing (non-production only, until migration 0328 formalizes them).
    allow_unmigrated_revisions_table: bool = False


@dataclass(frozen=True)
class PartitionIngestResult:
    partition_date: dt.date
    status: str
    run_id: str | None
    sha256: str
    received_at: dt.datetime
    appended_rows: int = 0
    restated_rows: int = 0
    #: Ledger rows written (one per rebased line); no stored bar changes on a rebase.
    rebased_lines: int = 0
    new_lines: int = 0
    detail: dict[str, Any] = field(default_factory=dict)

    @property
    def changed_rows(self) -> int:
        """Rows of ``equity_daily_bars`` written (appended or restated)."""
        return self.appended_rows + self.restated_rows


@dataclass(frozen=True)
class PartitionFile:
    path: Path
    received_at: dt.datetime | None = None


@dataclass(frozen=True)
class DirectoryFile:
    #: ``nasdaqlisted``, ``otherlisted`` or ``listing_events`` (Trading System Adds/Deletes).
    kind: str
    path: Path
    source_url: str


@dataclass(frozen=True)
class DailyRefreshRequest:
    partitions: tuple[PartitionFile, ...] = ()
    directory_files: tuple[DirectoryFile, ...] = ()
    #: Report cutoff for the directory files (A1: a file created or received later is refused).
    directory_cutoff: dt.date | None = None
    source: str = SOURCE_NAME
    minimum_partition_lines: int = 5_000
    allow_unmigrated_revisions_table: bool = False


@dataclass(frozen=True)
class DailyRefreshResult:
    partitions: tuple[PartitionIngestResult, ...]
    directory: tuple[dict[str, Any], ...]


class PartitionFetcher(Protocol):
    """What C12 plugs in once RX11 approves a daily price source (not implemented here).

    Implementations must send ``User-Agent: atx-db/0.1 atx-research@example.com``
    (:data:`APPROVED_USER_AGENT`) on every request, write each partition to a
    retained local file with a ``<file>.receipt.json`` receipt (the A1 cache
    receipt: source_url, received_at, sha256, bytes), and never overwrite a
    retained partition.
    """

    def list_available(self, since: dt.date | None) -> Sequence[dt.date]: ...

    def fetch(self, partition_date: dt.date, dest_dir: Path) -> PartitionFile: ...


def _watermark_name(source: str, partition_date: dt.date) -> str:
    return f"partition:{source}:{partition_date.isoformat()}"


def _latest_name(source: str) -> str:
    return f"latest_partition:{source}"


def _watermark(store: DuckDBStore, name: str) -> dict[str, Any] | None:
    row = store.con.execute(
        "SELECT watermark_value FROM dataset_watermarks WHERE dataset_id = ? AND watermark_name = ?",
        [DATASET_ID, name],
    ).fetchone()
    if row is None:
        return None
    try:
        value = json.loads(row[0])
    except (TypeError, ValueError):
        return {"value": row[0]}
    return value if isinstance(value, dict) else {"value": value}


def _set_watermark(store: DuckDBStore, name: str, value: dict[str, Any], at: dt.datetime) -> None:
    """Upsert one watermark without deleting its key (a replayed DELETE+INSERT of an ART key corrupts it)."""
    params = [json.dumps(value, sort_keys=True, default=str), at, DATASET_ID, name]
    if _watermark(store, name) is not None:
        store.con.execute(
            "UPDATE dataset_watermarks SET watermark_value = ?, updated_at = ? "
            "WHERE dataset_id = ? AND watermark_name = ?", params,
        )
    else:
        store.con.execute(
            "INSERT INTO dataset_watermarks (watermark_value, updated_at, dataset_id, watermark_name) "
            "VALUES (?, ?, ?, ?)", params,
        )


def _record_partition_file(store: DuckDBStore, *, path: Path, sha: str, metadata: dict[str, Any],
                           at: dt.datetime) -> None:
    """:func:`atx_db.warehouse.record_source_file`'s ``raw_source_files`` row, upserted without deleting its key."""
    source_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"{DATASET_ID}:{path}:{path}"))
    values = [DATASET_ID, str(path), str(path), sha, path.stat().st_size, at, "available",
              json.dumps(metadata, sort_keys=True, default=str), source_id]
    exists = store.con.execute("SELECT count(*) FROM raw_source_files WHERE source_id = ?", [source_id]).fetchone()
    if exists and exists[0]:
        store.con.execute(
            "UPDATE raw_source_files SET dataset_id = ?, source_url = ?, cache_path = ?, sha256 = ?, byte_count = ?, "
            "fetched_at = ?, status = ?, metadata_json = ? WHERE source_id = ?", values,
        )
    else:
        store.con.execute(
            "INSERT INTO raw_source_files (dataset_id, source_url, cache_path, sha256, byte_count, fetched_at, status, "
            "metadata_json, source_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", values,
        )


def _checkpoint(store: DuckDBStore) -> None:
    """CHECKPOINT the committed partition; the WAL must then be empty, so a crash never replays it (M3)."""
    store.con.execute("CHECKPOINT")
    if str(store.path) == ":memory:":
        return
    wal = Path(f"{store.path}.wal")
    size = wal.stat().st_size if wal.exists() else 0
    if size:
        raise RuntimeError(
            f"partition committed, but {wal} still holds {size:,} bytes after CHECKPOINT (another transaction is "
            "open on this database); stop and checkpoint before applying the next partition"
        )


def _table_exists(store: DuckDBStore, name: str) -> bool:
    row = store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE table_name = ? AND NOT temporary", [name]
    ).fetchone()
    return bool(row and row[0])


def _receipt(path: Path, sha256: str) -> tuple[dt.datetime, str]:
    # The A1 retained-file rule (cache receipt, else mtime), read-only.
    from .activation import _read_cache_receipt

    return _read_cache_receipt(path, sha256)


def _drop_temporaries(store: DuckDBStore) -> None:
    for name in _TEMPORARIES:
        store.con.execute(f"DROP TABLE IF EXISTS temp.main.{name}")


def partition_trade_dates(store: DuckDBStore, path: Path) -> tuple[dt.date | None, dt.date | None]:
    """The first and last trade date of a partition file (a bounded aggregate)."""
    reader = ("read_parquet(?)" if source_format(path) == "parquet"
              else "read_csv(?, delim = '\t', header = true, all_varchar = true)")
    row = store.con.execute(
        f"SELECT min(try_cast(tradingDate AS DATE)), max(try_cast(tradingDate AS DATE)) FROM {reader}", [str(path)]
    ).fetchone()
    return (row[0], row[1]) if row else (None, None)


def _unit_decision(store: DuckDBStore, path: Path) -> tuple[int, str, dict[str, Any]]:
    """Scale and raw unit of the partition's ``shares`` (0327's rule: format evidence + median)."""
    row = store.con.execute(
        """
        SELECT count(*), count(DISTINCT vendor_id), median(shares_outstanding) FILTER (WHERE shares_outstanding > 0)
        FROM ticker_history_source_rows WHERE vendor_id > 0 AND shares_outstanding IS NOT NULL
        """
    ).fetchone()
    rows, lines, median = (int(row[0]), int(row[1]), row[2]) if row else (0, 0, None)
    decision = _share_unit_decision(
        rows, lines, None if median is None else float(median), json.dumps({"source_path": str(path)}), True
    )
    if decision["action"] == "abort":
        raise RuntimeError(f"partition share unit refused ({path}): {decision['reason']}")
    if decision["action"] == "scale_x1000":
        return 1000, "thousands", decision
    if decision.get("stored_unit") == "units":
        return 1, "units", decision
    unit = SHARES_UNIT_BY_FORMAT[source_format(path)]  # no positive shares: nothing to scale
    return SHARES_UNIT_SCALE[unit], unit, decision


def _map_lines(store: DuckDBStore, source: str) -> None:
    """``_p14_lines``: every positive vendor line of the partition -> its ``security_id``."""
    con = store.con
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _p14_existing_lines AS
        SELECT vendor_security_id, arg_max(security_id, trade_date) AS security_id
        FROM equity_daily_bars
        WHERE source = ?
          AND vendor_security_id IN (SELECT vendor_security_id FROM ticker_history_source_rows WHERE vendor_id > 0)
        GROUP BY vendor_security_id
        """,
        [source],
    )
    fresh = _count(store, """
        SELECT count(DISTINCT vendor_security_id) FROM ticker_history_source_rows
        WHERE vendor_id > 0 AND vendor_security_id NOT IN (SELECT vendor_security_id FROM _p14_existing_lines)
    """)
    if not fresh:  # the usual day: every line is known, no symbol map needed
        con.execute("CREATE OR REPLACE TEMP TABLE _p14_lines AS "
                    "SELECT vendor_security_id, security_id, false AS new_line FROM _p14_existing_lines")
        return
    _create_symbol_map(store)
    # A new line takes its current SEC ticker's id unless another line of this
    # source already uses it (or an earlier new line does): then its vendor id.
    # ``taken`` is an inner join built on the few fresh ids, never on the bars.
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _p14_lines AS
        WITH incoming AS (
            SELECT vendor_security_id, min(current_symbol) AS current_symbol
            FROM ticker_history_source_rows WHERE vendor_id > 0
            GROUP BY vendor_security_id
        ), fresh AS (
            SELECT i.vendor_security_id, i.current_symbol, m.security_id AS sec_id
            FROM incoming i
            LEFT JOIN _p14_existing_lines e USING (vendor_security_id)
            LEFT JOIN broad_symbol_map m ON m.symbol = i.current_symbol
            WHERE e.vendor_security_id IS NULL
        ), taken AS (
            SELECT DISTINCT b.security_id
            FROM equity_daily_bars b JOIN (SELECT DISTINCT sec_id FROM fresh) f ON b.security_id = f.sec_id
            WHERE b.source = ?
        ), ranked AS (
            SELECT f.*, row_number() OVER (PARTITION BY sec_id ORDER BY vendor_security_id) AS rk,
                   t.security_id IS NOT NULL AS sec_id_taken
            FROM fresh f LEFT JOIN taken t ON t.security_id = f.sec_id
        )
        SELECT vendor_security_id, security_id, false AS new_line FROM _p14_existing_lines
        UNION ALL
        SELECT vendor_security_id,
               CASE WHEN sec_id IS NOT NULL AND rk = 1 AND NOT sec_id_taken THEN sec_id
                    ELSE 'TBLTICKERHISTORY-' || vendor_security_id END,
               true
        FROM ranked
        """,
        [source],
    )


def _stage_rows(store: DuckDBStore, *, source: str, run_id: str, received_at: dt.datetime, loaded_at: dt.datetime,
                scale: int) -> None:
    """``_p14_new``: the partition's canonical rows, projected as the bulk loader projects them."""
    store.con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE _p14_new AS
        WITH {_RAW_CTE}, clocked AS (
            SELECT r.*, l.security_id, l.new_line,
                   greatest(CAST(r.trade_date AS TIMESTAMP) + INTERVAL 22 HOUR, CAST(? AS TIMESTAMP)) AS row_at
            FROM raw r
            JOIN _p14_lines l USING (vendor_security_id)
            WHERE r.vendor_id > 0 AND NOT (
                coalesce(r.volume < 0, false)
                OR coalesce(r.open <= 0, false) OR coalesce(r.high <= 0, false)
                OR coalesce(r.low <= 0, false) OR coalesce(r.close <= 0, false)
                OR coalesce(NOT isfinite(r.open), false) OR coalesce(NOT isfinite(r.high), false)
                OR coalesce(NOT isfinite(r.low), false) OR coalesce(NOT isfinite(r.close), false)
                OR coalesce(r.high < greatest(r.open, r.low, r.close), false)
                OR coalesce(r.low > least(r.open, r.high, r.close), false)
            )
        )
        SELECT ? AS source, security_id, vendor_security_id, symbol, trade_date, open, high, low, close,
               adjusted_close, volume, NULL::DOUBLE AS vwap, NULL::DOUBLE AS dividend_amount, split_factor,
               false AS is_adjusted, row_at AS available_at, ? AS run_id,
               CAST(? AS TIMESTAMP) AS source_loaded_at, CAST(row_at AS DATE) AS as_of_date,
               true AS is_latest_revision,
               -- A8: stored in shares (units) whatever the source unit.
               shares_outstanding * ? AS shares_outstanding, shares_outstanding * ? * close AS market_cap_usd,
               cumul_return_factor, return_factor, new_line
        FROM clocked
        QUALIFY count(*) OVER (PARTITION BY security_id, trade_date) = 1
        """,
        [received_at, source, run_id, loaded_at, scale, scale],
    )


_FACTOR = ("CASE WHEN b.close > 0 AND b.adjusted_close > 0 AND isfinite(b.adjusted_close / b.close) "
           "THEN b.adjusted_close / b.close END")


def _stage_basis(store: DuckDBStore, *, source: str, partition_date: dt.date, tolerance: float) -> None:
    """``_p14_calc``: each partition row's stored factor, series run, change flags and ledger multiple.

    Three bounded reads of ``equity_daily_bars``, none of them a sort: the
    partition lines' first / previous / next stored dates (a streaming
    aggregate over ~12K groups), the previous and next bars (a hash join built
    on those ~12K lines), and the stored rows of the partition date itself
    (pruned to that date).
    """
    con = store.con
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE _p14_span AS
        SELECT security_id, min(trade_date) AS first_date,
               max(trade_date) FILTER (WHERE trade_date < ?) AS prev_date,
               min(trade_date) FILTER (WHERE trade_date > ?) AS next_date
        FROM equity_daily_bars
        WHERE source = ? AND security_id IN (SELECT security_id FROM _p14_new)
        GROUP BY security_id
        """,
        [partition_date, partition_date, source],
    )
    con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE _p14_near AS
        SELECT b.security_id, b.trade_date, b.close, b.adjusted_close, b.run_id, {_FACTOR} AS factor
        FROM equity_daily_bars b
        JOIN _p14_span s ON s.security_id = b.security_id
        WHERE b.source = ? AND (b.trade_date = s.prev_date OR b.trade_date = s.next_date)
        """,
        [source],
    )
    con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE _p14_stored AS
        SELECT b.*, {_FACTOR} AS factor
        FROM equity_daily_bars b
        WHERE b.source = ? AND b.trade_date = ? AND b.security_id IN (SELECT security_id FROM _p14_new)
        """,
        [source, partition_date],
    )
    # The vendor multiple already recorded for this date (a re-delivered latest partition corrects it).
    if _table_exists(store, REBASES_TABLE):
        recorded = (f"SELECT security_id, product(multiplier) AS recorded FROM {REBASES_TABLE} "
                    "WHERE source = ? AND partition_date = ? GROUP BY security_id")
        recorded_params: list[Any] = [source, partition_date]
    else:
        recorded, recorded_params = "SELECT NULL::VARCHAR AS security_id, NULL::DOUBLE AS recorded LIMIT 0", []
    differs = " OR ".join(f"k.{name} IS DISTINCT FROM n.{name}" for name in _VALUE_COLUMNS)
    tol = float(tolerance)
    con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE _p14_calc AS
        WITH joined AS (
            SELECT n.security_id, n.trade_date, n.close, n.run_id AS partition_run, n.available_at AS row_at,
                   CASE WHEN n.cumul_return_factor > 0 AND isfinite(n.cumul_return_factor)
                        THEN n.cumul_return_factor END AS crf,
                   CASE WHEN n.return_factor > 0 AND isfinite(n.return_factor) THEN n.return_factor END AS rf,
                   s.security_id IS NOT NULL AS has_history, s.first_date, s.prev_date, s.next_date,
                   p.close AS prev_close, p.adjusted_close AS prev_adj, p.factor AS prev_factor, p.run_id AS prev_run,
                   x.factor AS next_factor, x.run_id AS next_run,
                   k.security_id IS NOT NULL AS key_exists, k.adjusted_close AS key_adj, k.factor AS key_factor,
                   k.run_id AS key_run, k.available_at AS key_available_at,
                   coalesce(k.security_id IS NOT NULL AND ({differs}), false) AS raw_changed,
                   coalesce(r.recorded, 1.0) AS recorded
            FROM _p14_new n
            LEFT JOIN _p14_span s ON s.security_id = n.security_id
            LEFT JOIN _p14_near p ON p.security_id = n.security_id AND p.trade_date = s.prev_date
            LEFT JOIN _p14_near x ON x.security_id = n.security_id AND x.trade_date = s.next_date
            LEFT JOIN _p14_stored k ON k.security_id = n.security_id
            LEFT JOIN ({recorded}) r ON r.security_id = n.security_id
        ), moded AS (
            SELECT *,
                   CASE WHEN NOT has_history THEN 'new_line'
                        WHEN next_date IS NULL AND prev_date IS NOT NULL THEN 'extend'
                        WHEN next_date IS NULL THEN 'only_bar'
                        WHEN key_exists THEN 'older_key'
                        ELSE 'gap_fill' END AS mode
            FROM joined
        ), based AS (
            SELECT *,
                   CASE mode
                       WHEN 'new_line' THEN crf
                       WHEN 'extend' THEN CASE WHEN prev_factor IS NULL THEN crf
                                               WHEN rf IS NULL THEN prev_factor
                                               ELSE prev_factor / rf END
                       WHEN 'only_bar' THEN coalesce(key_factor, crf)
                       WHEN 'older_key' THEN key_factor
                       ELSE coalesce(prev_factor, next_factor, crf) END AS factor,
                   CASE mode
                       WHEN 'new_line' THEN partition_run
                       WHEN 'extend' THEN prev_run
                       WHEN 'gap_fill' THEN CASE WHEN prev_date IS NOT NULL THEN prev_run ELSE next_run END
                       ELSE key_run END AS series_run,
                   mode = 'extend' AND (prev_factor IS NULL OR rf IS NULL) AS unverifiable,
                   CASE WHEN mode = 'extend' AND prev_factor IS NOT NULL THEN crf * rf / recorded END AS multiple
            FROM moded
        )
        SELECT *,
               CASE WHEN factor > 0 AND isfinite(close * factor) THEN close * factor END AS adj_new,
               NOT key_exists AS is_new,
               key_exists AND (raw_changed OR NOT coalesce(
                   abs((CASE WHEN factor > 0 AND isfinite(close * factor) THEN close * factor END) / key_adj - 1)
                       <= {tol!r},
                   factor IS NULL AND key_adj IS NULL)) AS is_changed,
               coalesce(abs(multiple - 1) > {tol!r}, false) AS rebased,
               crf IS NOT NULL AND abs(crf - 1) > {tol!r} AS off_convention
        FROM based
        """,
        recorded_params,
    )


def _publish_new_lines(store: DuckDBStore, *, source: str, run_id: str, partition_date: dt.date,
                       received_at: dt.datetime) -> int:
    """Link rows for vendor lines first seen in this partition; extend every line's last seen date."""
    con = store.con
    # Non-key columns only, and only rows that move (a replayed UPDATE of non-indexed columns is safe).
    con.execute(
        """
        UPDATE securities AS s SET last_seen_date = greatest(s.last_seen_date, ?), active = true
        FROM (SELECT DISTINCT security_id FROM _p14_new) n
        WHERE s.security_id = n.security_id AND s.source = ?
          AND (s.last_seen_date IS NULL OR s.last_seen_date < ? OR NOT s.active)
        """,
        [partition_date, source, partition_date],
    )
    new = con.execute("SELECT count(DISTINCT security_id) FROM _p14_new WHERE new_line").fetchone()
    con.execute(
        """
        INSERT INTO securities (security_id, issuer_id, primary_symbol, name, asset_class, country, currency, active,
                                first_seen_date, last_seen_date, source)
        SELECT security_id, NULL, min(symbol), min(symbol), 'EQUITY', 'US', 'USD', true, ?, ?, ?
        FROM _p14_new n
        WHERE new_line AND NOT EXISTS (SELECT 1 FROM securities s WHERE s.security_id = n.security_id)
        GROUP BY security_id
        """,
        [partition_date, partition_date, source],
    )
    con.execute(
        """
        INSERT INTO security_identifier_history (security_id, id_type, id_value, valid_from, valid_to, as_of_date,
                                                 available_at, source, run_id)
        SELECT DISTINCT security_id, ?, vendor_security_id, ?, NULL, ?, CAST(? AS TIMESTAMP), ?, ?
        FROM _p14_new WHERE new_line
        """,
        [TBLTICKERHISTORY_ID_TYPE, partition_date, partition_date, received_at, source, run_id],
    )
    con.execute(
        """
        INSERT INTO exchange_listings (security_id, ticker, exchange_code, mic, currency, valid_from, valid_to,
                                       as_of_date, available_at, source, run_id)
        SELECT security_id, min(symbol), NULL, NULL, 'USD', ?, NULL, ?, CAST(? AS TIMESTAMP), ?, ?
        FROM _p14_new WHERE new_line GROUP BY security_id
        """,
        [partition_date, partition_date, received_at, source, run_id],
    )
    return int(new[0]) if new else 0


def _count(store: DuckDBStore, sql: str) -> int:
    row = store.con.execute(sql).fetchone()
    return int(row[0]) if row and row[0] is not None else 0


def ingest_ticker_history_partition(store: DuckDBStore, options: PartitionIngestOptions) -> PartitionIngestResult:
    """Apply one TickerHistory3 date partition (see the module docstring); idempotent per sha256."""
    path = options.partition_path
    if not path.is_file():
        raise FileNotFoundError(path)
    sha = file_sha256(path)
    if options.received_at is not None:
        received_at, receipt_basis = options.received_at, "caller"
    else:
        received_at, receipt_basis = _receipt(path, sha)
    try:
        stage_source(store.con, str(path))
        diagnostics = source_diagnostics(store.con)
        dates = store.con.execute(
            "SELECT min(trade_date), max(trade_date) FROM ticker_history_source_rows"
        ).fetchone()
        if dates is None or dates[0] is None or dates[0] != dates[1]:
            raise ValueError(f"a partition must hold exactly one trade date, {path} holds {dates}")
        partition_date: dt.date = dates[0]
        mark = _watermark(store, _watermark_name(options.source, partition_date))
        if mark is not None and mark.get("sha256") == sha:
            return PartitionIngestResult(partition_date, STATUS_UNCHANGED, mark.get("run_id"), sha, received_at,
                                         detail={"watermark": mark})
        latest = _watermark(store, _latest_name(options.source))
        latest_date = dt.date.fromisoformat(str(latest["partition_date"])) if latest else None
        if mark is None and latest_date is not None and partition_date < latest_date:
            raise OutOfOrderPartitionError(
                f"partition {partition_date} is before the latest applied partition {latest_date} and was never "
                "applied; partitions are applied in date order"
            )
        run_id = options.run_id or f"ticker-history-partition-{partition_date.isoformat()}-{uuid.uuid4()}"
        return _apply(store, options, path=path, sha=sha, received_at=received_at, receipt_basis=receipt_basis,
                      partition_date=partition_date, latest_date=latest_date, restatement_of=mark, run_id=run_id,
                      diagnostics=diagnostics)
    finally:
        _drop_temporaries(store)


def _apply(store: DuckDBStore, options: PartitionIngestOptions, *, path: Path, sha: str, received_at: dt.datetime,
           receipt_basis: str, partition_date: dt.date, latest_date: dt.date | None,
           restatement_of: dict[str, Any] | None, run_id: str, diagnostics: dict[str, Any]) -> PartitionIngestResult:
    source = options.source
    con = store.con
    loaded_at = now_utc_naive()
    scale, raw_unit, unit_decision = _unit_decision(store, path)
    _map_lines(store, source)
    _stage_rows(store, source=source, run_id=run_id, received_at=received_at, loaded_at=loaded_at, scale=scale)
    lines = _count(store, "SELECT count(DISTINCT security_id) FROM _p14_new")
    if lines < options.minimum_partition_lines:
        raise RuntimeError(f"partition {partition_date} has {lines:,} valid lines < {options.minimum_partition_lines:,}")
    _stage_basis(store, source=source, partition_date=partition_date, tolerance=options.rebase_tolerance)
    counts = con.execute(
        """
        SELECT count(*) FILTER (WHERE is_new), count(*) FILTER (WHERE is_changed), count(*) FILTER (WHERE rebased),
               count(*) FILTER (WHERE unverifiable), count(*) FILTER (WHERE is_new AND mode = 'gap_fill'),
               count(*) FILTER (WHERE off_convention)
        FROM _p14_calc
        """
    ).fetchone()
    appended, restated, rebase_lines, unverifiable, gap_fills, off_convention = (int(v) for v in counts or (0,) * 6)
    missing = {
        name: ddl
        for name, ddl, needed in ((REBASES_TABLE, REBASES_TABLE_DDL, rebase_lines),
                                  (REVISIONS_TABLE, REVISIONS_TABLE_DDL, restated))
        if needed and not _table_exists(store, name)
    }
    if missing and not options.allow_unmigrated_revisions_table:
        raise RevisionsTableNotMigratedError(
            f"partition {partition_date} rebases {rebase_lines:,} lines and restates {restated:,} rows, which "
            f"needs {', '.join(missing)}; not formalized (migration 0328). Nothing was written"
        )
    # The first write: the A8 unit check's quality row (it raises on a conclusive failure, and that row stays).
    share_units = vendor_share_unit_check(
        store, table="_p14_new", source=source, run_id=run_id, unit=raw_unit, min_pairs=options.shares_unit_min_pairs
    )

    def staged(overrides: dict[str, str]) -> str:  # a partition row (``n``) with some columns replaced
        return ", ".join(f"{overrides[c]} AS {c}" if c in overrides else f"n.{c}" for c in BAR_COLUMNS)

    if restated:
        # A restated row is available no earlier than the row it supersedes (that row's superseded_at).
        restated_at = "greatest(n.available_at, coalesce(c.key_available_at, n.available_at))"
        con.execute(
            f"""
            CREATE OR REPLACE TEMP TABLE _p14_restated AS
            SELECT {staged({'adjusted_close': 'c.adj_new', 'run_id': 'c.key_run',
                            'available_at': restated_at, 'as_of_date': f'CAST({restated_at} AS DATE)'})}
            FROM _p14_new n JOIN _p14_calc c ON c.security_id = n.security_id AND c.is_changed
            """
        )
    rebased_examples = [
        {"security_id": row[0], "first_affected_date": str(row[1]), "last_affected_date": str(row[2]),
         "multiplier": row[3]}
        for row in con.execute(
            "SELECT security_id, first_date, prev_date, multiple FROM _p14_calc WHERE rebased "
            "ORDER BY security_id LIMIT 50"
        ).fetchall()
    ]
    summary: dict[str, Any] = {
        "partition_date": partition_date.isoformat(), "sha256": sha, "run_id": run_id,
        "received_at": received_at.isoformat(), "receipt_basis": receipt_basis, "source_path": str(path),
        "appended_rows": appended, "restated_rows": restated, "rebased_lines": rebase_lines,
    }
    warnings = {"rebase_unverifiable_lines": unverifiable, "gap_fill_rows": gap_fills,
                "off_convention_lines": off_convention}
    with store.transaction():  # a failure rolls back every write below; nothing is left half-applied
        for ddl in missing.values():
            con.execute(ddl)
        if restated:
            # Supersede, then a small keyed replace on the partition's one trade date.
            superseded = ", ".join("false" if c == "is_latest_revision" else f"k.{c}" for c in BAR_COLUMNS)
            con.execute(
                f"""
                INSERT INTO {REVISIONS_TABLE} ({_COLUMNS}, superseded_at, superseded_by_run_id, revision_reason)
                SELECT {superseded}, r.available_at, ?, ?
                FROM _p14_stored k JOIN _p14_restated r ON r.security_id = k.security_id
                """,
                [run_id, REVISION_REASON_RESTATEMENT],
            )
            con.execute(
                "DELETE FROM equity_daily_bars WHERE source = ? AND trade_date = ? "
                "AND security_id IN (SELECT security_id FROM _p14_restated)",
                [source, partition_date],
            )
            con.execute(f"INSERT INTO equity_daily_bars ({_COLUMNS}) SELECT {_COLUMNS} FROM _p14_restated")
        if rebase_lines:
            # Ledger only: one row per rebased line; no stored bar changes.
            con.execute(
                f"""
                INSERT INTO {REBASES_TABLE} (source, security_id, partition_date, available_at, multiplier,
                                             first_affected_date, last_affected_date, detection_basis,
                                             cumul_return_factor, return_factor, prior_close, prior_adjusted_close,
                                             run_id, source_loaded_at)
                SELECT ?, security_id, trade_date, row_at, multiple, first_date, prev_date, ?,
                       crf, rf, prev_close, prev_adj, ?, CAST(? AS TIMESTAMP)
                FROM _p14_calc WHERE rebased
                """,
                [source, REBASE_DETECTION_BASIS, run_id, loaded_at],
            )
        con.execute(
            f"""
            INSERT INTO equity_daily_bars ({_COLUMNS})
            SELECT {staged({'adjusted_close': 'c.adj_new', 'run_id': 'c.series_run'})}
            FROM _p14_new n JOIN _p14_calc c ON c.security_id = n.security_id AND c.is_new
            """
        )
        summary["new_lines"] = _publish_new_lines(store, source=source, run_id=run_id, partition_date=partition_date,
                                                  received_at=received_at)
        finished_at = now_utc_naive()
        _set_watermark(store, _watermark_name(source, partition_date), summary, finished_at)
        if latest_date is None or partition_date > latest_date:
            _set_watermark(store, _latest_name(source), {"partition_date": partition_date.isoformat(),
                                                         "run_id": run_id, "sha256": sha}, finished_at)
        _record_partition_file(store, path=path, sha=sha, at=finished_at, metadata={
            "mode": "incremental_partition", "partition_date": partition_date.isoformat(), "run_id": run_id,
            "received_at": received_at.isoformat(), "receipt_basis": receipt_basis})
        con.execute(
            "INSERT INTO dataset_runs (run_id, dataset_id, status, started_at, finished_at, rows_loaded, source, "
            "params_json) VALUES (?, ?, 'succeeded', ?, ?, ?, ?, ?)",
            [run_id, DATASET_ID, loaded_at, finished_at, appended + restated, source, json.dumps({
                "source_path": str(path), "partition_date": partition_date.isoformat(), "sha256": sha,
                "received_at": received_at.isoformat(), "receipt_basis": receipt_basis, "shares_unit": raw_unit,
                "mode": "incremental_partition",
            }, sort_keys=True)],
        )
        detail = {
            **summary, **warnings,
            "lines": lines,
            "restatement_of": restatement_of,
            "rebased_examples": rebased_examples,
            "rebase_tolerance": options.rebase_tolerance,
            "share_unit_decision": unit_decision,
            "share_units": share_units,
            "source_diagnostics": diagnostics,
            "provenance": {**source_provenance(path), "availability_policy":
                           "partition receipt clock; stored bars never rebased (factor continued through "
                           "returnFactor); vendor rebases in equity_adjustment_rebases"},
        }
        quality_check(
            store, dataset_id=DATASET_ID, table_name="equity_daily_bars", check_name="incremental_partition_applied",
            status="warning" if any(warnings.values()) else "passed", severity="warning",
            observed_value=float(appended), threshold_value=float(options.minimum_partition_lines), details=detail,
        )
    _checkpoint(store)
    LOGGER.info("applied ticker-history partition %s: %s", partition_date, summary)
    return PartitionIngestResult(partition_date, STATUS_APPLIED, run_id, sha, received_at, appended_rows=appended,
                                 restated_rows=restated, rebased_lines=rebase_lines,
                                 new_lines=summary["new_lines"], detail=detail)


def _known_at(alias: str) -> str:
    return f"coalesce({alias}.available_at, CAST({alias}.trade_date AS TIMESTAMP) + INTERVAL 22 HOUR)"


def bars_asof_sql(store: DuckDBStore, *, basis: str = BASIS_VENDOR) -> str:
    """Point-in-time bars: one row per (source, security_id, trade_date) as known at a cutoff.

    One ``?`` placeholder: the cutoff ``TIMESTAMP``. The version: the
    ``equity_daily_bars`` row when it is available at the cutoff, else the
    newest superseded row (``equity_daily_bar_revisions``, when present)
    available then, so a restatement is never visible before its receipt.
    Stored adjusted closes never change once written (the module docstring),
    so no later dividend or split leaks into them.

    ``basis``: ``vendor`` (default) applies the rebase multiplier at read time:
    each line's adjusted closes are rescaled by ``close / adjusted_close`` of
    its latest bar known at the cutoff, the product of the vendor rebases
    recorded since the line's anchor and known by then, giving the vendor's own
    convention as of the cutoff (factor 1.0 on the line's latest known bar;
    before the cutoff's later rebases). ``stored`` returns the stored values,
    whose level is relative to the line's anchor; ratios within a line are
    identical under both. Neither reads the full table through a sort: the
    bars are a filtered scan, the revisions branch is built on the (small)
    revisions table, and the vendor scale is a streaming aggregate per line.
    """
    if basis not in (BASIS_VENDOR, BASIS_STORED):
        raise ValueError(f"basis must be {BASIS_VENDOR!r} or {BASIS_STORED!r}, got {basis!r}")
    columns = ", ".join(f"b.{name}" for name in BAR_COLUMNS)
    ctes = ["params AS (SELECT CAST(? AS TIMESTAMP) AS cutoff)"]
    known = [f"SELECT {columns} FROM equity_daily_bars b, params WHERE {_known_at('b')} <= params.cutoff"]
    if _table_exists(store, REVISIONS_TABLE):
        keys = "{a}.source = {b}.source AND {a}.security_id = {b}.security_id AND {a}.trade_date = {b}.trade_date"
        ctes.append(f"""rev AS (
    SELECT b.*, {_known_at('b')} AS known_at FROM {REVISIONS_TABLE} b, params WHERE {_known_at('b')} <= params.cutoff
)""")
        ctes.append(f"""rev_blocked AS (
    SELECT DISTINCT r.source, r.security_id, r.trade_date
    FROM rev r JOIN equity_daily_bars c ON {keys.format(a='c', b='r')}, params
    WHERE {_known_at('c')} <= params.cutoff
)""")
        known.append(f"""SELECT {columns} FROM (
    SELECT r.*, row_number() OVER (PARTITION BY r.source, r.security_id, r.trade_date
                                   ORDER BY r.known_at DESC, r.superseded_at DESC, r.source_loaded_at DESC) AS rn
    FROM rev r LEFT JOIN rev_blocked x ON {keys.format(a='x', b='r')}
    WHERE x.source IS NULL
) b WHERE b.rn = 1""")
    ctes.append("known AS NOT MATERIALIZED (\n" + "\nUNION ALL\n".join(known) + "\n)")
    if basis == BASIS_STORED:
        return "WITH " + ",\n".join(ctes) + f"\nSELECT {_COLUMNS} FROM known"
    ctes.append(f"""scale AS (
    SELECT b.source, b.security_id,
           arg_max(b.close / b.adjusted_close, b.trade_date) FILTER (WHERE {_FACTOR} IS NOT NULL) AS to_vendor
    FROM known b GROUP BY b.source, b.security_id
)""")
    select = ", ".join("k.adjusted_close * s.to_vendor AS adjusted_close" if name == "adjusted_close"
                       else f"k.{name}" for name in BAR_COLUMNS)
    return ("WITH " + ",\n".join(ctes) + f"\nSELECT {select}\nFROM known k\n"
            "LEFT JOIN scale s ON s.source = k.source AND s.security_id = k.security_id")


def pending_partitions(
    store: DuckDBStore, files: Sequence[PartitionFile], *, source: str = SOURCE_NAME
) -> list[PartitionFile]:
    """The given partition files not yet applied with their current bytes, in trade-date order."""
    pending: list[tuple[dt.date, PartitionFile]] = []
    for item in files:
        first, _last = partition_trade_dates(store, item.path)
        if first is None:
            continue
        mark = _watermark(store, _watermark_name(source, first))
        if mark is None or mark.get("sha256") != file_sha256(item.path):
            pending.append((first, item))
    return [item for _date, item in sorted(pending, key=lambda pair: pair[0])]


def ingest_directory_files(
    store: DuckDBStore, files: Sequence[DirectoryFile], *, cutoff: dt.date | None, run_id: str | None = None
) -> list[dict[str, Any]]:
    """Append retained Nasdaq Trader files under the A1 pinned-snapshot policy.

    Each file is dated by its own ``File Creation Time`` trailer, carries its
    original receipt (``<file>.receipt.json``, else mtime) as ``available_at``,
    and supersedes, never deletes, earlier rows. An unchanged file is a no-op.
    """
    from .symbol_directory import SnapshotReceipt, load_directory_snapshot, load_listing_events_snapshot

    loads: list[dict[str, Any]] = []
    for item in files:
        payload = item.path.read_bytes()
        sha = file_sha256(item.path)
        received_at, basis = _receipt(item.path, sha)
        receipt = SnapshotReceipt(received_at, basis, now_utc_naive())
        text = payload.decode("utf-8", errors="replace")
        with store.transaction():
            if item.kind == "listing_events":
                load = load_listing_events_snapshot(
                    store, source_url=item.source_url, text=text, sha256=sha, cutoff=cutoff, run_id=run_id,
                    receipt=receipt, cache_path=item.path, byte_count=len(payload),
                )
            elif item.kind in ("nasdaqlisted", "otherlisted"):
                load = load_directory_snapshot(
                    store, directory=item.kind, source_url=item.source_url, text=text, sha256=sha, cutoff=cutoff,
                    run_id=run_id, receipt=receipt, cache_path=item.path, byte_count=len(payload),
                )
            else:
                raise ValueError(f"unknown directory file kind {item.kind!r}")
        loads.append({"kind": item.kind, "path": str(item.path), **load.as_detail()})
    return loads


def refresh_daily_prices(store: DuckDBStore, request: DailyRefreshRequest) -> DailyRefreshResult:
    """The C12 hand-off: apply the given partitions in date order, then the directory files.

    Stops at the first failing partition (later dates must not be applied past a
    gap); already-applied partitions are no-ops. No network access.
    """
    results: list[PartitionIngestResult] = []
    for item in pending_partitions(store, request.partitions, source=request.source):
        results.append(ingest_ticker_history_partition(store, PartitionIngestOptions(
            partition_path=item.path, received_at=item.received_at, source=request.source,
            minimum_partition_lines=request.minimum_partition_lines,
            allow_unmigrated_revisions_table=request.allow_unmigrated_revisions_table,
        )))
    directory = ingest_directory_files(store, request.directory_files, cutoff=request.directory_cutoff)
    return DailyRefreshResult(tuple(results), tuple(directory))
