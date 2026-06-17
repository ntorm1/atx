#!/usr/bin/env python3
"""
Build daily US equity split adjustment factors from public web data.

Final dataset layout:

    data/us_split_adjustment_factors/factors_by_symbol/
      symbol=AAPL/data_0.parquet
      symbol=MSFT/data_0.parquet
      ...

    data/us_split_adjustment_factors/factors_by_date/
      date=1962-01-02/data_0.parquet
      date=1962-01-03/data_0.parquet
      ...

The symbol-partitioned factors are canonical. The date-partitioned factors are
a derived mirror for daily panel reads and to preserve the requested
symbol,date,return_factor daily export shape. Each factor Parquet file
physically contains:

    symbol: string
    date: date32
    return_factor: float64

For the default split-only mode, close * return_factor gives the close adjusted
onto the latest share basis available from the downloaded history. For example,
dates before a 2-for-1 split receive factor 0.5; the split date and later dates
receive factor 1.0 for that event.
"""

from __future__ import annotations

import argparse
import concurrent.futures as futures
import csv
import datetime as dt
import gzip
import io
import json
import logging
import math
import random
import re
import shutil
import sys
import threading
import time
from dataclasses import dataclass
from email.utils import parsedate_to_datetime
from pathlib import Path
from typing import Any
from urllib.parse import quote

import duckdb
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq
import requests


NASDAQ_LISTED_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt"
OTHER_LISTED_URL = "https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt"
YAHOO_CHART_URL = "https://query1.finance.yahoo.com/v8/finance/chart/{symbol}"
SEC_COMPANY_TICKERS_URL = "https://www.sec.gov/files/company_tickers.json"
SEC_COMPANYFACTS_URL = "https://data.sec.gov/api/xbrl/companyfacts/CIK{cik}.json"
SEC_SUBMISSIONS_URL = "https://data.sec.gov/submissions/CIK{cik}.json"
SP500_CONSTITUENTS_URL = (
    "https://raw.githubusercontent.com/datasets/s-and-p-500-companies/"
    "main/data/constituents.csv"
)
HTTP_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/125.0 Safari/537.36"
    )
}

EPOCH = dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
DEFAULT_START_EPOCH = -2208988800  # 1900-01-01 UTC.
DATA_VERSION = "security_master_v2"

GICS_SECTOR_CODES = {
    "Energy": "10",
    "Materials": "15",
    "Industrials": "20",
    "Consumer Discretionary": "25",
    "Consumer Staples": "30",
    "Health Care": "35",
    "Financials": "40",
    "Information Technology": "45",
    "Communication Services": "50",
    "Utilities": "55",
    "Real Estate": "60",
}

FINAL_SCHEMA = pa.schema(
    [
        ("symbol", pa.string()),
        ("date", pa.date32()),
        ("return_factor", pa.float64()),
    ]
)

EVENT_SCHEMA = pa.schema(
    [
        ("symbol", pa.string()),
        ("date", pa.date32()),
        ("split_factor", pa.float64()),
    ]
)

DIVIDEND_SCHEMA = pa.schema(
    [
        ("symbol", pa.string()),
        ("date", pa.date32()),
        ("cash_dividend", pa.float64()),
        ("currency", pa.string()),
        ("source", pa.string()),
    ]
)

SHARES_SCHEMA = pa.schema(
    [
        ("symbol", pa.string()),
        ("date", pa.date32()),
        ("shares_outstanding", pa.int64()),
        ("cik", pa.string()),
        ("filed_date", pa.date32()),
        ("form", pa.string()),
        ("accession", pa.string()),
        ("source", pa.string()),
    ]
)

SECTOR_SCHEMA = pa.schema(
    [
        ("symbol", pa.string()),
        ("date", pa.date32()),
        ("security_name", pa.string()),
        ("cik", pa.string()),
        ("sec_sic", pa.string()),
        ("sec_sic_description", pa.string()),
        ("gics_sector_code", pa.string()),
        ("gics_sector", pa.string()),
        ("gics_sub_industry", pa.string()),
        ("gics_source", pa.string()),
        ("source", pa.string()),
    ]
)

SECURITY_MASTER_SCHEMA = pa.schema(
    [
        ("date", pa.date32()),
        ("symbol", pa.string()),
        ("cumulative_adjustment_factor", pa.float64()),
        ("cash_dividend", pa.float64()),
        ("dividend_currency", pa.string()),
        ("shares_outstanding", pa.int64()),
        ("shares_as_of_date", pa.date32()),
        ("shares_filed_date", pa.date32()),
        ("sec_cik", pa.string()),
        ("sec_sic", pa.string()),
        ("sec_sic_description", pa.string()),
        ("gics_sector_code", pa.string()),
        ("gics_sector", pa.string()),
        ("gics_sub_industry", pa.string()),
        ("gics_source", pa.string()),
    ]
)


@dataclass(frozen=True)
class SymbolRecord:
    symbol: str
    name: str = ""
    exchange: str = ""
    source: str = "manual"
    etf: bool = False


@dataclass
class SymbolResult:
    symbol: str
    ok: bool
    rows: int = 0
    min_date: str | None = None
    max_date: str | None = None
    split_events: int = 0
    yahoo_symbol: str | None = None
    frame: pd.DataFrame | None = None
    event_frame: pd.DataFrame | None = None
    dividend_frame: pd.DataFrame | None = None
    currency: str | None = None
    error: str | None = None


@dataclass
class ReferenceResult:
    symbol: str
    ok: bool
    shares_rows: int = 0
    sector_rows: int = 0
    cik: str | None = None
    shares_frame: pd.DataFrame | None = None
    sector_frame: pd.DataFrame | None = None
    error: str | None = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build US stock split adjustment factors with symbol-canonical "
            "storage and an optional date-partitioned mirror."
        )
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=Path("data/us_split_adjustment_factors"),
        help="Root for factors_by_symbol/, factors_by_date/, _cache/, _staging/, and manifest/.",
    )
    parser.add_argument(
        "--mode",
        choices=("split", "adjclose"),
        default="split",
        help=(
            "split: use split events only. adjclose: use Yahoo Adj Close / Close, "
            "which includes dividends as well as splits."
        ),
    )
    parser.add_argument(
        "--datasets",
        nargs="+",
        choices=("all", "yahoo", "sec", "security-master"),
        default=["all"],
        help=(
            "Dataset groups to build. yahoo writes factors/dividends/splits; "
            "sec writes shares/sectors; security-master writes the final joined file."
        ),
    )
    parser.add_argument(
        "--symbols",
        default="",
        help="Optional comma/space separated symbols. If omitted, use Nasdaq Trader directories.",
    )
    parser.add_argument(
        "--symbol-file",
        type=Path,
        help="Optional file with one symbol per line, or CSV with symbol in the first column.",
    )
    parser.add_argument(
        "--symbol-regex",
        help="Optional regex filter applied to output symbols after universe discovery.",
    )
    parser.add_argument("--limit", type=int, help="Optional max symbols after filtering.")
    parser.add_argument(
        "--include-etfs",
        action="store_true",
        help="Include ETF/NextShares rows from Nasdaq Trader symbol directories.",
    )
    parser.add_argument(
        "--include-test-issues",
        action="store_true",
        help="Include Nasdaq Trader test issues.",
    )
    parser.add_argument(
        "--common-only",
        action="store_true",
        help="Keep only names that look like common/ordinary/ADS equity issues.",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=4,
        help="Parallel Yahoo chart download workers.",
    )
    parser.add_argument(
        "--batch-size",
        type=int,
        default=50,
        help="Symbols per staging Parquet batch before final partitioning.",
    )
    parser.add_argument(
        "--start-epoch",
        type=int,
        default=DEFAULT_START_EPOCH,
        help="Unix timestamp for earliest requested history. Default is 1900-01-01 UTC.",
    )
    parser.add_argument(
        "--end-date",
        default="",
        help="Optional inclusive end date YYYY-MM-DD. Default is now.",
    )
    parser.add_argument(
        "--request-timeout",
        type=float,
        default=30.0,
        help="HTTP timeout in seconds.",
    )
    parser.add_argument(
        "--request-delay",
        type=float,
        default=0.05,
        help="Small randomized delay per uncached Yahoo request.",
    )
    parser.add_argument(
        "--yahoo-min-interval",
        type=float,
        default=0.20,
        help="Global minimum seconds between uncached Yahoo requests across all workers.",
    )
    parser.add_argument(
        "--sec-user-agent",
        default="atx-security-master/0.1 contact@example.com",
        help="SEC EDGAR User-Agent header. Set this to a real contact for production runs.",
    )
    parser.add_argument(
        "--sec-workers",
        type=int,
        default=2,
        help="Parallel SEC EDGAR workers for CompanyFacts/submissions.",
    )
    parser.add_argument(
        "--sec-request-delay",
        type=float,
        default=0.12,
        help="Small randomized delay per uncached SEC request.",
    )
    parser.add_argument(
        "--sec-min-interval",
        type=float,
        default=0.35,
        help="Global minimum seconds between uncached SEC requests across all workers.",
    )
    parser.add_argument(
        "--backoff-max-seconds",
        type=float,
        default=180.0,
        help="Maximum sleep between retries after transient HTTP failures or rate limits.",
    )
    parser.add_argument(
        "--sp500-url",
        default=SP500_CONSTITUENTS_URL,
        help="Open CSV with Symbol, GICS Sector, and GICS Sub-Industry columns.",
    )
    parser.add_argument("--retries", type=int, default=4, help="HTTP retries per symbol.")
    parser.add_argument(
        "--refresh-cache",
        action="store_true",
        help="Ignore cached Yahoo chart payloads and download fresh responses.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Replace existing generated output under output-root.",
    )
    parser.add_argument(
        "--incremental",
        action="store_true",
        help=(
            "Preserve existing canonical symbol partitions and replace only the "
            "symbols processed in this run; derived outputs are rebuilt afterward."
        ),
    )
    parser.add_argument(
        "--force-symbols",
        action="store_true",
        help="In --incremental mode, process requested symbols even if the planner marks them complete.",
    )
    parser.add_argument(
        "--skip-date-mirror",
        action="store_true",
        help="Only write the symbol-partitioned canonical dataset, not factors_by_date/.",
    )
    parser.add_argument(
        "--skip-events",
        action="store_true",
        help="Do not write split_events_by_symbol/.",
    )
    parser.add_argument(
        "--skip-security-master",
        action="store_true",
        help="Do not write security_master/security_master.parquet.",
    )
    parser.add_argument(
        "--keep-staging",
        action="store_true",
        help="Keep staging batch Parquet after final partitioned output is written.",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=("DEBUG", "INFO", "WARNING", "ERROR"),
    )
    return parser.parse_args()


