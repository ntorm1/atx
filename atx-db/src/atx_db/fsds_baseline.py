"""P12: external validation of standardized fundamentals against SEC Financial Statement Data Sets.

The SEC (DERA) publishes quarterly Financial Statement Data Sets (FSDS): ``sub.txt`` (one row per
submission: ``adsh, cik, form, period, fy, fp, filed, accepted, prevrpt``), ``num.txt`` (one row
per numeric XBRL fact: ``adsh, tag, version, ddate, qtrs, uom, [segments,] coreg, value``) and
``tag.txt`` (tag definitions with the ``custom`` extension flag). FSDS is SEC's own extraction of
the filings the warehouse ingests through CompanyFacts, so agreement on (issuer, canonical item,
period, filing) is an independent check of ingestion, period selection, revision handling and
standardization.

Pipeline (bounded; nothing here opens the governed warehouse by itself):

1. :func:`fetch_fsds_quarters` -- quarterly zips (ruling RX10's eight-zip cap is lifted by gate U3;
   ``max_zips`` still enforces one when passed), approved SEC user agent, paced by the host-wide
   ``sec_http`` limiter (<= 5 requests/s across all workers), streamed into ``data/cache/P12-fsds``
   with a SHA-256 manifest. A cached zip is re-hashed and reused, never downloaded again.
2. :func:`load_fsds_subset` -- streams ``sub/num/tag`` out of each zip into a temporary file and
   reads it with DuckDB (memory_limit 384MB, 2 threads), keeping only the panel issuers'
   submissions, into a small standalone subset DB (idempotent per quarter + zip hash + panel).
3. :func:`canonical_fsds_facts` -- maps ``(tag, version, ddate, qtrs, uom)`` to the warehouse
   canonical ``(item_id, basis, period)`` with the warehouse's own ``standardization_rules.csv``
   alias priorities and ``fundamental_items.csv`` alias validity windows: per filing the
   lowest-priority us-gaap alias wins; ``coalesce_or_difference``/``coalesce_or_sum`` rules fall
   back to the raw component items reported in the same filing (mirrors
   ``_standardization_set_based``: compositions read raw items, never other rules' outputs).
   Dimensional (``segments``) and co-registrant (``coreg``) facts are excluded.
4. :func:`benchmark_coverage` -- the 50 issuers x 5 fiscal years x 10 core items grid; every
   unmapped cell carries a reason (window, IFRS, dimensional-only, unit, incomplete derivation,
   us-gaap alias gap with candidate tags, custom extension, not reported).
5. :func:`run_fsds_comparison` -- loads the latest FSDS vintage per key into
   ``vendor_baseline_facts`` and scores it with ``fact_disagreement`` (spec tolerance 0.5 % or
   $1M; per-share items 0.5 % or $0.005), then classifies every row against the warehouse
   revision filed in the *same accession*: agreement, vintage difference (warehouse carries a later
   re-report), value mismatch, or missing warehouse period/item.

FSDS ``ddate``/``period`` are rounded to the nearest month end; the comparison aligns them to the
warehouse's exact ``period_end`` (nearest within 15 days) before scoring. FSDS ``accepted`` is the
EDGAR acceptance timestamp as published -- America/New_York wall time without a zone (not a warehouse
PIT clock); in the P12 subset DB it only orders FSDS vintages.

Full history (node X.4; user gate U3 granted, rulings C-42 / C-76; lifts the RX10 eight-zip cap):

6. :func:`fetch_fsds_history` -- one GET of SEC's FSDS index page decides which quarters are
   published; every listed quarter from 2009q2 on is fetched once into the same cache + manifest
   (retained zips are re-hashed and reused), through ``sec_http`` with at most two attempts per
   request, so this job alone can never trip the host SEC block (5 consecutive episodes).
7. :func:`stage_fsds_history` -- streams each quarter's ``sub/num/tag/pre`` out of its zip into
   ``data/staging/fsds-v2/<table>/<quarter>.parquet`` (DuckDB 256MB / 1 thread, no warehouse), every
   source column kept with strict typed casts (custom tags, segments, coreg all kept); resumable per
   quarter, the staging manifest being the commit record. Stager v2 (X.4 fix round 1): FSDS
   ``accepted`` is EDGAR's America/New_York wall clock published without a zone, so it is staged as
   ``accepted_et_naive`` (as published) plus ``accepted_utc`` (TIMESTAMPTZ, DST-aware conversion) --
   see :data:`ACCEPTED_CLOCK`; ``num.value`` is exact ``DECIMAL(28,4)`` (:data:`VALUE_TYPE`). Each file
   carries Parquet key/value metadata (stager version, quarter, zip sha256, clock/value notes).
   :func:`verify_fsds_staging` proves completeness against the SEC index, re-hashes the files, proves
   one file (and one quarter value) per quarter and table with row counts equal to the manifest, the
   schema and metadata of every file, and the recorded DST and NUM-orphan findings.

Run ``python -m atx_db.fsds_baseline history-fetch | stage | verify`` (see :func:`main`).
"""

from __future__ import annotations

import calendar
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import time
import uuid
import zipfile
from collections.abc import Callable, Iterable, Iterator, Mapping, Sequence
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from urllib.parse import urljoin, urlsplit

import duckdb
import pandas as pd

from .connection import DuckDBStore, bounded_config, resolve_data_dir
from .fact_disagreement import FactDisagreementOptions, refresh_fact_disagreement
from .item_registry import FundamentalItemSeedRow, read_fundamental_item_seed
from .sec_http import (
    APPROVED_SEC_USER_AGENT,
    SecBlockedError,
    SecRateLimiter,
    default_sec_limiter,
    read_bounded_response,
    sec_session,
)
from .standardization import StandardizationRule, default_standardization_rules, rule_input_kinds
from .statement_map_seed import FundamentalStatementMapRow, read_statement_map_seed

FSDS_URL_TEMPLATE = "https://www.sec.gov/files/dera/data/financial-statement-data-sets/{quarter}.zip"
SEC_USER_AGENT = APPROVED_SEC_USER_AGENT
RX10_MAX_ZIPS = 8
CACHE_DIRNAME = "P12-fsds"
MANIFEST_NAME = "P12-fsds-manifest.json"
SUBSET_DB_NAME = "P12-fsds-subset.duckdb"
LIGHT_MEMORY_LIMIT = "384MB"
LIGHT_THREADS = 2
_COPY_CHUNK = 1 << 20

FSDS_VENDOR = "SEC_FSDS"
FSDS_BASELINE_SOURCE = "sec_fsds_v1"
FSDS_PARITY_SOURCE = "fsds_parity_v1"
TOLERANCE_REL = 0.005
TOLERANCE_ABS_MONETARY = 1_000_000.0
TOLERANCE_ABS_PER_SHARE = 0.005
PERIOD_ALIGN_DAYS = 15
GRID_MATCH_DAYS = 20
BENCHMARK_YEARS = 5

ANNUAL_FORMS = frozenset({"10-K", "10-K/A", "10-KT", "10-KT/A", "20-F", "20-F/A", "40-F", "40-F/A"})
_BASES_BY_KIND = {"duration": ("annual", "quarterly"), "instant": ("instant",)}
_BASIS_BY_QTRS = {4: "annual", 1: "quarterly", 0: "instant"}
_GRID_BASIS_BY_KIND = {"duration": "annual", "instant": "instant"}
_GRID_QTRS_BY_KIND = {"duration": 4, "instant": 0}
_UOM_BY_UNIT_KIND = {"monetary": "USD", "per_share": "USD/shares"}  # unit_type label on baseline rows
# FSDS publishes per-share facts (EarningsPerShareDiluted) with uom 'USD' (measured on all eight
# loaded quarters); 'USD/shares' is accepted too in case a release spells the ratio out.
_FSDS_UOMS_BY_UNIT_KIND = {"monetary": ("USD",), "per_share": ("USD", "USD/shares")}
_COMPOSITION_RULES = frozenset({"sum", "difference", "coalesce_or_sum", "coalesce_or_difference"})
_DIFFERENCE_RULES = frozenset({"difference", "coalesce_or_difference"})
_DIRECT_FIRST_RULES = frozenset({"identity", "coalesce_priority", "first_non_null", "coalesce_or_sum", "coalesce_or_difference"})


# ---------------------------------------------------------------------------------------------
# Benchmark panel, quarters and core items
# ---------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class BenchmarkIssuer:
    ticker: str
    cik: str
    segment: str
    filing_quarter: int  # calendar quarter in which the issuer's 10-K is filed


def _issuer(ticker: str, cik: int, segment: str, filing_quarter: int) -> BenchmarkIssuer:
    return BenchmarkIssuer(ticker, f"{cik:010d}", segment, filing_quarter)


# 50 US-GAAP 10-K filers across the industry templates (bank/broker/insurer/REIT/utility) and the
# three 10-K filing seasons the eight RX10 zips cover. CIKs are the historical filers: XOM is
# Exxon Mobil Corp (0000034088) -- the current SEC ticker map points XOM at a 2026 holding company
# (0002115436) that filed none of the FY2021-FY2025 10-Ks.
BENCHMARK_PANEL: tuple[BenchmarkIssuer, ...] = (
    _issuer("JPM", 19617, "bank", 1),
    _issuer("BAC", 70858, "bank", 1),
    _issuer("WFC", 72971, "bank", 1),
    _issuer("GS", 886982, "broker", 1),
    _issuer("SCHW", 316709, "broker", 1),
    _issuer("PGR", 80661, "insurer", 1),
    _issuer("TRV", 86312, "insurer", 1),
    _issuer("MET", 1099219, "insurer", 1),
    _issuer("PLD", 1045609, "reit", 1),
    _issuer("O", 726728, "reit", 1),
    _issuer("SPG", 1063761, "reit", 1),
    _issuer("AMT", 1053507, "reit", 1),
    _issuer("NEE", 753308, "utility", 1),
    _issuer("DUK", 1326160, "utility", 1),
    _issuer("SO", 92122, "utility", 1),
    _issuer("XOM", 34088, "energy", 1),
    _issuer("CVX", 93410, "energy", 1),
    _issuer("COP", 1163165, "energy", 1),
    _issuer("GOOGL", 1652044, "tech", 1),
    _issuer("META", 1326801, "tech", 1),
    _issuer("AMZN", 1018724, "tech", 1),
    _issuer("NVDA", 1045810, "tech", 1),
    _issuer("ADBE", 796343, "tech", 1),
    _issuer("IBM", 51143, "tech", 1),
    _issuer("JNJ", 200406, "health", 1),
    _issuer("PFE", 78003, "health", 1),
    _issuer("UNH", 731766, "health", 1),
    _issuer("MRK", 310158, "health", 1),
    _issuer("ABBV", 1551152, "health", 1),
    _issuer("LLY", 59478, "health", 1),
    _issuer("WMT", 104169, "consumer", 1),
    _issuer("HD", 354950, "consumer", 1),
    _issuer("KO", 21344, "consumer", 1),
    _issuer("PEP", 77476, "consumer", 1),
    _issuer("MCD", 63908, "consumer", 1),
    _issuer("CAT", 18230, "industrial", 1),
    _issuer("HON", 773840, "industrial", 1),
    _issuer("GE", 40545, "industrial", 1),
    _issuer("UPS", 1090727, "industrial", 1),
    _issuer("VZ", 732712, "telecom", 1),
    _issuer("T", 732717, "telecom", 1),
    _issuer("MSFT", 789019, "tech", 3),
    _issuer("PG", 80424, "consumer", 3),
    _issuer("CSCO", 858877, "tech", 3),
    _issuer("NKE", 320187, "consumer", 3),
    _issuer("FDX", 1048911, "industrial", 3),
    _issuer("AAPL", 320193, "tech", 4),
    _issuer("COST", 909832, "consumer", 4),
    _issuer("DIS", 1744489, "media", 4),
    _issuer("V", 1403161, "tech", 4),
)

# Eight zips (RX10 cap). A 10-K carries three fiscal years of income/cash-flow statement and two
# balance sheets, so alternate filing years cover five fiscal years per season:
#   Q1 filers (Dec/Jan/Nov FYE): 2022q1, 2024q1, 2026q1 -> FY2019-FY2025 IS/CF, FY2020-FY2025 BS
#   Q3 filers (May/Jun/Jul FYE): 2021q3, 2023q3, 2025q3 -> FY2019-FY2025 IS/CF, FY2020-FY2025 BS
#   Q4 filers (Aug/Sep FYE):     2023q4, 2025q4         -> FY2021-FY2025 IS/CF, FY2022-FY2025 BS
BENCHMARK_QUARTERS: tuple[str, ...] = (
    "2021q3",
    "2022q1",
    "2023q3",
    "2023q4",
    "2024q1",
    "2025q3",
    "2025q4",
    "2026q1",
)


@dataclass(frozen=True)
class CoreItem:
    name: str
    item_id: int
    canonical_code: str
    period_kind: str  # duration | instant
    unit_kind: str  # monetary | per_share
    keywords: tuple[str, ...]  # tag regexes used only to classify unmapped cells


CORE_ITEMS: tuple[CoreItem, ...] = (
    CoreItem("revenue", 1001, "revenue", "duration", "monetary",
             ("Revenue", "Sales", "PremiumsEarned", "InterestAndDividendIncomeOperating")),
    CoreItem("gross_profit", 1004, "gross_profit__1004", "duration", "monetary", ("GrossProfit",)),
    CoreItem("operating_income", 1014, "operating_income", "duration", "monetary",
             ("^OperatingIncome", "^OperatingProfit", "IncomeLossFromOperations")),
    CoreItem("net_income", 1031, "net_income_total", "duration", "monetary",
             ("NetIncomeLoss", "ProfitLoss", "NetIncome")),
    CoreItem("eps_diluted", 1035, "eps_diluted", "duration", "per_share",
             ("EarningsPerShareDiluted", "PerDilutedShare", "EarningsPerShareBasicAndDiluted")),
    CoreItem("total_assets", 1101, "total_assets", "instant", "monetary", ("^Assets$", "TotalAssets")),
    CoreItem("total_liabilities", 1201, "total_liabilities", "instant", "monetary", ("^Liabilities",)),
    CoreItem("common_equity", 1220, "common_equity", "instant", "monetary",
             ("StockholdersEquity", "CommonEquity")),
    CoreItem("cfo", 1301, "cash_flow_from_operations", "duration", "monetary", ("OperatingActivities",)),
    CoreItem("capex", 1305, "capex__1305", "duration", "monetary",
             ("PaymentsToAcquire(Property|Productive|OtherProductive|Machinery|Equipment|OilAndGas|RealEstate)",
              "CapitalExpenditure", "PaymentsForCapitalImprovements")),
)
# Keywords are RE2-compatible (they also run inside DuckDB regexp_matches) and only rank candidate
# tags for an unmapped cell; they never map a value.


def default_cache_dir() -> Path:
    return resolve_data_dir() / "cache" / CACHE_DIRNAME


def governed_warehouse_path() -> Path:
    return (resolve_data_dir() / "warehouse.duckdb").resolve()


def _normalize_cik(value: Any) -> str | None:
    if value is None:
        return None
    try:
        if pd.isna(value):
            return None
    except (TypeError, ValueError):
        pass
    token = str(value).strip()
    if not token:
        return None
    try:
        return f"{int(float(token)):010d}"
    except ValueError:
        return None


def _as_date(value: Any) -> dt.date | None:
    if value is None:
        return None
    if isinstance(value, dt.datetime):
        return value.date()
    if isinstance(value, dt.date):
        return value
    try:
        if pd.isna(value):
            return None
    except (TypeError, ValueError):
        pass
    token = str(value).strip()
    if not token:
        return None
    return pd.Timestamp(token).date()


def _month_end(year: int, month: int) -> dt.date:
    return dt.date(year, month, calendar.monthrange(year, month)[1])


def _within_tolerance(warehouse: float, vendor: float, abs_tol: float, rel_tol: float) -> bool:
    """Same agreement rule as ``fact_disagreement.compute_fact_disagreement_rows``."""

    diff = abs(float(warehouse) - float(vendor))
    rel = diff / abs(float(vendor)) if float(vendor) != 0 else diff
    return diff <= abs_tol or rel <= rel_tol


