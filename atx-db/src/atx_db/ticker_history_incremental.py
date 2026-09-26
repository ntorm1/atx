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
    the bulk loader projects them (``adjusted_close = close x
    cumulReturnFactor``, invalid OHLCV excluded, duplicate vendor keys
    quarantined). New keys are appended; a key already stored with different
    raw values (a re-delivered, corrected partition) is restated, keeping the
    stored adjustment factor (the basis moves only through rebases). Rows of vendor
    lines not yet in the table get the bulk loader's line id (current SEC
    ticker map, else ``TBLTICKERHISTORY-<vendor id>``).

Vendor factor rebases
    The vendor's ``cumulReturnFactor`` is backward-anchored: it is 1.0 on the
    latest date for every line (measured: 12,489 of 12,489 lines on 2026-09-18),
    and each new dividend or split restates every earlier factor of that line
    by one common multiple. The partition's own ``returnFactor`` links its day
    to the line's previous one (``crf_prev = crf_day x returnFactor``, the
    vendor recurrence checked in :mod:`atx_db.ticker_history_quality`). So the
    stored history's basis multiple is ``m = crf_day x returnFactor x close_prev
    / adjusted_close_prev`` (previous = the line's latest stored bar before the
    partition date). When ``|m - 1| > rebase_tolerance``, that line's stored
    adjusted history moves to the new basis in place (``adjusted_close x m``),
    and one row is recorded in the rebase ledger ``equity_adjustment_rebases``:
    (line, receipt clock, ``multiplier`` m, first and last affected trade
    date, detection evidence). Raw prices, volume, share counts and every
    bar's own clock (``available_at``, ``as_of_date``, ``run_id``) do not
    change, and neither does any return. Only that line is touched. Detection
    runs only for lines with no stored bar after the partition date (a
    re-delivered older date never rebases).

Revisions and point-in-time reads
    ``equity_daily_bars`` keeps exactly one row per (source, security_id,
    trade_date). Its raw fields and clocks are as first published; its
    adjusted fields hold the latest basis. A factor-only revision is one ledger
    row per (line, rebase), never a copy of history. A restatement of raw values
    (a re-delivered, corrected partition; rare) copies the superseded row first
    into ``equity_daily_bar_revisions`` (``is_latest_revision = false``,
    ``superseded_at``, ``superseded_by_run_id``, ``revision_reason``), and the
    restated row carries the partition's receipt clock as ``available_at``.
    Nothing is deleted. :func:`bars_asof_sql` reconstructs the bars as known
    at a cutoff: the newest version available then, with the adjusted close
    divided by every later rebase's multiplier. Backtests that need pre-rebase
    adjusted prices must read through it. Both tables await formalization by
    migration 0328 (:data:`REBASES_TABLE_DDL`, :data:`REVISIONS_TABLE_DDL`).
    Until then they are created only when the caller passes
    ``allow_unmigrated_revisions_table=True`` (non-production). Otherwise a
    partition that needs one is refused before any write
    (:class:`RevisionsTableNotMigratedError`).

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
from .warehouse import file_sha256, now_utc_naive, quality_check, record_source_file

LOGGER = logging.getLogger(__name__)