def selected_dataset_groups(args: argparse.Namespace) -> set[str]:
    requested = set(args.datasets)
    if "all" in requested:
        return {"yahoo", "sec", "security-master"}
    return requested


class RateLimitedError(RuntimeError):
    def __init__(self, message: str, retry_after: float | None = None):
        super().__init__(message)
        self.retry_after = retry_after


class RequestLimiter:
    def __init__(self, min_interval_seconds: float):
        self.min_interval_seconds = max(0.0, float(min_interval_seconds))
        self._lock = threading.Lock()
        self._next_allowed = 0.0

    def wait(self) -> None:
        if self.min_interval_seconds <= 0:
            return
        with self._lock:
            now = time.monotonic()
            sleep_for = self._next_allowed - now
            if sleep_for > 0:
                time.sleep(sleep_for)
                now = time.monotonic()
            self._next_allowed = now + self.min_interval_seconds


def retry_after_seconds(response: requests.Response) -> float | None:
    raw = response.headers.get("Retry-After")
    if not raw:
        return None
    try:
        return max(0.0, float(raw))
    except ValueError:
        try:
            retry_at = parsedate_to_datetime(raw)
            if retry_at.tzinfo is None:
                retry_at = retry_at.replace(tzinfo=dt.timezone.utc)
            return max(0.0, (retry_at - dt.datetime.now(dt.timezone.utc)).total_seconds())
        except (TypeError, ValueError):
            return None


def sleep_before_retry(
    attempt: int,
    max_seconds: float,
    retry_after: float | None = None,
) -> None:
    if retry_after is not None:
        delay = min(max_seconds, max(0.0, retry_after))
    else:
        delay = min(max_seconds, 0.75 * (2**attempt)) * (0.75 + random.random())
    if delay > 0:
        time.sleep(delay)


def epoch_to_date(seconds: int | float) -> dt.date:
    return (EPOCH + dt.timedelta(seconds=int(seconds))).date()


def end_epoch_from_args(value: str) -> int:
    if not value:
        return int(time.time())
    end_date = dt.date.fromisoformat(value)
    # Yahoo period2 is exclusive-ish. Use next midnight UTC to include the date.
    end_dt = dt.datetime.combine(end_date + dt.timedelta(days=1), dt.time(), dt.timezone.utc)
    return int((end_dt - EPOCH).total_seconds())


def read_public_text(url: str, timeout: float) -> str:
    response = requests.get(url, headers=HTTP_HEADERS, timeout=timeout)
    response.raise_for_status()
    return response.text


def strip_footer(text: str) -> str:
    lines = [line for line in text.splitlines() if not line.startswith("File Creation Time")]
    return "\n".join(lines) + "\n"


def looks_common_equity(name: str) -> bool:
    lower = name.lower()
    bad = (
        " warrant",
        " warrants",
        " right",
        " rights",
        " unit",
        " units",
        " preferred",
        " preference",
        " notes due",
        " note due",
        " bond",
        " debenture",
    )
    if any(token in lower for token in bad):
        return False
    good = (
        "common stock",
        "ordinary share",
        "ordinary shares",
        "american depositary",
        "adr",
        "ads",
        "class a",
        "class b",
        "class c",
    )
    return any(token in lower for token in good)


def load_nasdaq_symbols(args: argparse.Namespace) -> list[SymbolRecord]:
    records: list[SymbolRecord] = []

    nasdaq_text = strip_footer(read_public_text(NASDAQ_LISTED_URL, args.request_timeout))
    for row in csv.DictReader(io.StringIO(nasdaq_text), delimiter="|"):
        symbol = (row.get("Symbol") or "").strip()
        if not symbol:
            continue
        name = (row.get("Security Name") or "").strip()
        is_etf = (row.get("ETF") or "").strip().upper() == "Y"
        is_nextshares = (row.get("NextShares") or "").strip().upper() == "Y"
        is_test = (row.get("Test Issue") or "").strip().upper() == "Y"
        if is_test and not args.include_test_issues:
            continue
        if (is_etf or is_nextshares) and not args.include_etfs:
            continue
        if args.common_only and not looks_common_equity(name):
            continue
        records.append(
            SymbolRecord(
                symbol=symbol,
                name=name,
                exchange="NASDAQ",
                source="nasdaqlisted",
                etf=is_etf or is_nextshares,
            )
        )

    other_text = strip_footer(read_public_text(OTHER_LISTED_URL, args.request_timeout))
    for row in csv.DictReader(io.StringIO(other_text), delimiter="|"):
        symbol = (row.get("NASDAQ Symbol") or row.get("ACT Symbol") or "").strip()
        if not symbol:
            continue
        name = (row.get("Security Name") or "").strip()
        is_etf = (row.get("ETF") or "").strip().upper() == "Y"
        is_test = (row.get("Test Issue") or "").strip().upper() == "Y"
        if is_test and not args.include_test_issues:
            continue
        if is_etf and not args.include_etfs:
            continue
        if args.common_only and not looks_common_equity(name):
            continue
        records.append(
            SymbolRecord(
                symbol=symbol,
                name=name,
                exchange=(row.get("Exchange") or "").strip(),
                source="otherlisted",
                etf=is_etf,
            )
        )

    return dedupe_records(records)


def load_manual_symbols(args: argparse.Namespace) -> list[SymbolRecord]:
    symbols: list[str] = []
    if args.symbols:
        symbols.extend(token for token in re.split(r"[\s,]+", args.symbols.strip()) if token)
    if args.symbol_file:
        for line in args.symbol_file.read_text(encoding="utf-8").splitlines():
            clean = line.strip()
            if not clean or clean.startswith("#"):
                continue
            symbols.append(clean.split(",")[0].strip())
    return dedupe_records([SymbolRecord(symbol=symbol.upper()) for symbol in symbols])


def dedupe_records(records: list[SymbolRecord]) -> list[SymbolRecord]:
    seen: set[str] = set()
    out: list[SymbolRecord] = []
    for record in records:
        key = record.symbol.upper()
        if key in seen:
            continue
        seen.add(key)
        out.append(record)
    return out


def load_symbol_universe(args: argparse.Namespace) -> list[SymbolRecord]:
    if args.symbols or args.symbol_file:
        records = load_manual_symbols(args)
    else:
        records = load_nasdaq_symbols(args)

    if args.symbol_regex:
        pattern = re.compile(args.symbol_regex)
        records = [record for record in records if pattern.search(record.symbol)]
    if args.limit is not None:
        records = records[: args.limit]
    return records


def yahoo_symbol_candidates(symbol: str) -> list[str]:
    primary = symbol.strip().upper().replace(".", "-").replace("/", "-")
    primary = primary.replace("$", "-P")
    candidates = [primary]

    # Some preferred shares are rendered as ABC-A by listing feeds and ABC-PA by Yahoo.
    match = re.fullmatch(r"([A-Z]+)-([A-Z])", primary)
    if match:
        candidates.append(f"{match.group(1)}-P{match.group(2)}")

    # Keep original as a final fallback for feeds that already match Yahoo.
    original = symbol.strip().upper()
    if original not in candidates:
        candidates.append(original)

    deduped: list[str] = []
    for candidate in candidates:
        if candidate and candidate not in deduped:
            deduped.append(candidate)
    return deduped


def cache_path(cache_dir: Path, symbol: str) -> Path:
    return cache_dir / f"{quote(symbol, safe='')}.json.gz"


def read_cached_json(path: Path) -> dict[str, Any]:
    with gzip.open(path, "rt", encoding="utf-8") as handle:
        return json.load(handle)


def write_cached_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with gzip.open(tmp, "wt", encoding="utf-8") as handle:
        json.dump(payload, handle, separators=(",", ":"))
    tmp.replace(path)


def request_json_with_cache(
    url: str,
    path: Path,
    headers: dict[str, str],
    timeout: float,
    delay: float,
    retries: int,
    refresh: bool,
    backoff_max_seconds: float,
    limiter: RequestLimiter | None = None,
) -> dict[str, Any]:
    if path.exists() and not refresh:
        return read_cached_json(path)

    last_error = ""
    for attempt in range(retries + 1):
        if delay > 0:
            time.sleep(delay * (0.5 + random.random()))
        try:
            if limiter:
                limiter.wait()
            response = requests.get(url, headers=headers, timeout=timeout)
            if response.status_code in (429, 502, 503, 504):
                raise RateLimitedError(
                    f"HTTP {response.status_code}",
                    retry_after_seconds(response),
                )
            response.raise_for_status()
            payload = response.json()
            write_cached_json(path, payload)
            return payload
        except RateLimitedError as exc:
            last_error = str(exc)
            if attempt < retries:
                sleep_before_retry(attempt, backoff_max_seconds, exc.retry_after)
        except Exception as exc:  # noqa: BLE001 - caller records per-symbol errors.
            last_error = str(exc)
            if attempt < retries:
                sleep_before_retry(attempt, backoff_max_seconds)
    raise RuntimeError(last_error or f"failed to fetch {url}")


