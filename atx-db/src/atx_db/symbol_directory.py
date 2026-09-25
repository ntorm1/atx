"""Nasdaq Trader symbol-directory and Trading System Adds/Deletes ingestion.

Both sources are *pinned snapshots*: a file's own ``File Creation Time:`` trailer
dates its rows (``as_of_date``), an operator/report date only bounds which files
may be loaded (the cutoff), and ``available_at`` is when the file's bytes were
received (a retained file keeps its original receipt time; a reload does not
make old evidence look newly known, and ``source_loaded_at`` records the reload).
A reload never deletes evidence: rows superseded by a re-dated or newer load of the same
source keep their data and are marked ``is_latest_revision = false``.
"""

from __future__ import annotations

import contextlib
import datetime as dt
import hashlib
import io
import json
import uuid
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import pandas as pd
import requests

from .clock import resolve_as_of_date
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import insert_frame, now_utc_naive, quality_check, record_source_file, symbol_key

NASDAQ_LISTED_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt"
OTHER_LISTED_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt"
TRADING_SYSTEM_ADDS_DELETES_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/TradingSystemAddsDeletes.txt"
SOURCE_NAME = "Nasdaq Trader Symbol Directory"
LISTING_EVENTS_SOURCE_NAME = "Nasdaq Trader Trading System Adds/Deletes"

# The only user agent approved for SEC/Nasdaq requests from this project.
APPROVED_USER_AGENT = "atx-db/0.1 atx-research@example.com"

SYMBOL_DIRECTORY_DATASET_ID = "nasdaq_symbol_directory"
LISTING_EVENTS_DATASET_ID = "nasdaq_listing_events"

SOURCE_STATUS_LOADED = "loaded"
SOURCE_STATUS_UNCHANGED = "unchanged"
SOURCE_STATUS_EMPTY = "empty_snapshot"
SOURCE_STATUS_UNAVAILABLE = "source_unavailable_for_snapshot"

AS_OF_BASIS_FILE_CREATION_TIME = "file_creation_time"
AS_OF_BASIS_OPERATOR_CUTOFF = "operator_cutoff_no_file_creation_time"
# Symbols are resolved to securities through the *current* ticker maps; that is
# not historical identity evidence and is labeled as such everywhere it is used.
IDENTITY_BASIS_CURRENT_TICKER = "current_ticker_unverified"

# Nasdaq publishes both single-letter and spelled-out action codes. They are
# normalized exactly once, here, to the canonical lower-case vocabulary.
NASDAQ_ACTION_CANONICAL: dict[str, str] = {"a": "add", "add": "add", "d": "delete", "delete": "delete"}


@dataclass(frozen=True)
class NasdaqSymbolDirectoryOptions:
    nasdaq_url: str = NASDAQ_LISTED_URL
    other_url: str = OTHER_LISTED_URL
    # Report cutoff: a file created after this date is not loaded. It never
    # re-dates a file that carries its own File Creation Time.
    as_of_date: dt.date | None = None
    request_timeout: int = 60
    user_agent: str = APPROVED_USER_AGENT
    run_id: str | None = None


@dataclass(frozen=True)
class NasdaqListingEventsOptions:
    source_url: str = TRADING_SYSTEM_ADDS_DELETES_URL
    # Report cutoff (see NasdaqSymbolDirectoryOptions.as_of_date).
    as_of_date: dt.date | None = None
    request_timeout: int = 60
    user_agent: str = APPROVED_USER_AGENT
    run_id: str | None = None


class SnapshotAfterCutoffError(ValueError):
    """A pinned snapshot file was created, or received, after the requested report cutoff."""


def cutoff_end(cutoff: dt.date) -> dt.datetime:
    """Last instant of the cutoff day: the knowledge bound for ``available_at``."""
    return dt.datetime.combine(cutoff, dt.time.max)


RECEIPT_BASIS_NETWORK_DOWNLOAD = "network_download"
RECEIPT_BASIS_CACHE_RECEIPT = "cache_receipt"
RECEIPT_BASIS_FILE_MTIME = "file_mtime"


