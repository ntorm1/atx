"""Stage ``ftd``: SEC fails-to-deliver (NSCC CNS aggregate fails) -> ``ftd/`` and ``ftd/cusip_map.parquet``.

Source: the SEC "Fails-to-Deliver Data" page
(``https://www.sec.gov/data-research/sec-markets-data/fails-deliver-data``), twice-monthly files
``cnsfailsYYYYMM{a,b}.zip`` (``a`` = first half of the month, ``b`` = second half). ``download`` lands every
file from ``FIRST_MONTH`` (2013-01: the 2013-2017 files are the look-back of the 13F CUSIP map; the delivery
window is 2018-01 onward) as served, under ``data/raw/sec_ftd/`` with an append-only receipt ledger
(url, bytes, sha256, fetched_at, HTTP Last-Modified / ETag).

Output ``ftd/year=YYYY/ftd.parquet``, one row per file row (``settlement_date, cusip, symbol``):
``settlement_date DATE, cusip, symbol, quantity DOUBLE, description, price DOUBLE, security_id BIGINT`` plus
``available_at TIMESTAMPTZ, vintage_risk, map_basis, vendor_date, vendor_date_fallback, sid_repaired,
source_file``.

Symbol rule ``ftd-ticker-asof-settlement-v1`` (the stage-S rule): the SEC symbol maps to the single ORATS id
whose vendor ticker has the same canonical form ('.', '/', '-', whitespace removed, case-sensitive) on the
last session on or before the settlement date, else on the latest earlier session within 7 calendar days
that carries it; two or more ids carrying the form that day -> ``ambiguous`` (unmapped). Several FTD rows of
one settlement date reaching one id: rows with the same symbol keep it (a CUSIP change); otherwise only the
symbol with an exact vendor-ticker match keeps it, when exactly one does (``collision`` for the rest).

Clock rule ``ftd-publication-halfmonth-v1`` (see ``LAG_DAYS``): the SEC states that the first half of a
month (``a``: settlements 1st-14th) is available at the end of the month and the second half (``b``: 15th to
month end) at about the 15th of the next month, without guarantee. ``available_at`` = max(nominal date (last
day of the month for ``a``, the 15th of the next month for ``b``) + ``LAG_DAYS`` days at 00:00 UTC, the
file's HTTP Last-Modified when it lies within [nominal - 20 d, nominal + 60 d]). Evidence: the Last-Modified
of the 141 files posted since the 2020-12 site migration lags the nominal date by -2.2 .. +5.6 days, except
6 late or re-posted files (the manifest's ``publication_evidence``). ``vintage_risk`` is true on every row:
SEC re-posts files (every 2013-2020 file carries a 2020-12 re-upload stamp) and keeps no revision history.

``cusip_map.parquet``: ``(cusip, security_id, symbol, first_seen, last_seen, n_obs)`` over mapped FTD rows,
one row per distinct triple (for the 13F stage).
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import io
import json
import re
import sys
import time
import zipfile
from pathlib import Path
from typing import Any

from . import common as C
from . import shortflow_common as S

STAGE = "ftd"
SCHEMA = "atx.alpha-panel.ftd/v1"
RULE = "ftd-ticker-asof-settlement-v1"
CLOCK_RULE = "ftd-publication-halfmonth-v1"
PAGE_URL = "https://www.sec.gov/data-research/sec-markets-data/fails-deliver-data"
FIRST_MONTH = "201301"
DELIVERY_START = dt.date(2018, 1, 1)
LAG_DAYS = 7
PLAUSIBLE_LM_DAYS = 60
STALENESS_DAYS = 60
RAW_DIR = S.RAW_ROOT / "sec_ftd"
NAME_RE = re.compile(r"cnsfails(\d{6})([ab])(?:_\d+)?\.zip$", re.IGNORECASE)
HEADER = ["SETTLEMENT DATE", "CUSIP", "SYMBOL", "QUANTITY (FAILS)", "DESCRIPTION", "PRICE"]
MODULES = ("ftd", "shortflow_common", "common", "finra_fetch")


def ledger() -> S.Ledger:
    return S.Ledger(RAW_DIR / "receipts.jsonl")


# ---------------------------------------------------------------- publication rule
def nominal_date(file_id: str) -> dt.date:
    """``YYYYMMa`` -> last day of the month; ``YYYYMMb`` -> 15th of the next month (SEC stated schedule)."""
    y, m, half = int(file_id[:4]), int(file_id[4:6]), file_id[6]
    nxt = dt.date(y + (m == 12), 1 if m == 12 else m + 1, 1)
    return nxt - dt.timedelta(days=1) if half == "a" else nxt + dt.timedelta(days=14)


def rule_available_at(file_id: str) -> dt.datetime:
    return S.at_utc(nominal_date(file_id) + dt.timedelta(days=LAG_DAYS))


def lm_plausible(file_id: str, last_modified: dt.datetime | None) -> bool:
    """A Last-Modified within [nominal - 20 d, nominal + 60 d] is taken as (possibly late) first publication."""
    if last_modified is None:
        return False
    nom = S.at_utc(nominal_date(file_id))
    return nom - dt.timedelta(days=20) <= last_modified <= nom + dt.timedelta(days=PLAUSIBLE_LM_DAYS)


def available_at(file_id: str, last_modified: dt.datetime | None = None) -> tuple[dt.datetime, str]:
    """``max(rule, plausible Last-Modified)`` and its basis (``rule`` / ``last_modified_after_rule``)."""
    rule = rule_available_at(file_id)
    if lm_plausible(file_id, last_modified) and last_modified > rule:
        return last_modified, "last_modified_after_rule"
    return rule, "rule"


def half_bounds(file_id: str) -> tuple[dt.date, dt.date]:
    """Settlement-date span of a file as observed in all 328 files: a = 1st..14th, b = 15th..month end."""
    y, m, half = int(file_id[:4]), int(file_id[4:6]), file_id[6]
    nxt = dt.date(y + (m == 12), 1 if m == 12 else m + 1, 1)
    return (dt.date(y, m, 1), dt.date(y, m, 14)) if half == "a" else (dt.date(y, m, 15), nxt - dt.timedelta(days=1))


# ---------------------------------------------------------------- download
def discover(page_html: str) -> dict[str, str]:
    """``{file_id: absolute url}`` for every ``cnsfailsYYYYMM{a,b}`` link on the page (``_0`` variants included)."""
    out: dict[str, str] = {}
    for href in re.findall(r'href="([^"]+\.zip)"', page_html):
        m = NAME_RE.search(href)
        if not m:
            continue
        fid = m.group(1) + m.group(2).lower()
        url = href if href.startswith("http") else "https://www.sec.gov" + href
        out.setdefault(fid, url)
    return out


def download(first: str = FIRST_MONTH, refetch: bool = False) -> dict[str, Any]:
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    led = ledger()
    status, body, hdr = S.sec_get(PAGE_URL, timeout=120)
    if status != 200:
        raise RuntimeError(f"FTD page HTTP {status}")
    page_sha = hashlib.sha256(body).hexdigest()
    snap = RAW_DIR / f"page_{dt.datetime.now(dt.UTC):%Y%m%dT%H%M%SZ}.html.gz"
    S.F.write_gzip_atomic(snap, body)
    led.append({"key": "page", "kind": "page", "url": PAGE_URL, "sha256": page_sha, "bytes": len(body),
                "fetched_at": S.utc_now(), "file": snap.name})
    links = {k: v for k, v in discover(body.decode("utf-8", errors="replace")).items() if k[:6] >= first}
    have = led.latest()
    todo = [k for k in sorted(links) if refetch or not _landed(have.get(k))]
    print(f"ftd download: {len(links)} files listed since {first}, {len(todo)} to fetch", flush=True)
    stats = {"listed": len(links), "fetched": 0, "absent": 0, "bytes": 0}
    for fid in todo:
        url = links[fid]
        dest = RAW_DIR / f"cnsfails{fid}.zip"
        st, data, h = S.sec_get(url, timeout=300)
        rec = {"key": fid, "kind": "file", "url": url, "http_status": st, "fetched_at": S.utc_now(),
               "last_modified": h.get("last-modified"), "etag": h.get("etag"), "content_length": h.get("content-length")}
        if st == 200:
            tmp = dest.with_name(dest.name + ".partial")
            tmp.write_bytes(data)
            tmp.replace(dest)
            rec.update(file=dest.name, bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
            stats["fetched"] += 1
            stats["bytes"] += len(data)
        else:
            stats["absent"] += 1
        led.append(rec)
    return stats


def _landed(rec: dict[str, Any] | None) -> bool:
    return bool(rec and rec.get("http_status") == 200 and rec.get("file") and (RAW_DIR / rec["file"]).exists())


# ---------------------------------------------------------------- parse
def parse_text(text: str) -> tuple[list[tuple], dict[str, Any]]:
    """Rows ``(settlement_date, cusip, symbol, quantity, description, price)`` of one FTD text file.

    The header must equal ``HEADER``. ``Trailer record count N`` / ``Trailer total quantity`` lines are
    checked, not emitted. A line with more than six fields is a description containing ``|`` (the middle
    fields are re-joined). Price ``.``/empty -> None (the SEC fills unavailable or sub-penny prices so).
    """
    st: dict[str, Any] = {"lines": 0, "rows": 0, "bad": 0, "joined_description": 0, "trailer_count": None,
                          "trailer_quantity": None, "header_ok": False, "blank": 0}
    rows: list[tuple] = []
    for i, raw in enumerate(text.splitlines()):
        st["lines"] += 1
        line = raw.rstrip("\r\n")
        if not line.strip():
            st["blank"] += 1
            continue
        if i == 0 or line.upper().startswith("SETTLEMENT DATE"):
            st["header_ok"] = [p.strip().upper() for p in line.split("|")] == HEADER
            continue
        if line.startswith("Trailer"):
            m = re.search(r"(\d+)\s*$", line)
            if m and "count" in line.lower():
                st["trailer_count"] = int(m.group(1))
            elif m and "quantity" in line.lower():
                st["trailer_quantity"] = int(m.group(1))
            continue
        parts = line.split("|")
        if len(parts) > 6:
            parts = parts[:4] + ["|".join(parts[4:-1])] + [parts[-1]]
            st["joined_description"] += 1
        if len(parts) != 6 or not re.fullmatch(r"\d{8}", parts[0].strip()):
            st["bad"] += 1
            continue
        d = parts[0].strip()
        try:
            sd = dt.date(int(d[:4]), int(d[4:6]), int(d[6:8]))
            qty = float(parts[3].strip())
        except ValueError:
            st["bad"] += 1
            continue
        p = parts[5].strip()
        try:
            price = float(p) if p not in ("", ".") else None
        except ValueError:
            price = None
        sym = parts[2].strip() or None
        rows.append((sd, parts[1].strip().upper(), sym, qty, parts[4].strip() or None, price))
    st["rows"] = len(rows)
    return rows, st


def read_zip(path: Path, sha256: str) -> str:
    blob = path.read_bytes()
    if hashlib.sha256(blob).hexdigest() != sha256:
        raise RuntimeError(f"{path}: sha256 mismatch vs receipt")
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        names = [n for n in z.namelist() if not n.endswith("/")]
        if len(names) != 1:
            raise RuntimeError(f"{path}: expected one member, got {names}")
        return z.read(names[0]).decode("latin-1")


def stage_parsed(receipt: dict[str, Any]) -> Path:
    """One tmp parquet of every landed file's rows (+ file id, file order), cached by the set of sha256s."""
    import pyarrow as pa
    import pyarrow.parquet as pq

    recs = {k: v for k, v in ledger().latest().items() if v.get("kind") == "file" and v.get("http_status") == 200}
    ident = sorted((k, v["sha256"]) for k, v in recs.items())
    dest = S.tmp_dir("ftd") / "parsed.parquet"
    stamp = dest.with_suffix(".json")
    if dest.exists() and stamp.exists() and C.read_json(stamp).get("files") == ident:
        receipt["parse"] = C.read_json(stamp)["stats"] | {"reused": True}
        return dest
    schema = pa.schema([("settlement_date", pa.date32()), ("cusip", pa.string()), ("symbol", pa.string()),
                        ("quantity", pa.float64()), ("description", pa.string()), ("price", pa.float64()),
                        ("file_id", pa.string()), ("line_no", pa.int32())])
    per_file: dict[str, Any] = {}
    tmp = dest.with_name(dest.name + ".partial")
    with pq.ParquetWriter(tmp, schema, compression="zstd") as w:
        for fid, sha in ident:
            text = read_zip(RAW_DIR / recs[fid]["file"], sha)
            rows, st = parse_text(text)
            lo, hi = half_bounds(fid)
            dates = [r[0] for r in rows]
            st["min_settlement"] = min(dates).isoformat() if dates else None
            st["max_settlement"] = max(dates).isoformat() if dates else None
            st["outside_half"] = sum(1 for d in dates if not (lo <= d <= hi))
            st["trailer_count_ok"] = st["trailer_count"] is None or st["trailer_count"] == len(rows)
            per_file[fid] = st
            if rows:
                cols = list(zip(*rows))
                arrays = [pa.array(c, t.type) for c, t in zip(cols, schema)]
                arrays += [pa.array([fid] * len(rows), pa.string()), pa.array(range(len(rows)), pa.int32())]
                w.write_table(pa.table(arrays, schema=schema))
    tmp.replace(dest)
    agg = {"files": len(per_file), "rows": sum(v["rows"] for v in per_file.values()),
           "bad_lines": sum(v["bad"] for v in per_file.values()),
           "joined_description_lines": sum(v["joined_description"] for v in per_file.values()),
           "header_not_ok": [k for k, v in per_file.items() if not v["header_ok"]],
           "trailer_count_mismatch": [k for k, v in per_file.items() if not v["trailer_count_ok"]],
           "rows_outside_nominal_half": {k: v["outside_half"] for k, v in per_file.items() if v["outside_half"]},
           "per_file": per_file}
    C.write_json_atomic(stamp, {"files": ident, "stats": agg})
    receipt["parse"] = agg
    return dest


