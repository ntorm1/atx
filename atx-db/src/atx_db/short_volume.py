"""FINRA daily short-volume flow warehouse surfaces.

FINRA daily short-volume files are a public, daily short-sale flow proxy. They
are not a substitute for bi-monthly short interest (the outstanding stock), but
they are useful for detecting high-frequency short-sale pressure. This module
keeps the raw FINRA market-code rows and materializes a per-security/day rollup.

Tests and normal fixture loads are offline only: callers inject a FINRA
pipe-delimited file (``Date|Symbol|ShortVolume|ShortExemptVolume|TotalVolume|Market``)
or a normalized CSV with equivalent columns.
"""

from __future__ import annotations

import datetime as dt
import hashlib
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

import pandas as pd

from .asof import finra_short_volume_asof, short_volume_metrics_asof  # noqa: F401
from .calendar import xnys_sessions
from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .finra import FINRA_OK_OUTCOMES, FinraReceiptStore, PoliteFinraClient, fetch_finra_file
from .warehouse import file_sha256, insert_frame, json_dumps, now_utc_naive, quality_check, record_source_file, snake_case, symbol_key


SOURCE_NAME = "FINRA daily short-volume flow"
DEFAULT_SOURCE = "injected_finra_daily_short_volume_v1"

MARKET_CODE_NAMES = {
    "N": "NYSE_TRF",
    "Q": "NASDAQ_TRF_CARTERET",
    "B": "NASDAQ_TRF_CHICAGO",
    "D": "ADF",
    "ALL": "AGGREGATED",
}

SHORT_VOLUME_COLUMNS = [
    "volume_id", "security_id", "symbol", "trade_date", "market_code",
    "short_volume", "short_exempt_volume", "total_volume",
    "restatement_seq", "is_latest", "as_of_date", "available_at",
    "source", "source_file", "source_file_sha256", "raw_payload_json", "run_id",
]

SHORT_VOLUME_METRIC_COLUMNS = [
    "metric_id", "security_id", "symbol", "trade_date",
    "short_volume", "short_exempt_volume", "total_volume",
    "short_volume_ratio", "short_exempt_ratio",
    "short_volume_ratio_percentile", "short_exempt_ratio_percentile",
    "market_count", "dominant_market_code", "dominant_market_total_volume",
    "dominant_market_share_pct", "is_high_short_flow",
    "restatement_seq", "is_latest_revision", "as_of_date", "available_at",
    "source", "run_id",
]

COLUMN_ALIASES = {
    "date": "trade_date",
    "tradedate": "trade_date",
    "trade_date": "trade_date",
    "symbol": "symbol",
    "ticker": "symbol",
    "tic": "symbol",
    "shortvolume": "short_volume",
    "short_volume": "short_volume",
    "shortsalevolume": "short_volume",
    "shortexemptvolume": "short_exempt_volume",
    "short_exempt_volume": "short_exempt_volume",
    "totalvolume": "total_volume",
    "total_volume": "total_volume",
    "market": "market_code",
    "marketcode": "market_code",
    "market_code": "market_code",
    "asofdate": "as_of_date",
    "as_of_date": "as_of_date",
    "availableat": "available_at",
    "available_at": "available_at",
    "knowledgefrom": "available_at",
    "sourceloadedat": "available_at",
}


@dataclass(frozen=True)
class FinraShortVolumeOptions:
    source_file: Path | None = None
    source: str = DEFAULT_SOURCE
    replace_source_file: bool = True
    run_id: str | None = None


def _empty_short_volume_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=SHORT_VOLUME_COLUMNS)


def _empty_metric_frame() -> pd.DataFrame:
    return pd.DataFrame(columns=SHORT_VOLUME_METRIC_COLUMNS)


def _normalize_columns(frame: pd.DataFrame) -> pd.DataFrame:
    renamed: dict[str, str] = {}
    for column in frame.columns:
        normalized = snake_case(str(column)).lower()
        compact = normalized.replace("_", "")
        renamed[column] = COLUMN_ALIASES.get(normalized, COLUMN_ALIASES.get(compact, normalized))
    return frame.rename(columns=renamed)


def _string(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="string")
    return frame[column].replace("", pd.NA).astype("string")


def _numeric(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="Float64")
    return pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce")


def _date(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NaT] * len(frame), index=frame.index)
    return pd.to_datetime(frame[column].replace("", pd.NA), errors="coerce").dt.date


