"""Stage ``events`` table ``governance`` (S6.6): officer / director changes, auditor changes, non-reliance
restatements and going-concern audit opinions, 2019+ (ruling D7).

Rows (one per disclosing filing and event type):

* ``officer_director_change``: an 8-K / 8-K/A carrying item 5.02 (``sec_filings/eight_k_items.parquet``).
  ``ceo_departure`` / ``cfo_departure``: the filing's 8-K main document or an EX-99 exhibit contains a departure
  construct directly followed by the role title ("resigned as Chief Executive Officer", "retirement as President
  and Chief Executive Officer", "step down as CFO", "from his position as Chief Financial Officer", ...; the
  ``ceo_depart_*`` / ``cfo_depart_*`` EDGAR full-text-search queries of ``events_sources``, which do not stem, so
  every inflection is its own phrase). ``departure_basis = 'fts_phrase'``; NULL flags mean no phrase was found
  (not "no departure": a departure phrased otherwise stays unflagged).
* ``auditor_change``: item 4.01 (changes in registrant's certifying accountant).
* ``non_reliance``: item 4.02 (non-reliance on previously issued financial statements: a restatement).
* ``going_concern``: an original 10-K whose text contains the auditor's going-concern explanatory paragraph wording
  ("management's plans in regard to / regarding these matters are (also) described in Note N", the
  ``going_concern`` query); the row's clock is the 10-K acceptance.

``auditor_name_before`` / ``auditor_name_after`` on ``auditor_change`` rows: ``dei:AuditorName`` of the latest 10-K
accepted before the 8-K and of the first 10-K accepted after it, from lane NOTES' ``identity_cover/cover_page.parquet``
when that stage is published (``auditor_basis``); NULL otherwise.

Clock: ``available_at`` = the disclosing filing's EDGAR acceptance (sec_filings rule); ``event_date`` = the 8-K
report date (the 10-K period end for ``going_concern``). ``security_id`` / ``link_tier`` via
``events_common.attach_security`` on the filing date.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
from typing import Any

from . import common as C
from . import events_common as EC
from . import events_sources as ES

TABLE = "governance"
SCHEMA = "atx.alpha-panel.events.governance/v1"
START = dt.date(2019, 1, 1)
ITEM_EVENTS = {"5.02": "officer_director_change", "4.01": "auditor_change", "4.02": "non_reliance"}
DEPART_QIDS = {"ceo": tuple(f"ceo_depart_{i}" for i in range(3)), "cfo": tuple(f"cfo_depart_{i}" for i in range(3))}


def _hit_files(qids: tuple[str, ...]) -> list[str]:
    return [ES.fts_hits_path(q).as_posix() for q in qids if ES.fts_hits_path(q).exists()]


def _hits_sql(qids: tuple[str, ...]) -> str:
    """Accessions with a hit in an 8-K main document or an EX-99 exhibit (an EX-10 agreement is not evidence)."""
    files = _hit_files(qids)
    if not files:
        return "SELECT CAST(NULL AS VARCHAR) AS adsh WHERE false"
    lst = ", ".join(f"'{f}'" for f in files)
    return f"""SELECT DISTINCT adsh FROM read_parquet([{lst}], union_by_name = true)
               WHERE upper(coalesce(file_type, '')) IN ('8-K', '8-K/A') OR upper(coalesce(file_type, '')) LIKE 'EX-99%'"""


def cover_path() -> Any:
    return C.build_root() / "identity_cover" / "cover_page.parquet"


def build() -> dict[str, Any]:
    items_p = EC.sec_stage_path("eight_k_items.parquet").as_posix()
    filings = EC.sec_stage_path("filings.parquet").as_posix()
    con = C.connect(memory="350MB", threads=2)
    item_list = ", ".join(f"'{i}'" for i in ITEM_EVENTS)
    case = " ".join(f"WHEN '{k}' THEN '{v}'" for k, v in ITEM_EVENTS.items())
    con.execute(f"CREATE TABLE ceo AS {_hits_sql(DEPART_QIDS['ceo'])}")
    con.execute(f"CREATE TABLE cfo AS {_hits_sql(DEPART_QIDS['cfo'])}")
    con.execute(f"""
        CREATE TABLE k AS
        SELECT i.cik, i.accession, i.form, i.is_amendment, i.item, CASE i.item {case} END AS event_type,
               i.filing_date, i.report_date AS event_date, i.available_at, i.acceptance_clock, i.vintage_risk,
               CASE WHEN i.item = '5.02' THEN (i.accession IN (SELECT adsh FROM ceo)) END AS ceo_departure,
               CASE WHEN i.item = '5.02' THEN (i.accession IN (SELECT adsh FROM cfo)) END AS cfo_departure
        FROM read_parquet('{items_p}') i
        WHERE i.item IN ({item_list}) AND i.filing_date >= DATE '{START}'""")
    gc_files = _hit_files(("going_concern",))
    if gc_files:
        lst = ", ".join(f"'{f}'" for f in gc_files)
        con.execute(f"""
            INSERT INTO k
            SELECT f.cik, f.accession, f.form, f.is_amendment, NULL, 'going_concern', f.filing_date,
                   f.report_date, f.available_at, f.acceptance_clock, f.vintage_risk, NULL, NULL
            FROM read_parquet('{filings}') f
            WHERE f.form IN ('10-K', '10-KT') AND f.filing_date >= DATE '{START}'
              AND f.accession IN (SELECT DISTINCT adsh FROM read_parquet([{lst}], union_by_name = true)
                                  WHERE upper(coalesce(file_type, '')) IN ('10-K', '10-KT'))""")
    auditor_basis = "identity_cover not published"
    cov = cover_path()
    if cov.exists():
        con.execute(f"""
            CREATE TABLE aud AS
            SELECT DISTINCT entity_cik AS cik, available_at AS aud_at, auditor_name
            FROM read_parquet('{cov.as_posix()}')
            WHERE form IN ('10-K', '10-K/A', '20-F', '40-F') AND auditor_name IS NOT NULL AND entity_cik IS NOT NULL""")
        con.execute("""
            CREATE TABLE k2 AS
            SELECT k.*, CASE WHEN k.item = '4.01' THEN b.auditor_name END AS auditor_name_before,
                   CASE WHEN k.item = '4.01' THEN a.auditor_name END AS auditor_name_after
            FROM k ASOF LEFT JOIN aud b ON b.cik = k.cik AND k.available_at > b.aud_at
                   ASOF LEFT JOIN aud a ON a.cik = k.cik AND a.aud_at > k.available_at""")
        auditor_basis = "identity_cover/cover_page.parquet dei:AuditorName (10-K/20-F/40-F covers)"
    else:
        con.execute("""CREATE TABLE k2 AS SELECT *, CAST(NULL AS VARCHAR) AS auditor_name_before,
                       CAST(NULL AS VARCHAR) AS auditor_name_after FROM k""")
    EC.attach_security(con, "k2", "k3", date_col="filing_date")
    out = C.stage_dir(EC.STAGE) / f"{TABLE}.parquet"
    rows = C.copy_to_parquet(con, """
        SELECT cik, security_id, link_tier, link_basis, is_member_issuer, accession, form, is_amendment, item,
               event_type, filing_date, event_date, available_at, acceptance_clock, vintage_risk, ceo_departure,
               cfo_departure, CASE WHEN ceo_departure OR cfo_departure THEN 'fts_phrase' END AS departure_basis,
               auditor_name_before, auditor_name_after
        FROM k3 ORDER BY cik, available_at, accession, event_type""", out)
    o = out.as_posix()
    per_year = {}
    for y, et, n, m, ceo, cfo in con.execute(f"""
            SELECT year(filing_date), event_type, count(*), count(*) FILTER (WHERE is_member_issuer),
                   count(*) FILTER (WHERE ceo_departure), count(*) FILTER (WHERE cfo_departure)
            FROM read_parquet('{o}') WHERE NOT is_amendment GROUP BY 1, 2 ORDER BY 1, 2""").fetchall():
        d = per_year.setdefault(str(y), {})
        d[et] = {"rows": int(n), "member_issuer": int(m)}
        if et == "officer_director_change":
            d[et].update(ceo_departure=int(ceo), cfo_departure=int(cfo))
    receipt = {"rows": rows, "per_year_originals": per_year, "auditor_basis": auditor_basis,
               "fts_hit_files": {q: ES.fts_hits_path(q).exists() for qs in DEPART_QIDS.values() for q in qs}
               | {"going_concern": bool(gc_files)},
               "link": dict(con.execute(f"SELECT link_basis, count(*) FROM read_parquet('{o}') GROUP BY 1").fetchall())}
    con.close()
    EC.publish_table(TABLE, ("events_governance", "events_common", "events_sources", "common"), {
        "schema": SCHEMA, "rule": __doc__, **receipt, "peak_memory": ES.peak_memory()})
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.parse_args(argv)
    print(json.dumps(build(), indent=1, default=str), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
