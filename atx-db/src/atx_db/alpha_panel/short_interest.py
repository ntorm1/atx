"""Stage S: FINRA consolidated short interest -> ``short_interest/si.parquet``.

Sources
* Existing landing (read-only) ``C:/atx/data/finra_short_interest`` (``C.FINRA_SI_DIR``):
  ``raw/si_YYYYMMDD.csv`` (settlements 2017-12-29 .. 2026-08-31, sha256 in ``raw/manifest.csv``) and
  ``dissemination_schedule.csv`` (official FINRA settlement / publication dates through 2026-12-31).
* New landing ``atx-db/data/raw/finra_short_interest`` (``fetch``): settlements listed by the FINRA
  partitions API (or scheduled and already published) that the existing landing lacks, fetched from
  ``https://cdn.finra.org/equity/otcmarket/biweekly/shrtYYYYMMDD.csv`` (gzip at rest, manifest.csv).

Output ``short_interest/si.parquet`` (one row per ``(security_id, settlement_date)``):
``security_id BIGINT, symbol, settlement_date DATE, dissemination_date DATE, si_shares, si_prev,
si_dtc, adv_finra (DOUBLE), revision_flag, market_class, split_flag`` plus provenance
``dissemination_source, map_basis, source_file``. ``short_interest/mapping_audit.parquet`` keeps
every FINRA row (after duplicate resolution) with its mapping outcome.

Rule ``si-ticker-asof-settlement-v3`` (same decisions as the earlier as-of producer
``build-equity/audits/iteration21_finra_si_asof.py``, re-based on TickerHistory3):
* map date = last vendor session (>= 1,000 vendor rows) on or before the settlement date, at most 7
  calendar days back; FINRA ``symbolCode`` -> the ORATS ``securityID`` whose ``ticker_tk`` has the same
  canonical form that day ('.', '/', '-', whitespace removed, case-sensitive; FINRA writes BRKB where
  ORATS writes BRK.B). A canonical form carried by two or more ids is ambiguous -> unmapped.
  When no vendor row carries the form on the map date (TickerHistory3 omits thin lines on some
  sessions), the latest earlier session within 7 calendar days of the settlement that carries it is
  used (``vendor_date``, ``vendor_date_fallback``). Vendor rows filed under ``securityID = 0`` are
  re-attributed with the shared ``sid0-bracketed-ticker-v2`` repair (identical to stage P).
* duplicate ``(settlement, symbol)`` rows: exchange class before OTC/OTCBB, then the earliest
  landing, then the last line of the file.
* two symbols -> one id on a settlement: exchange-class rows form the pool when any exists; a pool of
  one keeps its row, else the single exact raw-ticker match keeps it, else every row is dropped.
* ``dissemination_date`` = the official FINRA publication (exchange receipt up to 2020-12-31) date;
  a settlement missing from the schedule gets settlement + 7 US business days (NYSE sessions) and
  ``dissemination_source = 'rule-7bd'``. Visible on sessions strictly after it (contract).
* ``si_dtc`` = FINRA ``daysToCoverQuantity`` (FINRA floors it at 1.00), NULL when
  ``averageDailyVolumeQuantity`` is 0 (FINRA then writes 999.99).
"""

from __future__ import annotations

import argparse
import bisect
import csv
import datetime as dt
import hashlib
import io
import json
import sys
import time
from pathlib import Path
from typing import Any

from . import common as C
from . import finra_fetch as F

RULE = "si-ticker-asof-settlement-v3"
OTC_CLASSES = ("OTC", "OTCBB")
LOOKBACK_DAYS = 7
RULE_BUSINESS_DAYS = 7
PARTITIONS_URL = "https://api.finra.org/partitions/group/otcMarket/name/consolidatedShortInterest"
API_DATA_URL = "https://api.finra.org/data/group/otcMarket/name/consolidatedShortInterest"
API_PAGE = 5000
API_SORT = ("symbolCode", "marketClassCode")
FILE_URL = "https://cdn.finra.org/equity/otcmarket/biweekly/shrt{d:%Y%m%d}.csv"
OLD_RAW = C.FINRA_SI_DIR / "raw"
SCHEDULE = C.FINRA_SI_DIR / "dissemination_schedule.csv"
OLD_ASOF = C.FINRA_SI_DIR / "asof"
RAW_COLUMNS = [
    "accountingYearMonthNumber", "symbolCode", "issueName", "issuerServicesGroupExchangeCode", "marketClassCode",
    "currentShortPositionQuantity", "previousShortPositionQuantity", "stockSplitFlag", "averageDailyVolumeQuantity",
    "daysToCoverQuantity", "revisionFlag", "changePercent", "changePreviousNumber", "settlementDate",
]
# NYSE full-day closures after the vendor calendar ends (2026-09-18); used only by the rule fallback.
NYSE_HOLIDAYS_AFTER_VENDOR = {
    dt.date(2026, 11, 26), dt.date(2026, 12, 25), dt.date(2027, 1, 1), dt.date(2027, 1, 18), dt.date(2027, 2, 15),
    dt.date(2027, 3, 26), dt.date(2027, 5, 31), dt.date(2027, 6, 18), dt.date(2027, 7, 5), dt.date(2027, 9, 6),
    dt.date(2027, 11, 25), dt.date(2027, 12, 24),
}