def _short_volume_id(row: pd.Series) -> str:
    parts = [
        row.get("source"), row.get("symbol"), row.get("trade_date"),
        row.get("market_code"), row.get("available_at"),
    ]
    payload = "|".join("" if pd.isna(part) else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _metric_id(row: pd.Series) -> str:
    parts = [row.get("source"), row.get("symbol"), row.get("trade_date"), row.get("available_at")]
    payload = "|".join("" if pd.isna(part) else str(part) for part in parts)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _default_available_at(trade_dates: pd.Series) -> pd.Series:
    # FINRA daily files are a delayed public flow proxy. In the absence of an
    # explicit fixture value, use a conservative next-day evening timestamp.
    parsed = pd.to_datetime(trade_dates, errors="coerce")
    return parsed + pd.Timedelta(days=1, hours=22)


def normalize_short_volume_rows(
    frame: pd.DataFrame,
    *,
    options: FinraShortVolumeOptions,
    source_file_sha256: str | None = None,
    source_file: Path | None = None,
) -> pd.DataFrame:
    if frame.empty:
        return _empty_short_volume_frame()

    raw = _normalize_columns(frame.copy())
    required = {"trade_date", "symbol", "short_volume", "short_exempt_volume", "total_volume"}
    missing = sorted(required.difference(raw.columns))
    if missing:
        raise ValueError(f"FINRA daily short-volume rows missing expected columns: {missing}")

    trade_date = _date(raw, "trade_date")
    as_of_date = _date(raw, "as_of_date")
    as_of_date = as_of_date.where(pd.notna(as_of_date), trade_date)
    if "available_at" in raw.columns:
        available_at = pd.to_datetime(raw["available_at"].replace("", pd.NA), errors="coerce")
        available_at = available_at.fillna(_default_available_at(trade_date))
    else:
        available_at = _default_available_at(trade_date)

    out = pd.DataFrame(index=raw.index)
    out["security_id"] = _string(raw, "security_id")
    out["symbol"] = _string(raw, "symbol").map(lambda v: symbol_key(None if pd.isna(v) else str(v))).replace("", pd.NA)
    out["trade_date"] = trade_date
    market = _string(raw, "market_code").fillna("ALL").str.strip().str.upper()
    out["market_code"] = market.where(market.ne(""), "ALL")
    out["short_volume"] = _numeric(raw, "short_volume")
    out["short_exempt_volume"] = _numeric(raw, "short_exempt_volume")
    out["total_volume"] = _numeric(raw, "total_volume")
    out["restatement_seq"] = 0
    out["is_latest"] = True
    out["as_of_date"] = as_of_date
    out["available_at"] = available_at
    out["source"] = options.source
    out["source_file"] = str(source_file) if source_file else pd.NA
    out["source_file_sha256"] = source_file_sha256
    out["raw_payload_json"] = raw.apply(lambda r: json_dumps(r.dropna().to_dict()), axis=1)
    out["run_id"] = options.run_id

    out = out[
        out["symbol"].notna()
        & out["trade_date"].notna()
        & out["as_of_date"].notna()
        & out["available_at"].notna()
        & out["market_code"].notna()
    ].copy()
    if out.empty:
        return _empty_short_volume_frame()
    out["volume_id"] = out.apply(_short_volume_id, axis=1)
    return out[SHORT_VOLUME_COLUMNS]


def compute_short_volume_metrics(rows: pd.DataFrame, *, run_id: str | None = None) -> pd.DataFrame:
    """Roll raw latest FINRA market-code rows into symbol/day metrics."""
    if rows.empty:
        return _empty_metric_frame()

    raw = rows.copy()
    for column in ("short_volume", "short_exempt_volume", "total_volume"):
        raw[column] = pd.to_numeric(raw[column], errors="coerce")
    raw["available_at"] = pd.to_datetime(raw["available_at"], errors="coerce")

    grouped = raw.groupby(["source", "symbol", "trade_date"], dropna=False)
    out = grouped.agg(
        security_id=("security_id", lambda s: next((v for v in s if pd.notna(v)), pd.NA)),
        short_volume=("short_volume", "sum"),
        short_exempt_volume=("short_exempt_volume", "sum"),
        total_volume=("total_volume", "sum"),
        market_count=("market_code", "nunique"),
        as_of_date=("as_of_date", "max"),
        available_at=("available_at", "max"),
    ).reset_index()

    dominant = (
        raw.sort_values(["source", "symbol", "trade_date", "total_volume", "market_code"], ascending=[True, True, True, False, True])
        .drop_duplicates(["source", "symbol", "trade_date"], keep="first")
        [["source", "symbol", "trade_date", "market_code", "total_volume"]]
        .rename(columns={"market_code": "dominant_market_code", "total_volume": "dominant_market_total_volume"})
    )
    out = out.merge(dominant, on=["source", "symbol", "trade_date"], how="left")

    total = out["total_volume"].where(out["total_volume"] > 0)
    out["short_volume_ratio"] = out["short_volume"] / total
    out["short_exempt_ratio"] = out["short_exempt_volume"] / total
    out["dominant_market_share_pct"] = out["dominant_market_total_volume"] / total * 100.0

    by_date = out.groupby("trade_date", dropna=False)
    out["short_volume_ratio_percentile"] = by_date["short_volume_ratio"].rank(pct=True)
    out["short_exempt_ratio_percentile"] = by_date["short_exempt_ratio"].rank(pct=True)
    out["is_high_short_flow"] = out["short_volume_ratio_percentile"].ge(0.90) & out["total_volume"].gt(0)
    out["restatement_seq"] = 0
    out["is_latest_revision"] = True
    out["run_id"] = run_id
    out["metric_id"] = out.apply(_metric_id, axis=1)

    return out[SHORT_VOLUME_METRIC_COLUMNS]


def _read_source_file(path: Path) -> pd.DataFrame:
    with path.open("r", encoding="utf-8-sig", errors="replace") as handle:
        first_line = handle.readline()
    sep = "|" if "|" in first_line else ","
    return pd.read_csv(path, dtype=str, keep_default_na=False, sep=sep)


def _delete_existing_ids(store: DuckDBStore, table_name: str, id_column: str, frame: pd.DataFrame) -> None:
    ids = frame[[id_column]].drop_duplicates()
    relation = f"{table_name}_{id_column}_delete"
    store.con.register(relation, ids)
    try:
        store.con.execute(
            f"DELETE FROM {table_name} AS dst USING {relation} AS src WHERE dst.{id_column} = src.{id_column}"
        )
    finally:
        store.con.unregister(relation)


def _resolve_security_ids(store: DuckDBStore, source: str) -> None:
    store.con.execute(
        """
        UPDATE finra_short_volume v
        SET security_id = s.security_id
        FROM securities s
        WHERE s.primary_symbol = v.symbol
          AND v.source = ?
          AND v.security_id IS NULL
        """,
        [source],
    )


def _recompute_short_volume_latest(store: DuckDBStore, source: str) -> None:
    store.con.execute(
        """
        WITH ranked AS (
            SELECT
                volume_id,
                row_number() OVER (
                    PARTITION BY source, symbol, trade_date, market_code
                    ORDER BY available_at DESC, volume_id
                ) AS rn,
                dense_rank() OVER (
                    PARTITION BY source, symbol, trade_date, market_code
                    ORDER BY available_at ASC
                ) - 1 AS seq
            FROM finra_short_volume
            WHERE source = ?
        )
        UPDATE finra_short_volume v
        SET is_latest = (r.rn = 1),
            restatement_seq = r.seq,
            updated_at = now()
        FROM ranked r
        WHERE r.volume_id = v.volume_id
        """,
        [source],
    )


def _recompute_metric_latest(store: DuckDBStore, source: str) -> None:
    store.con.execute(
        """
        WITH ranked AS (
            SELECT
                metric_id,
                row_number() OVER (
                    PARTITION BY source, symbol, trade_date
                    ORDER BY available_at DESC, metric_id
                ) AS rn,
                dense_rank() OVER (
                    PARTITION BY source, symbol, trade_date
                    ORDER BY available_at ASC
                ) - 1 AS seq
            FROM short_volume_metrics
            WHERE source = ?
        )
        UPDATE short_volume_metrics m
        SET is_latest_revision = (r.rn = 1),
            restatement_seq = r.seq,
            updated_at = now()
        FROM ranked r
        WHERE r.metric_id = m.metric_id
        """,
        [source],
    )


def load_finra_short_volume(store: DuckDBStore, options: FinraShortVolumeOptions) -> int:
    store.initialize()
    if options.source_file is None:
        return 0

    source_file = Path(options.source_file)
    frame = _read_source_file(source_file)
    source_hash = file_sha256(source_file)
    normalized = normalize_short_volume_rows(
        frame,
        options=options,
        source_file_sha256=source_hash,
        source_file=source_file,
    )
    record_source_file(
        store,
        dataset_id="finra_short_volume",
        source_url=str(source_file),
        cache_path=source_file,
        sha256=source_hash,
        metadata={"rows": int(len(frame))},
    )
    if normalized.empty:
        return 0

    with store.transaction():
        if options.replace_source_file:
            store.con.execute(
                "DELETE FROM finra_short_volume WHERE source = ? AND source_file_sha256 = ?",
                [options.source, source_hash],
            )
        _delete_existing_ids(store, "finra_short_volume", "volume_id", normalized)
        insert_frame(store, normalized, "finra_short_volume", "finra_short_volume_insert")
        _resolve_security_ids(store, options.source)
        _recompute_short_volume_latest(store, options.source)
    return int(len(normalized))


def load_short_volume_inputs(store: DuckDBStore, source: str = DEFAULT_SOURCE) -> pd.DataFrame:
    store.initialize()
    return store.con.execute(
        """
        SELECT *
        FROM finra_short_volume
        WHERE source = ?
          AND is_latest
        ORDER BY symbol, trade_date, market_code
        """,
        [source],
    ).df()


def refresh_short_volume_metrics(
    store: DuckDBStore,
    *,
    source: str = DEFAULT_SOURCE,
    run_id: str | None = None,
) -> int:
    inputs = load_short_volume_inputs(store, source)
    metrics = compute_short_volume_metrics(inputs, run_id=run_id)
    if metrics.empty:
        return 0

    with store.transaction():
        _delete_existing_ids(store, "short_volume_metrics", "metric_id", metrics)
        insert_frame(store, metrics, "short_volume_metrics", "short_volume_metrics_insert")
        _recompute_metric_latest(store, source)
    return int(len(metrics))


class FinraShortVolumeDataset(Dataset):
    dataset_id = "finra_short_volume"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FinraShortVolumeOptions) -> DatasetLoadResult:
        rows = load_finra_short_volume(store, options)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="finra_short_volume",
            check_name="rows_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={
                "source": options.source,
                "source_file": str(options.source_file) if options.source_file else None,
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"source_file": str(options.source_file) if options.source_file else None},
        )