def normalize_vendor_symbol(symbol: str) -> str:
    return symbol.strip().upper().replace(".", "-").replace("/", "-")


def cik10(cik: int | str) -> str:
    return f"{int(cik):010d}"


def sec_headers(args: argparse.Namespace) -> dict[str, str]:
    return {
        "User-Agent": args.sec_user_agent,
        "Accept": "application/json",
        "Accept-Encoding": "gzip, deflate",
    }


def load_sec_ticker_map(
    args: argparse.Namespace,
    cache_dir: Path,
    limiter: RequestLimiter | None = None,
) -> dict[str, str]:
    payload = request_json_with_cache(
        SEC_COMPANY_TICKERS_URL,
        cache_dir / "company_tickers.json.gz",
        sec_headers(args),
        args.request_timeout,
        args.sec_request_delay,
        args.retries,
        args.refresh_cache,
        args.backoff_max_seconds,
        limiter,
    )
    out: dict[str, str] = {}
    for item in payload.values():
        ticker = normalize_vendor_symbol(str(item.get("ticker") or ""))
        cik_value = item.get("cik_str")
        if ticker and cik_value is not None:
            out[ticker] = cik10(cik_value)
    return out


def load_sp500_gics_map(args: argparse.Namespace, cache_dir: Path) -> dict[str, dict[str, str]]:
    cache_file = cache_dir / "sp500_constituents.csv"
    if cache_file.exists() and not args.refresh_cache:
        text = cache_file.read_text(encoding="utf-8")
    else:
        response = requests.get(args.sp500_url, headers=HTTP_HEADERS, timeout=args.request_timeout)
        response.raise_for_status()
        text = response.text
        cache_file.parent.mkdir(parents=True, exist_ok=True)
        cache_file.write_text(text, encoding="utf-8")

    out: dict[str, dict[str, str]] = {}
    for row in csv.DictReader(io.StringIO(text)):
        symbol = normalize_vendor_symbol(str(row.get("Symbol") or ""))
        if not symbol:
            continue
        sector = str(row.get("GICS Sector") or "").strip()
        out[symbol] = {
            "gics_sector": sector,
            "gics_sector_code": GICS_SECTOR_CODES.get(sector, ""),
            "gics_sub_industry": str(row.get("GICS Sub-Industry") or "").strip(),
            "source": args.sp500_url,
        }
    return out


def date_or_none(value: Any) -> dt.date | None:
    if not value:
        return None
    try:
        return dt.date.fromisoformat(str(value)[:10])
    except ValueError:
        return None


def sec_symbol_candidates(symbol: str) -> list[str]:
    normalized = normalize_vendor_symbol(symbol)
    candidates = [normalized]
    original = symbol.strip().upper()
    if original not in candidates:
        candidates.append(original)
    return candidates


def lookup_cik(symbol: str, cik_map: dict[str, str]) -> str | None:
    for candidate in sec_symbol_candidates(symbol):
        if candidate in cik_map:
            return cik_map[candidate]
    return None


def extract_shares_frame(
    symbol: str,
    cik: str,
    facts_payload: dict[str, Any],
    max_date: dt.date,
) -> pd.DataFrame:
    facts = facts_payload.get("facts") or {}
    values = (
        facts.get("dei", {})
        .get("EntityCommonStockSharesOutstanding", {})
        .get("units", {})
        .get("shares", [])
    )
    rows: list[tuple[str, dt.date, int, str, dt.date | None, str, str, str]] = []
    for item in values:
        as_of = date_or_none(item.get("end"))
        filed = date_or_none(item.get("filed"))
        raw_value = item.get("val")
        if as_of is None or as_of > max_date or raw_value is None:
            continue
        try:
            shares = int(raw_value)
        except (TypeError, ValueError):
            continue
        if shares <= 0:
            continue
        rows.append(
            (
                symbol,
                as_of,
                shares,
                cik,
                filed,
                str(item.get("form") or ""),
                str(item.get("accn") or ""),
                "sec_companyfacts_dei_entity_common_stock_shares_outstanding",
            )
        )

    frame = pd.DataFrame(
        rows,
        columns=[
            "symbol",
            "date",
            "shares_outstanding",
            "cik",
            "filed_date",
            "form",
            "accession",
            "source",
        ],
    )
    if frame.empty:
        return frame
    frame.sort_values(["symbol", "date", "filed_date", "accession"], inplace=True)
    return frame.drop_duplicates(["symbol", "date"], keep="last")


def sector_frame_for_symbol(
    record: SymbolRecord,
    cik: str | None,
    submissions_payload: dict[str, Any] | None,
    sp500_map: dict[str, dict[str, str]],
    as_of_date: dt.date,
) -> pd.DataFrame:
    key = normalize_vendor_symbol(record.symbol)
    sp500 = sp500_map.get(key, {})
    sic = ""
    sic_description = ""
    if submissions_payload:
        sic = str(submissions_payload.get("sic") or "")
        sic_description = str(submissions_payload.get("sicDescription") or "")

    gics_source = sp500.get("source", "") if sp500 else ""
    source_parts = []
    if cik:
        source_parts.append("sec_submissions")
    if sp500:
        source_parts.append("datasets_sp500_constituents")
    if not source_parts:
        source_parts.append("nasdaq_trader_symbol_directory")

    frame = pd.DataFrame(
        [
            (
                record.symbol,
                as_of_date,
                record.name,
                cik or "",
                sic,
                sic_description,
                sp500.get("gics_sector_code", ""),
                sp500.get("gics_sector", ""),
                sp500.get("gics_sub_industry", ""),
                gics_source,
                "+".join(source_parts),
            )
        ],
        columns=[
            "symbol",
            "date",
            "security_name",
            "cik",
            "sec_sic",
            "sec_sic_description",
            "gics_sector_code",
            "gics_sector",
            "gics_sub_industry",
            "gics_source",
            "source",
        ],
    )
    return frame


def process_reference_symbol(
    record: SymbolRecord,
    args: argparse.Namespace,
    cik_map: dict[str, str],
    sp500_map: dict[str, dict[str, str]],
    cache_dir: Path,
    as_of_date: dt.date,
    max_shares_date: dt.date,
    limiter: RequestLimiter | None = None,
) -> ReferenceResult:
    cik = lookup_cik(record.symbol, cik_map)
    facts_payload: dict[str, Any] | None = None
    submissions_payload: dict[str, Any] | None = None
    errors: list[str] = []

    if cik:
        headers = sec_headers(args)
        try:
            facts_payload = request_json_with_cache(
                SEC_COMPANYFACTS_URL.format(cik=cik),
                cache_dir / "companyfacts" / f"{cik}.json.gz",
                headers,
                args.request_timeout,
                args.sec_request_delay,
                args.retries,
                args.refresh_cache,
                args.backoff_max_seconds,
                limiter,
            )
        except Exception as exc:  # noqa: BLE001 - preserve partial coverage.
            errors.append(f"companyfacts: {exc}")
        try:
            submissions_payload = request_json_with_cache(
                SEC_SUBMISSIONS_URL.format(cik=cik),
                cache_dir / "submissions" / f"{cik}.json.gz",
                headers,
                args.request_timeout,
                args.sec_request_delay,
                args.retries,
                args.refresh_cache,
                args.backoff_max_seconds,
                limiter,
            )
        except Exception as exc:  # noqa: BLE001 - preserve partial coverage.
            errors.append(f"submissions: {exc}")

    shares_frame = (
        extract_shares_frame(record.symbol, cik, facts_payload, max_shares_date)
        if cik and facts_payload
        else pd.DataFrame(columns=SHARES_SCHEMA.names)
    )
    sector_frame = sector_frame_for_symbol(
        record,
        cik,
        submissions_payload,
        sp500_map,
        as_of_date,
    )
    return ReferenceResult(
        symbol=record.symbol,
        ok=True,
        shares_rows=len(shares_frame),
        sector_rows=len(sector_frame),
        cik=cik,
        shares_frame=shares_frame,
        sector_frame=sector_frame,
        error="; ".join(errors) if errors else None,
    )