def _tmp() -> Path:
    return F.finra_tmp("finra_si")


def _new_manifest() -> Path:
    return F.SI_RAW_DIR / "manifest.csv"


def load_schedule() -> dict[dt.date, tuple[dt.date, str]]:
    with SCHEDULE.open(newline="", encoding="utf-8") as fh:
        return {
            dt.date.fromisoformat(r["settlement_date"]): (dt.date.fromisoformat(r["dissemination_date"]), r["source"])
            for r in csv.DictReader(fh)
        }


def old_manifest() -> dict[str, dict[str, str]]:
    with (OLD_RAW / "manifest.csv").open(newline="", encoding="utf-8") as fh:
        return {r["settlement_date"]: r for r in csv.DictReader(fh)}


# ---------------------------------------------------------------- fetch
def fetch(retry_absent: bool = False, today: dt.date | None = None) -> dict[str, Any]:
    """Land settlements the existing landing lacks: API partitions + scheduled, already-published dates."""
    today = today or dt.datetime.now(dt.UTC).date()
    F.SI_RAW_DIR.mkdir(parents=True, exist_ok=True)
    status, body = F.get(PARTITIONS_URL, accept="application/json")
    if status != 200:
        raise F.FetchError(f"partitions API HTTP {status}")
    stamp = dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%SZ")
    snap = F.SI_RAW_DIR / f"consolidatedShortInterest_partitions_{stamp}.json.gz"
    F.write_gzip_atomic(snap, body)
    api_dates = sorted(dt.date.fromisoformat(p["partitions"][0]) for p in json.loads(body)["availablePartitions"])
    old = {dt.date.fromisoformat(k) for k in old_manifest()}
    sched = load_schedule()
    last_old = max(old)
    scheduled_published = [s for s, (pub, _) in sched.items() if s > last_old and pub <= today]
    wanted = sorted({d for d in api_dates if d not in old} | set(scheduled_published))
    man = F.load_manifest(_new_manifest())
    fetched = []
    for d in wanted:
        prior = man.get(d.isoformat())
        if F.is_done(prior, F.SI_RAW_DIR, retry_absent) and int((prior or {}).get("http_status") or 0) == 200:
            continue
        row = F.fetch_one(d, FILE_URL.format(d=d), F.SI_RAW_DIR / f"si_{d:%Y%m%d}.csv.gz", _new_manifest())
        print(f"  {d}: cdn http {row['http_status']} rows {row['rows']}", flush=True)
        if int(row["http_status"]) != 200 and d in api_dates:
            # the partition is published but the CDN file is not (yet): take it from the Query API
            row = fetch_api(d)
            print(f"  {d}: query API rows {row['rows']}", flush=True)
        fetched.append({k: row[k] for k in ("date", "http_status", "bytes", "rows", "url")})
    return {
        "partitions_snapshot": snap.name, "api_partitions": len(api_dates),
        "api_last": api_dates[-1].isoformat() if api_dates else None,
        "api_dates_not_in_old_landing": [d.isoformat() for d in api_dates if d not in old],
        "scheduled_published_after_old_landing": [d.isoformat() for d in scheduled_published],
        "fetched": fetched,
    }


def fetch_api(d: dt.date) -> dict[str, Any]:
    """One settlement from the public Query API as served (comma CSV, header once), in stable sorted pages."""
    base = {"compareFilters": [{"compareType": "EQUAL", "fieldName": "settlementDate", "fieldValue": d.isoformat()}],
            "sortFields": list(API_SORT)}
    status, _body, hdr = F.request(API_DATA_URL, accept="text/plain", json_body={**base, "limit": 1, "offset": 0})
    if status != 200:
        raise F.FetchError(f"query API probe {d}: HTTP {status}")
    total = int(hdr.get("record-total", "0"))
    header: str | None = None
    parts: list[str] = []
    rows = offset = 0
    while offset < total:
        status, body, _ = F.request(API_DATA_URL, accept="text/plain", json_body={**base, "limit": API_PAGE, "offset": offset})
        if status != 200:
            raise F.FetchError(f"query API page {d} offset {offset}: HTTP {status}")
        text = body.decode("utf-8")
        nl = text.find("\n")
        if nl < 0:
            break
        h, data = text[:nl].rstrip("\r"), text[nl + 1:]
        if header is None:
            header = h
        elif h != header:
            raise F.FetchError(f"query API header changed between pages for {d}")
        page_rows = sum(1 for r in csv.reader(io.StringIO(data)) if r)
        if page_rows == 0:
            break
        parts.append(data if data.endswith("\n") else data + "\n")
        rows += page_rows
        offset += page_rows
    if header is None or rows != total:
        raise F.FetchError(f"query API {d}: {rows} rows vs record-total {total}")
    raw = (header + "\n" + "".join(parts)).encode("utf-8")
    dest = F.SI_RAW_DIR / f"si_api_{d:%Y%m%d}.csv.gz"
    F.write_gzip_atomic(dest, raw)
    row = {
        "date": d.isoformat(), "file": dest.name, "bytes": len(raw), "sha256_of_raw_bytes": hashlib.sha256(raw).hexdigest(),
        "rows": rows, "url": f"{API_DATA_URL}?settlementDate={d}&sortFields={','.join(API_SORT)}&limit={API_PAGE}",
        "http_status": 200, "downloaded_at": dt.datetime.now(dt.UTC).isoformat(timespec="seconds"),
    }
    F.append_manifest(_new_manifest(), row)
    return row


