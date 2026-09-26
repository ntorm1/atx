"""P12: external validation of standardized fundamentals against SEC Financial Statement Data Sets.

The SEC (DERA) publishes quarterly Financial Statement Data Sets (FSDS): ``sub.txt`` (one row per
submission: ``adsh, cik, form, period, fy, fp, filed, accepted, prevrpt``), ``num.txt`` (one row
per numeric XBRL fact: ``adsh, tag, version, ddate, qtrs, uom, [segments,] coreg, value``) and
``tag.txt`` (tag definitions with the ``custom`` extension flag). FSDS is SEC's own extraction of
the filings the warehouse ingests through CompanyFacts, so agreement on (issuer, canonical item,
period, filing) is an independent check of ingestion, period selection, revision handling and
standardization.

Pipeline (bounded; nothing here opens the governed warehouse by itself):

1. :func:`fetch_fsds_quarters` -- ruling RX10: at most eight quarterly zips, approved SEC user
   agent, <= 4 requests/s, streamed into ``data/cache/P12-fsds`` with a SHA-256 manifest. A cached
   zip is re-hashed and reused, never downloaded again.
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
EDGAR acceptance timestamp as published (not a warehouse PIT clock); it only orders FSDS vintages.
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
from collections.abc import Callable, Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import duckdb
import pandas as pd

from .connection import DuckDBStore, resolve_data_dir
from .fact_disagreement import FactDisagreementOptions, refresh_fact_disagreement
from .item_registry import FundamentalItemSeedRow, read_fundamental_item_seed
from .standardization import StandardizationRule, default_standardization_rules
from .statement_map_seed import FundamentalStatementMapRow, read_statement_map_seed

FSDS_URL_TEMPLATE = "https://www.sec.gov/files/dera/data/financial-statement-data-sets/{quarter}.zip"
SEC_USER_AGENT = "atx-db/0.1 atx-research@example.com"
RX10_MAX_ZIPS = 8
MIN_REQUEST_INTERVAL_S = 0.25
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


def _write_manifest(cache_dir: Path, manifest: Mapping[str, Any]) -> None:
    path = cache_dir / MANIFEST_NAME
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    temporary.write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    os.replace(temporary, path)


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


def fetch_fsds_quarters(
    quarters: Sequence[str] = BENCHMARK_QUARTERS,
    cache_dir: Path | None = None,
    *,
    session: Any | None = None,
    sleep: Callable[[float], None] = time.sleep,
    monotonic: Callable[[], float] = time.monotonic,
) -> list[FsdsArchive]:
    """Download (once) and hash FSDS quarterly zips under the RX10 cap.

    A zip already on disk is re-hashed and reused -- never downloaded again; a manifest/zip hash
    mismatch or a recorded-but-missing zip is an error rather than a silent re-download. The cap
    counts every distinct quarter ever recorded in the manifest, not just this call.
    """

    cache_dir = Path(cache_dir or default_cache_dir())
    cache_dir.mkdir(parents=True, exist_ok=True)
    wanted = list(dict.fromkeys(_validate_quarter(q) for q in quarters))
    manifest = _read_manifest(cache_dir)
    recorded: dict[str, Any] = manifest.setdefault("archives", {})
    if len(set(recorded) | set(wanted)) > RX10_MAX_ZIPS:
        raise ValueError(
            f"RX10 allows at most {RX10_MAX_ZIPS} FSDS zips; recorded={sorted(recorded)} requested={wanted}"
        )

    last_request = float("-inf")
    results: list[FsdsArchive] = []
    for quarter in wanted:
        path = cache_dir / f"{quarter}.zip"
        url = FSDS_URL_TEMPLATE.format(quarter=quarter)
        entry = recorded.get(quarter)
        if path.is_file():
            digest, size = _sha256_file(path)
            if entry is None:
                entry = {"quarter": quarter, "url": url, "sha256": digest, "bytes": size,
                         "fetched_at_utc": None, "note": "adopted pre-existing file; not downloaded"}
                recorded[quarter] = entry
                _write_manifest(cache_dir, manifest)
            elif entry["sha256"] != digest:
                raise RuntimeError(f"{path} sha256 {digest} != recorded {entry['sha256']}; refusing to re-download")
            results.append(FsdsArchive(quarter, path, digest, size, url, False))
            continue
        if entry is not None:
            raise RuntimeError(f"{quarter} is recorded in {MANIFEST_NAME} but {path} is missing; refusing to re-download")

        if session is None:
            import requests

            session = requests.Session()
        wait = MIN_REQUEST_INTERVAL_S - (monotonic() - last_request)
        if wait > 0:
            sleep(wait)
        last_request = monotonic()
        headers = {"User-Agent": SEC_USER_AGENT, "Accept-Encoding": "gzip, deflate"}
        temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.part")
        digest = hashlib.sha256()
        size = 0
        try:
            with session.get(url, headers=headers, stream=True, timeout=(30, 300)) as response:
                status = int(response.status_code)
                if status != 200:
                    raise RuntimeError(f"GET {url} returned HTTP {status}")
                last_modified = response.headers.get("Last-Modified")
                with temporary.open("wb") as handle:
                    for chunk in response.iter_content(chunk_size=_COPY_CHUNK):
                        if chunk:
                            handle.write(chunk)
                            digest.update(chunk)
                            size += len(chunk)
            with zipfile.ZipFile(temporary) as archive:
                members = sorted(archive.namelist())
            missing = {"sub.txt", "num.txt", "tag.txt"} - set(members)
            if missing:
                raise RuntimeError(f"{url} lacks FSDS members {sorted(missing)}")
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)
        recorded[quarter] = {
            "quarter": quarter,
            "url": url,
            "sha256": digest.hexdigest(),
            "bytes": size,
            "fetched_at_utc": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
            "user_agent": SEC_USER_AGENT,
            "http_status": status,
            "last_modified": last_modified,
            "members": members,
        }
        _write_manifest(cache_dir, manifest)
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
        )

    out: dict[tuple[int, str], CanonicalRule] = {}
    for item in items:
        for basis in _BASES_BY_KIND[item.period_kind]:
            rule = build(item.item_id, basis, item.unit_kind)
            if rule.canonical_code != item.canonical_code:
                raise ValueError(f"{item.name}: rule code {rule.canonical_code} != {item.canonical_code}")
            out[(item.item_id, basis)] = rule
            for input_id in rule.inputs:
                if (input_id, basis) not in out:
                    kind = "per_share" if unit_types.get(input_id) == "per_share" else "monetary"
                    out[(input_id, basis)] = build(input_id, basis, kind)
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

    out_frames: list[pd.DataFrame] = []
    for item in items:
        for basis in _BASES_BY_KIND[item.period_kind]:
            rule = rules_map[(item.item_id, basis)]
            direct = raw[(raw["item_id"] == item.item_id) & (raw["basis"] == basis)]
            direct_rows = direct[[*_FILING_COLUMNS, "ddate"]].copy()
            direct_rows["value"] = _apply_rule_value(direct["value"].astype(float), rule).to_numpy()
            direct_rows["derivation"] = "direct"
            direct_rows["source_tags_json"] = [json.dumps([f"us-gaap:{tag}"]) for tag in direct["tag"]]
            parts = [direct_rows]
            if rule.inputs:
                derived = _derive(raw, rule, basis, filings)
                if not derived.empty and rule.combination_rule.startswith("coalesce_or_"):
                    have = set(zip(direct_rows["adsh"], direct_rows["ddate"], strict=True))
                    derived = derived[[key not in have for key in zip(derived["adsh"], derived["ddate"], strict=True)]]
                parts.append(derived)
            combined = pd.concat([p for p in parts if not p.empty], ignore_index=True) if any(not p.empty for p in parts) else pd.DataFrame()
            if combined.empty:
                continue
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


def _derive(raw: pd.DataFrame, rule: CanonicalRule, basis: str, filings: pd.DataFrame) -> pd.DataFrame:
    """Composition over raw component items reported in the same filing and period."""

    keyed: pd.DataFrame | None = None
    for position, input_id in enumerate(rule.inputs):
        part = raw[(raw["item_id"] == input_id) & (raw["basis"] == basis)][["adsh", "ddate", "value", "tag"]]
        part = part.rename(columns={"value": f"v{position}", "tag": f"t{position}"})
        keyed = part if keyed is None else keyed.merge(part, on=["adsh", "ddate"], how="outer")
    if keyed is None or keyed.empty:
        return pd.DataFrame()
    value_cols = [f"v{i}" for i in range(len(rule.inputs))]
    present = keyed[value_cols].notna()
    zero_fill = rule.missing_policy == "zero_fill"
    if rule.combination_rule in _DIFFERENCE_RULES:
        # The minuend must be reported; the subtrahend may be zero-filled only under zero_fill.
        keep = present["v0"] & (present["v1"] | zero_fill)
        keyed = keyed[keep].copy()
        value = keyed["v0"].astype(float) - keyed["v1"].fillna(0.0).astype(float)
    else:
        keep = present.all(axis=1) | (zero_fill & present.any(axis=1))
        keyed = keyed[keep].copy()
        value = keyed[value_cols].fillna(0.0).astype(float).sum(axis=1)
    if keyed.empty:
        return pd.DataFrame()
    keyed["value"] = _apply_rule_value(value, rule).to_numpy()
    keyed["derivation"] = "derived"
    tag_cols = [f"t{i}" for i in range(len(rule.inputs))]
    keyed["source_tags_json"] = [
        json.dumps([f"us-gaap:{tag}" for tag in row if isinstance(tag, str)]) for row in keyed[tag_cols].itertuples(index=False)
    ]
    merged = keyed.merge(filings, on="adsh", how="left")
    return merged[[*_FILING_COLUMNS, "ddate", "value", "derivation", "source_tags_json"]]


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
