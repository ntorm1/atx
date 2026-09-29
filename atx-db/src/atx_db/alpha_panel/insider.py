"""Stage ``insider`` (D11): Forms 3/4/5 transactions from the SEC Insider Transactions Data Sets.

Source: the quarterly ``YYYYqN_form345.zip`` files listed on ``DATASETS_PAGE`` (URLs are taken from the page:
most live under ``/files/structureddata/data/insider-transactions-data-sets/``, the newest quarter may sit under
``/files/datastandardsinnovation/...``). Quarters ``FIRST_QUARTER`` .. latest. A data set holds the filings
EDGAR accepted in that quarter (after 17:30 ET on the quarter's last business day -> next quarter).

Per quarter (resumable; ``receipts.jsonl`` is the append-only ledger): fetch the zip with the approved SEC user
agent through the host-wide limiter (<= 5 req/s, shared 403/429 pause) -> receipt (url, bytes, sha256,
fetched_at) -> extract SUBMISSION / REPORTINGOWNER / NONDERIV_TRANS / DERIV_TRANS one at a time -> DuckDB
transform -> ``transactions/year=YYYY/YYYYqN.parquet`` and ``owners/year=YYYY/YYYYqN.parquet`` -> delete the zip
and the TSVs -> "parsed" receipt.

Rows: one per non-derivative and derivative transaction (``table_type``). Reporting owner: the owner with the
smallest CIK on the filing (``owner_*``) plus every owner's CIK (``reporting_owner_ciks``) and any-owner flags;
the full owner list is ``owners``. Clock: ``available_at`` = EDGAR acceptance of the accession joined from
``sec_filings/filings.parquet`` (``available_basis = 'acceptance'``), else ``filing_date`` + 1 day 00:00
America/New_York (EDGAR closes 22:00 ET; ``'filing_date_eod_et'``). Amendments (``4/A``) are new rows.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import sys
import zipfile
from pathlib import Path
from typing import Any

from . import common as C
from . import sec_filings as SF

STAGE = "insider"
SCHEMA = "atx.alpha-panel.insider/v1"
DATASETS_PAGE = "https://www.sec.gov/data-research/sec-markets-data/insider-transactions-data-sets"
SEC_ROOT = "https://www.sec.gov"
FIRST_QUARTER = "2015q1"
TABLES = ("SUBMISSION", "REPORTINGOWNER", "NONDERIV_TRANS", "DERIV_TRANS")
FORMS_345 = ("3", "4", "5", "3/A", "4/A", "5/A")
DUCKDB_MEMORY = os.environ.get("ATX_INSIDER_DUCKDB_MEMORY", "450MB")
MAX_ZIP_BYTES = 2 * 1024 ** 3
AVAILABLE_RULE = ("available_at = acceptance_utc of the accession in sec_filings/filings.parquet "
                  "(available_basis 'acceptance'; acceptance_clock as in sec_filings, vintage_risk "
                  "'acceptance_clock_unresolved' when that clock is the conservative reading); "
                  "else filing_date + 1 day 00:00 America/New_York "
                  "('filing_date_eod_et', after EDGAR's 22:00 ET close)")

QUARTER_RE = re.compile(r"(\d{4})q([1-4])_form345\.zip", re.IGNORECASE)

# ---------------------------------------------------------------------------------------------------------
# Pure helpers
# ---------------------------------------------------------------------------------------------------------


def quarter_key(q: str) -> tuple[int, int]:
    m = re.fullmatch(r"(\d{4})q([1-4])", q.strip().lower())
    if not m:
        raise ValueError(f"bad quarter {q!r}")
    return int(m.group(1)), int(m.group(2))


def dataset_links(html: str, first: str = FIRST_QUARTER) -> dict[str, str]:
    """``{'2015q1': absolute url, ...}`` for every form345 zip linked on the data-sets page, from ``first`` on."""
    out: dict[str, str] = {}
    lo = quarter_key(first)
    for href in re.findall(r'href="([^"]+_form345\.zip)"', html, flags=re.IGNORECASE):
        m = QUARTER_RE.search(href)
        if not m:
            continue
        q = f"{m.group(1)}q{m.group(2)}"
        if quarter_key(q) < lo:
            continue
        url = href if href.startswith("http") else SEC_ROOT + (href if href.startswith("/") else "/" + href)
        out[q] = url
    return dict(sorted(out.items(), key=lambda kv: quarter_key(kv[0])))


def relationship_flags(rel: str | None) -> dict[str, bool]:
    """``'Director,Officer,TenPercentOwner'`` -> flags (the SQL twin is ``REL_SQL``)."""
    toks = {t.strip().upper() for t in (rel or "").replace(";", ",").split(",") if t.strip()}
    return {"is_director": "DIRECTOR" in toks, "is_officer": "OFFICER" in toks,
            "is_ten_percent_owner": "TENPERCENTOWNER" in toks, "is_other": "OTHER" in toks}


def sec_date_sql(col: str) -> str:
    """SEC data-set dates are ``DD-MON-YYYY`` (``31-MAR-2015``)."""
    return f"CAST(try_strptime(nullif(trim({col}), ''), '%d-%b-%Y') AS DATE)"


def num_sql(col: str) -> str:
    return f"try_cast(nullif(trim({col}), '') AS DOUBLE)"


def rel_sql(col: str, token: str) -> str:
    return f"coalesce(list_contains(string_split(upper(replace(replace({col}, ' ', ''), ';', ',')), ','), '{token}'), false)"


EOD_SQL = ("CAST(timezone('America/New_York', CAST(filing_date AS TIMESTAMP) + INTERVAL 1 DAY) "
           "AT TIME ZONE 'UTC' AS TIMESTAMP)")

# ---------------------------------------------------------------------------------------------------------
# Transform (DuckDB over the extracted TSVs of one quarter)
# ---------------------------------------------------------------------------------------------------------


def _read_tsv(con, path: Path, table: str) -> None:
    opts = ("delim='\\t', header=true, all_varchar=true, quote='\"', escape='\"', strict_mode=false, "
            "null_padding=true, sample_size=-1")
    try:
        con.execute(f"CREATE OR REPLACE TEMP TABLE {table} AS SELECT * FROM read_csv('{path.as_posix()}', {opts})")
    except Exception:  # noqa: BLE001 - non-UTF-8 bytes: fall back to latin-1
        con.execute(f"CREATE OR REPLACE TEMP TABLE {table} AS SELECT * FROM "
                    f"read_csv('{path.as_posix()}', {opts}, encoding='latin-1')")


def _col(con, table: str, name: str) -> str:
    cols = {r[0].upper() for r in con.execute(f"DESCRIBE {table}").fetchall()}
    return name if name.upper() in cols else "NULL"


def transform_quarter(con, tsv_dir: Path, quarter: str, lookup: str | None, tx_dest: Path,
                      own_dest: Path) -> dict[str, Any]:
    """TSVs of one quarter -> typed transactions and owners Parquet. ``lookup``: accession->acceptance parquet."""
    for t in TABLES:
        _read_tsv(con, tsv_dir / f"{t}.tsv", t.lower())
    aff = _col(con, "submission", "AFF10B5ONE")
    look = (f"(SELECT accession, acceptance_utc, acceptance_clock, filing_date AS sub_filing_date "
            f"FROM read_parquet('{lookup}'))"
            if lookup else "(SELECT NULL::VARCHAR AS accession, NULL::TIMESTAMP AS acceptance_utc, "
                           "NULL::VARCHAR AS acceptance_clock, NULL::DATE AS sub_filing_date WHERE false)")
    con.execute(f"""
    CREATE OR REPLACE TEMP TABLE sub_t AS
    SELECT s.ACCESSION_NUMBER AS accession, try_cast(s.ISSUERCIK AS BIGINT) AS issuer_cik,
           s.ISSUERNAME AS issuer_name, nullif(s.ISSUERTRADINGSYMBOL, '') AS issuer_symbol,
           s.DOCUMENT_TYPE AS form, upper(coalesce(s.DOCUMENT_TYPE, '')) LIKE '%/A' AS is_amendment,
           {sec_date_sql('s.FILING_DATE')} AS filing_date, {sec_date_sql('s.PERIOD_OF_REPORT')} AS period_of_report,
           {sec_date_sql('s.DATE_OF_ORIG_SUB')} AS date_of_orig_sub,
           CASE WHEN {aff} IS NULL OR trim({aff}) = '' THEN NULL ELSE upper(trim({aff})) IN ('1', 'TRUE', 'Y') END
               AS aff10b5one,
           l.acceptance_utc, l.acceptance_clock, l.sub_filing_date
    FROM submission s LEFT JOIN {look} l ON l.accession = s.ACCESSION_NUMBER
    """)
    con.execute(f"""
    CREATE OR REPLACE TEMP TABLE own_t AS
    SELECT DISTINCT o.ACCESSION_NUMBER AS accession, try_cast(o.RPTOWNERCIK AS BIGINT) AS owner_cik,
           o.RPTOWNERNAME AS owner_name, nullif(o.RPTOWNER_RELATIONSHIP, '') AS relationship,
           {rel_sql('o.RPTOWNER_RELATIONSHIP', 'DIRECTOR')} AS is_director,
           {rel_sql('o.RPTOWNER_RELATIONSHIP', 'OFFICER')} AS is_officer,
           nullif(o.RPTOWNER_TITLE, '') AS officer_title,
           {rel_sql('o.RPTOWNER_RELATIONSHIP', 'TENPERCENTOWNER')} AS is_ten_percent_owner,
           {rel_sql('o.RPTOWNER_RELATIONSHIP', 'OTHER')} AS is_other,
           nullif(o.RPTOWNER_TXT, '') AS other_text
    FROM reportingowner o
    """)
    con.execute("""
    CREATE OR REPLACE TEMP TABLE own_agg AS
    SELECT accession, count(*) AS n_reporting_owners, list(owner_cik ORDER BY owner_cik) AS reporting_owner_ciks,
           arg_min(owner_cik, owner_cik) AS owner_cik, arg_min(owner_name, owner_cik) AS owner_name,
           arg_min(is_director, owner_cik) AS is_director, arg_min(is_officer, owner_cik) AS is_officer,
           arg_min(officer_title, owner_cik) AS officer_title,
           arg_min(is_ten_percent_owner, owner_cik) AS is_ten_percent_owner, arg_min(is_other, owner_cik) AS is_other,
           arg_min(other_text, owner_cik) AS other_text,
           bool_or(is_director) AS any_director, bool_or(is_officer) AS any_officer,
           bool_or(is_ten_percent_owner) AS any_ten_percent_owner
    FROM own_t GROUP BY accession
    """)
    nd, de = "nonderiv_trans", "deriv_trans"
    ex_col = next((f"t.{c}" for c in ("EXCERCISE_DATE", "EXERCISE_DATE") if _col(con, de, c) != "NULL"), "NULL")
    tx_sql = f"""
    WITH tx AS (
        SELECT 'non_derivative' AS table_type, t.ACCESSION_NUMBER AS accession,
               try_cast(t.NONDERIV_TRANS_SK AS BIGINT) AS trans_sk, t.SECURITY_TITLE AS security_title,
               {sec_date_sql('t.TRANS_DATE')} AS transaction_date,
               {sec_date_sql('t.DEEMED_EXECUTION_DATE')} AS deemed_execution_date,
               nullif(t.TRANS_FORM_TYPE, '') AS trans_form_type, nullif(t.TRANS_CODE, '') AS transaction_code,
               t.EQUITY_SWAP_INVOLVED IN ('1', 'true', 'TRUE') AS equity_swap,
               nullif(t.TRANS_TIMELINESS, '') AS timeliness,
               {num_sql('t.TRANS_SHARES')} AS shares, {num_sql('t.TRANS_PRICEPERSHARE')} AS price,
               nullif(t.TRANS_ACQUIRED_DISP_CD, '') AS acquired_disposed,
               {num_sql('t.SHRS_OWND_FOLWNG_TRANS')} AS shares_owned_after,
               nullif(t.DIRECT_INDIRECT_OWNERSHIP, '') AS direct_indirect,
               nullif(t.NATURE_OF_OWNERSHIP, '') AS nature_of_ownership,
               NULL::DOUBLE AS conv_exercise_price, NULL::DATE AS exercise_date, NULL::DATE AS expiration_date,
               NULL::VARCHAR AS underlying_title, NULL::DOUBLE AS underlying_shares, NULL::DOUBLE AS total_value,
               coalesce(t.TRANS_PRICEPERSHARE_FN, '') <> '' AS price_footnoted,
               coalesce(t.TRANS_SHARES_FN, '') <> '' AS shares_footnoted
        FROM {nd} t
        UNION ALL
        SELECT 'derivative', t.ACCESSION_NUMBER, try_cast(t.DERIV_TRANS_SK AS BIGINT), t.SECURITY_TITLE,
               {sec_date_sql('t.TRANS_DATE')}, {sec_date_sql('t.DEEMED_EXECUTION_DATE')},
               nullif(t.TRANS_FORM_TYPE, ''), nullif(t.TRANS_CODE, ''),
               t.EQUITY_SWAP_INVOLVED IN ('1', 'true', 'TRUE'), nullif(t.TRANS_TIMELINESS, ''),
               {num_sql('t.TRANS_SHARES')}, {num_sql('t.TRANS_PRICEPERSHARE')}, nullif(t.TRANS_ACQUIRED_DISP_CD, ''),
               {num_sql('t.SHRS_OWND_FOLWNG_TRANS')}, nullif(t.DIRECT_INDIRECT_OWNERSHIP, ''),
               nullif(t.NATURE_OF_OWNERSHIP, ''), {num_sql('t.CONV_EXERCISE_PRICE')},
               {sec_date_sql(ex_col)},
               {sec_date_sql('t.EXPIRATION_DATE')}, nullif(t.UNDLYNG_SEC_TITLE, ''),
               {num_sql('t.UNDLYNG_SEC_SHARES')}, {num_sql('t.TRANS_TOTAL_VALUE')},
               coalesce(t.TRANS_PRICEPERSHARE_FN, '') <> '', coalesce(t.TRANS_SHARES_FN, '') <> ''
        FROM {de} t
    )
    SELECT tx.table_type, s.issuer_cik, s.issuer_name, s.issuer_symbol, tx.accession, s.form, s.is_amendment,
           s.filing_date, s.period_of_report, s.date_of_orig_sub, s.aff10b5one,
           a.owner_cik, a.owner_name, a.is_director, a.is_officer, a.officer_title, a.is_ten_percent_owner,
           a.is_other, a.other_text, a.n_reporting_owners, a.reporting_owner_ciks, a.any_director, a.any_officer,
           a.any_ten_percent_owner,
           tx.trans_sk, tx.security_title, tx.transaction_date, tx.deemed_execution_date, tx.trans_form_type,
           tx.transaction_code, tx.equity_swap, tx.timeliness, tx.shares, tx.price, tx.acquired_disposed,
           tx.shares_owned_after, tx.direct_indirect, tx.nature_of_ownership, tx.conv_exercise_price,
           tx.exercise_date, tx.expiration_date, tx.underlying_title, tx.underlying_shares, tx.total_value,
           tx.price_footnoted, tx.shares_footnoted,
           s.acceptance_utc, s.acceptance_clock,
           CASE WHEN s.acceptance_clock = 'unresolved_conservative' THEN 'acceptance_clock_unresolved' END
               AS vintage_risk,
           coalesce(s.acceptance_utc, {EOD_SQL.replace('filing_date', 's.filing_date')}) AS available_at,
           CASE WHEN s.acceptance_utc IS NOT NULL THEN 'acceptance' ELSE 'filing_date_eod_et' END AS available_basis,
           '{quarter}' AS dataset_quarter
    FROM tx LEFT JOIN sub_t s USING (accession) LEFT JOIN own_agg a USING (accession)
    ORDER BY s.filing_date, tx.accession, tx.table_type DESC, tx.trans_sk
    """
    n_tx = C.copy_to_parquet(con, tx_sql, tx_dest)
    n_own = C.copy_to_parquet(con, f"""
        SELECT o.*, s.issuer_cik, s.form, s.filing_date, s.acceptance_utc, s.acceptance_clock,
               coalesce(s.acceptance_utc, {EOD_SQL.replace('filing_date', 's.filing_date')}) AS available_at,
               CASE WHEN s.acceptance_utc IS NOT NULL THEN 'acceptance' ELSE 'filing_date_eod_et' END AS available_basis,
               '{quarter}' AS dataset_quarter
        FROM own_t o LEFT JOIN sub_t s USING (accession) ORDER BY s.filing_date, o.accession, o.owner_cik""", own_dest)
    stats = con.execute(f"""
        SELECT count(*) AS submissions,
               count(*) FILTER (WHERE acceptance_utc IS NOT NULL) AS with_acceptance,
               count(*) FILTER (WHERE acceptance_utc IS NOT NULL AND sub_filing_date <> filing_date) AS filing_date_mismatch,
               count(*) FILTER (WHERE filing_date IS NULL) AS no_filing_date
        FROM sub_t""").fetchone()
    by_basis = dict(con.execute(f"SELECT available_basis, count(*) FROM read_parquet('{tx_dest.as_posix()}') "
                                "GROUP BY 1").fetchall())
    return {"transactions": n_tx, "owners": n_own, "submissions": int(stats[0]),
            "submissions_with_acceptance": int(stats[1]), "filing_date_mismatch": int(stats[2]),
            "no_filing_date": int(stats[3]), "available_basis": {k: int(v) for k, v in by_basis.items()},
            "raw_rows": {t: int(con.execute(f"SELECT count(*) FROM {t.lower()}").fetchone()[0]) for t in TABLES}}


# ---------------------------------------------------------------------------------------------------------
# Fetch + ledger
# ---------------------------------------------------------------------------------------------------------


def _ledger() -> Path:
    return C.stage_dir(STAGE) / "receipts.jsonl"


def _append(rec: dict[str, Any]) -> None:
    with _ledger().open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(rec, default=str) + "\n")
        fh.flush()
        os.fsync(fh.fileno())


def _receipts() -> list[dict[str, Any]]:
    p = _ledger()
    if not p.exists():
        return []
    return [json.loads(x) for x in p.read_text(encoding="utf-8").splitlines() if x.strip()]


def _now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def _session():
    from .. import sec_http
    return sec_http.sec_session()


def fetch_page() -> tuple[str, dict[str, Any]]:
    resp = _session().get(DATASETS_PAGE, timeout=60)
    resp.raise_for_status()
    body = resp.content
    return body.decode("utf-8", "replace"), {"url": DATASETS_PAGE, "bytes": len(body),
                                             "sha256": hashlib.sha256(body).hexdigest(), "fetched_at": _now()}


def fetch_zip(url: str, dest: Path) -> dict[str, Any]:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".partial")
    h = hashlib.sha256()
    n = 0
    resp = _session().get(url, timeout=300, stream=True)
    try:
        status = int(resp.status_code)
        if status != 200:
            return {"url": url, "http_status": status, "bytes": 0, "sha256": None, "fetched_at": _now()}
        with tmp.open("wb") as fh:
            for chunk in resp.iter_content(1 << 20):
                if not chunk:
                    continue
                n += len(chunk)
                if n > MAX_ZIP_BYTES:
                    raise ValueError(f"{url}: larger than {MAX_ZIP_BYTES} bytes")
                h.update(chunk)
                fh.write(chunk)
    finally:
        resp.close()
    os.replace(tmp, dest)
    return {"url": url, "http_status": 200, "bytes": n, "sha256": h.hexdigest(), "fetched_at": _now()}


def _scratch() -> Path:
    p = C.build_root() / "_tmp" / "insider"
    p.mkdir(parents=True, exist_ok=True)
    return p


def build_lookup(con) -> Path:
    """accession -> acceptance_utc for Forms 3/4/5 from the published sec_filings stage."""
    sec = C.build_root() / SF.STAGE
    man = sec / "manifest.json"
    if not man.exists() or C.read_json(man).get("status") != "complete":
        raise SystemExit(f"{man}: sec_filings stage is not published")
    dest = _scratch() / "acceptance_345.parquet"
    stamp = _scratch() / "acceptance_345.sha256"
    # Keyed on the bytes the lookup is built from (filings.parquet), not on the manifest: republishing the
    # sec_filings manifest (derived tables, verification) must not force every quarter to be reprocessed.
    man_sha = C.sha256_file(sec / "filings.parquet")
    if dest.exists() and stamp.exists() and stamp.read_text().strip() == man_sha:
        return dest
    forms = ", ".join(f"'{f}'" for f in FORMS_345)
    C.copy_to_parquet(con, f"""
        SELECT accession, arg_min(acceptance_utc, r) AS acceptance_utc, arg_min(acceptance_clock, r) AS acceptance_clock,
               min(filing_date) AS filing_date
        FROM (SELECT *, CASE WHEN acceptance_clock LIKE 'file_%' THEN 0 WHEN acceptance_clock = 'accession_consensus'
                             THEN 1 ELSE 2 END AS r
              FROM read_parquet('{(sec / 'filings.parquet').as_posix()}')
              WHERE form IN ({forms}) AND acceptance_utc IS NOT NULL)
        GROUP BY accession""", dest)
    stamp.write_text(man_sha)
    return dest


def _out_paths(quarter: str) -> tuple[Path, Path]:
    y = quarter[:4]
    stage = C.stage_dir(STAGE)
    return (stage / "transactions" / f"year={y}" / f"{quarter}.parquet",
            stage / "owners" / f"year={y}" / f"{quarter}.parquet")


def process_quarter(con, quarter: str, url: str, lookup: Path) -> dict[str, Any]:
    zdir = _scratch()
    zpath = zdir / f"{quarter}_form345.zip"
    fetched = next((r for r in reversed(_receipts()) if r.get("event") == "fetched" and r.get("quarter") == quarter
                    and r.get("url") == url and r.get("http_status") == 200), None)
    if not (zpath.exists() and fetched and C.sha256_file(zpath) == fetched.get("sha256")):
        rec = fetch_zip(url, zpath)
        _append({"event": "fetched", "quarter": quarter, **rec})
        if rec["http_status"] != 200:
            return {"quarter": quarter, "status": f"http_{rec['http_status']}"}
        fetched = rec
    tsv_dir = zdir / quarter
    if tsv_dir.exists():
        shutil.rmtree(tsv_dir)
    tsv_dir.mkdir(parents=True)
    with zipfile.ZipFile(zpath) as zf:
        names = {n.upper(): n for n in zf.namelist()}
        for t in TABLES:
            member = names.get(f"{t}.TSV")
            if member is None:
                raise ValueError(f"{zpath}: missing {t}.tsv")
            with zf.open(member) as src, (tsv_dir / f"{t}.tsv").open("wb") as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
    tx_dest, own_dest = _out_paths(quarter)
    stats = transform_quarter(con, tsv_dir, quarter, lookup.as_posix(), tx_dest, own_dest)
    for t in TABLES:
        con.execute(f"DROP TABLE IF EXISTS {t.lower()}")
    shutil.rmtree(tsv_dir)
    zpath.unlink()
    rec = {"event": "parsed", "quarter": quarter, "url": url, "sha256": fetched["sha256"],
           "bytes": fetched["bytes"], "fetched_at": fetched["fetched_at"], "parsed_at": _now(),
           "zip_deleted": True, "lookup_sha256": C.sha256_file(lookup), **stats}
    _append(rec)
    return rec


def coverage(con) -> dict[str, Any]:
    stage = C.stage_dir(STAGE)
    tx = (stage / "transactions" / "*" / "*.parquet").as_posix()
    rows = con.execute(f"""
        SELECT year(filing_date) AS y, count(*) AS rows,
               count(*) FILTER (WHERE vintage_risk = 'acceptance_clock_unresolved') AS clock_unresolved,
               count(*) FILTER (WHERE table_type = 'non_derivative') AS non_derivative,
               count(*) FILTER (WHERE table_type = 'derivative') AS derivative,
               count(DISTINCT issuer_cik) AS issuers, count(DISTINCT accession) AS filings,
               count(*) FILTER (WHERE available_basis = 'acceptance') AS basis_acceptance,
               count(*) FILTER (WHERE available_basis <> 'acceptance') AS basis_filing_date_eod,
               count(*) FILTER (WHERE is_amendment) AS amendments,
               count(*) FILTER (WHERE transaction_code = 'P') AS open_market_buys,
               count(*) FILTER (WHERE transaction_code = 'S') AS open_market_sales,
               count(DISTINCT issuer_cik) FILTER (WHERE transaction_code IN ('P', 'S')) AS issuers_with_p_or_s
        FROM read_parquet('{tx}', hive_partitioning = false) GROUP BY 1 ORDER BY 1""")
    cols = [d[0] for d in rows.description]
    return {str(r[0]): dict(zip(cols[1:], (int(v) for v in r[1:]))) for r in rows.fetchall()}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--first", default=FIRST_QUARTER)
    ap.add_argument("--limit", type=int, default=None, help="process at most N new quarters")
    ap.add_argument("--no-publish", action="store_true")
    args = ap.parse_args(argv)
    con = C.connect(memory=DUCKDB_MEMORY)
    lookup = build_lookup(con)
    html, page = fetch_page()
    _append({"event": "page", **page})
    links = dataset_links(html, args.first)
    if not links:
        raise SystemExit("no form345 links found on the data-sets page")
    done = {r["quarter"]: r for r in _receipts() if r.get("event") == "parsed"}
    lookup_sha = C.sha256_file(lookup)
    n_new = 0
    for q, url in links.items():
        tx_dest, own_dest = _out_paths(q)
        prev = done.get(q)
        if prev and prev.get("url") == url and prev.get("lookup_sha256") == lookup_sha and tx_dest.exists() \
                and own_dest.exists():
            continue
        if args.limit is not None and n_new >= args.limit:
            break
        disk = shutil.disk_usage(C.build_root())
        if disk.free < 35 * 1024 ** 3 + MAX_ZIP_BYTES:
            raise SystemExit(f"disk floor: {disk.free / 1024 ** 3:.1f} GB free")
        rec = process_quarter(con, q, url, lookup)
        print(json.dumps({k: rec.get(k) for k in ("quarter", "transactions", "owners", "available_basis", "status")},
                         default=str), flush=True)
        n_new += 1
    receipts = _receipts()
    parsed = {r["quarter"]: r for r in receipts if r.get("event") == "parsed"}
    missing = [q for q in links if q not in parsed]
    if args.no_publish or missing:
        print(json.dumps({"missing_quarters": missing}), flush=True)
        con.close()
        return 0 if args.no_publish else 3
    cov = coverage(con)
    con.close()
    sec_manifest = C.build_root() / SF.STAGE / "manifest.json"
    payload = {
        "rules": {
            "rows": "one row per non-derivative and derivative transaction (NONDERIV_TRANS, DERIV_TRANS); holdings "
                    "tables are not loaded",
            "owner": "owner_* = the reporting owner with the smallest CIK on the filing; any_* flags and "
                     "reporting_owner_ciks cover every owner; owners/ holds one row per (accession, owner)",
            "relationship": "RPTOWNER_RELATIONSHIP tokens Director / Officer (+ RPTOWNER_TITLE) / TenPercentOwner / Other",
            "available_at": AVAILABLE_RULE,
            "vintage": "amendments (3/A, 4/A, 5/A) are separate accessions and rows; nothing is overwritten",
            "quarters": f"{args.first}..{max(links, key=quarter_key)} as listed on {DATASETS_PAGE}",
        },
        "staleness": "event data, no staleness",
        "sources": {"datasets_page": page,
                    "quarters": {q: {k: parsed[q].get(k) for k in ("url", "bytes", "sha256", "fetched_at",
                                                                  "zip_deleted")} for q in links},
                    "sec_filings_manifest": {"path": str(sec_manifest), "sha256": C.sha256_file(sec_manifest)}},
        "rows": {"transactions": sum(int(parsed[q]["transactions"]) for q in links),
                 "owners": sum(int(parsed[q]["owners"]) for q in links)},
        "per_quarter": {q: {k: parsed[q].get(k) for k in ("transactions", "owners", "submissions",
                                                          "submissions_with_acceptance", "filing_date_mismatch",
                                                          "available_basis")} for q in links},
        "coverage_per_year": cov,
    }
    print(C.write_stage_manifest(STAGE, SCHEMA, ("common", "sec_filings", "insider"), payload), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