def fetch_chart(
    record: SymbolRecord,
    args: argparse.Namespace,
    cache_dir: Path,
    end_epoch: int,
    limiter: RequestLimiter | None = None,
) -> tuple[dict[str, Any], str]:
    cache_file = cache_path(cache_dir, record.symbol)
    if cache_file.exists() and not args.refresh_cache:
        payload = read_cached_json(cache_file)
        result = chart_result(payload)
        yahoo_symbol = str(result.get("meta", {}).get("symbol") or record.symbol)
        return payload, yahoo_symbol

    last_error = ""
    for yahoo_symbol in yahoo_symbol_candidates(record.symbol):
        params = {
            "period1": str(args.start_epoch),
            "period2": str(end_epoch),
            "interval": "1d",
            "events": "div,splits",
            "includeAdjustedClose": "true",
        }
        url = YAHOO_CHART_URL.format(symbol=quote(yahoo_symbol, safe=""))
        for attempt in range(args.retries + 1):
            if args.request_delay > 0:
                time.sleep(args.request_delay * (0.5 + random.random()))
            try:
                if limiter:
                    limiter.wait()
                response = requests.get(
                    url,
                    params=params,
                    headers=HTTP_HEADERS,
                    timeout=args.request_timeout,
                )
                if response.status_code in (429, 502, 503, 504):
                    raise RateLimitedError(
                        f"HTTP {response.status_code}",
                        retry_after_seconds(response),
                    )
                response.raise_for_status()
                payload = response.json()
                result = chart_result(payload)
                timestamps = result.get("timestamp") or []
                quote_data = (result.get("indicators", {}).get("quote") or [{}])[0]
                closes = quote_data.get("close") or []
                if not timestamps or not any(is_valid_number(value) for value in closes):
                    raise RuntimeError("empty chart history")
                write_cached_json(cache_file, payload)
                actual = str(result.get("meta", {}).get("symbol") or yahoo_symbol)
                return payload, actual
            except RateLimitedError as exc:
                last_error = f"{yahoo_symbol}: {exc}"
                if attempt < args.retries:
                    sleep_before_retry(attempt, args.backoff_max_seconds, exc.retry_after)
            except Exception as exc:  # noqa: BLE001 - keep per-symbol failures isolated.
                last_error = f"{yahoo_symbol}: {exc}"
                if attempt < args.retries:
                    sleep_before_retry(attempt, args.backoff_max_seconds)
        logging.debug("Yahoo candidate failed for %s: %s", record.symbol, last_error)

    raise RuntimeError(last_error or "no Yahoo candidates returned data")


def chart_result(payload: dict[str, Any]) -> dict[str, Any]:
    chart = payload.get("chart") or {}
    error = chart.get("error")
    if error:
        raise RuntimeError(error)
    results = chart.get("result") or []
    if not results:
        raise RuntimeError("missing chart result")
    return results[0]


def is_valid_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and math.isfinite(float(value))


def parse_split_ratio(event: dict[str, Any]) -> float | None:
    numerator = event.get("numerator")
    denominator = event.get("denominator")
    if is_valid_number(numerator) and is_valid_number(denominator):
        num = float(numerator)
        den = float(denominator)
        if num > 0 and den > 0:
            return den / num

    ratio = str(event.get("splitRatio") or "").strip()
    match = re.fullmatch(r"([0-9.]+)\s*[:/]\s*([0-9.]+)", ratio)
    if match:
        num = float(match.group(1))
        den = float(match.group(2))
        if num > 0 and den > 0:
            return den / num
    return None


def split_events(result: dict[str, Any]) -> list[tuple[dt.date, float]]:
    raw = ((result.get("events") or {}).get("splits") or {}).values()
    by_date: dict[dt.date, float] = {}
    for event in raw:
        event_date_raw = event.get("date")
        if not is_valid_number(event_date_raw):
            continue
        factor = parse_split_ratio(event)
        if factor is None:
            continue
        event_date = epoch_to_date(event_date_raw)
        by_date[event_date] = by_date.get(event_date, 1.0) * factor
    return sorted(by_date.items(), key=lambda item: item[0])


def dividend_events(result: dict[str, Any]) -> list[tuple[dt.date, float]]:
    raw = ((result.get("events") or {}).get("dividends") or {}).values()
    by_date: dict[dt.date, float] = {}
    for event in raw:
        event_date_raw = event.get("date")
        amount = event.get("amount")
        if not is_valid_number(event_date_raw) or not is_valid_number(amount):
            continue
        event_date = epoch_to_date(event_date_raw)
        by_date[event_date] = by_date.get(event_date, 0.0) + float(amount)
    return sorted(by_date.items(), key=lambda item: item[0])


def split_return_factors(dates: list[dt.date], events: list[tuple[dt.date, float]]) -> list[float]:
    if not events:
        return [1.0] * len(dates)

    total = 1.0
    for _, factor in events:
        total *= factor

    factors: list[float] = []
    prefix = 1.0
    event_index = 0
    for date_value in dates:
        while event_index < len(events) and events[event_index][0] <= date_value:
            prefix *= events[event_index][1]
            event_index += 1
        factors.append(total / prefix)
    return factors


def chart_to_frame(
    symbol: str,
    payload: dict[str, Any],
    mode: str,
) -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame, int]:
    result = chart_result(payload)
    timestamps = result.get("timestamp") or []
    quote_data = (result.get("indicators", {}).get("quote") or [{}])[0]
    closes = quote_data.get("close") or []
    adjcloses = ((result.get("indicators", {}).get("adjclose") or [{}])[0]).get("adjclose") or []

    rows: list[tuple[str, dt.date, float]] = []
    dates: list[dt.date] = []
    events = split_events(result)
    dividends = dividend_events(result)
    currency = str((result.get("meta") or {}).get("currency") or "")
    event_frame = pd.DataFrame(
        [(symbol, event_date, split_factor) for event_date, split_factor in events],
        columns=["symbol", "date", "split_factor"],
    )
    dividend_frame = pd.DataFrame(
        [
            (symbol, event_date, cash_dividend, currency, "yahoo_chart")
            for event_date, cash_dividend in dividends
        ],
        columns=["symbol", "date", "cash_dividend", "currency", "source"],
    )

    if mode == "split":
        for ts_value, close_value in zip(timestamps, closes):
            if not is_valid_number(close_value):
                continue
            dates.append(epoch_to_date(ts_value))
        factors = split_return_factors(dates, events)
        rows = [(symbol, date_value, factor) for date_value, factor in zip(dates, factors)]
        split_count = len(events)
    else:
        split_count = len(events)
        for ts_value, close_value, adj_value in zip(timestamps, closes, adjcloses):
            if not is_valid_number(close_value) or not is_valid_number(adj_value):
                continue
            close_float = float(close_value)
            if close_float == 0:
                continue
            rows.append((symbol, epoch_to_date(ts_value), float(adj_value) / close_float))

    frame = pd.DataFrame(rows, columns=["symbol", "date", "return_factor"])
    return frame, event_frame, dividend_frame, split_count


def process_symbol(
    record: SymbolRecord,
    args: argparse.Namespace,
    cache_dir: Path,
    end_epoch: int,
    limiter: RequestLimiter | None = None,
) -> SymbolResult:
    try:
        payload, yahoo_symbol = fetch_chart(record, args, cache_dir, end_epoch, limiter)
        frame, event_frame, dividend_frame, split_count = chart_to_frame(
            record.symbol,
            payload,
            args.mode,
        )
        if frame.empty:
            raise RuntimeError("no valid daily rows")
        return SymbolResult(
            symbol=record.symbol,
            ok=True,
            rows=len(frame),
            min_date=str(frame["date"].min()),
            max_date=str(frame["date"].max()),
            split_events=split_count,
            yahoo_symbol=yahoo_symbol,
            frame=frame,
            event_frame=event_frame,
            dividend_frame=dividend_frame,
            currency=str((chart_result(payload).get("meta") or {}).get("currency") or ""),
        )
    except Exception as exc:  # noqa: BLE001 - keep going on bad symbols.
        return SymbolResult(symbol=record.symbol, ok=False, error=str(exc))


def chunks(records: list[SymbolRecord], batch_size: int) -> list[list[SymbolRecord]]:
    return [records[index : index + batch_size] for index in range(0, len(records), batch_size)]


def safe_rmtree(path: Path, required_parent: Path) -> None:
    resolved = path.resolve()
    parent = required_parent.resolve()
    if parent != resolved and parent not in resolved.parents:
        raise RuntimeError(f"refusing to remove path outside {parent}: {resolved}")
    if path.exists():
        shutil.rmtree(path)


def remove_symbol_partitions(root: Path, records: list[SymbolRecord], required_parent: Path) -> None:
    for record in records:
        safe_rmtree(root / f"symbol={record.symbol}", required_parent)


def has_parquet_files(root: Path) -> bool:
    return root.exists() and any(root.rglob("*.parquet"))


def write_staging_batch(
    batch_path: Path,
    frames: list[pd.DataFrame],
    schema: pa.Schema,
    sort_by: list[str],
) -> int:
    combined = pd.concat(frames, ignore_index=True)
    combined.sort_values(sort_by, inplace=True)
    table = pa.Table.from_pandas(combined, schema=schema, preserve_index=False)
    pq.write_table(table, batch_path, compression="zstd", use_dictionary=["symbol"])
    return len(combined)


def sql_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def stats_for_dataset(final_dir: Path) -> dict[str, Any]:
    final_glob = (final_dir / "**" / "*.parquet").as_posix()
    con = duckdb.connect()
    row = con.execute(
        f"""
        SELECT
            count(*)::UBIGINT AS rows,
            count(DISTINCT symbol)::UBIGINT AS symbols,
            min(date)::DATE AS min_date,
            max(date)::DATE AS max_date,
            count(DISTINCT date)::UBIGINT AS dates
        FROM read_parquet({sql_quote(final_glob)})
        """
    ).fetchone()
    return {
        "rows": int(row[0] or 0),
        "symbols": int(row[1] or 0),
        "min_date": str(row[2]) if row[2] else None,
        "max_date": str(row[3]) if row[3] else None,
        "dates": int(row[4] or 0),
    }