# ---------------------------------------------------------------- parse
def _sources(receipt: dict[str, Any]) -> list[dict[str, Any]]:
    """Every landed settlement file with a verified raw sha256 (existing landing first)."""
    out: list[dict[str, Any]] = []
    bad = []
    for sd, r in sorted(old_manifest().items()):
        path = OLD_RAW / r["file"]
        sha = C.sha256_file(path)
        if sha != r["sha256"]:
            bad.append(r["file"])
            continue
        out.append({"settlement": sd, "path": path, "sha256": sha, "landing": 0, "gz": False, "file": r["file"]})
    for sd, r in sorted(F.load_manifest(_new_manifest()).items()):
        if int(r.get("http_status") or 0) != 200:
            continue
        if any(s["settlement"] == sd for s in out):
            continue  # the existing landing already holds this settlement; it is the earlier vintage
        path = F.SI_RAW_DIR / r["file"]
        out.append({"settlement": sd, "path": path, "sha256": r["sha256_of_raw_bytes"], "landing": 1, "gz": True,
                    "file": r["file"]})
    if bad:
        raise RuntimeError(f"existing landing sha256 mismatch vs its manifest: {bad}")
    receipt["sources"] = {
        "existing_landing": {"dir": str(OLD_RAW), "files": sum(1 for s in out if s["landing"] == 0),
                             "manifest_sha256": C.sha256_file(OLD_RAW / "manifest.csv")},
        "new_landing": {"dir": str(F.SI_RAW_DIR), "files": [s["file"] for s in out if s["landing"] == 1],
                        "manifest_sha256": C.sha256_file(_new_manifest()) if _new_manifest().exists() else None},
        "schedule": {"path": str(SCHEDULE), "sha256": C.sha256_file(SCHEDULE)},
        "files_sha256": {s["file"]: s["sha256"] for s in out},
    }
    return out


def parse_sources(sources: list[dict[str, Any]], receipt: dict[str, Any]) -> Path:
    """One parsed parquet per source file (all strings, file order kept as ``line_no``); cached by sha."""
    import pyarrow as pa
    import pyarrow.csv as pacsv
    import pyarrow.parquet as pq

    pdir = _tmp() / "parsed"
    pdir.mkdir(parents=True, exist_ok=True)
    keep = set()
    stats = {"files": 0, "rows": 0, "invalid_rows_skipped": 0, "reused": 0}
    for s in sources:
        dest = pdir / f"{Path(s['file']).name.split('.')[0]}_{s['sha256'][:12]}.parquet"
        keep.add(dest.name)
        stats["files"] += 1
        if dest.exists():
            stats["reused"] += 1
            stats["rows"] += pq.ParquetFile(dest).metadata.num_rows
            continue
        body = F.read_gzip_verified(s["path"], s["sha256"]) if s["gz"] else s["path"].read_bytes()
        skipped = [0]

        def _skip(_row: Any) -> str:
            skipped[0] += 1
            return "skip"

        tbl = pacsv.read_csv(
            io.BytesIO(body),
            read_options=pacsv.ReadOptions(block_size=1 << 23),
            parse_options=(pacsv.ParseOptions(delimiter=",", quote_char='"', invalid_row_handler=_skip) if s["file"].startswith("si_api_")
                           else pacsv.ParseOptions(delimiter="|", quote_char=False, invalid_row_handler=_skip)),
            convert_options=pacsv.ConvertOptions(column_types={c: pa.string() for c in RAW_COLUMNS},
                                                 include_columns=RAW_COLUMNS, strings_can_be_null=False),
        )
        n = tbl.num_rows
        tbl = tbl.append_column("line_no", pa.array(range(n), pa.int32()))
        tbl = tbl.append_column("source_file", pa.array([s["file"]] * n, pa.string()))
        tbl = tbl.append_column("landing", pa.array([s["landing"]] * n, pa.int8()))
        tmp = dest.with_name(dest.name + ".partial")
        pq.write_table(tbl, tmp, compression="zstd")
        tmp.replace(dest)
        stats["rows"] += n
        stats["invalid_rows_skipped"] += skipped[0]
    for p in pdir.glob("*.parquet"):
        if p.name not in keep:
            p.unlink()
    receipt["parse"] = stats
    return pdir


# ---------------------------------------------------------------- dates
def add_business_days(d: dt.date, n: int, sessions: list[dt.date]) -> dt.date:
    """``d`` + n NYSE business days: vendor sessions where known, weekdays minus closures beyond."""
    last = sessions[-1]
    known = set(sessions)
    while n:
        d += dt.timedelta(days=1)
        if d <= last:
            if d in known:
                n -= 1
        elif d.weekday() < 5 and d not in NYSE_HOLIDAYS_AFTER_VENDOR:
            n -= 1
    return d


