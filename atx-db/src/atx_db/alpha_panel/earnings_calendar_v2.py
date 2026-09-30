"""Stage ``earnings_calendar_v2`` (S6.5): the v1 8-K 2.02 calendar plus foreign-private-issuer 6-K results releases
and a periodic-report fallback, with timing and expected dates recomputed over the combined per-issuer sequence.

v1 (``earnings_calendar``, read-only) stays as published; v2 is a new stage. One row per disclosing filing:

* ``source = '8k_202'``: every v1 row (original 8-K with item 2.02); ``fiscal_period_end`` as v1.
* ``source = '6k_results'`` (2019+, ruling D7): a 6-K whose EX-99 exhibit (else main document) was fetched for the
  ``fpi_period_results`` / ``fpi_results`` full-text queries and whose opening text announces period results (:func:`classify_release`: a
  results / earnings headline naming a quarter, half year, nine months or (fiscal) year, not a notice of an
  upcoming release, a call invitation, AGM / voting / tender results or an operating update).
  ``fiscal_period_end`` = the first "(quarter | half year | six months | ... | year) ended <date>" in the opening
  text (``period_basis = 'text_date'``), else a label ("third quarter 2024", "H1 2025", "fiscal 2024") mapped with the
  issuer's fiscal-year end (``'text_label'``).
* ``source = 'periodic_report'``: an original 10-Q / 10-K / 10-QT / 10-KT / 20-F / 40-F filed 2018+ for which no
  announcement row of the same CIK and period (8-K: same ``fiscal_period_end``; 6-K: within 10 days) was accepted
  before it: the report is then the first public disclosure of the period's results. ``fiscal_period_end`` = the
  report date.

``timing``, ``session_date``, ``reaction_session``, ``is_primary`` and the expected dates (``yoy_364`` /
``prev_primary_plus_91``) come from ``earnings_calendar.process_cik`` run over each CIK's combined rows in
acceptance order, so v2 values can differ from v1 where a 6-K or a periodic row precedes a 2.02. ``is_fpi``: the CIK
filed a 20-F, 40-F or 6-K since 2018. Clock: ``available_at`` = the filing's EDGAR acceptance (sec_filings rule).

Coverage (the S6.5 done test, ``manifest.json`` ``member_coverage``): per year from 2019, the member issuers
(distinct CIKs of panel ``member_equity`` cells that year) with at least one v2 row whose ``session_date`` falls in
the year, overall, for FPIs, from announcements only (8-K / 6-K) and with the expected count (>= 3 rows; FPIs >= 2).
Metadata only; no returns.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import sys
from pathlib import Path
from typing import Any

import pyarrow as pa
import pyarrow.parquet as pq

from . import common as C
from . import earnings_calendar as EC1
from . import events_sources as ES

STAGE = "earnings_calendar_v2"
SCHEMA = "atx.alpha-panel.earnings-calendar/v2"
CLASSIFIER_VERSION = "fpi-results-v1"
START_6K = dt.date(2019, 1, 1)
START_PERIODIC = dt.date(2018, 1, 1)
PERIODIC_FORMS = ("10-Q", "10-K", "10-QT", "10-KT", "20-F", "40-F")
FPI_MATCH_DAYS = 10
OPENING_CHARS = 2500

_MONTHS = {m: i + 1 for i, m in enumerate(("january", "february", "march", "april", "may", "june", "july", "august",
                                            "september", "october", "november", "december"))}
_MON = r"(January|February|March|April|May|June|July|August|September|October|November|December)"
_PERIOD_WORD = (r"(?:first|second|third|fourth|1st|2nd|3rd|4th|q[1-4]|h[12]|full[- ]year|fiscal(?: year)?|"
                r"half[- ]year|first half|second half|interim|annual|quarterly|six[- ]months?|nine[- ]months?|"
                r"three[- ]months?|twelve[- ]months?|year[- ]end|year|quarter|semi[- ]annual|[0-9]{4}(?:/[0-9]{2})?)")
HEADLINE = re.compile(rf"\b{_PERIOD_WORD}\b[^.\n]{{0,80}}?\b(?:financial |operating |unaudited |consolidated |"
                      rf"preliminary )?(?:results|earnings)\b|\b(?:results|earnings) (?:for|of) (?:the )?"
                      rf"{_PERIOD_WORD}\b", re.IGNORECASE)
NOT_RESULTS = re.compile(
    r"\b(?:will (?:report|announce|release|publish|host|hold)|to (?:report|announce|release|publish)|"
    r"(?:date|timing) (?:of|for) (?:its |the )?(?:release|announcement|publication)|conference call (?:to|on|and "
    r"webcast)|to be (?:held|released|announced)|invitation|release date|notice of)\b|"
    r"\bresults of (?:the )?(?:annual|extraordinary|general|special|voting|vote|tender|exchange|rights|offer|"
    r"meeting|poll|auction|election)|\b(?:agm|egm) results|\bvoting results|\boperating update\b|"
    r"\bproduction (?:update|report)\b|\btraffic (?:figures|results)\b", re.IGNORECASE)
ENDED = re.compile(rf"\b(?:quarter|half[- ]year|half|six months|nine months|three months|twelve months|year|"
                   rf"period|months)\s+(?:ended|ending)\s+(?:on\s+)?{_MON}\s+(\d{{1,2}}),?\s+(\d{{4}})",
                   re.IGNORECASE)
LABEL = re.compile(r"\b(?:(?P<q>first|second|third|fourth|1st|2nd|3rd|4th)[- ]quarter(?: of)?(?: fiscal)?(?: year)?"
                   r"|(?P<qq>q[1-4])|(?P<h>first half|second half|h1|h2|half[- ]year|interim)|(?P<n>nine[- ]months?)"
                   r"|(?P<y>full[- ]year|fiscal(?: year)?|annual|year[- ]end))\s*(?:of\s+)?(?:fiscal\s+|fy\s*)?"
                   r"(?P<yr>(?:19|20)\d{2})\b", re.IGNORECASE)
_Q = {"first": 1, "1st": 1, "second": 2, "2nd": 2, "third": 3, "3rd": 3, "fourth": 4, "4th": 4}


def _month_end(y: int, m: int) -> dt.date:
    return dt.date(y + (m == 12), m % 12 + 1, 1) - dt.timedelta(days=1)


def _fiscal_quarter_end(fy: int, q: int, fye_month: int) -> dt.date:
    """End of quarter ``q`` of the fiscal year labelled ``fy`` (the calendar year in which it ends)."""
    months = fy * 12 + (fye_month - 1) - 3 * (4 - q)
    return _month_end(months // 12, months % 12 + 1)


def classify_release(text: str, filed: dt.date | None, fye_month: int = 12) -> dict[str, Any] | None:
    """Results-release classification of a 6-K document's opening text; None when it is not one.

    Returns ``{"period_end", "period_basis", "headline"}`` (``period_end`` may be None when no period is named)."""
    head = text[:OPENING_CHARS]
    m = HEADLINE.search(head)
    if not m:
        return None
    lo = max(0, m.start() - 160)
    context = head[lo:m.end() + 80]
    if NOT_RESULTS.search(context):
        return None
    headline = " ".join(head[lo:m.end() + 80].split())[:240]
    for e in ENDED.finditer(head):
        try:
            pe = dt.date(int(e.group(3)), _MONTHS[e.group(1).lower()], int(e.group(2)))
        except ValueError:
            continue
        if filed is None or dt.timedelta(0) <= filed - pe <= dt.timedelta(days=400):
            return {"period_end": pe, "period_basis": "text_date", "headline": headline}
    for lb in LABEL.finditer(head):
        fy = int(lb.group("yr"))
        g = {k: (v or "").lower() for k, v in lb.groupdict().items()}
        if g["q"]:
            pe = _fiscal_quarter_end(fy, _Q[g["q"]], fye_month)
        elif g["qq"]:
            pe = _fiscal_quarter_end(fy, int(g["qq"][1]), fye_month)
        elif g["h"]:
            pe = _fiscal_quarter_end(fy, 4 if g["h"] in ("second half", "h2") else 2, fye_month)
        elif g["n"]:
            pe = _fiscal_quarter_end(fy, 3, fye_month)
        else:
            pe = _fiscal_quarter_end(fy, 4, fye_month)
        if filed is None or dt.timedelta(0) <= filed - pe <= dt.timedelta(days=400):
            return {"period_end": pe, "period_basis": "text_label", "headline": headline}
    return {"period_end": None, "period_basis": None, "headline": headline}


# ---------------------------------------------------------------------------------------------------------
# phase parse: classify the fetched 6-K documents (pure Python over the document catalog; ruling C-1)

FPI_SCHEMA = pa.schema([
    ("cik", pa.int64()), ("accession", pa.string()), ("form", pa.string()), ("source_doc", pa.string()),
    ("doc_sha256", pa.string()), ("filing_date", pa.date32()), ("is_results", pa.bool_()),
    ("period_end", pa.date32()), ("period_basis", pa.string()), ("headline", pa.string()),
    ("classifier_version", pa.string()),
])


def _tmp() -> Path:
    d = C.build_root() / "_tmp" / "events"
    d.mkdir(parents=True, exist_ok=True)
    return d


def _fye_months() -> dict[int, int]:
    t = pq.read_table(C.build_root() / "sec_filings" / "issuer_profile.parquet", columns=["cik", "fiscal_year_end"])
    out = {}
    for c, f in zip(t.column("cik").to_pylist(), t.column("fiscal_year_end").to_pylist()):
        if f and len(f) == 4 and f[:2].isdigit() and 1 <= int(f[:2]) <= 12:
            out[c] = int(f[:2])
    return out


def phase_parse() -> dict[str, Any]:
    fye = _fye_months()
    store = ES.open_store()
    rows: list[dict[str, Any]] = []
    seen: set[str] = set()

    def pred(r: dict[str, Any]) -> bool:
        q = r.get("qids") or []
        return (r.get("form") or "").startswith("6-K") and ("fpi_results" in q or "fpi_period_results" in q)

    for r in ES.iter_catalog(pred):
        if r["accession"] in seen or r["filing_date"] is None or r["filing_date"] < START_6K:
            continue
        text = ES.document_text(ES.read_document(r, store), r["source_doc"])
        res = classify_release(text, r["filing_date"], fye.get(r["cik"], 12))
        seen.add(r["accession"])
        rows.append({"cik": r["cik"], "accession": r["accession"], "form": r["form"], "source_doc": r["source_doc"],
                     "doc_sha256": r.get("doc_sha256"), "filing_date": r["filing_date"], "is_results": res is not None,
                     "period_end": res and res["period_end"], "period_basis": res and res["period_basis"],
                     "headline": res and res["headline"], "classifier_version": CLASSIFIER_VERSION})
    dest = _tmp() / "fpi_releases.parquet"
    tmp = dest.with_name(dest.name + ".partial")
    pq.write_table(pa.Table.from_pylist(rows, schema=FPI_SCHEMA), tmp, compression="zstd")
    os.replace(tmp, dest)
    receipt = {"documents": len(rows), "results": sum(r["is_results"] for r in rows),
               "with_period": sum(r["period_end"] is not None for r in rows),
               "text_date": sum(r["period_basis"] == "text_date" for r in rows),
               "peak_memory": ES.peak_memory()}
    C.write_json_atomic(_tmp() / "fpi_parse_receipt.json", receipt)
    return receipt


# ---------------------------------------------------------------------------------------------------------
# phase build: combine, recompute timing / primaries / expected dates per CIK, coverage (DuckDB, guarded)

OUT_SCHEMA = pa.schema([*EC1.OUT_SCHEMA, ("source", pa.string()), ("period_basis", pa.string()),
                        ("is_fpi", pa.bool_()), ("headline", pa.string())])


def _merge_cik(rows: list[dict[str, Any]], cal: EC1.SessionCalendar) -> list[dict[str, Any]]:
    """Drop periodic rows preceded by an announcement of the same period; recompute the v1 fields."""
    ann = [r for r in rows if r["source"] != "periodic_report"]
    keep = list(ann)
    for r in rows:
        if r["source"] != "periodic_report":
            continue
        pe, at = r["fiscal_period_end"], r["announcement_utc"]
        covered = any(a["fiscal_period_end"] is not None and pe is not None and a["announcement_utc"] is not None
                      and at is not None and a["announcement_utc"] <= at
                      and (a["fiscal_period_end"] == pe if a["source"] == "8k_202"
                           else abs((a["fiscal_period_end"] - pe).days) <= FPI_MATCH_DAYS) for a in ann)
        if not covered:
            keep.append(r)
    return EC1.process_cik(keep, cal)


def build_stage() -> dict[str, Any]:
    root = C.build_root()
    v1 = (root / "earnings_calendar" / "announcements.parquet").as_posix()
    filings = (root / "sec_filings" / "filings.parquet").as_posix()
    fpi = _tmp() / "fpi_releases.parquet"
    con = C.connect(memory="350MB", threads=2)
    cal_dates = [r[0] for r in con.execute(
        f"SELECT session_date FROM read_parquet('{C.calendar_path().as_posix()}') ORDER BY 1").fetchall()]
    cal, _audit = EC1.build_calendar(cal_dates)
    forms = ", ".join(f"'{f}'" for f in PERIODIC_FORMS)
    con.execute(f"""
        CREATE TABLE fpi_ciks AS SELECT DISTINCT cik FROM read_parquet('{filings}')
        WHERE form IN ('20-F', '40-F', '6-K') AND filing_date >= DATE '{START_PERIODIC}'""")
    parts = [f"""
        SELECT cik, accession, form, items, filing_date, event_date, announcement_utc, available_at,
               acceptance_clock, vintage_risk, fiscal_period_end, fiscal_period_form, '8k_202' AS source,
               'periodic_report_date' AS period_basis, CAST(NULL AS VARCHAR) AS headline
        FROM read_parquet('{v1}')""",
             f"""
        SELECT cik, accession, form, NULL, filing_date, report_date, acceptance_utc, available_at, acceptance_clock,
               vintage_risk, report_date, form, 'periodic_report', 'report_date', NULL
        FROM read_parquet('{filings}')
        WHERE form IN ({forms}) AND NOT is_amendment AND filing_date >= DATE '{START_PERIODIC}'
          AND report_date IS NOT NULL"""]
    if fpi.exists():
        parts.append(f"""
        SELECT r.cik, r.accession, f.form, NULL, f.filing_date, f.report_date, f.acceptance_utc, f.available_at,
               f.acceptance_clock, f.vintage_risk, r.period_end, NULL, '6k_results', r.period_basis, r.headline
        FROM read_parquet('{fpi.as_posix()}') r JOIN read_parquet('{filings}') f USING (cik, accession)
        WHERE r.is_results""")
    reader = con.execute(f"""
        SELECT u.*, (u.cik IN (SELECT cik FROM fpi_ciks)) AS is_fpi FROM ({" UNION ALL ".join(parts)}) u
        ORDER BY u.cik, u.announcement_utc, u.accession""").fetch_record_batch(50_000)
    dest_dir = C.stage_dir(STAGE)
    dest = dest_dir / "announcements.parquet"
    tmp = dest.with_name(dest.name + ".partial")
    writer = pq.ParquetWriter(tmp, OUT_SCHEMA, compression="zstd")
    pending: list[dict[str, Any]] = []
    cur: int | None = None
    rows: list[dict[str, Any]] = []
    total = 0

    def flush() -> int:
        if not pending:
            return 0
        t = pa.Table.from_pylist(pending, schema=OUT_SCHEMA)
        writer.write_table(t)
        pending.clear()
        return t.num_rows

    for batch in reader:
        for r in batch.to_pylist():
            if r["cik"] != cur:
                if cur is not None:
                    pending.extend(_merge_cik(rows, cal))
                cur, rows = r["cik"], []
            rows.append(r)
        if len(pending) >= 50_000:
            total += flush()
    if cur is not None:
        pending.extend(_merge_cik(rows, cal))
    total += flush()
    writer.close()
    os.replace(tmp, dest)
    receipt: dict[str, Any] = {"rows": total, "fpi_parse": C.read_json(_tmp() / "fpi_parse_receipt.json")
                               if (_tmp() / "fpi_parse_receipt.json").exists() else None}
    a = dest.as_posix()
    receipt["per_year_source"] = {
        str(y): dict(zip(("8k_202", "6k_results", "periodic_report", "primary", "fpi_rows"), map(int, v)))
        for y, *v in con.execute(f"""
            SELECT year(session_date), count(*) FILTER (WHERE source = '8k_202'),
                   count(*) FILTER (WHERE source = '6k_results'), count(*) FILTER (WHERE source = 'periodic_report'),
                   count(*) FILTER (WHERE is_primary), count(*) FILTER (WHERE is_fpi)
            FROM read_parquet('{a}') WHERE session_date >= DATE '2019-01-01' GROUP BY 1 ORDER BY 1""").fetchall()}
    receipt["member_coverage"] = member_coverage(con, dest)
    con.close()
    receipt["peak_memory"] = ES.peak_memory()
    manifest = {"schema": SCHEMA, "status": "complete", "stage": STAGE, "rule": __doc__,
                "classifier_version": CLASSIFIER_VERSION,
                "code": C.code_identity("earnings_calendar_v2", "earnings_calendar", "events_sources", "common"),
                "files": C.output_hashes(dest_dir, "*.parquet"),
                "input_manifests_sha256": {
                    s: {"path": str(root / s / "manifest.json"), "sha256": C.sha256_file(root / s / "manifest.json")}
                    for s in ("earnings_calendar", "sec_filings", "panel") if (root / s / "manifest.json").exists()},
                "clock_rule": "available_at = EDGAR acceptance (UTC) of the disclosing filing; usable at session d "
                              "only if available_at < 22:00 UTC of session d-1",
                **receipt}
    C.write_json_atomic(dest_dir / "manifest.json", manifest)
    return receipt


def member_coverage(con: Any, ann: Path) -> dict[str, Any]:
    """Per year 2019+: member issuers (CIKs of ``member_equity`` panel cells) with >= 1 v2 row that year."""
    panel_root = C.build_root() / "panel"
    a = ann.as_posix()
    out: dict[str, Any] = {}
    con.execute(f"""
        CREATE OR REPLACE TEMP TABLE yr AS
        SELECT cik, year(session_date) AS y, count(*) AS n,
               count(*) FILTER (WHERE source IN ('8k_202', '6k_results')) AS n_ann, bool_or(is_fpi) AS is_fpi
        FROM read_parquet('{a}') WHERE session_date IS NOT NULL GROUP BY 1, 2""")
    for ydir in sorted(panel_root.glob("year=*")):
        y = int(ydir.name.split("=")[1])
        if y < 2019 or not list(ydir.glob("*.parquet")):
            continue
        rec = con.execute(f"""
            WITH m AS (
                SELECT cik, bool_or(cik IS NOT NULL) AS linked, count(DISTINCT security_id) AS lines
                FROM read_parquet('{ydir.as_posix()}/*.parquet') WHERE member_equity GROUP BY 1
            ), j AS (
                SELECT m.cik, coalesce(yr.n, 0) AS n, coalesce(yr.n_ann, 0) AS n_ann,
                       coalesce(yr.is_fpi, m.cik IN (SELECT cik FROM fpi_ciks)) AS is_fpi
                FROM m LEFT JOIN yr ON yr.cik = m.cik AND yr.y = {y} WHERE m.cik IS NOT NULL
            )
            SELECT count(*), count(*) FILTER (WHERE n > 0), count(*) FILTER (WHERE n_ann > 0),
                   count(*) FILTER (WHERE n >= CASE WHEN is_fpi THEN 2 ELSE 3 END),
                   count(*) FILTER (WHERE is_fpi), count(*) FILTER (WHERE is_fpi AND n > 0),
                   count(*) FILTER (WHERE is_fpi AND n_ann > 0),
                   (SELECT count(DISTINCT security_id) FROM read_parquet('{ydir.as_posix()}/*.parquet')
                    WHERE member_equity AND cik IS NULL)
            FROM j""").fetchone()
        issuers, cov, cov_ann, cov_full, fpis, fpi_cov, fpi_ann, unlinked = (int(x or 0) for x in rec)
        out[str(y)] = {"member_issuers": issuers, "covered": cov,
                       "covered_share": round(cov / issuers, 4) if issuers else None,
                       "covered_by_announcement_share": round(cov_ann / issuers, 4) if issuers else None,
                       "expected_count_share": round(cov_full / issuers, 4) if issuers else None,
                       "fpi_issuers": fpis, "fpi_covered_share": round(fpi_cov / fpis, 4) if fpis else None,
                       "fpi_covered_by_announcement_share": round(fpi_ann / fpis, 4) if fpis else None,
                       "unlinked_member_lines": unlinked}
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--phase", choices=("parse", "build", "all"), default="all")
    args = ap.parse_args(argv)
    out: dict[str, Any] = {}
    if args.phase in ("parse", "all"):
        out["parse"] = phase_parse()
    if args.phase in ("build", "all"):
        out["build"] = build_stage()
    print(json.dumps(out, indent=1, default=str), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