def finalize_factor_dataset(
    stage_dir: Path,
    final_dir: Path,
    partition_by: str,
    order_by: str,
    merge_existing: bool = False,
) -> dict[str, Any]:
    stage_glob = (stage_dir / "*.parquet").as_posix()
    final_sql_path = final_dir.as_posix()

    con = duckdb.connect()
    merge_option = ",\n            OVERWRITE_OR_IGNORE true" if merge_existing else ""
    copy_sql = f"""
        COPY (
            SELECT
                symbol::VARCHAR AS symbol,
                CAST(date AS DATE) AS date,
                return_factor::DOUBLE AS return_factor
            FROM read_parquet({sql_quote(stage_glob)})
            WHERE symbol IS NOT NULL
              AND date IS NOT NULL
              AND return_factor IS NOT NULL
            ORDER BY {order_by}
        )
        TO {sql_quote(final_sql_path)}
        (
            FORMAT PARQUET,
            PARTITION_BY ({partition_by}),
            WRITE_PARTITION_COLUMNS true,
            COMPRESSION ZSTD,
            ROW_GROUP_SIZE 122880{merge_option}
        )
    """
    con.execute(copy_sql)
    return stats_for_dataset(final_dir)


def materialize_factor_date_mirror_from_symbol(
    factors_by_symbol_dir: Path,
    factors_by_date_dir: Path,
) -> dict[str, Any]:
    source_glob = (factors_by_symbol_dir / "**" / "*.parquet").as_posix()
    final_sql_path = factors_by_date_dir.as_posix()
    con = duckdb.connect()
    con.execute(
        f"""
        COPY (
            SELECT
                symbol::VARCHAR AS symbol,
                CAST(date AS DATE) AS date,
                return_factor::DOUBLE AS return_factor
            FROM read_parquet({sql_quote(source_glob)})
            WHERE symbol IS NOT NULL
              AND date IS NOT NULL
              AND return_factor IS NOT NULL
            ORDER BY date, symbol
        )
        TO {sql_quote(final_sql_path)}
        (
            FORMAT PARQUET,
            PARTITION_BY (date),
            WRITE_PARTITION_COLUMNS true,
            COMPRESSION ZSTD,
            ROW_GROUP_SIZE 122880
        )
        """
    )
    return stats_for_dataset(factors_by_date_dir)


def finalize_event_dataset(
    stage_dir: Path,
    final_dir: Path,
    merge_existing: bool = False,
) -> dict[str, Any]:
    stage_glob = (stage_dir / "*.parquet").as_posix()
    final_sql_path = final_dir.as_posix()

    con = duckdb.connect()
    merge_option = ",\n            OVERWRITE_OR_IGNORE true" if merge_existing else ""
    copy_sql = f"""
        COPY (
            SELECT
                symbol::VARCHAR AS symbol,
                CAST(date AS DATE) AS date,
                split_factor::DOUBLE AS split_factor
            FROM read_parquet({sql_quote(stage_glob)})
            WHERE symbol IS NOT NULL
              AND date IS NOT NULL
              AND split_factor IS NOT NULL
            ORDER BY symbol, date
        )
        TO {sql_quote(final_sql_path)}
        (
            FORMAT PARQUET,
            PARTITION_BY (symbol),
            WRITE_PARTITION_COLUMNS true,
            COMPRESSION ZSTD,
            ROW_GROUP_SIZE 122880{merge_option}
        )
    """
    con.execute(copy_sql)
    return stats_for_dataset(final_dir)


def finalize_dividend_dataset(
    stage_dir: Path,
    final_dir: Path,
    merge_existing: bool = False,
) -> dict[str, Any]:
    stage_glob = (stage_dir / "*.parquet").as_posix()
    final_sql_path = final_dir.as_posix()

    con = duckdb.connect()
    merge_option = ",\n            OVERWRITE_OR_IGNORE true" if merge_existing else ""
    con.execute(
        f"""
        COPY (
            SELECT
                symbol::VARCHAR AS symbol,
                CAST(date AS DATE) AS date,
                cash_dividend::DOUBLE AS cash_dividend,
                currency::VARCHAR AS currency,
                source::VARCHAR AS source
            FROM read_parquet({sql_quote(stage_glob)})
            WHERE symbol IS NOT NULL
              AND date IS NOT NULL
              AND cash_dividend IS NOT NULL
            ORDER BY symbol, date
        )
        TO {sql_quote(final_sql_path)}
        (
            FORMAT PARQUET,
            PARTITION_BY (symbol),
            WRITE_PARTITION_COLUMNS true,
            COMPRESSION ZSTD,
            ROW_GROUP_SIZE 122880{merge_option}
        )
        """
    )
    return stats_for_dataset(final_dir)


def finalize_shares_dataset(
    stage_dir: Path,
    final_dir: Path,
    merge_existing: bool = False,
) -> dict[str, Any]:
    stage_glob = (stage_dir / "*.parquet").as_posix()
    final_sql_path = final_dir.as_posix()

    con = duckdb.connect()
    merge_option = ",\n            OVERWRITE_OR_IGNORE true" if merge_existing else ""
    con.execute(
        f"""
        COPY (
            SELECT
                symbol::VARCHAR AS symbol,
                CAST(date AS DATE) AS date,
                shares_outstanding::BIGINT AS shares_outstanding,
                cik::VARCHAR AS cik,
                CAST(filed_date AS DATE) AS filed_date,
                form::VARCHAR AS form,
                accession::VARCHAR AS accession,
                source::VARCHAR AS source
            FROM read_parquet({sql_quote(stage_glob)})
            WHERE symbol IS NOT NULL
              AND date IS NOT NULL
              AND shares_outstanding IS NOT NULL
            ORDER BY symbol, date
        )
        TO {sql_quote(final_sql_path)}
        (
            FORMAT PARQUET,
            PARTITION_BY (symbol),
            WRITE_PARTITION_COLUMNS true,
            COMPRESSION ZSTD,
            ROW_GROUP_SIZE 122880{merge_option}
        )
        """
    )
    return stats_for_dataset(final_dir)


def finalize_sector_dataset(
    stage_dir: Path,
    final_dir: Path,
    merge_existing: bool = False,
) -> dict[str, Any]:
    stage_glob = (stage_dir / "*.parquet").as_posix()
    final_sql_path = final_dir.as_posix()

    con = duckdb.connect()
    merge_option = ",\n            OVERWRITE_OR_IGNORE true" if merge_existing else ""
    con.execute(
        f"""
        COPY (
            SELECT
                symbol::VARCHAR AS symbol,
                CAST(date AS DATE) AS date,
                security_name::VARCHAR AS security_name,
                cik::VARCHAR AS cik,
                sec_sic::VARCHAR AS sec_sic,
                sec_sic_description::VARCHAR AS sec_sic_description,
                gics_sector_code::VARCHAR AS gics_sector_code,
                gics_sector::VARCHAR AS gics_sector,
                gics_sub_industry::VARCHAR AS gics_sub_industry,
                gics_source::VARCHAR AS gics_source,
                source::VARCHAR AS source
            FROM read_parquet({sql_quote(stage_glob)})
            WHERE symbol IS NOT NULL
            ORDER BY symbol
        )
        TO {sql_quote(final_sql_path)}
        (
            FORMAT PARQUET,
            PARTITION_BY (symbol),
            WRITE_PARTITION_COLUMNS true,
            COMPRESSION ZSTD,
            ROW_GROUP_SIZE 122880{merge_option}
        )
        """
    )
    return stats_for_dataset(final_dir)


def dataset_glob_or_empty(
    con: duckdb.DuckDBPyConnection,
    name: str,
    root: Path,
    create_sql: str,
) -> None:
    files = list(root.rglob("*.parquet")) if root.exists() else []
    if files:
        con.execute(
            f"CREATE OR REPLACE TEMP VIEW {name} AS "
            f"SELECT * FROM read_parquet({sql_quote((root / '**' / '*.parquet').as_posix())})"
        )
    else:
        con.execute(f"CREATE OR REPLACE TEMP VIEW {name} AS {create_sql}")


def stats_for_security_master(path: Path) -> dict[str, Any]:
    con = duckdb.connect()
    row = con.execute(
        f"""
        SELECT
            count(*)::UBIGINT AS rows,
            count(DISTINCT symbol)::UBIGINT AS symbols,
            min(date)::DATE AS min_date,
            max(date)::DATE AS max_date,
            count(DISTINCT date)::UBIGINT AS dates
        FROM read_parquet({sql_quote(path.as_posix())})
        """
    ).fetchone()
    return {
        "rows": int(row[0] or 0),
        "symbols": int(row[1] or 0),
        "min_date": str(row[2]) if row[2] else None,
        "max_date": str(row[3]) if row[3] else None,
        "dates": int(row[4] or 0),
    }