def settlement_table(settlements: list[dt.date], sessions: list[dt.date]) -> tuple[list[tuple], dict[str, Any]]:
    sched = load_schedule()
    rows, rule_rows, no_map = [], [], []
    for s in settlements:
        if s in sched:
            pub, src = sched[s]
        else:
            pub, src = add_business_days(s, RULE_BUSINESS_DAYS, sessions), "rule-7bd"
            rule_rows.append(s.isoformat())
        if pub <= s:
            raise RuntimeError(f"dissemination {pub} not after settlement {s}")
        i = bisect.bisect_right(sessions, s) - 1
        md = sessions[i] if i >= 0 and (s - sessions[i]).days <= LOOKBACK_DAYS else None
        if md is None:
            no_map.append(s.isoformat())
        rows.append((s, pub, src, md))
    return rows, {"rule_fallback_settlements": rule_rows, "settlements_without_vendor_date": no_map}


# ---------------------------------------------------------------- build
def build_asof(con, pglob: str, th_primary: Path, st_rows: list[tuple], sessions: list[dt.date],
               receipt: dict[str, Any]) -> Path:
    """Vendor ``(settlement, canonical ticker) -> ids`` as of each settlement.

    Primary: vendor rows on the map date (last session <= settlement). Fallback, only for a FINRA
    canonical symbol that no vendor row carries on the map date: the latest earlier session within
    ``LOOKBACK_DAYS`` calendar days of the settlement on which some vendor row carries it (TickerHistory3
    omits thinly traded lines on some sessions; the earlier as-of producer's vendor files had them).
    """
    dest = _tmp() / "th_asof.parquet"
    dmap: dict[dt.date, dt.date] = {}
    for s, _pub, _src, md in st_rows:
        if md is None:
            continue
        for d in sessions[bisect.bisect_left(sessions, s - dt.timedelta(days=LOOKBACK_DAYS)):bisect.bisect_left(sessions, md)]:
            if d in dmap:
                raise RuntimeError(f"fallback windows overlap on {d}")
            dmap[d] = s
    con.execute("CREATE OR REPLACE TEMP TABLE dmap (d DATE, settlement_date DATE)")
    con.executemany("INSERT INTO dmap VALUES (?, ?)", sorted(dmap.items()))
    rep, _ = F.sid0_repairs(con)
    ck_t = F.CANON_SQL.format(x="t.ticker_tk")
    ck_r = F.CANON_SQL.format(x="r.ticker_tk")
    sql = f"""
        WITH prim AS (
            SELECT st.settlement_date, t.map_date AS vendor_date, t.security_id, t.ticker_tk, {ck_t} AS ck,
                   t.sid_repaired, false AS fallback
            FROM read_parquet('{th_primary.as_posix()}') t JOIN settle st ON st.map_date = t.map_date
        ),
        need AS (
            SELECT DISTINCT CAST(settlementDate AS DATE) AS settlement_date, {F.CANON_SQL.format(x='symbolCode')} AS ck
            FROM read_parquet('{pglob}') WHERE settlementDate IS NOT NULL AND settlementDate <> ''
            EXCEPT
            SELECT settlement_date, ck FROM prim
        ),
        win AS (
            SELECT dm.settlement_date, t.tradingDate AS vendor_date, t.securityID AS security_id, t.ticker_tk,
                   {ck_t} AS ck, false AS sid_repaired
            FROM read_parquet('{C.TICKERHISTORY.as_posix()}') t JOIN dmap dm ON dm.d = t.tradingDate
            WHERE t.securityID > 0 AND t.ticker_tk IS NOT NULL AND trim(t.ticker_tk) <> ''
            UNION ALL
            SELECT dm.settlement_date, r.trade_date, r.security_id, r.ticker_tk, {ck_r}, true
            FROM read_parquet('{rep.as_posix()}') r JOIN dmap dm ON dm.d = r.trade_date
            WHERE r.basis = 'bracketed'
        ),
        fb AS (
            SELECT w.*, max(w.vendor_date) OVER (PARTITION BY w.settlement_date, w.ck) AS last_d
            FROM win w SEMI JOIN need n ON n.settlement_date = w.settlement_date AND n.ck = w.ck
        )
        SELECT * FROM prim
        UNION ALL
        SELECT settlement_date, vendor_date, security_id, ticker_tk, ck, sid_repaired, true
        FROM fb WHERE vendor_date = last_d
    """
    n = C.copy_to_parquet(con, sql, dest)
    fb_rows = con.execute(f"SELECT count(*) FROM read_parquet('{dest.as_posix()}') WHERE fallback").fetchone()[0]
    receipt["vendor_asof"] = {"rows": n, "fallback_rows": int(fb_rows), "fallback_window_sessions": len(dmap),
                              "lookback_calendar_days": LOOKBACK_DAYS}
    return dest