__all__ = [
    "APPROVED_USER_AGENT",
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
#: single precision (~6e-8 relative); a 0.01 % dividend moves m by 1e-4.
REBASE_TOLERANCE = 1e-6
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
#: Values that make a stored row differ from a re-delivered one. The adjusted
#: close is not among them: a re-delivered day's factor is on the basis of its
#: delivery, and the basis only moves through the rebase ledger.
#: A restated row keeps the stored factor (``adjusted_close / close``).
_VALUE_COLUMNS = ("vendor_security_id", "symbol", "open", "high", "low", "close", "volume",
                  "shares_outstanding", "market_cap_usd")

#: The rebase ledger: one row per (line, vendor factor rebase). Stored
#: ``adjusted_close`` of the line's bars dated first..last_affected_date is on
#: the basis after every ledger row; the value known at a cutoff before
#: ``available_at`` is the stored value / ``multiplier``. For migration 0328 to
#: formalize (created only with ``allow_unmigrated_revisions_table``). Key
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

#: The superseded-revision history table, for raw-value restatements only
#: (factor-only revisions go to the rebase ledger). For migration 0328 to
#: formalize (created only with ``allow_unmigrated_revisions_table``). No
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
    "_p14_existing_lines", "_p14_lines", "_p14_new", "_p14_diff", "_p14_rebase", "_p14_restated",
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
    rebased_lines: int = 0
    rebased_rows: int = 0
    new_lines: int = 0
    detail: dict[str, Any] = field(default_factory=dict)

    @property
    def changed_rows(self) -> int:
        return self.appended_rows + self.restated_rows + self.rebased_rows


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


def _set_watermark(store: DuckDBStore, name: str, value: dict[str, Any]) -> None:
    store.con.execute(
        "DELETE FROM dataset_watermarks WHERE dataset_id = ? AND watermark_name = ?", [DATASET_ID, name]
    )
    store.con.execute(
        "INSERT INTO dataset_watermarks (dataset_id, watermark_name, watermark_value, updated_at) "
        "VALUES (?, ?, ?, now())",
        [DATASET_ID, name, json.dumps(value, sort_keys=True, default=str)],
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
    _create_symbol_map(store)
    # A new line takes its current SEC ticker's id unless another line of this
    # source already uses it (or an earlier new line does): then its vendor id.
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
        ), ranked AS (
            SELECT f.*, row_number() OVER (PARTITION BY sec_id ORDER BY vendor_security_id) AS rk,
                   EXISTS (SELECT 1 FROM equity_daily_bars b WHERE b.source = ? AND b.security_id = f.sec_id)
                       AS sec_id_taken
            FROM fresh f
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


def _stage_changes(store: DuckDBStore, source: str, tolerance: float) -> None:
    """``_p14_diff`` (new / restated keys) and ``_p14_rebase`` (per-line basis multiple)."""
    differs = " OR ".join(f"b.{name} IS DISTINCT FROM n.{name}" for name in _VALUE_COLUMNS)
    store.con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE _p14_diff AS
        SELECT n.security_id, n.trade_date, b.security_id IS NULL AS is_new,
               coalesce(b.security_id IS NOT NULL AND ({differs}), false) AS is_changed
        FROM _p14_new n
        LEFT JOIN equity_daily_bars b
          ON b.source = n.source AND b.security_id = n.security_id AND b.trade_date = n.trade_date
        """
    )
    # m = crf_day x returnFactor x close_prev / adjusted_close_prev over the line's
    # latest stored bar before the partition date. Only for lines with no stored
    # bar after it: the partition's crf is on the basis of its own date, so an
    # older date's crf says nothing about the line's later bars.
    store.con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE _p14_rebase AS
        WITH span AS (
            SELECT security_id, min(trade_date) AS first_date, max(trade_date) AS last_date
            FROM equity_daily_bars
            WHERE source = ? AND security_id IN (SELECT security_id FROM _p14_new)
            GROUP BY security_id
        ), prior AS (
            SELECT n.security_id, n.trade_date AS partition_date, n.available_at AS rebase_at,
                   n.cumul_return_factor AS crf, n.return_factor AS rf, s.first_date,
                   p.trade_date AS prior_date, p.close AS prior_close, p.adjusted_close AS prior_adj
            FROM _p14_new n
            JOIN span s ON s.security_id = n.security_id AND s.last_date <= n.trade_date
            ASOF JOIN (SELECT security_id, trade_date, close, adjusted_close
                       FROM equity_daily_bars WHERE source = ?) p
              ON p.security_id = n.security_id AND n.trade_date > p.trade_date
        )
        SELECT *,
               CASE WHEN crf > 0 AND rf > 0 AND prior_close > 0 AND prior_adj > 0
                    THEN crf * rf * prior_close / prior_adj END AS multiple,
               coalesce(abs(crf * rf * prior_close / prior_adj - 1) > {float(tolerance)!r}, false)
                   AND crf > 0 AND rf > 0 AND prior_close > 0 AND prior_adj > 0 AS rebased
        FROM prior
        """,
        [source, source],
    )


def _supersede(store: DuckDBStore, *, where: str, params: list[Any], superseded_at: dt.datetime, run_id: str,
               reason: str) -> int:
    """Copy the latest rows matching ``where`` (alias ``b``) into the revisions table as superseded."""
    columns = ", ".join(f"b.{name}" if name != "is_latest_revision" else "false" for name in BAR_COLUMNS)
    before = store.con.execute(f"SELECT count(*) FROM {REVISIONS_TABLE}").fetchone()
    store.con.execute(
        f"""
        INSERT INTO {REVISIONS_TABLE} ({_COLUMNS}, superseded_at, superseded_by_run_id, revision_reason)
        SELECT {columns}, greatest(CAST(? AS TIMESTAMP), coalesce(b.available_at, CAST(? AS TIMESTAMP))), ?, ?
        FROM equity_daily_bars b
        WHERE {where}
        """,
        [superseded_at, superseded_at, run_id, reason, *params],
    )
    after = store.con.execute(f"SELECT count(*) FROM {REVISIONS_TABLE}").fetchone()
    return int(after[0] if after else 0) - int(before[0] if before else 0)


def _publish_new_lines(store: DuckDBStore, *, source: str, run_id: str, partition_date: dt.date,
                       received_at: dt.datetime) -> int:
    """Link rows for vendor lines first seen in this partition; extend every line's last seen date."""
    con = store.con
    con.execute(
        """
        UPDATE securities AS s SET last_seen_date = greatest(s.last_seen_date, ?), active = true
        FROM (SELECT DISTINCT security_id FROM _p14_new) n
        WHERE s.security_id = n.security_id AND s.source = ?
        """,
        [partition_date, source],
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
    loaded_at = now_utc_naive()
    scale, raw_unit, unit_decision = _unit_decision(store, path)
    _map_lines(store, source)
    _stage_rows(store, source=source, run_id=run_id, received_at=received_at, loaded_at=loaded_at, scale=scale)
    lines = _count(store, "SELECT count(DISTINCT security_id) FROM _p14_new")
    if lines < options.minimum_partition_lines:
        raise RuntimeError(f"partition {partition_date} has {lines:,} valid lines < {options.minimum_partition_lines:,}")
    _stage_changes(store, source, options.rebase_tolerance)
    restated = _count(store, "SELECT count(*) FROM _p14_diff WHERE is_changed")
    rebase_lines = _count(store, "SELECT count(*) FROM _p14_rebase WHERE rebased")
    unverifiable = _count(store, "SELECT count(*) FROM _p14_rebase WHERE multiple IS NULL")
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
    # The first write: the A8 unit check's quality row (it raises on a conclusive failure).
    share_units = vendor_share_unit_check(
        store, table="_p14_new", source=source, run_id=run_id, unit=raw_unit, min_pairs=options.shares_unit_min_pairs
    )
    for ddl in missing.values():
        store.con.execute(ddl)
    with store.transaction():  # a failure rolls back every write below, the run row included
        store.con.execute(
            "INSERT OR REPLACE INTO dataset_runs (run_id, dataset_id, status, started_at, source, params_json) "
            "VALUES (?, ?, 'running', current_timestamp, ?, ?)",
            [run_id, DATASET_ID, source, json.dumps({
                "source_path": str(path), "partition_date": partition_date.isoformat(), "sha256": sha,
                "received_at": received_at.isoformat(), "receipt_basis": receipt_basis, "shares_unit": raw_unit,
                "mode": "incremental_partition",
            }, sort_keys=True)],
        )
        restated_rows = rebased_rows = 0
        if restated:
            # The restated row keeps the stored adjustment factor (see _VALUE_COLUMNS).
            kept_factor = ", ".join(
                "CASE WHEN b.close > 0 AND b.adjusted_close > 0 THEN n.close * (b.adjusted_close / b.close) "
                "ELSE n.adjusted_close END" if c == "adjusted_close" else f"n.{c}"
                for c in BAR_COLUMNS
            )
            store.con.execute(
                f"""
                CREATE OR REPLACE TEMP TABLE _p14_restated AS
                SELECT {kept_factor}
                FROM _p14_new n
                JOIN _p14_diff d ON d.security_id = n.security_id AND d.trade_date = n.trade_date AND d.is_changed
                JOIN equity_daily_bars b
                  ON b.source = n.source AND b.security_id = n.security_id AND b.trade_date = n.trade_date
                """
            )
            where = ("b.source = ? AND EXISTS (SELECT 1 FROM _p14_diff d WHERE d.is_changed "
                     "AND d.security_id = b.security_id AND d.trade_date = b.trade_date)")
            restated_rows = _supersede(store, where=where, params=[source], superseded_at=received_at, run_id=run_id,
                                       reason=REVISION_REASON_RESTATEMENT)
            store.con.execute(f"DELETE FROM equity_daily_bars AS b WHERE {where}", [source])
            store.con.execute(f"INSERT INTO equity_daily_bars ({_COLUMNS}) SELECT * FROM _p14_restated")
        if rebase_lines:
            # One ledger row per rebased line; the bars move to the new basis in
            # place. Raw fields and every bar clock stay as first published.
            store.con.execute(
                f"""
                INSERT INTO {REBASES_TABLE} (source, security_id, partition_date, available_at, multiplier,
                                             first_affected_date, last_affected_date, detection_basis,
                                             cumul_return_factor, return_factor, prior_close, prior_adjusted_close,
                                             run_id, source_loaded_at)
                SELECT ?, security_id, partition_date, rebase_at, multiple, first_date, prior_date, ?,
                       crf, rf, prior_close, prior_adj, ?, CAST(? AS TIMESTAMP)
                FROM _p14_rebase WHERE rebased
                """,
                [source, REBASE_DETECTION_BASIS, run_id, loaded_at],
            )
            affected = ("r.rebased AND b.source = ? AND b.security_id = r.security_id "
                        "AND b.trade_date BETWEEN r.first_date AND r.prior_date")
            row = store.con.execute(
                f"SELECT count(*) FROM equity_daily_bars b, _p14_rebase r WHERE {affected}", [source]
            ).fetchone()
            rebased_rows = int(row[0]) if row else 0
            store.con.execute(
                "UPDATE equity_daily_bars AS b SET adjusted_close = b.adjusted_close * r.multiple "
                f"FROM _p14_rebase r WHERE {affected}",
                [source],
            )
        appended = _count(store, "SELECT count(*) FROM _p14_diff WHERE is_new")
        store.con.execute(
            f"INSERT INTO equity_daily_bars ({_COLUMNS}) SELECT {', '.join('n.' + c for c in BAR_COLUMNS)} "
            "FROM _p14_new n JOIN _p14_diff d USING (security_id, trade_date) WHERE d.is_new"
        )
        new_lines = _publish_new_lines(store, source=source, run_id=run_id, partition_date=partition_date,
                                       received_at=received_at)
        summary = {
            "partition_date": partition_date.isoformat(), "sha256": sha, "run_id": run_id,
            "received_at": received_at.isoformat(), "receipt_basis": receipt_basis, "source_path": str(path),
            "appended_rows": appended, "restated_rows": restated_rows, "rebased_lines": rebase_lines,
            "rebased_rows": rebased_rows, "new_lines": new_lines,
        }
        _set_watermark(store, _watermark_name(source, partition_date), summary)
        if latest_date is None or partition_date > latest_date:
            _set_watermark(store, _latest_name(source), {"partition_date": partition_date.isoformat(),
                                                         "run_id": run_id, "sha256": sha})
    rebased_detail = [
        {"security_id": row[0], "first_affected_date": str(row[1]), "last_affected_date": str(row[2]),
         "multiplier": row[3]}
        for row in store.con.execute(
            "SELECT security_id, first_date, prior_date, multiple FROM _p14_rebase WHERE rebased "
            "ORDER BY security_id LIMIT 50"
        ).fetchall()
    ]
    detail = {
        **summary,
        "lines": lines,
        "restatement_of": restatement_of,
        "rebase_unverifiable_lines": unverifiable,
        "rebased_examples": rebased_detail,
        "rebase_tolerance": options.rebase_tolerance,
        "share_unit_decision": unit_decision,
        "share_units": share_units,
        "source_diagnostics": diagnostics,
        "provenance": {**source_provenance(path), "availability_policy":
                       "partition receipt clock; factor rebases in the ledger, bar clocks unchanged"},
    }
    record_source_file(
        store, dataset_id=DATASET_ID, source_url=str(path), cache_path=path, status="available", sha256=sha,
        metadata={"mode": "incremental_partition", "partition_date": partition_date.isoformat(), "run_id": run_id,
                  "received_at": received_at.isoformat(), "receipt_basis": receipt_basis},
    )
    store.con.execute(
        "UPDATE dataset_runs SET status = 'succeeded', finished_at = current_timestamp, rows_loaded = ? "
        "WHERE run_id = ?",
        [appended + restated_rows, run_id],
    )
    quality_check(
        store, dataset_id=DATASET_ID, table_name="equity_daily_bars", check_name="incremental_partition_applied",
        status="warning" if unverifiable else "passed", severity="warning",
        observed_value=float(appended), threshold_value=float(options.minimum_partition_lines), details=detail,
    )
    LOGGER.info("applied ticker-history partition %s: %s", partition_date, summary)
    return PartitionIngestResult(partition_date, STATUS_APPLIED, run_id, sha, received_at, appended_rows=appended,
                                 restated_rows=restated_rows, rebased_lines=rebase_lines, rebased_rows=rebased_rows,
                                 new_lines=new_lines, detail=detail)


def bars_asof_sql(store: DuckDBStore) -> str:
    """Point-in-time bars: one row per (source, security_id, trade_date) as known at a cutoff.

    One ``?`` placeholder: the cutoff ``TIMESTAMP``. The version: among the
    latest rows (``equity_daily_bars``) and raw-value restatements
    (``equity_daily_bar_revisions``, when present), the newest with
    ``available_at <= cutoff``. Its adjusted close: the stored value divided by
    the multiplier of every ledger rebase (``equity_adjustment_rebases``, when
    present) of that line that covers the date, arrived after the cutoff, and
    was applied to that version (a superseded version holds the basis as of its
    ``superseded_at``). So neither a restatement nor a factor rebase is visible
    before its receipt, and the pre-rebase adjusted history known then is
    returned. Backtests that need pre-rebase adjusted history must read bars
    through this relation. ``equity_daily_bars`` alone holds the latest basis.
    """
    versions = [f"SELECT {_COLUMNS}, NULL::TIMESTAMP AS basis_through FROM equity_daily_bars"]
    if _table_exists(store, REVISIONS_TABLE):
        versions.append(f"SELECT {_COLUMNS}, superseded_at AS basis_through FROM {REVISIONS_TABLE}")
    known = f"""params AS (SELECT CAST(? AS TIMESTAMP) AS cutoff),
known AS (
    SELECT v.*
    FROM ({' UNION ALL '.join(versions)}) v, params
    WHERE coalesce(v.available_at, CAST(v.trade_date AS TIMESTAMP) + INTERVAL 22 HOUR) <= params.cutoff
    QUALIFY row_number() OVER (PARTITION BY v.source, v.security_id, v.trade_date
                               ORDER BY v.available_at DESC NULLS LAST, v.source_loaded_at DESC) = 1
)"""
    if not _table_exists(store, REBASES_TABLE):
        return f"WITH {known}\nSELECT {_COLUMNS} FROM known"
    select = ", ".join(
        "k.adjusted_close / coalesce(u.undo, 1.0) AS adjusted_close" if name == "adjusted_close" else f"k.{name}"
        for name in BAR_COLUMNS
    )
    return f"""WITH {known},
undo AS (
    SELECT k.source, k.security_id, k.trade_date, product(r.multiplier) AS undo
    FROM known k
    JOIN {REBASES_TABLE} r
      ON r.source = k.source AND r.security_id = k.security_id
     AND k.trade_date BETWEEN r.first_affected_date AND r.last_affected_date
    CROSS JOIN params
    WHERE r.available_at > params.cutoff
      AND (k.basis_through IS NULL OR r.available_at <= k.basis_through)
    GROUP BY k.source, k.security_id, k.trade_date
)
SELECT {select}
FROM known k
LEFT JOIN undo u ON u.source = k.source AND u.security_id = k.security_id AND u.trade_date = k.trade_date
"""


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