# ---------------------------------------------------------------------------------------------
# 1. Fetch (RX10)
# ---------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class FsdsArchive:
    quarter: str
    path: Path
    sha256: str
    size_bytes: int
    url: str
    downloaded_now: bool


def _validate_quarter(quarter: str) -> str:
    token = str(quarter).strip().lower()
    if len(token) != 6 or not token[:4].isdigit() or token[4] != "q" or token[5] not in "1234":
        raise ValueError(f"invalid FSDS quarter {quarter!r}; expected e.g. 2024q1")
    return token


def _sha256_file(path: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as handle:
        while chunk := handle.read(_COPY_CHUNK):
            digest.update(chunk)
            size += len(chunk)
    return digest.hexdigest(), size


def _read_manifest(cache_dir: Path) -> dict[str, Any]:
    path = cache_dir / MANIFEST_NAME
    if not path.is_file():
        return {"schema": "p12_fsds_manifest_v1", "archives": {}}
    return json.loads(path.read_text(encoding="utf-8"))


def _write_json_atomic(path: Path, payload: Mapping[str, Any]) -> None:
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    temporary.write_text(json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8")
    os.replace(temporary, path)


def _write_manifest(cache_dir: Path, manifest: Mapping[str, Any]) -> None:
    _write_json_atomic(cache_dir / MANIFEST_NAME, manifest)


@contextmanager
def _single_writer_lock(path: Path, *, timeout_s: float) -> Iterator[None]:
    """Hold an OS byte-range lock on ``path``; the OS releases it if the holder dies.

    Local to this module (X.4 review M3): the fetch and stage locks must not depend on ``sec_http`` internals.
    """

    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
    deadline = time.monotonic() + timeout_s
    try:
        if os.name == "nt":
            import msvcrt

            while True:
                os.lseek(fd, 0, os.SEEK_SET)
                try:
                    msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise TimeoutError(f"could not lock {path} within {timeout_s:.1f}s") from None
                    time.sleep(0.1)
            try:
                yield
            finally:
                os.lseek(fd, 0, os.SEEK_SET)
                msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            while True:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() > deadline:
                        raise TimeoutError(f"could not lock {path} within {timeout_s:.1f}s") from None
                    time.sleep(0.1)
            try:
                yield
            finally:
                fcntl.flock(fd, fcntl.LOCK_UN)
    finally:
        os.close(fd)


def cached_fsds_archives(cache_dir: Path | None = None, quarters: Iterable[str] | None = None) -> list[FsdsArchive]:
    """Return manifest-recorded zips (hash-verified) without any network access."""

    cache_dir = Path(cache_dir or default_cache_dir())
    manifest = _read_manifest(cache_dir)
    wanted = None if quarters is None else {_validate_quarter(q) for q in quarters}
    out: list[FsdsArchive] = []
    for quarter, entry in sorted(manifest.get("archives", {}).items()):
        if wanted is not None and quarter not in wanted:
            continue
        path = cache_dir / f"{quarter}.zip"
        if not path.is_file():
            raise FileNotFoundError(f"{path} recorded in {MANIFEST_NAME} but missing")
        digest, size = _sha256_file(path)
        if digest != entry["sha256"]:
            raise RuntimeError(f"{path} sha256 {digest} != recorded {entry['sha256']}")
        out.append(FsdsArchive(quarter, path, digest, size, entry["url"], False))
    return out


def _receipt_sidecar(zip_path: Path) -> Path:
    """``.<quarter>.zip.receipt.json``: a download's receipt, written before the zip is moved into place."""

    return zip_path.with_name(f".{zip_path.name}.receipt.json")


def _read_receipt_sidecar(path: Path) -> dict[str, Any] | None:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return payload if isinstance(payload, dict) else None


def fetch_fsds_quarters(
    quarters: Sequence[str] = BENCHMARK_QUARTERS,
    cache_dir: Path | None = None,
    *,
    session: Any | None = None,
    limiter: SecRateLimiter | None = None,
    max_zips: int | None = None,
    progress: Callable[[dict[str, Any]], None] | None = None,
) -> list[FsdsArchive]:
    """Download (once) and hash FSDS quarterly zips.

    A zip already on disk is re-hashed and reused -- never downloaded again; a manifest/zip hash
    mismatch or a recorded-but-missing zip is an error rather than a silent re-download. Any final
    status other than 200 raises, so a refused request stops the run. ``max_zips`` (e.g.
    :data:`RX10_MAX_ZIPS`) caps every distinct quarter ever recorded in the manifest, not just this
    call; ``None`` since user gate U3 granted the full FSDS history (ruling C-42). The default
    session is ``sec_http.sec_session``: every attempt takes a token from the host-wide limiter.
    """

    cache_dir = Path(cache_dir or default_cache_dir())
    cache_dir.mkdir(parents=True, exist_ok=True)
    wanted = list(dict.fromkeys(_validate_quarter(q) for q in quarters))
    manifest = _read_manifest(cache_dir)
    recorded: dict[str, Any] = manifest.setdefault("archives", {})
    if max_zips is not None and len(set(recorded) | set(wanted)) > max_zips:
        raise ValueError(
            f"at most {max_zips} FSDS zips allowed; recorded={sorted(recorded)} requested={wanted}"
        )

    results: list[FsdsArchive] = []
    for quarter in wanted:
        path = cache_dir / f"{quarter}.zip"
        url = FSDS_URL_TEMPLATE.format(quarter=quarter)
        entry = recorded.get(quarter)
        sidecar = _receipt_sidecar(path)
        if path.is_file():
            digest, size = _sha256_file(path)
            if entry is None:
                pending = _read_receipt_sidecar(sidecar)
                if pending is not None and pending.get("sha256") == digest and pending.get("bytes") == size:
                    # Killed between the zip's replace and the manifest write: the receipt survived beside it.
                    entry = {**pending, "note": "receipt recovered from its pre-replace sidecar"}
                else:
                    entry = {"quarter": quarter, "url": url, "sha256": digest, "bytes": size,
                             "fetched_at_utc": None, "note": "adopted pre-existing file; not downloaded"}
                recorded[quarter] = entry
                _write_manifest(cache_dir, manifest)
            elif entry["sha256"] != digest:
                raise RuntimeError(f"{path} sha256 {digest} != recorded {entry['sha256']}; refusing to re-download")
            sidecar.unlink(missing_ok=True)
            results.append(FsdsArchive(quarter, path, digest, size, url, False))
            if progress is not None:
                progress({"quarter": quarter, "fetch": "reused", "sha256": digest, "bytes": size})
            continue
        if entry is not None:
            raise RuntimeError(f"{quarter} is recorded in {MANIFEST_NAME} but {path} is missing; refusing to re-download")

        if session is None:
            session = sec_session(limiter=limiter)
        headers = {"User-Agent": SEC_USER_AGENT, "Accept-Encoding": "gzip, deflate"}
        temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.part")
        digest = hashlib.sha256()
        size = 0
        started = time.monotonic()
        requested_at = dt.datetime.now(dt.UTC).isoformat(timespec="milliseconds")
        try:
            with session.get(url, headers=headers, stream=True, timeout=(30, 300)) as response:
                status = int(response.status_code)
                if status != 200:
                    raise RuntimeError(f"GET {url} returned HTTP {status}")
                last_modified = response.headers.get("Last-Modified")
                declared = response.headers.get("Content-Length")
                with temporary.open("wb") as handle:
                    for chunk in response.iter_content(chunk_size=_COPY_CHUNK):
                        if chunk:
                            handle.write(chunk)
                            digest.update(chunk)
                            size += len(chunk)
            if declared and str(declared).isdigit() and int(declared) != size:
                raise RuntimeError(f"GET {url} delivered {size} bytes, Content-Length {declared}")
            with zipfile.ZipFile(temporary) as archive:
                members = sorted(archive.namelist())
            missing = {"sub.txt", "num.txt", "tag.txt"} - set(members)
            if missing:
                raise RuntimeError(f"{url} lacks FSDS members {sorted(missing)}")
            receipt = {
                "quarter": quarter,
                "url": url,
                "sha256": digest.hexdigest(),
                "bytes": size,
                "requested_at_utc": requested_at,
                "fetched_at_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
                "elapsed_s": round(time.monotonic() - started, 3),
                "user_agent": SEC_USER_AGENT,
                "http_status": status,
                "last_modified": last_modified,
                "members": members,
            }
            # The receipt is durable before the zip appears (X.4 review M6): a kill between the replace and the
            # manifest write leaves the sidecar, and the next run adopts the zip with its full receipt.
            _write_json_atomic(sidecar, receipt)
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)
        recorded[quarter] = receipt
        _write_manifest(cache_dir, manifest)
        sidecar.unlink(missing_ok=True)
        if progress is not None:
            progress({"quarter": quarter, "fetch": "downloaded", "sha256": digest.hexdigest(), "bytes": size,
                      "elapsed_s": recorded[quarter]["elapsed_s"]})
        results.append(FsdsArchive(quarter, path, digest.hexdigest(), size, url, True))
    return results


# ---------------------------------------------------------------------------------------------
# 2. Loader (bounded DuckDB, panel subset)
# ---------------------------------------------------------------------------------------------

_SUBSET_SCHEMA = (
    """
    CREATE TABLE IF NOT EXISTS fsds_load_manifest (
        quarter VARCHAR PRIMARY KEY,
        zip_sha256 VARCHAR NOT NULL,
        panel_sha256 VARCHAR NOT NULL,
        sub_rows BIGINT NOT NULL,
        num_rows BIGINT NOT NULL,
        tag_rows BIGINT NOT NULL,
        num_txt_bytes BIGINT NOT NULL,
        num_columns VARCHAR NOT NULL,
        loaded_at TIMESTAMP NOT NULL
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS fsds_sub (
        quarter VARCHAR NOT NULL, adsh VARCHAR NOT NULL, cik VARCHAR NOT NULL, name VARCHAR,
        sic VARCHAR, countryba VARCHAR, form VARCHAR, period DATE, fy INTEGER, fp VARCHAR,
        filed DATE, accepted TIMESTAMP, prevrpt BOOLEAN, afs VARCHAR, fye VARCHAR, instance VARCHAR
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS fsds_num (
        quarter VARCHAR NOT NULL, adsh VARCHAR NOT NULL, tag VARCHAR NOT NULL, version VARCHAR,
        ddate DATE, qtrs INTEGER, uom VARCHAR, segments VARCHAR, coreg VARCHAR, value DOUBLE
    )
    """,
    """
    CREATE TABLE IF NOT EXISTS fsds_tag (
        quarter VARCHAR NOT NULL, tag VARCHAR NOT NULL, version VARCHAR, custom BOOLEAN,
        abstract BOOLEAN, datatype VARCHAR, iord VARCHAR, crdr VARCHAR, tlabel VARCHAR
    )
    """,
)


def open_fsds_subset(db_path: Path, *, read_only: bool = False) -> duckdb.DuckDBPyConnection:
    """Open the standalone FSDS subset DB with light-work caps."""

    db_path = Path(db_path)
    if db_path.resolve() == governed_warehouse_path():
        raise ValueError("the FSDS subset DB must not be the governed warehouse")
    if not read_only:
        db_path.parent.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect(str(db_path), read_only=read_only)
    con.execute("SET TimeZone='UTC'")
    con.execute(f"SET memory_limit='{LIGHT_MEMORY_LIMIT}'")
    con.execute(f"SET threads={LIGHT_THREADS}")
    con.execute("SET preserve_insertion_order=false")
    temp_dir = (db_path.resolve().parent / f".{db_path.name}.tmp").as_posix().replace("'", "''")
    con.execute(f"SET temp_directory='{temp_dir}'")
    if not read_only:
        for statement in _SUBSET_SCHEMA:
            con.execute(statement)
    return con


_LOADER_VERSION = "p12_fsds_loader_v2"  # v2: RFC-4180 quoting


def _panel_sha256(ciks: Iterable[str]) -> str:
    """Reload key: the panel plus the loader version (a parser change forces a reload)."""

    payload = "|".join([_LOADER_VERSION, *sorted(ciks)])
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _tsv_source(path: Path) -> tuple[str, list[str]]:
    with path.open("r", encoding="utf-8", errors="replace") as handle:
        header = handle.readline().rstrip("\r\n").split("\t")
    columns = ", ".join(f"'{name}': 'VARCHAR'" for name in header)
    literal = path.resolve().as_posix().replace("'", "''")
    # FSDS fields are tab-delimited and RFC-4180 quoted when they contain a tab (seen in num.txt
    # ``segments`` of 2025q3: 48 of 3.72M rows); strict parsing, every column read as VARCHAR.
    source = (
        f"read_csv('{literal}', delim='\t', header=true, quote='\"', escape='\"', auto_detect=false, "
        f"columns={{{columns}}}, ignore_errors=false)"
    )
    return source, header


def _col(header: Sequence[str], name: str, expr: str | None = None) -> str:
    if name in header:
        return expr or name
    return "NULL"


def _extract_member(archive: zipfile.ZipFile, member: str, target: Path) -> Path:
    with archive.open(member) as source, target.open("wb") as sink:
        shutil.copyfileobj(source, sink, _COPY_CHUNK)
    return target


def load_fsds_subset(
    archives: Sequence[FsdsArchive],
    subset_db: Path,
    *,
    ciks: Iterable[str],
    work_dir: Path | None = None,
) -> list[dict[str, Any]]:
    """Stream each zip's sub/num/tag into the subset DB, keeping only the panel issuers.

    Each member is copied out of the zip in 1 MiB chunks into a P12 temp directory (deleted after
    the quarter) and read by DuckDB under a 384MB/2-thread cap with the panel filter pushed into
    the scan, so no file is ever materialized in pandas. Reloading a quarter with the same zip
    hash and panel is a no-op.
    """

    panel = sorted({c for c in (_normalize_cik(v) for v in ciks) if c})
    if not panel:
        raise ValueError("load_fsds_subset needs at least one panel CIK")
    panel_sha = _panel_sha256(panel)
    subset_db = Path(subset_db)
    work_root = Path(work_dir or subset_db.parent)
    con = open_fsds_subset(subset_db)
    stats: list[dict[str, Any]] = []
    try:
        con.execute("CREATE OR REPLACE TEMP TABLE _p12_panel(cik VARCHAR)")
        con.executemany("INSERT INTO _p12_panel VALUES (?)", [[c] for c in panel])
        for archive in archives:
            quarter = archive.quarter
            existing = con.execute(
                "SELECT zip_sha256, panel_sha256 FROM fsds_load_manifest WHERE quarter = ?", [quarter]
            ).fetchone()
            if existing is not None and tuple(existing) == (archive.sha256, panel_sha):
                row = con.execute(
                    "SELECT sub_rows, num_rows, tag_rows FROM fsds_load_manifest WHERE quarter = ?", [quarter]
                ).fetchone()
                stats.append({"quarter": quarter, "reused": True, "sub_rows": row[0], "num_rows": row[1], "tag_rows": row[2]})
                continue
            extract_dir = work_root / f"P12-extract-{quarter}-{uuid.uuid4().hex[:8]}"
            extract_dir.mkdir(parents=True, exist_ok=False)
            try:
                with zipfile.ZipFile(archive.path) as zipped:
                    sub_path = _extract_member(zipped, "sub.txt", extract_dir / "sub.txt")
                    num_path = _extract_member(zipped, "num.txt", extract_dir / "num.txt")
                    tag_path = _extract_member(zipped, "tag.txt", extract_dir / "tag.txt")
                stats.append(_load_quarter(con, quarter, archive.sha256, panel_sha, sub_path, num_path, tag_path))
            finally:
                shutil.rmtree(extract_dir, ignore_errors=True)
    finally:
        con.close()
    return stats


def _load_quarter(
    con: duckdb.DuckDBPyConnection,
    quarter: str,
    zip_sha256: str,
    panel_sha: str,
    sub_path: Path,
    num_path: Path,
    tag_path: Path,
) -> dict[str, Any]:
    sub_src, sub_h = _tsv_source(sub_path)
    num_src, num_h = _tsv_source(num_path)
    tag_src, tag_h = _tsv_source(tag_path)
    for required, header, member in (
        ({"adsh", "cik", "form", "period", "filed"}, sub_h, "sub.txt"),
        ({"adsh", "tag", "version", "ddate", "qtrs", "uom", "value"}, num_h, "num.txt"),
        ({"tag", "version", "custom"}, tag_h, "tag.txt"),
    ):
        missing = required - set(header)
        if missing:
            raise ValueError(f"{quarter} {member} lacks columns {sorted(missing)}; header={header}")

    def nullif(name: str, header: Sequence[str]) -> str:
        return _col(header, name, f"nullif(trim({name}), '')")

    con.execute("BEGIN TRANSACTION")
    try:
        for table in ("fsds_sub", "fsds_num", "fsds_tag", "fsds_load_manifest"):
            con.execute(f"DELETE FROM {table} WHERE quarter = ?", [quarter])
        con.execute(
            f"""
            INSERT INTO fsds_sub
            SELECT
                ? AS quarter, adsh, lpad(CAST(TRY_CAST(cik AS BIGINT) AS VARCHAR), 10, '0') AS cik,
                {_col(sub_h, 'name')}, {_col(sub_h, 'sic')}, {_col(sub_h, 'countryba')}, form,
                CAST(try_strptime(period, '%Y%m%d') AS DATE),
                {_col(sub_h, 'fy', 'TRY_CAST(fy AS INTEGER)')}, {nullif('fp', sub_h)},
                CAST(try_strptime(filed, '%Y%m%d') AS DATE),
                {_col(sub_h, 'accepted', 'TRY_CAST(accepted AS TIMESTAMP)')},
                {_col(sub_h, 'prevrpt', "prevrpt = '1'")}, {_col(sub_h, 'afs')}, {_col(sub_h, 'fye')},
                {_col(sub_h, 'instance')}
            FROM {sub_src}
            WHERE lpad(CAST(TRY_CAST(cik AS BIGINT) AS VARCHAR), 10, '0') IN (SELECT cik FROM _p12_panel)
            """,
            [quarter],
        )
        con.execute(
            f"""
            INSERT INTO fsds_num
            SELECT
                ? AS quarter, adsh, tag, version,
                CAST(try_strptime(ddate, '%Y%m%d') AS DATE), TRY_CAST(qtrs AS INTEGER), uom,
                {nullif('segments', num_h)}, {nullif('coreg', num_h)}, TRY_CAST(value AS DOUBLE)
            FROM {num_src}
            WHERE adsh IN (SELECT adsh FROM fsds_sub WHERE quarter = ?)
            """,
            [quarter, quarter],
        )
        con.execute(
            f"""
            INSERT INTO fsds_tag
            SELECT
                ? AS quarter, tag, version, custom = '1', {_col(tag_h, 'abstract', "abstract = '1'")},
                {_col(tag_h, 'datatype')}, {_col(tag_h, 'iord')}, {_col(tag_h, 'crdr')}, {_col(tag_h, 'tlabel')}
            FROM {tag_src} AS src
            WHERE EXISTS (
                SELECT 1 FROM fsds_num k WHERE k.quarter = ? AND k.tag = src.tag AND k.version = src.version
            )
            """,
            [quarter, quarter],
        )
        counts = con.execute(
            """
            SELECT
                (SELECT count(*) FROM fsds_sub WHERE quarter = ?),
                (SELECT count(*) FROM fsds_num WHERE quarter = ?),
                (SELECT count(*) FROM fsds_tag WHERE quarter = ?)
            """,
            [quarter, quarter, quarter],
        ).fetchone()
        con.execute(
            "INSERT INTO fsds_load_manifest VALUES (?, ?, ?, ?, ?, ?, ?, ?, now())",
            [quarter, zip_sha256, panel_sha, counts[0], counts[1], counts[2], num_path.stat().st_size, ",".join(num_h)],
        )
        con.execute("COMMIT")
    except BaseException:
        con.execute("ROLLBACK")
        raise
    return {
        "quarter": quarter,
        "reused": False,
        "sub_rows": int(counts[0]),
        "num_rows": int(counts[1]),
        "tag_rows": int(counts[2]),
        "num_txt_bytes": int(num_path.stat().st_size),
        "num_columns": num_h,
    }


# ---------------------------------------------------------------------------------------------
# 3. Canonical mapping (tag, ddate, qtrs, uom) -> (item_id, basis, period)
# ---------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class CanonicalAlias:
    tag: str
    priority: int
    valid_from: dt.date | None
    valid_to: dt.date | None
    # statement_map.value_multiplier for (concept, item): the warehouse stores raw x multiplier
    # (e.g. PaymentsToAcquirePropertyPlantAndEquipment -1.0, capex as a signed outflow).
    value_multiplier: float = 1.0


@dataclass(frozen=True)
class CanonicalRule:
    item_id: int
    canonical_code: str
    basis: str
    unit_kind: str
    aliases: tuple[CanonicalAlias, ...]
    inputs: tuple[int, ...]
    combination_rule: str
    missing_policy: str
    sign_multiplier: float
    absolute_value: bool
    scale_multiplier: float
    valid_from: dt.date | None = None
    valid_to: dt.date | None = None
    # Per input: "item" (raw statement item) or "output" (that item's own rule output).
    input_kinds: tuple[str, ...] = ()


def canonical_rules(
    items: Sequence[CoreItem] = CORE_ITEMS,
    *,
    rules: Sequence[StandardizationRule] | None = None,
    item_seed: Sequence[FundamentalItemSeedRow] | None = None,
    statement_rows: Sequence[FundamentalStatementMapRow] | None = None,
) -> dict[tuple[int, str], CanonicalRule]:
    """Resolve the warehouse standardization rules (and their raw component items) per basis."""

    rules = tuple(rules if rules is not None else default_standardization_rules())
    seed = tuple(item_seed if item_seed is not None else read_fundamental_item_seed())
    statement = tuple(statement_rows if statement_rows is not None else read_statement_map_seed())
    validity: dict[tuple[int, str], tuple[dt.date | None, dt.date | None]] = {}
    unit_types: dict[int, str | None] = {}
    for row in seed:
        unit_types.setdefault(row.item_id, row.unit_type)
        if row.alias_scheme == "us-gaap" and row.alias_code:
            validity[(row.item_id, row.alias_code)] = (_as_date(row.valid_from), _as_date(row.valid_to))
    multipliers: dict[tuple[int, str], float] = {}
    for srow in sorted(statement, key=lambda r: r.industry_template != "ALL"):  # ALL template wins
        if srow.taxonomy == "us-gaap" and srow.is_active and srow.item_id is not None:
            multipliers.setdefault((int(srow.item_id), srow.concept), float(srow.value_multiplier))
    active = {(rule.item_id, rule.basis): rule for rule in rules if rule.is_active}

    def build(item_id: int, basis: str, unit_kind: str) -> CanonicalRule:
        rule = active.get((item_id, basis))
        if rule is None:
            raise ValueError(f"no active standardization rule for item {item_id} basis {basis}")
        aliases = tuple(
            CanonicalAlias(
                alias.alias_code,
                int(alias.priority),
                *validity.get((item_id, alias.alias_code), (None, None)),
                value_multiplier=multipliers.get((item_id, alias.alias_code), 1.0),
            )
            for alias in rule.source_aliases
            if alias.alias_scheme == "us-gaap"
        )
        return CanonicalRule(
            item_id=item_id,
            canonical_code=rule.canonical_code,
            basis=basis,
            unit_kind=unit_kind,
            aliases=aliases,
            inputs=tuple(rule.source_item_ids) if rule.combination_rule in _COMPOSITION_RULES else (),
            combination_rule=rule.combination_rule,
            missing_policy=rule.missing_policy,
            sign_multiplier=-1.0 if rule.sign_rule == "invert" else 1.0,
            absolute_value=rule.sign_rule == "absolute",
            scale_multiplier={"identity": 1.0, "thousands": 1_000.0, "millions": 1_000_000.0}[rule.scale_rule],
            valid_from=rule.valid_from,
            valid_to=rule.valid_to,
            input_kinds=rule_input_kinds(rule) if rule.combination_rule in _COMPOSITION_RULES else (),
        )

    out: dict[tuple[int, str], CanonicalRule] = {}

    def add(item_id: int, basis: str, unit_kind: str) -> CanonicalRule:
        if (item_id, basis) in out:
            return out[(item_id, basis)]
        rule = build(item_id, basis, unit_kind)
        out[(item_id, basis)] = rule
        for input_id in rule.inputs:  # raw inputs need the item's aliases; output inputs its rule
            kind = "per_share" if unit_types.get(input_id) == "per_share" else "monetary"
            add(input_id, basis, kind)
        return rule

    for item in items:
        for basis in _BASES_BY_KIND[item.period_kind]:
            rule = add(item.item_id, basis, item.unit_kind)
            if rule.canonical_code != item.canonical_code:
                raise ValueError(f"{item.name}: rule code {rule.canonical_code} != {item.canonical_code}")
    return out


def _alias_table(rules_map: Mapping[tuple[int, str], CanonicalRule]) -> pd.DataFrame:
    rows = [
        {
            "item_id": rule.item_id,
            "basis": rule.basis,
            "tag": alias.tag,
            "priority": alias.priority,
            "alias_valid_from": alias.valid_from or rule.valid_from,
            "alias_valid_to": alias.valid_to or rule.valid_to,
            "expected_uom": uom,
            "value_multiplier": alias.value_multiplier,
        }
        for rule in rules_map.values()
        for alias in rule.aliases
        for uom in _FSDS_UOMS_BY_UNIT_KIND[rule.unit_kind]
    ]
    return pd.DataFrame(
        rows,
        columns=["item_id", "basis", "tag", "priority", "alias_valid_from", "alias_valid_to", "expected_uom", "value_multiplier"],
    )


FACT_COLUMNS = [
    "quarter", "adsh", "cik", "form", "period", "fy", "fp", "filed", "accepted",
    "tag", "version", "ddate", "qtrs", "uom", "value",
]
# ``fy``/``fp`` describe the filing's own reporting period (``period``), not comparatives.
_FILING_COLUMNS = ["quarter", "adsh", "cik", "form", "period", "fy", "fp", "filed", "accepted"]
CANONICAL_COLUMNS = [
    *_FILING_COLUMNS,
    "item_id",
    "canonical_code",
    "benchmark_item",
    "basis",
    "unit_kind",
    "ddate",
    "value",
    "derivation",
    "source_tags_json",
]


def fsds_alias_facts(
    con: duckdb.DuckDBPyConnection,
    rules_map: Mapping[tuple[int, str], CanonicalRule],
) -> pd.DataFrame:
    """Non-dimensional, non-coreg us-gaap facts whose tag is an alias of a needed rule."""

    tags = sorted({alias.tag for rule in rules_map.values() for alias in rule.aliases})
    return con.execute(
        """
        SELECT n.quarter, n.adsh, s.cik, s.form, s.period, s.fy, s.fp, s.filed, s.accepted,
               n.tag, n.version, n.ddate, n.qtrs, n.uom, n.value
        FROM fsds_num n
        JOIN fsds_sub s ON s.quarter = n.quarter AND s.adsh = n.adsh
        WHERE list_contains(CAST(? AS VARCHAR[]), n.tag)
          AND n.version LIKE 'us-gaap/%'
          AND n.segments IS NULL
          AND n.coreg IS NULL
          AND n.qtrs IN (0, 1, 4)
          AND n.value IS NOT NULL
          AND n.ddate IS NOT NULL
        """,
        [tags],
    ).df()


def _apply_rule_value(values: pd.Series, rule: CanonicalRule) -> pd.Series:
    out = values.abs() if rule.absolute_value else values * rule.sign_multiplier
    return out * rule.scale_multiplier


def canonical_fsds_facts(
    facts: pd.DataFrame,
    rules_map: Mapping[tuple[int, str], CanonicalRule] | None = None,
    items: Sequence[CoreItem] = CORE_ITEMS,
) -> pd.DataFrame:
    """Map FSDS facts to canonical (item, basis, ddate) values, one row per filing.

    ``facts`` must already exclude dimensional/co-registrant rows (see :func:`fsds_alias_facts`).
    ``qtrs`` 4 -> annual, 1 -> quarterly, 0 -> instant; the uom must be the item's FSDS unit (USD;
    FSDS also labels per-share facts USD); alias validity windows are checked against ``ddate``.
    """

    rules_map = rules_map if rules_map is not None else canonical_rules(items)
    if facts is None or facts.empty:
        return pd.DataFrame(columns=CANONICAL_COLUMNS)
    frame = facts.copy()
    frame["ddate"] = pd.to_datetime(frame["ddate"]).dt.date
    frame["basis"] = frame["qtrs"].astype("Int64").map(_BASIS_BY_QTRS)
    frame = frame[frame["basis"].notna()]
    candidates = frame.merge(_alias_table(rules_map), on=["tag", "basis"], how="inner")
    if candidates.empty:
        return pd.DataFrame(columns=CANONICAL_COLUMNS)
    in_window = [
        (valid_from is None or pd.isna(valid_from) or valid_from <= ddate)
        and (valid_to is None or pd.isna(valid_to) or ddate < valid_to)
        for valid_from, valid_to, ddate in zip(
            candidates["alias_valid_from"], candidates["alias_valid_to"], candidates["ddate"], strict=True
        )
    ]
    valid = (candidates["uom"] == candidates["expected_uom"]) & pd.Series(in_window, index=candidates.index, dtype=bool)
    raw = (
        candidates[valid]
        .sort_values(["adsh", "item_id", "basis", "ddate", "priority", "tag"], kind="mergesort")
        .drop_duplicates(subset=["adsh", "item_id", "basis", "ddate"], keep="first")
        .copy()
    )
    # Statement points carry raw x statement_map.value_multiplier; compositions read those.
    raw["value"] = raw["value"].astype(float) * raw["value_multiplier"].astype(float)
    filings = frame[_FILING_COLUMNS].drop_duplicates(subset=["adsh"])
    name_by_id = {item.item_id: item.name for item in items}
    outputs: dict[tuple[int, str], pd.DataFrame] = {}
    filing_clock = {
        adsh: (cik, accepted)
        for adsh, cik, accepted in zip(filings["adsh"], filings["cik"], filings["accepted"], strict=True)
    }

    def without_covered(derived: pd.DataFrame, direct: pd.DataFrame) -> pd.DataFrame:
        """Drop compositions the rule's own direct value already covers (engine semantics).

        The engine suppresses a coalesce_or_* composition when a direct value for the same
        issuer and period is visible at or before the composition's clock, from any filing: a
        later 10-K whose equity statement carries only the NCI-inclusive total for an older
        balance date does not re-derive stockholders' equity the earlier 10-K reported.
        """

        if derived.empty or direct.empty:
            return derived
        first_direct: dict[tuple[str, dt.date], pd.Timestamp] = {}
        for adsh, ddate in zip(direct["adsh"], direct["ddate"], strict=True):
            cik, accepted = filing_clock[adsh]
            seen = first_direct.get((cik, ddate))
            if seen is None or accepted < seen:
                first_direct[(cik, ddate)] = accepted
        keep = []
        for adsh, ddate in zip(derived["adsh"], derived["ddate"], strict=True):
            cik, accepted = filing_clock[adsh]
            seen = first_direct.get((cik, ddate))
            keep.append(seen is None or seen > accepted)
        return derived[keep]

    def raw_rows(item_id: int, basis: str) -> pd.DataFrame:
        part = raw[(raw["item_id"] == item_id) & (raw["basis"] == basis)]
        return pd.DataFrame(
            {
                "adsh": part["adsh"].to_numpy(),
                "ddate": part["ddate"].to_numpy(),
                "value": part["value"].astype(float).to_numpy(),
                "tags": [[f"us-gaap:{tag}"] for tag in part["tag"]],
            }
        )

    def output_rows(item_id: int, basis: str) -> pd.DataFrame:
        """The rule's own output: direct value, else its composition (engine semantics)."""

        key = (item_id, basis)
        if key in outputs:
            return outputs[key]
        rule = rules_map[key]
        parts: list[pd.DataFrame] = []
        if rule.combination_rule in _DIRECT_FIRST_RULES:
            direct = raw_rows(item_id, basis)
            direct["value"] = _apply_rule_value(direct["value"], rule).to_numpy()
            direct["derivation"] = "direct"
            parts.append(direct)
        if rule.inputs:
            sources = [
                output_rows(input_id, basis) if kind == "output" else raw_rows(input_id, basis)
                for input_id, kind in zip(rule.inputs, rule.input_kinds or ("item",) * len(rule.inputs), strict=True)
            ]
            labels = [
                f"atx-rule-output:{rules_map[(input_id, basis)].canonical_code}" if kind == "output" else None
                for input_id, kind in zip(rule.inputs, rule.input_kinds or ("item",) * len(rule.inputs), strict=True)
            ]
            derived = _derive(sources, labels, rule)
            if parts and rule.combination_rule.startswith("coalesce_or_"):
                derived = without_covered(derived, parts[0])
            parts.append(derived)
        non_empty = [p for p in parts if not p.empty]
        result = (
            pd.concat(non_empty, ignore_index=True)
            if non_empty
            else pd.DataFrame(columns=["adsh", "ddate", "value", "tags", "derivation"])
        )
        outputs[key] = result
        return result

    out_frames: list[pd.DataFrame] = []
    for item in items:
        for basis in _BASES_BY_KIND[item.period_kind]:
            rule = rules_map[(item.item_id, basis)]
            combined = output_rows(item.item_id, basis)
            if combined.empty:
                continue
            combined = combined.merge(filings, on="adsh", how="left")
            combined["source_tags_json"] = [json.dumps(tags) for tags in combined["tags"]]
            combined["item_id"] = item.item_id
            combined["canonical_code"] = rule.canonical_code
            combined["benchmark_item"] = name_by_id[item.item_id]
            combined["basis"] = basis
            combined["unit_kind"] = rule.unit_kind
            out_frames.append(combined[CANONICAL_COLUMNS])
    if not out_frames:
        return pd.DataFrame(columns=CANONICAL_COLUMNS)
    return pd.concat(out_frames, ignore_index=True).sort_values(
        ["cik", "benchmark_item", "basis", "ddate", "accepted", "adsh"], kind="mergesort"
    ).reset_index(drop=True)


def _derive(sources: Sequence[pd.DataFrame], labels: Sequence[str | None], rule: CanonicalRule) -> pd.DataFrame:
    """Composition over inputs reported in the same filing and period (engine semantics).

    Differences are n-ary (input 1 minus every later input). ``skip`` needs every input,
    ``zero_fill`` at least one (absent inputs count as 0), ``zero_fill_subtrahends`` needs
    input 1 and counts absent later inputs as 0. An ``output`` input is labeled
    ``atx-rule-output:<canonical_code>`` as the engine labels it in ``input_codes_json``.
    """

    keyed: pd.DataFrame | None = None
    for position, source in enumerate(sources):
        part = source[["adsh", "ddate", "value", "tags"]].rename(columns={"value": f"v{position}", "tags": f"t{position}"})
        if labels[position] is not None:
            part[f"t{position}"] = pd.Series([[labels[position]] for _ in range(len(part))], index=part.index, dtype=object)
        keyed = part if keyed is None else keyed.merge(part, on=["adsh", "ddate"], how="outer")
    if keyed is None or keyed.empty:
        return pd.DataFrame()
    value_cols = [f"v{i}" for i in range(len(sources))]
    present = keyed[value_cols].notna()
    if rule.missing_policy == "zero_fill":
        keep = present.any(axis=1)
    elif rule.missing_policy == "zero_fill_subtrahends":
        keep = present["v0"]
    else:
        keep = present.all(axis=1)
    keyed = keyed[keep].copy()
    if keyed.empty:
        return pd.DataFrame()
    filled = keyed[value_cols].fillna(0.0).astype(float)
    if rule.combination_rule in _DIFFERENCE_RULES:
        value = filled["v0"]
        for column in value_cols[1:]:
            value = value - filled[column]
    else:
        value = filled.sum(axis=1)
    tag_cols = [f"t{i}" for i in range(len(sources))]
    return pd.DataFrame(
        {
            "adsh": keyed["adsh"].to_numpy(),
            "ddate": keyed["ddate"].to_numpy(),
            "value": _apply_rule_value(value, rule).to_numpy(),
            "tags": [
                [tag for tags in row if isinstance(tags, list) for tag in tags]
                for row in keyed[tag_cols].itertuples(index=False)
            ],
            "derivation": "derived",
        }
    )


# ---------------------------------------------------------------------------------------------
# 4. Benchmark grid coverage (50 issuers x 5 FY x 10 items)
# ---------------------------------------------------------------------------------------------

CELL_COLUMNS = [
    "ticker", "cik", "segment", "fy_end", "benchmark_item", "canonical_code", "basis", "status", "reason",
    "n_filings", "n_distinct_values", "fsds_latest_value", "fsds_latest_adsh", "derivation", "detail",
]


@dataclass(frozen=True)
class FsdsCoverage:
    cells: pd.DataFrame
    grid_facts: pd.DataFrame
    summary: dict[str, Any]


def issuer_fiscal_year_ends(
    con: duckdb.DuckDBPyConnection, ciks: Iterable[str], years: int = BENCHMARK_YEARS
) -> dict[str, list[dt.date]]:
    """Latest annual-report period per issuer and the ``years - 1`` fiscal year ends before it."""

    forms = sorted(ANNUAL_FORMS)
    placeholders = ", ".join("?" for _ in forms)
    rows = con.execute(
        f"SELECT cik, max(period) FROM fsds_sub WHERE form IN ({placeholders}) GROUP BY cik", forms
    ).fetchall()
    latest = {str(cik): period for cik, period in rows if period is not None}
    out: dict[str, list[dt.date]] = {}
    for cik in ciks:
        period = latest.get(cik)
        if period is None:
            out[cik] = []
            continue
        out[cik] = [_month_end(period.year - k, period.month) for k in range(years)]
    return out


def _diagnostic_facts(con: duckdb.DuckDBPyConnection, rules_map: Mapping[tuple[int, str], CanonicalRule], items: Sequence[CoreItem]) -> pd.DataFrame:
    alias_tags = sorted({alias.tag for rule in rules_map.values() for alias in rule.aliases})
    pattern = "|".join(f"(?:{kw})" for item in items for kw in item.keywords)
    return con.execute(
        """
        SELECT s.cik, n.adsh, n.tag, n.version, n.ddate, n.qtrs, n.uom, n.value,
               n.segments IS NOT NULL OR n.coreg IS NOT NULL AS has_dims,
               coalesce(t.custom, n.version = n.adsh) AS custom
        FROM fsds_num n
        JOIN fsds_sub s ON s.quarter = n.quarter AND s.adsh = n.adsh
        LEFT JOIN fsds_tag t ON t.quarter = n.quarter AND t.tag = n.tag AND t.version = n.version
        WHERE n.qtrs IN (0, 4)
          AND n.value IS NOT NULL
          AND n.ddate IS NOT NULL
          AND (list_contains(CAST(? AS VARCHAR[]), n.tag) OR regexp_matches(n.tag, ?))
        """,
        [alias_tags, pattern],
    ).df()


def _near(dates: pd.Series, target: dt.date, days: int) -> pd.Series:
    return pd.Series([abs((d - target).days) <= days for d in dates], index=dates.index, dtype=bool)


def benchmark_coverage(
    con: duckdb.DuckDBPyConnection,
    panel: Sequence[BenchmarkIssuer] = BENCHMARK_PANEL,
    items: Sequence[CoreItem] = CORE_ITEMS,
    *,
    years: int = BENCHMARK_YEARS,
    rules_map: Mapping[tuple[int, str], CanonicalRule] | None = None,
) -> FsdsCoverage:
    """Classify every (issuer, fiscal-year end, core item) cell of the FSDS benchmark grid."""

    rules_map = rules_map if rules_map is not None else canonical_rules(items)
    canon = canonical_fsds_facts(fsds_alias_facts(con, rules_map), rules_map, items)
    grid_bases = {"annual", "instant"}
    canon_fy = canon[canon["basis"].isin(grid_bases)] if not canon.empty else canon
    diag = _diagnostic_facts(con, rules_map, items)
    if not diag.empty:
        diag["ddate"] = pd.to_datetime(diag["ddate"]).dt.date
    periods = con.execute(
        """
        SELECT DISTINCT s.cik, n.ddate, n.qtrs
        FROM fsds_num n JOIN fsds_sub s ON s.quarter = n.quarter AND s.adsh = n.adsh
        WHERE n.qtrs IN (0, 4) AND n.segments IS NULL AND n.coreg IS NULL AND n.ddate IS NOT NULL
        """
    ).df()
    if not periods.empty:
        periods["ddate"] = pd.to_datetime(periods["ddate"]).dt.date
    taxonomy = {
        str(cik): (bool(ifrs), bool(gaap))
        for cik, ifrs, gaap in con.execute(
            """
            SELECT s.cik, bool_or(n.version LIKE 'ifrs%'), bool_or(n.version LIKE 'us-gaap%')
            FROM fsds_num n JOIN fsds_sub s ON s.quarter = n.quarter AND s.adsh = n.adsh
            GROUP BY s.cik
            """
        ).fetchall()
    }
    fy_ends = issuer_fiscal_year_ends(con, [issuer.cik for issuer in panel], years)

    cells: list[dict[str, Any]] = []
    grid_parts: list[pd.DataFrame] = []
    for issuer in panel:
        issuer_canon = canon_fy[canon_fy["cik"] == issuer.cik] if not canon_fy.empty else canon_fy
        issuer_diag = diag[diag["cik"] == issuer.cik] if not diag.empty else diag
        issuer_periods = periods[periods["cik"] == issuer.cik] if not periods.empty else periods
        ends = fy_ends.get(issuer.cik) or []
        for item in items:
            basis = _GRID_BASIS_BY_KIND[item.period_kind]
            qtrs = _GRID_QTRS_BY_KIND[item.period_kind]
            rule = rules_map[(item.item_id, basis)]
            if not ends:
                for _ in range(years):
                    cells.append(_cell(issuer, None, item, basis, "unmapped", "issuer_not_in_window",
                                       "no annual-report submission for this CIK in the loaded quarters"))
                continue
            for fy_end in ends:
                hit = issuer_canon[
                    (issuer_canon["item_id"] == item.item_id)
                    & (issuer_canon["basis"] == basis)
                    & _near(issuer_canon["ddate"], fy_end, GRID_MATCH_DAYS)
                ] if not issuer_canon.empty else issuer_canon
                if not hit.empty:
                    latest = hit.sort_values(["accepted", "adsh"], kind="mergesort").iloc[-1]
                    derivation = "direct" if (hit["derivation"] == "direct").any() else "derived"
                    cell = _cell(issuer, fy_end, item, basis, "mapped", f"mapped_{derivation}",
                                 latest["source_tags_json"])
                    cell.update(
                        n_filings=int(hit["adsh"].nunique()),
                        n_distinct_values=int(hit["value"].round(6).nunique()),
                        fsds_latest_value=float(latest["value"]),
                        fsds_latest_adsh=str(latest["adsh"]),
                        derivation=derivation,
                    )
                    cells.append(cell)
                    grid_parts.append(hit.assign(ticker=issuer.ticker, fy_end=fy_end))
                    continue
                reason, detail = _classify_unmapped(
                    fy_end, item, qtrs, rule, rules_map, issuer_diag, issuer_periods, taxonomy.get(issuer.cik)
                )
                cells.append(_cell(issuer, fy_end, item, basis, "unmapped", reason, detail))
    cells_df = pd.DataFrame(cells, columns=CELL_COLUMNS)
    grid_facts = pd.concat(grid_parts, ignore_index=True) if grid_parts else pd.DataFrame(columns=[*CANONICAL_COLUMNS, "ticker", "fy_end"])
    return FsdsCoverage(cells_df, grid_facts, _coverage_summary(cells_df, panel, items, years))


def _cell(issuer: BenchmarkIssuer, fy_end: dt.date | None, item: CoreItem, basis: str, status: str, reason: str, detail: str) -> dict[str, Any]:
    return {
        "ticker": issuer.ticker,
        "cik": issuer.cik,
        "segment": issuer.segment,
        "fy_end": fy_end,
        "benchmark_item": item.name,
        "canonical_code": item.canonical_code,
        "basis": basis,
        "status": status,
        "reason": reason,
        "n_filings": 0,
        "n_distinct_values": 0,
        "fsds_latest_value": None,
        "fsds_latest_adsh": None,
        "derivation": None,
        "detail": detail,
    }


def _classify_unmapped(
    fy_end: dt.date,
    item: CoreItem,
    qtrs: int,
    rule: CanonicalRule,
    rules_map: Mapping[tuple[int, str], CanonicalRule],
    diag: pd.DataFrame,
    periods: pd.DataFrame,
    taxonomy: tuple[bool, bool] | None,
) -> tuple[str, str]:
    if taxonomy is not None and taxonomy[0] and not taxonomy[1]:
        return "ifrs_filer", "filings use the ifrs-full taxonomy; no us-gaap facts"
    in_window = (
        not periods.empty
        and bool(((periods["qtrs"] == qtrs) & _near(periods["ddate"], fy_end, GRID_MATCH_DAYS)).any())
    )
    if not in_window:
        return "period_not_in_window", f"no loaded filing reports qtrs={qtrs} facts for this fiscal year end"
    window = diag[(diag["qtrs"] == qtrs) & _near(diag["ddate"], fy_end, GRID_MATCH_DAYS)] if not diag.empty else diag
    own_tags = {alias.tag for alias in rule.aliases}
    component_tags: dict[int, set[str]] = {
        input_id: {alias.tag for alias in rules_map[(input_id, rule.basis)].aliases} for input_id in rule.inputs
    }
    all_rule_tags = own_tags | set().union(*component_tags.values()) if component_tags else set(own_tags)
    gaap = window[window["version"].astype(str).str.startswith("us-gaap/")] if not window.empty else window
    rule_hits = gaap[gaap["tag"].isin(all_rule_tags)] if not gaap.empty else gaap
    if not rule_hits.empty:
        plain = rule_hits[~rule_hits["has_dims"]]
        if plain.empty:
            return "dimensional_only", f"rule tags only with segments/coreg: {sorted(set(rule_hits['tag']))[:3]}"
        expected_uoms = _FSDS_UOMS_BY_UNIT_KIND[rule.unit_kind]
        if not plain["uom"].isin(expected_uoms).any():
            return "unit_mismatch", f"rule tags in uom {sorted(set(plain['uom']))[:3]} (expected {list(expected_uoms)})"
        if component_tags and not (plain["tag"].isin(own_tags)).any():
            present = sorted(i for i, tags in component_tags.items() if plain["tag"].isin(tags).any())
            missing = sorted(i for i in component_tags if i not in present)
            return "derivation_incomplete", f"components present item_ids={present} missing item_ids={missing}"
        return "alias_outside_validity", f"rule tags present but outside alias validity: {sorted(set(plain['tag']))[:3]}"
    pattern = re.compile("|".join(f"(?:{kw})" for kw in item.keywords))
    if window.empty:
        return "not_reported", "no rule or keyword tag reported for this period"
    matches = pd.Series([bool(pattern.search(tag)) for tag in window["tag"]], index=window.index, dtype=bool)
    keyword = window[matches & ~window["has_dims"].astype(bool) & ~window["tag"].isin(all_rule_tags)]
    if keyword.empty:
        return "not_reported", "no rule or keyword tag reported for this period"
    ranked = keyword.assign(absval=keyword["value"].abs()).sort_values("absval", ascending=False)
    standard = ranked[~ranked["custom"].astype(bool) & ranked["version"].astype(str).str.startswith("us-gaap/")]
    if not standard.empty:
        return "alias_gap_us_gaap", json.dumps(list(dict.fromkeys(standard["tag"]))[:3])
    return "custom_extension", json.dumps(list(dict.fromkeys(ranked["tag"]))[:3])


def _coverage_summary(cells: pd.DataFrame, panel: Sequence[BenchmarkIssuer], items: Sequence[CoreItem], years: int) -> dict[str, Any]:
    total = len(cells)
    mapped = cells["status"] == "mapped"
    by_item = {}
    for item in items:
        sub = cells[cells["benchmark_item"] == item.name]
        by_item[item.name] = {
            "cells": len(sub),
            "mapped": int((sub["status"] == "mapped").sum()),
            "mapped_direct": int((sub["reason"] == "mapped_direct").sum()),
            "mapped_derived": int((sub["reason"] == "mapped_derived").sum()),
            "unmapped_reasons": {k: int(v) for k, v in sub.loc[sub["status"] != "mapped", "reason"].value_counts().items()},
        }
    gap_tags: dict[str, dict[str, int]] = {}
    for item in items:
        sub = cells[(cells["benchmark_item"] == item.name) & (cells["reason"].isin(["alias_gap_us_gaap", "custom_extension"]))]
        counter: dict[str, int] = {}
        for detail in sub["detail"]:
            for tag in json.loads(detail):  # up to three keyword-ranked candidates per cell
                counter[tag] = counter.get(tag, 0) + 1
        if counter:
            gap_tags[item.name] = dict(sorted(counter.items(), key=lambda kv: (-kv[1], kv[0])))
    in_window = cells["reason"] != "period_not_in_window"
    return {
        "issuers": len(panel),
        "years": years,
        "items": len(items),
        "cells": total,
        "mapped": int(mapped.sum()),
        "mapped_ratio": (float(mapped.sum()) / total) if total else None,
        "cells_in_window": int(in_window.sum()),
        "mapped_ratio_in_window": (float(mapped.sum()) / float(in_window.sum())) if int(in_window.sum()) else None,
        "mapped_direct": int((cells["reason"] == "mapped_direct").sum()),
        "mapped_derived": int((cells["reason"] == "mapped_derived").sum()),
        "restated_within_window_cells": int((cells["n_distinct_values"] > 1).sum()),
        "unmapped_reasons": {k: int(v) for k, v in cells.loc[~mapped, "reason"].value_counts().items()},
        "by_item": by_item,
        "top_gap_tags": gap_tags,
    }


# ---------------------------------------------------------------------------------------------
# 5. Comparison harness (fact_disagreement) -- needs a DB holding fundamental_standardized
# ---------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class FsdsComparisonOptions:
    run_id: str | None = None
    tolerance_rel: float = TOLERANCE_REL
    tolerance_abs_monetary: float = TOLERANCE_ABS_MONETARY
    tolerance_abs_per_share: float = TOLERANCE_ABS_PER_SHARE
    period_align_days: int = PERIOD_ALIGN_DAYS
    failing_sample_limit: int = 25


@dataclass(frozen=True)
class FsdsComparisonResult:
    rows: pd.DataFrame
    unresolved_issuers: list[str]
    summary: dict[str, Any] = field(default_factory=dict)


def select_latest_vintage(facts: pd.DataFrame) -> pd.DataFrame:
    """Latest FSDS filing per (cik, item, basis, ddate): the vintage visible at the window end."""

    if facts.empty:
        return facts
    ordered = facts.sort_values(["cik", "item_id", "basis", "ddate", "accepted", "filed", "adsh"], kind="mergesort")
    return ordered.drop_duplicates(subset=["cik", "item_id", "basis", "ddate"], keep="last").reset_index(drop=True)


def _panel_symbols(panel: Sequence[BenchmarkIssuer]) -> dict[str, str]:
    return {issuer.cik: issuer.ticker for issuer in panel}


def resolve_security_ids(store: DuckDBStore, ciks: Sequence[str], panel: Sequence[BenchmarkIssuer]) -> dict[str, str]:
    """CIK -> warehouse security_id from ``fundamental_standardized`` itself.

    Prefers the security whose symbol is the panel ticker, else the one carrying the most rows.
    """

    if not ciks:
        return {}
    con = store.con
    con.execute("CREATE OR REPLACE TEMP TABLE _p12_ciks(cik BIGINT)")
    con.executemany("INSERT INTO _p12_ciks VALUES (?)", [[int(c)] for c in ciks])
    rows = con.execute(
        """
        SELECT TRY_CAST(cik AS BIGINT) AS cik_num, security_id, any_value(symbol) AS symbol, count(*) AS n
        FROM fundamental_standardized
        WHERE TRY_CAST(cik AS BIGINT) IN (SELECT cik FROM _p12_ciks)
        GROUP BY ALL
        """
    ).fetchall()
    symbols = _panel_symbols(panel)
    best: dict[str, tuple[int, int, str]] = {}
    for cik_num, security_id, symbol, n in rows:
        cik = f"{int(cik_num):010d}"
        wanted = symbols.get(cik, "").replace(".", "-").upper()
        got = (symbol or "").replace(".", "-").upper()
        score = (1 if wanted and got == wanted else 0, int(n), str(security_id))
        if cik not in best or score[:2] > best[cik][:2] or (score[:2] == best[cik][:2] and score[2] < best[cik][2]):
            best[cik] = score
    return {cik: score[2] for cik, score in best.items()}


def _align_periods(store: DuckDBStore, keys: pd.DataFrame, days: int) -> pd.Series:
    """Nearest warehouse period_end (same security/item/basis) within ``days`` of FSDS ddate."""

    con = store.con
    con.execute(
        "CREATE OR REPLACE TEMP TABLE _p12_keys(key_id BIGINT, security_id VARCHAR, item_id INTEGER, basis VARCHAR, ddate DATE)"
    )
    con.executemany(
        "INSERT INTO _p12_keys VALUES (?, ?, ?, ?, ?)",
        [
            [int(i), r.security_id, int(r.item_id), r.basis, r.ddate]
            for i, r in zip(keys.index, keys.itertuples(index=False), strict=True)
        ],
    )
    aligned = con.execute(
        """
        WITH wh AS (
            SELECT DISTINCT security_id, item_id, lower(basis) AS basis, period_end
            FROM fundamental_standardized
            WHERE security_id IN (SELECT DISTINCT security_id FROM _p12_keys)
        )
        SELECT k.key_id, wh.period_end
        FROM _p12_keys k
        JOIN wh ON wh.security_id = k.security_id AND wh.item_id = k.item_id AND wh.basis = k.basis
               AND abs(date_diff('day', wh.period_end, k.ddate)) <= ?
        QUALIFY row_number() OVER (
            PARTITION BY k.key_id ORDER BY abs(date_diff('day', wh.period_end, k.ddate)), wh.period_end DESC
        ) = 1
        """,
        [int(days)],
    ).fetchall()
    mapping = {int(key_id): period_end for key_id, period_end in aligned}
    return pd.Series([mapping.get(int(i), d) for i, d in zip(keys.index, keys["ddate"], strict=True)], index=keys.index)


def _group_names(unit_kind: str) -> tuple[str, str]:
    return f"{FSDS_BASELINE_SOURCE}:{unit_kind}", f"{FSDS_PARITY_SOURCE}:{unit_kind}"


def run_fsds_comparison(
    store: DuckDBStore,
    facts: pd.DataFrame,
    *,
    panel: Sequence[BenchmarkIssuer] = BENCHMARK_PANEL,
    options: FsdsComparisonOptions | None = None,
) -> FsdsComparisonResult:
    """Score FSDS canonical facts against ``fundamental_standardized`` through ``fact_disagreement``.

    ``facts`` holds one row per filing (``canonical_fsds_facts`` output, e.g. the coverage
    ``grid_facts``). The latest vintage per key becomes the ``vendor_baseline_facts`` row; every
    scored row is then checked against the warehouse revision whose ``source_accession`` equals
    the FSDS ``adsh`` of that vintage.
    """

    options = options or FsdsComparisonOptions()
    if facts is None or facts.empty:
        return FsdsComparisonResult(pd.DataFrame(), [], {"rows": 0})
    base = select_latest_vintage(facts[CANONICAL_COLUMNS].copy())
    ciks = sorted(base["cik"].unique())
    security_ids = resolve_security_ids(store, ciks, panel)
    unresolved = sorted(c for c in ciks if c not in security_ids)
    base["security_id"] = base["cik"].map(security_ids)
    base = base[base["security_id"].notna()].reset_index(drop=True)
    if base.empty:
        return FsdsComparisonResult(pd.DataFrame(), unresolved, {"rows": 0, "unresolved_issuers": unresolved})
    base["fsds_ddate"] = base["ddate"]
    base["period_end"] = _align_periods(store, base[["security_id", "item_id", "basis", "ddate"]], options.period_align_days)
    # The filing's fy/fp label only its own period; a comparative (e.g. FY2021 inside the FY2023
    # 10-K) gets no fiscal label rather than the filing's.
    own_period = [
        _as_date(period) is not None and abs((_as_date(period) - ddate).days) <= GRID_MATCH_DAYS
        for period, ddate in zip(base["period"], base["ddate"], strict=True)
    ]
    base["fy"] = base["fy"].where(own_period)
    base["fp"] = base["fp"].where(own_period)
    symbols = _panel_symbols(panel)

    for unit_kind, abs_tol in (("monetary", options.tolerance_abs_monetary), ("per_share", options.tolerance_abs_per_share)):
        baseline_source, parity_source = _group_names(unit_kind)
        group = base[base["unit_kind"] == unit_kind]
        rows = [
            {
                "vendor": FSDS_VENDOR,
                "vendor_fact_id": f"{r.adsh}|{r.canonical_code}|{r.basis}|{r.fsds_ddate}",
                "security_id": r.security_id,
                "symbol": symbols.get(r.cik),
                "cik": r.cik,
                "item_id": int(r.item_id),
                "canonical_code": r.canonical_code,
                "basis": r.basis,
                "period_end": r.period_end,
                "fiscal_year": None if pd.isna(r.fy) else int(r.fy),
                "fiscal_period": None if pd.isna(r.fp) else str(r.fp),
                "value": float(r.value),
                "unit_type": _UOM_BY_UNIT_KIND[unit_kind],
                "source_accession": r.adsh,
                "as_of_date": r.filed,
                "available_at": r.accepted if not pd.isna(r.accepted) else pd.Timestamp(r.filed),
                "is_latest_revision": True,
            }
            for r in group.itertuples(index=False)
        ]
        refresh_fact_disagreement(
            store,
            FactDisagreementOptions(
                source=parity_source,
                baseline_source=baseline_source,
                vendor=FSDS_VENDOR,
                baseline_rows=rows,
                tolerance_abs=abs_tol,
                tolerance_rel=options.tolerance_rel,
                run_id=options.run_id,
            ),
        )
    classified = _classify_rows(store, options)
    return FsdsComparisonResult(classified, unresolved, _comparison_summary(classified, unresolved, options))


def _classify_rows(store: DuckDBStore, options: FsdsComparisonOptions) -> pd.DataFrame:
    sources = [_group_names(kind)[1] for kind in ("monetary", "per_share")]
    frame = store.con.execute(
        """
        WITH same AS (
            SELECT security_id, item_id, lower(basis) AS basis, period_end, source_accession,
                   arg_max(value, available_at) AS value
            FROM fundamental_standardized
            WHERE security_id IN (SELECT DISTINCT security_id FROM fact_disagreement WHERE source IN (?, ?))
            GROUP BY ALL
        ),
        item_any AS (
            SELECT security_id, item_id, lower(basis) AS basis, count(*) AS n
            FROM fundamental_standardized
            WHERE security_id IN (SELECT DISTINCT security_id FROM fact_disagreement WHERE source IN (?, ?))
            GROUP BY ALL
        )
        SELECT d.source, d.symbol, d.cik, d.security_id, d.item_id, d.canonical_code, d.basis, d.period_end,
               d.fiscal_year, d.fiscal_period, d.vendor_value, d.warehouse_value, d.absolute_difference,
               d.relative_difference, d.tolerance_abs, d.tolerance_rel, d.agreement_status,
               b.source_accession AS fsds_accession, b.vendor_fact_id, b.available_at AS fsds_accepted,
               same.value AS warehouse_same_filing_value, coalesce(item_any.n, 0) AS warehouse_item_rows
        FROM fact_disagreement d
        JOIN vendor_baseline_facts b ON b.baseline_fact_id = d.baseline_fact_id
        LEFT JOIN same
          ON same.security_id = d.security_id AND same.item_id = d.item_id AND same.basis = d.basis
         AND same.period_end = d.period_end AND same.source_accession = b.source_accession
        LEFT JOIN item_any
          ON item_any.security_id = d.security_id AND item_any.item_id = d.item_id AND item_any.basis = d.basis
        WHERE d.source IN (?, ?)
        ORDER BY d.canonical_code, d.symbol, d.period_end
        """,
        [*sources, *sources, *sources],
    ).df()
    if frame.empty:
        return frame
    classes: list[str] = []
    hints: list[str | None] = []
    for row in frame.itertuples(index=False):
        abs_tol, rel_tol = float(row.tolerance_abs), float(row.tolerance_rel)
        same_value = row.warehouse_same_filing_value
        has_same = same_value is not None and not pd.isna(same_value)
        if row.agreement_status == "agrees":
            classes.append("agrees")
            hints.append(None)
        elif row.agreement_status == "missing_warehouse":
            classes.append("missing_warehouse_period" if int(row.warehouse_item_rows) > 0 else "missing_warehouse_item")
            hints.append(None)
        elif has_same and _within_tolerance(same_value, row.vendor_value, abs_tol, rel_tol):
            classes.append("vintage_difference")
            hints.append("warehouse latest revision is a later filing; same-accession revision agrees")
        else:
            classes.append("value_mismatch" if has_same else "value_mismatch_no_same_filing")
            hints.append(_mismatch_hint(float(same_value) if has_same else float(row.warehouse_value), float(row.vendor_value), abs_tol, rel_tol))
    frame["parity_class"] = classes
    frame["hint"] = hints
    frame["benchmark_item"] = frame["canonical_code"].map({item.canonical_code: item.name for item in CORE_ITEMS})
    return frame


def _mismatch_hint(warehouse: float, vendor: float, abs_tol: float, rel_tol: float) -> str | None:
    if _within_tolerance(-warehouse, vendor, abs_tol, rel_tol):
        return "sign_flip"
    for factor, label in ((1_000.0, "scale_x1000"), (1e-3, "scale_div1000"), (1e6, "scale_x1e6"), (1e-6, "scale_div1e6")):
        if _within_tolerance(warehouse, vendor * factor, abs_tol, rel_tol):
            return label
    return None


def _comparison_summary(frame: pd.DataFrame, unresolved: list[str], options: FsdsComparisonOptions) -> dict[str, Any]:
    if frame.empty:
        return {"rows": 0, "unresolved_issuers": unresolved}
    by_item: dict[str, Any] = {}
    for item in CORE_ITEMS:
        sub = frame[frame["canonical_code"] == item.canonical_code]
        if sub.empty:
            continue
        counts = {k: int(v) for k, v in sub["parity_class"].value_counts().items()}
        with_same = sub["warehouse_same_filing_value"].notna()
        same_ok = [
            _within_tolerance(w, v, float(a), float(r))
            for w, v, a, r in zip(
                sub.loc[with_same, "warehouse_same_filing_value"], sub.loc[with_same, "vendor_value"],
                sub.loc[with_same, "tolerance_abs"], sub.loc[with_same, "tolerance_rel"], strict=True,
            )
        ]
        failing = sub[sub["parity_class"] != "agrees"].head(options.failing_sample_limit)
        by_item[item.name] = {
            "rows": len(sub),
            "classes": counts,
            "latest_agreement_ratio": counts.get("agrees", 0) / len(sub),
            "same_filing_rows": int(with_same.sum()),
            "same_filing_agreement_ratio": (sum(same_ok) / len(same_ok)) if same_ok else None,
            "failing_sample": [
                {
                    "ticker": r.symbol,
                    "period_end": str(_as_date(r.period_end)),
                    "fsds_value": r.vendor_value,
                    "warehouse_latest": None if pd.isna(r.warehouse_value) else r.warehouse_value,
                    "warehouse_same_filing": None if pd.isna(r.warehouse_same_filing_value) else r.warehouse_same_filing_value,
                    "class": r.parity_class,
                    "hint": r.hint,
                    "fsds_accession": r.fsds_accession,
                }
                for r in failing.itertuples(index=False)
            ],
        }
    total = len(frame)
    agrees = int((frame["parity_class"] == "agrees").sum())
    return {
        "rows": total,
        "agrees": agrees,
        "latest_agreement_ratio": agrees / total if total else None,
        "classes": {k: int(v) for k, v in frame["parity_class"].value_counts().items()},
        "tolerance": {
            "relative": options.tolerance_rel,
            "absolute_monetary": options.tolerance_abs_monetary,
            "absolute_per_share": options.tolerance_abs_per_share,
        },
        "unresolved_issuers": unresolved,
        "by_item": by_item,
    }


def copy_warehouse_standardized_subset(store: DuckDBStore, warehouse_path: Path, ciks: Sequence[str]) -> int:
    """HEAVY SLOT ONLY: copy the panel's ``fundamental_standardized`` rows into a scratch store.

    Attaches the warehouse READ_ONLY, copies the rows whose CIK is in ``ciks`` (every revision),
    and detaches, so the comparison writes only to the scratch DB.
    """

    scratch = Path(store.path).resolve()
    if scratch == Path(warehouse_path).resolve():
        raise ValueError("scratch DB must differ from the warehouse")
    con = store.con
    literal = Path(warehouse_path).resolve().as_posix().replace("'", "''")
    con.execute(f"ATTACH '{literal}' AS p12_wh (READ_ONLY)")
    try:
        con.execute("CREATE OR REPLACE TEMP TABLE _p12_copy_ciks(cik BIGINT)")
        con.executemany("INSERT INTO _p12_copy_ciks VALUES (?)", [[int(c)] for c in ciks])
        scratch_cols = [r[0] for r in con.execute("SELECT column_name FROM information_schema.columns WHERE table_catalog = current_database() AND table_schema = 'main' AND table_name = 'fundamental_standardized' ORDER BY ordinal_position").fetchall()]
        wh_cols = {r[0] for r in con.execute("SELECT column_name FROM information_schema.columns WHERE table_catalog = 'p12_wh' AND table_name = 'fundamental_standardized'").fetchall()}
        cols = ", ".join(c for c in scratch_cols if c in wh_cols)
        con.execute("DELETE FROM fundamental_standardized WHERE TRY_CAST(cik AS BIGINT) IN (SELECT cik FROM _p12_copy_ciks)")
        con.execute(
            f"""
            INSERT INTO fundamental_standardized ({cols})
            SELECT {cols} FROM p12_wh.fundamental_standardized
            WHERE TRY_CAST(cik AS BIGINT) IN (SELECT cik FROM _p12_copy_ciks)
            """
        )
        return int(con.execute("SELECT count(*) FROM fundamental_standardized WHERE TRY_CAST(cik AS BIGINT) IN (SELECT cik FROM _p12_copy_ciks)").fetchone()[0])
    finally:
        con.execute("DETACH p12_wh")


# ---------------------------------------------------------------------------------------------
# 6-7. Full FSDS history: fetch every published quarter, stage it to Parquet (node X.4, gate U3)
# ---------------------------------------------------------------------------------------------

FSDS_INDEX_URL = "https://www.sec.gov/data-research/sec-markets-data/financial-statement-data-sets"
FSDS_HISTORY_FIRST_QUARTER = "2009q2"
#: Attempts per SEC request in the history fetch: an SEC 403/429 pauses the host once and is retried once; a
#: second refusal stops the run, so this job alone never reaches the host trip (5 consecutive block episodes).
FSDS_HISTORY_MAX_ATTEMPTS = 2
FSDS_INDEX_MAX_BYTES = 8 * 1024 * 1024
FETCH_LOCK_NAME = ".fsds-fetch.lock"
#: One staging directory per stager version, never re-staged in place over another version: v1
#: (``x4_fsds_stage_v1``: naive ``accepted``, DOUBLE ``value``) stays in ``data/staging/fsds`` until reclaimed.
STAGING_DIRNAME = "fsds-v2"
STAGING_MANIFEST_NAME = "fsds-staging-manifest.json"
STAGING_MANIFEST_SCHEMA = "x4_fsds_staging_manifest_v2"
STAGING_LOCK_NAME = ".fsds-stage.lock"
STAGING_WORK_DIRNAME = ".work"
#: v2 (X.4 fix round 1): ``accepted`` -> ``accepted_et_naive`` + ``accepted_utc``; ``value`` DECIMAL(28,4) exact;
#: Parquet key/value metadata; source order no longer forced (``preserve_insertion_order`` false, 1 thread).
STAGER_VERSION = "x4_fsds_stage_v2"
STAGE_MEMORY_LIMIT = "256MB"
STAGE_THREADS = 1
STAGE_ROW_GROUP_SIZE = 122_880
FSDS_TABLES: tuple[str, ...] = ("sub", "num", "tag", "pre")
#: FSDS ``accepted`` is EDGAR's acceptance wall clock in America/New_York, published without a zone (measured over
#: 433,717 submissions: acceptances after 17:30 roll ``filed`` to the next day -- EDGAR's 17:30 ET cutoff).
FSDS_ACCEPTED_ZONE = "America/New_York"
ACCEPTED_CLOCK = (
    "sub.accepted_utc (TIMESTAMP WITH TIME ZONE; Parquet isAdjustedToUTC=true) is the PIT acceptance instant: FSDS "
    "'accepted' read as America/New_York wall time and converted DST-aware (DuckDB ICU timezone()) to UTC. "
    "sub.accepted_et_naive (TIMESTAMP, no zone) is FSDS 'accepted' exactly as published: EDGAR's America/New_York "
    "wall clock; never compare it with a UTC clock. Nonexistent/ambiguous local times are counted per quarter "
    "(accepted_dst in the staging manifest)."
)
VALUE_TYPE = (
    "num.value = DECIMAL(28,4), exact: the FSDS NUMERIC(28,4) text (at most 24 integer and 4 fraction digits); "
    "any other text fails the quarter, nothing is rounded"
)
#: Plain decimal text with at most 24 integer and 4 fraction digits: exactly representable as DECIMAL(28,4).
_DECIMAL_28_4_TEXT = r"-?([0-9]{1,24}(\.[0-9]{0,4})?|\.[0-9]{1,4})"
NUM_ORPHAN_STATUS_UNRECOVERABLE = "unrecoverable from FSDS: no SUB row in any staged quarter (as published by SEC)"

# Canonical columns per table in the FSDS readme order. A source column not listed is kept as VARCHAR after them
# (``extra_columns`` in the manifest); a listed column absent from a quarter is a typed NULL (``missing_columns``).
# Kinds: None = VARCHAR as published (empty -> NULL, never trimmed); cik = 10-digit zero-padded text; date =
# yyyymmdd; accepted = two columns (see ACCEPTED_CLOCK); decimal = exact DECIMAL(28,4); the rest are strict casts,
# so a malformed value fails the quarter instead of becoming NULL.
_STAGE_SCHEMA: dict[str, tuple[tuple[str, str | None], ...]] = {
    "sub": (
        ("adsh", None), ("cik", "cik"), ("name", None), ("sic", None), ("countryba", None), ("stprba", None),
        ("cityba", None), ("zipba", None), ("bas1", None), ("bas2", None), ("baph", None), ("countryma", None),
        ("stprma", None), ("cityma", None), ("zipma", None), ("mas1", None), ("mas2", None), ("countryinc", None),
        ("stprinc", None), ("ein", None), ("former", None), ("changed", "date"), ("afs", None), ("wksi", "bool"),
        ("fye", None), ("form", None), ("period", "date"), ("fy", "int"), ("fp", None), ("filed", "date"),
        ("accepted", "accepted"), ("prevrpt", "bool"), ("detail", "bool"), ("instance", None), ("nciks", "int"),
        ("aciks", None),
    ),
    "num": (
        ("adsh", None), ("tag", None), ("version", None), ("ddate", "date"), ("qtrs", "int"), ("uom", None),
        ("segments", None), ("coreg", None), ("value", "decimal"), ("footnote", None),
    ),
    "tag": (
        ("tag", None), ("version", None), ("custom", "bool"), ("abstract", "bool"), ("datatype", None),
        ("iord", None), ("crdr", None), ("tlabel", None), ("doc", None),
    ),
    "pre": (
        ("adsh", None), ("report", "int"), ("line", "int"), ("stmt", None), ("inpth", "bool"), ("rfile", None),
        ("tag", None), ("version", None), ("plabel", None), ("negating", "bool"),
    ),
}
_STAGE_REQUIRED: dict[str, frozenset[str]] = {
    "sub": frozenset({"adsh", "cik", "form", "period", "filed", "accepted"}),
    "num": frozenset({"adsh", "tag", "version", "ddate", "qtrs", "uom", "value"}),
    "tag": frozenset({"tag", "version", "custom"}),
    "pre": frozenset({"adsh", "report", "line", "tag", "version"}),
}
_KIND_SQL: dict[str | None, str] = {
    None: "VARCHAR", "cik": "VARCHAR", "date": "DATE", "int": "INTEGER", "decimal": "DECIMAL(28,4)",
    "bool": "BOOLEAN", "timestamp": "TIMESTAMP",
}
#: Parquet key/value metadata per table (DuckDB ``KV_METADATA``), beside the stager version, table, quarter and zip
#: sha256 that every file carries.
_TABLE_KV_METADATA: dict[str, dict[str, str]] = {
    "sub": {"atx_accepted_zone": FSDS_ACCEPTED_ZONE, "atx_accepted_clock": ACCEPTED_CLOCK},
    "num": {"atx_value_type": VALUE_TYPE},
}
_IDENTIFIER = re.compile(r"^[a-z][a-z0-9_]*$")


def _staged_columns(table: str) -> list[tuple[str, str]]:
    """(name, DuckDB type) of every staged column of ``table``, ``quarter`` first: the manifest's output schema."""

    out = [("quarter", "VARCHAR")]
    for name, kind in _STAGE_SCHEMA[table]:
        if kind == "accepted":
            out += [(f"{name}_et_naive", "TIMESTAMP"), (f"{name}_utc", "TIMESTAMP WITH TIME ZONE")]
        else:
            out.append((name, _KIND_SQL[kind]))
    return out


def _quarter_key(quarter: str) -> tuple[int, int]:
    token = _validate_quarter(quarter)
    return int(token[:4]), int(token[5])


def fsds_quarter_range(first: str, last: str) -> list[str]:
    """Every calendar quarter from ``first`` to ``last`` inclusive (``2009q2`` style)."""

    year, part = _quarter_key(first)
    end = _quarter_key(last)
    out: list[str] = []
    while (year, part) <= end:
        out.append(f"{year}q{part}")
        year, part = (year + 1, 1) if part == 4 else (year, part + 1)
    return out


def fsds_index_quarters(session: Any, cache_dir: Path, *, index_url: str = FSDS_INDEX_URL) -> dict[str, Any]:
    """One GET of SEC's FSDS page; the quarters it links at exactly :data:`FSDS_URL_TEMPLATE`.

    The page is kept beside the zips (``fsds-index-<stamp>.html``) and the manifest's ``index`` entry records
    url, status, sha256, bytes and the listed quarters. A non-200 status or a page without quarter links raises.
    """

    requested_at = dt.datetime.now(dt.UTC)
    response = session.get(index_url, timeout=(30, 120), stream=True)
    try:
        status = int(response.status_code)
        if status != 200:
            raise RuntimeError(f"GET {index_url} returned HTTP {status}")
        body = read_bounded_response(response, FSDS_INDEX_MAX_BYTES)
        final_url = str(response.url)
    finally:
        response.close()
    listed: set[str] = set()
    other_zip_links: list[str] = []
    for href in re.findall(r'href="([^"]+\.zip)"', body.decode("utf-8", "replace")):
        absolute = urljoin(final_url, href)
        match = re.search(r"/(\d{4}q[1-4])\.zip$", urlsplit(absolute).path)
        if match and absolute == FSDS_URL_TEMPLATE.format(quarter=match.group(1)):
            listed.add(match.group(1))
        else:
            other_zip_links.append(href)
    if not listed:
        raise RuntimeError(f"{index_url} links no FSDS quarter zip at {FSDS_URL_TEMPLATE}")
    stamp = requested_at.strftime("%Y%m%dT%H%M%SZ")
    page_path = Path(cache_dir) / f"fsds-index-{stamp}.html"
    page_path.write_bytes(body)
    receipt = {
        "url": index_url,
        "final_url": final_url,
        "http_status": status,
        "requested_at_utc": requested_at.isoformat(timespec="milliseconds"),
        "sha256": hashlib.sha256(body).hexdigest(),
        "bytes": len(body),
        "page_file": page_path.name,
        "user_agent": SEC_USER_AGENT,
        "quarters_listed": sorted(listed, key=_quarter_key),
        "other_zip_links": other_zip_links,
    }
    manifest = _read_manifest(Path(cache_dir))
    manifest["index"] = receipt
    _write_manifest(Path(cache_dir), manifest)
    return receipt


def fetch_fsds_history(
    first: str = FSDS_HISTORY_FIRST_QUARTER,
    last: str | None = None,
    cache_dir: Path | None = None,
    *,
    session: Any | None = None,
    limiter: SecRateLimiter | None = None,
    index_url: str = FSDS_INDEX_URL,
    progress: Callable[[dict[str, Any]], None] | None = None,
) -> dict[str, Any]:
    """Step 1 of X.4: fetch every FSDS quarter SEC lists from ``first`` to ``last`` (default: the latest listed).

    One writer per cache (``.fsds-fetch.lock``); ``.part`` files left by a killed run are removed first. The
    default session is ``sec_session(max_attempts=FSDS_HISTORY_MAX_ATTEMPTS)`` on the host-wide limiter (after a
    preflight that refuses a tripped host). Zips already in the cache are re-hashed and reused. Quarters in the
    calendar range that the page does not list are reported as ``unavailable_in_range`` and never requested.
    """

    cache_dir = Path(cache_dir or default_cache_dir())
    cache_dir.mkdir(parents=True, exist_ok=True)
    first = _validate_quarter(first)
    with _single_writer_lock(cache_dir / FETCH_LOCK_NAME, timeout_s=1.0):
        stale_parts = sorted(p.name for p in cache_dir.glob(".*.zip.*.part"))
        for name in stale_parts:
            (cache_dir / name).unlink(missing_ok=True)
        preflight = None
        if session is None:
            limiter = limiter or default_sec_limiter()
            preflight = limiter.preflight()
            session = sec_session(limiter=limiter, max_attempts=FSDS_HISTORY_MAX_ATTEMPTS)
        index = fsds_index_quarters(session, cache_dir, index_url=index_url)
        listed: list[str] = index["quarters_listed"]
        last = _validate_quarter(last) if last else listed[-1]
        low, high = _quarter_key(first), _quarter_key(last)
        in_range = [q for q in listed if low <= _quarter_key(q) <= high]
        unavailable = [q for q in fsds_quarter_range(first, last) if q not in set(listed)]
        archives = fetch_fsds_quarters(in_range, cache_dir, session=session, limiter=limiter, progress=progress)
    downloaded = [a for a in archives if a.downloaded_now]
    return {
        "first": first,
        "last": last,
        "index": {k: index[k] for k in ("url", "http_status", "sha256", "bytes", "page_file", "requested_at_utc")},
        "listed_quarters": len(listed),
        "listed_out_of_range": [q for q in listed if not low <= _quarter_key(q) <= high],
        "quarters_in_range": len(in_range),
        "unavailable_in_range": unavailable,
        "downloaded_now": [a.quarter for a in downloaded],
        "reused": [a.quarter for a in archives if not a.downloaded_now],
        "bytes_downloaded_now": sum(a.size_bytes for a in downloaded),
        "bytes_in_range": sum(a.size_bytes for a in archives),
        "stale_parts_removed": stale_parts,
        "limiter_preflight": preflight,
        "limiter_after": limiter.status() if limiter is not None else None,
    }


def default_staging_dir() -> Path:
    return resolve_data_dir() / "staging" / STAGING_DIRNAME


def _read_staging_manifest(staging_dir: Path) -> dict[str, Any]:
    path = Path(staging_dir) / STAGING_MANIFEST_NAME
    if not path.is_file():
        return {"schema": STAGING_MANIFEST_SCHEMA, "quarters": {}}
    return json.loads(path.read_text(encoding="utf-8"))


def _staged_intact(entry: Mapping[str, Any] | None, zip_sha256: str, staging_dir: Path) -> bool:
    """A manifest entry of this stager version for this zip, with every table file present at its recorded size."""

    if not entry or entry.get("stager_version") != STAGER_VERSION or entry.get("zip_sha256") != zip_sha256:
        return False
    for table in FSDS_TABLES:
        info = entry.get("tables", {}).get(table)
        if info is None:
            return False
        try:
            if (Path(staging_dir) / info["path"]).stat().st_size != info["parquet_bytes"]:
                return False
        except OSError:
            return False
    return True


def _extract_counting(archive: zipfile.ZipFile, member: str, target: Path) -> tuple[int, int]:
    """Stream one member to ``target`` in 1 MiB chunks (CRC-checked by zipfile); return (bytes, lines)."""

    size = lines = 0
    last = b""
    with archive.open(member) as source, target.open("wb") as sink:
        while chunk := source.read(_COPY_CHUNK):
            sink.write(chunk)
            size += len(chunk)
            lines += chunk.count(b"\n")
            last = chunk[-1:]
    if size and last != b"\n":
        lines += 1
    return size, lines


def _sql_text(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _stage_scalar(name: str, kind: str | None, present: bool) -> str:
    """The typed SQL expression of one source column (a typed NULL when the quarter lacks it)."""

    sql_type = _KIND_SQL[kind]
    if not present:
        return f"CAST(NULL AS {sql_type})"
    ident = f'"{name}"'
    if kind is None:
        return f"nullif({ident}, '')"
    value = f"nullif(trim({ident}), '')"
    if kind == "cik":
        return f"lpad(CAST(CAST({value} AS BIGINT) AS VARCHAR), 10, '0')"
    if kind == "date":
        return f"CAST(strptime({value}, '%Y%m%d') AS DATE)"
    if kind == "decimal":
        # A plain CAST to DECIMAL would round a fifth fraction digit away; only exactly representable text passes.
        # Text of <= 14 characters has <= 14 integer digits, so it fits DECIMAL(18,4) (int64) and widens exactly;
        # DuckDB's VARCHAR -> DECIMAL(28,4) (int128) parse is ~7x slower (measured on 2013q1 num, 3.16M rows).
        return (
            f"CASE WHEN {value} IS NULL THEN NULL "
            f"WHEN NOT regexp_full_match({value}, {_sql_text(_DECIMAL_28_4_TEXT)}) "
            f"THEN error('FSDS {name} is not exact {sql_type} text: ' || {value}) "
            f"WHEN length({value}) <= 14 THEN CAST(CAST({value} AS DECIMAL(18,4)) AS {sql_type}) "
            f"ELSE CAST({value} AS {sql_type}) END"
        )
    return f"CAST({value} AS {sql_type})"


def _stage_columns(name: str, kind: str | None, present: bool) -> list[tuple[str, str]]:
    """(SQL expression, staged column name) pairs for one source column; see :func:`_staged_columns`."""

    if kind == "accepted":
        naive = _stage_scalar(name, "timestamp", present)
        return [(naive, f"{name}_et_naive"), (f"timezone({_sql_text(FSDS_ACCEPTED_ZONE)}, {naive})", f"{name}_utc")]
    return [(_stage_scalar(name, kind, present), name)]


def _accepted_dst_findings(con: duckdb.DuckDBPyConnection, sub_files: str) -> dict[str, Any]:
    """Acceptance wall times that do not name exactly one instant in America/New_York.

    ``nonexistent``: the wall time falls in a spring-forward gap (converting ``accepted_utc`` back does not give the
    published wall time). ``ambiguous``: the wall time falls in a fall-back overlap (the instant one hour earlier or
    later shows the same wall time); ICU picked one of the two. EDGAR accepts from 06:00 ET, so both are expected 0.
    """

    zone = _sql_text(FSDS_ACCEPTED_ZONE)
    gap = f"timezone({zone}, accepted_utc) <> accepted_et_naive"
    overlap = (
        f"(timezone({zone}, accepted_utc + INTERVAL 1 HOUR) = accepted_et_naive "
        f"OR timezone({zone}, accepted_utc - INTERVAL 1 HOUR) = accepted_et_naive)"
    )
    nonexistent, ambiguous, null_rows, flagged = con.execute(
        f"SELECT count(*) FILTER (WHERE {gap}), count(*) FILTER (WHERE {overlap}), "
        f"count(*) FILTER (WHERE accepted_utc IS NULL), "
        f"coalesce(list(adsh ORDER BY adsh) FILTER (WHERE {gap} OR {overlap}), []) "
        f"FROM read_parquet({_sql_text(sub_files)})"
    ).fetchone()
    return {"nonexistent": int(nonexistent), "ambiguous": int(ambiguous), "accepted_null_rows": int(null_rows),
            "adsh": list(flagged)}


def _stage_table(
    con: duckdb.DuckDBPyConnection, quarter: str, table: str, source_path: Path, target: Path,
    kv_metadata: Mapping[str, str],
) -> dict[str, Any]:
    source, header = _tsv_source(source_path)
    if len(set(header)) != len(header) or not all(_IDENTIFIER.match(name) for name in header):
        raise ValueError(f"{quarter} {table}.txt has an unexpected header {header!r}")
    missing_required = _STAGE_REQUIRED[table] - set(header)
    if missing_required:
        raise ValueError(f"{quarter} {table}.txt lacks columns {sorted(missing_required)}; header={header}")
    schema = _STAGE_SCHEMA[table]
    known = {name for name, _ in schema}
    extra = [name for name in header if name not in known]
    parts = [f"'{_validate_quarter(quarter)}' AS quarter"]
    for name, kind in schema:
        parts += [f'{expr} AS "{alias}"' for expr, alias in _stage_columns(name, kind, name in header)]
    parts += [f"nullif(\"{name}\", '') AS \"{name}\"" for name in extra]
    literal = target.resolve().as_posix().replace("'", "''")
    kv = {**kv_metadata, "atx_table": table, **_TABLE_KV_METADATA.get(table, {})}
    if not all(_IDENTIFIER.match(key) for key in kv):
        raise ValueError(f"Parquet metadata keys must be identifiers: {sorted(kv)}")
    kv_sql = ", ".join(f"{key}: {_sql_text(value)}" for key, value in kv.items())
    rows = con.execute(
        f"COPY (SELECT {', '.join(parts)} FROM {source}) TO '{literal}' "
        f"(FORMAT parquet, COMPRESSION zstd, ROW_GROUP_SIZE {STAGE_ROW_GROUP_SIZE}, KV_METADATA {{{kv_sql}}})"
    ).fetchone()[0]
    digest, size = _sha256_file(target)
    info: dict[str, Any] = {
        "rows": int(rows),
        "parquet_bytes": size,
        "parquet_sha256": digest,
        "source_columns": header,
        "extra_columns": extra,
        "missing_columns": [name for name, _ in schema if name not in header],
    }
    if table == "sub":
        info["accepted_dst"] = _accepted_dst_findings(con, target.resolve().as_posix())
    return info


def _stage_quarter(
    quarter: str, cache_dir: Path, archive_entry: Mapping[str, Any], staging_dir: Path, work_root: Path,
) -> dict[str, Any]:
    """Stage one quarter into a private work directory, then move its four files into place."""

    started = time.monotonic()
    zip_path = cache_dir / f"{quarter}.zip"
    digest, zip_bytes = _sha256_file(zip_path)
    if digest != archive_entry["sha256"]:
        raise RuntimeError(f"{zip_path} sha256 {digest} != recorded {archive_entry['sha256']}")
    work = work_root / f"{quarter}-{uuid.uuid4().hex[:8]}"
    work.mkdir(parents=True)
    tables: dict[str, Any] = {}
    try:
        config = bounded_config(STAGE_MEMORY_LIMIT, STAGE_THREADS, temp_directory=work / "duckdb_tmp",
                                max_temp_directory_size="8GB")
        con = duckdb.connect(":memory:", config=config)  # preserve_insertion_order false (doctrine), 1 thread
        kv_metadata = {"atx_stager_version": STAGER_VERSION, "atx_quarter": quarter, "atx_zip_sha256": digest}
        try:
            # accepted_utc is a TIMESTAMPTZ built with an explicit zone; the session zone only fixes the
            # INTERVAL arithmetic of the DST findings (absolute hours in UTC).
            con.execute("SET TimeZone='UTC'")
            with zipfile.ZipFile(zip_path) as zipped:
                names = set(zipped.namelist())
                for table in FSDS_TABLES:
                    member = f"{table}.txt"
                    if member not in names:
                        raise RuntimeError(f"{zip_path} lacks {member}; members={sorted(names)}")
                    source_path = work / member
                    source_bytes, source_lines = _extract_counting(zipped, member, source_path)
                    info = _stage_table(con, quarter, table, source_path, work / f"{table}.parquet", kv_metadata)
                    source_path.unlink()
                    info.update(
                        path=f"{table}/{quarter}.parquet",
                        source_bytes=source_bytes,
                        source_lines=source_lines,
                        # > 0 only when quoted values span lines (or blank lines): rows are what the strict parser read
                        line_rows_delta=max(source_lines - 1, 0) - info["rows"],
                    )
                    tables[table] = info
        finally:
            con.close()
        for table in FSDS_TABLES:
            destination = staging_dir / table / f"{quarter}.parquet"
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.replace(work / f"{table}.parquet", destination)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return {
        "quarter": quarter,
        "zip_sha256": digest,
        "zip_bytes": zip_bytes,
        "stager_version": STAGER_VERSION,
        "staged_at_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
        "elapsed_s": round(time.monotonic() - started, 3),
        "tables": tables,
    }


def stage_fsds_history(
    quarters: Iterable[str] | None = None,
    *,
    cache_dir: Path | None = None,
    staging_dir: Path | None = None,
    progress: Callable[[dict[str, Any]], None] | None = None,
) -> dict[str, Any]:
    """Step 2 of X.4: stage every fetched quarter (default: all in the fetch manifest) to Parquet, resumably.

    One writer per staging dir (``.fsds-stage.lock``). A quarter is skipped when its manifest entry matches this
    stager version and the zip's recorded sha256 and its four files are present at their recorded sizes; any other
    quarter is (re)staged in ``.work/<quarter>-<id>`` and its files replace ``<table>/<quarter>.parquet`` before the
    manifest entry is written. A killed run leaves at most a work directory (removed at the next start) and replaced
    files of a quarter without an entry (re-staged at the next start); a quarter is never staged into two files.
    A directory holding another stager version's quarters is refused (each version stages into its own directory).
    The run ends with a cross-quarter pass recording the NUM accessions that have no SUB row in any quarter.
    """

    cache_dir = Path(cache_dir or default_cache_dir())
    staging_dir = Path(staging_dir or default_staging_dir())
    staging_dir.mkdir(parents=True, exist_ok=True)
    archives: dict[str, Any] = _read_manifest(cache_dir).get("archives", {})
    chosen = archives if quarters is None else quarters
    wanted = sorted({_validate_quarter(q) for q in chosen}, key=_quarter_key)
    unknown = [q for q in wanted if q not in archives]
    if unknown:
        raise ValueError(f"quarters not in {MANIFEST_NAME} (fetch them first): {unknown}")
    started = time.monotonic()
    staged: list[str] = []
    reused: list[str] = []
    with _single_writer_lock(staging_dir / STAGING_LOCK_NAME, timeout_s=1.0):
        work_root = staging_dir / STAGING_WORK_DIRNAME
        stale_work = sorted(p.name for p in work_root.iterdir()) if work_root.is_dir() else []
        if work_root.exists():
            shutil.rmtree(work_root)
        manifest = _read_staging_manifest(staging_dir)
        entries: dict[str, Any] = manifest.setdefault("quarters", {})
        foreign = sorted({str(e.get("stager_version")) for e in entries.values()} - {STAGER_VERSION})
        if foreign:
            raise ValueError(
                f"{staging_dir} holds quarters staged by {foreign}; stage {STAGER_VERSION} into its own directory "
                f"(default {default_staging_dir()}), never in place over another version that readers may use"
            )
        manifest.update(
            schema=STAGING_MANIFEST_SCHEMA,
            stager_version=STAGER_VERSION,
            accepted_zone=FSDS_ACCEPTED_ZONE,
            accepted_clock=ACCEPTED_CLOCK,
            value_type=VALUE_TYPE,
            output_schema={table: [list(column) for column in _staged_columns(table)] for table in FSDS_TABLES},
        )
        for quarter in wanted:
            if _staged_intact(entries.get(quarter), archives[quarter]["sha256"], staging_dir):
                reused.append(quarter)
                if progress is not None:
                    progress({"quarter": quarter, "stage": "reused"})
                continue
            entry = _stage_quarter(quarter, cache_dir, archives[quarter], staging_dir, work_root)
            entries[quarter] = entry
            _write_json_atomic(staging_dir / STAGING_MANIFEST_NAME, manifest)
            staged.append(quarter)
            if progress is not None:
                progress({"quarter": quarter, "stage": "staged", "elapsed_s": entry["elapsed_s"],
                          "rows": {t: entry["tables"][t]["rows"] for t in FSDS_TABLES},
                          "parquet_bytes": sum(entry["tables"][t]["parquet_bytes"] for t in FSDS_TABLES),
                          "accepted_dst": entry["tables"]["sub"]["accepted_dst"]})
        # Cross-quarter pass over everything staged: NUM accessions without a SUB row in ANY quarter (X.4 review
        # M2). Staged unfiltered; recorded here so a consumer's inner join to sub drops them visibly, not silently.
        orphan_started = time.monotonic()
        con = _staging_reader(work_root / f"orphans-{uuid.uuid4().hex[:8]}")
        try:
            orphans = _num_orphan_accessions(con, staging_dir) if entries else []
        finally:
            con.close()
        manifest["num_orphan_accessions"] = orphans
        manifest["num_orphan_facts"] = sum(o["num_rows"] for o in orphans)
        _write_json_atomic(staging_dir / STAGING_MANIFEST_NAME, manifest)
        shutil.rmtree(work_root, ignore_errors=True)
    return {
        "staging_dir": str(staging_dir),
        "stager_version": STAGER_VERSION,
        "quarters_requested": len(wanted),
        "staged_now": staged,
        "reused": reused,
        "stale_work_dirs_removed": stale_work,
        "num_orphan_accessions": orphans,
        "orphan_pass_s": round(time.monotonic() - orphan_started, 3),
        "elapsed_s": round(time.monotonic() - started, 3),
    }


def _staging_reader(spill: Path) -> duckdb.DuckDBPyConnection:
    """A bounded (256MB, 1 thread) in-memory DuckDB with a private spill dir, session zone UTC."""

    con = duckdb.connect(":memory:", config=bounded_config(STAGE_MEMORY_LIMIT, STAGE_THREADS, temp_directory=spill))
    con.execute("SET TimeZone='UTC'")
    return con


def _table_files(staging_dir: Path, table: str) -> str:
    return (Path(staging_dir) / table).resolve().as_posix() + "/*.parquet"


def _num_orphan_accessions(con: duckdb.DuckDBPyConnection, staging_dir: Path) -> list[dict[str, Any]]:
    """Count global NUM orphans using bounded, source-quarter passes.

    Materialize each NUM/PRE file's accession counts before joining: a CTE alone
    lets the optimizer move the join below aggregation and hash all-history facts.
    SUB membership is global, as are PRE totals for an orphan accession. The final
    aggregation also preserves the result if a quarter has several Parquet files.
    These are audit scratch tables only; staged files and their receipts are unchanged.
    """

    files = {table: sorted((Path(staging_dir) / table).glob("*.parquet")) for table in ("sub", "num", "pre")}
    for table, paths in files.items():
        if not paths:
            raise ValueError(f"No staged {table} Parquet files for the NUM orphan audit")
    scratch = ("x4_orphan_subs", "x4_orphan_num", "x4_orphan_pre", "x4_orphan_file")
    try:
        con.execute("CREATE TEMP TABLE x4_orphan_subs (adsh VARCHAR)")
        con.execute("CREATE TEMP TABLE x4_orphan_num (adsh VARCHAR, quarter VARCHAR, num_rows BIGINT)")
        con.execute("CREATE TEMP TABLE x4_orphan_pre (adsh VARCHAR, pre_rows BIGINT)")
        for path in files["sub"]:
            con.execute(f"INSERT INTO x4_orphan_subs SELECT adsh FROM read_parquet({_sql_text(path.as_posix())})")
        for path in files["num"]:
            con.execute(
                "CREATE OR REPLACE TEMP TABLE x4_orphan_file AS "
                f"SELECT adsh, quarter, count(*) AS num_rows FROM read_parquet({_sql_text(path.as_posix())}) "
                "GROUP BY adsh, quarter"
            )
            con.execute(
                "INSERT INTO x4_orphan_num SELECT n.* FROM x4_orphan_file n "
                "ANTI JOIN x4_orphan_subs s ON n.adsh = s.adsh"
            )
        if con.execute("SELECT count(*) FROM x4_orphan_num").fetchone()[0]:
            for path in files["pre"]:
                con.execute(
                    "CREATE OR REPLACE TEMP TABLE x4_orphan_file AS "
                    f"SELECT adsh, count(*) AS pre_rows FROM read_parquet({_sql_text(path.as_posix())}) "
                    "GROUP BY adsh"
                )
                con.execute(
                    "INSERT INTO x4_orphan_pre SELECT p.* FROM x4_orphan_file p "
                    "SEMI JOIN x4_orphan_num n ON p.adsh = n.adsh"
                )
        rows = con.execute(
            "WITH orphan AS (SELECT adsh, quarter, sum(num_rows) AS num_rows FROM x4_orphan_num GROUP BY ALL), "
            "pre_rows AS (SELECT adsh, sum(pre_rows) AS pre_rows FROM x4_orphan_pre GROUP BY adsh) "
            "SELECT o.adsh, o.quarter, o.num_rows, coalesce(p.pre_rows, 0) FROM orphan o "
            "LEFT JOIN pre_rows p ON o.adsh = p.adsh ORDER BY o.quarter, o.adsh"
        ).fetchall()
    finally:
        for table in reversed(scratch):
            con.execute(f"DROP TABLE IF EXISTS {table}")
    return [{"adsh": adsh, "quarter": quarter, "num_rows": int(num_rows), "pre_rows": int(pre_rows),
             "status": NUM_ORPHAN_STATUS_UNRECOVERABLE} for adsh, quarter, num_rows, pre_rows in rows]


def _verify_completeness(fetch_manifest: Mapping[str, Any], entries: Mapping[str, Any]) -> tuple[dict[str, Any], list[str]]:
    """Every quarter the SEC index lists from :data:`FSDS_HISTORY_FIRST_QUARTER` on is fetched and staged by this
    stager version, and every staged table parsed each source line into one row with the full column set."""

    archives: dict[str, Any] = fetch_manifest.get("archives", {})
    listed: list[str] = list(fetch_manifest.get("index", {}).get("quarters_listed", []))
    first = _quarter_key(FSDS_HISTORY_FIRST_QUARTER)
    expected = [q for q in listed if _quarter_key(q) >= first]
    problems: list[str] = []
    if not expected:
        problems.append("fetch manifest has no SEC index listing: completeness is unproven")
    not_fetched = [q for q in expected if q not in archives]
    not_staged = [q for q in expected if q not in entries]
    problems += [f"{q}: listed by the SEC index but not fetched" for q in not_fetched]
    problems += [f"{q}: listed by the SEC index but not staged" for q in not_staged]
    for quarter, entry in sorted(entries.items(), key=lambda item: _quarter_key(item[0])):
        if entry.get("stager_version") != STAGER_VERSION:
            problems.append(f"{quarter}: stager {entry.get('stager_version')} != {STAGER_VERSION}")
        for table in FSDS_TABLES:
            info = entry.get("tables", {}).get(table, {})
            if info.get("line_rows_delta") != 0:
                problems.append(f"{table} {quarter}: line_rows_delta {info.get('line_rows_delta')} != 0")
            if info.get("extra_columns") or info.get("missing_columns"):
                problems.append(f"{table} {quarter}: extra {info.get('extra_columns')} / missing "
                                f"{info.get('missing_columns')} source columns")
    return {
        "index_quarters_listed": len(listed),
        "index_page_file": fetch_manifest.get("index", {}).get("page_file"),
        "expected_from": FSDS_HISTORY_FIRST_QUARTER,
        "expected": len(expected),
        "not_fetched": not_fetched,
        "not_staged": not_staged,
        "staged_not_listed": sorted(set(entries) - set(listed), key=_quarter_key),
    }, problems


def _verify_file_contract(
    con: duckdb.DuckDBPyConnection, staging_dir: Path, table: str, files: Sequence[Path], entries: Mapping[str, Any],
) -> list[str]:
    """Every file of ``table`` has the staged schema and this stager's key/value metadata for its quarter."""

    problems: list[str] = []
    expected_schema = _staged_columns(table)
    expected_kv = {"atx_stager_version": STAGER_VERSION, "atx_table": table, **_TABLE_KV_METADATA.get(table, {})}
    kv_by_file: dict[str, dict[str, str]] = {}
    for file_name, key, value in con.execute(
        f"SELECT file_name, decode(key), decode(value) FROM parquet_kv_metadata({_sql_text(_table_files(staging_dir, table))})"
    ).fetchall():
        kv_by_file.setdefault(Path(str(file_name)).stem, {})[str(key)] = str(value)
    for path in files:
        quarter = path.stem
        schema = [(str(row[0]), str(row[1])) for row in con.execute(
            f"DESCRIBE SELECT * FROM read_parquet({_sql_text(path.resolve().as_posix())})"
        ).fetchall()]
        if schema != expected_schema:
            problems.append(f"{table} {quarter}: schema {schema} != staged schema {expected_schema}")
        kv = kv_by_file.get(quarter, {})
        want = {**expected_kv, "atx_quarter": quarter,
                "atx_zip_sha256": str(entries.get(quarter, {}).get("zip_sha256"))}
        wrong = sorted(key for key, value in want.items() if kv.get(key) != value)
        if wrong:
            problems.append(f"{table} {quarter}: Parquet key/value metadata missing or wrong: {wrong}")
    return problems


def verify_fsds_staging(
    staging_dir: Path | None = None, cache_dir: Path | None = None, *, rehash: bool = True,
) -> dict[str, Any]:
    """Prove the staged history.

    * Completeness: every quarter the saved SEC index lists from 2009q2 on is fetched and staged by this stager
      version, with line/row delta 0 and no extra or missing source columns (:func:`_verify_completeness`).
    * Per table: one file and one ``quarter`` value per quarter, rows equal to the manifest, no file without a
      manifest entry, the staged schema and key/value metadata in every file; with ``rehash`` every Parquet sha256
      equals its entry; every entry's zip sha256 equals the fetch manifest's.
    * Recomputed from the files: the DST findings of ``accepted`` equal the manifest's per-quarter counts and the
      cross-quarter NUM orphan accessions equal the manifest's list.

    Takes the staging lock (never beside a stager).
    """

    cache_dir = Path(cache_dir or default_cache_dir())
    staging_dir = Path(staging_dir or default_staging_dir())
    fetch_manifest = _read_manifest(cache_dir)
    archives: dict[str, Any] = fetch_manifest.get("archives", {})
    problems: list[str] = []
    out_tables: dict[str, Any] = {}
    with _single_writer_lock(staging_dir / STAGING_LOCK_NAME, timeout_s=1.0):
        manifest = _read_staging_manifest(staging_dir)
        entries: dict[str, Any] = manifest.get("quarters", {})
        completeness, completeness_problems = _verify_completeness(fetch_manifest, entries)
        problems += completeness_problems
        for key, want in (("stager_version", STAGER_VERSION), ("accepted_clock", ACCEPTED_CLOCK),
                          ("accepted_zone", FSDS_ACCEPTED_ZONE), ("value_type", VALUE_TYPE)):
            if manifest.get(key) != want:
                problems.append(f"staging manifest {key} {manifest.get(key)!r} != {want!r}")
        spill = staging_dir / STAGING_WORK_DIRNAME / f"verify-{uuid.uuid4().hex[:8]}"
        con = _staging_reader(spill)
        dst: dict[str, Any] = {}
        num_orphans: list[dict[str, Any]] = []
        try:
            for table in FSDS_TABLES:
                files = sorted((staging_dir / table).glob("*.parquet"))
                orphans = [f.stem for f in files if f.stem not in entries]
                if files:
                    problems += _verify_file_contract(con, staging_dir, table, files, entries)
                rows_by_quarter: dict[str, int] = {}
                files_by_quarter: dict[str, set[str]] = {}
                wrong_file = 0
                if files:
                    pattern = _sql_text(_table_files(staging_dir, table))
                    for quarter, filename, count in con.execute(
                        f"SELECT quarter, filename, count(*) FROM read_parquet({pattern}, filename=true) GROUP BY ALL"
                    ).fetchall():
                        stem = Path(str(filename)).stem
                        rows_by_quarter[quarter] = rows_by_quarter.get(quarter, 0) + int(count)
                        files_by_quarter.setdefault(quarter, set()).add(stem)
                        wrong_file += int(stem != quarter)
                duplicates = sorted(q for q, stems in files_by_quarter.items() if len(stems) > 1)
                for quarter, entry in entries.items():
                    info = entry["tables"][table]
                    if rows_by_quarter.get(quarter) != info["rows"]:
                        problems.append(f"{table} {quarter}: {rows_by_quarter.get(quarter)} rows != manifest {info['rows']}")
                    if rehash:
                        path = staging_dir / info["path"]
                        digest = _sha256_file(path)[0] if path.is_file() else None
                        if digest != info["parquet_sha256"]:
                            problems.append(f"{table} {quarter}: parquet sha256 {digest} != manifest")
                problems += [f"{table} {q}: file without a manifest entry" for q in orphans]
                problems += [f"{table} {q}: quarter in more than one file" for q in duplicates]
                if wrong_file:
                    problems.append(f"{table}: {wrong_file} (quarter, file) groups whose quarter differs from the file")
                out_tables[table] = {
                    "files": len(files),
                    "quarters": len(rows_by_quarter),
                    "rows": sum(rows_by_quarter.values()),
                    "parquet_bytes": sum(f.stat().st_size for f in files),
                    "duplicate_quarters": duplicates,
                    "orphan_files": orphans,
                    "quarter_file_mismatches": wrong_file,
                }
            if entries and out_tables["sub"]["files"]:
                dst = _accepted_dst_findings(con, _table_files(staging_dir, "sub"))
                recorded = {key: sum(int(e["tables"]["sub"].get("accepted_dst", {}).get(key, -1)) for e in entries.values())
                            for key in ("nonexistent", "ambiguous", "accepted_null_rows")}
                for key, value in recorded.items():
                    if dst[key] != value:
                        problems.append(f"sub accepted_dst {key}: recomputed {dst[key]} != manifest {value}")
            if entries and all(out_tables[t]["files"] for t in ("sub", "num", "pre")):
                num_orphans = _num_orphan_accessions(con, staging_dir)
                if num_orphans != manifest.get("num_orphan_accessions"):
                    problems.append(f"num orphan accessions recomputed {num_orphans} != manifest "
                                    f"{manifest.get('num_orphan_accessions')}")
        finally:
            con.close()
            shutil.rmtree(spill, ignore_errors=True)
        for quarter, entry in entries.items():
            if archives.get(quarter, {}).get("sha256") != entry.get("zip_sha256"):
                problems.append(f"{quarter}: staged from zip {entry.get('zip_sha256')} != fetch manifest")
    return {
        "staging_dir": str(staging_dir),
        "stager_version": manifest.get("stager_version"),
        "quarters": sorted(entries, key=_quarter_key),
        "quarter_count": len(entries),
        "completeness": completeness,
        "rehashed": rehash,
        "tables": out_tables,
        "accepted_dst": dst,
        "num_orphan_accessions": num_orphans,
        "num_orphan_facts": sum(o["num_rows"] for o in num_orphans),
        "problems": problems,
        "ok": not problems,
    }


def main(argv: Sequence[str] | None = None) -> int:
    """``python -m atx_db.fsds_baseline history-fetch | stage | verify`` -- node X.4, full FSDS history.

    Prints one JSON line per event and a final summary line (also written to ``--out``). Exit 3 when the host SEC
    limiter refuses (tripped host or unreadable state; never auto-cleared), 1 when ``verify`` finds a problem.
    """

    import argparse

    parser = argparse.ArgumentParser(prog="python -m atx_db.fsds_baseline", description=main.__doc__)
    parser.add_argument("--cache-dir", default=None, help="zips + manifest (default data/cache/P12-fsds)")
    parser.add_argument("--staging-dir", default=None, help=f"Parquet staging root (default data/staging/{STAGING_DIRNAME})")
    parser.add_argument("--out", default=None, help="also write the summary JSON to this path")
    commands = parser.add_subparsers(dest="command", required=True)
    fetch = commands.add_parser("history-fetch", help="SEC index page + every listed quarter from --first on")
    fetch.add_argument("--first", default=FSDS_HISTORY_FIRST_QUARTER)
    fetch.add_argument("--last", default=None, help="default: the latest quarter the index lists")
    stage = commands.add_parser("stage", help="stream sub/num/tag/pre of fetched quarters to Parquet (resumable)")
    stage.add_argument("--quarters", nargs="+", default=None, help="default: every quarter in the fetch manifest")
    verify = commands.add_parser("verify", help="re-hash staged files; prove one file per (table, quarter)")
    verify.add_argument("--no-rehash", action="store_true")
    args = parser.parse_args(argv)

    def emit(event: Mapping[str, Any]) -> None:
        print(json.dumps(event, sort_keys=True, default=str), flush=True)

    cache_dir = Path(args.cache_dir) if args.cache_dir else None
    staging_dir = Path(args.staging_dir) if args.staging_dir else None
    emit({"event": "start", "command": args.command, "module": __file__, "pid": os.getpid(),
          "at_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds")})
    try:
        if args.command == "history-fetch":
            summary = fetch_fsds_history(args.first, args.last, cache_dir, progress=emit)
        elif args.command == "stage":
            summary = stage_fsds_history(args.quarters, cache_dir=cache_dir, staging_dir=staging_dir, progress=emit)
        else:
            summary = verify_fsds_staging(staging_dir, cache_dir, rehash=not args.no_rehash)
    except SecBlockedError as exc:
        emit({"event": "sec_blocked", "error": str(exc)})
        return 3
    payload = {"event": "summary", "command": args.command,
               "at_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"), **summary}
    emit(payload)
    if args.out:
        _write_json_atomic(Path(args.out), json.loads(json.dumps(payload, default=str)))
    return 0 if summary.get("ok", True) else 1


if __name__ == "__main__":
    raise SystemExit(main())