def _stage_sql(pglob: str, th: str) -> str:
    canon_sym = F.CANON_SQL.format(x="symbolCode")
    return f"""
    CREATE OR REPLACE TEMP TABLE audit AS
    WITH raw AS (
        SELECT CAST(settlementDate AS DATE) AS settlement_date, trim(symbolCode) AS symbol,
               {canon_sym} AS ck, trim(symbolCode) AS sym_u,
               marketClassCode AS market_class, issueName AS issue_name,
               TRY_CAST(currentShortPositionQuantity AS DOUBLE) AS si_shares,
               TRY_CAST(previousShortPositionQuantity AS DOUBLE) AS si_prev,
               TRY_CAST(daysToCoverQuantity AS DOUBLE) AS dtc_raw,
               TRY_CAST(averageDailyVolumeQuantity AS DOUBLE) AS adv_finra,
               NULLIF(trim(revisionFlag), '') AS revision_flag, NULLIF(trim(stockSplitFlag), '') AS split_flag,
               accountingYearMonthNumber <> replace(settlementDate, '-', '') AS aymn_mismatch,
               marketClassCode NOT IN {OTC_CLASSES} AS is_exchange,
               source_file, landing, line_no
        FROM read_parquet('{pglob}')
        WHERE settlementDate IS NOT NULL AND settlementDate <> '' AND trim(symbolCode) <> ''
    ),
    dedup AS (
        SELECT *, row_number() OVER (PARTITION BY settlement_date, symbol
                                     ORDER BY is_exchange DESC, landing, source_file, line_no DESC) AS dup_rank,
               count(*) OVER (PARTITION BY settlement_date, symbol) AS dup_n,
               count(DISTINCT market_class) OVER (PARTITION BY settlement_date, symbol) AS dup_classes
        FROM raw
    ),
    th AS (
        SELECT settlement_date, vendor_date, security_id, trim(ticker_tk) AS tk, ck, sid_repaired, fallback
        FROM read_parquet('{th}')
    ),
    th_canon AS (
        SELECT settlement_date, ck, min(security_id) AS sid, count(DISTINCT security_id) AS n_ids,
               bool_and(sid_repaired) AS sid_repaired, max(vendor_date) AS vendor_date, bool_or(fallback) AS fallback
        FROM th GROUP BY 1, 2
    ),
    th_exact AS (
        SELECT DISTINCT settlement_date, security_id, tk FROM th
    ),
    cand AS (
        SELECT d.*, st.dissemination_date, st.dissemination_source, st.map_date,
               tc.vendor_date, coalesce(tc.fallback, false) AS vendor_date_fallback,
               CASE WHEN tc.n_ids = 1 THEN tc.sid END AS sid0,
               coalesce(tc.n_ids = 1 AND tc.sid_repaired, false) AS sid_repaired,
               CASE WHEN st.map_date IS NULL THEN 'no_vendor_date' WHEN tc.n_ids > 1 THEN 'ambiguous'
                    WHEN tc.n_ids = 1 THEN 'candidate' ELSE 'unmapped' END AS outcome0,
               coalesce(tc.n_ids = 1 AND te.security_id IS NOT NULL, false) AS exact
        FROM dedup d
        JOIN settle st ON st.settlement_date = d.settlement_date
        LEFT JOIN th_canon tc ON tc.settlement_date = d.settlement_date AND tc.ck = d.ck
        LEFT JOIN th_exact te ON te.settlement_date = d.settlement_date AND te.security_id = tc.sid AND te.tk = d.sym_u
        WHERE d.dup_rank = 1
    ),
    pooled AS (
        SELECT *,
               count(*) FILTER (WHERE is_exchange) OVER w AS n_exch
        FROM cand WINDOW w AS (PARTITION BY settlement_date, sid0)
    ),
    pooled2 AS (
        SELECT *, (is_exchange OR n_exch = 0) AS in_pool FROM pooled
    ),
    resolved AS (
        SELECT *,
               count(*) FILTER (WHERE in_pool) OVER w AS pool_n,
               count(*) FILTER (WHERE in_pool AND exact) OVER w AS pool_exact
        FROM pooled2 WINDOW w AS (PARTITION BY settlement_date, sid0)
    )
    SELECT settlement_date, symbol, market_class, issue_name, si_shares, si_prev, dtc_raw, adv_finra,
           revision_flag, split_flag, aymn_mismatch, is_exchange, source_file, line_no, dup_n, dup_classes,
           dissemination_date, dissemination_source, map_date, vendor_date, vendor_date_fallback,
           sid0 AS candidate_security_id, exact, sid_repaired,
           CASE WHEN outcome0 <> 'candidate' THEN outcome0
                WHEN in_pool AND (pool_n = 1 OR (exact AND pool_exact = 1)) THEN 'mapped'
                WHEN NOT in_pool THEN 'collision_lost_to_exchange_row'
                ELSE 'collision_dropped' END AS outcome,
           pool_n
    FROM resolved
    """