# ---------------------------------------------------------------------------
# Fetch half (tier-1 v2 X.2 Step 2, ruling C-91): Reg SHO daily files + receipts, no warehouse
# ---------------------------------------------------------------------------

REGSHO_DAILY_URL = "https://cdn.finra.org/equity/regsho/daily/{prefix}shvol{day:%Y%m%d}.txt"
#: One pipe-delimited file per trade date and reporting facility (Date|Symbol|ShortVolume|ShortExemptVolume|
#: TotalVolume|Market). CNMS is the consolidated NMS file (the FINRA TRFs + ADF); the facility files carry
#: the same NMS volume split by facility (FORF: OTC equities) and reach further back.
REGSHO_DAILY_PREFIXES: dict[str, str] = {
    "CNMS": "consolidated NMS (FINRA TRFs + ADF)",
    "FNSQ": "FINRA/Nasdaq TRF Carteret",
    "FNQC": "FINRA/Nasdaq TRF Chicago",
    "FNYX": "FINRA/NYSE TRF",
    "FNRA": "FINRA ADF",
    "FORF": "FINRA ORF (OTC equities)",
}
REGSHO_DAILY_HEADER = "Date|Symbol|ShortVolume|ShortExemptVolume|TotalVolume|Market"
#: FINRA posts a trade date's file no later than 18:00 America/New_York that same day.
REGSHO_DAILY_POSTED_BY_ET = dt.time(18, 0)
_NEW_YORK = ZoneInfo("America/New_York")