@dataclass(frozen=True)
class SnapshotReceipt:
    """When a snapshot file's bytes were received, how that is known, and this load's time.

    ``received_at`` drives row ``available_at``: a fresh download's own receipt time,
    or -- for a retained cache file -- its recorded receipt (``cache_receipt``) or,
    failing that, its file modification time (``file_mtime``). ``loaded_at`` is the
    time of this (re)load and is recorded only as load metadata.
    """

    received_at: dt.datetime
    basis: str
    loaded_at: dt.datetime


@dataclass(frozen=True)
class SnapshotDating:
    as_of_date: dt.date
    source_file_created_at: dt.datetime | None
    as_of_basis: str


@dataclass(frozen=True)
class SnapshotLoad:
    """Outcome of loading one pinned snapshot file."""

    source_url: str
    status: str
    as_of_date: dt.date | None
    source_file_created_at: dt.datetime | None
    as_of_basis: str | None
    rows_inserted: int = 0
    latest_rows: int = 0
    superseded_rows: int = 0
    detail: dict[str, Any] = field(default_factory=dict)

    def as_detail(self) -> dict[str, Any]:
        return {
            "source_url": self.source_url,
            "source_status": self.status,
            "as_of_date": self.as_of_date.isoformat() if self.as_of_date else None,
            "source_file_created_at": (
                self.source_file_created_at.isoformat() if self.source_file_created_at else None
            ),
            "as_of_basis": self.as_of_basis,
            "rows_inserted": self.rows_inserted,
            "latest_rows": self.latest_rows,
            "superseded_rows": self.superseded_rows,
            **self.detail,
        }


def _bool_flag(value: Any) -> bool | None:
    if value is None or pd.isna(value) or value == "":
        return None
    return str(value).strip().upper() == "Y"


def _read_directory_text(text: str) -> pd.DataFrame:
    lines = [line for line in text.splitlines() if line and not line.startswith("File Creation Time")]
    if not lines:
        return pd.DataFrame()
    return pd.read_csv(io.StringIO("\n".join(lines)), sep="|", dtype=str, keep_default_na=False)


def _file_creation_time(text: str) -> dt.datetime | None:
    for line in reversed(text.splitlines()):
        if not line.startswith("File Creation Time:"):
            continue
        raw = line.split(":", 1)[1].split("|", 1)[0].strip()
        if not raw:
            return None
        for fmt in ("%m%d%Y%H:%M", "%m%d%Y%H%M"):
            try:
                return dt.datetime.strptime(raw, fmt)
            except ValueError:
                pass
    return None


def resolve_directory_as_of_date(
    options: NasdaqSymbolDirectoryOptions | NasdaqListingEventsOptions,
    text: str,
) -> dt.date:
    """Resolve a date from an explicit option or the file trailer, without the clock.

    Legacy helper: an explicit ``options.as_of_date`` wins. Loaders no longer use
    it to date rows -- see :func:`snapshot_dating`, where the file's own creation
    time dates the snapshot and an operator date is only the load cutoff.
    """
    created = _file_creation_time(text)
    return resolve_as_of_date(options.as_of_date, source_max_date=created)


def snapshot_dating(text: str, *, cutoff: dt.date | None) -> SnapshotDating:
    """Date a pinned Nasdaq Trader file by its own ``File Creation Time:`` trailer.

    The operator cutoff is used as the snapshot date only when the file carries no
    trailer at all (labeled ``operator_cutoff_no_file_creation_time``). Without a
    trailer and without a cutoff the load is not reproducible and fails.
    """
    created = _file_creation_time(text)
    if created is not None:
        return SnapshotDating(created.date(), created, AS_OF_BASIS_FILE_CREATION_TIME)
    if cutoff is None:
        raise ValueError(
            "as_of_date is required: the Nasdaq Trader file has no File Creation Time "
            "trailer and no operator cutoff was supplied"
        )
    return SnapshotDating(cutoff, None, AS_OF_BASIS_OPERATOR_CUTOFF)