def materialize_security_master(
    factors_by_symbol_dir: Path,
    dividends_by_symbol_dir: Path,
    shares_by_symbol_dir: Path,
    sectors_by_symbol_dir: Path,
    security_master_dir: Path,
) -> dict[str, Any]:
    security_master_dir.mkdir(parents=True, exist_ok=True)
    out_file = security_master_dir / "security_master.parquet"
    if out_file.exists():
        out_file.unlink()

    con = duckdb.connect()
    dataset_glob_or_empty(
        con,
        "factors",
        factors_by_symbol_dir,
        """
        SELECT
            NULL::VARCHAR AS symbol,
            NULL::DATE AS date,
            NULL::DOUBLE AS return_factor
        WHERE false
        """,
    )
    dataset_glob_or_empty(
        con,
        "dividends",
        dividends_by_symbol_dir,
        """
        SELECT
            NULL::VARCHAR AS symbol,
            NULL::DATE AS date,
            NULL::DOUBLE AS cash_dividend,
            NULL::VARCHAR AS currency,
            NULL::VARCHAR AS source
        WHERE false
        """,
    )
    dataset_glob_or_empty(
        con,
        "shares",
        shares_by_symbol_dir,
        """
        SELECT
            NULL::VARCHAR AS symbol,
            NULL::DATE AS date,
            NULL::BIGINT AS shares_outstanding,
            NULL::VARCHAR AS cik,
            NULL::DATE AS filed_date,
            NULL::VARCHAR AS form,
            NULL::VARCHAR AS accession,
            NULL::VARCHAR AS source
        WHERE false
        """,
    )
    dataset_glob_or_empty(
        con,
        "sectors",
        sectors_by_symbol_dir,
        """
        SELECT
            NULL::VARCHAR AS symbol,
            NULL::DATE AS date,
            NULL::VARCHAR AS security_name,
            NULL::VARCHAR AS cik,
            NULL::VARCHAR AS sec_sic,
            NULL::VARCHAR AS sec_sic_description,
            NULL::VARCHAR AS gics_sector_code,
            NULL::VARCHAR AS gics_sector,
            NULL::VARCHAR AS gics_sub_industry,
            NULL::VARCHAR AS gics_source,
            NULL::VARCHAR AS source
        WHERE false
        """,
    )
    con.execute(
        f"""
        COPY (
            WITH factors_with_shares AS (
                SELECT
                    f.symbol,
                    f.date,
                    f.return_factor,
                    sh.date AS shares_as_of_date,
                    sh.shares_outstanding,
                    sh.filed_date AS shares_filed_date,
                    sh.cik AS shares_cik
                FROM (
                    SELECT symbol, date, return_factor
                    FROM factors
                    ORDER BY symbol, date
                ) f
                ASOF LEFT JOIN (
                    SELECT symbol, date, shares_outstanding, filed_date, cik
                    FROM shares
                    ORDER BY symbol, date
                ) sh
                ON f.symbol = sh.symbol
               AND f.date >= sh.date
            )
            SELECT
                f.date::DATE AS date,
                f.symbol::VARCHAR AS symbol,
                f.return_factor::DOUBLE AS cumulative_adjustment_factor,
                coalesce(d.cash_dividend, 0.0)::DOUBLE AS cash_dividend,
                d.currency::VARCHAR AS dividend_currency,
                f.shares_outstanding::BIGINT AS shares_outstanding,
                f.shares_as_of_date::DATE AS shares_as_of_date,
                f.shares_filed_date::DATE AS shares_filed_date,
                coalesce(nullif(sec.cik, ''), f.shares_cik)::VARCHAR AS sec_cik,
                nullif(sec.sec_sic, '')::VARCHAR AS sec_sic,
                nullif(sec.sec_sic_description, '')::VARCHAR AS sec_sic_description,
                nullif(sec.gics_sector_code, '')::VARCHAR AS gics_sector_code,
                nullif(sec.gics_sector, '')::VARCHAR AS gics_sector,
                nullif(sec.gics_sub_industry, '')::VARCHAR AS gics_sub_industry,
                nullif(sec.gics_source, '')::VARCHAR AS gics_source
            FROM factors_with_shares f
            LEFT JOIN dividends d
              ON f.symbol = d.symbol
             AND f.date = d.date
            LEFT JOIN sectors sec
              ON f.symbol = sec.symbol
            ORDER BY f.date, f.symbol
        )
        TO {sql_quote(out_file.as_posix())}
        (
            FORMAT PARQUET,
            COMPRESSION ZSTD,
            ROW_GROUP_SIZE 122880
        )
        """
    )
    stats = stats_for_security_master(out_file)
    stats["path"] = str(out_file)
    stats["schema"] = SECURITY_MASTER_SCHEMA.names
    return stats