def regsho_daily_posted_by_utc(trade_date: dt.date) -> dt.datetime:
    """Trade date 18:00 America/New_York in UTC: when FINRA's daily short-volume file is public at the latest."""

    return dt.datetime.combine(trade_date, REGSHO_DAILY_POSTED_BY_ET, tzinfo=_NEW_YORK).astimezone(dt.UTC)


def fetch_regsho_daily_files(
    client: PoliteFinraClient,
    root: Path,
    *,
    prefixes: Iterable[str],
    start_by_prefix: dict[str, dt.date],
    end: dt.date,
    absent_final_days: int = 14,
    progress: Callable[[str, int, int, dict[str, Any]], None] | None = None,
) -> dict[str, Any]:
    """Fetch every Reg SHO daily short-volume file for XNYS sessions in ``[start, end]`` per prefix into ``root``.

    Files are stored gzip-compressed at ``<prefix>/<yyyy>/<prefix>shvol<yyyymmdd>.txt.gz`` with one receipt
    each (sha256 / bytes over the raw file, Last-Modified, ETag, line count, header, posted-by clock). A
    session without a file is an ``absent`` receipt. Date-major order, so a killed run leaves every prefix
    covered to about the same date; a rerun verifies retained files instead of requesting them.
    """

    prefixes = [prefix.upper() for prefix in prefixes]
    unknown = sorted(set(prefixes) - set(REGSHO_DAILY_PREFIXES))
    if unknown:
        raise ValueError(f"unknown Reg SHO daily prefixes: {unknown}")
    store = FinraReceiptStore(root).load()
    sessions = xnys_sessions(min(start_by_prefix[prefix] for prefix in prefixes), end)
    planned = sum(1 for day in sessions for prefix in prefixes if day >= start_by_prefix[prefix])
    per_prefix: dict[str, list[dict[str, Any]]] = {prefix: [] for prefix in prefixes}
    done = 0
    for day in sessions:
        for prefix in prefixes:
            if day < start_by_prefix[prefix]:
                continue
            record = fetch_finra_file(
                client,
                store,
                key=f"{prefix}:{day.isoformat()}",
                url=REGSHO_DAILY_URL.format(prefix=prefix, day=day),
                relative_path=f"{prefix}/{day.year}/{prefix}shvol{day:%Y%m%d}.txt.gz",
                data_date=day,
                run_date=end,
                extra={
                    "prefix": prefix,
                    "trade_date": day.isoformat(),
                    "posted_by_utc": regsho_daily_posted_by_utc(day).isoformat(),
                },
                absent_final_days=absent_final_days,
            )
            done += 1
            per_prefix[prefix].append({
                key: record.get(key)
                for key in ("key", "outcome", "status", "error", "trade_date", "bytes", "stored_bytes", "lines",
                            "header", "last_modified")
            })
            if progress:
                progress("regsho", done, planned, record)

    summary: dict[str, Any] = {"root": str(root), "sessions": len(sessions), "planned": planned,
                               "receipt_lines_before": store.lines, "receipt_corrupt_lines": store.corrupt_lines,
                               "posted_by_rule": "trade_date 18:00 America/New_York", "prefixes": {}}
    for prefix, records in per_prefix.items():
        outcomes: dict[str, int] = {}
        for record in records:
            outcomes[str(record["outcome"])] = outcomes.get(str(record["outcome"]), 0) + 1
        ok = [record for record in records if record["outcome"] in FINRA_OK_OUTCOMES]
        ok_dates = [str(record["trade_date"]) for record in ok]
        first_ok = ok_dates[0] if ok_dates else None
        # sessions after the first file without one (the only real gaps; before it the prefix did not exist)
        gaps = [
            str(record["trade_date"])
            for record in records
            if first_ok is not None and str(record["trade_date"]) > first_ok
            and record["outcome"] in ("absent", "absent_known")
        ]
        headers = {str(record.get("header")) for record in ok}
        summary["prefixes"][prefix] = {
            "description": REGSHO_DAILY_PREFIXES[prefix],
            "probe_start": start_by_prefix[prefix].isoformat(),
            "attempted": len(records),
            "outcomes": outcomes,
            "files_ok": len(ok),
            "first_date": first_ok,
            "last_date": ok_dates[-1] if ok_dates else None,
            "raw_bytes": sum(int(record.get("bytes") or 0) for record in ok),
            "stored_bytes": sum(int(record.get("stored_bytes") or 0) for record in ok),
            "lines": sum(int(record.get("lines") or 0) for record in ok),
            "header_only_files": sum(1 for record in ok if int(record.get("lines") or 0) <= 1),
            "headers_seen": sorted(headers)[:5],
            "gap_sessions": len(gaps),
            "gap_sample": gaps[:40],
            "failed": [record for record in records if record["outcome"] == "failed"][:20],
        }
    return summary


class ShortVolumeMetricsDataset(Dataset):
    dataset_id = "short_volume_metrics"
    source_name = "Derived FINRA daily short-volume metrics"

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: FinraShortVolumeOptions) -> DatasetLoadResult:
        rows = refresh_short_volume_metrics(store, source=options.source, run_id=options.run_id)
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="short_volume_metrics",
            check_name="rows_materialized",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"source": options.source},
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=rows,
            source=options.source,
            details={"grain": "symbol,trade_date"},
        )
