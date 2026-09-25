#!/usr/bin/env python
"""RI1: measure reconstructed price-line -> CIK links on the retained files.

Read-only over the retained sources -- ``TickerHistory3.parquet``,
``companyfacts.zip``, ``submissions.zip`` and ``company_tickers.json``; never
the warehouse, never the network. Every intermediate lives in ``--work`` (a
scratch directory outside ``data/``): the staging DuckDB runs with
``memory_limit`` <= 512MB and 2 threads, and nothing per bar reaches Python.

Stages (``--stages``, default all, in order; each reads the earlier stages' tables)::

    stage    vendor lines/symbols/share runs, line-month presence and the earnings-flag
             (operating company) proxy from TickerHistory3; Company Facts share counts
    lines    loader-emulated price-line ids and the A5 current-ticker bridge per line
    filings  lifecycle + periodic filings and names of every candidate and current CIK
    run      reconstruct_in_batches (links.parquet, evidence_rows.parquet), the blind
             holdout (currently linked lines, no ticker snapshot) and decoy null runs
    measure  population, tiers, conflicts, precision probes, holdout, attrition
             estimate and the 0327 schema audit -> measure.json

Precision probes use signals the reconstruction never reads: (a) the line's
symbol abbreviates one of the issuer's SEC names (first letter + in-order
letters), (b) the issuer's periodic filings stop near the line's last trade.
Each is compared with a permutation null (the same lines paired with other
linked issuers); ``precision = (m - r) / (t - r)`` with ``t`` the rate on
reference true pairs, and ``(m - r) / (1 - r)`` as a lower bound.

Example::

    python scripts/identity_reconstruction_measure.py --work <scratch>/RI1-measure --decoy 1.0137 --decoy 0.9871
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import math
import os
import random
import re
import sys
import time
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
from collections.abc import Iterator, Mapping, Sequence
from pathlib import Path

# numpy's OpenBLAS commits per-thread buffers at import: ~515 MB of private bytes on this
# 16-thread host (measured) before any work. One BLAS thread keeps the process baseline ~95 MB.
os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db import historical_identity as hi
from atx_db import identity_reconstruction as ir
from atx_db._submissions_archive import SubmissionsArchive
from atx_db.market_owner_bridge import PriceLine, classify_reconstructed, normalize_symbol
from atx_db.migrations.bodies_0327 import create_historical_identity_tables
from atx_db.warehouse import cik_security_id

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "data"
TICKER_HISTORY = DATA / "staging" / "broad-bars" / "2026-09-20-updated" / "TickerHistory3.parquet"
COMPANYFACTS = DATA / "cache" / "companyfacts.zip"
SUBMISSIONS = DATA / "cache" / "submissions.zip"
COMPANY_TICKERS = DATA / "cache" / "company_tickers.json"

STAGES = ("stage", "facts", "lines", "filings", "run", "measure")
STAGE_DB = "ri1_stage.duckdb"
RUN_ID = "RI1-measure"
#: Shared-host light-work caps: DuckDB memory_limit, and the process commit (private bytes) ceiling
#: checked at every progress line (the run stops rather than grow past it; chunk smaller instead).
MAX_MEMORY_MB = 384
MAX_PRIVATE_MB = 600
PERIODIC_FORMS = ("10-K", "10-K405", "10-KSB", "10-KT", "10-Q", "10-QSB", "10-QT", "20-F", "40-F")
#: market_owner_bridge.STALE_LINK_DAYS: a line whose last bar is older than this before the horizon has ended.
ACTIVE_DAYS = 30
#: Cessation probe: the issuer's last periodic filing lies within +-this many days of the line's last trade ...
CESSATION_DAYS = 400
#: ... among issuers that were filing within this many days before the last trade (own and null alike).
ACTIVE_BEFORE_DAYS = 200
PERMUTATIONS = 5
NULL_DRAWS = 10
SAMPLE_PER_TIER = 50
SEED = 20260925
#: Delisted/acquired large caps for the human spot check (any symbol the line traded under).
SPOT_CHECK = (
    "CELG", "TWTR", "MON", "TWX", "TIF", "XLNX", "ATVI", "LNKD", "WFM", "AABA", "RHT", "SHLD", "ESRX", "AGN",
    "CA", "COL", "DOW", "DD", "DELL", "EMC", "HNZ", "BRCM", "KRFT", "LLTC", "ALTR", "PCP", "SNDK", "BHI",
)

_ARROW = {"VARCHAR": pa.string(), "DATE": pa.date32(), "TIMESTAMP": pa.timestamp("us"), "BOOLEAN": pa.bool_()}
EVIDENCE_SCHEMA = pa.schema([(name, _ARROW[kind]) for name, kind in hi.EVIDENCE_COLUMNS])
LINK_SCHEMA = pa.schema(
    [
        ("vendor_id", pa.int64()),
        ("price_security_id", pa.string()),
        ("cik", pa.string()),
        ("valid_from", pa.date32()),
        ("valid_to", pa.date32()),
        ("available_at", pa.timestamp("us")),
        ("first_evidence_at", pa.timestamp("us")),
        ("evidence_complete_at", pa.timestamp("us")),
        ("tier", pa.string()),
        ("tier_rank", pa.int32()),
        ("weight", pa.float64()),
        ("share_weight", pa.float64()),
        ("share_items", pa.int32()),
        ("terminal", pa.bool_()),
        ("listing", pa.bool_()),
        ("symbol_hit", pa.bool_()),
        ("symbol", pa.string()),
        ("current_cik", pa.string()),
        ("competitors", pa.string()),
        ("tier_attained_at", pa.timestamp("us")),
        ("tier_at_available_at", pa.string()),
        ("high_from", pa.timestamp("us")),
        ("low_from", pa.timestamp("us")),
        ("tier_history", pa.string()),
    ]
)
FILINGS_SCHEMA = pa.schema(
    [
        ("cik", pa.string()),
        ("form", pa.string()),
        ("filing_date", pa.date32()),
        ("accession", pa.string()),
        ("accepted_at", pa.timestamp("us")),
    ]
)
NAMES_SCHEMA = pa.schema([("cik", pa.string()), ("name", pa.string()), ("kind", pa.string())])
META_SCHEMA = pa.schema(
    [
        ("vendor_id", pa.int64()),
        ("price_security_id", pa.string()),
        ("cur_cik", pa.string()),
        ("cur_method", pa.string()),
        ("unlinked_reason", pa.string()),
    ]
)


# ---------------------------------------------------------------------------------------------
# Plumbing


def _memory() -> tuple[int, int, int]:
    """(peak working set, current private bytes, peak private bytes) of this process in MB.

    Windows ``GetProcessMemoryInfo``; POSIX reports max RSS for all three.
    """
    if sys.platform != "win32":
        import resource

        rss = int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024)
        return (rss, rss, rss)
    import ctypes
    from ctypes import wintypes

    class Counters(ctypes.Structure):
        _fields_ = (
            ("cb", wintypes.DWORD),
            ("PageFaultCount", wintypes.DWORD),
            ("PeakWorkingSetSize", ctypes.c_size_t),
            ("WorkingSetSize", ctypes.c_size_t),
            ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPagedPoolUsage", ctypes.c_size_t),
            ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
            ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
            ("PagefileUsage", ctypes.c_size_t),
            ("PeakPagefileUsage", ctypes.c_size_t),
        )

    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    kernel32 = ctypes.windll.kernel32
    kernel32.GetCurrentProcess.restype = ctypes.c_void_p
    getter = ctypes.windll.psapi.GetProcessMemoryInfo
    getter.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD]
    getter(kernel32.GetCurrentProcess(), ctypes.byref(counters), counters.cb)
    return (
        round(counters.PeakWorkingSetSize / 1e6),
        round(counters.PagefileUsage / 1e6),
        round(counters.PeakPagefileUsage / 1e6),
    )


def _peak_mb() -> dict[str, int]:
    peak_ws, _private, peak_private = _memory()
    return {"peak_working_set_mb": peak_ws, "peak_private_mb": peak_private}


def _log(message: str) -> None:
    peak_ws, private, peak_private = _memory()
    print(
        f"[{dt.datetime.now():%H:%M:%S}] {message} | working set peak {peak_ws} MB, "
        f"private {private} MB (peak {peak_private} MB)",
        flush=True,
    )
    if private > MAX_PRIVATE_MB:
        raise SystemExit(f"private bytes {private} MB exceed the {MAX_PRIVATE_MB} MB process cap; chunk smaller")


def _utc_mtime(path: Path) -> dt.datetime:
    return dt.datetime.fromtimestamp(path.stat().st_mtime, dt.UTC).replace(tzinfo=None)


def _file_sha256(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def _memory_mb(text: str) -> int:
    match = re.fullmatch(r"\s*(\d+)\s*(MB|MiB|GB|GiB)\s*", text, flags=re.IGNORECASE)
    if match is None:
        raise ValueError(f"memory limit {text!r} must look like 256MB")
    value = int(match.group(1))
    return value * 1024 if match.group(2).upper().startswith("G") else value


def _connect(path: Path, memory: str) -> duckdb.DuckDBPyConnection:
    if _memory_mb(memory) > MAX_MEMORY_MB:
        raise ValueError(f"memory limit {memory} exceeds the {MAX_MEMORY_MB}MB light-work cap")
    con = duckdb.connect(str(path), config={"memory_limit": memory, "threads": 2})
    spill = path.parent / "duckdb-tmp"
    spill.mkdir(exist_ok=True)
    con.execute(f"SET temp_directory='{spill.as_posix()}'")
    con.execute("SET preserve_insertion_order=false")
    con.execute("PRAGMA disable_progress_bar")
    return con


def _chunks[T](values: Sequence[T], size: int) -> Iterator[Sequence[T]]:
    for start in range(0, len(values), size):
        yield values[start : start + size]


def _insert(con: duckdb.DuckDBPyConnection, table: str, columns: Mapping[str, list[object]], schema: pa.Schema) -> None:
    con.register("ri_measure_batch", pa.table(dict(columns), schema=schema))
    try:
        con.execute(f"INSERT INTO {table} SELECT * FROM ri_measure_batch")
    finally:
        con.unregister("ri_measure_batch")


def _rows(con: duckdb.DuckDBPyConnection, sql: str, params: Sequence[object] = ()) -> list[dict[str, object]]:
    cursor = con.execute(sql, list(params))
    names = [column[0] for column in cursor.description]
    return [dict(zip(names, row, strict=True)) for row in cursor.fetchall()]


def _one(con: duckdb.DuckDBPyConnection, sql: str, params: Sequence[object] = ()) -> dict[str, object]:
    (row,) = _rows(con, sql, params)
    return row


# ---------------------------------------------------------------------------------------------
# Stages


def stage_sources(work: Path, memory: str) -> None:
    """Stage the vendor tables and every Company Facts share count into the scratch DuckDB."""
    path = work / STAGE_DB
    for stale in (path, path.with_name(path.name + ".wal")):
        stale.unlink(missing_ok=True)
    con = _connect(path, memory)
    source = str(TICKER_HISTORY)
    started = time.perf_counter()
    ir.stage_vendor_ticker_history(con, TICKER_HISTORY)
    _log(f"vendor lines/symbols/share runs staged in {time.perf_counter() - started:.0f}s")
    con.execute(
        """
        CREATE TABLE ri_line_months AS
        SELECT securityID AS vendor_id, CAST(date_trunc('month', tradingDate) AS DATE) AS month,
               max(tradingDate) AS formation_bar
        FROM read_parquet(?)
        WHERE securityID > 0 AND close > 0 AND tradingDate IS NOT NULL
        GROUP BY 1, 2
        """,
        [source],
    )
    con.execute(
        "CREATE TABLE ri_line_earn AS SELECT DISTINCT securityID AS vendor_id FROM read_parquet(?) "
        "WHERE securityID > 0 AND earnFlag = '0'",
        [source],
    )
    con.execute(
        """
        CREATE TABLE ri_symbol_keyed AS
        SELECT count(DISTINCT upper(trim(coalesce(nullif(ticker_tk, ''), nullif(todayTicker, ''))))) AS symbols,
               count(*) AS bars
        FROM read_parquet(?) WHERE coalesce(securityID, 0) <= 0 AND close > 0
        """,
        [source],
    )
    _log("line-month presence and earnings-flag proxy staged")
    recorded = TICKER_HISTORY.with_name(TICKER_HISTORY.name + ".sha256").read_text(encoding="utf-8").split()[0]
    meta = {
        "ticker_history_sha256": recorded.lower(),
        "company_tickers_sha256": _file_sha256(COMPANY_TICKERS),
        "company_tickers_observed_at": _utc_mtime(COMPANY_TICKERS).isoformat(),
    }
    con.execute("CREATE TABLE ri_meta (key VARCHAR, value VARCHAR)")
    con.executemany("INSERT INTO ri_meta VALUES (?, ?)", sorted(meta.items()))
    con.close()


def stage_facts(work: Path, memory: str) -> None:
    """Stream every Company Facts share count into the scratch DuckDB (one archive member at a time)."""
    con = _connect(work / STAGE_DB, memory)
    con.execute("DROP TABLE IF EXISTS ri_share_facts")
    started = time.perf_counter()
    facts = ir.stage_share_facts(con, ir.iter_companyfacts_share_facts(COMPANYFACTS), batch=20_000)
    _log(f"{facts} Company Facts share counts staged in {time.perf_counter() - started:.0f}s")
    meta = {
        "companyfacts_sha256": _file_sha256(COMPANYFACTS),
        "companyfacts_observed_at": _utc_mtime(COMPANYFACTS).isoformat(),
        "share_facts": str(facts),
    }
    con.execute("DELETE FROM ri_meta WHERE key IN (SELECT unnest(?::VARCHAR[]))", [list(meta)])
    con.executemany("INSERT INTO ri_meta VALUES (?, ?)", sorted(meta.items()))
    con.close()


def stage_lines(work: Path, memory: str) -> None:
    """Loader-emulated price-line ids plus the A5 current-ticker bridge outcome per vendor line.

    Emulates ``ticker_history._apply_security_ids`` / ``disambiguate_vendor_collisions``: a line
    whose current symbol maps to a CIK in the SEC snapshot takes ``SEC-CIK-*`` (the most-bars
    line keeps a shared id, the others are re-keyed), every other line ``TBLTICKERHISTORY-<id>``.
    """
    con = _connect(work / STAGE_DB, memory)
    observed = _utc_mtime(COMPANY_TICKERS)
    tickers: list[tuple[str, str, dt.datetime | None]] = []
    sec_map: dict[str, str] = {}
    for value in json.loads(COMPANY_TICKERS.read_text(encoding="utf-8")).values():
        cik = hi.normalize_cik(value["cik_str"])
        ticker = str(value["ticker"]).strip().upper()
        tickers.append((cik, ticker, observed))
        security_id = cik_security_id(cik)
        if ticker not in sec_map or security_id < sec_map[ticker]:
            sec_map[ticker] = security_id
    rows = con.execute(
        "SELECT vendor_id, last_symbol, today_ticker, first_trade_date, last_trade_date, bar_rows FROM ri_vendor_lines"
    ).fetchall()
    grouped: dict[str, list[tuple[int, str, int]]] = defaultdict(list)
    for vendor_id, _symbol, today, _first, _last, bars in rows:
        base = sec_map.get(str(today or ""), f"TBLTICKERHISTORY-{vendor_id}")
        grouped[base].append((-int(bars), str(vendor_id), int(vendor_id)))
    line_ids: dict[int, str] = {}
    for base, members in grouped.items():
        for rank, (_bars, _text, vendor_id) in enumerate(sorted(members)):
            line_ids[vendor_id] = base if rank == 0 else f"TBLTICKERHISTORY-{vendor_id}"
    lines = [
        PriceLine(line_ids[int(vendor_id)], symbol, first, last, int(bars))
        for vendor_id, symbol, _today, first, last, bars in rows
    ]
    bridge_rows, _members, _ambiguous = classify_reconstructed(lines, tickers, {})
    by_line = {}
    for row in bridge_rows:
        by_line.setdefault(row.price_security_id, row)
    columns: dict[str, list[object]] = defaultdict(list)
    for vendor_id, *_rest in rows:
        row = by_line[line_ids[int(vendor_id)]]
        columns["vendor_id"].append(int(vendor_id))
        columns["price_security_id"].append(line_ids[int(vendor_id)])
        columns["cur_cik"].append(row.cik if row.owner_security_id else None)
        columns["cur_method"].append(row.link_method)
        columns["unlinked_reason"].append(row.unlinked_reason)
    con.execute("DROP TABLE IF EXISTS ri_line_meta")
    con.execute(
        "CREATE TABLE ri_line_meta (vendor_id BIGINT, price_security_id VARCHAR, cur_cik VARCHAR, "
        "cur_method VARCHAR, unlinked_reason VARCHAR)"
    )
    _insert(con, "ri_line_meta", columns, META_SCHEMA)
    linked = sum(1 for cik in columns["cur_cik"] if cik)
    _log(f"{len(rows)} vendor lines; current-ticker bridge links {linked}")
    con.close()


def stage_filings(work: Path, memory: str, params: ir.ReconstructionParams) -> None:
    """Lifecycle + periodic filings and SEC names of every candidate and currently linked CIK."""
    con = _connect(work / STAGE_DB, memory)
    vendor_ids = [int(vendor_id) for (vendor_id,) in con.execute("SELECT vendor_id FROM ri_vendor_lines ORDER BY 1")
                  .fetchall()]
    ciks: set[str] = set()
    for chunk in _chunks(vendor_ids, 2000):
        ciks.update(str(row[1]) for row in ir.match_share_counts(con, params, vendor_ids=chunk))
    candidates = len(ciks)
    ciks.update(
        str(cik) for (cik,) in con.execute("SELECT DISTINCT cur_cik FROM ri_line_meta WHERE cur_cik IS NOT NULL")
        .fetchall()
    )
    _log(f"{candidates} candidate CIKs, {len(ciks)} with the currently linked ones")
    ordered = sorted(ciks)
    con.execute("DROP TABLE IF EXISTS ri_filings")
    con.execute(
        "CREATE TABLE ri_filings (cik VARCHAR, form VARCHAR, filing_date DATE, accession VARCHAR, accepted_at TIMESTAMP)"
    )
    for chunk in _chunks(ordered, 2500):
        filings = ir.read_lifecycle_filings(SUBMISSIONS, chunk, forms=(*ir.LIFECYCLE_FORMS, *PERIODIC_FORMS))
        _insert(
            con,
            "ri_filings",
            {
                "cik": [filing.cik for filing in filings],
                "form": [filing.form for filing in filings],
                "filing_date": [filing.filing_date for filing in filings],
                "accession": [filing.accession for filing in filings],
                "accepted_at": [filing.accepted_at for filing in filings],
            },
            FILINGS_SCHEMA,
        )
        _log(f"filings read for {chunk[-1]}")
    names: dict[str, list[object]] = defaultdict(list)
    with SubmissionsArchive(SUBMISSIONS) as archive:
        digest = archive.sha256
        for cik in ordered:
            member = f"CIK{cik}.json"
            if member not in archive:
                continue
            data = json.loads(archive.read(member))
            entries = [(data.get("name"), "current")] + [
                (former.get("name"), "former") for former in data.get("formerNames") or ()
            ]
            for name, kind in entries:
                if name:
                    names["cik"].append(cik)
                    names["name"].append(str(name))
                    names["kind"].append(kind)
    con.execute("DROP TABLE IF EXISTS ri_names")
    con.execute("CREATE TABLE ri_names (cik VARCHAR, name VARCHAR, kind VARCHAR)")
    _insert(con, "ri_names", names, NAMES_SCHEMA)
    con.execute("DELETE FROM ri_meta WHERE key IN ('submissions_sha256', 'candidate_ciks')")
    con.executemany(
        "INSERT INTO ri_meta VALUES (?, ?)", [("submissions_sha256", digest), ("candidate_ciks", str(candidates))]
    )
    _log(f"{len(names['cik'])} issuer names read")
    con.close()


def _accumulate(totals: Counter[str], result: ir.ReconstructionResult) -> None:
    for key, value in result.summary().items():
        if isinstance(value, bool | str):
            continue
        if isinstance(value, int):
            totals[key] += value
        elif isinstance(value, dict):
            for sub, count in value.items():
                totals[f"{key}.{sub}"] += int(count)


def _append_link(columns: dict[str, list[object]], link: ir.ReconstructedLink) -> None:
    kinds = {item.kind for item in link.items}
    values: dict[str, object] = {
        "vendor_id": link.vendor_id,
        "price_security_id": link.price_security_id,
        "cik": link.cik,
        "valid_from": link.valid_from,
        "valid_to": link.valid_to,
        "available_at": link.available_at,
        "first_evidence_at": min(item.known_at for item in link.items if item.kind == ir.EV_SHARES),
        "evidence_complete_at": link.evidence_complete_at,
        "tier": link.tier,
        "tier_rank": ir.TIERS.index(link.tier),
        "weight": link.weight,
        "share_weight": link.share_weight,
        "share_items": sum(1 for item in link.items if item.kind == ir.EV_SHARES),
        "terminal": ir.EV_TERMINAL in kinds,
        "listing": ir.EV_LISTING in kinds,
        "symbol_hit": ir.EV_SYMBOL in kinds,
        "symbol": link.symbol,
        "current_cik": link.current_cik,
        "competitors": json.dumps(link.competitors),
        "tier_attained_at": link.tier_attained_at,
        "tier_at_available_at": link.tier_history[0][0],
        "high_from": next((since for tier, since in link.tier_history if tier == ir.TIER_HIGH), None),
        "low_from": next((since for tier, since in link.tier_history if tier == ir.TIER_LOW), None),
        "tier_history": json.dumps([[tier, since.isoformat()] for tier, since in link.tier_history]),
    }
    for key, value in values.items():
        columns[key].append(value)


def run_reconstruction(
    work: Path, memory: str, params: ir.ReconstructionParams, batch_size: int, decoys: Sequence[float]
) -> None:
    """The measured reconstruction (evidence rows + links), the blind holdout and the decoy null runs."""
    con = _connect(work / STAGE_DB, memory)
    meta = dict(con.execute("SELECT key, value FROM ri_meta").fetchall())
    line_meta = con.execute("SELECT vendor_id, price_security_id, cur_cik FROM ri_line_meta").fetchall()
    lines = ir.read_vendor_lines(
        con,
        {int(vendor_id): str(line_id) for vendor_id, line_id, _cik in line_meta},
        {int(vendor_id): str(cik) for vendor_id, _line_id, cik in line_meta if cik},
    )
    filings = [
        ir.IssuerFiling(str(cik), str(form), filing_date, accession, accepted_at)
        for cik, form, filing_date, accession, accepted_at in con.execute(
            "SELECT cik, form, filing_date, accession, accepted_at FROM ri_filings "
            "WHERE form IN (SELECT unnest(?::VARCHAR[]))",
            [list(ir.LIFECYCLE_FORMS)],
        ).fetchall()
    ]
    tickers = ir.read_sec_ticker_snapshot(COMPANY_TICKERS)
    ticker_at = dt.datetime.fromisoformat(meta["company_tickers_observed_at"])
    observed = dt.datetime.fromisoformat(meta["companyfacts_observed_at"])
    artifacts = {key: value for key, value in meta.items() if key.endswith("_sha256")}
    horizon = max(line.last_trade_date for line in lines)
    price_start = min(line.first_trade_date for line in lines)
    summary: dict[str, object] = {
        "lines": len(lines),
        "lifecycle_filings": len(filings),
        "snapshot_pairs": len(tickers),
        "horizon": horizon.isoformat(),
        "price_start": price_start.isoformat(),
        "params": params.digest(),
    }
    _log(f"{len(lines)} lines, {len(filings)} lifecycle filings; horizon {horizon}")

    started = time.perf_counter()
    totals: Counter[str] = Counter()
    links: dict[str, list[object]] = defaultdict(list)
    writer = pq.ParquetWriter(work / "evidence_rows.parquet", EVIDENCE_SCHEMA)
    try:
        for index, result in enumerate(
            ir.reconstruct_in_batches(
                con, lines, filings=filings, tickers=tickers, ticker_observed_at=ticker_at, params=params,
                batch_size=batch_size,
            )
        ):
            _accumulate(totals, result)
            rows = result.evidence_rows(
                observed_at=observed, run_id=RUN_ID, artifact_sha256=meta["companyfacts_sha256"], artifacts=artifacts
            )
            if rows:
                writer.write_table(pa.Table.from_pylist(rows, schema=EVIDENCE_SCHEMA))
            for link in result.links:
                _append_link(links, link)
            if index % 10 == 0:
                _log(f"batch {index}: {totals['link_segments']} links so far")
    finally:
        writer.close()
    pq.write_table(pa.table(dict(links), schema=LINK_SCHEMA), work / "links.parquet")
    summary["main"] = {**totals, "seconds": round(time.perf_counter() - started)}
    _log(f"main run done: {dict(totals)}")

    started = time.perf_counter()
    holdout = [line for line in lines if line.current_cik]
    blind_totals: Counter[str] = Counter()
    blind: dict[str, list[object]] = defaultdict(list)
    for chunk in _chunks(holdout, batch_size):
        result = ir.reconstruct_issuer_links(
            con, chunk, filings=filings, tickers=(), params=params, horizon=horizon, price_start=price_start
        )
        _accumulate(blind_totals, result)
        for link in result.links:
            _append_link(blind, link)
    pq.write_table(pa.table(dict(blind), schema=LINK_SCHEMA), work / "links_blind.parquet")
    summary["blind_holdout"] = {**blind_totals, "seconds": round(time.perf_counter() - started)}
    _log(f"blind holdout done: {dict(blind_totals)}")

    catalog = con.execute("SELECT current_database()").fetchone()[0]  # type: ignore[index]
    for factor in decoys:
        started = time.perf_counter()
        # Null model: every issuer count scaled -- the same value distribution, no true matches.
        con.execute(
            "CREATE OR REPLACE TEMP TABLE ri_share_facts AS SELECT cik, concept, as_of, round(value * ?) AS value, "
            f"accession, form, filed FROM {catalog}.main.ri_share_facts",
            [factor],
        )
        decoy_totals: Counter[str] = Counter()
        for result in ir.reconstruct_in_batches(
            con, lines, filings=filings, tickers=tickers, ticker_observed_at=ticker_at, params=params,
            batch_size=batch_size,
        ):
            _accumulate(decoy_totals, result)
        con.execute("DROP TABLE temp.main.ri_share_facts")
        summary[f"decoy_{factor}"] = {**decoy_totals, "seconds": round(time.perf_counter() - started)}
        _log(f"decoy x{factor} done: {dict(decoy_totals)}")
    summary["memory"] = _peak_mb()
    (work / "run.json").write_text(json.dumps(summary, indent=1, default=str), encoding="utf-8")
    con.close()


# ---------------------------------------------------------------------------------------------
# Measurement


_STATE_TAG = re.compile(r"/[A-Z]{2,3}/?")


def _name_letters(name: str) -> str:
    """Issuer name letters for the abbreviation probe (state tags and a leading THE dropped)."""
    text = _STATE_TAG.sub(" ", name.upper())
    text = re.sub(r"^\s*THE\s+", "", text)
    return re.sub(r"[^A-Z]", "", text)


def _ticker_root(symbol: str) -> str:
    key = normalize_symbol(symbol) or ""
    return re.sub(r"[^A-Z]", "", key.split("-", 1)[0])


def _abbreviates(root: str, letters: str) -> bool:
    """The ticker root starts like the name and its letters appear in the name in order."""
    if not root or not letters or root[0] != letters[0]:
        return False
    remaining = iter(letters)
    return all(char in remaining for char in root)


def _consistent(roots: Sequence[str], names: Sequence[str]) -> bool:
    return any(_abbreviates(root, letters) for root in roots for letters in names)


def _wilson(hits: int, n: int, z: float = 1.96) -> tuple[float, float]:
    if n == 0:
        return (0.0, 1.0)
    p = hits / n
    centre = (p + z * z / (2 * n)) / (1 + z * z / n)
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / (1 + z * z / n)
    return (max(0.0, centre - half), min(1.0, centre + half))


def _precision(hits: int, n: int, null: float, truth: float) -> dict[str, object]:
    """Mixture estimate: m = p t + (1 - p) r  =>  p = (m - r) / (t - r)."""

    def clip(value: float) -> float:
        return round(max(0.0, min(1.0, value)), 4)

    m = hits / n if n else 0.0
    low, high = _wilson(hits, n)
    identified = n > 0 and truth > null and not math.isnan(truth) and not math.isnan(null)
    scale = truth - null
    estimate = clip((m - null) / scale) if identified else None
    return {
        "n": n,
        "consistent": hits,
        "rate": round(m, 4),
        "null_rate": round(null, 4),
        "true_rate": round(truth, 4),
        "precision_est": estimate,
        "precision_ci95": [clip((low - null) / scale), clip((high - null) / scale)] if identified else None,
        "precision_lower_bound": clip((m - null) / (1 - null)) if n and null < 1 else None,
        "error_rate_est": None if estimate is None else round(1 - estimate, 4),
    }


def _segment_roots(
    symbols: Mapping[int, list[tuple[str, dt.date, dt.date]]], vendor_id: int, start: dt.date, end: dt.date
) -> list[str]:
    roots = {_ticker_root(symbol) for symbol, first, last in symbols.get(vendor_id, ()) if first < end and last >= start}
    return sorted(root for root in roots if root)


def _last_periodic(dates: Sequence[dt.date]) -> dt.date | None:
    return dates[-1] if dates else None


def _active_before(dates: Sequence[dt.date], day: dt.date) -> bool:
    low = bisect_left(dates, day - dt.timedelta(days=ACTIVE_BEFORE_DAYS))
    return bisect_right(dates, day) > low


def _ceased_near(dates: Sequence[dt.date], day: dt.date) -> bool:
    last = _last_periodic(dates)
    return last is not None and abs((last - day).days) <= CESSATION_DAYS


def measure(work: Path, memory: str) -> dict[str, object]:
    con = _connect(work / STAGE_DB, memory)
    run = json.loads((work / "run.json").read_text(encoding="utf-8"))
    horizon = dt.date.fromisoformat(run["horizon"])
    for name in ("links", "links_blind", "evidence_rows"):
        con.execute(
            f"CREATE OR REPLACE TEMP VIEW {name} AS SELECT * FROM read_parquet('{(work / f'{name}.parquet').as_posix()}')"
        )
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE line_facts AS
        SELECT l.vendor_id, l.first_trade_date, l.last_trade_date, l.bar_rows, l.last_symbol, m.price_security_id,
               m.cur_cik, m.cur_method, m.unlinked_reason, l.last_trade_date < ? AS ended,
               e.vendor_id IS NOT NULL AS earn
        FROM ri_vendor_lines l JOIN ri_line_meta m USING (vendor_id) LEFT JOIN ri_line_earn e USING (vendor_id)
        """,
        [horizon - dt.timedelta(days=ACTIVE_DAYS)],
    )
    out: dict[str, object] = {"horizon": run["horizon"], "run": run}

    # Population ---------------------------------------------------------------------------
    out["population"] = _one(
        con,
        """
        SELECT count(*) AS lines,
               count(*) FILTER (WHERE cur_cik IS NULL) AS unlinked_now,
               count(*) FILTER (WHERE ended) AS ended,
               count(*) FILTER (WHERE ended AND cur_cik IS NULL) AS ended_unlinked_now,
               count(*) FILTER (WHERE earn) AS earnings_flag,
               count(*) FILTER (WHERE ended AND earn) AS ended_earnings_flag,
               count(*) FILTER (WHERE ended AND cur_cik IS NULL AND earn) AS ended_unlinked_now_earnings_flag
        FROM line_facts
        """,
    )
    out["current_bridge_outcome"] = dict(
        con.execute(
            "SELECT coalesce(unlinked_reason, 'linked:' || cur_method), count(*) FROM line_facts GROUP BY 1 ORDER BY 2 DESC"
        ).fetchall()
    )
    out["symbol_keyed_out_of_scope"] = _one(con, "SELECT * FROM ri_symbol_keyed")

    # Linkage by tier on the hook population (lines the current-ticker bridge leaves unlinked)
    scopes = _rows(
        con,
        """
        WITH best AS (SELECT vendor_id, min(tier_rank) AS rank FROM links GROUP BY 1),
        scoped AS (
            SELECT 'unlinked_now' AS scope, f.vendor_id, b.rank FROM line_facts f LEFT JOIN best b USING (vendor_id)
            WHERE f.cur_cik IS NULL
            UNION ALL
            SELECT 'delisted_unlinked_now', f.vendor_id, b.rank FROM line_facts f LEFT JOIN best b USING (vendor_id)
            WHERE f.cur_cik IS NULL AND f.ended
            UNION ALL
            SELECT 'delisted_unlinked_now_earnings_flag', f.vendor_id, b.rank
            FROM line_facts f LEFT JOIN best b USING (vendor_id) WHERE f.cur_cik IS NULL AND f.ended AND f.earn
        )
        SELECT scope, count(*) AS lines, count(rank) AS linked, count(*) FILTER (WHERE rank = 0) AS high,
               count(*) FILTER (WHERE rank = 1) AS medium, count(*) FILTER (WHERE rank = 2) AS low
        FROM scoped GROUP BY scope ORDER BY scope
        """,
    )
    for row in scopes:
        lines = int(row["lines"])  # type: ignore[call-overload]
        for key in ("linked", *ir.TIERS):
            row[f"{key}_share"] = round(int(row[key]) / lines, 4) if lines else None  # type: ignore[call-overload]
    out["linkage_by_tier"] = scopes
    out["segments"] = _one(
        con,
        """
        SELECT count(*) AS segments, count(DISTINCT vendor_id) AS lines,
               count(*) FILTER (WHERE current_cik IS NULL) AS segments_unlinked_now,
               count(*) - count(DISTINCT vendor_id) AS extra_segments,
               quantile_cont(date_diff('day', valid_from, CAST(available_at AS DATE)), [0.1, 0.5, 0.9])
                   FILTER (WHERE current_cik IS NULL) AS availability_lag_days_q10_50_90
        FROM links
        """,
    )
    out["evidence_rows"] = _rows(
        con,
        """
        SELECT evidence_status, availability_status, coalesce(rejection_reason, '') AS rejection_reason,
               count(*) AS rows, count(DISTINCT native_key) AS lines
        FROM evidence_rows GROUP BY ALL ORDER BY 1, 3
        """,
    )
    out["conflicts"] = {
        "unresolved_conflict_lines": run["main"].get("unresolved_conflict_lines", 0),
        "dominated_rivals": run["main"].get("dominated_rivals", 0),
        "low_tier_links": run["main"].get("links_by_tier.low", 0),
    }
    out["current_holder_reuse_guard"] = _one(
        con,
        """
        SELECT count(DISTINCT k.vendor_id) FILTER (WHERE f.ended) AS stale_current_holders_with_link,
               count(DISTINCT k.vendor_id) FILTER (WHERE f.ended AND k.cik <> f.cur_cik) AS stale_other_cik,
               count(DISTINCT k.vendor_id) FILTER (WHERE NOT f.ended AND k.cik <> f.cur_cik) AS active_other_cik
        FROM links k JOIN line_facts f USING (vendor_id) WHERE f.cur_cik IS NOT NULL
        """,
    )

    # Blind holdout: currently linked lines reconstructed without the ticker snapshot -----------
    out["blind_holdout"] = {
        "lines": _one(
            con,
            """
            WITH b AS (SELECT vendor_id, bool_and(cik = current_cik) AS agree FROM links_blind GROUP BY 1)
            SELECT count(*) AS current_linked_lines, count(b.vendor_id) AS reconstructed,
                   count(*) FILTER (WHERE b.agree) AS agree, count(*) FILTER (WHERE NOT b.agree) AS disagree,
                   count(*) FILTER (WHERE NOT b.agree AND f.ended) AS disagree_stale_current_link,
                   count(b.vendor_id) FILTER (WHERE f.ended) AS reconstructed_stale,
                   count(*) FILTER (WHERE f.ended) AS stale
            FROM line_facts f LEFT JOIN b USING (vendor_id) WHERE f.cur_cik IS NOT NULL
            """,
        ),
        "segments_by_tier": _rows(
            con,
            """
            SELECT k.tier, count(*) AS segments, count(*) FILTER (WHERE k.cik = k.current_cik) AS agree,
                   count(*) FILTER (WHERE k.cik <> k.current_cik AND NOT f.ended) AS disagree_active_line,
                   count(*) FILTER (WHERE k.cik <> k.current_cik AND f.ended) AS disagree_stale_line
            FROM links_blind k JOIN line_facts f USING (vendor_id) GROUP BY 1 ORDER BY min(k.tier_rank)
            """,
        ),
    }

    # Precision probes ------------------------------------------------------------------------
    symbols: dict[int, list[tuple[str, dt.date, dt.date]]] = defaultdict(list)
    for vendor_id, symbol, first, last in con.execute(
        "SELECT vendor_id, symbol, first_date, last_date FROM ri_vendor_symbols"
    ).fetchall():
        symbols[int(vendor_id)].append((str(symbol), first, last))
    names: dict[str, list[str]] = defaultdict(list)
    raw_names: dict[str, list[str]] = defaultdict(list)
    for cik, name in con.execute("SELECT cik, name FROM ri_names ORDER BY cik, kind, name").fetchall():
        raw_names[str(cik)].append(str(name))
        letters = _name_letters(str(name))
        if letters and letters not in names[str(cik)]:
            names[str(cik)].append(letters)
    periodic: dict[str, list[dt.date]] = defaultdict(list)
    for cik, filing_date in con.execute(
        "SELECT cik, filing_date FROM ri_filings WHERE form IN (SELECT unnest(?::VARCHAR[])) ORDER BY cik, filing_date",
        [list(PERIODIC_FORMS)],
    ).fetchall():
        periodic[str(cik)].append(filing_date)

    reference = _rows(
        con,
        "SELECT vendor_id, cur_cik FROM line_facts WHERE cur_method = 'current_sec_ticker' AND NOT ended",
    )
    true_hits = sum(
        _consistent(
            sorted({_ticker_root(symbol) for symbol, _first, _last in symbols.get(int(row["vendor_id"]), ())} - {""}),  # type: ignore[call-overload]
            names.get(str(row["cur_cik"]), []),
        )
        for row in reference
    )
    blind_agree = _rows(
        con, "SELECT vendor_id, cik, valid_from, valid_to FROM links_blind WHERE cik = current_cik"
    )
    blind_hits = sum(
        _consistent(
            _segment_roots(symbols, int(row["vendor_id"]), row["valid_from"], row["valid_to"]),  # type: ignore[arg-type,call-overload]
            names.get(str(row["cik"]), []),
        )
        for row in blind_agree
    )
    true_rate = true_hits / len(reference) if reference else float("nan")
    blind_rate = blind_hits / len(blind_agree) if blind_agree else float("nan")

    hook = _rows(
        con,
        """
        SELECT k.vendor_id, k.cik, k.valid_from, k.valid_to, k.available_at, k.tier, k.terminal, k.weight,
               f.last_trade_date, f.ended, f.earn
        FROM links k JOIN line_facts f USING (vendor_id) WHERE f.cur_cik IS NULL ORDER BY k.vendor_id, k.valid_from
        """,
    )
    for row in hook:
        row["roots"] = _segment_roots(symbols, int(row["vendor_id"]), row["valid_from"], row["valid_to"])  # type: ignore[arg-type,call-overload]
        row["name_consistent"] = _consistent(row["roots"], names.get(str(row["cik"]), []))  # type: ignore[arg-type]
    pool = sorted({str(row["cik"]) for row in hook})
    rng = random.Random(SEED)
    name_probe: dict[str, object] = {
        "true_rate_reference": {"pairs": len(reference), "rate": round(true_rate, 4),
                                "basis": "active current_sec_ticker lines x current CIK"},
        "true_rate_blind_agree": {"pairs": len(blind_agree), "rate": round(blind_rate, 4),
                                  "basis": "blind-holdout links agreeing with the current CIK"},
    }
    for tier in (*ir.TIERS, "all"):
        members = [row for row in hook if tier == "all" or row["tier"] == tier]
        if not members:
            continue
        null_hits = null_total = 0
        for _round in range(PERMUTATIONS):
            for row in members:
                other = rng.choice(pool)
                while other == row["cik"] and len(pool) > 1:
                    other = rng.choice(pool)
                null_hits += _consistent(row["roots"], names.get(other, []))  # type: ignore[arg-type]
                null_total += 1
        hits = sum(1 for row in members if row["name_consistent"])
        name_probe[tier] = _precision(hits, len(members), null_hits / null_total, true_rate) | {
            "precision_est_blind_truth": _precision(hits, len(members), null_hits / null_total, blind_rate)[
                "precision_est"
            ]
        }
    out["precision_name_probe"] = name_probe

    # Cessation probe: ended lines whose segment reaches the last trade, no terminal-filing item.
    active_pool = [cik for cik in pool if periodic.get(cik)]
    cessation: dict[str, object] = {}
    for tier in (*ir.TIERS, "all"):
        eligible = [
            row
            for row in hook
            if (tier == "all" or row["tier"] == tier)
            and row["ended"]
            and not row["terminal"]
            and row["valid_to"] == row["last_trade_date"] + dt.timedelta(days=1)  # type: ignore[operator]
        ]
        own = [row for row in eligible if _active_before(periodic.get(str(row["cik"]), []), row["last_trade_date"])]  # type: ignore[arg-type]
        hits = sum(_ceased_near(periodic[str(row["cik"])], row["last_trade_date"]) for row in own)  # type: ignore[arg-type]
        null_hits = null_total = 0
        for row in own:
            accepted = 0
            for _draw in range(NULL_DRAWS * 40):
                other = rng.choice(active_pool)
                dates = periodic[other]
                if other == row["cik"] or not _active_before(dates, row["last_trade_date"]):  # type: ignore[arg-type]
                    continue
                null_hits += _ceased_near(dates, row["last_trade_date"])  # type: ignore[arg-type]
                null_total += 1
                accepted += 1
                if accepted >= NULL_DRAWS:
                    break
        if not own:
            continue
        null_rate = null_hits / null_total if null_total else float("nan")
        cessation[tier] = {
            "eligible_links": len(eligible),
            "issuer_active_before_last_trade": len(own),
            "ceased_near_last_trade": hits,
            "rate": round(hits / len(own), 4),
            "null_rate": round(null_rate, 4),
            "null_pairs": null_total,
            "precision_lower_bound": _precision(hits, len(own), null_rate, 1.0)["precision_lower_bound"],
        }
    out["precision_cessation_probe"] = cessation

    # Hand-check sample (50 per tier) and the spot check of well-known delisted names.
    sample_rows = []
    for tier in ir.TIERS:
        members = [row for row in hook if row["tier"] == tier]
        for row in rng.sample(members, min(SAMPLE_PER_TIER, len(members))):
            dates = periodic.get(str(row["cik"]), [])
            sample_rows.append(
                {
                    "tier": tier,
                    "vendor_id": row["vendor_id"],
                    "symbols": " ".join(row["roots"]),  # type: ignore[arg-type]
                    "cik": row["cik"],
                    "names": " | ".join(raw_names.get(str(row["cik"]), [])),
                    "valid_from": row["valid_from"],
                    "valid_to": row["valid_to"],
                    "available_at": row["available_at"],
                    "last_trade": row["last_trade_date"],
                    "last_periodic_filing": _last_periodic(dates),
                    "name_consistent": row["name_consistent"],
                }
            )
    with (work / "precision_sample.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(sample_rows[0]) if sample_rows else ["tier"])
        writer.writeheader()
        writer.writerows(sample_rows)
    out["precision_sample"] = {"path": str(work / "precision_sample.csv"), "rows": len(sample_rows)}
    spot = _rows(
        con,
        """
        WITH hit AS (SELECT DISTINCT vendor_id FROM ri_vendor_symbols WHERE symbol IN (SELECT unnest(?::VARCHAR[])))
        SELECT f.vendor_id, f.last_symbol, f.first_trade_date, f.last_trade_date, f.cur_cik, k.cik, k.tier,
               k.valid_from, k.valid_to, k.available_at,
               (SELECT string_agg(DISTINCT e.rejection_reason, ',') FROM evidence_rows e
                WHERE e.native_key = CAST(f.vendor_id AS VARCHAR) AND e.rejection_reason IS NOT NULL) AS rejected
        FROM hit JOIN line_facts f USING (vendor_id) LEFT JOIN links k USING (vendor_id)
        ORDER BY f.last_symbol, f.vendor_id, k.valid_from
        """,
        [list(SPOT_CHECK)],
    )
    for row in spot:
        row["issuer_names"] = raw_names.get(str(row["cik"]), [])[:3] if row["cik"] else []
    out["spot_check"] = spot

    # Attrition estimate (R2a owner_link_attrition proxy) -------------------------------------
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE formation AS
        WITH j AS (
            SELECT lm.vendor_id, lm.month, f.cur_cik IS NULL AS unlinked, f.earn,
                   k.valid_from <= lm.formation_bar AND lm.formation_bar < k.valid_to AS covers,
                   CAST(lm.formation_bar AS TIMESTAMP) + INTERVAL 22 HOUR AS cutoff, k.available_at, k.high_from,
                   k.low_from
            FROM ri_line_months lm JOIN line_facts f USING (vendor_id)
            LEFT JOIN links k ON k.vendor_id = lm.vendor_id AND f.cur_cik IS NULL
            WHERE lm.month > (SELECT min(month) FROM ri_line_months)
              AND lm.month < (SELECT max(month) FROM ri_line_months)
        )
        -- Tier filters use the tier in force at the formation cutoff (tier_history), never the final label.
        SELECT vendor_id, month, unlinked, earn,
               coalesce(bool_or(covers), false) AS hindsight,
               CASE WHEN bool_or(covers AND available_at <= cutoff AND high_from <= cutoff
                                 AND (low_from IS NULL OR low_from > cutoff)) THEN 0
                    WHEN bool_or(covers AND available_at <= cutoff AND (low_from IS NULL OR low_from > cutoff)) THEN 1
                    WHEN bool_or(covers AND available_at <= cutoff) THEN 2 END AS pit_rank
        FROM j GROUP BY vendor_id, month, unlinked, earn
        """
    )
    monthly = _rows(
        con,
        """
        SELECT month, earn, count(*) AS eligible, count(*) FILTER (WHERE unlinked) AS unlinked,
               count(*) FILTER (WHERE unlinked AND pit_rank IS NOT NULL) AS pit_all,
               count(*) FILTER (WHERE unlinked AND pit_rank <= 1) AS pit_high_medium,
               count(*) FILTER (WHERE unlinked AND pit_rank = 0) AS pit_high,
               count(*) FILTER (WHERE unlinked AND hindsight) AS hindsight
        FROM formation GROUP BY ALL ORDER BY month, earn
        """,
    )
    out["attrition_estimate"] = _attrition(monthly)
    # I1: how much of the final tier is known when the link becomes available.
    out["pit_tier"] = {
        "by_tier_at_available_at": _rows(
            con,
            """
            SELECT tier AS final_tier, tier_at_available_at, count(*) AS links,
                   quantile_cont(date_diff('day', CAST(available_at AS DATE), CAST(tier_attained_at AS DATE)), 0.5)
                       AS median_days_to_final_tier
            FROM links WHERE current_cik IS NULL GROUP BY ALL ORDER BY 1, 2
            """,
        ),
        "terminal_items_by_clock_basis": _rows(
            con,
            """
            SELECT json_extract_string(j.value, '$.filing_clock_basis') AS basis, count(*) AS items
            FROM evidence_rows e, json_each(json_extract(e.value_json, '$.items')) j
            WHERE e.evidence_status = 'reconstructed'
              AND json_extract_string(j.value, '$.kind') = 'terminal_filing_alignment'
            GROUP BY 1 ORDER BY 2 DESC
            """,
        ),
    }
    out["schema_audit"] = _schema_audit(work)
    out["memory"] = _peak_mb()
    con.close()
    return out


def _attrition_summary(per_month: Mapping[dt.date, Counter[str]], months: Sequence[dt.date]) -> dict[str, object]:
    """Mean per-formation attrition before/after the hook over ``months`` and the relative reduction."""
    if not months:
        return {}
    rates: dict[str, list[float]] = defaultdict(list)
    for month in months:
        bucket = per_month[month]
        eligible = bucket["eligible"]
        rates["before"].append(bucket["unlinked"] / eligible)
        rates["after_pit_all_tiers"].append((bucket["unlinked"] - bucket["pit_all"]) / eligible)
        rates["after_pit_high_medium"].append((bucket["unlinked"] - bucket["pit_high_medium"]) / eligible)
        rates["after_pit_high_only"].append((bucket["unlinked"] - bucket["pit_high"]) / eligible)
        rates["after_hindsight_no_pit"].append((bucket["unlinked"] - bucket["hindsight"]) / eligible)
    means = {key: math.fsum(values) / len(values) for key, values in rates.items()}
    summary: dict[str, object] = {"formations": len(months)}
    summary.update({f"mean_{key}": round(value, 4) for key, value in means.items()})
    for key in ("after_pit_all_tiers", "after_pit_high_medium", "after_pit_high_only", "after_hindsight_no_pit"):
        before = means["before"]
        summary[f"relative_reduction_{key}"] = round((before - means[key]) / before, 4) if before else None
    return summary


def _attrition(monthly: Sequence[Mapping[str, object]]) -> dict[str, object]:
    """Per-formation attrition = unlinked eligible / eligible, before and after the hook."""
    eras = (("2012-2014", 2012, 2014), ("2015-2017", 2015, 2017), ("2018-2020", 2018, 2020),
            ("2021-2023", 2021, 2023), ("2024-2026", 2024, 2026))
    result: dict[str, object] = {}
    for scope, keep in (("all_lines", (True, False)), ("earnings_flag_lines", (True,))):
        per_month: dict[dt.date, Counter[str]] = defaultdict(Counter)
        for row in monthly:
            if row["earn"] not in keep:
                continue
            bucket = per_month[row["month"]]  # type: ignore[index]
            for key in ("eligible", "unlinked", "pit_all", "pit_high_medium", "pit_high", "hindsight"):
                bucket[key] += int(row[key])  # type: ignore[call-overload]
        months = sorted(per_month)
        scoped: dict[str, object] = {"all_formations": _attrition_summary(per_month, months)}
        for label, first, last in eras:
            scoped[label] = _attrition_summary(per_month, [month for month in months if first <= month.year <= last])
        result[scope] = scoped
    return result


def _schema_audit(work: Path) -> dict[str, object]:
    """Load every evidence row into the 0327 DDL (CHECK constraints apply) and run the A9 audits."""
    path = work / "ri1_validate.duckdb"
    for stale in (path, path.with_name(path.name + ".wal")):
        stale.unlink(missing_ok=True)
    con = _connect(path, "192MB")
    create_historical_identity_tables(con)
    columns = ", ".join(name for name, _kind in hi.EVIDENCE_COLUMNS)
    source = (work / "evidence_rows.parquet").as_posix()
    total = int(con.execute(f"SELECT count(*) FROM read_parquet('{source}')").fetchone()[0])  # type: ignore[index]
    for offset in range(0, total, 1000):  # bounded transactions (transaction-local storage does not spill)
        con.execute(
            f"INSERT INTO security_identity_evidence ({columns}) SELECT {columns} "
            f"FROM read_parquet('{source}', file_row_number = true) "
            f"WHERE file_row_number >= {offset} AND file_row_number < {offset + 1000}"
        )
    audit = hi.audit_identity_evidence(con)
    labels = _one(
        con,
        f"""
        SELECT count(*) AS rows,
               count(*) FILTER (WHERE evidence_status NOT IN ('reconstructed', 'conflicting', 'unknown')) AS bad_status,
               count(*) FILTER (WHERE availability_status <> 'modeled') AS not_modeled,
               count(*) FILTER (WHERE method <> '{ir.METHOD}') AS other_method,
               count(*) FILTER (WHERE json_extract_string(value_json, '$.identity_basis')
                                IS DISTINCT FROM '{ir.IDENTITY_BASIS}') AS missing_basis,
               count(*) FILTER (WHERE evidence_status = 'reconstructed' AND available_at IS NULL) AS accepted_no_clock,
               count(*) FILTER (WHERE evidence_status <> 'reconstructed' AND available_at IS NOT NULL)
                   AS rejected_with_clock
        FROM security_identity_evidence
        """,
    )
    sql_b = [int(con.execute(sql).fetchone()[0]) for sql in hi.ACCEPTANCE_SQL_B]  # type: ignore[index]
    con.close()
    return {"loaded_rows": total, "audit_nonzero": {k: v for k, v in audit.items() if v}, "sql_b": sql_b,
            "labels": labels}


# ---------------------------------------------------------------------------------------------


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="RI1 reconstructed line -> CIK link measurement (read-only).")
    parser.add_argument("--work", type=Path, required=True, help="scratch directory (outside data/)")
    parser.add_argument("--stages", default=",".join(STAGES), help=f"comma list of {STAGES}")
    parser.add_argument("--memory-limit", default="192MB", help=f"DuckDB memory limit (<= {MAX_MEMORY_MB}MB)")
    parser.add_argument("--batch-size", type=int, default=500)
    parser.add_argument("--decoy", type=float, action="append", default=[], help="decoy scale factor (repeatable)")
    args = parser.parse_args(argv)
    work = args.work.resolve()
    if work == DATA.resolve() or DATA.resolve() in work.parents:
        parser.error("--work must be a scratch directory outside data/")
    if _memory_mb(args.memory_limit) > MAX_MEMORY_MB:
        parser.error(f"--memory-limit must be at most {MAX_MEMORY_MB}MB")
    stages = [stage.strip() for stage in args.stages.split(",") if stage.strip()]
    unknown = sorted(set(stages) - set(STAGES))
    if unknown:
        parser.error(f"unknown stages {unknown}")
    work.mkdir(parents=True, exist_ok=True)
    params = ir.ReconstructionParams()
    if "stage" in stages:
        stage_sources(work, args.memory_limit)
    if "facts" in stages:
        stage_facts(work, args.memory_limit)
    if "lines" in stages:
        stage_lines(work, args.memory_limit)
    if "filings" in stages:
        stage_filings(work, args.memory_limit, params)
    if "run" in stages:
        run_reconstruction(work, args.memory_limit, params, args.batch_size, args.decoy)
    if "measure" in stages:
        result = measure(work, args.memory_limit)
        (work / "measure.json").write_text(json.dumps(result, indent=1, default=str), encoding="utf-8")
        _log(f"measure.json written to {work}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