def build() -> dict[str, Any]:
    t_all = time.perf_counter()
    receipt: dict[str, Any] = {"stage": "short_interest", "rule": RULE, "tickerhistory": C.file_identity(C.TICKERHISTORY)}
    out = C.stage_dir("short_interest")
    con = C.connect(memory=F.DUCKDB_MEMORY, threads=2, temp_dir=_tmp() / "spill")
    with C.timed(receipt, "sources"):
        sources = _sources(receipt)
    with C.timed(receipt, "parse"):
        pdir = parse_sources(sources, receipt)
    pglob = (pdir / "*.parquet").as_posix()
    with C.timed(receipt, "calendar"):
        sessions = F.sessions(con)
    settlements = [r[0] for r in con.execute(
        f"SELECT DISTINCT CAST(settlementDate AS DATE) FROM read_parquet('{pglob}') "
        f"WHERE settlementDate IS NOT NULL AND settlementDate <> '' ORDER BY 1").fetchall()]
    st_rows, st_info = settlement_table(settlements, sessions)
    receipt["settlements"] = {"count": len(settlements), "first": settlements[0].isoformat(),
                              "last": settlements[-1].isoformat(), **st_info}
    con.execute("CREATE OR REPLACE TEMP TABLE settle (settlement_date DATE, dissemination_date DATE, "
                "dissemination_source VARCHAR, map_date DATE)")
    con.executemany("INSERT INTO settle VALUES (?, ?, ?, ?)", st_rows)
    th = _tmp() / "th_map_dates.parquet"
    with C.timed(receipt, "ticker_extract"):
        receipt["ticker_extract_rows"] = F.ticker_extract(con, sorted({r[3] for r in st_rows if r[3]}), th)
        _, receipt["sid0_repair"] = F.sid0_repairs(con)
    with C.timed(receipt, "vendor_asof"):
        asof = build_asof(con, pglob, th, st_rows, sessions, receipt)
    with C.timed(receipt, "map"):
        con.execute(_stage_sql(pglob, asof.as_posix()))
    audit_path = out / "mapping_audit.parquet"
    C.copy_to_parquet(con, "SELECT * FROM audit ORDER BY settlement_date, symbol", audit_path)
    sql = """
        SELECT CAST(candidate_security_id AS BIGINT) AS security_id, symbol, settlement_date, dissemination_date,
               si_shares, si_prev, CASE WHEN adv_finra > 0 THEN dtc_raw END AS si_dtc, adv_finra,
               revision_flag, market_class, split_flag, dissemination_source,
               CASE WHEN sid_repaired THEN 'sid0_repaired' WHEN exact THEN 'exact' ELSE 'canonical' END AS map_basis,
               vendor_date, vendor_date_fallback, source_file
        FROM audit WHERE outcome = 'mapped'
        ORDER BY settlement_date, security_id
    """
    with C.timed(receipt, "write"):
        rows = C.copy_to_parquet(con, sql, out / "si.parquet")
    si = (out / "si.parquet").as_posix()
    dupkeys = con.execute(f"SELECT count(*) FROM (SELECT security_id, settlement_date FROM read_parquet('{si}') "
                          "GROUP BY 1, 2 HAVING count(*) > 1)").fetchone()[0]
    if dupkeys:
        raise RuntimeError(f"si.parquet has {dupkeys} duplicate (security_id, settlement_date) keys")
    receipt["rows"] = rows
    receipt["stats"] = stats(con, si)
    receipt["timings_s"]["total"] = round(time.perf_counter() - t_all, 1)
    receipt["outputs"] = {"si": C.file_identity(out / "si.parquet") | {"sha256": C.sha256_file(out / "si.parquet")},
                          "mapping_audit": C.file_identity(audit_path)}
    C.write_json_atomic(out / "manifest.json", receipt)
    return receipt


def _q(con, sql: str) -> list[tuple]:
    return con.execute(sql).fetchall()