def normalize_nasdaq_action(value: Any) -> str | None:
    """Map ``A``/``Add`` -> ``add`` and ``D``/``Delete`` -> ``delete`` (case-insensitive).

    Blank values become ``None``; an unrecognized code is preserved verbatim so it
    stays visible (and countable) instead of being silently dropped.
    """
    if value is None or pd.isna(value):
        return None
    raw = str(value).strip()
    if not raw:
        return None
    return NASDAQ_ACTION_CANONICAL.get(raw.lower(), raw)


def _parse_event_date(value: Any) -> dt.date | None:
    if value is None or pd.isna(value) or str(value).strip() == "":
        return None
    parsed = pd.to_datetime(str(value).strip(), format="%m/%d/%Y", errors="coerce")
    if pd.isna(parsed):
        parsed = pd.to_datetime(str(value).strip(), errors="coerce")
    if pd.isna(parsed):
        return None
    return parsed.date()


def _event_id_part(value: Any) -> str:
    if value is None or pd.isna(value):
        return ""
    return str(value)


def normalize_nasdaq_listed(frame: pd.DataFrame, *, as_of_date: dt.date, source_url: str, run_id: str | None) -> pd.DataFrame:
    if frame.empty:
        return pd.DataFrame()
    return pd.DataFrame(
        {
            "directory": "nasdaqlisted",
            "symbol": frame["Symbol"].map(symbol_key),
            "security_name": frame["Security Name"].str.strip(),
            "market_category": frame["Market Category"].str.strip(),
            "exchange": "NASDAQ",
            "cqs_symbol": pd.NA,
            "etf": frame["ETF"].map(_bool_flag),
            "test_issue": frame["Test Issue"].map(_bool_flag),
            "financial_status": frame["Financial Status"].str.strip(),
            "round_lot_size": pd.to_numeric(frame["Round Lot Size"], errors="coerce").astype("Int64"),
            "next_shares": frame["NextShares"].map(_bool_flag),
            "nasdaq_symbol": frame["Symbol"].map(symbol_key),
            "as_of_date": as_of_date,
            "source_url": source_url,
            "run_id": run_id,
        }
    )


def normalize_other_listed(frame: pd.DataFrame, *, as_of_date: dt.date, source_url: str, run_id: str | None) -> pd.DataFrame:
    if frame.empty:
        return pd.DataFrame()
    return pd.DataFrame(
        {
            "directory": "otherlisted",
            "symbol": frame["ACT Symbol"].map(symbol_key),
            "security_name": frame["Security Name"].str.strip(),
            "market_category": pd.NA,
            "exchange": frame["Exchange"].str.strip(),
            "cqs_symbol": frame["CQS Symbol"].map(symbol_key),
            "etf": frame["ETF"].map(_bool_flag),
            "test_issue": frame["Test Issue"].map(_bool_flag),
            "financial_status": pd.NA,
            "round_lot_size": pd.to_numeric(frame["Round Lot Size"], errors="coerce").astype("Int64"),
            "next_shares": pd.NA,
            "nasdaq_symbol": frame["NASDAQ Symbol"].map(symbol_key),
            "as_of_date": as_of_date,
            "source_url": source_url,
            "run_id": run_id,
        }
    )


DIRECTORY_NORMALIZERS: dict[str, Callable[..., pd.DataFrame]] = {
    "nasdaqlisted": normalize_nasdaq_listed,
    "otherlisted": normalize_other_listed,
}


def normalize_listing_events(
    frame: pd.DataFrame,
    *,
    as_of_date: dt.date,
    source_url: str,
    run_id: str | None,
    source_file_created_at: dt.datetime | None,
) -> pd.DataFrame:
    if frame.empty:
        return pd.DataFrame()
    normalized = pd.DataFrame(
        {
            "symbol": frame["Symbol"].map(symbol_key),
            "company_name": frame["Company Name"].str.strip(),
            "nasdaq_action": frame["NASDAQ Action"].map(normalize_nasdaq_action),
            "bx_action": frame["BX Action"].map(normalize_nasdaq_action),
            "psx_action": frame["PSX Action"].map(normalize_nasdaq_action),
            "effective_date": frame["Effective Date"].map(_parse_event_date),
            "primary_listing_market": frame["Primary Listing Market"].str.strip().replace({"": pd.NA}),
            "as_of_date": as_of_date,
            "source_file_created_at": source_file_created_at,
            "source_url": source_url,
            "run_id": run_id,
        }
    )
    normalized = normalized[normalized["symbol"] != ""].copy()
    # The snapshot date is part of the identity so a re-dated load of the same file
    # yields new rows (the old ones are superseded) instead of a key collision.
    normalized["event_id"] = [
        str(
            uuid.uuid5(
                uuid.NAMESPACE_URL,
                "|".join(
                    _event_id_part(part)
                    for part in (
                        source_url,
                        row.symbol,
                        row.effective_date,
                        row.nasdaq_action,
                        row.bx_action,
                        row.psx_action,
                        row.primary_listing_market,
                        source_file_created_at,
                        as_of_date,
                    )
                ),
            )
        )
        for row in normalized.itertuples(index=False)
    ]
    normalized.insert(0, "event_id", normalized.pop("event_id"))
    return normalized.drop_duplicates(subset=["event_id"])