# ---------------------------------------------------------------- build
def publication_evidence() -> dict[str, Any]:
    """Lag of each file's HTTP Last-Modified behind its nominal date, for stamps within 60 days of it."""
    out: dict[str, Any] = {}
    lags = []
    for k, v in sorted(ledger().latest().items()):
        if v.get("kind") != "file" or v.get("http_status") != 200:
            continue
        lm = S.http_date(v.get("last_modified"))
        nom = S.at_utc(nominal_date(k))
        lag = (lm - nom).total_seconds() / 86400 if lm else None
        plausible = lm_plausible(k, lm)
        av, basis = available_at(k, lm)
        out[k] = {"last_modified": v.get("last_modified"), "nominal": nominal_date(k).isoformat(),
                  "lag_days": round(lag, 3) if lag is not None else None, "plausible_first_publication": plausible,
                  "rule_available_at": rule_available_at(k).isoformat(), "available_at": av.isoformat(),
                  "available_at_basis": basis}
        if plausible:
            lags.append((lag, k))
    lags.sort()
    return {"per_file": out, "plausible_files": len(lags),
            "lag_days_max": lags[-1] if lags else None, "lag_days_min": lags[0] if lags else None,
            "lag_days_p50": lags[len(lags) // 2] if lags else None,
            "plausible_files_before_rule": sum(1 for lag, _ in lags if lag < LAG_DAYS),
            "files_last_modified_after_rule": sorted(k for k, v in out.items() if v["available_at_basis"] != "rule"),
            "lag_days_rule": LAG_DAYS}


def build() -> dict[str, Any]:
    t0 = time.perf_counter()
    receipt: dict[str, Any] = {"rule": RULE, "clock_rule": CLOCK_RULE, "tickerhistory": C.file_identity(C.TICKERHISTORY)}
    S.require_free(1.0)
    con = S.connect("ftd")
    with C.timed(receipt, "parse"):
        parsed = stage_parsed(receipt)
    sessions = S.sessions_all(con)
    with C.timed(receipt, "vendor"):
        vendor = S.vendor_ticker_table(con, sessions)
    lms = {k: S.http_date(v.get("last_modified")) for k, v in ledger().latest().items() if v.get("kind") == "file"}
    avail = [(fid, *available_at(fid, lms.get(fid)), nominal_date(fid)) for fid in
             [r[0] for r in con.execute(f"SELECT DISTINCT file_id FROM read_parquet('{parsed.as_posix()}')").fetchall()]]
    con.execute("CREATE OR REPLACE TEMP TABLE fav (file_id VARCHAR, available_at TIMESTAMPTZ, available_at_basis VARCHAR, nominal DATE)")
    con.executemany("INSERT INTO fav VALUES (?, ?, ?, ?)", avail)
    out = C.stage_dir(STAGE)
    years = [r[0] for r in con.execute(
        f"SELECT DISTINCT year(settlement_date) FROM read_parquet('{parsed.as_posix()}') ORDER BY 1").fetchall()]
    per_year: dict[str, Any] = {}
    for y in years:
        with C.timed(receipt, f"map_{y}"):
            per_year[str(y)] = _build_year(con, parsed, vendor, y, out)
        print(f"ftd {y}: {per_year[str(y)]}", flush=True)
    glob = (out / "year=*" / "ftd.parquet").as_posix()
    with C.timed(receipt, "cusip_map"):
        n_map = C.copy_to_parquet(con, f"""
            SELECT cusip, security_id, symbol, min(settlement_date) AS first_seen, max(settlement_date) AS last_seen,
                   count(*) AS n_obs
            FROM read_parquet('{glob}', hive_partitioning = false)
            WHERE security_id IS NOT NULL
            GROUP BY 1, 2, 3 ORDER BY 1, 4""", out / "cusip_map.parquet")
    cm = (out / "cusip_map.parquet").as_posix()
    cmap = con.execute(f"""
        SELECT count(DISTINCT cusip), count(DISTINCT security_id),
               (SELECT count(*) FROM (SELECT cusip FROM read_parquet('{cm}') GROUP BY 1 HAVING count(DISTINCT security_id) > 1)),
               (SELECT count(*) FROM (SELECT security_id FROM read_parquet('{cm}') GROUP BY 1 HAVING count(DISTINCT cusip) > 1))
        FROM read_parquet('{cm}')""").fetchone()
    receipt["cusip_map"] = {"rows": n_map, "cusips": cmap[0], "security_ids": cmap[1],
                            "cusips_with_several_ids": cmap[2], "ids_with_several_cusips": cmap[3]}
    receipt["per_year"] = per_year
    receipt["publication_evidence"] = publication_evidence()
    receipt["coverage_by_year"] = coverage(con, glob)
    receipt["timings_s"]["total"] = round(time.perf_counter() - t0, 1)
    led = ledger()
    recs = {k: v for k, v in led.latest().items() if v.get("kind") == "file" and v.get("http_status") == 200}
    payload = {
        "rule": RULE, "clock_rule": CLOCK_RULE,
        "rule_text": (
            "Symbol -> security_id: the ORATS securityID whose vendor ticker has the same canonical form "
            "('.', '/', '-', whitespace removed, case-sensitive) on the last session on or before the settlement "
            "date, else the latest earlier session within 7 calendar days carrying it; >= 2 ids that day -> "
            "ambiguous (unmapped). Several rows of one settlement date on one id: same symbol keeps it; else only a "
            "single exact vendor-ticker match keeps it (others 'collision'). sid-0 vendor rows repaired by "
            "sid0-bracketed-ticker-v2."),
        "clock_text": (
            f"available_at = max(nominal publication date + {LAG_DAYS} days at 00:00 UTC, the file's HTTP "
            f"Last-Modified when it lies within [nominal - 20 d, nominal + {PLAUSIBLE_LM_DAYS} d]); nominal = last "
            "day of the month (a file, settlements 1st-14th) or 15th of the next month (b file, 15th-month end), the "
            "SEC's stated schedule. vintage_risk = true on every row (files are re-posted; no revision history)."),
        "staleness_rule": (f"a consumer treats the latest FTD value as NaN when the latest published settlement "
                           f"date is more than {STALENESS_DAYS} days before the session; a security absent from a "
                           "published file had no CNS fail balance that day (all fails are included since 2008-09-16) "
                           "unless its row exists but is unmapped."),
        "staleness_days": STALENESS_DAYS,
        "delivery_window_start": DELIVERY_START.isoformat(),
        "lookback_rows_before_delivery_window": "2013-01..2017-12 rows are kept as the 13F CUSIP-map look-back",
        "sources": {"page": PAGE_URL, "ledger": str(led.path), "ledger_sha256": C.sha256_file(led.path),
                    "files": {k: {"url": v["url"], "sha256": v["sha256"], "bytes": v["bytes"],
                                  "last_modified": v.get("last_modified"), "fetched_at": v["fetched_at"]}
                              for k, v in sorted(recs.items())}},
        "receipt": receipt,
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)
    return receipt


def _build_year(con, parsed: Path, vendor: Path | str, y: int, out: Path) -> dict[str, Any]:
    pq_ = parsed.as_posix()
    tmp = S.tmp_dir("ftd")
    keys = f"SELECT settlement_date AS key_date, symbol FROM read_parquet('{pq_}') WHERE year(settlement_date) = {y}"
    mapped = tmp / f"map_{y}.parquet"
    S.map_symbols_asof(con, keys, vendor, mapped)
    # 1) the year's rows once per (settlement_date, cusip, symbol) (first file, first line), with the map outcome
    c_y = tmp / f"c_{y}.parquet"
    C.copy_to_parquet(con, f"""
        SELECT r.settlement_date, r.cusip, r.symbol, r.quantity, r.description, r.price, r.file_id, r.line_no,
               m.candidate_security_id AS sid0, m.outcome, coalesce(m.exact, false) AS exact, m.vendor_date,
               m.vendor_date_fallback, m.sid_repaired
        FROM (SELECT * FROM read_parquet('{pq_}') WHERE year(settlement_date) = {y}
              QUALIFY row_number() OVER (PARTITION BY settlement_date, cusip, symbol ORDER BY file_id, line_no) = 1) r
        LEFT JOIN read_parquet('{mapped.as_posix()}') m ON m.key_date = r.settlement_date AND m.symbol = r.symbol""", c_y)
    # 2) symbols per (settlement_date, candidate id): the collision rule
    k_y = tmp / f"k_{y}.parquet"
    C.copy_to_parquet(con, f"""
        SELECT settlement_date, sid0, count(DISTINCT symbol) AS n_sym,
               count(DISTINCT symbol) FILTER (WHERE exact) AS n_exact_sym
        FROM read_parquet('{c_y.as_posix()}') WHERE sid0 IS NOT NULL GROUP BY 1, 2""", k_y)
    dest = out / f"year={y}" / "ftd.parquet"
    sql = f"""
        SELECT c.settlement_date, c.cusip, c.symbol, c.quantity, c.description, c.price,
               CAST(CASE WHEN c.sid0 IS NULL THEN NULL WHEN k.n_sym = 1 THEN c.sid0
                         WHEN c.exact AND k.n_exact_sym = 1 THEN c.sid0 END AS BIGINT) AS security_id,
               fav.available_at, fav.available_at_basis, true AS vintage_risk,
               CASE WHEN c.symbol IS NULL THEN 'no_symbol'
                    WHEN c.sid0 IS NULL THEN coalesce(c.outcome, 'unmapped')
                    WHEN k.n_sym = 1 OR (c.exact AND k.n_exact_sym = 1) THEN CASE WHEN c.sid_repaired THEN 'sid0_repaired'
                         WHEN c.exact THEN 'exact' ELSE 'canonical' END
                    ELSE 'collision' END AS map_basis,
               c.vendor_date, c.vendor_date_fallback, c.sid_repaired,
               'cnsfails' || c.file_id || '.zip' AS source_file
        FROM read_parquet('{c_y.as_posix()}') c
        LEFT JOIN read_parquet('{k_y.as_posix()}') k ON k.settlement_date = c.settlement_date AND k.sid0 = c.sid0
        JOIN fav ON fav.file_id = c.file_id
        ORDER BY c.settlement_date, c.cusip, c.symbol
    """
    n = C.copy_to_parquet(con, sql, dest)
    d = dest.as_posix()
    dup = con.execute(f"SELECT count(*) FROM read_parquet('{pq_}') WHERE year(settlement_date) = {y}").fetchone()[0] - n
    basis = dict(con.execute(f"SELECT map_basis, count(*) FROM read_parquet('{d}') GROUP BY 1").fetchall())
    r = con.execute(f"""SELECT count(DISTINCT settlement_date), count(DISTINCT security_id),
                              sum(quantity * coalesce(price, 0)) FILTER (WHERE security_id IS NOT NULL) / nullif(sum(quantity * coalesce(price, 0)), 0),
                              count(*) FILTER (WHERE security_id IS NOT NULL), min(settlement_date), max(settlement_date)
                       FROM read_parquet('{d}')""").fetchone()
    return {"rows": n, "duplicate_rows_dropped": int(dup), "map_basis": {k: int(v) for k, v in basis.items()},
            "settlement_dates": int(r[0]), "ids": int(r[1]),
            "mapped_share_rows": round(r[3] / n, 4) if n else None,
            "mapped_share_fail_value": round(float(r[2]), 4) if r[2] is not None else None,
            "first": str(r[4]), "last": str(r[5])}


def coverage(con, glob: str) -> dict[str, Any]:
    """Per year: share of member_equity panel cells whose line had a mapped FTD row in the prior 30 days (sessions)."""
    panel = C.build_root() / "panel"
    if not any(panel.glob("year=*/*.parquet")):
        return {"note": "panel stage absent"}
    rows = con.execute(f"""
        WITH m AS (
            SELECT session_date, security_id FROM read_parquet('{(panel / 'year=*' / '*.parquet').as_posix()}', hive_partitioning = false)
            WHERE member_equity
        ),
        f AS (
            SELECT DISTINCT security_id, settlement_date FROM read_parquet('{glob}', hive_partitioning = false)
            WHERE security_id IS NOT NULL
        ),
        j AS (
            SELECT m.session_date, m.security_id, f.settlement_date
            FROM m ASOF LEFT JOIN f ON f.security_id = m.security_id AND m.session_date > f.settlement_date
        )
        SELECT year(session_date), count(*),
               count(*) FILTER (WHERE settlement_date IS NOT NULL AND date_diff('day', settlement_date, session_date) <= 30),
               count(*) FILTER (WHERE settlement_date IS NOT NULL AND date_diff('day', settlement_date, session_date) <= 365)
        FROM j GROUP BY 1 ORDER BY 1""").fetchall()
    return {str(y): {"member_equity_cells": n, "share_with_mapped_ftd_row_last_30d": round(a / n, 4),
                     "share_with_mapped_ftd_row_last_365d": round(b / n, 4)} for y, n, a, b in rows}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("download")
    d.add_argument("--first", default=FIRST_MONTH)
    d.add_argument("--refetch", action="store_true")
    sub.add_parser("build")
    sub.add_parser("evidence")
    args = ap.parse_args(argv)
    if args.cmd == "download":
        print(json.dumps(download(args.first, args.refetch)), flush=True)
    elif args.cmd == "evidence":
        ev = publication_evidence()
        print(json.dumps({k: v for k, v in ev.items() if k != "per_file"}, default=str), flush=True)
    else:
        rec = build()
        print(json.dumps({k: v for k, v in rec.items() if k not in ("parse", "per_year", "publication_evidence")}, default=str)[:4000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
