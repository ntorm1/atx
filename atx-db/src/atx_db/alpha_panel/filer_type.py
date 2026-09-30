"""Stage ``thirteenf_filer_type`` (lane OWN, S5.1): a dated institution type for every 13F filer and quarter.

See docs/ALPHA_PANEL_OWNERSHIP.md. Types (``filer_type``): ``hedge_fund``, ``mutual_fund`` (RIC / BDC adviser),
``bank_trust``, ``insurance``, ``pension_endowment`` (public and private pensions, endowments, foundations, sovereign
wealth and central banks), ``broker_dealer``, ``other`` (evidenced: wealth-management and institutional RIAs, private
equity, corporates), and ``unclassified`` (no evidence; never a default). ``filer_subtype`` keeps the finer label.

Inputs:
* ``thirteenf`` stage: ``filers.parquet`` (name, CRD and SEC file number from the cover page, per quarter),
  ``filings.parquet`` and the holdings parts (per-filer SH value, the coverage weight);
* this stage's own landings: ADV monthly rosters and bulk Schedule D tables (:mod:`adv`), and the 13F cover page
  address plus the "other included managers" list (``OTHERMANAGER2``: CRD / SEC file number / 13F file number of
  every manager whose holdings the filing includes), range-read from the 13F data-set zips (:func:`fetch_cover`);
* ``sec_filings``: ``issuer_profile`` (SIC, EDGAR names, former names; a 2026-09-19 snapshot) and ``filings``
  (dated forms filed per CIK: X-17A-5 broker-dealer reports, N-CSR / N-PORT / N-CEN fund reports).

Rules: :func:`classify` (ordered, first match) over the evidence assembled per (filer CIK, quarter P) by
:func:`build`; link methods in :data:`LINK_ORDER`; name normalisation :func:`norm_name`.

Clock: ``available_at`` of a (filer, P) label = the latest clock among the evidence used: the ADV roster's
``available_at`` (roster published by the 13F deadline P + 45 d when one covers the adviser), the 13F filing's
``available_at`` for cover-page evidence, the bulk file's publication for Schedule D evidence
(``vintage_risk`` ``adv_bulk_backfill``). ``filer_type_pit`` repeats the rules on evidence published by the 13F
deadline only (no bulk Schedule D, no later roster).
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import re
import shutil
import sys
import time
from pathlib import Path
from typing import Any

from . import common as C
from . import sec_docs as D
from . import shortflow_common as S

STAGE = "thirteenf_filer_type"
SCHEMA = "atx.alpha-panel.thirteenf-filer-type/v1"
RAW_DIR = S.RAW_ROOT / "sec_adv" / "thirteenf_cover"
COVER_FROM = dt.date(2019, 1, 1)          # data sets whose filings start on or after this date (periods 2018q4+)
COVER_MEMBERS = ("COVERPAGE", "OTHERMANAGER2")
MODULES = ("filer_type", "adv", "sec_docs", "thirteenf", "shortflow_common", "common", "finra_fetch")


def ledger() -> S.Ledger:
    return S.Ledger(RAW_DIR / "receipts.jsonl")


def cover_dir() -> Path:
    p = C.stage_dir(STAGE) / "thirteenf_cover"
    p.mkdir(parents=True, exist_ok=True)
    return p


# ---------------------------------------------------------------- name normalisation
_SUFFIX = {"INC", "INCORPORATED", "LLC", "L L C", "LP", "L P", "LLP", "LTD", "LIMITED", "CORP", "CORPORATION", "CO",
           "COMPANY", "PLC", "NA", "N A", "SA", "AG", "GMBH", "NV", "BV", "THE", "LLLP", "PC", "SE", "SPA", "AB", "ASA",
           "AS", "KG", "PTE", "PTY", "SARL", "SAS", "ULC", "LTDA"}


def norm_name(name: str | None) -> str | None:
    """EDGAR / ADV firm name -> comparison key: upper case, ``&`` -> AND, EDGAR state tags (``/DE/``, ``/ADV``)
    dropped, punctuation to spaces, trailing legal-form words (INC, LLC, LP, CO, ...) and a leading THE removed."""
    if not name:
        return None
    s = name.upper().replace("&", " AND ")
    s = re.sub(r"/[A-Z]{1,4}/?\s*$", " ", s)
    s = re.sub(r"\\+", " ", s)
    s = re.sub(r"[^A-Z0-9 ]+", " ", s)
    toks = s.split()
    if toks and toks[0] == "THE":
        toks = toks[1:]
    while toks and (toks[-1] in _SUFFIX or " ".join(toks[-2:]) in _SUFFIX):
        toks = toks[:-2] if " ".join(toks[-2:]) in _SUFFIX else toks[:-1]
    out = " ".join(toks)
    return out or None


# ---------------------------------------------------------------- 13F cover pages and included managers
def _read_tsv_rows(path: Path, keep: dict[str, str]) -> list[dict[str, str | None]]:
    """Stream a 13F data-set TSV (no quoting) keeping ``keep`` (TSV column -> output name)."""
    out = []
    with path.open(encoding="utf-8", errors="replace", newline="") as fh:
        rd = csv.reader(fh, delimiter="\t", quoting=csv.QUOTE_NONE)
        header = [h.strip().upper() for h in next(rd)]
        idx = {o: header.index(k) for k, o in keep.items() if k in header}
        for row in rd:
            out.append({o: ((row[i].strip() or None) if i < len(row) else None) for o, i in idx.items()})
    return out


COVER_KEEP = {"ACCESSION_NUMBER": "accession", "FILINGMANAGER_NAME": "manager_name",
              "FILINGMANAGER_STREET1": "street1", "FILINGMANAGER_CITY": "city",
              "FILINGMANAGER_STATEORCOUNTRY": "state_or_country", "FILINGMANAGER_ZIPCODE": "zip",
              "REPORTTYPE": "report_type", "CRDNUMBER": "crd_number", "SECFILENUMBER": "sec_file_number",
              "FORM13FFILENUMBER": "form13f_file_number"}
INCLUDED_KEEP = {"ACCESSION_NUMBER": "accession", "SEQUENCENUMBER": "sequence", "CIK": "cik",
                 "FORM13FFILENUMBER": "form13f_file_number", "CRDNUMBER": "crd_number",
                 "SECFILENUMBER": "sec_file_number", "NAME": "name"}


def _write_rows(rows: list[dict[str, Any]], cols: list[str], dest: Path, extra: dict[str, Any]) -> int:
    import pyarrow as pa
    import pyarrow.parquet as pq

    data = {c: pa.array([r.get(c) for r in rows], pa.string()) for c in cols}
    for k, v in extra.items():
        data[k] = pa.array([v] * len(rows), pa.string())
    tmp = dest.with_name(dest.name + ".partial")
    dest.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.table(data), tmp, compression="zstd")
    tmp.replace(dest)
    return len(rows)


def thirteenf_datasets(first: dt.date = COVER_FROM) -> list[tuple[str, str]]:
    """``[(source_period, url)]`` of every parsed 13F data set whose filing dates start on or after ``first``."""
    recs = {}
    for r in S.Ledger(S.RAW_ROOT / "sec_13f" / "receipts.jsonl").records():
        if r.get("kind") == "zip" and r.get("status") == "parsed":
            recs[r["key"]] = r
    out = [(k, r["url"], dt.date.fromisoformat(r["filing_date_range"][0])) for k, r in recs.items()]
    return [(k, u) for k, u, a in sorted(out, key=lambda x: x[2]) if a >= first]


def fetch_cover(limit: int | None = None) -> dict[str, Any]:
    """Range-read COVERPAGE.tsv and OTHERMANAGER2.tsv of each 13F data set -> parquet (network + stdlib + pyarrow)."""
    led = ledger()
    have = led.latest()
    stats = {"datasets": 0, "done_now": 0}
    todo = [(k, u) for k, u in thirteenf_datasets() if not (have.get(k, {}).get("status") == "parsed"
                                                              and (cover_dir() / f"source={k}" / "cover.parquet").exists())]
    stats["datasets"] = len(todo)
    for k, url in todo[:limit] if limit else todo:
        t0 = time.perf_counter()
        entries, info = D.zip_directory(url)
        by = {e["name"].rsplit("/", 1)[-1].upper(): e for e in entries}
        rec: dict[str, Any] = {"key": k, "kind": "13f_cover", "url": url, "zip_bytes": info["zip_bytes"],
                               "last_modified": info["last_modified"], "tail_sha256": info["tail_sha256"], "members": {},
                               "fetched_at": S.utc_now()}
        work = S.tmp_dir("filer_type_cover") / k
        work.mkdir(parents=True, exist_ok=True)
        for m in COVER_MEMBERS:
            e = by.get(f"{m}.TSV")
            if e is None:
                rec["members"][m] = {"status": "absent"}
                continue
            lo, hi = D.member_range(e, info["zip_bytes"])
            raw = RAW_DIR / k / f"{m}.range"
            st, _, h = D.get_range(url, lo, hi, dest=raw)
            if st != 206:
                raise RuntimeError(f"{k} {m}: ranged GET {st}")
            n = D.inflate_range_file(raw, e, work / f"{m}.tsv")
            raw.unlink()
            rec["members"][m] = {"range": f"bytes={lo}-{hi}", "bytes": int(h["x-bytes"]), "sha256": h["x-sha256"],
                                 "inflated_bytes": n, "crc32": f"{e['crc']:08x}"}
        out = cover_dir() / f"source={k}"
        if (work / "COVERPAGE.tsv").exists():
            rows = _read_tsv_rows(work / "COVERPAGE.tsv", COVER_KEEP)
            rec["members"]["COVERPAGE"]["rows"] = _write_rows(rows, list(COVER_KEEP.values()), out / "cover.parquet",
                                                              {"source_period": k})
        if (work / "OTHERMANAGER2.tsv").exists():
            rows = _read_tsv_rows(work / "OTHERMANAGER2.tsv", INCLUDED_KEEP)
            rec["members"]["OTHERMANAGER2"]["rows"] = _write_rows(rows, list(INCLUDED_KEEP.values()),
                                                                  out / "included.parquet", {"source_period": k})
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(RAW_DIR / k, ignore_errors=True)
        rec.update(status="parsed", raw_deleted=True, elapsed_s=round(time.perf_counter() - t0, 1))
        led.append(rec)
        stats["done_now"] += 1
        print(f"  13F cover {k}: {rec['members'].get('COVERPAGE', {}).get('rows')} cover rows, "
              f"{rec['members'].get('OTHERMANAGER2', {}).get('rows')} included managers, {rec['elapsed_s']} s", flush=True)
    return stats


# ---------------------------------------------------------------- rules
BANK_SIC = frozenset({"6021", "6022", "6029", "6035", "6036"})
INSURANCE_SIC = frozenset({"6311", "6321", "6324", "6331", "6351", "6361", "6399", "6411"})
BD_SIC = frozenset({"6211"})
ADVICE_SIC = frozenset({"6282"})
FUND_SIC = frozenset({"6722", "6726"})
TYPES = ("hedge_fund", "mutual_fund", "bank_trust", "insurance", "pension_endowment", "broker_dealer", "other",
         "unclassified")
_W = r"(?:^|\s)({})(?:\s|$)"
SOVEREIGN_RE = re.compile(_W.format(
    "NORGES BANK|SWISS NATIONAL BANK|CENTRAL BANK|NATIONAL BANK OF [A-Z]+|MONETARY AUTHORITY|INVESTMENT AUTHORITY|"
    "SOVEREIGN|GOVERNMENT OF|GOVERNMENT PENSION|STATE OF [A-Z]+ INVESTMENT|TEMASEK|GIC PRIVATE|KUWAIT INVESTMENT|"
    "QATAR INVESTMENT|PUBLIC INVESTMENT FUND|KHAZANAH|MUBADALA|CHINA INVESTMENT CORP|SAFE INVESTMENTS"))
PENSION_RE = re.compile(_W.format(
    "RETIREMENT|PENSION|PENSIONS|PENSIOEN|SUPERANNUATION|TEACHERS|PUBLIC EMPLOYEES|STATE BOARD OF ADMINISTRATION|"
    "INVESTMENT BOARD|PERMANENT FUND|COMMON RETIREMENT|ANNUITY FUND|PROVIDENT FUND|CAISSE DE DEPOT|"
    "CPP INVESTMENT|PSP INVESTMENTS|OMERS|OPTRUST|HEALTHCARE OF ONTARIO|WORKERS COMPENSATION|STATE TREASURER|"
    "TREASURER OF THE STATE|INVESTMENT COUNCIL|EMPLOYEES FUND|ARBEJDSMARKEDETS|ALECTA|AP[1-7]|FORSTA AP|ANDRA AP|"
    "TREDJE AP|FJARDE AP|SJUNDE AP"))
ENDOWMENT_RE = re.compile(_W.format(
    "UNIVERSITY|COLLEGE|ENDOWMENT|REGENTS|TRUSTEES OF|BOARD OF TRUSTEES|INSTITUTE OF TECHNOLOGY|HOSPITAL|"
    "HEALTH SYSTEM|CHURCH|DIOCESE|FOUNDATION INC|CHARITABLE"))
BANK_RE = re.compile(_W.format(
    "BANK|BANKS|BANCORP|BANCORPORATION|BANCSHARES|BANKSHARES|BANKING|BANQUE|BANCO|BANCA|SPARKASSE|"
    "TRUST CO|TRUST COMPANY|TRUST CORP|TRUST COMPANY N A|NATIONAL ASSOCIATION|N A|SAVINGS|FSB|TRUSTCO|"
    "BANK AND TRUST|FIDUCIARY TRUST"))
INSURANCE_RE = re.compile(_W.format(
    "INSURANCE|ASSURANCE|ASSURANCES|REINSURANCE|RE INSURANCE|LIFE INSURANCE|MUTUAL LIFE|CASUALTY|INDEMNITY|"
    "UNDERWRITERS|VERSICHERUNG|VERSICHERUNGS|ASSICURAZIONI|LIFE CO|LIFE ASSURANCE|MUTUAL INSURANCE|ASSURANCE CO"))
ROOT_STOP = frozenset({"FIRST", "AMERICAN", "NATIONAL", "GLOBAL", "CAPITAL", "INVESTMENT", "INVESTMENTS", "ASSET",
                       "WEALTH", "FINANCIAL", "GROUP", "PARTNERS", "UNITED", "NEW", "NORTH", "SOUTH", "EAST", "WEST",
                       "PACIFIC", "ATLANTIC", "INTERNATIONAL", "US", "U", "S", "THE", "STATE", "CENTRAL", "GENERAL",
                       "TRUST", "BANK", "SECURITIES", "ADVISORS", "ADVISERS", "MANAGEMENT", "FUND", "FUNDS",
                       "PRIVATE", "SUMMIT", "PREMIER", "INDEPENDENT", "STRATEGIC", "CAPITOL", "EQUITY", "MARKET",
                       "MARKETS", "TRADING", "ALPHA", "BLUE", "GREEN", "RED", "BLACK", "WHITE", "SILVER", "GOLD",
                       "GOLDEN", "EAGLE", "LION", "STAR", "SUN", "OCEAN", "RIVER", "MOUNTAIN", "PEAK", "ROCK",
                       "STONE", "BRIDGE", "HARBOR", "HARBOUR", "COMMONWEALTH", "HERITAGE", "LEGACY", "PINNACLE"})
HEDGE_MIN_SHARE = 0.5     # hedge-fund GAV share of private-fund GAV, or hedge plurality by fund count
PIV_MIN_SHARE = 0.5       # pooled-investment-vehicle share of Item 5.D RAUM
RIC_MIN_SHARE = 0.5       # registered investment company + BDC share of Item 5.D RAUM
RETAIL_MIN_SHARE = 0.5    # individuals + high-net-worth share (RIA subtype only)


def name_key(raw: str | None) -> str | None:
    """Upper-case, ``&`` -> AND, punctuation -> spaces (legal-form words kept) for the name-token rules."""
    if not raw:
        return None
    s = raw.upper().replace("&", " AND ")
    s = re.sub(r"[^A-Z0-9 ]+", " ", s)
    return " ".join(s.split()) or None


def name_root(norm: str | None) -> str | None:
    """First token of a normalised name when distinctive (>= 4 characters, not a generic word) - used only to join
    an unlinked 13F filer to a broker-dealer (X-17A-5 filer) of the same family (``JANE STREET GROUP`` ->
    ``JANE``)."""
    if not norm:
        return None
    tok = norm.split()[0]
    return tok if len(tok) >= 4 and tok not in ROOT_STOP and not tok.isdigit() else None


def private_fund_kind(ev: dict[str, Any], use_bulk: bool) -> tuple[str | None, str | None]:
    """``('hedge' | 'pe' | None, basis)`` from roster fund counts (2023+ rosters, PIT) else, when allowed, the bulk
    Schedule D 7.B.1 gross asset values (backfill)."""
    nh = ev.get("n_hedge_funds") or 0
    npe = (ev.get("n_pe_funds") or 0) + (ev.get("n_vc_funds") or 0) + (ev.get("n_real_estate_funds") or 0)
    if ev.get("roster_has_fund_types") and (nh or npe):
        return ("hedge" if nh >= npe else "pe"), "adv_roster_fund_counts"
    if use_bulk and (ev.get("bulk_total_gav") or 0) > 0:
        hs = (ev.get("bulk_hedge_gav") or 0) / ev["bulk_total_gav"]
        ps = (ev.get("bulk_pe_gav") or 0) / ev["bulk_total_gav"]
        if hs >= HEDGE_MIN_SHARE or (hs > 0 and hs >= ps):
            return "hedge", "adv_schedule_d_7b1_gav"
        if ps > 0:
            return "pe", "adv_schedule_d_7b1_gav"
    return None, None


def classify_adviser(ev: dict[str, Any], use_bulk: bool) -> tuple[str, str, str] | None:
    """Type from ADV adviser evidence (own adviser or included-manager family); None without ADV evidence."""
    if not ev.get("adv_linked"):
        return None
    tot = ev.get("raum_5d_sum") or 0.0
    share = (lambda k: (ev.get(k) or 0.0) / tot) if tot > 0 else (lambda k: 0.0)
    if ev.get("act_bank") or ev.get("act_trust_company"):
        return "bank_trust", ("bank_adviser" if ev.get("act_bank") else "trust_company"), "adv_item_6a"
    era = bool(ev.get("era"))
    piv = share("raum_f")
    pfa = bool(ev.get("private_fund_adviser")) or era
    if pfa and (era or piv >= PIV_MIN_SHARE):
        kind, basis = private_fund_kind(ev, use_bulk)
        if kind == "hedge":
            return "hedge_fund", ("exempt_reporting_adviser" if era else "registered_adviser"), basis
        if kind == "pe":
            return "other", "private_equity", basis
        if ev.get("fee_performance") and not era:
            return "hedge_fund", "registered_adviser", "adv_piv_performance_fee"
        if era:
            return "other", "private_fund_adviser", "adv_exempt_reporting_adviser"
    ric = share("raum_d") + share("raum_e")
    if ric >= RIC_MIN_SHARE:
        return "mutual_fund", "ric_adviser", "adv_item_5d_ric_share"
    if (ev.get("bd_raum_share") or 0) >= 0.5 or (ev.get("act_broker_dealer") and not ev.get("family")):
        return "broker_dealer", "dual_registrant", "adv_item_6a"
    retail = share("raum_a") + share("raum_b")
    if tot <= 0:
        return "other", "ria_no_raum", "adv_registered_no_5d"
    return "other", ("ria_wealth" if retail >= RETAIL_MIN_SHARE else "ria_institutional"), "adv_item_5d_mix"


def classify(ev: dict[str, Any], use_bulk: bool = True) -> tuple[str, str, str]:
    """Ordered rules over one (filer, quarter) evidence row -> ``(filer_type, filer_subtype, type_basis)``.

    1. own ADV adviser (link methods in :data:`LINK_ORDER`): :func:`classify_adviser`;
    2. name: sovereign / central bank, pension, endowment-foundation -> ``pension_endowment``;
    3. bank: EDGAR SIC 6021/6022/6029/6035/6036 or bank name tokens -> ``bank_trust``;
    4. insurance: SIC 63xx / 6411 or insurance name tokens -> ``insurance``;
    5. included-manager family (OTHERMANAGER2 CRDs in ADV): :func:`classify_adviser` on RAUM summed over the family;
    6. broker-dealer: the CIK files X-17A-5, SIC 6211, or a distinctive name root shared with an X-17A-5 filer;
    7. registered fund (the CIK files N-CSR / N-CEN / NPORT-P) or SIC 6722/6726 -> ``mutual_fund``;
    8. SIC 6282 -> ``other`` / ``investment_adviser_unlinked``; any other SIC -> ``other`` / ``corporate``;
    9. else ``unclassified``.
    """
    own = classify_adviser(ev.get("own") or {}, use_bulk)
    if own is not None:
        return own
    nk = ev.get("name_key") or ""
    sic = ev.get("sic")
    if SOVEREIGN_RE.search(nk):
        return "pension_endowment", "sovereign_central_bank", "name_rule"
    if PENSION_RE.search(nk):
        return "pension_endowment", "pension", "name_rule"
    if ENDOWMENT_RE.search(nk):
        return "pension_endowment", "endowment_foundation", "name_rule"
    if sic in BANK_SIC:
        return "bank_trust", "bank", "edgar_sic"
    if BANK_RE.search(nk):
        return "bank_trust", "bank", "name_rule"
    if sic in INSURANCE_SIC:
        return "insurance", "insurer", "edgar_sic"
    if INSURANCE_RE.search(nk):
        return "insurance", "insurer", "name_rule"
    fam = classify_adviser(ev.get("family") or {}, use_bulk)
    if fam is not None:
        return fam[0], fam[1], "included_managers:" + fam[2]
    if ev.get("x17a5_filer"):
        return "broker_dealer", "broker_dealer", "edgar_x17a5_filer"
    if sic in BD_SIC:
        return "broker_dealer", "broker_dealer", "edgar_sic"
    if ev.get("x17a5_root"):
        return "broker_dealer", "broker_dealer", "name_root_x17a5_filer"
    if ev.get("fund_registrant") or sic in FUND_SIC:
        return "mutual_fund", "registered_fund", "edgar_fund_forms"
    if sic in ADVICE_SIC:
        return "other", "investment_adviser_unlinked", "edgar_sic"
    if sic and sic not in ("0000",) and not sic.startswith("67"):
        return "other", "corporate", "edgar_sic"
    return "unclassified", "unclassified", "no_evidence"


LINK_ORDER = ("cik_roster", "crd_cover", "sec_number_cover", "cik_adv_bulk", "name_state", "name_unique")
FIRST_PERIOD = dt.date(2018, 12, 31)
MEMORY = "400MB"


# ---------------------------------------------------------------- build: per-filer SH value (coverage weight)
def build_values() -> dict[str, Any]:
    """Per (quarter, filer) 13F SH value of the ``final`` effective filings, rule ``13f-row-sanity-v1`` of the
    thirteenf stage (unit repair from ``filing_checks``, price outliers excluded) -> ``filer_values.parquet``."""
    import pyarrow.parquet as pq

    from . import thirteenf as T

    t0 = time.perf_counter()
    tmp = S.tmp_dir("filer_type")
    con = C.connect(memory=MEMORY, threads=2, temp_dir=tmp, db_file="values.duckdb")
    root = C.build_root() / "thirteenf"
    con.execute(f"CREATE VIEW fl AS SELECT * FROM read_parquet('{(root / 'filings.parquet').as_posix()}')")
    con.execute(f"""CREATE TABLE eff AS SELECT e.accession, e.period_q, fl.source_period, CAST(e.filer_cik AS BIGINT) AS fid
                    FROM ({T._effective_sql(None)}) e JOIN fl USING (accession)""")
    periods = [r[0] for r in con.execute(f"SELECT DISTINCT period_q FROM eff WHERE period_q >= DATE '{FIRST_PERIOD}' "
                                         "AND period_q <= current_date ORDER BY 1").fetchall()]
    hglob = (root / "parts" / "source=*" / "holdings.parquet").as_posix()
    chk = (root / "filing_checks.parquet").as_posix()
    dest = C.stage_dir(STAGE) / "filer_values.parquet"
    part = dest.with_name(dest.name + ".partial")
    writer = None
    per_q = {}
    try:
        for p in periods:
            con.execute(f"""CREATE OR REPLACE TABLE h AS
                SELECT e.fid, h.cusip, h.shares, h.value_usd * coalesce(CAST(c.unit_factor AS DOUBLE), 1) AS v,
                       (h.sshprnamt_type = 'SH' AND h.put_call IS NULL AND h.shares > 0) AS sh_row
                FROM read_parquet('{hglob}', hive_partitioning = false) h
                JOIN (SELECT * FROM eff WHERE period_q = DATE '{p}') e USING (accession, source_period)
                LEFT JOIN (SELECT accession, unit_factor FROM read_parquet('{chk}') WHERE period_q = DATE '{p}') c USING (accession)
                WHERE h.period_q = DATE '{p}'""")
            tbl = con.execute(f"""
                WITH cons AS (SELECT cusip, median(v / shares) AS px FROM h WHERE sh_row AND v > 0 GROUP BY 1
                              HAVING count(*) >= {T.CONSENSUS_MIN_ROWS}),
                x AS (SELECT h.*, coalesce(h.sh_row AND h.v > 0 AND c.px IS NOT NULL
                                           AND ((h.v / h.shares) / c.px > {T.PRICE_OUTLIER_X}
                                                OR (h.v / h.shares) / c.px < 1.0 / {T.PRICE_OUTLIER_X}), false)
                             OR coalesce(h.v > {T.IMPLAUSIBLE_VALUE_USD}, false)
                             OR coalesce(h.sh_row AND h.v / h.shares > {T.IMPLAUSIBLE_PRICE_USD}, false) AS outl
                      FROM h LEFT JOIN cons c USING (cusip))
                SELECT DATE '{p}' AS period_q, fid AS filer_cik, sum(v) FILTER (WHERE sh_row AND NOT outl) AS sh_value_usd,
                       count(*) AS n_rows, count(*) FILTER (WHERE sh_row AND NOT outl) AS n_sh_rows
                FROM x GROUP BY fid ORDER BY fid""").to_arrow_table()
            if writer is None:
                writer = pq.ParquetWriter(part, tbl.schema, compression="zstd")
            writer.write_table(tbl)
            per_q[p.isoformat()] = {"filers": tbl.num_rows}
    finally:
        if writer is not None:
            writer.close()
        con.close()
        for f in tmp.glob("values.duckdb*"):
            f.unlink(missing_ok=True)
    part.replace(dest)
    return {"periods": len(periods), "per_quarter": per_q, "elapsed_s": round(time.perf_counter() - t0, 1)}


# ---------------------------------------------------------------- build: links, evidence, labels
def _num(x: str) -> str:
    return f"try_cast(replace(trim({x}), ',', '') AS DOUBLE)"


def _yn(x: str) -> str:
    return f"coalesce(upper(trim({x})) = 'Y', false)"


def _int(x: str) -> str:
    return f"try_cast(nullif(regexp_replace(coalesce({x}, ''), '[^0-9]', '', 'g'), '') AS BIGINT)"


_CT = "abcdefghijklmn"


def _bulk_available_at() -> tuple[dt.datetime, dict[str, Any]]:
    """Publication clock of the bulk tables = the latest zip Last-Modified among the parts used (conservative)."""
    from . import adv as A

    lms = {}
    for k, r in A.ledger().latest().items():
        if k.startswith("bulk:") and r.get("status") == "parsed":
            lms[k] = r.get("zip_last_modified")
    ts = [S.http_date(v) for v in lms.values() if v]
    return max(ts), lms


def _names_table(con, raws: list[str]):
    import pyarrow as pa

    uniq = sorted({r for r in raws if r})
    con.register("nm_arrow", pa.table({"raw": pa.array(uniq, pa.string()),
                                       "norm": pa.array([norm_name(r) for r in uniq], pa.string())}))
    con.execute("CREATE OR REPLACE TABLE nm AS SELECT * FROM nm_arrow")
    con.unregister("nm_arrow")


def build() -> dict[str, Any]:
    import pyarrow as pa
    import pyarrow.parquet as pq

    from . import thirteenf as T

    t0 = time.perf_counter()
    root = C.build_root()
    stage = C.stage_dir(STAGE)
    tmp = S.tmp_dir("filer_type")
    values = stage / "filer_values.parquet"
    if not values.exists():
        raise SystemExit("run `filer_type values` first")
    bulk_av, bulk_lms = _bulk_available_at()
    con = C.connect(memory=MEMORY, threads=2, temp_dir=tmp, db_file="build.duckdb")
    receipt: dict[str, Any] = {"bulk_available_at": bulk_av.isoformat(), "bulk_last_modified": bulk_lms}
    adv = stage / "adv"
    raum = ", ".join(f"{_num('raum_' + x)} AS raum_{x}" for x in _CT)
    con.execute(f"""CREATE TABLE rost AS
        SELECT roster_date, roster_kind, available_at, vintage_risk AS roster_vintage_risk, {_int('crd')} AS crd,
               upper(trim(sec_number)) AS sec_number, {_int('cik')} AS adv_cik, legal_name, business_name,
               upper(trim(state)) AS state, {raum}, {_num('raum_total')} AS raum_total,
               {_yn('fee_performance')} AS fee_performance, {_yn('act_broker_dealer')} AS act_broker_dealer,
               {_yn('act_bank')} AS act_bank, {_yn('act_trust_company')} AS act_trust_company,
               {_yn('private_fund_adviser')} AS private_fund_adviser,
               bool_or(any_hedge_funds IS NOT NULL) OVER (PARTITION BY roster_date, roster_kind) AS roster_has_fund_types,
               {_num('n_hedge_funds')} AS n_hedge_funds, {_num('n_pe_funds')} AS n_pe_funds, {_num('n_vc_funds')} AS n_vc_funds,
               {_num('n_real_estate_funds')} AS n_real_estate_funds, {_num('private_fund_gav')} AS private_fund_gav
        FROM read_parquet('{(adv / 'rosters' / '*.parquet').as_posix()}')""")
    con.execute("DELETE FROM rost WHERE crd IS NULL")
    b = adv / "bulk"
    date_sql = ("coalesce(try_strptime(trim({x}), '%m/%d/%Y %I:%M:%S %p'), try_strptime(trim({x}), '%m/%d/%Y'), "
                "try_strptime(trim({x}), '%Y-%m-%d'))::DATE")
    braum = ", ".join(f"{_num(chr(34) + '5d3' + x + chr(34))} AS raum_{x}" for x in _CT)
    con.execute(f"""CREATE TABLE bulk_f AS
        SELECT 'registered' AS kind, filingid, {_int('"1e1"')} AS crd, upper(trim("1d")) AS sec_number,
               {date_sql.format(x='datesubmitted')} AS filed, "1a" AS legal_name, "1b1" AS business_name,
               upper(trim("1f1_state")) AS state, {_int('"1n_cik"')} AS public_cik, {braum}, {_num('"5f2c"')} AS raum_total,
               {_yn('"5e6"')} AS fee_performance, {_yn('"6a1"')} AS act_broker_dealer, {_yn('"6a7"')} AS act_bank,
               {_yn('"6a8"')} AS act_trust_company, {_yn('"7b"')} AS private_fund_adviser
        FROM read_parquet('{(b / 'ia_adv_base_a.parquet').as_posix()}')
        UNION ALL
        SELECT 'exempt', filingid, {_int('"1e1"')}, upper(trim("1d")), {date_sql.format(x='datesubmitted')}, "1a", "1b1",
               upper(trim("1f1_state")), NULL, {', '.join('NULL' for _ in _CT)}, NULL, false, {_yn('"6a1"')},
               {_yn('"6a7"')}, {_yn('"6a8"')}, {_yn('"7b"')}
        FROM read_parquet('{(b / 'era_adv_base.parquet').as_posix()}')""")
    con.execute(f"""CREATE TABLE bulk_cik AS
        SELECT DISTINCT f.crd, CAST(c.cik AS BIGINT) AS cik, f.filed
        FROM (SELECT filingid, cik FROM read_parquet('{(b / 'ia_1d3_cik.parquet').as_posix()}')
              UNION ALL SELECT filingid, cik FROM read_parquet('{(b / 'era_1d3_cik.parquet').as_posix()}')) c
        JOIN bulk_f f USING (filingid) WHERE f.crd IS NOT NULL AND try_cast(c.cik AS BIGINT) IS NOT NULL
        UNION SELECT crd, public_cik, filed FROM bulk_f WHERE public_cik IS NOT NULL AND crd IS NOT NULL""")
    con.execute(f"""CREATE TABLE bulk_funds AS
        SELECT f.crd, f.filed, sum(g) FILTER (WHERE t LIKE 'HEDGE%') AS bulk_hedge_gav,
               sum(g) FILTER (WHERE t LIKE 'PRIVATE EQUITY%' OR t LIKE 'VENTURE%' OR t LIKE 'REAL ESTATE%') AS bulk_pe_gav,
               sum(g) AS bulk_total_gav, count(*) AS bulk_n_funds
        FROM (SELECT filingid, upper(fund_type) AS t, {_num('gross_asset_value')} AS g
              FROM read_parquet('{(b / 'ia_schedule_d_7b1.parquet').as_posix()}')
              UNION ALL SELECT filingid, upper(fund_type), {_num('gross_asset_value')}
              FROM read_parquet('{(b / 'era_schedule_d_7b1.parquet').as_posix()}')) x
        JOIN bulk_f f USING (filingid) WHERE f.crd IS NOT NULL GROUP BY 1, 2""")
    # 13F side
    fil = (root / "thirteenf" / "filers.parquet").as_posix()
    cov = (stage / "thirteenf_cover" / "source=*" / "cover.parquet").as_posix()
    inc = (stage / "thirteenf_cover" / "source=*" / "included.parquet").as_posix()
    periods = [r[0] for r in con.execute(f"SELECT DISTINCT period_q FROM read_parquet('{fil}') WHERE period_q >= DATE "
                                         f"'{FIRST_PERIOD}' AND period_q <= current_date ORDER BY 1").fetchall()]
    con.execute("CREATE TABLE dl (period_q DATE, cutoff_ts TIMESTAMPTZ)")
    con.executemany("INSERT INTO dl VALUES (?, ?)",
                    [(p, S.at_utc(T.deadline45(p)) + dt.timedelta(hours=46)) for p in periods])
    con.execute(f"""CREATE TABLE fq AS
        SELECT f.period_q, CAST(f.filer_cik AS BIGINT) AS fid, f.filer_name, {_int('f.crd_number')} AS crd_cover,
               upper(trim(f.sec_file_number)) AS sec_cover, f.base_accession, f.first_available_at, dl.cutoff_ts,
               upper(trim(c.state_or_country)) AS cover_state
        FROM read_parquet('{fil}') f JOIN dl USING (period_q)
        LEFT JOIN (SELECT accession, any_value(state_or_country) AS state_or_country
                   FROM read_parquet('{cov}', hive_partitioning = false) GROUP BY 1) c ON c.accession = f.base_accession""")
    con.execute(f"""CREATE TABLE inc AS
        SELECT DISTINCT accession, {_int('crd_number')} AS crd, upper(trim(sec_file_number)) AS sec, {_int('cik')} AS cik, name
        FROM read_parquet('{inc}', hive_partitioning = false)""")
    # EDGAR evidence
    prof = (root / "sec_filings" / "issuer_profile.parquet").as_posix()
    filings = (root / "sec_filings" / "filings.parquet").as_posix()
    con.execute(f"""CREATE TABLE prof AS SELECT cik, nullif(sic, '') AS sic, name FROM read_parquet('{prof}')
                    WHERE cik IN (SELECT DISTINCT fid FROM fq)""")
    con.execute(f"""CREATE TABLE forms AS
        SELECT cik, max(form LIKE 'X-17A-5%') AS x17a5, min(filing_date) FILTER (WHERE form LIKE 'X-17A-5%') AS x17a5_first,
               min(filing_date) FILTER (WHERE form IN ('N-CSR', 'N-CSRS', 'N-CEN', 'NPORT-P')) AS fund_first
        FROM read_parquet('{filings}')
        WHERE form LIKE 'X-17A-5%' OR form IN ('N-CSR', 'N-CSRS', 'N-CEN', 'NPORT-P') GROUP BY 1""")
    con.execute(f"""CREATE TABLE bd_names AS
        SELECT p.cik, p.name, f.x17a5_first FROM read_parquet('{prof}') p JOIN forms f USING (cik) WHERE f.x17a5""")
    # names
    raws = [r[0] for q in ("SELECT DISTINCT filer_name FROM fq", "SELECT DISTINCT legal_name FROM rost",
                           "SELECT DISTINCT business_name FROM rost", "SELECT DISTINCT name FROM bd_names")
            for r in con.execute(q).fetchall()]
    _names_table(con, raws)
    roots = {}
    for _cik, name, first in con.execute("SELECT cik, name, x17a5_first FROM bd_names").fetchall():
        rt = name_root(norm_name(name))
        if rt and (rt not in roots or first < roots[rt]):
            roots[rt] = first
    con.execute("CREATE TABLE bd_roots (root VARCHAR, first_date DATE)")
    if roots:
        con.executemany("INSERT INTO bd_roots VALUES (?, ?)", list(roots.items()))
    con.execute("""CREATE TABLE rn_all AS
        SELECT r.crd, n.norm, r.state, r.available_at FROM rost r JOIN nm n ON n.raw = r.legal_name WHERE n.norm IS NOT NULL
        UNION ALL
        SELECT r.crd, n.norm, r.state, r.available_at FROM rost r JOIN nm n ON n.raw = r.business_name WHERE n.norm IS NOT NULL""")
    con.execute("CREATE TABLE rnames AS SELECT DISTINCT crd, norm, state FROM rn_all")
    con.execute("CREATE TABLE rname_min AS SELECT crd, norm, min(available_at) AS first_av FROM rn_all GROUP BY 1, 2")
    # link candidates (own adviser)
    con.execute(f"""CREATE TABLE links AS
        WITH filers AS (SELECT DISTINCT fid FROM fq),
        known AS (SELECT DISTINCT crd FROM rost)
        SELECT r.adv_cik AS fid, r.crd, 'cik_roster' AS method, min(r.available_at) AS link_from, false AS backfill
        FROM rost r WHERE r.adv_cik IN (SELECT fid FROM filers) GROUP BY 1, 2
        UNION ALL
        SELECT b.cik, b.crd, 'cik_adv_bulk', TIMESTAMPTZ '{bulk_av.isoformat()}', true
        FROM bulk_cik b WHERE b.cik IN (SELECT fid FROM filers) AND b.crd IN (SELECT crd FROM known) GROUP BY 1, 2
        UNION ALL
        SELECT fid, crd_cover, 'crd_cover', min(first_available_at), false FROM fq
        WHERE crd_cover > 0 AND crd_cover IN (SELECT crd FROM known) GROUP BY 1, 2
        UNION ALL
        SELECT q.fid, r.crd, 'sec_number_cover', min(greatest(q.first_available_at, r.available_at)), false
        FROM fq q JOIN rost r ON r.sec_number = q.sec_cover
        WHERE q.sec_cover LIKE '801-%' OR q.sec_cover LIKE '802-%' GROUP BY 1, 2
        UNION ALL
        SELECT q.fid, rn.crd, 'name_state', min(greatest(q.first_available_at, m.first_av)), false
        FROM fq q JOIN nm n ON n.raw = q.filer_name JOIN rnames rn ON rn.norm = n.norm AND rn.state = q.cover_state
        JOIN rname_min m ON m.crd = rn.crd AND m.norm = rn.norm GROUP BY 1, 2
        UNION ALL
        SELECT q.fid, u.crd, 'name_unique', min(greatest(q.first_available_at, u.first_av)), false
        FROM fq q JOIN nm n ON n.raw = q.filer_name
        JOIN (SELECT norm, min(crd) AS crd, min(first_av) AS first_av FROM rname_min GROUP BY 1 HAVING count(DISTINCT crd) = 1) u
          ON u.norm = n.norm GROUP BY 1, 2""")
    order = " ".join(f"WHEN '{m}' THEN {i}" for i, m in enumerate(LINK_ORDER))
    con.execute(f"""CREATE TABLE own AS
        WITH c AS (
            SELECT q.period_q, q.fid, l.crd, l.method, l.backfill, l.link_from, CASE l.method {order} END AS mrank,
                   l.link_from < q.cutoff_ts AND NOT l.backfill AS pit_ok
            FROM fq q JOIN links l USING (fid))
        SELECT period_q, fid, 'full' AS version, arg_min(crd, mrank * 1e12 + crd) AS crd,
               arg_min(method, mrank * 1e12 + crd) AS method, arg_min(link_from, mrank * 1e12 + crd) AS link_from,
               arg_min(backfill, mrank * 1e12 + crd) AS link_backfill
        FROM c GROUP BY 1, 2
        UNION ALL
        SELECT period_q, fid, 'pit', arg_min(crd, mrank * 1e12 + crd), arg_min(method, mrank * 1e12 + crd),
               arg_min(link_from, mrank * 1e12 + crd), false
        FROM c WHERE pit_ok GROUP BY 1, 2""")
    # family: included managers of the base filing -> CRD (direct, via SEC file number, via CIK)
    con.execute("""CREATE TABLE fam AS
        WITH m AS (
            SELECT q.period_q, q.fid, i.crd AS crd_i, i.sec, i.cik FROM fq q JOIN inc i ON i.accession = q.base_accession),
        s AS (SELECT DISTINCT sec_number, crd FROM rost WHERE sec_number LIKE '80%'),
        k AS (SELECT DISTINCT adv_cik AS cik, crd, false AS backfill FROM rost WHERE adv_cik IS NOT NULL
              UNION SELECT cik, crd, true FROM bulk_cik)
        SELECT DISTINCT m.period_q, m.fid, coalesce(r.crd, s.crd, k.crd) AS crd,
               r.crd IS NULL AND s.crd IS NULL AND coalesce(k.backfill, false) AS backfill
        FROM m LEFT JOIN (SELECT DISTINCT crd FROM rost) r ON r.crd = m.crd_i
        LEFT JOIN s ON s.sec_number = m.sec LEFT JOIN k ON k.cik = m.cik
        WHERE coalesce(r.crd, s.crd, k.crd) IS NOT NULL""")
    # roster snapshot for every (quarter, filer, crd, version) needed
    con.execute("""CREATE TABLE need AS
        SELECT DISTINCT o.period_q, o.fid, o.version, o.crd, 'own' AS role FROM own o
        UNION ALL SELECT DISTINCT f.period_q, f.fid, v.version, f.crd, 'family' FROM fam f
        CROSS JOIN (SELECT 'full' AS version UNION ALL SELECT 'pit') v WHERE v.version = 'full' OR NOT f.backfill""")
    con.execute("CREATE TABLE need2 AS SELECT n.*, dl.cutoff_ts FROM need n JOIN dl USING (period_q)")
    rcols = ("roster_date, roster_kind, available_at, roster_vintage_risk, " + ", ".join(f"raum_{x}" for x in _CT) +
             ", raum_total, fee_performance, act_broker_dealer, act_bank, act_trust_company, private_fund_adviser, "
             "roster_has_fund_types, n_hedge_funds, n_pe_funds, n_vc_funds, n_real_estate_funds, private_fund_gav, "
             "legal_name")
    con.execute(f"""CREATE TABLE snap AS
        WITH before AS (
            SELECT n.*, {', '.join('r.' + c.strip() for c in rcols.split(','))}, false AS after_cutoff
            FROM need2 n ASOF LEFT JOIN rost r ON r.crd = n.crd AND n.cutoff_ts > r.available_at),
        after AS (
            SELECT n.period_q, n.fid, n.version, n.crd, n.role, n.cutoff_ts,
                   {', '.join('r.' + c.strip() for c in rcols.split(','))}, true AS after_cutoff
            FROM (SELECT * FROM before WHERE roster_date IS NULL AND version = 'full') n
            ASOF LEFT JOIN rost r ON r.crd = n.crd AND n.cutoff_ts <= r.available_at)
        SELECT * FROM before WHERE roster_date IS NOT NULL OR version = 'pit'
        UNION ALL SELECT * FROM after""")
    con.execute("""CREATE TABLE snapb AS
        SELECT s.*, b.bulk_hedge_gav, b.bulk_pe_gav, b.bulk_total_gav
        FROM snap s ASOF LEFT JOIN bulk_funds b ON b.crd = s.crd AND CAST(s.cutoff_ts AS DATE) >= b.filed""")
    con.execute(f"""CREATE TABLE ev AS
        SELECT q.period_q, q.fid, v.version, q.filer_name, q.first_available_at, q.cutoff_ts, p.sic,
               coalesce(f.x17a5_first < CAST(q.cutoff_ts AS DATE), false) AS x17a5_filer,
               coalesce(f.fund_first < CAST(q.cutoff_ts AS DATE), false) AS fund_registrant,
               coalesce(br.first_date < CAST(q.cutoff_ts AS DATE), false) AS x17a5_root_hit, n.norm AS filer_norm,
               o.crd AS own_crd, o.method AS own_method, o.link_from AS own_link_from, o.link_backfill AS own_link_backfill,
               val.sh_value_usd
        FROM fq q CROSS JOIN (SELECT 'full' AS version UNION ALL SELECT 'pit') v
        LEFT JOIN prof p ON p.cik = q.fid LEFT JOIN forms f ON f.cik = q.fid
        LEFT JOIN nm n ON n.raw = q.filer_name
        LEFT JOIN own o ON o.period_q = q.period_q AND o.fid = q.fid AND o.version = v.version
        LEFT JOIN read_parquet('{values.as_posix()}') val ON val.period_q = q.period_q AND val.filer_cik = q.fid
        LEFT JOIN bd_roots br ON br.root = split_part(n.norm, ' ', 1)""")
    receipt["rows"] = {t: con.execute(f"SELECT count(*) FROM {t}").fetchone()[0]
                       for t in ("rost", "bulk_f", "bulk_cik", "bulk_funds", "fq", "inc", "links", "own", "fam", "snap", "ev")}
    receipt["link_methods"] = dict(con.execute("SELECT method, count(DISTINCT fid) FROM links GROUP BY 1").fetchall())
    out = stage / "filer_type.parquet"
    part = out.with_name(out.name + ".partial")
    schema = _schema()
    writer = None
    per_q: dict[str, Any] = {}
    try:
        for p in periods:
            rows = _classify_quarter(con, p, bulk_av)
            tbl = pa.Table.from_pylist(rows, schema=schema)
            if writer is None:
                writer = pq.ParquetWriter(part, schema, compression="zstd")
            writer.write_table(tbl)
            per_q[p.isoformat()] = len(rows)
    finally:
        if writer is not None:
            writer.close()
    part.replace(out)
    receipt["coverage"] = coverage(con, out)
    receipt["type_value_shares"] = type_shares(con, out)
    receipt["timings_s"] = {"total": round(time.perf_counter() - t0, 1)}
    con.close()
    for f in tmp.glob("build.duckdb*"):
        f.unlink(missing_ok=True)
    publish(receipt)
    return receipt


def _schema():
    import pyarrow as pa

    f = [("period_q", pa.date32()), ("filer_cik", pa.int64()), ("filer_name", pa.string()),
         ("filer_type", pa.string()), ("filer_subtype", pa.string()), ("type_basis", pa.string()),
         ("match_method", pa.string()), ("adviser_crd", pa.int64()), ("adviser_name", pa.string()),
         ("family_n_advisers", pa.int32()), ("roster_date", pa.date32()), ("roster_kind", pa.string()),
         ("roster_after_cutoff", pa.bool_()), ("ric_share", pa.float64()), ("piv_share", pa.float64()),
         ("retail_share", pa.float64()), ("raum_usd", pa.float64()), ("n_hedge_funds", pa.float64()),
         ("n_pe_vc_re_funds", pa.float64()), ("bulk_hedge_gav_share", pa.float64()), ("sic", pa.string()),
         ("sh_value_usd", pa.float64()),
         ("filer_type_pit", pa.string()), ("filer_subtype_pit", pa.string()), ("type_basis_pit", pa.string()),
         ("match_method_pit", pa.string()),
         ("available_at", pa.timestamp("us", tz="UTC")), ("available_at_pit", pa.timestamp("us", tz="UTC")),
         ("vintage_risk", pa.string())]
    return pa.schema(f)


def _adviser_ev(rows: list[dict[str, Any]], family: bool) -> dict[str, Any]:
    """Collapse roster snapshots of one adviser (own) or several (family) into rule evidence."""
    rows = [r for r in rows if r.get("roster_date") is not None]
    if not rows:
        return {}
    ev: dict[str, Any] = {"adv_linked": True, "family": family}
    for x in _CT:
        ev[f"raum_{x}"] = sum((r.get(f"raum_{x}") or 0.0) for r in rows)
    ev["raum_5d_sum"] = sum(ev[f"raum_{x}"] for x in _CT)
    tot = [(r.get("raum_total") or sum((r.get(f"raum_{x}") or 0.0) for x in _CT)) for r in rows]
    t = sum(tot)

    def w(k: str) -> float:
        if t > 0:
            return sum(tt for tt, r in zip(tot, rows, strict=True) if r.get(k)) / t
        return 1.0 if any(r.get(k) for r in rows) else 0.0

    ev["raum_total"] = t
    ev["era"] = all(r.get("roster_kind") == "exempt" for r in rows)
    for k in ("act_bank", "act_trust_company", "act_broker_dealer", "fee_performance", "private_fund_adviser"):
        ev[k] = w(k) >= 0.5 if family else bool(rows[0].get(k))
    ev["bd_raum_share"] = w("act_broker_dealer") if family else None
    ev["roster_has_fund_types"] = any(r.get("roster_has_fund_types") for r in rows)
    for k in ("n_hedge_funds", "n_pe_funds", "n_vc_funds", "n_real_estate_funds", "bulk_hedge_gav", "bulk_pe_gav",
              "bulk_total_gav"):
        ev[k] = sum((r.get(k) or 0.0) for r in rows)
    return ev


def _classify_quarter(con, p: dt.date, bulk_av: dt.datetime) -> list[dict[str, Any]]:
    ev_rows = con.execute(f"SELECT * FROM ev WHERE period_q = DATE '{p}'").fetch_arrow_table().to_pylist()
    snaps: dict[tuple[int, str, str], list[dict[str, Any]]] = {}
    for r in con.execute(f"SELECT * FROM snapb WHERE period_q = DATE '{p}'").fetch_arrow_table().to_pylist():
        snaps.setdefault((r["fid"], r["version"], r["role"]), []).append(r)
    by: dict[int, dict[str, dict[str, Any]]] = {}
    for e in ev_rows:
        by.setdefault(e["fid"], {})[e["version"]] = e
    out = []
    for fid, vs in by.items():
        res = {}
        for version in ("full", "pit"):
            e = vs.get(version) or {}
            own_rows = [r for r in snaps.get((fid, version, "own"), []) if r["crd"] == e.get("own_crd")]
            fam_rows = snaps.get((fid, version, "family"), [])
            own = _adviser_ev(own_rows, family=False)
            if version == "pit":
                for k in ("bulk_hedge_gav", "bulk_pe_gav", "bulk_total_gav"):
                    own.pop(k, None)
            fam = _adviser_ev(fam_rows, family=True)
            evd = {"own": own, "family": fam, "name_key": name_key(e.get("filer_name")), "sic": e.get("sic"),
                   "x17a5_filer": e.get("x17a5_filer"), "x17a5_root": e.get("x17a5_root_hit"),
                   "fund_registrant": e.get("fund_registrant")}
            t, st, basis = classify(evd, use_bulk=(version == "full"))
            used = own if own else (fam if basis.startswith("included_managers") else {})
            src_rows = own_rows if own else (fam_rows if used else [])
            clocks = [e.get("first_available_at")] + [r.get("available_at") for r in src_rows if r.get("available_at")]
            if own and e.get("own_link_from"):
                clocks.append(e["own_link_from"])
            risk = set()
            if version == "full":
                if any(r.get("after_cutoff") for r in src_rows):
                    risk.add("roster_after_cutoff")
                if e.get("own_link_backfill") and own:
                    risk.add("adv_bulk_backfill")
                if "7b1" in basis:
                    risk.add("adv_bulk_backfill")
                if "adv_bulk_backfill" in risk:
                    clocks.append(bulk_av)
            if basis.endswith("edgar_sic"):
                risk.add("sic_snapshot")
            res[version] = (t, st, basis, e, own, fam, src_rows, max(c for c in clocks if c is not None), risk)
        t, st, basis, e, own, fam, src_rows, av, risk = res["full"]
        ev_used = own or fam
        tot = ev_used.get("raum_5d_sum") or 0.0

        def sh(k: str, ev_used: dict[str, Any] = ev_used, tot: float = tot) -> float | None:
            return (ev_used.get(k) or 0.0) / tot if tot > 0 else None

        r0 = src_rows[0] if src_rows else {}
        pt = res["pit"]
        out.append({
            "period_q": p, "filer_cik": fid, "filer_name": e.get("filer_name"), "filer_type": t, "filer_subtype": st,
            "type_basis": basis, "match_method": (e.get("own_method") if own else ("included_managers" if fam and
                                                  basis.startswith("included_managers") else None)),
            "adviser_crd": e.get("own_crd") if own else None, "adviser_name": r0.get("legal_name") if own else None,
            "family_n_advisers": len({r["crd"] for r in src_rows}) if (fam and not own) else None,
            "roster_date": r0.get("roster_date"), "roster_kind": r0.get("roster_kind"),
            "roster_after_cutoff": r0.get("after_cutoff"),
            "ric_share": (sh("raum_d") or 0) + (sh("raum_e") or 0) if tot > 0 else None, "piv_share": sh("raum_f"),
            "retail_share": (sh("raum_a") or 0) + (sh("raum_b") or 0) if tot > 0 else None,
            "raum_usd": ev_used.get("raum_total"), "n_hedge_funds": ev_used.get("n_hedge_funds"),
            "n_pe_vc_re_funds": ((ev_used.get("n_pe_funds") or 0) + (ev_used.get("n_vc_funds") or 0) +
                                 (ev_used.get("n_real_estate_funds") or 0)) if ev_used else None,
            "bulk_hedge_gav_share": ((ev_used.get("bulk_hedge_gav") or 0) / ev_used["bulk_total_gav"])
            if ev_used.get("bulk_total_gav") else None,
            "sic": e.get("sic"), "sh_value_usd": e.get("sh_value_usd"),
            "filer_type_pit": pt[0], "filer_subtype_pit": pt[1], "type_basis_pit": pt[2],
            "match_method_pit": (pt[3].get("own_method") if pt[4] else None),
            "available_at": av, "available_at_pit": pt[7], "vintage_risk": ",".join(sorted(risk)) or None})
    return out


def coverage(con, out: Path) -> dict[str, Any]:
    """Per quarter: share of 13F SH value (final effective filings) whose filer has a type (not ``unclassified``)."""
    rows = con.execute(f"""
        SELECT period_q, count(*), sum(sh_value_usd),
               sum(sh_value_usd) FILTER (WHERE filer_type <> 'unclassified'),
               sum(sh_value_usd) FILTER (WHERE filer_type_pit <> 'unclassified'),
               count(*) FILTER (WHERE filer_type <> 'unclassified'),
               sum(sh_value_usd) FILTER (WHERE vintage_risk IS NULL AND filer_type <> 'unclassified')
        FROM read_parquet('{out.as_posix()}') GROUP BY 1 ORDER BY 1""").fetchall()
    return {str(p): {"filers": n, "sh_value_usd": v, "typed_value_share": round(a / v, 4) if v else None,
                     "typed_value_share_pit": round(b / v, 4) if v else None, "typed_filers": k,
                     "typed_value_share_no_vintage_risk": round((c or 0) / v, 4) if v else None}
            for p, n, v, a, b, k, c in rows}


def type_shares(con, out: Path) -> dict[str, Any]:
    rows = con.execute(f"""
        SELECT year(period_q) AS y, filer_type, sum(sh_value_usd) / sum(sum(sh_value_usd)) OVER (PARTITION BY year(period_q))
        FROM read_parquet('{out.as_posix()}') GROUP BY 1, 2 ORDER BY 1, 2""").fetchall()
    res: dict[str, Any] = {}
    for y, t, s in rows:
        res.setdefault(str(y), {})[t] = round(s or 0, 4)
    return res


def publish(receipt: dict[str, Any]) -> Path:
    from . import adv as A

    root = C.build_root()
    ins = {k: C.sha256_file(root / k / "manifest.json") for k in ("thirteenf", "sec_filings")
           if (root / k / "manifest.json").exists()}
    led_adv, led_cov = A.ledger(), ledger()
    payload = {
        "rules": {
            "types": list(TYPES),
            "classify": classify.__doc__,
            "links": ("own adviser link, first of " + ", ".join(LINK_ORDER) + "; cik_roster = the roster's CIK# (2023+ "
                      "rosters), crd_cover / sec_number_cover = the 13F cover page's CRD / SEC file number, cik_adv_bulk = "
                      "Form ADV Item 1.D(3) / 1.N CIKs in the bulk tables (backfill), name_state = normalised name and "
                      "main-office state equal, name_unique = normalised name held by one CRD only"),
            "family": "OTHERMANAGER2 of the base 13F filing -> CRD directly, via SEC file number or via CIK; RAUM summed",
            "roster_choice": ("latest roster with available_at before the cutoff (13F deadline P + 45 d rolled to the next "
                              "SEC business day, + 46 h); filer_type falls back to the first later roster "
                              "(vintage_risk roster_after_cutoff), filer_type_pit never does"),
            "thresholds": {"hedge_min_share": HEDGE_MIN_SHARE, "piv_min_share": PIV_MIN_SHARE,
                           "ric_min_share": RIC_MIN_SHARE, "retail_min_share": RETAIL_MIN_SHARE},
            "available_at": ("latest clock of the evidence used: the 13F filing (filed + 46 h), the roster, the link's first "
                             "evidence, and the bulk publication when bulk evidence decided (vintage_risk adv_bulk_backfill)"),
            "coverage": "share of per-filer 13F SH value (final effective filings, 13f-row-sanity-v1) with a type",
        },
        "staleness": "quarterly label; applies to the 13F holdings of the same period_q",
        "inputs": {"input_manifests_sha256": ins, "adv_ledger": str(led_adv.path),
                   "adv_ledger_sha256": C.sha256_file(led_adv.path), "cover_ledger": str(led_cov.path),
                   "cover_ledger_sha256": C.sha256_file(led_cov.path)},
        "receipt": receipt,
    }
    return C.write_stage_manifest(STAGE, SCHEMA, MODULES, payload)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch-cover")
    f.add_argument("--limit", type=int)
    sub.add_parser("values")
    sub.add_parser("build")
    args = ap.parse_args(argv)
    if args.cmd == "fetch-cover":
        D.start_memory_trace()
        print(json.dumps(fetch_cover(args.limit)), flush=True)
        print(json.dumps({"peak_memory_gb": D.peak_memory_gb(), "run": "unguarded per C-1 (network landing, no DuckDB)"}),
              flush=True)
    elif args.cmd == "values":
        print(json.dumps(build_values(), default=str)[:4000], flush=True)
    else:
        print(json.dumps(build(), default=str)[:8000], flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
