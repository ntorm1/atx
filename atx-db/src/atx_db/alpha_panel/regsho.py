"""Stage ``regsho_threshold``: daily Reg SHO threshold security lists -> ``regsho_threshold/``.

Sources (every listing market found to publish history; public, no login):

| market | url | history probed |
| --- | --- | --- |
| ``nasdaq`` | ``https://www.nasdaqtrader.com/dynamic/symdir/regsho/nasdaqthYYYYMMDD.txt`` | 2018-01-02 on |
| ``nyse``, ``nyse_american``, ``nyse_arca`` | ``https://www.nyse.com/api/regulatory/threshold-securities/download?selectedDate=YYYY-MM-DD&market=...`` | 2018-01-02 on |
| ``cboe_bzx`` | ``https://cdn.cboe.com/resources/us/equities/market-statistics/reg-sho-threshold/bzx_equities_reg_sho_threshold_YYYYMMDD.txt`` | 2014-08-20 on |
| ``finra_otc`` | FINRA Query API ``otcMarket/thresholdList`` (OTC equities; month ranges) | 2016-01-04 on |

``download --market M`` lands one file per exchange session (weekdays minus the exchange holiday list) as
served, gzip at rest (``mtime=0``), under ``data/raw/regsho_threshold/<market>/`` with an append-only
``receipts.jsonl`` (url, http status, bytes, sha256 of the raw bytes, fetched_at, HTTP Last-Modified).
Requests to one host are sequential and at least 0.55 s apart (<= 2 req/s).

Output ``regsho_threshold/year=YYYY/threshold.parquet``: one row per ``(list_date, market, symbol)`` in a
published list: ``list_date DATE, market, symbol, security_name, market_category, on_list BOOL (Reg SHO
threshold flag = Y), rule_flag (Nasdaq Rule 3210 / FINRA Rule 4320 column), security_id BIGINT,
available_at TIMESTAMPTZ, available_at_basis, vintage_risk, run_days (consecutive published lists of that
market carrying the symbol with on_list, ending at this one), map_basis, source_file``. ``lists.parquet``
holds one row per ``(market, list_date)`` attempted: ``status`` (``list`` / ``empty_list`` /
``empty_or_absent`` / ``absent``), row counts, stamps and ``available_at``.

Clock rule ``regsho-publication-v1`` (per market; evidence in the manifest):
* ``nasdaq``: HTTP Last-Modified of the file dated D is 03:00-04:00 UTC on D+1 (23:00 ET on D). Rule
  ``available_at = max(D + 1 day 06:00 UTC, Last-Modified when within 10 days of D)``.
* ``nyse*``: the file's trailer stamp is D 21:05 ET (22:02 ET since 2024); the API also returns an empty
  list stamped D 21:05:00 for dates it has not published, so the stamp is not proof. Rule ``D + 1 day 06:00
  UTC``; ``vintage_risk`` true (served by an API, reconstructed on request).
* ``cboe_bzx``: the file dated D is posted the morning of the next session (Last-Modified 07:02 UTC = its
  trailer stamp 03:02 ET; 08:01 ET in 2018). Rule ``max(next session 16:00 UTC, Last-Modified when within 10
  days)``; old files carry a 2026-03 re-upload stamp -> ``vintage_risk`` true.
* ``finra_otc``: no stamp; rule ``next session 16:00 UTC``, ``vintage_risk`` true.
A consumer uses a list at decision session d only if ``available_at < 22:00 UTC of session d-1``.

Symbol rule: the stage-S as-of rule on the list date (``shortflow_common.map_symbols_asof``); a second symbol
reaching an id already mapped in the same market list keeps it only as the single exact vendor-ticker match.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import io
import json
import re
import sys
import time
from pathlib import Path
from typing import Any

from . import common as C
from . import shortflow_common as S

STAGE = "regsho_threshold"
SCHEMA = "atx.alpha-panel.regsho-threshold/v1"
RULE = "regsho-ticker-asof-listdate-v1"
CLOCK_RULE = "regsho-publication-v1"
START = dt.date(2018, 1, 2)
STALENESS_DAYS = 10
RAW_DIR = S.RAW_ROOT / "regsho_threshold"
MARKETS = ("nasdaq", "nyse", "nyse_american", "nyse_arca", "cboe_bzx", "finra_otc")
NYSE_NAMES = {"nyse": "NYSE", "nyse_american": "NYSE American", "nyse_arca": "NYSE Arca"}
NYSE_BY_CATEGORY = {v: k for k, v in NYSE_NAMES.items()}
# landed sources used by build: the combined NYSE file (market='') replaces the three per-market files
SOURCES = ("nasdaq", "nyse_combined", "cboe_bzx", "finra_otc")
NASDAQ_URL = "https://www.nasdaqtrader.com/dynamic/symdir/regsho/nasdaqth{d:%Y%m%d}.txt"
NYSE_URL = "https://www.nyse.com/api/regulatory/threshold-securities/download"
CBOE_URL = "https://cdn.cboe.com/resources/us/equities/market-statistics/reg-sho-threshold/bzx_equities_reg_sho_threshold_{d:%Y%m%d}.txt"
CBOE_HOLIDAYS_URL = "https://www-api.cboe.com/us/equities/market_statistics/reg_sho_threshold/holidays/"
FINRA_URL = "https://api.finra.org/data/group/otcMarket/name/thresholdList"
FINRA_PAGE = 5000
PLAUSIBLE_DAYS = 10
NYSE_INTERVAL_S = 4.0
MODULES = ("regsho", "shortflow_common", "common", "finra_fetch")
STAMP_RE = re.compile(r"^\d{14}$")


def ledger(market: str) -> S.Ledger:
    return S.Ledger(RAW_DIR / market / "receipts.jsonl")


# ---------------------------------------------------------------- calendar
def holidays(host: S.PoliteHost | None = None) -> set[dt.date]:
    """Exchange holidays from the Cboe list (2014-09 .. 2029), landed once as evidence."""
    path = RAW_DIR / "cboe_holidays.json"
    if not path.exists():
        host = host or S.PoliteHost()
        st, body, _ = host.request(CBOE_HOLIDAYS_URL)
        if st != 200:
            raise RuntimeError(f"cboe holidays HTTP {st}")
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(body)
    return {dt.date.fromisoformat(x) for x in json.loads(path.read_text())["dates"]}


def exchange_sessions(start: dt.date, end: dt.date, hol: set[dt.date]) -> list[dt.date]:
    out, d = [], start
    while d <= end:
        if d.weekday() < 5 and d not in hol:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


def next_session(d: dt.date, hol: set[dt.date]) -> dt.date:
    d += dt.timedelta(days=1)
    while d.weekday() >= 5 or d in hol:
        d += dt.timedelta(days=1)
    return d


# ---------------------------------------------------------------- parse
def parse_pipe(text: str) -> tuple[list[dict[str, str]], str | None, str]:
    """Pipe list -> (rows as {column: value}, trailer stamp or None, header line).

    Blank lines and a 14-digit trailer stamp are not rows. Columns are taken from the header by name
    (``Filler`` columns dropped).
    """
    lines = [ln.rstrip("\r") for ln in text.replace("\r\n", "\n").split("\n")]
    lines = [ln for ln in lines if ln.strip()]
    if not lines:
        return [], None, ""
    header = lines[0]
    cols = [c.strip() for c in header.split("|")]
    stamp = None
    rows: list[dict[str, str]] = []
    for ln in lines[1:]:
        s = ln.strip()
        if STAMP_RE.match(s):
            stamp = s
            continue
        parts = [p.strip() for p in ln.split("|")]
        row = {c: (parts[i] if i < len(parts) else "") for i, c in enumerate(cols) if c and c.lower() != "filler"}
        if row.get("Symbol"):
            rows.append(row)
    return rows, stamp, header


def normalise(market: str, row: dict[str, str]) -> dict[str, Any]:
    """Common fields of one list row."""
    if market == "finra_otc":
        # Before 2019 FINRA leaves regShoThresholdFlag / rule4320Flag blank and marks only thresholdListFlag:
        # R (SEC-reporting issuer: Reg SHO threshold) or NR (non-reporting: FINRA Rule 4320). From 2019 on the
        # flags are filled and R <-> regSho Y, NR <-> rule4320 Y (checked on the landed months).
        reg = (row.get("regShoThresholdFlag") or "").strip().upper()
        r4320 = (row.get("rule4320Flag") or "").strip().upper()
        tl = (row.get("thresholdListFlag") or "").strip().upper()
        return {"symbol": row.get("issueSymbolIdentifier", "").strip(), "security_name": row.get("issueName") or None,
                "market_category": row.get("marketCategoryDescription") or row.get("marketClassCode") or None,
                "on_list": reg == "Y" if reg else tl == "R",
                "rule_flag": (r4320 or ("Y" if tl == "NR" else "N" if tl == "R" else None))}
    flag = row.get("Reg SHO Threshold Flag")
    return {"symbol": row["Symbol"].strip(), "security_name": row.get("Security Name") or row.get("CompanyName") or None,
            "market_category": row.get("Market Category") or None,
            "on_list": True if flag is None else flag.strip().upper() == "Y",
            "rule_flag": row.get("Rule 3210") or None}


def stamp_et_to_utc(stamp: str | None) -> dt.datetime | None:
    """14-digit ``YYYYMMDDhhmmss`` in US Eastern time -> aware UTC."""
    if not stamp or not STAMP_RE.match(stamp):
        return None
    from zoneinfo import ZoneInfo

    t = dt.datetime.strptime(stamp, "%Y%m%d%H%M%S").replace(tzinfo=ZoneInfo("America/New_York"))
    return t.astimezone(dt.UTC)


def classify(market: str, status: int, rows: int, stamp: str | None, body_ok: bool) -> str:
    """``list`` / ``empty_list`` / ``empty_or_absent`` / ``absent`` for one attempted (market, date)."""
    if status != 200 or not body_ok:
        return "absent"
    if rows > 0:
        return "list"
    if (market in NYSE_NAMES or market == "nyse_combined") and stamp and stamp.endswith("0500"):
        return "empty_or_absent"      # the NYSE API's synthesized empty answer for an unpublished date
    return "empty_list"


def rule_available_at(market: str, d: dt.date, hol: set[dt.date], last_modified: dt.datetime | None) -> tuple[dt.datetime, str]:
    if market in ("nasdaq",) + tuple(NYSE_NAMES):
        base = S.at_utc(d + dt.timedelta(days=1), 6)
    else:
        base = S.at_utc(next_session(d, hol), 16)
    if market in ("nasdaq", "cboe_bzx") and last_modified is not None:
        if S.at_utc(d) <= last_modified <= S.at_utc(d + dt.timedelta(days=PLAUSIBLE_DAYS)) and last_modified > base:
            return last_modified, "last_modified_after_rule"
    return base, "rule"


# ---------------------------------------------------------------- download
def _fetch_file(host: S.PoliteHost, market: str, d: dt.date) -> tuple[int, bytes, dict[str, str], str]:
    if market == "nasdaq":
        url = NASDAQ_URL.format(d=d)
        st, body, hdr = host.request(url)
        if st == 200 and not body.lstrip().startswith(b"Symbol|"):
            st = 404  # nasdaqtrader answers a missing file with its HTML page and 200
        return st, body if st == 200 else b"", hdr, url
    if market in NYSE_NAMES or market == "nyse_combined":
        params = {"selectedDate": d.isoformat(), "market": NYSE_NAMES.get(market, "")}
        st, body, hdr = host.request(NYSE_URL, params=params)
        ok = st == 200 and body.lstrip().startswith(b"Symbol|")
        return (st if ok else (st if st != 200 else 404)), body if ok else b"", hdr, S.full_url(NYSE_URL, params)
    if market == "cboe_bzx":
        url = CBOE_URL.format(d=d)
        st, body, hdr = host.request(url)
        ok = st == 200 and body.lstrip().startswith(b"Symbol|")
        return (st if ok else (st if st != 200 else 404)), body if ok else b"", hdr, url
    raise ValueError(market)


def download(market: str, start: dt.date = START, end: dt.date | None = None, retry_absent: bool = False,
             interval: float | None = None) -> dict[str, Any]:
    # nyse.com sits behind Cloudflare, which banned the host for one hour after ~55 requests at 1.8 req/s
    nyse_host = market in NYSE_NAMES or market == "nyse_combined"
    host = S.PoliteHost(min_interval=interval or (NYSE_INTERVAL_S if nyse_host else S.HOST_MIN_INTERVAL_S))
    hol = holidays(host)
    end = end or (dt.datetime.now(dt.UTC).date() - dt.timedelta(days=1))
    if market == "finra_otc":
        return _download_finra(host, start, end, hol)
    led = ledger(market)
    have = led.latest()
    days = exchange_sessions(start, end, hol)
    todo = [d for d in days if not _done(market, have.get(d.isoformat()), retry_absent)]
    print(f"{market}: {len(days)} sessions {start}..{end}, {len(todo)} to fetch", flush=True)
    stats = {"sessions": len(days), "fetched": 0, "absent": 0}
    t0 = time.perf_counter()
    for i, d in enumerate(todo, 1):
        st, body, hdr, url = _fetch_file(host, market, d)
        rec: dict[str, Any] = {"key": d.isoformat(), "market": market, "url": url, "http_status": st,
                               "fetched_at": S.utc_now(), "last_modified": hdr.get("last-modified")}
        if st == 200:
            dest = RAW_DIR / market / f"{market}_{d:%Y%m%d}.txt.gz"
            S.F.write_gzip_atomic(dest, body)
            rec.update(file=dest.name, bytes=len(body), sha256=hashlib.sha256(body).hexdigest())
            stats["fetched"] += 1
        else:
            stats["absent"] += 1
        led.append(rec)
        if i % 200 == 0 or i == len(todo):
            print(f"  {market} {i}/{len(todo)} last={d} fetched={stats['fetched']} absent={stats['absent']} "
                  f"{time.perf_counter() - t0:.0f}s", flush=True)
    return stats


def _done(market: str, rec: dict[str, Any] | None, retry_absent: bool) -> bool:
    if rec is None:
        return False
    if rec.get("http_status") == 200:
        return bool(rec.get("file")) and (RAW_DIR / market / rec["file"]).exists()
    return rec.get("http_status") in (403, 404) and not retry_absent


def _download_finra(host: S.PoliteHost, start: dt.date, end: dt.date, hol: set[dt.date]) -> dict[str, Any]:
    """One CSV per calendar month from the Query API (tradeDate range), pages of 5,000 rows."""
    led = ledger("finra_otc")
    have = led.latest()
    months = []
    m = dt.date(start.year, start.month, 1)
    while m <= end:
        months.append(m)
        m = dt.date(m.year + (m.month == 12), 1 if m.month == 12 else m.month + 1, 1)
    stats = {"months": len(months), "fetched": 0, "rows": 0}
    for m in months:
        key = f"{m:%Y-%m}"
        m_end = min(dt.date(m.year + (m.month == 12), 1 if m.month == 12 else m.month + 1, 1) - dt.timedelta(days=1), end)
        rec = have.get(key)
        complete_month = m_end < end
        if rec and rec.get("http_status") == 200 and (RAW_DIR / "finra_otc" / rec["file"]).exists() and rec.get("complete"):
            continue
        # The API sorts only under an EQUAL filter on the partition key, so a month range is fetched as one
        # unsorted page; a month above one page falls back to one sorted, paged request per trade date.
        filt = {"compareFilters": [{"compareType": "GTE", "fieldName": "tradeDate", "fieldValue": max(m, start).isoformat()},
                                   {"compareType": "LTE", "fieldName": "tradeDate", "fieldValue": m_end.isoformat()}]}
        header, data, total = _finra_page(host, filt, 0, key)
        if total <= FINRA_PAGE:
            parts, offset = ([data] if data else []), sum(1 for r in csv.reader(io.StringIO(data)) if r)
        else:
            parts, offset = [], 0
            for d in exchange_sessions(max(m, start), m_end, hol):
                df = {"compareFilters": [{"compareType": "EQUAL", "fieldName": "tradeDate", "fieldValue": d.isoformat()}],
                      "sortFields": ["issueSymbolIdentifier"]}
                off, dtotal = 0, None
                while dtotal is None or off < dtotal:
                    h, chunk, dtotal = _finra_page(host, df, off, f"{key}/{d}")
                    n = sum(1 for r in csv.reader(io.StringIO(chunk)) if r)
                    if n == 0:
                        break
                    parts.append(chunk)
                    off += n
                offset += off
        raw = ((header or "") + "\n" + "".join(parts)).encode("utf-8")
        if offset != total:
            raise RuntimeError(f"finra thresholdList {key}: {offset} rows vs record-total {total}")
        dest = RAW_DIR / "finra_otc" / f"finra_otc_{m:%Y%m}.csv.gz"
        S.F.write_gzip_atomic(dest, raw)
        led.append({"key": key, "market": "finra_otc", "url": FINRA_URL, "filter": filt, "http_status": 200,
                    "file": dest.name, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest(), "rows": offset,
                    "fetched_at": S.utc_now(), "complete": complete_month})
        stats["fetched"] += 1
        stats["rows"] += offset
        print(f"  finra_otc {key}: {offset} rows", flush=True)
    return stats


def _finra_page(host: S.PoliteHost, filt: dict[str, Any], offset: int, key: str) -> tuple[str, str, int]:
    """(header, data lines with trailing newline, record-total) of one Query API page."""
    st, body, hdr = host.request(FINRA_URL, json_body={**filt, "limit": FINRA_PAGE, "offset": offset}, accept="text/plain")
    if st != 200:
        raise RuntimeError(f"finra thresholdList {key} offset {offset}: HTTP {st} {body[:200]!r}")
    text = body.decode("utf-8")
    nl = text.find("\n")
    if nl < 0:
        return text.rstrip("\r"), "", int(hdr.get("record-total", "0"))
    data = text[nl + 1:]
    return text[:nl].rstrip("\r"), (data if not data or data.endswith("\n") else data + "\n"), int(hdr.get("record-total", "0"))


# ---------------------------------------------------------------- build
def _rows_for_build(hol: set[dt.date], end: dt.date) -> tuple[list[tuple], list[tuple], dict[str, Any]]:
    """(list rows, per-(market, date) status rows, evidence) from every market's landed files."""
    rows: list[tuple] = []
    lists: list[tuple] = []
    evidence: dict[str, Any] = {}
    for market in SOURCES:
        led = ledger(market)
        recs = led.latest()
        ev = {"files": 0, "lm_plausible": 0, "lm_lag_hours": [], "stamp_lag_hours": []}
        if market == "finra_otc":
            by_date: dict[dt.date, list[dict[str, Any]]] = {}
            files: dict[dt.date, str] = {}
            for key, r in sorted(recs.items()):
                if r.get("http_status") != 200:
                    continue
                body = S.F.read_gzip_verified(RAW_DIR / market / r["file"], r["sha256"])
                for row in csv.DictReader(io.StringIO(body.decode("utf-8"))):
                    d = dt.date.fromisoformat(row["tradeDate"])
                    by_date.setdefault(d, []).append(normalise(market, row))
                    files[d] = r["file"]
                ev["files"] += 1
            first = min(by_date) if by_date else START
            for d in exchange_sessions(max(START, first), end, hol):
                av, basis = rule_available_at(market, d, hol, None)
                items = by_date.get(d, [])
                status = "list" if items else ("absent" if d > max(by_date, default=START) else "empty_or_absent")
                lists.append((market, d, status, len(items), sum(1 for x in items if x["on_list"]), None, None, av, basis,
                              True, files.get(d)))
                for x in items:
                    rows.append((d, market, x["symbol"], x["security_name"], x["market_category"], x["on_list"],
                                 x["rule_flag"], av, basis, True, files.get(d)))
            evidence[market] = {"files": ev["files"], "note": "Query API month extracts; no publication stamp"}
            continue
        for key, r in sorted(recs.items()):
            d = dt.date.fromisoformat(key)
            if d > end:
                continue
            lm = S.http_date(r.get("last_modified"))
            if r.get("http_status") != 200:
                for m in (NYSE_NAMES if market == "nyse_combined" else (market,)):
                    av, basis = rule_available_at(m, d, hol, None)
                    lists.append((m, d, "absent", 0, 0, None, r.get("last_modified"), av, basis, True, None))
                continue
            body = S.F.read_gzip_verified(RAW_DIR / market / r["file"], r["sha256"])
            items_raw, stamp, _hdr = parse_pipe(body.decode("utf-8", errors="replace"))
            items = [normalise(market, x) for x in items_raw]
            status = classify(market, 200, len(items), stamp, True)
            plausible = lm is not None and S.at_utc(d) <= lm <= S.at_utc(d + dt.timedelta(days=PLAUSIBLE_DAYS))
            ev["files"] += 1
            if plausible:
                ev["lm_plausible"] += 1
                ev["lm_lag_hours"].append((lm - S.at_utc(d)).total_seconds() / 3600)
            st_utc = stamp_et_to_utc(stamp)
            if st_utc is not None:
                ev["stamp_lag_hours"].append((st_utc - S.at_utc(d)).total_seconds() / 3600)
            if market == "nyse_combined":
                vr = True
                by_mkt: dict[str, list[dict[str, Any]]] = {m: [] for m in NYSE_NAMES}
                other = 0
                for x in items:
                    m = NYSE_BY_CATEGORY.get(x["market_category"] or "")
                    if m is None:
                        other += 1
                        continue
                    by_mkt[m].append(x)
                ev["rows_other_category"] = ev.get("rows_other_category", 0) + other
                for m, its in by_mkt.items():
                    av, basis = rule_available_at(m, d, hol, lm)
                    st_m = "list" if its else ("empty_list" if status == "list" else status)
                    lists.append((m, d, st_m, len(its), sum(1 for x in its if x["on_list"]), stamp,
                                  r.get("last_modified"), av, basis, vr, r["file"]))
                    for x in its:
                        rows.append((d, m, x["symbol"], x["security_name"], x["market_category"], x["on_list"],
                                     x["rule_flag"], av, basis, vr, r["file"]))
                continue
            av, basis = rule_available_at(market, d, hol, lm)
            vr = not plausible
            lists.append((market, d, status, len(items), sum(1 for x in items if x["on_list"]), stamp,
                          r.get("last_modified"), av, basis, vr, r["file"]))
            for x in items:
                rows.append((d, market, x["symbol"], x["security_name"], x["market_category"], x["on_list"],
                             x["rule_flag"], av, basis, vr, r["file"]))
        for k in ("lm_lag_hours", "stamp_lag_hours"):
            v = sorted(ev[k])
            ev[k] = ({"n": len(v), "min": round(v[0], 2), "p50": round(v[len(v) // 2], 2), "p99": round(v[int(len(v) * .99)], 2),
                      "max": round(v[-1], 2)} if v else None)
        evidence[market] = ev
    return rows, lists, evidence


def build(end: dt.date | None = None) -> dict[str, Any]:
    import pyarrow as pa
    import pyarrow.parquet as pq

    t0 = time.perf_counter()
    receipt: dict[str, Any] = {"rule": RULE, "clock_rule": CLOCK_RULE}
    hol = holidays()
    end = end or max(dt.date.fromisoformat(k) for k in ledger("nasdaq").latest())
    con = S.connect("regsho")
    with C.timed(receipt, "parse"):
        rows, lists, evidence = _rows_for_build(hol, end)
    tmp = S.tmp_dir("regsho")
    schema = pa.schema([("list_date", pa.date32()), ("market", pa.string()), ("symbol", pa.string()),
                        ("security_name", pa.string()), ("market_category", pa.string()), ("on_list", pa.bool_()),
                        ("rule_flag", pa.string()), ("available_at", pa.timestamp("us", tz="UTC")),
                        ("available_at_basis", pa.string()), ("vintage_risk", pa.bool_()), ("source_file", pa.string())])
    raw = tmp / "rows.parquet"
    cols = list(zip(*rows)) if rows else [[] for _ in schema]
    pq.write_table(pa.table([pa.array(c, t.type) for c, t in zip(cols, schema)], schema=schema), raw, compression="zstd")
    lschema = pa.schema([("market", pa.string()), ("list_date", pa.date32()), ("status", pa.string()),
                         ("rows", pa.int32()), ("rows_on_list", pa.int32()), ("file_stamp", pa.string()),
                         ("http_last_modified", pa.string()), ("available_at", pa.timestamp("us", tz="UTC")),
                         ("available_at_basis", pa.string()), ("vintage_risk", pa.bool_()), ("source_file", pa.string())])
    out = C.stage_dir(STAGE)
    lcols = list(zip(*lists))
    lt = out / "lists.parquet"
    pq.write_table(pa.table([pa.array(c, t.type) for c, t in zip(lcols, lschema)], schema=lschema),
                   lt.with_name(lt.name + ".partial"), compression="zstd")
    lt.with_name(lt.name + ".partial").replace(lt)
    sessions = S.sessions_all(con)
    vendor = S.vendor_ticker_table(con, sessions)
    mapped = tmp / "map.parquet"
    with C.timed(receipt, "map"):
        S.map_symbols_asof(con, f"SELECT list_date AS key_date, symbol FROM read_parquet('{raw.as_posix()}')", vendor, mapped)
    # run length over each market's sequence of published lists
    sql = f"""
        WITH lidx AS (
            SELECT market, list_date, row_number() OVER (PARTITION BY market ORDER BY list_date) AS li
            FROM read_parquet('{lt.as_posix()}') WHERE status IN ('list', 'empty_list')
        ),
        r AS (
            SELECT t.*, lidx.li, m.candidate_security_id AS sid0, m.outcome, m.exact, m.sid_repaired
            FROM read_parquet('{raw.as_posix()}') t
            JOIN lidx USING (market, list_date)
            LEFT JOIN read_parquet('{mapped.as_posix()}') m ON m.key_date = t.list_date AND m.symbol = t.symbol
        ),
        g AS (
            SELECT *, li - row_number() OVER (PARTITION BY market, symbol, on_list ORDER BY li) AS grp FROM r
        ),
        k AS (
            SELECT *, row_number() OVER (PARTITION BY market, symbol, on_list, grp ORDER BY li) AS run_n,
                   count(DISTINCT symbol) OVER (PARTITION BY market, list_date, sid0) AS n_sym,
                   count(DISTINCT CASE WHEN exact THEN symbol END) OVER (PARTITION BY market, list_date, sid0) AS n_exact
            FROM g
        )
        SELECT list_date, market, symbol, security_name, market_category, on_list, rule_flag,
               CAST(CASE WHEN sid0 IS NULL THEN NULL WHEN n_sym = 1 THEN sid0 WHEN exact AND n_exact = 1 THEN sid0 END AS BIGINT) AS security_id,
               available_at, available_at_basis, vintage_risk,
               CASE WHEN on_list THEN run_n END AS run_days,
               CASE WHEN sid0 IS NULL THEN coalesce(outcome, 'unmapped')
                    WHEN n_sym = 1 OR (exact AND n_exact = 1) THEN CASE WHEN sid_repaired THEN 'sid0_repaired' WHEN exact THEN 'exact' ELSE 'canonical' END
                    ELSE 'collision' END AS map_basis,
               source_file, year(list_date) AS year
        FROM k
    """
    con.execute(f"CREATE OR REPLACE TEMP TABLE th AS {sql}")
    for f in out.glob("year=*/threshold.parquet"):
        f.unlink()
    years = [r[0] for r in con.execute("SELECT DISTINCT year FROM th ORDER BY 1").fetchall()]
    for y in years:
        C.copy_to_parquet(con, f"SELECT * EXCLUDE (year) FROM th WHERE year = {y} ORDER BY list_date, market, symbol",
                          out / f"year={y}" / "threshold.parquet")
    receipt["coverage"] = coverage(con, lt.as_posix())
    receipt["mapping"] = {f"{m}|{y}": {"rows": n, "on_list": o, "mapped": mp, "mapped_share": round(mp / n, 4) if n else None}
                          for m, y, n, o, mp in con.execute("""
                              SELECT market, year, count(*), count(*) FILTER (WHERE on_list),
                                     count(*) FILTER (WHERE security_id IS NOT NULL) FROM th GROUP BY 1, 2 ORDER BY 1, 2""").fetchall()}
    receipt["map_basis"] = dict(con.execute("SELECT map_basis, count(*) FROM th GROUP BY 1").fetchall())
    receipt["rows"] = int(con.execute("SELECT count(*) FROM th").fetchone()[0])
    receipt["evidence"] = evidence
    receipt["timings_s"]["total"] = round(time.perf_counter() - t0, 1)
    sources = {}
    for m in SOURCES:
        led = ledger(m)
        if led.path.exists():
            recs = led.latest()
            sources[m] = {"ledger": str(led.path), "ledger_sha256": C.sha256_file(led.path),
                          "files": sum(1 for r in recs.values() if r.get("http_status") == 200),
                          "files_sha256": {k: r["sha256"] for k, r in sorted(recs.items()) if r.get("http_status") == 200}}
    payload = {
        "rule": RULE, "clock_rule": CLOCK_RULE,
        "rule_text": ("symbol -> security_id as of the list date: stage-S canonical-ticker rule (last session <= list "
                      "date, 7-day look-back, >= 2 ids ambiguous -> unmapped); a second symbol reaching one id in one "
                      "market list keeps it only as the single exact vendor-ticker match."),
        "clock_text": ("nasdaq/nyse*: available_at = list_date + 1 day 06:00 UTC (nasdaq: later HTTP Last-Modified "
                       "within 10 days wins); cboe_bzx/finra_otc: next session 16:00 UTC (cboe: later Last-Modified "
                       "within 10 days wins). Consumer: use at decision session d only if available_at < 22:00 UTC of d-1."),
        "staleness_rule": f"treat on_threshold_list as NaN when the latest list visible for a market is > {STALENESS_DAYS} days old",
        "staleness_days": STALENESS_DAYS,
        "window": [START.isoformat(), end.isoformat()],
        "sources": sources, "receipt": receipt,
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)
    return receipt


def coverage(con, lists: str) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for m, y, n, lst, empty, eoa, ab, rows in con.execute(f"""
            SELECT market, year(list_date), count(*), count(*) FILTER (WHERE status = 'list'),
                   count(*) FILTER (WHERE status = 'empty_list'), count(*) FILTER (WHERE status = 'empty_or_absent'),
                   count(*) FILTER (WHERE status = 'absent'), sum(rows_on_list)
            FROM read_parquet('{lists}') GROUP BY 1, 2 ORDER BY 1, 2""").fetchall():
        out.setdefault(m, {})[str(y)] = {"sessions_attempted": n, "lists_with_rows": lst, "empty_lists": empty,
                                         "empty_or_absent": eoa, "absent": ab, "rows_on_list": int(rows or 0)}
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("download")
    d.add_argument("--market", choices=MARKETS + ("nyse_all", "nyse_combined"), required=True)
    d.add_argument("--start", type=dt.date.fromisoformat, default=START)
    d.add_argument("--end", type=dt.date.fromisoformat)
    d.add_argument("--retry-absent", action="store_true")
    d.add_argument("--interval", type=float, help="seconds between requests to the host")
    b = sub.add_parser("build")
    b.add_argument("--end", type=dt.date.fromisoformat)
    args = ap.parse_args(argv)
    if args.cmd == "download":
        ms = tuple(NYSE_NAMES) if args.market == "nyse_all" else (args.market,)
        for m in ms:
            try:
                print(json.dumps({m: download(m, args.start, args.end, args.retry_absent, args.interval)}), flush=True)
            except S.RateLimited as exc:
                print(json.dumps({m: {"stopped": "rate_limited", "error": str(exc)}}), flush=True)
                return 75
    else:
        rec = build(args.end)
        print(json.dumps({k: v for k, v in rec.items() if k in ("rows", "map_basis", "timings_s", "coverage")}, default=str)[:6000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