def _attach_security_ids(store: DuckDBStore, frame: pd.DataFrame) -> pd.DataFrame:
    """Resolve symbols through the CURRENT ticker maps (``current_ticker_unverified``)."""
    if frame.empty:
        frame["security_id"] = pd.Series(dtype="object")
        return frame
    symbols = pd.DataFrame({"symbol": sorted(set(frame["symbol"].dropna()))})
    store.con.register("nasdaq_listing_event_symbols", symbols)
    try:
        resolved = store.con.execute(
            """
            WITH candidates AS (
                SELECT ticker AS symbol, security_id, source_loaded_at
                FROM sec_company_tickers
                UNION ALL
                SELECT id_value AS symbol, security_id, source_loaded_at
                FROM security_identifier_history
                WHERE id_type = 'TICKER'
                  AND valid_to IS NULL
            )
            SELECT symbol, security_id
            FROM (
                SELECT
                    s.symbol,
                    c.security_id,
                    row_number() OVER (
                        PARTITION BY s.symbol
                        ORDER BY c.source_loaded_at DESC NULLS LAST, c.security_id
                    ) AS security_rank
                FROM nasdaq_listing_event_symbols s
                LEFT JOIN candidates c
                  ON c.symbol = s.symbol
            )
            WHERE security_rank = 1
            """
        ).df()
    finally:
        store.con.unregister("nasdaq_listing_event_symbols")
    return frame.merge(resolved, on="symbol", how="left")


def text_sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _receipt_dates_for_content(
    store: DuckDBStore, *, dataset_id: str, source_url: str, sha256: str
) -> set[dt.date]:
    """Snapshot dates previously stamped on byte-identical content of this source."""
    rows = store.con.execute(
        """
        SELECT metadata_json
        FROM raw_source_files
        WHERE dataset_id = ? AND source_url = ? AND sha256 = ?
        """,
        [dataset_id, source_url, sha256],
    ).fetchall()
    dates: set[dt.date] = set()
    for (metadata_json,) in rows:
        with contextlib.suppress(TypeError, ValueError):
            raw = json.loads(metadata_json or "{}").get("as_of_date")
            if raw:
                dates.add(dt.date.fromisoformat(str(raw)[:10]))
    return dates


def _latest_receipt(store: DuckDBStore, *, dataset_id: str, source_url: str) -> tuple[str | None, dt.date | None]:
    row = store.con.execute(
        """
        SELECT sha256, metadata_json
        FROM raw_source_files
        WHERE dataset_id = ? AND source_url = ?
        ORDER BY fetched_at DESC, source_id
        LIMIT 1
        """,
        [dataset_id, source_url],
    ).fetchone()
    if row is None:
        return None, None
    as_of: dt.date | None = None
    with contextlib.suppress(TypeError, ValueError):
        raw = json.loads(row[1] or "{}").get("as_of_date")
        if raw:
            as_of = dt.date.fromisoformat(str(raw)[:10])
    return row[0], as_of


def _date_list_sql(dates: set[dt.date]) -> tuple[str, list[dt.date]]:
    ordered = sorted(dates)
    return ", ".join("?" for _ in ordered), ordered