def write_json(path: Path, payload: dict[str, Any] | list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_json_if_exists(path: Path, default: Any) -> Any:
    if not path.exists():
        return default
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return default


def rows_by_symbol(rows: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
    out: dict[str, dict[str, Any]] = {}
    for row in rows:
        symbol = str(row.get("symbol") or "")
        if symbol:
            out[symbol] = row
    return out


def previous_weekday(date_value: dt.date) -> dt.date:
    while date_value.weekday() >= 5:
        date_value -= dt.timedelta(days=1)
    return date_value


def requested_target_date(args: argparse.Namespace, end_epoch: int) -> dt.date:
    if args.end_date:
        return previous_weekday(dt.date.fromisoformat(args.end_date))
    return previous_weekday(epoch_to_date(end_epoch))


def symbol_partition_exists(root: Path, symbol: str) -> bool:
    partition = root / f"symbol={symbol}"
    return partition.exists() and any(partition.rglob("*.parquet"))


def manifest_row_covers_request(
    row: dict[str, Any],
    args: argparse.Namespace,
    target_date: dt.date,
) -> bool:
    if row.get("data_version") != DATA_VERSION:
        return False
    if int(row.get("requested_start_epoch") or DEFAULT_START_EPOCH) > args.start_epoch:
        return False
    max_date = date_or_none(row.get("max_date"))
    return max_date is not None and max_date >= target_date


def yahoo_symbol_complete(
    record: SymbolRecord,
    row: dict[str, Any] | None,
    args: argparse.Namespace,
    target_date: dt.date,
    factors_by_symbol_dir: Path,
    dividends_by_symbol_dir: Path,
    events_by_symbol_dir: Path,
) -> bool:
    if args.force_symbols or args.refresh_cache or row is None:
        return False
    if not manifest_row_covers_request(row, args, target_date):
        return False
    if not symbol_partition_exists(factors_by_symbol_dir, record.symbol):
        return False
    if int(row.get("dividend_events") or 0) > 0 and not symbol_partition_exists(
        dividends_by_symbol_dir,
        record.symbol,
    ):
        return False
    if not args.skip_events and int(row.get("split_events") or 0) > 0 and not symbol_partition_exists(
        events_by_symbol_dir,
        record.symbol,
    ):
        return False
    return True


def reference_symbol_complete(
    record: SymbolRecord,
    row: dict[str, Any] | None,
    args: argparse.Namespace,
    shares_by_symbol_dir: Path,
    sectors_by_symbol_dir: Path,
) -> bool:
    if args.force_symbols or args.refresh_cache or row is None:
        return False
    if row.get("data_version") != DATA_VERSION:
        return False
    if int(row.get("shares_rows") or 0) > 0 and not symbol_partition_exists(
        shares_by_symbol_dir,
        record.symbol,
    ):
        return False
    if int(row.get("sector_rows") or 0) > 0 and not symbol_partition_exists(
        sectors_by_symbol_dir,
        record.symbol,
    ):
        return False
    return True


def build(args: argparse.Namespace) -> dict[str, Any]:
    groups = selected_dataset_groups(args)
    build_yahoo = "yahoo" in groups
    build_sec = "sec" in groups
    build_security_master = "security-master" in groups and not args.skip_security_master

    output_root = args.output_root
    factors_by_symbol_dir = output_root / "factors_by_symbol"
    factors_by_date_dir = output_root / "factors_by_date"
    events_by_symbol_dir = output_root / "split_events_by_symbol"
    dividends_by_symbol_dir = output_root / "dividends_by_symbol"
    shares_by_symbol_dir = output_root / "shares_outstanding_by_symbol"
    sectors_by_symbol_dir = output_root / "sectors_by_symbol"
    security_master_dir = output_root / "security_master"
    legacy_date_dir = output_root / "parquet"
    cache_root = output_root / "_cache"
    yahoo_cache_dir = cache_root / "yahoo_chart_div_splits"
    sec_cache_dir = cache_root / "sec"
    reference_cache_dir = cache_root / "open_reference"
    stage_root = output_root / "_staging" / args.mode
    factor_stage_dir = stage_root / "factors"
    event_stage_dir = stage_root / "events"
    dividend_stage_dir = stage_root / "dividends"
    shares_stage_dir = stage_root / "shares"
    sectors_stage_dir = stage_root / "sectors"
    manifest_dir = output_root / "manifest"

    output_root.mkdir(parents=True, exist_ok=True)
    yahoo_cache_dir.mkdir(parents=True, exist_ok=True)
    sec_cache_dir.mkdir(parents=True, exist_ok=True)
    reference_cache_dir.mkdir(parents=True, exist_ok=True)
    manifest_dir.mkdir(parents=True, exist_ok=True)

    generated_targets = [stage_root, legacy_date_dir]
    if build_yahoo:
        generated_targets.extend([factors_by_symbol_dir, dividends_by_symbol_dir])
        if not args.skip_date_mirror:
            generated_targets.append(factors_by_date_dir)
        if not args.skip_events:
            generated_targets.append(events_by_symbol_dir)
    if build_sec:
        generated_targets.extend([shares_by_symbol_dir, sectors_by_symbol_dir])
    if build_security_master:
        generated_targets.append(security_master_dir)

    safe_rmtree(stage_root, output_root)
    if args.overwrite:
        for target in generated_targets:
            safe_rmtree(target, output_root)
    elif not args.incremental:
        existing = [path for path in generated_targets if path.exists() and any(path.iterdir())]
        if existing:
            joined = ", ".join(str(path) for path in existing)
            raise RuntimeError(
                f"generated output already exists at {joined}; pass --overwrite or --incremental"
            )

    if build_yahoo:
        factor_stage_dir.mkdir(parents=True, exist_ok=True)
        dividend_stage_dir.mkdir(parents=True, exist_ok=True)
        if not args.skip_events:
            event_stage_dir.mkdir(parents=True, exist_ok=True)
    if build_sec:
        shares_stage_dir.mkdir(parents=True, exist_ok=True)
        sectors_stage_dir.mkdir(parents=True, exist_ok=True)

    records = load_symbol_universe(args) if (build_yahoo or build_sec) else []
    if (build_yahoo or build_sec) and not records:
        raise RuntimeError("symbol universe is empty")

    end_epoch = end_epoch_from_args(args.end_date)
    as_of_date = epoch_to_date(end_epoch)
    target_date = requested_target_date(args, end_epoch)
    if build_yahoo or build_sec:
        logging.info(
            "building groups %s in %s mode for %d symbols",
            sorted(groups),
            args.mode,
            len(records),
        )
    else:
        logging.info("building groups %s in %s mode", sorted(groups), args.mode)
    logging.info("output root: %s", output_root)
    logging.info("requested backfill start epoch=%s target_date=%s", args.start_epoch, target_date)

    previous_yahoo_rows = rows_by_symbol(
        read_json_if_exists(manifest_dir / f"{args.mode}_symbols_succeeded.json", [])
    )
    previous_reference_rows = rows_by_symbol(
        read_json_if_exists(manifest_dir / f"{args.mode}_reference_symbols_succeeded.json", [])
    )

    yahoo_records = records
    reference_records = records
    skipped_yahoo: list[dict[str, Any]] = []
    skipped_reference: list[dict[str, Any]] = []
    if args.incremental and build_yahoo:
        todo: list[SymbolRecord] = []
        for record in records:
            previous = previous_yahoo_rows.get(record.symbol)
            if yahoo_symbol_complete(
                record,
                previous,
                args,
                target_date,
                factors_by_symbol_dir,
                dividends_by_symbol_dir,
                events_by_symbol_dir,
            ):
                skipped = dict(previous)
                skipped["skipped_this_run"] = True
                skipped_yahoo.append(skipped)
            else:
                todo.append(record)
        yahoo_records = todo
        logging.info(
            "yahoo planner: %d requested, %d complete, %d to build",
            len(records),
            len(skipped_yahoo),
            len(yahoo_records),
        )
    if args.incremental and build_sec:
        todo = []
        for record in records:
            previous = previous_reference_rows.get(record.symbol)
            if reference_symbol_complete(
                record,
                previous,
                args,
                shares_by_symbol_dir,
                sectors_by_symbol_dir,
            ):
                skipped = dict(previous)
                skipped["skipped_this_run"] = True
                skipped_reference.append(skipped)
            else:
                todo.append(record)
        reference_records = todo
        logging.info(
            "sec planner: %d requested, %d complete, %d to build",
            len(records),
            len(skipped_reference),
            len(reference_records),
        )

    successes: list[dict[str, Any]] = []
    failures: list[dict[str, Any]] = []
    reference_successes: list[dict[str, Any]] = []
    reference_failures: list[dict[str, Any]] = []
    factor_stage_files = 0
    factor_staged_rows = 0
    event_stage_files = 0
    event_staged_rows = 0
    dividend_stage_files = 0
    dividend_staged_rows = 0
    shares_stage_files = 0
    shares_staged_rows = 0
    sectors_stage_files = 0
    sectors_staged_rows = 0

    yahoo_limiter = RequestLimiter(args.yahoo_min_interval)
    sec_limiter = RequestLimiter(args.sec_min_interval)

    if build_yahoo:
        for batch_index, batch in enumerate(chunks(yahoo_records, args.batch_size), start=1):
            logging.info(
                "yahoo batch %d/%d: %d symbols",
                batch_index,
                math.ceil(len(yahoo_records) / args.batch_size),
                len(batch),
            )
            frames: list[pd.DataFrame] = []
            event_frames: list[pd.DataFrame] = []
            dividend_frames: list[pd.DataFrame] = []
            with futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
                pending = [
                    executor.submit(
                        process_symbol,
                        record,
                        args,
                        yahoo_cache_dir,
                        end_epoch,
                        yahoo_limiter,
                    )
                    for record in batch
                ]
                for future in futures.as_completed(pending):
                    result = future.result()
                    if result.ok and result.frame is not None:
                        frames.append(result.frame)
                        if (
                            not args.skip_events
                            and result.event_frame is not None
                            and not result.event_frame.empty
                        ):
                            event_frames.append(result.event_frame)
                        if result.dividend_frame is not None and not result.dividend_frame.empty:
                            dividend_frames.append(result.dividend_frame)
                        successes.append(
                            {
                                "symbol": result.symbol,
                                "yahoo_symbol": result.yahoo_symbol,
                                "rows": result.rows,
                                "min_date": result.min_date,
                                "max_date": result.max_date,
                                "split_events": result.split_events,
                                "dividend_events": (
                                    len(result.dividend_frame)
                                    if result.dividend_frame is not None
                                    else 0
                                ),
                                "currency": result.currency,
                                "data_version": DATA_VERSION,
                                "requested_start_epoch": args.start_epoch,
                                "target_date": str(target_date),
                            }
                        )
                    else:
                        failures.append({"symbol": result.symbol, "error": result.error})
                        logging.warning("%s failed: %s", result.symbol, result.error)

            if frames:
                batch_path = factor_stage_dir / f"batch-{batch_index:06d}.parquet"
                rows = write_staging_batch(
                    batch_path,
                    frames,
                    FINAL_SCHEMA,
                    ["symbol", "date"],
                )
                factor_stage_files += 1
                factor_staged_rows += rows
                logging.info("wrote %s factor rows to %s", rows, batch_path)

            if event_frames:
                event_batch_path = event_stage_dir / f"batch-{batch_index:06d}.parquet"
                rows = write_staging_batch(
                    event_batch_path,
                    event_frames,
                    EVENT_SCHEMA,
                    ["symbol", "date"],
                )
                event_stage_files += 1
                event_staged_rows += rows
                logging.info("wrote %s split event rows to %s", rows, event_batch_path)

            if dividend_frames:
                dividend_batch_path = dividend_stage_dir / f"batch-{batch_index:06d}.parquet"
                rows = write_staging_batch(
                    dividend_batch_path,
                    dividend_frames,
                    DIVIDEND_SCHEMA,
                    ["symbol", "date"],
                )
                dividend_stage_files += 1
                dividend_staged_rows += rows
                logging.info("wrote %s dividend rows to %s", rows, dividend_batch_path)

        if factor_stage_files == 0 and not has_parquet_files(factors_by_symbol_dir):
            raise RuntimeError("no factor staging rows were written")

    if build_sec:
        cik_map = load_sec_ticker_map(args, sec_cache_dir, sec_limiter)
        sp500_map = load_sp500_gics_map(args, reference_cache_dir)
        for batch_index, batch in enumerate(chunks(reference_records, args.batch_size), start=1):
            logging.info(
                "sec batch %d/%d: %d symbols",
                batch_index,
                math.ceil(len(reference_records) / args.batch_size),
                len(batch),
            )
            shares_frames: list[pd.DataFrame] = []
            sector_frames: list[pd.DataFrame] = []
            with futures.ThreadPoolExecutor(max_workers=args.sec_workers) as executor:
                pending = [
                    executor.submit(
                        process_reference_symbol,
                        record,
                        args,
                        cik_map,
                        sp500_map,
                        sec_cache_dir,
                        as_of_date,
                        target_date,
                        sec_limiter,
                    )
                    for record in batch
                ]
                for future in futures.as_completed(pending):
                    result = future.result()
                    if result.ok:
                        if result.shares_frame is not None and not result.shares_frame.empty:
                            shares_frames.append(result.shares_frame)
                        if result.sector_frame is not None and not result.sector_frame.empty:
                            sector_frames.append(result.sector_frame)
                        reference_successes.append(
                            {
                                "symbol": result.symbol,
                                "cik": result.cik,
                                "shares_rows": result.shares_rows,
                                "sector_rows": result.sector_rows,
                                "warning": result.error,
                                "data_version": DATA_VERSION,
                                "target_date": str(target_date),
                            }
                        )
                        if result.error:
                            logging.warning("%s SEC partial coverage: %s", result.symbol, result.error)
                    else:
                        reference_failures.append({"symbol": result.symbol, "error": result.error})
                        logging.warning("%s SEC failed: %s", result.symbol, result.error)

            if shares_frames:
                shares_batch_path = shares_stage_dir / f"batch-{batch_index:06d}.parquet"
                rows = write_staging_batch(
                    shares_batch_path,
                    shares_frames,
                    SHARES_SCHEMA,
                    ["symbol", "date"],
                )
                shares_stage_files += 1
                shares_staged_rows += rows
                logging.info("wrote %s shares rows to %s", rows, shares_batch_path)

            if sector_frames:
                sectors_batch_path = sectors_stage_dir / f"batch-{batch_index:06d}.parquet"
                rows = write_staging_batch(
                    sectors_batch_path,
                    sector_frames,
                    SECTOR_SCHEMA,
                    ["symbol"],
                )
                sectors_stage_files += 1
                sectors_staged_rows += rows
                logging.info("wrote %s sector rows to %s", rows, sectors_batch_path)

    factors_by_symbol_stats: dict[str, Any] | None = None
    factors_by_date_stats: dict[str, Any] | None = None
    events_by_symbol_stats: dict[str, Any] | None = None
    dividends_by_symbol_stats: dict[str, Any] | None = None
    shares_by_symbol_stats: dict[str, Any] | None = None
    sectors_by_symbol_stats: dict[str, Any] | None = None
    security_master_stats: dict[str, Any] | None = None

    if build_yahoo:
        if args.incremental:
            remove_symbol_partitions(factors_by_symbol_dir, yahoo_records, output_root)
            remove_symbol_partitions(dividends_by_symbol_dir, yahoo_records, output_root)
            if not args.skip_events:
                remove_symbol_partitions(events_by_symbol_dir, yahoo_records, output_root)
        if factor_stage_files > 0:
            factors_by_symbol_stats = finalize_factor_dataset(
                factor_stage_dir,
                factors_by_symbol_dir,
                partition_by="symbol",
                order_by="symbol, date",
                merge_existing=args.incremental,
            )
        elif has_parquet_files(factors_by_symbol_dir):
            factors_by_symbol_stats = stats_for_dataset(factors_by_symbol_dir)
        if not args.skip_date_mirror:
            safe_rmtree(factors_by_date_dir, output_root)
            if args.incremental or factor_stage_files == 0:
                factors_by_date_stats = materialize_factor_date_mirror_from_symbol(
                    factors_by_symbol_dir,
                    factors_by_date_dir,
                )
            else:
                factors_by_date_stats = finalize_factor_dataset(
                    factor_stage_dir,
                    factors_by_date_dir,
                    partition_by="date",
                    order_by="date, symbol",
                )
        if not args.skip_events and event_stage_files > 0:
            events_by_symbol_stats = finalize_event_dataset(
                event_stage_dir,
                events_by_symbol_dir,
                merge_existing=args.incremental,
            )
        elif args.incremental and has_parquet_files(events_by_symbol_dir):
            events_by_symbol_stats = stats_for_dataset(events_by_symbol_dir)
        if dividend_stage_files > 0:
            dividends_by_symbol_stats = finalize_dividend_dataset(
                dividend_stage_dir,
                dividends_by_symbol_dir,
                merge_existing=args.incremental,
            )
        elif args.incremental and has_parquet_files(dividends_by_symbol_dir):
            dividends_by_symbol_stats = stats_for_dataset(dividends_by_symbol_dir)

    if build_sec:
        if args.incremental:
            remove_symbol_partitions(shares_by_symbol_dir, reference_records, output_root)
            remove_symbol_partitions(sectors_by_symbol_dir, reference_records, output_root)
        if shares_stage_files > 0:
            shares_by_symbol_stats = finalize_shares_dataset(
                shares_stage_dir,
                shares_by_symbol_dir,
                merge_existing=args.incremental,
            )
        elif args.incremental and has_parquet_files(shares_by_symbol_dir):
            shares_by_symbol_stats = stats_for_dataset(shares_by_symbol_dir)
        if sectors_stage_files > 0:
            sectors_by_symbol_stats = finalize_sector_dataset(
                sectors_stage_dir,
                sectors_by_symbol_dir,
                merge_existing=args.incremental,
            )
        elif args.incremental and has_parquet_files(sectors_by_symbol_dir):
            sectors_by_symbol_stats = stats_for_dataset(sectors_by_symbol_dir)

    if build_security_master:
        if not factors_by_symbol_dir.exists():
            raise RuntimeError(
                f"{factors_by_symbol_dir} is required for security master materialization"
            )
        security_master_stats = materialize_security_master(
            factors_by_symbol_dir,
            dividends_by_symbol_dir,
            shares_by_symbol_dir,
            sectors_by_symbol_dir,
            security_master_dir,
        )

    if not args.keep_staging:
        safe_rmtree(stage_root, output_root)

    outputs: dict[str, Any] = {
        "factors_by_symbol": None,
        "factors_by_date": None,
        "split_events_by_symbol": None,
        "dividends_by_symbol": None,
        "shares_outstanding_by_symbol": None,
        "sectors_by_symbol": None,
        "security_master": None,
    }
    if factors_by_symbol_stats is not None:
        outputs["factors_by_symbol"] = {
            "path": str(factors_by_symbol_dir),
            "schema": ["symbol", "date", "return_factor"],
            "partitioning": "hive/symbol=SYMBOL",
            "canonical": True,
            **factors_by_symbol_stats,
        }
    if factors_by_date_stats is not None:
        outputs["factors_by_date"] = {
            "path": str(factors_by_date_dir),
            "schema": ["symbol", "date", "return_factor"],
            "partitioning": "hive/date=YYYY-MM-DD",
            "canonical": False,
            **factors_by_date_stats,
        }
    if events_by_symbol_stats is not None:
        outputs["split_events_by_symbol"] = {
            "path": str(events_by_symbol_dir),
            "schema": ["symbol", "date", "split_factor"],
            "partitioning": "hive/symbol=SYMBOL",
            "canonical": True,
            **events_by_symbol_stats,
        }
    if dividends_by_symbol_stats is not None:
        outputs["dividends_by_symbol"] = {
            "path": str(dividends_by_symbol_dir),
            "schema": DIVIDEND_SCHEMA.names,
            "partitioning": "hive/symbol=SYMBOL",
            "canonical": True,
            **dividends_by_symbol_stats,
        }
    if shares_by_symbol_stats is not None:
        outputs["shares_outstanding_by_symbol"] = {
            "path": str(shares_by_symbol_dir),
            "schema": SHARES_SCHEMA.names,
            "partitioning": "hive/symbol=SYMBOL",
            "canonical": True,
            **shares_by_symbol_stats,
        }
    if sectors_by_symbol_stats is not None:
        outputs["sectors_by_symbol"] = {
            "path": str(sectors_by_symbol_dir),
            "schema": SECTOR_SCHEMA.names,
            "partitioning": "hive/symbol=SYMBOL",
            "canonical": True,
            **sectors_by_symbol_stats,
        }
    if security_master_stats is not None:
        outputs["security_master"] = {
            "partitioning": "single parquet file",
            "canonical": False,
            **security_master_stats,
        }

    all_yahoo_successes = successes + skipped_yahoo
    all_reference_successes = reference_successes + skipped_reference
    primary_output = (
        outputs.get("security_master")
        or outputs.get("factors_by_date")
        or outputs.get("factors_by_symbol")
        or {}
    )

    manifest = {
        "created_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "data_version": DATA_VERSION,
        "mode": args.mode,
        "dataset_groups": sorted(groups),
        "sources": {
            "nasdaq_listed": NASDAQ_LISTED_URL,
            "other_listed": OTHER_LISTED_URL,
            "yahoo_chart": "https://query1.finance.yahoo.com/v8/finance/chart/{symbol}",
            "sec_company_tickers": SEC_COMPANY_TICKERS_URL,
            "sec_companyfacts": "https://data.sec.gov/api/xbrl/companyfacts/CIK{cik}.json",
            "sec_submissions": "https://data.sec.gov/submissions/CIK{cik}.json",
            "sp500_constituents": args.sp500_url,
        },
        "universe": {
            "requested_symbols": len(records),
            "include_etfs": bool(args.include_etfs),
            "include_test_issues": bool(args.include_test_issues),
            "common_only": bool(args.common_only),
            "symbol_regex": args.symbol_regex,
            "limit": args.limit,
        },
        "download": {
            "start_epoch": args.start_epoch,
            "end_epoch": end_epoch,
            "refresh_cache": bool(args.refresh_cache),
            "workers": args.workers,
            "sec_workers": args.sec_workers,
        },
        "output": {
            "root": str(output_root),
            "parquet": primary_output.get("path"),
            "schema": primary_output.get("schema"),
            "partitioning": primary_output.get("partitioning"),
            "rows": primary_output.get("rows"),
            "symbols": primary_output.get("symbols"),
            "dates": primary_output.get("dates"),
            "min_date": primary_output.get("min_date"),
            "max_date": primary_output.get("max_date"),
        },
        "outputs": outputs,
        "staging": {
            "factor_files": factor_stage_files,
            "factor_rows": factor_staged_rows,
            "event_files": event_stage_files,
            "event_rows": event_staged_rows,
            "dividend_files": dividend_stage_files,
            "dividend_rows": dividend_staged_rows,
            "shares_files": shares_stage_files,
            "shares_rows": shares_staged_rows,
            "sectors_files": sectors_stage_files,
            "sectors_rows": sectors_staged_rows,
            "kept": bool(args.keep_staging),
        },
        "symbols_succeeded": len(all_yahoo_successes),
        "symbols_failed": len(failures),
        "symbols_skipped": len(skipped_yahoo),
        "reference_symbols_succeeded": len(all_reference_successes),
        "reference_symbols_failed": len(reference_failures),
        "reference_symbols_skipped": len(skipped_reference),
    }

    write_json(manifest_dir / f"{args.mode}_manifest.json", manifest)
    if build_yahoo:
        write_json(manifest_dir / f"{args.mode}_symbols_succeeded.json", all_yahoo_successes)
        write_json(manifest_dir / f"{args.mode}_symbols_failed.json", failures)
    if build_sec:
        write_json(
            manifest_dir / f"{args.mode}_reference_symbols_succeeded.json",
            all_reference_successes,
        )
        write_json(manifest_dir / f"{args.mode}_reference_symbols_failed.json", reference_failures)
    return manifest


def main() -> int:
    args = parse_args()
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%H:%M:%S",
    )
    try:
        manifest = build(args)
    except Exception as exc:  # noqa: BLE001 - CLI boundary.
        logging.error("%s", exc)
        return 1

    for name, output in manifest["outputs"].items():
        if not output:
            continue
        rows = output.get("rows")
        symbols = output.get("symbols")
        dates = output.get("dates")
        path = output.get("path")
        print(f"built {name}: {rows} rows for {symbols} symbols across {dates} dates: {path}")
    if manifest["symbols_failed"]:
        print(
            f"warning: {manifest['symbols_failed']} symbols failed; see "
            f"{args.output_root / 'manifest' / (args.mode + '_symbols_failed.json')}",
            file=sys.stderr,
        )
    if manifest["reference_symbols_failed"]:
        print(
            f"warning: {manifest['reference_symbols_failed']} reference symbols failed; see "
            f"{args.output_root / 'manifest' / (args.mode + '_reference_symbols_failed.json')}",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