def stats(con, si: str) -> dict[str, Any]:
    s: dict[str, Any] = {}
    s["duplicates"] = {
        "raw_rows_dropped_as_duplicate_symbol_key": int(_q(con, "SELECT coalesce(sum(dup_n - 1), 0) FROM audit")[0][0]),
        "duplicate_symbol_keys": int(_q(con, "SELECT count(*) FROM audit WHERE dup_n > 1")[0][0]),
        "duplicate_symbol_keys_mixed_class": int(_q(con, "SELECT count(*) FROM audit WHERE dup_classes > 1")[0][0]),
        "security_settlement_collisions_resolved": int(_q(
            con, "SELECT count(*) FROM audit WHERE outcome = 'mapped' AND pool_n > 1")[0][0]),
        "collision_rows_dropped": int(_q(con, "SELECT count(*) FROM audit WHERE outcome = 'collision_dropped'")[0][0]),
        "collision_rows_lost_to_exchange_row": int(_q(
            con, "SELECT count(*) FROM audit WHERE outcome = 'collision_lost_to_exchange_row'")[0][0]),
        "accounting_month_vs_settlement_mismatch": int(_q(con, "SELECT count(*) FROM audit WHERE aymn_mismatch")[0][0]),
    }
    by_year = _q(con, """
        SELECT year(settlement_date) y, count(*) n, count(*) FILTER (WHERE is_exchange) n_exch,
               count(*) FILTER (WHERE outcome = 'mapped') m,
               count(*) FILTER (WHERE outcome = 'mapped' AND is_exchange) m_exch,
               count(*) FILTER (WHERE outcome = 'ambiguous') amb, count(*) FILTER (WHERE outcome = 'unmapped') unm,
               count(*) FILTER (WHERE outcome LIKE 'collision%') col, count(*) FILTER (WHERE outcome = 'no_vendor_date') nv
        FROM audit GROUP BY 1 ORDER BY 1""")
    s["mapping_by_settlement_year"] = {
        str(y): {"finra_rows": n, "exchange_rows": ne, "mapped": m, "mapped_exchange": me,
                 "mapped_share_exchange_rows": round(me / ne, 4) if ne else None, "mapped_share_all_rows": round(m / n, 4),
                 "ambiguous": a, "unmapped": u, "collision": c, "no_vendor_date": nv}
        for y, n, ne, m, me, a, u, c, nv in by_year
    }
    by_class = _q(con, """
        SELECT market_class, count(*), count(*) FILTER (WHERE outcome = 'mapped'),
               count(*) FILTER (WHERE outcome = 'ambiguous'), count(*) FILTER (WHERE outcome = 'unmapped')
        FROM audit GROUP BY 1 ORDER BY 2 DESC""")
    s["mapping_by_market_class"] = {
        str(k): {"finra_rows": n, "mapped": m, "mapped_share": round(m / n, 4), "ambiguous": a, "unmapped": u}
        for k, n, m, a, u in by_class
    }
    by_class_cov = _q(con, """
        SELECT market_class, count(*), count(*) FILTER (WHERE outcome = 'mapped')
        FROM audit WHERE settlement_date >= DATE '2020-09-15' GROUP BY 1 ORDER BY 2 DESC""")
    s["mapping_by_market_class_coverage_window"] = {
        str(k): {"finra_rows": n, "mapped": m, "mapped_share": round(m / n, 4)} for k, n, m in by_class_cov
    }
    s["unmapped_exchange_top_adv_since_2020"] = [
        {"symbol": a, "issue": b, "class": c, "max_adv": d, "settlements": e, "outcome": f}
        for a, b, c, d, e, f in _q(con, """
            SELECT symbol, any_value(issue_name), any_value(market_class), max(adv_finra), count(*), any_value(outcome)
            FROM audit WHERE outcome <> 'mapped' AND is_exchange AND settlement_date >= DATE '2020-09-15'
            GROUP BY 1 ORDER BY 4 DESC LIMIT 15""")
    ]
    out_year = _q(con, f"""
        SELECT year(settlement_date), count(*), count(DISTINCT security_id), min(settlement_date), max(settlement_date),
               count(*) FILTER (WHERE si_dtc IS NULL), count(*) FILTER (WHERE si_dtc = 1.0),
               count(*) FILTER (WHERE revision_flag = 'R'), count(*) FILTER (WHERE split_flag = 'S'),
               count(*) FILTER (WHERE market_class IN {OTC_CLASSES})
        FROM read_parquet('{si}') GROUP BY 1 ORDER BY 1""")
    s["output_by_settlement_year"] = {
        str(y): {"rows": n, "ids": i, "first_settlement": str(a), "last_settlement": str(b), "si_dtc_null": dn,
                 "si_dtc_eq_1": d1, "revision_R": r, "split_S": sp, "otc_class_rows": oc}
        for y, n, i, a, b, dn, d1, r, sp, oc in out_year
    }
    diss = _q(con, f"""
        SELECT year(dissemination_date), count(*), count(DISTINCT security_id) FROM read_parquet('{si}') GROUP BY 1 ORDER BY 1""")
    s["output_by_dissemination_year"] = {str(y): {"rows": n, "ids": i} for y, n, i in diss}
    last = _q(con, f"SELECT max(settlement_date), max(dissemination_date), count(DISTINCT security_id) FROM read_parquet('{si}')")[0]
    s["last_settlement"], s["last_dissemination"], s["ids_total"] = str(last[0]), str(last[1]), int(last[2])
    s["dissemination_sources"] = dict(_q(con, f"SELECT dissemination_source, count(*) FROM read_parquet('{si}') GROUP BY 1"))
    s["map_basis"] = dict(_q(con, f"SELECT map_basis, count(*) FROM read_parquet('{si}') GROUP BY 1"))
    s["dtc_raw_999_99_rows"] = int(_q(con, "SELECT count(*) FROM audit WHERE dtc_raw = 999.99 AND outcome = 'mapped'")[0][0])
    return s