def _snapshot_available_at(receipt: SnapshotReceipt, dating: SnapshotDating) -> tuple[dt.datetime, bool]:
    """Receipt time, never earlier than the file's own creation stamp (returns floored flag)."""
    created = dating.source_file_created_at
    if created is not None and receipt.received_at < created:
        return created, True
    return receipt.received_at, False


def _receipt_detail(receipt: SnapshotReceipt, available_at: dt.datetime, floored: bool) -> dict[str, Any]:
    return {
        "available_at": available_at.isoformat(),
        "received_at": receipt.received_at.isoformat(),
        "receipt_basis": receipt.basis,
        "available_at_floored_to_file_creation": floored,
        "loaded_at": receipt.loaded_at.isoformat(),
    }


def _record_snapshot_receipt(
    store: DuckDBStore,
    *,
    dataset_id: str,
    source_url: str,
    cache_path: Path | None,
    sha256: str,
    byte_count: int,
    dating: SnapshotDating,
    cutoff: dt.date | None,
    receipt: SnapshotReceipt,
    available_at: dt.datetime,
    status: str,
) -> None:
    record_source_file(
        store,
        dataset_id=dataset_id,
        source_url=source_url,
        cache_path=cache_path,
        status="available",
        sha256=sha256,
        metadata={
            "as_of_date": dating.as_of_date.isoformat(),
            "as_of_basis": dating.as_of_basis,
            "source_file_created_at": (
                dating.source_file_created_at.isoformat() if dating.source_file_created_at else None
            ),
            "cutoff": cutoff.isoformat() if cutoff else None,
            "received_at": receipt.received_at.isoformat(),
            "receipt_basis": receipt.basis,
            "available_at": available_at.isoformat(),
            "loaded_at": receipt.loaded_at.isoformat(),
            "bytes": byte_count,
            "sha256": sha256,
            "source_status": status,
        },
    )


