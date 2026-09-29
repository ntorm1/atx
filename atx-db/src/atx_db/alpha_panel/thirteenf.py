"""Stage ``thirteenf``: SEC Form 13F data sets -> holdings, filings, filers, PIT CUSIP map and aggregates.

Source: every ``*_form13f.zip`` linked from the SEC "Form 13F Data Sets" page
(``https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets``): quarterly filing-date data sets
``2013q2`` .. ``2023q4`` and three-month ranges from ``01jan2024-29feb2024``. ``fetch`` handles one zip at a
time: download (SEC user agent and host-wide 5 req/s limiter), sha256, parse SUBMISSION / COVERPAGE /
SUMMARYPAGE / INFOTABLE into Parquet parts, then delete the zip and the extracted TSVs. Only the Parquet parts
and one receipt line per zip (url, bytes, sha256, fetched_at, Last-Modified, member sizes, row counts) are
kept (``data/raw/sec_13f/receipts.jsonl``).

Parts, one directory per data set (``parts/source=<period>/``):
* ``filings.parquet``: one row per accession: ``accession, filer_cik, period_of_report, period_q (quarter end),
  filing_date, available_at, clock_basis, submission_type, is_amendment, amendment_no, amendment_type,
  report_type, filing_manager_name, form13f_file_number, crd_number, sec_file_number,
  other_included_managers_count, table_entry_total, table_value_total_raw, is_confidential_omitted,
  n_infotable_rows, source_period``.
* ``holdings.parquet``: one row per INFOTABLE row with the filing fields above (``accession, infotable_sk,
  filer_cik, period_of_report, period_q, filing_date, available_at, submission_type, amendment_type, cusip,
  cusip_raw, cusip_repaired, figi, nameofissuer, titleofclass, shares`` (SSHPRNAMT when SH), ``prn_amount``
  (when PRN), ``sshprnamt_type, value_raw, value_usd, value_unit, put_call, discretion, other_manager,
  voting_sole, voting_shared, voting_none, source_period``). Originals and amendments are separate rows
  (separate vintages); nothing is overwritten.

Rules:
* Clock ``13f-filed-plus-46h-v1``: the data sets carry the EDGAR filing date only (no acceptance time), so
  ``available_at = filing_date 00:00 UTC + 46 h`` (the house FC1 floor also used for fundamentals). EDGAR
  gives a 13F submitted after 17:30 ET the next business day's date, so the acceptance precedes the end of
  the filing date; the floor adds a further day.
* Value unit ``13f-value-unit-v1``: the SEC readme: "Starting on January 3, 2023, market value is reported
  rounded to the nearest dollar. Previously, market value was reported in thousands." ``value_usd =
  VALUE * 1000`` for filing dates before 2023-01-03, else ``VALUE`` (a filing-date rule, so Q4-2022 reports
  filed in 2023 are in dollars). ``build`` audits each filing's implied prices against the cross-filer median
  per CUSIP and flags ``value_unit_check`` (``ok`` / ``ratio_high`` / ``ratio_low`` / ``insufficient``).
* CUSIP: upper-cased, non-alphanumerics removed; an all-digit 7-8 character value is left-padded with zeros
  (spreadsheet-dropped leading zeros; ``cusip_repaired``).
* PIT CUSIP map ``13f-cusip-ftd-window-v1`` (``cusip_map_pit.parquet``): at quarter end P a CUSIP maps to a
  security_id only through mapped FTD rows (stage ``ftd``) with settlement date >= P - 730 days whose FTD
  ``available_at`` is before P + 45 days (00:00 UTC), i.e. FTD information public by the 13F deadline. One id
  -> ``unique``; several ids -> the id with the latest settlement date (``latest``), unless tied
  (``ambiguous``, unmapped). Compromise (disclosed): a holding filed before P + 45 days may be mapped with FTD
  files published after its filing but before P + 45 days (identity information only).
* Effective holdings per (filer, quarter) as of a cutoff: the latest original (13F-HR) or RESTATEMENT
  amendment filed by the cutoff is the base; NEW HOLDINGS amendments filed after the base are added.
  Versions: ``final`` (no cutoff: every filing in the data sets) and ``asof45`` (filing date on or before the
  first SEC business day on or after P + 45 days, the statutory deadline).
* Aggregates per ``(version, security_id, period_q)`` (``agg_final.parquet``, ``agg_asof45.parquet``) over SH
  rows without put/call and shares > 0: ``inst_shares``, ``n_holders`` (distinct filer CIKs), ``top10_share``
  (the 10 largest holders' shares over ``inst_shares``), ``inst_value_usd``, changes vs the prior quarter
  (``d_inst_shares, pct_inst_shares, d_n_holders``; NULL when the prior quarter has no row), and
  ``available_at`` = the latest ``available_at`` among the version's filings of every filer that reported the
  security in any of its filings for that quarter (the latest filing needed; conservative).
* Row sanity ``13f-row-sanity-v1`` (per quarter, over every filing): the consensus implied price of a CUSIP is
  the median of ``value_usd / shares`` over SH rows without put/call (>= 5 rows). A filing whose median ratio to
  the consensus lies in [300, 3000] (filed before 2023-01-03: value reported in dollars) or in [1/3000, 1/300]
  (filed from 2023-01-03: value still in thousands) gets ``unit_factor`` 0.001 / 1000 (``filing_checks.parquet``).
  After that repair a row is a ``price_outlier`` when its ratio is above 20 or below 1/20, when its value exceeds
  $1 trillion, or when its implied price exceeds $2,000,000 per share; outliers are excluded from every
  aggregate (their count and shares are reported per security and quarter).
* ``filers.parquet``: per (filer CIK, quarter) of the ``final`` version: name, submission types, report type,
  entry count, total value, file numbers. Filer type (hedge fund / mutual fund / other) is NOT classified:
  see docs/ALPHA_PANEL_SHORTFLOW.md.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import shutil
import sys
import time
import zipfile
from pathlib import Path
from typing import Any
from urllib.parse import urljoin

from . import common as C
from . import shortflow_common as S

STAGE = "thirteenf"
SCHEMA = "atx.alpha-panel.thirteenf/v1"
PAGE_URL = "https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets"
RAW_DIR = S.RAW_ROOT / "sec_13f"
CLOCK_RULE = "13f-filed-plus-46h-v1"
VALUE_RULE = "13f-value-unit-v1"
MAP_RULE = "13f-cusip-ftd-window-v1"
VALUE_DOLLARS_FROM = dt.date(2023, 1, 3)
MAP_LOOKBACK_DAYS = 730
DEADLINE_DAYS = 45
STALENESS_DAYS = 150
CONSENSUS_MIN_ROWS = 5
PRICE_OUTLIER_X = 20
IMPLAUSIBLE_VALUE_USD = 1e12   # no single 13F line is worth $1 trillion
IMPLAUSIBLE_PRICE_USD = 2e6    # above any US share price (BRK.A < $1M)
BUILD_MEMORY = "350MB"
BUILD_THREADS = 2
REQUIRED = ("SUBMISSION.tsv", "COVERPAGE.tsv", "SUMMARYPAGE.tsv", "INFOTABLE.tsv")
MODULES = ("thirteenf", "shortflow_common", "common", "finra_fetch")
_MONTHS = {m: i for i, m in enumerate(("jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"), 1)}
_RANGE_RE = re.compile(r"(\d{2})([a-z]{3})(\d{4})-(\d{2})([a-z]{3})(\d{4})_form13f\.zip$", re.IGNORECASE)
_QUARTER_RE = re.compile(r"(\d{4})q([1-4])_form13f\.zip$", re.IGNORECASE)


def ledger() -> S.Ledger:
    return S.Ledger(RAW_DIR / "receipts.jsonl")


def parts_dir() -> Path:
    return C.stage_dir(STAGE) / "parts"


# ---------------------------------------------------------------- pure helpers
def archive_period(url: str) -> tuple[str, dt.date, dt.date]:
    """``(source_period, first filing date, last filing date)`` from a data-set file name."""
    name = url.rsplit("/", 1)[-1]
    m = _RANGE_RE.search(name)
    if m:
        a = dt.date(int(m.group(3)), _MONTHS[m.group(2).lower()], int(m.group(1)))
        b = dt.date(int(m.group(6)), _MONTHS[m.group(5).lower()], int(m.group(4)))
        return name[: -len("_form13f.zip")], a, b
    m = _QUARTER_RE.search(name)
    if m:
        y, q = int(m.group(1)), int(m.group(2))
        a = dt.date(y, 3 * q - 2, 1)
        b = dt.date(y + (q == 4), 1 if q == 4 else 3 * q + 1, 1) - dt.timedelta(days=1)
        return name[: -len("_form13f.zip")], a, b
    raise ValueError(f"unrecognised 13F data set name {name!r}")


def discover(page_html: str) -> list[tuple[str, str, dt.date, dt.date]]:
    """``[(source_period, url, first, last)]`` oldest first."""
    out = {}
    for href in re.findall(r"href=[\"']([^\"']+?_form13f\.zip)[\"']", page_html, re.IGNORECASE):
        url = urljoin(PAGE_URL, href)
        sp, a, b = archive_period(url)
        out[sp] = (sp, url, a, b)
    return sorted(out.values(), key=lambda r: (r[2], r[3]))


def value_usd(value_raw: float | None, filing_date: dt.date) -> float | None:
    """Python twin of the SQL unit rule (tested): thousands before 2023-01-03, dollars from then on."""
    if value_raw is None:
        return None
    return value_raw * 1000.0 if filing_date < VALUE_DOLLARS_FROM else value_raw


def normalise_cusip(raw: str | None) -> tuple[str | None, bool]:
    """Python twin of the SQL CUSIP rule: ``(cusip, repaired)``."""
    if raw is None:
        return None, False
    c = re.sub(r"[^0-9A-Za-z]", "", raw.strip()).upper()
    if not c:
        return None, False
    if c.isdigit() and len(c) in (7, 8):
        return c.zfill(9), True
    return c, False


def quarter_end(d: dt.date) -> dt.date:
    q = (d.month - 1) // 3 + 1
    return dt.date(d.year + (q == 4), 1 if q == 4 else 3 * q + 1, 1) - dt.timedelta(days=1)


def prior_quarter_end(p: dt.date) -> dt.date:
    return dt.date(p.year, p.month - 2, 1) - dt.timedelta(days=1) if p.month > 3 else dt.date(p.year - 1, 12, 31)


def deadline45(p: dt.date) -> dt.date:
    """Statutory 13F deadline: 45 days after the quarter end, rolled to the next SEC business day."""
    return S.next_sec_business_day_on_or_after(p + dt.timedelta(days=DEADLINE_DAYS))


def effective_accessions(filings: list[tuple[str, dt.date, str, str | None, str | None]]) -> list[str]:
    """Effective accessions of one (filer, quarter): ``[(accession, filing_date, submission_type,
    is_amendment, amendment_type)]`` -> accessions whose holdings form the filer's report.

    Base = the latest (by filing date, then accession) 13F-HR original or RESTATEMENT amendment; NEW HOLDINGS
    amendments filed after the base are added. Without a base, every NEW HOLDINGS amendment counts. Notices
    (13F-NT) never count. Python twin of the SQL in :func:`_effective_sql` (tested).
    """
    hr = sorted((f for f in filings if f[2] in ("13F-HR", "13F-HR/A")), key=lambda f: (f[1], f[0]))
    base_i = -1
    for i, f in enumerate(hr):
        if _is_base(f[2], f[4]):
            base_i = i
    out = [hr[base_i][0]] if base_i >= 0 else []
    out += [f[0] for i, f in enumerate(hr) if i > base_i and not _is_base(f[2], f[4])]
    return out


def _is_base(submission_type: str, amendment_type: str | None) -> bool:
    return submission_type == "13F-HR" or (amendment_type or "").upper() == "RESTATEMENT"


# ---------------------------------------------------------------- fetch + parse
def _tsv(path: Path) -> str:
    return (f"read_csv('{path.as_posix()}', delim='\\t', header=true, all_varchar=true, quote='', escape='', "
            f"strict_mode=false, null_padding=true)")


def _d(col: str) -> str:
    return f"try_strptime(nullif(trim({col}), ''), '%d-%b-%Y')::DATE"


def _extract(zpath: Path, dest: Path) -> dict[str, Any]:
    dest.mkdir(parents=True, exist_ok=True)
    members: dict[str, Any] = {}
    with zipfile.ZipFile(zpath) as z:
        by_base = {}
        for n in z.namelist():
            b = Path(n.replace("\\", "/")).name.upper()
            by_base.setdefault(b, n)
        for req in REQUIRED:
            name = by_base.get(req.upper())
            if name is None:
                raise RuntimeError(f"{zpath.name}: member {req} missing")
            info = z.getinfo(name)
            S.require_free(info.file_size / 1024 ** 3 + 0.5)
            with z.open(info) as src, (dest / req).open("wb") as out:
                shutil.copyfileobj(src, out, length=8 << 20)
            members[req] = {"member": name, "bytes": info.file_size, "crc": info.CRC}
    return members


def parse_archive(zpath: Path, source_period: str, work: Path) -> dict[str, Any]:
    """Extract, parse into ``parts/source=<period>/``, return row counts. The caller deletes zip + work."""
    members = _extract(zpath, work)
    con = S.connect("13f_parse")
    sub, cov, summ, info = (work / m for m in REQUIRED)
    cols = {r[0].upper() for r in con.execute(f"DESCRIBE SELECT * FROM {_tsv(info)}").fetchall()}
    figi = "nullif(trim(FIGI), '')" if "FIGI" in cols else "CAST(NULL AS VARCHAR)"
    con.execute(f"""CREATE TEMP TABLE sub AS SELECT trim(ACCESSION_NUMBER) AS accession, {_d('FILING_DATE')} AS filing_date,
                    upper(trim(SUBMISSIONTYPE)) AS submission_type, lpad(trim(CIK), 10, '0') AS filer_cik,
                    {_d('PERIODOFREPORT')} AS period_of_report FROM {_tsv(sub)}""")
    ccols = {r[0].upper() for r in con.execute(f"DESCRIBE SELECT * FROM {_tsv(cov)}").fetchall()}

    def cc(name: str) -> str:
        return f"nullif(trim({name}), '')" if name in ccols else "CAST(NULL AS VARCHAR)"

    con.execute(f"""CREATE TEMP TABLE cov AS SELECT trim(ACCESSION_NUMBER) AS accession, {cc('ISAMENDMENT')} AS is_amendment,
                    {cc('AMENDMENTNO')} AS amendment_no, upper({cc('AMENDMENTTYPE')}) AS amendment_type,
                    upper({cc('REPORTTYPE')}) AS report_type, {cc('FILINGMANAGER_NAME')} AS filing_manager_name,
                    {cc('FORM13FFILENUMBER')} AS form13f_file_number, {cc('CRDNUMBER')} AS crd_number,
                    {cc('SECFILENUMBER')} AS sec_file_number FROM {_tsv(cov)}""")
    con.execute(f"""CREATE TEMP TABLE summ AS SELECT trim(ACCESSION_NUMBER) AS accession,
                    try_cast(nullif(trim(OTHERINCLUDEDMANAGERSCOUNT), '') AS BIGINT) AS other_included_managers_count,
                    try_cast(nullif(trim(TABLEENTRYTOTAL), '') AS BIGINT) AS table_entry_total,
                    try_cast(nullif(trim(TABLEVALUETOTAL), '') AS DOUBLE) AS table_value_total_raw,
                    nullif(trim(ISCONFIDENTIALOMITTED), '') AS is_confidential_omitted FROM {_tsv(summ)}""")
    con.execute(f"""CREATE TEMP TABLE f AS
        SELECT s.*, last_day(date_trunc('quarter', s.period_of_report) + INTERVAL 2 MONTH)::DATE AS period_q,
               (CAST(s.filing_date AS TIMESTAMP) + INTERVAL 46 HOUR) AT TIME ZONE 'UTC' AS available_at,
               'filed_plus_46h' AS clock_basis,
               c.is_amendment, c.amendment_no, c.amendment_type, c.report_type, c.filing_manager_name,
               c.form13f_file_number, c.crd_number, c.sec_file_number,
               m.other_included_managers_count, m.table_entry_total, m.table_value_total_raw, m.is_confidential_omitted,
               '{source_period}' AS source_period
        FROM sub s LEFT JOIN cov c USING (accession) LEFT JOIN summ m USING (accession)""")
    dup_acc = con.execute("SELECT count(*) - count(DISTINCT accession) FROM f").fetchone()[0]
    out = parts_dir() / f"source={source_period}"
    out.mkdir(parents=True, exist_ok=True)
    hold_sql = f"""
        WITH i AS (
            SELECT trim(ACCESSION_NUMBER) AS accession, try_cast(nullif(trim(INFOTABLE_SK), '') AS BIGINT) AS infotable_sk,
                   nullif(trim(NAMEOFISSUER), '') AS nameofissuer, nullif(trim(TITLEOFCLASS), '') AS titleofclass,
                   nullif(upper(regexp_replace(trim(CUSIP), '[^0-9A-Za-z]', '', 'g')), '') AS c0, nullif(trim(CUSIP), '') AS cusip_raw,
                   {figi} AS figi, try_cast(nullif(trim(VALUE), '') AS DOUBLE) AS value_raw,
                   try_cast(nullif(trim(SSHPRNAMT), '') AS DOUBLE) AS amt, upper(nullif(trim(SSHPRNAMTTYPE), '')) AS sshprnamt_type,
                   upper(nullif(trim(PUTCALL), '')) AS put_call, upper(nullif(trim(INVESTMENTDISCRETION), '')) AS discretion,
                   nullif(trim(OTHERMANAGER), '') AS other_manager,
                   try_cast(nullif(trim(VOTING_AUTH_SOLE), '') AS DOUBLE) AS voting_sole,
                   try_cast(nullif(trim(VOTING_AUTH_SHARED), '') AS DOUBLE) AS voting_shared,
                   try_cast(nullif(trim(VOTING_AUTH_NONE), '') AS DOUBLE) AS voting_none
            FROM {_tsv(info)}
        )
        SELECT i.accession, i.infotable_sk, f.filer_cik, f.period_of_report, f.period_q, f.filing_date, f.available_at,
               f.submission_type, f.amendment_type,
               CASE WHEN regexp_full_match(i.c0, '[0-9]{{7,8}}') THEN lpad(i.c0, 9, '0') ELSE i.c0 END AS cusip,
               i.cusip_raw, coalesce(regexp_full_match(i.c0, '[0-9]{{7,8}}'), false) AS cusip_repaired, i.figi,
               i.nameofissuer, i.titleofclass,
               CASE WHEN i.sshprnamt_type = 'SH' THEN i.amt END AS shares,
               CASE WHEN i.sshprnamt_type = 'PRN' THEN i.amt END AS prn_amount,
               i.sshprnamt_type, i.value_raw,
               i.value_raw * CASE WHEN f.filing_date < DATE '{VALUE_DOLLARS_FROM}' THEN 1000 ELSE 1 END AS value_usd,
               CASE WHEN f.filing_date < DATE '{VALUE_DOLLARS_FROM}' THEN 'thousands' ELSE 'dollars' END AS value_unit,
               i.put_call, i.discretion, i.other_manager, i.voting_sole, i.voting_shared, i.voting_none,
               '{source_period}' AS source_period
        FROM i LEFT JOIN f USING (accession)
        ORDER BY f.period_q, i.accession, i.infotable_sk
    """
    n_hold = C.copy_to_parquet(con, hold_sql, out / "holdings.parquet")
    hp = (out / "holdings.parquet").as_posix()
    orphan = con.execute(f"SELECT count(*) FROM read_parquet('{hp}') WHERE filer_cik IS NULL").fetchone()[0]
    n_fil = C.copy_to_parquet(con, f"""
        SELECT f.*, coalesce(h.n, 0) AS n_infotable_rows
        FROM f LEFT JOIN (SELECT accession, count(*) AS n FROM read_parquet('{hp}') GROUP BY 1) h USING (accession)
        ORDER BY period_q, filer_cik, filing_date, accession""", out / "filings.parquet")
    stats = con.execute(f"""SELECT count(*) FILTER (WHERE cusip IS NULL), count(*) FILTER (WHERE cusip_repaired),
                                   count(*) FILTER (WHERE sshprnamt_type NOT IN ('SH', 'PRN') OR sshprnamt_type IS NULL),
                                   count(*) FILTER (WHERE value_raw IS NULL), count(*) FILTER (WHERE amt_null)
                            FROM (SELECT *, shares IS NULL AND prn_amount IS NULL AS amt_null FROM read_parquet('{hp}'))""").fetchone()
    con.close()
    return {"members": members, "holdings_rows": n_hold, "filings_rows": n_fil, "duplicate_accessions_in_submission": int(dup_acc),
            "holdings_without_submission": int(orphan), "null_cusip": int(stats[0]), "cusip_repaired": int(stats[1]),
            "bad_sshprnamt_type": int(stats[2]), "null_value": int(stats[3]), "null_amount": int(stats[4]),
            "parts": {p.name: {"bytes": p.stat().st_size, "sha256": C.sha256_file(p)} for p in sorted(out.glob("*.parquet"))}}


def fetch(limit: int | None = None, only: str | None = None) -> dict[str, Any]:
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    led = ledger()
    st, body, _ = S.sec_get(PAGE_URL, timeout=120)
    if st != 200:
        raise RuntimeError(f"13F page HTTP {st}")
    snap = RAW_DIR / f"page_{dt.datetime.now(dt.UTC):%Y%m%dT%H%M%SZ}.html.gz"
    S.F.write_gzip_atomic(snap, body)
    led.append({"key": "page", "kind": "page", "url": PAGE_URL, "sha256": hashlib.sha256(body).hexdigest(),
                "bytes": len(body), "fetched_at": S.utc_now(), "file": snap.name})
    archives = discover(body.decode("utf-8", errors="replace"))
    done = {k for k, v in led.latest().items() if v.get("status") == "parsed" and _parts_ok(k, v)}
    todo = [a for a in archives if a[0] not in done and (only is None or a[0] == only)]
    if limit is not None:
        todo = todo[:limit]
    print(f"13F: {len(archives)} data sets listed, {len(done)} parsed, {len(todo)} to do", flush=True)
    stats = {"listed": len(archives), "parsed_before": len(done), "done_now": 0}
    for sp, url, a, b in todo:
        t0 = time.perf_counter()
        S.require_free(3.0)
        zpath = RAW_DIR / f"{sp}_form13f.zip"
        st, _, hdr = S.sec_get(url, timeout=1800, stream_to=zpath)
        if st != 200:
            led.append({"key": sp, "kind": "zip", "url": url, "http_status": st, "fetched_at": S.utc_now()})
            continue
        sha = C.sha256_file(zpath)
        rec = {"key": sp, "kind": "zip", "url": url, "http_status": 200, "bytes": zpath.stat().st_size, "sha256": sha,
               "fetched_at": S.utc_now(), "last_modified": hdr.get("last-modified"), "etag": hdr.get("etag"),
               "filing_date_range": [a.isoformat(), b.isoformat()], "status": "downloaded"}
        led.append(rec)
        work = S.tmp_dir("13f_work") / sp
        try:
            res = parse_archive(zpath, sp, work)
        finally:
            shutil.rmtree(work, ignore_errors=True)
        zpath.unlink()
        rec.update(status="parsed", parsed_at=S.utc_now(), zip_deleted=True, **res,
                   elapsed_s=round(time.perf_counter() - t0, 1))
        led.append(rec)
        stats["done_now"] += 1
        print(f"  {sp}: {res['holdings_rows']:,} holdings, {res['filings_rows']:,} filings, "
              f"{rec['bytes'] / 1e6:.0f} MB zip, {rec['elapsed_s']} s, free {S.free_gb():.1f} GB", flush=True)
    return stats


def _parts_ok(sp: str, rec: dict[str, Any]) -> bool:
    d = parts_dir() / f"source={sp}"
    return all((d / n).exists() and (d / n).stat().st_size == v["bytes"] for n, v in rec.get("parts", {}).items()) and bool(rec.get("parts"))


# ---------------------------------------------------------------- build
def _filings_sql() -> str:
    """Every accession once (the earliest data set that carries it)."""
    g = (parts_dir() / "source=*" / "filings.parquet").as_posix()
    return f"""
        SELECT * EXCLUDE (rk) FROM (
            SELECT *, row_number() OVER (PARTITION BY accession ORDER BY filing_date, source_period) AS rk,
                   count(*) OVER (PARTITION BY accession) AS n_data_sets
            FROM read_parquet('{g}', hive_partitioning = false)
        ) WHERE rk = 1"""


def _effective_sql(cutoff_col: str | None) -> str:
    """SQL over temp table ``fl`` -> effective ``(accession, filer_cik, period_q)`` (cutoff: filing_date <= column)."""
    cut = f"AND filing_date <= {cutoff_col}" if cutoff_col else ""
    return f"""
        WITH hr AS (
            SELECT accession, filer_cik, period_q, filing_date, submission_type, amendment_type,
                   (submission_type = '13F-HR' OR coalesce(amendment_type, '') = 'RESTATEMENT') AS is_base,
                   row_number() OVER (PARTITION BY filer_cik, period_q ORDER BY filing_date, accession) AS ord
            FROM fl WHERE submission_type IN ('13F-HR', '13F-HR/A') {cut}
        ),
        b AS (SELECT filer_cik, period_q, max(ord) FILTER (WHERE is_base) AS base_ord FROM hr GROUP BY 1, 2)
        SELECT hr.accession, hr.filer_cik, hr.period_q
        FROM hr JOIN b USING (filer_cik, period_q)
        WHERE (hr.is_base AND hr.ord = b.base_ord) OR (NOT hr.is_base AND hr.ord > coalesce(b.base_ord, 0))
    """


def build_cusip_map(con, periods: list[dt.date]) -> Path:
    ftd_glob = (C.build_root() / "ftd" / "year=*" / "ftd.parquet").as_posix()
    tmp = S.tmp_dir("13f_build")
    agg = tmp / "ftd_cusip_file.parquet"
    C.copy_to_parquet(con, f"""
        SELECT cusip, security_id, source_file, min(available_at) AS available_at, max(settlement_date) AS last_seen,
               min(settlement_date) AS first_seen, count(*) AS n_obs
        FROM read_parquet('{ftd_glob}', hive_partitioning = false)
        WHERE security_id IS NOT NULL AND cusip IS NOT NULL
        GROUP BY 1, 2, 3""", agg)
    dest = C.stage_dir(STAGE) / "cusip_map_pit.parquet"
    part_dir = tmp / "cmap_parts"
    part_dir.mkdir(parents=True, exist_ok=True)
    for f in part_dir.glob("*.parquet"):
        f.unlink()
    for p in periods:
        lo, cut = p - dt.timedelta(days=MAP_LOOKBACK_DAYS), S.at_utc(p + dt.timedelta(days=DEADLINE_DAYS))
        C.copy_to_parquet(con, f"""
            WITH w AS (
                SELECT cusip, security_id, max(last_seen) AS last_seen, sum(n_obs) AS n_obs
                FROM read_parquet('{agg.as_posix()}')
                WHERE available_at < TIMESTAMPTZ '{cut.isoformat()}' AND last_seen >= DATE '{lo}'
                GROUP BY 1, 2
            ),
            r AS (
                SELECT *, count(*) OVER (PARTITION BY cusip) AS n_ids,
                       rank() OVER (PARTITION BY cusip ORDER BY last_seen DESC) AS rk
                FROM w
            )
            SELECT DATE '{p}' AS period_q, cusip,
                   CAST(CASE WHEN count(*) = 1 THEN min(security_id) END AS BIGINT) AS security_id,
                   CASE WHEN max(n_ids) = 1 THEN 'unique' WHEN count(*) = 1 THEN 'latest' ELSE 'ambiguous' END AS map_basis,
                   max(n_ids) AS n_ids, sum(n_obs) AS n_obs, max(last_seen) AS last_seen
            FROM r WHERE rk = 1 GROUP BY 1, 2""", part_dir / f"cmap_{p}.parquet")
    C.copy_to_parquet(con, f"SELECT * FROM read_parquet('{(part_dir / '*.parquet').as_posix()}') ORDER BY period_q, cusip", dest)
    return dest


def build() -> dict[str, Any]:
    t0 = time.perf_counter()
    receipt: dict[str, Any] = {"clock_rule": CLOCK_RULE, "value_rule": VALUE_RULE, "map_rule": MAP_RULE}
    S.require_free(2.0)
    con = S.connect("13f_build", memory=BUILD_MEMORY)
    out = C.stage_dir(STAGE)
    tmp = S.tmp_dir("13f_build")
    fl0 = tmp / "filings_dedup0.parquet"
    C.copy_to_parquet(con, _filings_sql(), fl0)
    periods = [r[0] for r in con.execute(
        f"SELECT DISTINCT period_q FROM read_parquet('{fl0.as_posix()}') "
        "WHERE period_q BETWEEN DATE '2013-01-01' AND current_date ORDER BY 1").fetchall()]
    con.execute("CREATE OR REPLACE TEMP TABLE dl (period_q DATE, deadline DATE)")
    con.executemany("INSERT INTO dl VALUES (?, ?)", [(p, deadline45(p)) for p in periods])
    fl = tmp / "filings_dedup.parquet"
    C.copy_to_parquet(con, f"SELECT f.*, dl.deadline FROM read_parquet('{fl0.as_posix()}') f LEFT JOIN dl USING (period_q)", fl)
    con.execute(f"CREATE OR REPLACE VIEW fl AS SELECT * FROM read_parquet('{fl.as_posix()}')")
    with C.timed(receipt, "cusip_map"):
        cmap = build_cusip_map(con, periods)
    effp = {"eff_final": tmp / "eff_final.parquet", "eff_45": tmp / "eff_45.parquet"}
    C.copy_to_parquet(con, _effective_sql(None), effp["eff_final"])
    C.copy_to_parquet(con, _effective_sql("deadline"), effp["eff_45"])
    con.close()

    def fresh():
        c = S.connect("13f_build", memory=BUILD_MEMORY, threads=BUILD_THREADS)
        c.execute(f"CREATE OR REPLACE VIEW fl AS SELECT * FROM read_parquet('{fl.as_posix()}')")
        for name, path in effp.items():
            c.execute(f"CREATE OR REPLACE VIEW {name} AS SELECT * FROM read_parquet('{path.as_posix()}')")
        return c
    hglob = (parts_dir() / "source=*" / "holdings.parquet").as_posix()
    aggs = {"final": tmp / "agg_final_raw.parquet", "asof45": tmp / "agg_asof45_raw.parquet"}
    per_q: dict[str, Any] = {}
    import pyarrow.parquet as pq

    writers: dict[str, Any] = {}
    fwriter = qwriter = None
    try:
        for p in periods:
            tq = time.perf_counter()
            con = fresh()
            # integer keys keep the per-quarter working set small (accession / CIK strings stay in ``fp``)
            con.execute(f"""CREATE OR REPLACE TEMP TABLE fp AS
                SELECT row_number() OVER (ORDER BY accession)::INTEGER AS acc_id, accession, source_period,
                       CAST(filer_cik AS BIGINT) AS fid, filer_cik, filing_date, available_at, submission_type, deadline
                FROM fl WHERE period_q = DATE '{p}'""")
            con.execute(f"""CREATE OR REPLACE TEMP TABLE h AS
                SELECT fp.acc_id, fp.fid, h.cusip, h.shares, h.value_usd,
                       (h.sshprnamt_type = 'SH' AND h.put_call IS NULL AND h.shares > 0) AS sh_row,
                       (h.sshprnamt_type = 'SH' AND h.put_call IS NULL) AS sh_non_option
                FROM read_parquet('{hglob}', hive_partitioning = false) h
                JOIN fp USING (accession, source_period)
                WHERE h.period_q = DATE '{p}'""")
            # rule 13f-row-sanity-v1: consensus implied price per CUSIP (>= 5 rows), filing-level unit repair, row outliers
            con.execute(f"""CREATE OR REPLACE TEMP TABLE cons AS
                SELECT cusip, median(value_usd / shares) AS px FROM h WHERE sh_row AND value_usd > 0
                GROUP BY 1 HAVING count(*) >= {CONSENSUS_MIN_ROWS}""")
            con.execute(f"""CREATE OR REPLACE TEMP TABLE funit AS
                WITH fchk AS (
                    SELECT h.acc_id, median((h.value_usd / h.shares) / cons.px) AS med, count(*) AS n
                    FROM h JOIN cons USING (cusip) WHERE h.sh_row AND h.value_usd > 0 GROUP BY 1
                )
                SELECT fchk.acc_id, fp.accession, med, n,
                       CASE WHEN n >= 3 AND fp.filing_date < DATE '{VALUE_DOLLARS_FROM}' AND med BETWEEN 300 AND 3000 THEN 0.001
                            WHEN n >= 3 AND fp.filing_date >= DATE '{VALUE_DOLLARS_FROM}' AND med BETWEEN 1.0 / 3000 AND 1.0 / 300 THEN 1000
                            ELSE 1 END AS unit_factor,
                       CASE WHEN n < 3 THEN 'insufficient'
                            WHEN fp.filing_date < DATE '{VALUE_DOLLARS_FROM}' AND med BETWEEN 300 AND 3000 THEN 'dollars_reported_before_2023'
                            WHEN fp.filing_date >= DATE '{VALUE_DOLLARS_FROM}' AND med BETWEEN 1.0 / 3000 AND 1.0 / 300 THEN 'thousands_reported_from_2023'
                            WHEN med > {PRICE_OUTLIER_X} THEN 'ratio_high' WHEN med < 1.0 / {PRICE_OUTLIER_X} THEN 'ratio_low'
                            ELSE 'ok' END AS value_unit_check
                FROM fchk JOIN fp USING (acc_id)""")
            con.execute(f"""CREATE OR REPLACE TEMP TABLE hm AS
                SELECT h.acc_id, h.fid, h.cusip, h.shares, h.sh_row, h.sh_non_option, m.security_id,
                       coalesce(u.unit_factor, 1) AS unit_factor, h.value_usd * coalesce(u.unit_factor, 1) AS value_usd_clean,
                       coalesce(h.sh_row AND h.value_usd > 0 AND c.px IS NOT NULL
                                AND ((h.value_usd * coalesce(u.unit_factor, 1) / h.shares) / c.px > {PRICE_OUTLIER_X}
                                     OR (h.value_usd * coalesce(u.unit_factor, 1) / h.shares) / c.px < 1.0 / {PRICE_OUTLIER_X}),
                                false)
                       OR coalesce(h.value_usd * coalesce(u.unit_factor, 1) > {IMPLAUSIBLE_VALUE_USD}, false)
                       OR coalesce(h.sh_row AND h.value_usd * coalesce(u.unit_factor, 1) / h.shares > {IMPLAUSIBLE_PRICE_USD}, false)
                         AS price_outlier
                FROM h LEFT JOIN read_parquet('{cmap.as_posix()}') m ON m.period_q = DATE '{p}' AND m.cusip = h.cusip
                LEFT JOIN cons c ON c.cusip = h.cusip LEFT JOIN funit u ON u.acc_id = h.acc_id""")
            con.execute("DROP TABLE h")
            qtbl = con.execute(f"""
                SELECT DATE '{p}' AS period_q, u.accession, u.med AS price_ratio_median, u.n AS rows_checked, u.unit_factor,
                       u.value_unit_check, coalesce(o.n_out, 0) AS price_outlier_rows, coalesce(o.sh_out, 0) AS price_outlier_shares
                FROM funit u LEFT JOIN (SELECT acc_id, count(*) AS n_out, sum(shares) AS sh_out FROM hm WHERE price_outlier GROUP BY 1) o
                  USING (acc_id)""").to_arrow_table()
            if qwriter is None:
                qwriter = pq.ParquetWriter(tmp / "filing_checks.parquet.partial", qtbl.schema, compression="zstd")
            qwriter.write_table(qtbl)
            del qtbl
            q: dict[str, Any] = {}
            for ver, eff in (("final", "eff_final"), ("asof45", "eff_45")):
                vfilter = "" if ver == "final" else "AND fp.filing_date <= fp.deadline"
                con.execute(f"""CREATE OR REPLACE TEMP TABLE e AS
                    SELECT fp.acc_id FROM {eff} x JOIN fp USING (accession) WHERE x.period_q = DATE '{p}'""")
                sql = f"""
                    WITH he AS (SELECT hm.* FROM hm SEMI JOIN e USING (acc_id)),
                    sh AS (
                        SELECT security_id, fid, sum(shares) AS shares, sum(value_usd_clean) AS value_usd
                        FROM he WHERE security_id IS NOT NULL AND sh_row AND NOT price_outlier
                        GROUP BY 1, 2
                    ),
                    outl AS (
                        SELECT security_id, count(*) AS n_out, sum(shares) AS sh_out FROM he
                        WHERE security_id IS NOT NULL AND price_outlier GROUP BY 1
                    ),
                    fa AS (
                        SELECT fid, max(available_at) AS fa FROM fp
                        WHERE submission_type IN ('13F-HR', '13F-HR/A') {vfilter} GROUP BY 1
                    ),
                    anyh AS (
                        SELECT DISTINCT hm.security_id, hm.fid FROM hm JOIN fp USING (acc_id)
                        WHERE hm.security_id IS NOT NULL {vfilter}
                    ),
                    av AS (SELECT a.security_id, max(fa.fa) AS available_at FROM anyh a JOIN fa USING (fid) GROUP BY 1),
                    rk AS (SELECT *, row_number() OVER (PARTITION BY security_id ORDER BY shares DESC, fid) AS r FROM sh)
                    SELECT DATE '{p}' AS period_q, rk.security_id, sum(shares) AS inst_shares, count(*) AS n_holders,
                           sum(shares) FILTER (WHERE r <= 10) / sum(shares) AS top10_share,
                           max(shares) / sum(shares) AS top1_share, sum(value_usd) AS inst_value_usd,
                           coalesce(any_value(outl.n_out), 0) AS n_rows_price_outlier,
                           coalesce(any_value(outl.sh_out), 0) AS shares_price_outlier,
                           max(av.available_at) AS available_at,
                           (SELECT max(filing_date) FROM fp WHERE true {vfilter}) AS latest_filing_date_in_version
                    FROM rk LEFT JOIN av USING (security_id) LEFT JOIN outl USING (security_id)
                    GROUP BY rk.security_id
                """
                tbl = con.execute(sql).to_arrow_table()
                if ver not in writers:
                    writers[ver] = pq.ParquetWriter(aggs[ver].with_name(aggs[ver].name + ".partial"), tbl.schema, compression="zstd")
                writers[ver].write_table(tbl)
                n_sec = tbl.num_rows
                del tbl
                cov = con.execute("""
                    WITH he AS (SELECT hm.* FROM hm SEMI JOIN e USING (acc_id))
                    SELECT count(*), sum(value_usd_clean), sum(value_usd_clean) FILTER (WHERE security_id IS NOT NULL),
                           sum(value_usd_clean) FILTER (WHERE sh_non_option AND NOT price_outlier),
                           sum(value_usd_clean) FILTER (WHERE sh_non_option AND NOT price_outlier AND security_id IS NOT NULL),
                           count(DISTINCT fid), count(DISTINCT cusip), count(DISTINCT cusip) FILTER (WHERE security_id IS NOT NULL),
                           count(*) FILTER (WHERE price_outlier), count(DISTINCT acc_id) FILTER (WHERE unit_factor <> 1)
                    FROM he""").fetchone()
                q[ver] = {"rows": cov[0], "value_usd": cov[1],
                          "mapped_share_value_all_rows": round(cov[2] / cov[1], 4) if cov[1] else None,
                          "mapped_share_value_sh_non_option": round(cov[4] / cov[3], 4) if cov[3] else None,
                          "filers": cov[5], "cusips": cov[6], "cusips_mapped": cov[7], "securities": n_sec,
                          "rows_price_outlier": cov[8], "filings_unit_repaired": cov[9]}
                if ver == "final":
                    # filers of the final effective set
                    ftbl = con.execute(f"""
                        WITH he AS (SELECT hm.* FROM hm SEMI JOIN e USING (acc_id)),
                        agg AS (
                            SELECT fid, count(*) AS n_entries, sum(value_usd_clean) AS total_value_usd,
                                   count(*) FILTER (WHERE security_id IS NOT NULL) AS n_entries_mapped,
                                   count(*) FILTER (WHERE price_outlier) AS n_entries_price_outlier
                            FROM he GROUP BY 1
                        ),
                        base AS (
                            SELECT * FROM (
                                SELECT fl.*, CAST(fl.filer_cik AS BIGINT) AS fid, row_number() OVER (PARTITION BY filer_cik ORDER BY
                                    (submission_type = '13F-HR' OR coalesce(amendment_type, '') = 'RESTATEMENT') DESC,
                                    filing_date DESC, accession DESC) AS rb
                                FROM fl WHERE period_q = DATE '{p}') WHERE rb = 1
                        ),
                        types AS (
                            SELECT filer_cik, string_agg(DISTINCT submission_type, ',' ORDER BY submission_type) AS submission_types,
                                   count(*) AS n_filings, min(filing_date) AS first_filing_date, max(filing_date) AS last_filing_date,
                                   min(available_at) AS first_available_at
                            FROM fl WHERE period_q = DATE '{p}' GROUP BY 1
                        )
                        SELECT DATE '{p}' AS period_q, base.filer_cik, base.filing_manager_name AS filer_name, types.submission_types,
                               base.report_type, base.accession AS base_accession, types.n_filings, types.first_filing_date,
                               types.last_filing_date, types.first_available_at,
                               coalesce(agg.n_entries, 0) AS n_entries, coalesce(agg.n_entries_mapped, 0) AS n_entries_mapped,
                               coalesce(agg.n_entries_price_outlier, 0) AS n_entries_price_outlier,
                               agg.total_value_usd, base.table_entry_total, base.table_value_total_raw, base.other_included_managers_count,
                               base.is_confidential_omitted, base.form13f_file_number, base.crd_number, base.sec_file_number,
                               coalesce(u.value_unit_check, 'insufficient') AS value_unit_check, u.med AS value_price_ratio_median,
                               CAST(NULL AS VARCHAR) AS filer_type
                        FROM base JOIN types USING (filer_cik) LEFT JOIN agg USING (fid)
                        LEFT JOIN funit u ON u.accession = base.accession
                    """).to_arrow_table()
                    if fwriter is None:
                        fwriter = pq.ParquetWriter((tmp / "filers.parquet.partial"), ftbl.schema, compression="zstd")
                    fwriter.write_table(ftbl)
                    del ftbl
            q["value_unit_check_filings"] = dict(con.execute(
                "SELECT value_unit_check, count(*) FROM funit GROUP BY 1").fetchall())
            q["deadline45"] = deadline45(p).isoformat()
            q["elapsed_s"] = round(time.perf_counter() - tq, 1)
            per_q[p.isoformat()] = q
            con.close()
            print(f"13F {p}: final {q['final']['securities']} ids map {q['final']['mapped_share_value_sh_non_option']} "
                  f"outl {q['final']['rows_price_outlier']} unitfix {q['final']['filings_unit_repaired']} | "
                  f"asof45 {q['asof45']['securities']} ids | {q['elapsed_s']} s", flush=True)
    finally:
        for w in list(writers.values()) + [fwriter, qwriter]:
            if w is not None:
                w.close()
    con = fresh()
    for ver, pth in aggs.items():
        pth.with_name(pth.name + ".partial").replace(pth)
        C.copy_to_parquet(con, f"""
            WITH a AS (SELECT * FROM read_parquet('{pth.as_posix()}')),
            p AS (SELECT a.*, lag(period_q) OVER w AS prev_q, lag(inst_shares) OVER w AS prev_inst_shares,
                         lag(n_holders) OVER w AS prev_n_holders, lag(available_at) OVER w AS prev_available_at
                  FROM a WINDOW w AS (PARTITION BY security_id ORDER BY period_q))
            SELECT period_q AS period_of_report, security_id, inst_shares, n_holders, top10_share, top1_share, inst_value_usd,
                   n_rows_price_outlier, shares_price_outlier, available_at, latest_filing_date_in_version, '{ver}' AS version,
                   CASE WHEN prev_q = last_day(period_q - INTERVAL 3 MONTH) THEN prev_inst_shares END AS prior_inst_shares,
                   CASE WHEN prev_q = last_day(period_q - INTERVAL 3 MONTH) THEN inst_shares - prev_inst_shares END AS d_inst_shares,
                   CASE WHEN prev_q = last_day(period_q - INTERVAL 3 MONTH) AND prev_inst_shares > 0
                        THEN inst_shares / prev_inst_shares - 1 END AS pct_inst_shares,
                   CASE WHEN prev_q = last_day(period_q - INTERVAL 3 MONTH) THEN n_holders - prev_n_holders END AS d_n_holders,
                   CASE WHEN prev_q = last_day(period_q - INTERVAL 3 MONTH) THEN greatest(available_at, prev_available_at) END AS chg_available_at
            FROM p ORDER BY period_q, security_id""", out / f"agg_{ver}.parquet")
    (tmp / "filers.parquet.partial").replace(out / "filers.parquet")
    (tmp / "filing_checks.parquet.partial").replace(out / "filing_checks.parquet")
    C.copy_to_parquet(con, f"SELECT * FROM read_parquet('{fl.as_posix()}') ORDER BY period_q, filer_cik, filing_date, accession",
                      out / "filings.parquet")
    receipt["periods"] = per_q
    receipt["filings"] = _filing_stats(con, fl)
    receipt["cusip_map"] = dict(con.execute(f"SELECT map_basis, count(*) FROM read_parquet('{cmap.as_posix()}') GROUP BY 1").fetchall())
    receipt["coverage_by_year"] = coverage(con, out / "agg_asof45.parquet")
    receipt["validation"] = validate(con)
    receipt["timings_s"]["total"] = round(time.perf_counter() - t0, 1)
    led = ledger()
    recs = {k: v for k, v in led.latest().items() if v.get("kind") == "zip" and v.get("status") == "parsed"}
    payload = {
        "clock_rule": CLOCK_RULE, "value_rule": VALUE_RULE, "map_rule": MAP_RULE,
        "rule_text": {
            "clock": "available_at = filing_date 00:00 UTC + 46 h (no acceptance time in the data sets)",
            "value": "value_usd = VALUE * 1000 when filing_date < 2023-01-03 else VALUE (SEC readme)",
            "cusip_map": (f"cusip -> security_id at quarter end P via mapped FTD rows with settlement >= P - {MAP_LOOKBACK_DAYS} d "
                          f"and FTD available_at < P + {DEADLINE_DAYS} d; unique / latest / ambiguous"),
            "effective": ("per (filer, quarter): latest 13F-HR or RESTATEMENT = base, plus later NEW HOLDINGS amendments; "
                          "final = all filings, asof45 = filing_date <= first SEC business day >= P + 45 d"),
            "aggregate": ("SH rows, no put/call, shares > 0, mapped; inst_shares, n_holders, top10_share, inst_value_usd; "
                          "available_at = latest available_at among the version's filings of every filer that reported the "
                          "security for that quarter; changes vs the prior quarter in the same version"),
        },
        "staleness_rule": f"a consumer treats a 13F aggregate as NaN when the session is more than {STALENESS_DAYS} days after period_of_report",
        "staleness_days": STALENESS_DAYS,
        "filer_type": "not classified (no free authoritative hedge-fund / mutual-fund label joined; see docs)",
        "inputs": {"ftd_manifest_sha256": C.sha256_file(C.build_root() / "ftd" / "manifest.json"),
                   "clock_validation": "clock_validation.json (EDGAR index acceptance times, stratified sample)"},
        "sources": {"page": PAGE_URL, "ledger": str(led.path), "ledger_sha256": C.sha256_file(led.path),
                    "zips": {k: {"url": v["url"], "sha256": v["sha256"], "bytes": v["bytes"], "fetched_at": v["fetched_at"],
                                 "last_modified": v.get("last_modified"), "deleted_after_parse": v.get("zip_deleted")}
                             for k, v in sorted(recs.items())}},
        "receipt": receipt,
    }
    C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)
    return receipt


def _filing_stats(con, fl: Path) -> dict[str, Any]:
    f = fl.as_posix()
    by = con.execute(f"""SELECT year(period_q), count(*), count(*) FILTER (WHERE submission_type = '13F-HR'),
                                count(*) FILTER (WHERE submission_type = '13F-HR/A'),
                                count(*) FILTER (WHERE amendment_type = 'RESTATEMENT'), count(*) FILTER (WHERE amendment_type = 'NEW HOLDINGS'),
                                count(*) FILTER (WHERE submission_type LIKE '13F-NT%'),
                                count(*) FILTER (WHERE period_of_report <> period_q), count(*) FILTER (WHERE n_data_sets > 1),
                                count(DISTINCT filer_cik)
                         FROM read_parquet('{f}') GROUP BY 1 ORDER BY 1""").fetchall()
    return {str(r[0]): {"filings": r[1], "hr": r[2], "hr_a": r[3], "restatement": r[4], "new_holdings": r[5], "notice": r[6],
                        "period_not_quarter_end": r[7], "accession_in_several_data_sets": r[8], "filers": r[9]} for r in by}


def coverage(con, agg: Path) -> dict[str, Any]:
    """Per year: share of member_equity cells with a visible asof45 aggregate no older than the staleness rule."""
    panel = C.build_root() / "panel"
    cal = C.calendar_path().as_posix()
    rows = con.execute(f"""
        WITH s AS (SELECT session_date, lag(session_date) OVER (ORDER BY session_date) AS prev FROM read_parquet('{cal}')),
        m AS (
            SELECT p.session_date, p.security_id, (CAST(s.prev AS TIMESTAMP) + INTERVAL 22 HOUR) AT TIME ZONE 'UTC' AS cutoff
            FROM read_parquet('{(panel / 'year=*' / '*.parquet').as_posix()}', hive_partitioning = false) p
            JOIN s USING (session_date) WHERE p.member_equity AND s.prev IS NOT NULL
        ),
        a AS (SELECT security_id, available_at, period_of_report FROM read_parquet('{agg.as_posix()}') WHERE available_at IS NOT NULL),
        j AS (SELECT m.session_date, a.period_of_report FROM m ASOF LEFT JOIN a ON a.security_id = m.security_id AND m.cutoff > a.available_at)
        SELECT year(session_date), count(*),
               count(*) FILTER (WHERE period_of_report IS NOT NULL AND date_diff('day', period_of_report, session_date) <= {STALENESS_DAYS})
        FROM j GROUP BY 1 ORDER BY 1""").fetchall()
    return {str(y): {"member_equity_cells": n, "share_with_visible_inst_shares": round(k / n, 4)} for y, n, k in rows}


def validate(con) -> dict[str, Any]:
    """Spot checks: AAPL / GME / MSFT holders and inst_shares per quarter (asof45 and final)."""
    out = C.stage_dir(STAGE)
    cm = (out / "cusip_map_pit.parquet").as_posix()
    res: dict[str, Any] = {}
    for name, cusip in (("AAPL", "037833100"), ("MSFT", "594918104"), ("GME", "36467W109")):
        ids = con.execute(f"SELECT DISTINCT security_id FROM read_parquet('{cm}') WHERE cusip = '{cusip}' AND security_id IS NOT NULL").fetchall()
        rows = con.execute(f"""
            SELECT f.period_of_report, f.security_id, f.n_holders, f.inst_shares, a.n_holders, a.inst_shares, a.available_at
            FROM read_parquet('{(out / 'agg_final.parquet').as_posix()}') f
            LEFT JOIN read_parquet('{(out / 'agg_asof45.parquet').as_posix()}') a USING (security_id, period_of_report)
            WHERE f.security_id IN (SELECT DISTINCT security_id FROM read_parquet('{cm}') WHERE cusip = '{cusip}')
            ORDER BY 1""").fetchall()
        res[name] = {"cusip": cusip, "security_ids": [i[0] for i in ids],
                     "by_quarter": {str(r[0]): {"security_id": r[1], "final_holders": r[2], "final_inst_shares": r[3],
                                                "asof45_holders": r[4], "asof45_inst_shares": r[5], "asof45_available_at": str(r[6])}
                                    for r in rows}}
    return res


ACCEPTED_RE = re.compile(r"Accepted</div>\s*<div class=\"info\">([0-9-]{10} [0-9:]{8})</div>", re.IGNORECASE)


def validate_clock(per_year: int = 25, seed: int = 20260928) -> dict[str, Any]:
    """Evidence for ``13f-filed-plus-46h-v1``: EDGAR index-page acceptance time of a stratified random sample.

    ``per_year`` accessions (13F-HR and 13F-HR/A) per filing year; each ``<acc>-index.htm`` page is fetched
    once through the SEC limiter (receipts in ``data/raw/sec_13f/index_sample.jsonl``). The page's
    "Accepted" stamp is US Eastern time.
    """
    from zoneinfo import ZoneInfo

    led = S.Ledger(RAW_DIR / "index_sample.jsonl")
    have = led.latest()
    con = S.connect("13f_clock", memory="200MB")
    g = (parts_dir() / "source=*" / "filings.parquet").as_posix()
    sample = con.execute(f"""
        SELECT accession, filer_cik, filing_date, submission_type FROM (
            SELECT *, row_number() OVER (PARTITION BY year(filing_date) ORDER BY hash(accession || '{seed}')) AS rn
            FROM read_parquet('{g}', hive_partitioning = false) WHERE submission_type IN ('13F-HR', '13F-HR/A'))
        WHERE rn <= {per_year} ORDER BY filing_date""").fetchall()
    et = ZoneInfo("America/New_York")
    rows = []
    for acc, cik, fdate, stype in sample:
        url = f"https://www.sec.gov/Archives/edgar/data/{int(cik)}/{acc.replace('-', '')}/{acc}-index.htm"
        rec = have.get(acc)
        if rec is None or rec.get("http_status") != 200:
            st, body, _ = S.sec_get(url, timeout=60)
            m = ACCEPTED_RE.search(body.decode("utf-8", errors="replace")) if st == 200 else None
            rec = {"key": acc, "url": url, "http_status": st, "fetched_at": S.utc_now(),
                   "sha256": hashlib.sha256(body).hexdigest() if st == 200 else None, "accepted_et": m.group(1) if m else None}
            led.append(rec)
        if not rec.get("accepted_et"):
            continue
        acc_utc = dt.datetime.strptime(rec["accepted_et"], "%Y-%m-%d %H:%M:%S").replace(tzinfo=et).astimezone(dt.UTC)
        rule = S.at_utc(fdate) + dt.timedelta(hours=46)
        rows.append({"accession": acc, "filing_date": fdate.isoformat(), "submission_type": stype,
                     "accepted_utc": acc_utc.isoformat(), "rule_available_at": rule.isoformat(),
                     "hours_rule_minus_accepted": round((rule - acc_utc).total_seconds() / 3600, 2),
                     "accepted_date_et_minus_filing_date_days": (acc_utc.astimezone(et).date() - fdate).days})
    ok = [r for r in rows if r["hours_rule_minus_accepted"] > 0]
    lag = sorted(r["hours_rule_minus_accepted"] for r in rows)
    res = {"sample": len(sample), "with_accepted_stamp": len(rows), "accepted_before_rule": len(ok),
           "share_accepted_before_rule": round(len(ok) / len(rows), 4) if rows else None,
           "hours_rule_minus_accepted_min_p50_max": [lag[0], lag[len(lag) // 2], lag[-1]] if lag else None,
           "accepted_after_rule": [r for r in rows if r["hours_rule_minus_accepted"] <= 0],
           "accepted_date_vs_filing_date_days": {str(k): sum(1 for r in rows if r["accepted_date_et_minus_filing_date_days"] == k)
                                                 for k in sorted({r["accepted_date_et_minus_filing_date_days"] for r in rows})},
           "ledger": str(led.path), "rows": rows}
    C.write_json_atomic(C.stage_dir(STAGE) / "clock_validation.json", res)
    return {k: v for k, v in res.items() if k != "rows"}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("--limit", type=int)
    f.add_argument("--only")
    sub.add_parser("build")
    v = sub.add_parser("validate-clock")
    v.add_argument("--per-year", type=int, default=25)
    args = ap.parse_args(argv)
    if args.cmd == "validate-clock":
        print(json.dumps(validate_clock(args.per_year), default=str), flush=True)
        return 0
    if args.cmd == "fetch":
        print(json.dumps(fetch(args.limit, args.only)), flush=True)
    else:
        rec = build()
        print(json.dumps({k: v for k, v in rec.items() if k in ("filings", "cusip_map", "coverage_by_year", "timings_s")},
                         default=str)[:6000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
