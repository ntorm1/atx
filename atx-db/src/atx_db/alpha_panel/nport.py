"""Stage ``nport`` (lane OWN, S5.2): SEC Form N-PORT data sets -> fund equity holdings, fund ownership, fund flows.

Source: the quarterly ``YYYYqN_nport.zip`` data sets linked from ``PAGE_URL`` (2019q4 on; 0.24-0.44 GB each). A data
set holds the NPORT-P reports *filed* in that calendar quarter; until the 2024 amendments take effect only the
report for the third month of each fund's fiscal quarter is public, so each fund series appears about once per
calendar quarter, at its own fiscal quarter-end (``report_date``). ``fetch`` reads only :data:`MEMBERS` of each
zip by HTTP range (central directory from the zip tail, then one ranged GET per member; the other members, mostly
derivatives and debt detail, are never downloaded), lands each range as served under ``data/raw/sec_nport/``,
inflates it (CRC-32 checked), parses the quarter, then deletes the ranges and the TSVs; one quarter is on disk at a
time. ``data/raw/sec_nport/receipts.jsonl`` keeps url, range, bytes, sha256, Last-Modified and row counts.

Parts (``nport/parts/quarter=YYYYqN/``):
* ``filings.parquet``: one row per accession: registrant CIK / name, series id / name / LEI, ``sub_type``
  (NPORT-P, NPORT-P/A), ``report_date`` (period end), ``fiscal_year_end``, ``filing_date``, total assets, net
  assets, monthly sales / reinvestment / redemption flows (Item B.6), class returns summarised as the median over
  classes of the compounded 3-month total return (``ret_q``, ``n_classes``), ``available_at``.
* ``holdings.parquet``: equity holdings only (``ASSET_CAT`` EC common, EP preferred): accession, holding id, series,
  report date, issuer name / LEI / title, CUSIP (cleaned), ISIN, ticker, balance and unit, currency, ``value_usd``,
  percent of net assets, payoff profile (Long / Short), issuer type, country, restricted flag, fair-value level.

Build outputs (``nport/``):
* ``security_map.parquet``: (quarter, cusip) -> security_id, rule ``nport-cusip-13fmap-v1``: the stage-``thirteenf``
  PIT map at the calendar quarter end that contains the report date (``cusip_map_pit``, FTD evidence public by
  P + 45 d); else the CUSIP carried by the holding's ISIN (``US``/``CA`` + CUSIP + check digit) through the same
  map; else the holding's ticker by the stage-S as-of rule on the report date (``shortflow_common.map_symbols_asof``,
  7-day look-back, ambiguous -> unmapped). ``map_basis`` says which.
* ``fund_ownership.parquet``: per (security_id, calendar quarter Q): over the fund series whose latest report with
  ``report_date`` in (Q - 3 months, Q] was filed by its deadline (report_date + 60 days, next SEC business day; the
  latest such filing per (series, report_date) wins, amendments included): ``fund_shares`` (long, unit NS),
  ``fund_shares_short``, ``fund_value_usd``, ``n_fund_series``, ``n_fund_registrants``, ``top5_share``,
  ``flow_induced_shares`` (sum over series of held shares x the series' reported quarterly net flow / prior net
  assets, the Lou 2012 flow-induced trading measure), changes vs Q - 1, and ``available_at`` = the latest
  ``available_at`` of the included filings (conservative).
* ``fund_flows.parquet``: per (series, report_date): net assets, reported net flow over the 3 months
  (sales + reinvestments - redemptions), the prior report of the series and the flow proxy ``flow_proxy`` =
  NA_t - NA_(t-1) x (1 + ret_q) (TNA change minus return), both as a share of NA_(t-1).

Clock ``nport-acceptance-v1``: ``available_at`` = the accession's resolved EDGAR acceptance from ``sec_filings``
(NPORT-P / NPORT-P/A rows); else ``filing_date`` + 1 day 00:00 America/New_York (after EDGAR's 22:00 ET close).
A consumer uses a value at session d only if ``available_at < 22:00 UTC of d-1``. The public report is filed up to
60 days after the fiscal quarter end, so holdings are 1-2 months old when they become visible. Staleness: 180 days
after the calendar quarter end.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
import shutil
import sys
import time
from pathlib import Path
from typing import Any
from urllib.parse import urljoin

from . import common as C
from . import sec_docs as D
from . import shortflow_common as S

STAGE = "nport"
SCHEMA = "atx.alpha-panel.nport/v1"
PAGE_URL = "https://www.sec.gov/data-research/sec-markets-data/form-n-port-data-sets"
RAW_DIR = S.RAW_ROOT / "sec_nport"
MEMBERS = ("SUBMISSION", "REGISTRANT", "FUND_REPORTED_INFO", "MONTHLY_TOTAL_RETURN", "FUND_REPORTED_HOLDING",
           "IDENTIFIERS")
FIRST_QUARTER = "2019q4"
CLOCK_RULE = "nport-acceptance-v1"
MAP_RULE = "nport-cusip-13fmap-v1"
DEADLINE_DAYS = 60
STALENESS_DAYS = 180
EQUITY_CATS = ("EC", "EP")
PARSE_MEMORY = "300MB"
MODULES = ("nport", "sec_docs", "shortflow_common", "common", "finra_fetch")
_Q_RE = re.compile(r"(\d{4})q([1-4])_nport\.zip$", re.IGNORECASE)


def ledger() -> S.Ledger:
    return S.Ledger(RAW_DIR / "receipts.jsonl")


def parts_dir() -> Path:
    return C.stage_dir(STAGE) / "parts"


# ---------------------------------------------------------------- pure helpers
def quarter_key(q: str) -> tuple[int, int]:
    m = re.fullmatch(r"(\d{4})q([1-4])", q.strip().lower())
    if not m:
        raise ValueError(f"bad quarter {q!r}")
    return int(m.group(1)), int(m.group(2))


def dataset_links(html: str, first: str = FIRST_QUARTER) -> dict[str, str]:
    """``{'2019q4': url, ...}`` oldest first for every ``*_nport.zip`` linked on the page."""
    out: dict[str, str] = {}
    for href in re.findall(r"href=[\"']([^\"']+_nport\.zip)[\"']", html, re.IGNORECASE):
        m = _Q_RE.search(href)
        if not m:
            continue
        q = f"{m.group(1)}q{m.group(2)}"
        if quarter_key(q) >= quarter_key(first):
            out[q] = urljoin(PAGE_URL, href)
    return dict(sorted(out.items(), key=lambda kv: quarter_key(kv[0])))


def clean_cusip(raw: str | None) -> str | None:
    """Python twin of :data:`CUSIP_SQL`: upper-case alphanumerics; 9 characters, not all zeros / all 'N'."""
    if raw is None:
        return None
    c = re.sub(r"[^0-9A-Za-z]", "", raw).upper()
    if len(c) != 9 or set(c) <= {"0"} or set(c) <= {"N"} or c in ("NA0000000",):
        return None
    return c


CUSIP_SQL = ("CASE WHEN length(regexp_replace(upper(coalesce({x}, '')), '[^0-9A-Z]', '', 'g')) = 9 "
             "AND NOT regexp_full_match(regexp_replace(upper({x}), '[^0-9A-Z]', '', 'g'), '0+|N+|NA0+') "
             "THEN regexp_replace(upper({x}), '[^0-9A-Z]', '', 'g') END")


def isin_cusip(isin: str | None) -> str | None:
    """``US0378331005`` -> ``037833100`` when the ISIN is a US/CA ISIN with a valid check digit."""
    if not isin:
        return None
    s = re.sub(r"[^0-9A-Za-z]", "", isin).upper()
    if len(s) != 12 or s[:2] not in ("US", "CA") or not isin_check_ok(s):
        return None
    return s[2:11]


def isin_check_ok(isin: str) -> bool:
    """ISIN check digit: letters -> numbers (A=10), Luhn over the digit string."""
    if len(isin) != 12 or not isin[-1].isdigit():
        return False
    digits = "".join(str(int(ch, 36)) for ch in isin[:11])
    total = 0
    for i, ch in enumerate(reversed(digits)):
        d = int(ch)
        if i % 2 == 0:
            d *= 2
            if d > 9:
                d -= 9
        total += d
    return (10 - total % 10) % 10 == int(isin[-1])


def compound(returns: list[float | None]) -> float | None:
    """Compounded total return of monthly percent returns (N-PORT reports percent); None if any month missing."""
    if not returns or any(r is None for r in returns):
        return None
    g = 1.0
    for r in returns:
        g *= 1.0 + r / 100.0
    return g - 1.0


def calendar_quarter_end(d: dt.date) -> dt.date:
    q = (d.month - 1) // 3 + 1
    return dt.date(d.year + (q == 4), 1 if q == 4 else 3 * q + 1, 1) - dt.timedelta(days=1)


def deadline(report_date: dt.date) -> dt.date:
    return S.next_sec_business_day_on_or_after(report_date + dt.timedelta(days=DEADLINE_DAYS))


# ---------------------------------------------------------------- parse one quarter
def _tsv(path: Path) -> str:
    return (f"read_csv('{path.as_posix()}', delim='\\t', header=true, all_varchar=true, quote='', escape='', "
            "strict_mode=false, null_padding=true)")


def _cols(con, path: Path) -> set[str]:
    return {r[0].upper() for r in con.execute(f"DESCRIBE SELECT * FROM {_tsv(path)}").fetchall()}


def _date(x: str) -> str:
    return (f"coalesce(try_strptime(nullif(trim({x}), ''), '%d-%b-%Y'), try_strptime(nullif(trim({x}), ''), '%Y-%m-%d'))"
            "::DATE")


def _num(x: str) -> str:
    return f"try_cast(nullif(trim({x}), '') AS DOUBLE)"


def acceptance_lookup(con) -> Path:
    """accession -> (available_at, acceptance_clock) for NPORT-P forms from ``sec_filings`` (cached per filings sha)."""
    sec = C.build_root() / "sec_filings" / "filings.parquet"
    tmp = S.tmp_dir("nport")
    dest, stamp = tmp / "acceptance_nport.parquet", tmp / "acceptance_nport.sha256"
    sha = C.sha256_file(sec)
    if dest.exists() and stamp.exists() and stamp.read_text().strip() == sha:
        return dest
    C.copy_to_parquet(con, f"""
        SELECT accession, min(available_at) AS available_at, arg_min(acceptance_clock, available_at) AS acceptance_clock
        FROM read_parquet('{sec.as_posix()}') WHERE form LIKE 'NPORT-P%' GROUP BY 1""", dest)
    stamp.write_text(sha)
    return dest


def parse_quarter(tsv: Path, quarter: str, lookup: Path) -> dict[str, Any]:
    con = C.connect(memory=PARSE_MEMORY, threads=2, temp_dir=S.tmp_dir("nport"), db_file=f"parse_{quarter}.duckdb")
    out = parts_dir() / f"quarter={quarter}"
    out.mkdir(parents=True, exist_ok=True)
    try:
        f = {m: tsv / f"{m}.tsv" for m in MEMBERS}
        cs = {m: _cols(con, p) for m, p in f.items()}

        def c(m: str, name: str) -> str:
            return f"nullif(trim({name}), '')" if name in cs[m] else "CAST(NULL AS VARCHAR)"

        con.execute(f"""CREATE TABLE sub AS SELECT trim(ACCESSION_NUMBER) AS accession, {_date('FILING_DATE')} AS filing_date,
                        upper({c('SUBMISSION', 'SUB_TYPE')}) AS sub_type, {_date(c('SUBMISSION', 'REPORT_DATE'))} AS report_date,
                        {_date(c('SUBMISSION', 'REPORT_ENDING_PERIOD'))} AS fiscal_year_end,
                        {c('SUBMISSION', 'IS_LAST_FILING')} AS is_last_filing FROM {_tsv(f['SUBMISSION'])}""")
        con.execute(f"""CREATE TABLE reg AS SELECT trim(ACCESSION_NUMBER) AS accession, try_cast(trim(CIK) AS BIGINT) AS cik,
                        {c('REGISTRANT', 'REGISTRANT_NAME')} AS registrant_name, {c('REGISTRANT', 'FILE_NUM')} AS file_num,
                        {c('REGISTRANT', 'LEI')} AS registrant_lei FROM {_tsv(f['REGISTRANT'])}""")
        flows = ", ".join(f"{_num(c('FUND_REPORTED_INFO', f'{k}_FLOW_MON{i}'))} AS {k.lower()}_m{i}"
                          for k in ("SALES", "REINVESTMENT", "REDEMPTION") for i in (1, 2, 3))
        con.execute(f"""CREATE TABLE fri AS SELECT trim(ACCESSION_NUMBER) AS accession,
                        {c('FUND_REPORTED_INFO', 'SERIES_NAME')} AS series_name, {c('FUND_REPORTED_INFO', 'SERIES_ID')} AS series_id,
                        {c('FUND_REPORTED_INFO', 'SERIES_LEI')} AS series_lei,
                        {_num(c('FUND_REPORTED_INFO', 'TOTAL_ASSETS'))} AS total_assets,
                        {_num(c('FUND_REPORTED_INFO', 'TOTAL_LIABILITIES'))} AS total_liabilities,
                        {_num(c('FUND_REPORTED_INFO', 'NET_ASSETS'))} AS net_assets, {flows}
                        FROM {_tsv(f['FUND_REPORTED_INFO'])}""")
        rets = ", ".join(_num(c('MONTHLY_TOTAL_RETURN', f'MONTHLY_TOTAL_RETURN{i}')) + f" AS r{i}" for i in (1, 2, 3))
        con.execute(f"""CREATE TABLE mtr AS
                        SELECT accession, median(ret_q) AS ret_q, count(*) AS n_classes, count(ret_q) AS n_classes_ret FROM (
                          SELECT trim(ACCESSION_NUMBER) AS accession,
                                 (1 + r1 / 100) * (1 + r2 / 100) * (1 + r3 / 100) - 1 AS ret_q
                          FROM (SELECT ACCESSION_NUMBER, {rets} FROM {_tsv(f['MONTHLY_TOTAL_RETURN'])}))
                        GROUP BY 1""")
        n_fil = C.copy_to_parquet(con, f"""
            SELECT s.accession, r.cik, r.registrant_name, r.file_num, r.registrant_lei, i.series_id, i.series_name, i.series_lei,
                   s.sub_type, s.sub_type LIKE '%/A' AS is_amendment, s.report_date, s.fiscal_year_end, s.filing_date,
                   s.is_last_filing, i.total_assets, i.total_liabilities, i.net_assets,
                   {', '.join(f'i.{k}_m{j}' for k in ('sales', 'reinvestment', 'redemption') for j in (1, 2, 3))},
                   m.ret_q, m.n_classes, m.n_classes_ret,
                   coalesce(CAST(l.available_at AS TIMESTAMPTZ),
                            CAST(timezone('America/New_York', CAST(s.filing_date AS TIMESTAMP) + INTERVAL 1 DAY) AS TIMESTAMPTZ)) AS available_at,
                   CASE WHEN l.available_at IS NOT NULL THEN 'acceptance' ELSE 'filing_date_eod_et' END AS available_basis,
                   l.acceptance_clock, '{quarter}' AS dataset_quarter
            FROM sub s LEFT JOIN reg r USING (accession) LEFT JOIN fri i USING (accession) LEFT JOIN mtr m USING (accession)
            LEFT JOIN read_parquet('{lookup.as_posix()}') l USING (accession)
            ORDER BY s.report_date, i.series_id, s.accession""", out / "filings.parquet")
        h = f["FUND_REPORTED_HOLDING"]
        hc = lambda name: c("FUND_REPORTED_HOLDING", name)  # noqa: E731
        cats = ", ".join(f"'{x}'" for x in EQUITY_CATS)
        con.execute(f"""CREATE TABLE hq AS
            SELECT trim(ACCESSION_NUMBER) AS accession, try_cast(trim(HOLDING_ID) AS BIGINT) AS holding_id,
                   {hc('ISSUER_NAME')} AS issuer_name,
                   {hc('ISSUER_LEI')} AS issuer_lei, {hc('ISSUER_TITLE')} AS issuer_title, {hc('ISSUER_CUSIP')} AS cusip_raw,
                   {CUSIP_SQL.format(x=hc('ISSUER_CUSIP'))} AS cusip,
                   {_num(hc('BALANCE'))} AS balance, upper({hc('UNIT')}) AS unit, {hc('CURRENCY_CODE')} AS currency,
                   {_num(hc('CURRENCY_VALUE'))} AS value_usd, {_num(hc('EXCHANGE_RATE'))} AS exchange_rate,
                   {_num(hc('PERCENTAGE'))} AS pct_net_assets, upper({hc('PAYOFF_PROFILE')}) AS payoff_profile,
                   upper({hc('ASSET_CAT')}) AS asset_cat, upper({hc('ISSUER_TYPE')}) AS issuer_type,
                   {hc('INVESTMENT_COUNTRY')} AS country, {hc('IS_RESTRICTED_SECURITY')} AS is_restricted,
                   {hc('FAIR_VALUE_LEVEL')} AS fair_value_level
            FROM {_tsv(h)} WHERE upper(trim(ASSET_CAT)) IN ({cats})""")
        con.execute("CHECKPOINT")
        ic = lambda name: c("IDENTIFIERS", name)  # noqa: E731
        con.execute(f"""CREATE TABLE ids AS
            SELECT try_cast(trim(HOLDING_ID) AS BIGINT) AS holding_id, max(upper({ic('IDENTIFIER_ISIN')})) AS isin,
                   max({ic('IDENTIFIER_TICKER')}) AS ticker
            FROM {_tsv(f['IDENTIFIERS'])} WHERE try_cast(trim(HOLDING_ID) AS BIGINT) IN (SELECT holding_id FROM hq) GROUP BY 1""")
        n_hold = C.copy_to_parquet(con, f"""
            SELECT hq.accession, hq.holding_id, fl.series_id, fl.cik, fl.report_date, hq.issuer_name, hq.issuer_lei,
                   hq.issuer_title, hq.cusip_raw, hq.cusip, ids.isin, ids.ticker, hq.balance, hq.unit, hq.currency,
                   hq.value_usd, hq.exchange_rate, hq.pct_net_assets, hq.payoff_profile, hq.asset_cat, hq.issuer_type,
                   hq.country, hq.is_restricted, hq.fair_value_level, fl.available_at, '{quarter}' AS dataset_quarter
            FROM hq LEFT JOIN ids USING (holding_id)
            LEFT JOIN read_parquet('{(out / 'filings.parquet').as_posix()}') fl USING (accession)
            ORDER BY fl.report_date, fl.series_id, hq.accession, hq.holding_id""", out / "holdings.parquet", row_group_size=65536)
        stats = con.execute(f"""SELECT count(*), count(cusip), count(*) FILTER (WHERE unit = 'NS'),
                                       count(*) FILTER (WHERE payoff_profile = 'SHORT'), sum(value_usd)
                                FROM read_parquet('{(out / 'holdings.parquet').as_posix()}')""").fetchone()
        raw_rows = {m: int(con.execute(f"SELECT count(*) FROM {_tsv(p)}").fetchone()[0]) for m, p in f.items()
                    if m != "FUND_REPORTED_HOLDING" and m != "IDENTIFIERS"}
        basis = dict(con.execute(f"SELECT available_basis, count(*) FROM read_parquet('{(out / 'filings.parquet').as_posix()}') "
                                 "GROUP BY 1").fetchall())
    finally:
        con.close()
    return {"filings": n_fil, "equity_holdings": n_hold, "holdings_with_cusip": int(stats[1]), "holdings_unit_ns": int(stats[2]),
            "holdings_short": int(stats[3]), "equity_value_usd": float(stats[4] or 0), "raw_rows": raw_rows,
            "available_basis": {k: int(v) for k, v in basis.items()},
            "parts": {p.name: {"bytes": p.stat().st_size, "sha256": C.sha256_file(p)} for p in sorted(out.glob("*.parquet"))}}


def _drop_scratch(quarter: str) -> None:
    for p in S.tmp_dir("nport").glob(f"parse_{quarter}.duckdb*"):
        p.unlink(missing_ok=True)


# ---------------------------------------------------------------- fetch
def fetch(limit: int | None = None, only: str | None = None) -> dict[str, Any]:
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    led = ledger()
    r = D.session().get(PAGE_URL, timeout=120)
    body = r.content
    r.close()
    import hashlib

    led.append({"key": "page", "kind": "page", "url": PAGE_URL, "http_status": r.status_code, "bytes": len(body),
                "sha256": hashlib.sha256(body).hexdigest(), "fetched_at": S.utc_now()})
    links = dataset_links(body.decode("utf-8", "replace"))
    have = led.latest()
    todo = [(q, u) for q, u in links.items() if (only is None or q == only) and not _parsed(q, have.get(q))]
    if limit is not None:
        todo = todo[:limit]
    print(f"N-PORT: {len(links)} data sets listed, {len(todo)} to do", flush=True)
    con = C.connect(memory="200MB", threads=1)
    lookup = acceptance_lookup(con)
    con.close()
    done = 0
    for q, url in todo:
        t0 = time.perf_counter()
        entries, info = D.zip_directory(url)
        by = {e["name"].rsplit("/", 1)[-1].upper(): e for e in entries}
        need = [by[f"{m}.TSV"] for m in MEMBERS]
        S.require_free(sum(e["usize"] + e["csize"] for e in need) / 1024 ** 3 + 1.0)
        tsv = S.tmp_dir("nport") / q
        tsv.mkdir(parents=True, exist_ok=True)
        members = {}
        for m, e in zip(MEMBERS, need, strict=True):
            lo, hi = D.member_range(e, info["zip_bytes"])
            raw = RAW_DIR / q / f"{m}.range"
            st, _, h = D.get_range(url, lo, hi, dest=raw)
            if st != 206:
                raise RuntimeError(f"{q} {m}: ranged GET {st}")
            n = D.inflate_range_file(raw, e, tsv / f"{m}.tsv")
            raw.unlink()
            members[m] = {"range": f"bytes={lo}-{hi}", "bytes": int(h["x-bytes"]), "sha256": h["x-sha256"],
                          "inflated_bytes": n, "crc32": f"{e['crc']:08x}"}
        shutil.rmtree(RAW_DIR / q, ignore_errors=True)
        res = parse_quarter(tsv, q, lookup)
        _drop_scratch(q)
        shutil.rmtree(tsv, ignore_errors=True)
        rec = {"key": q, "kind": "dataset", "url": url, "http_status": 206, "zip_bytes": info["zip_bytes"],
               "last_modified": info["last_modified"], "etag": info.get("etag"), "tail_sha256": info["tail_sha256"],
               "members": members, "fetched_at": S.utc_now(), "status": "parsed", "raw_deleted": True, **res,
               "elapsed_s": round(time.perf_counter() - t0, 1)}
        led.append(rec)
        done += 1
        print(f"  {q}: {res['filings']:,} filings, {res['equity_holdings']:,} equity holdings, "
              f"{sum(v['bytes'] for v in members.values()) / 1e6:.0f} MB fetched, {rec['elapsed_s']} s, free {S.free_gb():.1f} GB",
              flush=True)
    return {"listed": len(links), "done_now": done}


def _parsed(q: str, rec: dict[str, Any] | None) -> bool:
    if not rec or rec.get("status") != "parsed":
        return False
    d = parts_dir() / f"quarter={q}"
    return all((d / n).exists() and (d / n).stat().st_size == v["bytes"] for n, v in rec.get("parts", {}).items())


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--limit", type=int)
    f.add_argument("--only")
    sub.add_parser("build")
    args = ap.parse_args(argv)
    if args.cmd == "fetch":
        print(json.dumps(fetch(args.limit, args.only)), flush=True)
    else:
        rec = build()
        print(json.dumps({k: rec.get(k) for k in ("coverage_by_year", "mapping", "timings_s")}, default=str)[:6000], flush=True)
    return 0


# ---------------------------------------------------------------- build
BUILD_MEMORY = "400MB"
FLOW_GAP_DAYS = (75, 105)   # a prior report counts for the quarterly flow when 75-105 days earlier


def _isin_cusip_sql(x: str) -> str:
    """SQL twin of :func:`isin_cusip` without the check-digit test (the check digit is verified in Python on the
    distinct ISINs, :func:`build`)."""
    return f"CASE WHEN regexp_full_match(upper({x}), '(US|CA)[0-9A-Z]{{9}}[0-9]') THEN substr(upper({x}), 3, 9) END"


def _effective_tables(con) -> None:
    """From table ``fil`` (one row per accession, with ``fund_key``) build ``dl`` (report_date -> deadline, calendar
    quarter), ``eff`` (per (fund, report_date) the latest filing, amendments included, filed by the deadline) and
    ``effq`` (per (fund, calendar quarter) the latest report date inside the quarter)."""
    rds = [r[0] for r in con.execute("SELECT DISTINCT report_date FROM fil WHERE report_date IS NOT NULL ORDER BY 1").fetchall()]
    con.execute("CREATE OR REPLACE TABLE dl (report_date DATE, deadline DATE, q DATE)")
    con.executemany("INSERT INTO dl VALUES (?, ?, ?)", [(d, deadline(d), calendar_quarter_end(d)) for d in rds])
    con.execute("""CREATE OR REPLACE TABLE eff AS
        SELECT * EXCLUDE (rk) FROM (
            SELECT f.*, dl.deadline, dl.q,
                   row_number() OVER (PARTITION BY f.fund_key, f.report_date ORDER BY f.filing_date DESC, f.accession DESC) AS rk
            FROM fil f JOIN dl USING (report_date) WHERE f.filing_date <= dl.deadline) WHERE rk = 1""")
    con.execute("""CREATE OR REPLACE TABLE effq AS
        SELECT * EXCLUDE (rk) FROM (
            SELECT e.*, row_number() OVER (PARTITION BY fund_key, q ORDER BY report_date DESC) AS rk FROM eff e) WHERE rk = 1""")


_FLOW = " + ".join(f"coalesce(sales_m{i}, 0) + coalesce(reinvestment_m{i}, 0) - coalesce(redemption_m{i}, 0)" for i in (1, 2, 3))
_GAP_OK = f"date_diff('day', prev_report_date, report_date) BETWEEN {FLOW_GAP_DAYS[0]} AND {FLOW_GAP_DAYS[1]} AND prev_net_assets > 0"
FLOWS_SQL = f"""
    WITH e AS (
        SELECT fund_key, series_id, cik, registrant_name, series_name, report_date, q, accession, filing_date, available_at,
               net_assets, total_assets, ret_q, n_classes, ({_FLOW}) AS net_flow_reported,
               lag(report_date) OVER w AS prev_report_date, lag(net_assets) OVER w AS prev_net_assets,
               lag(available_at) OVER w AS prev_available_at
        FROM eff WINDOW w AS (PARTITION BY fund_key ORDER BY report_date))
    SELECT *, CASE WHEN {_GAP_OK} THEN net_flow_reported / prev_net_assets END AS flow_pct_reported,
           CASE WHEN {_GAP_OK} AND ret_q IS NOT NULL THEN net_assets - prev_net_assets * (1 + ret_q) END AS flow_proxy,
           CASE WHEN {_GAP_OK} AND ret_q IS NOT NULL
                THEN (net_assets - prev_net_assets * (1 + ret_q)) / prev_net_assets END AS flow_proxy_pct,
           greatest(available_at, prev_available_at) AS flow_available_at
    FROM e ORDER BY report_date, fund_key"""


def aggregate_sql(q: dt.date) -> str:
    """Per security over table ``hq`` (one calendar quarter's effective equity holdings with ``security_id``,
    ``fund_key``, ``cik``, ``balance``, ``value_usd``, ``unit``, ``payoff_profile``, ``flow_pct_reported``,
    ``available_at``): long NS shares / value, short NS shares, series and registrant counts, top-5 series share,
    flow-induced shares (Lou 2012), latest clock."""
    return f"""
        WITH l AS (
            SELECT security_id, fund_key, any_value(cik) AS cik, sum(balance) AS shares, sum(value_usd) AS value_usd,
                   any_value(flow_pct_reported) AS flow_pct, max(available_at) AS available_at
            FROM hq WHERE security_id IS NOT NULL AND unit = 'NS' AND payoff_profile = 'LONG' AND balance > 0
            GROUP BY 1, 2),
        s AS (SELECT security_id, sum(balance) AS shares_short FROM hq
              WHERE security_id IS NOT NULL AND unit = 'NS' AND payoff_profile = 'SHORT' GROUP BY 1),
        r AS (SELECT *, row_number() OVER (PARTITION BY security_id ORDER BY shares DESC, fund_key) AS rk FROM l)
        SELECT DATE '{q.isoformat()}' AS period_q, r.security_id, sum(shares) AS fund_shares,
               any_value(s.shares_short) AS fund_shares_short, sum(value_usd) AS fund_value_usd,
               count(*) AS n_fund_series, count(DISTINCT cik) AS n_fund_registrants,
               sum(shares) FILTER (WHERE rk <= 5) / sum(shares) AS top5_share,
               sum(shares * flow_pct) AS flow_induced_shares, count(flow_pct) AS n_series_with_flow,
               max(available_at) AS available_at
        FROM r LEFT JOIN s USING (security_id) GROUP BY r.security_id ORDER BY r.security_id"""


def changes_sql(raw: str) -> str:
    """Quarter-on-quarter changes per security (only against the immediately preceding calendar quarter)."""
    adj = "prev_q = last_day(period_q - INTERVAL 3 MONTH)"
    return f"""
        WITH p AS (SELECT a.*, lag(period_q) OVER w AS prev_q, lag(fund_shares) OVER w AS prev_shares,
                          lag(n_fund_series) OVER w AS prev_n, lag(available_at) OVER w AS prev_av
                   FROM read_parquet('{raw}') a WINDOW w AS (PARTITION BY security_id ORDER BY period_q))
        SELECT period_q, security_id, fund_shares, fund_shares_short, fund_value_usd, n_fund_series, n_fund_registrants,
               top5_share, flow_induced_shares, n_series_with_flow, available_at,
               CASE WHEN {adj} THEN fund_shares - prev_shares END AS d_fund_shares,
               CASE WHEN {adj} AND prev_shares > 0 THEN fund_shares / prev_shares - 1 END AS pct_fund_shares,
               CASE WHEN {adj} THEN n_fund_series - prev_n END AS d_n_fund_series,
               CASE WHEN {adj} THEN greatest(available_at, prev_av) END AS chg_available_at
        FROM p ORDER BY period_q, security_id"""


def build() -> dict[str, Any]:
    """Security map, PIT fund ownership per (security, calendar quarter), fund flows; coverage; manifest."""
    import pyarrow.parquet as pq

    t0 = time.perf_counter()
    root = C.build_root()
    out = C.stage_dir(STAGE)
    tmp = S.tmp_dir("nport_build")
    con = C.connect(memory=BUILD_MEMORY, threads=2, temp_dir=tmp, db_file="nport_build.duckdb")
    receipt: dict[str, Any] = {"map_rule": MAP_RULE, "clock_rule": CLOCK_RULE}
    fglob = (parts_dir() / "quarter=*" / "filings.parquet").as_posix()
    hglob = (parts_dir() / "quarter=*" / "holdings.parquet").as_posix()
    con.execute(f"""CREATE TABLE fil AS
        SELECT *, coalesce(series_id, 'CIK' || CAST(cik AS VARCHAR)) AS fund_key
        FROM read_parquet('{fglob}', hive_partitioning = false) WHERE report_date IS NOT NULL""")
    _effective_tables(con)
    n_flows = C.copy_to_parquet(con, FLOWS_SQL, out / "fund_flows.parquet")
    # security map: 13F PIT map at the calendar quarter end, via CUSIP then ISIN, then ticker as of the report date
    cmap = (root / "thirteenf" / "cusip_map_pit.parquet").as_posix()
    con.execute(f"""CREATE TABLE keys AS
        SELECT DISTINCT e.q, h.report_date, h.cusip, upper(h.isin) AS isin, h.ticker
        FROM read_parquet('{hglob}', hive_partitioning = false) h JOIN effq e USING (accession)
        WHERE h.asset_cat = 'EC'""")
    isins = [r[0] for r in con.execute("SELECT DISTINCT isin FROM keys WHERE isin IS NOT NULL").fetchall()]
    con.execute("CREATE TABLE isin_ok (isin VARCHAR, isin_cusip VARCHAR)")
    con.executemany("INSERT INTO isin_ok VALUES (?, ?)", [(i, isin_cusip(i)) for i in isins if isin_cusip(i)])
    con.execute(f"""CREATE TABLE m1 AS
        SELECT k.*, i.isin_cusip, c1.security_id AS sid_cusip, c2.security_id AS sid_isin
        FROM keys k LEFT JOIN isin_ok i USING (isin)
        LEFT JOIN read_parquet('{cmap}') c1 ON c1.period_q = k.q AND c1.cusip = k.cusip
        LEFT JOIN read_parquet('{cmap}') c2 ON c2.period_q = k.q AND c2.cusip = i.isin_cusip""")
    sessions = S.sessions_all(con)
    vendor = S.vendor_ticker_table(con, sessions)
    tick = tmp / "ticker_map.parquet"
    S.map_symbols_asof(con, """SELECT DISTINCT report_date AS key_date, ticker AS symbol FROM m1
                               WHERE sid_cusip IS NULL AND sid_isin IS NULL AND ticker IS NOT NULL
                                 AND regexp_full_match(ticker, '[A-Za-z][A-Za-z0-9.\\-/ ]{0,9}')""", vendor, tick)
    C.copy_to_parquet(con, f"""
        SELECT m1.q, m1.report_date, m1.cusip, m1.isin, m1.ticker,
               CAST(coalesce(m1.sid_cusip, m1.sid_isin, t.candidate_security_id) AS BIGINT) AS security_id,
               CASE WHEN m1.sid_cusip IS NOT NULL THEN '13f_map_cusip' WHEN m1.sid_isin IS NOT NULL THEN '13f_map_isin'
                    WHEN t.candidate_security_id IS NOT NULL THEN 'ticker_asof_report_date' ELSE 'unmapped' END AS map_basis
        FROM m1 LEFT JOIN read_parquet('{tick.as_posix()}') t ON t.key_date = m1.report_date AND t.symbol = m1.ticker""",
                      out / "security_map.parquet")
    smap = (out / "security_map.parquet").as_posix()
    # aggregates per (security, quarter)
    qs = [r[0] for r in con.execute("SELECT DISTINCT q FROM effq ORDER BY 1").fetchall()]
    ff = (out / "fund_flows.parquet").as_posix()
    part = out / "fund_ownership.parquet.partial"
    writer = None
    per_q: dict[str, Any] = {}
    try:
        for q in qs:
            dqs = [r[0] for r in con.execute("SELECT DISTINCT dataset_quarter FROM effq WHERE q = ?", [q]).fetchall()]
            files = ", ".join(f"'{(parts_dir() / f'quarter={d}' / 'holdings.parquet').as_posix()}'" for d in sorted(dqs))
            con.execute(f"""CREATE OR REPLACE TABLE hq AS
                SELECT e.fund_key, e.cik, e.available_at, e.accession, h.report_date, h.balance, h.value_usd, h.payoff_profile,
                       h.unit, s.security_id, s.map_basis, f.flow_pct_reported
                FROM (SELECT * FROM effq WHERE q = DATE '{q}') e
                JOIN read_parquet([{files}], hive_partitioning = false) h USING (accession)
                LEFT JOIN read_parquet('{smap}') s ON s.q = e.q AND s.report_date = h.report_date
                     AND s.cusip IS NOT DISTINCT FROM h.cusip AND s.isin IS NOT DISTINCT FROM upper(h.isin)
                     AND s.ticker IS NOT DISTINCT FROM h.ticker
                LEFT JOIN read_parquet('{ff}') f ON f.fund_key = e.fund_key AND f.report_date = e.report_date
                WHERE h.asset_cat = 'EC'""")
            tbl = con.execute(aggregate_sql(q)).to_arrow_table()
            if writer is None:
                writer = pq.ParquetWriter(part, tbl.schema, compression="zstd")
            writer.write_table(tbl)
            cov = con.execute("""
                SELECT count(*), count(security_id), sum(value_usd), sum(value_usd) FILTER (WHERE security_id IS NOT NULL),
                       count(DISTINCT fund_key) FROM hq""").fetchone()
            per_q[q.isoformat()] = {"equity_rows": cov[0], "rows_mapped": cov[1], "securities": tbl.num_rows,
                                    "value_mapped_share": round(cov[3] / cov[2], 4) if cov[2] else None, "fund_series": cov[4]}
            print(f"nport {q}: {per_q[q.isoformat()]}", flush=True)
    finally:
        if writer is not None:
            writer.close()
    raw = out / "fund_ownership_raw.parquet"
    part.replace(raw)
    n_own = C.copy_to_parquet(con, changes_sql(raw.as_posix()), out / "fund_ownership.parquet")
    raw.unlink()
    receipt["per_quarter"] = per_q
    receipt["rows"] = {"fund_ownership": n_own, "fund_flows": n_flows,
                       "security_map": con.execute(f"SELECT count(*) FROM read_parquet('{smap}')").fetchone()[0]}
    receipt["mapping"] = dict(con.execute(f"SELECT map_basis, count(*) FROM read_parquet('{smap}') GROUP BY 1").fetchall())
    receipt["coverage_by_year"] = coverage(con, out / "fund_ownership.parquet")
    receipt["timings_s"] = {"total": round(time.perf_counter() - t0, 1)}
    con.close()
    for f in tmp.glob("nport_build.duckdb*"):
        f.unlink(missing_ok=True)
    publish(receipt)
    return receipt


def coverage(con, agg: Path) -> dict[str, Any]:
    """Per year: share of panel member_equity cells with a visible N-PORT aggregate (available_at < 22:00 UTC of the
    previous session) whose quarter end is at most ``STALENESS_DAYS`` old."""
    panel = C.build_root() / "panel"
    cal = C.calendar_path().as_posix()
    rows = con.execute(f"""
        WITH s AS (SELECT session_date, lag(session_date) OVER (ORDER BY session_date) AS prev FROM read_parquet('{cal}')),
        m AS (
            SELECT p.session_date, p.security_id, (CAST(s.prev AS TIMESTAMP) + INTERVAL 22 HOUR) AT TIME ZONE 'UTC' AS cutoff
            FROM read_parquet('{(panel / 'year=*' / '*.parquet').as_posix()}', hive_partitioning = false) p
            JOIN s USING (session_date) WHERE p.member_equity AND s.prev IS NOT NULL AND p.session_date >= DATE '2020-01-01'),
        a AS (SELECT security_id, available_at, period_q FROM read_parquet('{agg.as_posix()}') WHERE available_at IS NOT NULL),
        j AS (SELECT m.session_date, a.period_q FROM m ASOF LEFT JOIN a ON a.security_id = m.security_id AND m.cutoff > a.available_at)
        SELECT year(session_date), count(*),
               count(*) FILTER (WHERE period_q IS NOT NULL AND date_diff('day', period_q, session_date) <= {STALENESS_DAYS}),
               count(*) FILTER (WHERE period_q IS NOT NULL)
        FROM j GROUP BY 1 ORDER BY 1""").fetchall()
    return {str(y): {"member_equity_cells": n, "share_with_fresh_fund_ownership": round(k / n, 4),
                     "share_with_any_visible": round(v / n, 4)} for y, n, k, v in rows}


def publish(receipt: dict[str, Any]) -> None:
    root = C.build_root()
    led = ledger()
    recs = {k: v for k, v in led.latest().items() if v.get("kind") == "dataset" and v.get("status") == "parsed"}
    payload = {
        "rules": {"clock": CLOCK_RULE + ": acceptance from sec_filings, else filing_date + 1 day 00:00 ET",
                  "effective": ("per (fund series, report_date) the latest filing (amendments included) with filing_date <= "
                                "report_date + 60 d rolled to the next SEC business day; per (series, calendar quarter) the "
                                "latest report date in the quarter"),
                  "map": MAP_RULE + ": 13F PIT CUSIP map at the calendar quarter end via CUSIP, then US/CA ISIN -> CUSIP, "
                                    "then ticker as of the report date (stage-S rule)",
                  "aggregate": ("EC (common equity) rows, unit NS, payoff LONG, balance > 0; available_at = latest clock of the "
                                "included filings; flow_induced_shares = sum(shares x reported quarterly flow / prior NA)"),
                  "flows": ("net_flow_reported = sum over 3 months of sales + reinvestments - redemptions (Item B.6); "
                            "flow_proxy = NA_t - NA_(t-1) x (1 + ret_q) when the prior report is 75-105 days earlier; "
                            "ret_q = median over share classes of the compounded monthly total returns")},
        "staleness_rule": f"a consumer treats fund ownership as NaN more than {STALENESS_DAYS} days after period_q",
        "staleness_days": STALENESS_DAYS,
        "inputs": {"input_manifests_sha256": {k: C.sha256_file(root / k / "manifest.json")
                                              for k in ("sec_filings", "thirteenf", "panel") if (root / k / "manifest.json").exists()}},
        "sources": {"page": PAGE_URL, "ledger": str(led.path), "ledger_sha256": C.sha256_file(led.path),
                    "datasets": {k: {"url": v["url"], "zip_bytes": v["zip_bytes"], "last_modified": v["last_modified"],
                                     "members": v["members"], "fetched_at": v["fetched_at"], "raw_deleted": v.get("raw_deleted")}
                                 for k, v in sorted(recs.items())}},
        "receipt": receipt,
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)


if __name__ == "__main__":
    sys.exit(main())