def load_directory_snapshot(
    store: DuckDBStore,
    *,
    directory: str,
    source_url: str,
    text: str,
    sha256: str,
    cutoff: dt.date | None,
    run_id: str | None,
    receipt: SnapshotReceipt,
    cache_path: Path | None = None,
    byte_count: int | None = None,
) -> SnapshotLoad:
    """Load one directory file as a dated, non-destructive snapshot revision.

    Caller owns the transaction. Rows are dated by the file's creation time; a file
    created after ``cutoff``, or received after the end of the cutoff day (its
    ``available_at``), raises :class:`SnapshotAfterCutoffError`. Existing latest
    rows of this source at the same snapshot date, or at any date previously stamped
    on byte-identical content (an operator re-dating), are marked
    ``is_latest_revision = false`` -- never deleted. Reloading the exact file already
    loaded at the same date is a no-op.
    """
    normalizer = DIRECTORY_NORMALIZERS[directory]
    dating = snapshot_dating(text, cutoff=cutoff)
    if cutoff is not None and dating.as_of_date > cutoff:
        raise SnapshotAfterCutoffError(
            f"{source_url} snapshot was created {dating.as_of_date.isoformat()}, after the report cutoff "
            f"{cutoff.isoformat()}; supply an older retained file or a later --as-of-date"
        )
    available_at, floored = _snapshot_available_at(receipt, dating)
    if cutoff is not None and available_at > cutoff_end(cutoff):
        raise SnapshotAfterCutoffError(
            f"{source_url} ({cache_path or 'fetched payload'}) was received {available_at.isoformat()} "
            f"(receipt_basis={receipt.basis}), after the report cutoff day {cutoff.isoformat()}; a file acquired "
            "after the cutoff cannot evidence that snapshot. Restore the file's original receipt "
            "(<file>.receipt.json or preserved mtime) or use a later --as-of-date"
        )
    size = len(text.encode("utf-8")) if byte_count is None else byte_count
    frame = normalizer(_read_directory_text(text), as_of_date=dating.as_of_date, source_url=source_url, run_id=run_id)
    if frame.empty:
        return SnapshotLoad(source_url, SOURCE_STATUS_EMPTY, dating.as_of_date, dating.source_file_created_at,
                            dating.as_of_basis)
    frame["available_at"] = available_at
    frame["is_latest_revision"] = True

    latest_sha, latest_as_of = _latest_receipt(store, dataset_id=SYMBOL_DIRECTORY_DATASET_ID, source_url=source_url)
    existing = store.con.execute(
        """
        SELECT count(*) FROM nasdaq_symbol_directory
        WHERE source_url = ? AND as_of_date = ? AND coalesce(is_latest_revision, true)
        """,
        [source_url, dating.as_of_date],
    ).fetchone()
    existing_latest = int(existing[0]) if existing else 0
    if existing_latest and latest_sha == sha256 and latest_as_of == dating.as_of_date:
        return SnapshotLoad(source_url, SOURCE_STATUS_UNCHANGED, dating.as_of_date, dating.source_file_created_at,
                            dating.as_of_basis, latest_rows=existing_latest,
                            detail=_receipt_detail(receipt, available_at, floored))

    superseded_dates = {dating.as_of_date} | _receipt_dates_for_content(
        store, dataset_id=SYMBOL_DIRECTORY_DATASET_ID, source_url=source_url, sha256=sha256
    )
    placeholders, date_params = _date_list_sql(superseded_dates)
    predicate = f"source_url = ? AND coalesce(is_latest_revision, true) AND as_of_date IN ({placeholders})"
    counted = store.con.execute(
        f"SELECT count(*) FROM nasdaq_symbol_directory WHERE {predicate}", [source_url, *date_params]
    ).fetchone()
    superseded = int(counted[0]) if counted else 0
    if superseded:
        store.con.execute(
            f"UPDATE nasdaq_symbol_directory SET is_latest_revision = false WHERE {predicate}",
            [source_url, *date_params],
        )
    inserted = insert_frame(store, frame, "nasdaq_symbol_directory", "nasdaq_symbol_directory_snapshot_insert")
    _record_snapshot_receipt(
        store,
        dataset_id=SYMBOL_DIRECTORY_DATASET_ID,
        source_url=source_url,
        cache_path=cache_path,
        sha256=sha256,
        byte_count=size,
        dating=dating,
        cutoff=cutoff,
        receipt=receipt,
        available_at=available_at,
        status=SOURCE_STATUS_LOADED,
    )
    return SnapshotLoad(
        source_url,
        SOURCE_STATUS_LOADED,
        dating.as_of_date,
        dating.source_file_created_at,
        dating.as_of_basis,
        rows_inserted=inserted,
        latest_rows=inserted,
        superseded_rows=superseded,
        detail={
            "superseded_as_of_dates": [d.isoformat() for d in sorted(superseded_dates - {dating.as_of_date})],
            **_receipt_detail(receipt, available_at, floored),
        },
    )


def _action_counts(frame: pd.DataFrame) -> dict[str, int]:
    counts = {"add": 0, "delete": 0, "mixed": 0, "no_action": 0, "unrecognized": 0}
    for row in frame[["nasdaq_action", "bx_action", "psx_action"]].itertuples(index=False):
        actions = {action for action in row if action is not None and not pd.isna(action)}
        if not actions:
            counts["no_action"] += 1
        elif not actions <= {"add", "delete"}:
            counts["unrecognized"] += 1
        elif len(actions) > 1:
            counts["mixed"] += 1
        else:
            counts[next(iter(actions))] += 1
    return counts