# ---------------------------------------------------------------- validate
def validate() -> dict[str, Any]:
    """Compare with the earlier as-of CSVs (available_at = dissemination date)."""
    out = C.stage_dir("short_interest")
    si = (out / "si.parquet").as_posix()
    audit = (out / "mapping_audit.parquet").as_posix()
    th = (_tmp() / "th_asof.parquet").as_posix()
    con = C.connect(memory=F.DUCKDB_MEMORY, threads=2, temp_dir=_tmp() / "spill")
    res: dict[str, Any] = {"old_asof_dir": str(OLD_ASOF)}
    for field, col in (("si_shares", "si_shares"), ("si_dtc", "si_dtc")):
        path = (OLD_ASOF / f"{field}.csv").as_posix()
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE old AS
            SELECT security_id::BIGINT AS security_id, available_at::DATE AS available_at, value::DOUBLE AS value
            FROM read_csv('{path}', header=true, columns={{'security_id': 'BIGINT', 'available_at': 'DATE', 'value': 'DOUBLE'}})
        """)
        con.execute(f"""
            CREATE OR REPLACE TEMP TABLE cmp AS
            SELECT o.*, n.{col} AS new_value, n.security_id IS NOT NULL AS new_row, n.settlement_date AS n_settle,
                   n.symbol AS n_symbol
            FROM old o LEFT JOIN read_parquet('{si}') n
              ON n.security_id = o.security_id AND n.dissemination_date = o.available_at
        """)
        tot, match, mism, miss, newnull = _q(con, """
            SELECT count(*), count(*) FILTER (WHERE new_value = value),
                   count(*) FILTER (WHERE new_row AND new_value IS NOT NULL AND new_value <> value),
                   count(*) FILTER (WHERE NOT new_row), count(*) FILTER (WHERE new_row AND new_value IS NULL)
            FROM cmp""")[0]
        # explain rows missing from the new build: the old id's vendor ticker on the map date -> FINRA row outcome
        why = _q(con, f"""
            WITH miss AS (
                SELECT c.security_id, c.available_at, s.settlement_date
                FROM cmp c
                LEFT JOIN (SELECT DISTINCT settlement_date, dissemination_date FROM read_parquet('{audit}')) s
                  ON s.dissemination_date = c.available_at
                WHERE NOT c.new_row
            ),
            tk AS (
                SELECT m.*, t.ticker_tk, t.ck
                FROM miss m LEFT JOIN read_parquet('{th}') t
                  ON t.settlement_date = m.settlement_date AND t.security_id = m.security_id
            ),
            j AS (
                SELECT tk.*, a.outcome, a.candidate_security_id
                FROM tk LEFT JOIN read_parquet('{audit}') a
                  ON a.settlement_date = tk.settlement_date AND {F.CANON_SQL.format(x='a.symbol')} = tk.ck
            )
            SELECT CASE WHEN settlement_date IS NULL THEN 'no_settlement_for_available_at'
                        WHEN ticker_tk IS NULL THEN 'no_vendor_row_for_id_matching_a_finra_symbol_within_7d'
                        WHEN outcome IS NULL THEN 'no_finra_row_for_vendor_ticker'
                        WHEN outcome = 'mapped' AND candidate_security_id <> security_id THEN 'finra_row_mapped_to_other_id'
                        ELSE 'finra_row_' || outcome END AS reason,
                   count(DISTINCT (security_id, available_at)) AS n,
                   list_sort(list(DISTINCT coalesce(ticker_tk, '?')))[1:8] AS examples
            FROM j GROUP BY 1 ORDER BY 2 DESC""")
        mism_ex = _q(con, """SELECT security_id, available_at, value, new_value, n_symbol FROM cmp
                             WHERE new_row AND new_value IS NOT NULL AND new_value <> value LIMIT 10""")
        by_year = _q(con, """
            SELECT year(available_at), count(*), count(DISTINCT security_id), count(*) FILTER (WHERE new_value = value),
                   count(*) FILTER (WHERE NOT new_row)
            FROM cmp GROUP BY 1 ORDER BY 1""")
        res[field] = {
            "old_rows": tot, "exact_match": match, "exact_match_rate": round(match / tot, 6) if tot else None,
            "value_mismatch": mism, "missing_in_new": miss, "new_row_value_null": newnull,
            "missing_reasons": [{"reason": r, "rows": int(n), "example_tickers": ex} for r, n, ex in why],
            "mismatch_examples": [list(map(str, x)) for x in mism_ex],
            "by_available_year": {str(y): {"old_rows": n, "old_ids": i, "match": m, "missing": ms}
                                  for y, n, i, m, ms in by_year},
            "old_last_available_at": str(_q(con, "SELECT max(available_at) FROM old")[0][0]),
        }
        # rows in the new build inside the old window that the old build did not have
        extra = _q(con, f"""
            SELECT CASE WHEN n.market_class IN {OTC_CLASSES} OR n.market_class = 'IEX' THEN 'class_not_in_old_exchange_set'
                        WHEN '{field}' = 'si_dtc' AND n.si_dtc IS NULL THEN 'dtc_null'
                        ELSE 'mapped_only_in_new' END, count(*)
            FROM read_parquet('{si}') n LEFT JOIN old o
              ON o.security_id = n.security_id AND o.available_at = n.dissemination_date
            WHERE o.security_id IS NULL AND n.dissemination_date <= (SELECT max(available_at) FROM old)
            GROUP BY 1 ORDER BY 2 DESC""")
        res[field]["new_rows_not_in_old_within_old_window"] = {k: int(v) for k, v in extra}
    new_year = _q(con, f"""SELECT year(dissemination_date), count(*), count(DISTINCT security_id)
                           FROM read_parquet('{si}') GROUP BY 1 ORDER BY 1""")
    res["new_by_dissemination_year"] = {str(y): {"rows": n, "ids": i} for y, n, i in new_year}
    res["new_last_dissemination"] = str(_q(con, f"SELECT max(dissemination_date) FROM read_parquet('{si}')")[0][0])
    C.write_json_atomic(out / "validation.json", res)
    man_path = out / "manifest.json"
    if man_path.exists():
        man = C.read_json(man_path)
        man["validation"] = {k: {kk: vv for kk, vv in v.items() if kk in ("old_rows", "exact_match_rate", "value_mismatch",
                                                                           "missing_in_new", "new_row_value_null")}
                             for k, v in res.items() if k in ("si_shares", "si_dtc")}
        man["validation"]["file"] = "validation.json"
        C.write_json_atomic(man_path, man)
    return res


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--retry-absent", action="store_true")
    sub.add_parser("build")
    sub.add_parser("validate")
    args = ap.parse_args(argv)
    if args.cmd == "fetch":
        print(json.dumps(fetch(args.retry_absent), indent=1), flush=True)
    elif args.cmd == "build":
        rec = build()
        print(json.dumps({k: rec[k] for k in ("rows", "settlements", "timings_s")}, default=str), flush=True)
    else:
        res = validate()
        print(json.dumps({k: (v if not isinstance(v, dict) else {kk: vv for kk, vv in v.items() if kk != "by_available_year"})
                          for k, v in res.items()}, indent=1, default=str), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