def load_listing_events_snapshot(
    store: DuckDBStore,
    *,
    source_url: str,
    text: str,
    sha256: str,
    cutoff: dt.date | None,
    run_id: str | None,
    receipt: SnapshotReceipt,
    cache_path: Path | None = None,
    byte_count: int | None = None,
) -> SnapshotLoad:
    """Load one Trading System Adds/Deletes file as a dated, non-destructive revision.

    Caller owns the transaction. A file created after ``cutoff`` or received after
    the end of the cutoff day is not loaded (``source_unavailable_for_snapshot`` with
    ``reason`` ``created_after_cutoff`` / ``received_after_cutoff``). Actions are stored canonically
    (``add``/``delete``); security ids are current-ticker resolutions
    (``current_ticker_unverified``). Latest rows of this source from the same file
    (same creation time), the same snapshot date, or a date previously stamped on
    byte-identical content that are not part of this load are superseded
    (``is_latest_revision = false``), never deleted; rows already present are kept.
    """
    dating = snapshot_dating(text, cutoff=cutoff)
    base_detail = {"identity_basis": IDENTITY_BASIS_CURRENT_TICKER, "cutoff": cutoff.isoformat() if cutoff else None}
    if cutoff is not None and dating.as_of_date > cutoff:
        return SnapshotLoad(
            source_url, SOURCE_STATUS_UNAVAILABLE, dating.as_of_date, dating.source_file_created_at,
            dating.as_of_basis, detail={**base_detail, "reason": "created_after_cutoff"},
        )
    available_at, floored = _snapshot_available_at(receipt, dating)
    if cutoff is not None and available_at > cutoff_end(cutoff):
        return SnapshotLoad(
            source_url, SOURCE_STATUS_UNAVAILABLE, dating.as_of_date, dating.source_file_created_at,
            dating.as_of_basis,
            detail={**base_detail, "reason": "received_after_cutoff",
                    **_receipt_detail(receipt, available_at, floored)},
        )
    size = len(text.encode("utf-8")) if byte_count is None else byte_count
    frame = normalize_listing_events(
        _read_directory_text(text),
        as_of_date=dating.as_of_date,
        source_url=source_url,
        run_id=run_id,
        source_file_created_at=dating.source_file_created_at,
    )
    if frame.empty:
        return SnapshotLoad(source_url, SOURCE_STATUS_EMPTY, dating.as_of_date, dating.source_file_created_at,
                            dating.as_of_basis, detail=base_detail)
    frame = _attach_security_ids(store, frame)
    frame["available_at"] = available_at
    frame["is_latest_revision"] = True
    columns = [
        "event_id", "symbol", "security_id", "company_name", "nasdaq_action", "bx_action", "psx_action",
        "effective_date", "primary_listing_market", "as_of_date", "source_file_created_at", "source_url",
        "run_id", "available_at", "is_latest_revision",
    ]
    frame = frame.reindex(columns=columns)
    superseded_dates = {dating.as_of_date} | _receipt_dates_for_content(
        store, dataset_id=LISTING_EVENTS_DATASET_ID, source_url=source_url, sha256=sha256
    )
    placeholders, date_params = _date_list_sql(superseded_dates)
    store.con.register("nasdaq_listing_events_snapshot_ids", frame[["event_id"]])
    try:
        same_file = (
            "OR source_file_created_at = CAST(? AS TIMESTAMP)" if dating.source_file_created_at is not None else ""
        )
        file_params = [dating.source_file_created_at] if dating.source_file_created_at is not None else []
        predicate = f"""
            source_url = ?
            AND coalesce(is_latest_revision, true)
            AND event_id NOT IN (SELECT event_id FROM nasdaq_listing_events_snapshot_ids)
            AND (as_of_date IN ({placeholders}) {same_file})
        """
        params = [source_url, *date_params, *file_params]
        counted = store.con.execute(f"SELECT count(*) FROM nasdaq_listing_events WHERE {predicate}", params).fetchone()
        superseded = int(counted[0]) if counted else 0
        if superseded:
            store.con.execute(f"UPDATE nasdaq_listing_events SET is_latest_revision = false WHERE {predicate}", params)
        present = {
            event_id
            for (event_id,) in store.con.execute(
                """
                SELECT e.event_id FROM nasdaq_listing_events e
                JOIN nasdaq_listing_events_snapshot_ids s ON s.event_id = e.event_id
                """
            ).fetchall()
        }
        if present:
            store.con.execute(
                """
                UPDATE nasdaq_listing_events SET is_latest_revision = true
                WHERE event_id IN (SELECT event_id FROM nasdaq_listing_events_snapshot_ids)
                  AND NOT coalesce(is_latest_revision, true)
                """
            )
    finally:
        with contextlib.suppress(Exception):
            store.con.unregister("nasdaq_listing_events_snapshot_ids")
    new_rows = frame[~frame["event_id"].isin(present)]
    inserted = insert_frame(store, new_rows, "nasdaq_listing_events", "nasdaq_listing_events_snapshot_insert")
    status = SOURCE_STATUS_UNCHANGED if not inserted and not superseded else SOURCE_STATUS_LOADED
    _record_snapshot_receipt(
        store,
        dataset_id=LISTING_EVENTS_DATASET_ID,
        source_url=source_url,
        cache_path=cache_path,
        sha256=sha256,
        byte_count=size,
        dating=dating,
        cutoff=cutoff,
        receipt=receipt,
        available_at=available_at,
        status=status,
    )
    return SnapshotLoad(
        source_url,
        status,
        dating.as_of_date,
        dating.source_file_created_at,
        dating.as_of_basis,
        rows_inserted=inserted,
        latest_rows=len(frame),
        superseded_rows=superseded,
        detail={
            **base_detail,
            "symbols": int(frame["symbol"].nunique()),
            "unresolved_security_ids": int(frame["security_id"].isna().sum()),
            "action_counts": _action_counts(frame),
            **_receipt_detail(receipt, available_at, floored),
        },
    )


def _fetch(session: requests.Session, url: str, timeout: int) -> tuple[str, str, int]:
    response = session.get(url, timeout=timeout)
    response.raise_for_status()
    return response.text, text_sha256(response.content), len(response.content)


class NasdaqSymbolDirectoryDataset(Dataset):
    dataset_id = SYMBOL_DIRECTORY_DATASET_ID
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: NasdaqSymbolDirectoryOptions) -> DatasetLoadResult:
        """Fetch the current directory files (network) and load them as dated snapshots."""
        session = requests.Session()
        session.headers.update({"User-Agent": options.user_agent, "Accept": "text/plain,*/*"})
        fetched = [
            (directory, url, *_fetch(session, url, options.request_timeout))
            for directory, url in (("nasdaqlisted", options.nasdaq_url), ("otherlisted", options.other_url))
        ]
        received_at = now_utc_naive()
        receipt = SnapshotReceipt(received_at, RECEIPT_BASIS_NETWORK_DOWNLOAD, received_at)
        with store.transaction():
            loads = [
                load_directory_snapshot(
                    store,
                    directory=directory,
                    source_url=url,
                    text=text,
                    sha256=sha,
                    cutoff=options.as_of_date,
                    run_id=options.run_id,
                    receipt=receipt,
                    byte_count=size,
                )
                for directory, url, text, sha, size in fetched
            ]
        rows = sum(load.latest_rows for load in loads)
        as_of_dates = sorted({load.as_of_date.isoformat() for load in loads if load.as_of_date})
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="nasdaq_symbol_directory",
            check_name="snapshot_rows",
            status="passed" if rows > 0 else "failed",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"as_of_dates": as_of_dates},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=sum(load.rows_inserted for load in loads),
            source=f"{options.nasdaq_url};{options.other_url}",
            details={
                "as_of_date": as_of_dates[-1] if as_of_dates else None,
                "files": [load.as_detail() for load in loads],
            },
        )


class NasdaqListingEventsDataset(Dataset):
    dataset_id = LISTING_EVENTS_DATASET_ID
    source_name = LISTING_EVENTS_SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: NasdaqListingEventsOptions) -> DatasetLoadResult:
        """Fetch the current Adds/Deletes file (network) and load it as a dated snapshot."""
        session = requests.Session()
        session.headers.update({"User-Agent": options.user_agent, "Accept": "text/plain,*/*"})
        text, sha, size = _fetch(session, options.source_url, options.request_timeout)
        received_at = now_utc_naive()
        with store.transaction():
            load = load_listing_events_snapshot(
                store,
                source_url=options.source_url,
                text=text,
                sha256=sha,
                cutoff=options.as_of_date,
                run_id=options.run_id,
                receipt=SnapshotReceipt(received_at, RECEIPT_BASIS_NETWORK_DOWNLOAD, received_at),
                byte_count=size,
            )
        detail = load.as_detail()
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="nasdaq_listing_events",
            check_name="loaded_listing_event_rows",
            status="passed" if load.status != SOURCE_STATUS_UNAVAILABLE else "warning",
            observed_value=float(load.latest_rows),
            threshold_value=0.0,
            details=detail,
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=load.rows_inserted,
            source=options.source_url,
            details=detail,
        )
