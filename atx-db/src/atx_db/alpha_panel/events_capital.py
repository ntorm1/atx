"""Stage ``events`` table ``capital`` (S6.2): capital-market events from EDGAR metadata, 2019+ (ruling D7).

Event types (``sec_filings/filings.parquet`` forms unless noted):

* ``ipo``: an issuer's first 424B4 (final Rule 430A prospectus) since 2017 that follows an S-1 / F-1 / S-11
  (or amendment) filed within 540 days, by an issuer with no security line trading more than 5 days before it.
  ``first_session`` = the first session of a security line linked to the CIK (``security_master/lines.parquet``,
  not left-censored) within [-5, +45] days of the 424B4; ``ipo_listed`` when found. ``is_spac``: SIC 6770.
* ``follow_on``: any other 424B4 (a priced, non-shelf underwritten offering).
* ``shelf_takedown``: 424B5 (prospectus supplement under a shelf: primary takedowns, at-the-market programs).
* ``secondary_shelf_takedown``: 424B7 (selling-securityholder supplement).
  Prospectus filings of one issuer and type within ``CLUSTER_DAYS`` (a preliminary supplement then the final
  pricing supplement) form one event: ``available_at`` = the first filing's acceptance (the offering is public),
  ``last_available_at`` / ``last_accession`` = the last one's, ``n_filings``. The security type (common, debt,
  units) is not classified: 424B5 includes the base prospectus, whose text names every registered class.
* ``convert_pricing``: an 8-K whose main document or EX-99 exhibit contains "the pricing of" and "convertible
  senior notes due" / "convertible notes due" / "exchangeable senior notes due" (``convert_*`` full-text queries);
  8-Ks of one issuer within ``CLUSTER_DAYS`` are one event (launch then pricing release).
* ``spin_off``: a first Form 10-12B registration by an issuer with no trading line before it, whose filing text
  names a distribution (``spinoff`` query: "spin-off" / "separation and distribution agreement" / "pro rata
  distribution"); ``first_session`` = the issuer's first linked session within 400 days (``spin_listed``). The parent
  is not identified from metadata.

Clock: ``available_at`` = the disclosing filing's EDGAR acceptance (sec_filings rule); ``event_date`` = its filing
date (report date for 8-Ks). ``security_id`` / ``link_tier`` via ``events_common.attach_security`` (an IPO links to
the new line through the 30-day tolerance).
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

TABLE = "capital"
SCHEMA = "atx.alpha-panel.events.capital/v1"
START = dt.date(2019, 1, 1)
LOOKBACK = dt.date(2017, 1, 1)
CLUSTER_DAYS = 5
REG_FORMS = ("S-1", "S-1/A", "F-1", "F-1/A", "S-11", "S-11/A")
TAKEDOWN = {"424B5": "shelf_takedown", "424B7": "secondary_shelf_takedown"}
CONVERT_QIDS = tuple(f"convert_{i}" for i in range(3))


def _hits_list(qids: tuple[str, ...]) -> str | None:
    files = [ES.fts_hits_path(q).as_posix() for q in qids if ES.fts_hits_path(q).exists()]
    return ", ".join(f"'{f}'" for f in files) if files else None


def build() -> dict[str, Any]:
    filings = EC.sec_stage_path("filings.parquet").as_posix()
    prof = EC.sec_stage_path("issuer_profile.parquet").as_posix()
    lines = (C.build_root() / "security_master" / "lines.parquet").as_posix()
    lt = EC.link_table_path().as_posix()
    con = C.connect(memory="350MB", threads=2)
    forms = ", ".join(f"'{f}'" for f in (*REG_FORMS, "424B4", "424B5", "424B7", "10-12B"))
    con.execute(f"""
        CREATE TABLE f AS
        SELECT cik, accession, form, filing_date, report_date, available_at, acceptance_clock, vintage_risk
        FROM read_parquet('{filings}') WHERE form IN ({forms}) AND filing_date >= DATE '{LOOKBACK}'""")
    # first session of every security line linked to a CIK, and the CIK's earliest trading session
    con.execute(f"""
        CREATE TABLE cs AS
        SELECT DISTINCT l.cik, s.security_id, s.first_session, s.left_censored
        FROM read_parquet('{lt}') l JOIN read_parquet('{lines}') s USING (security_id) WHERE l.cik IS NOT NULL""")
    con.execute("CREATE TABLE cfirst AS SELECT cik, min(first_session) AS cik_first_session FROM cs GROUP BY 1")
    con.execute(f"CREATE TABLE spac AS SELECT DISTINCT cik FROM read_parquet('{prof}') WHERE sic = '6770'")
    # IPOs: first 424B4 of the CIK since LOOKBACK, preceded by a registration statement within 540 days
    con.execute(f"""
        CREATE TABLE b4 AS
        SELECT *, row_number() OVER (PARTITION BY cik ORDER BY available_at, accession) AS k
        FROM f WHERE form = '424B4'""")
    con.execute(f"""
        CREATE TABLE ipo AS
        SELECT b.*, EXISTS (SELECT 1 FROM f r WHERE r.cik = b.cik AND r.form IN ({", ".join(f"'{x}'" for x in REG_FORMS)})
                              AND r.filing_date BETWEEN b.filing_date - INTERVAL 540 DAY AND b.filing_date) AS has_reg,
               (SELECT min(first_session) FROM cs WHERE cs.cik = b.cik AND NOT coalesce(cs.left_censored, false)
                  AND cs.first_session BETWEEN b.filing_date - INTERVAL 5 DAY AND b.filing_date + INTERVAL 45 DAY)
                   AS first_session,
               cf.cik_first_session
        FROM b4 b LEFT JOIN cfirst cf USING (cik) WHERE b.k = 1""")
    con.execute(f"""
        CREATE TABLE ev (cik BIGINT, accession VARCHAR, form VARCHAR, event_type VARCHAR, filing_date DATE,
                         event_date DATE, available_at TIMESTAMP, acceptance_clock VARCHAR, vintage_risk VARCHAR,
                         last_accession VARCHAR, last_available_at TIMESTAMP, n_filings INTEGER, first_session DATE,
                         listed BOOLEAN, is_spac BOOLEAN, evidence VARCHAR)""")
    con.execute(f"""
        INSERT INTO ev
        SELECT cik, accession, form, 'ipo', filing_date, filing_date, available_at, acceptance_clock, vintage_risk,
               accession, available_at, 1, first_session, first_session IS NOT NULL, cik IN (SELECT cik FROM spac),
               'first 424B4 after S-1/F-1/S-11 within 540d; no line trading before'
        FROM ipo WHERE has_reg AND filing_date >= DATE '{START}'
          AND (cik_first_session IS NULL OR cik_first_session >= filing_date - INTERVAL 5 DAY)""")
    con.execute(f"""
        INSERT INTO ev
        SELECT b.cik, b.accession, b.form, 'follow_on', b.filing_date, b.filing_date, b.available_at,
               b.acceptance_clock, b.vintage_risk, b.accession, b.available_at, 1, NULL, NULL,
               b.cik IN (SELECT cik FROM spac), '424B4 not classified as the IPO'
        FROM b4 b WHERE b.filing_date >= DATE '{START}'
          AND b.accession NOT IN (SELECT accession FROM ev WHERE event_type = 'ipo')""")
    # shelf takedowns and convert pricings: cluster one issuer's filings within CLUSTER_DAYS
    parts = [f"""SELECT cik, accession, form, CASE form {" ".join(f"WHEN '{k}' THEN '{v}'" for k, v in TAKEDOWN.items())}
                        END AS event_type, filing_date, filing_date AS event_date, available_at, acceptance_clock,
                        vintage_risk
                 FROM f WHERE form IN ({", ".join(f"'{k}'" for k in TAKEDOWN)}) AND filing_date >= DATE '{START}'"""]
    cv = _hits_list(CONVERT_QIDS)
    if cv:
        parts.append(f"""
            SELECT g.cik, g.accession, g.form, 'convert_pricing', g.filing_date, g.report_date, g.available_at,
                   g.acceptance_clock, g.vintage_risk
            FROM read_parquet('{filings}') g
            WHERE g.form IN ('8-K', '8-K/A') AND g.filing_date >= DATE '{START}'
              AND g.accession IN (SELECT DISTINCT adsh FROM read_parquet([{cv}], union_by_name = true)
                                  WHERE upper(coalesce(file_type, '')) IN ('8-K', '8-K/A')
                                     OR upper(coalesce(file_type, '')) LIKE 'EX-99%')""")
    con.execute("CREATE TABLE tk AS " + " UNION ALL ".join(parts))
    con.execute(f"""
        CREATE TABLE tk2 AS
        SELECT *, sum(new) OVER (PARTITION BY cik, event_type ORDER BY available_at, accession) AS grp FROM (
            SELECT *, CASE WHEN lag(filing_date) OVER (PARTITION BY cik, event_type ORDER BY available_at, accession)
                               >= filing_date - INTERVAL {CLUSTER_DAYS} DAY THEN 0 ELSE 1 END AS new
            FROM tk)""")
    con.execute(f"""
        INSERT INTO ev
        SELECT cik, arg_min(accession, (available_at, accession)), arg_min(form, (available_at, accession)),
               event_type, min(filing_date), arg_min(event_date, (available_at, accession)), min(available_at),
               arg_min(acceptance_clock, (available_at, accession)), arg_min(vintage_risk, (available_at, accession)),
               arg_max(accession, (available_at, accession)), max(available_at), count(*), NULL, NULL,
               cik IN (SELECT cik FROM spac),
               'filings of one issuer and type within {CLUSTER_DAYS} days form one event'
        FROM tk2 GROUP BY cik, event_type, grp""")
    sp = _hits_list(("spinoff",))
    if sp:
        con.execute(f"""
            INSERT INTO ev
            SELECT f.cik, f.accession, f.form, 'spin_off', f.filing_date, f.filing_date, f.available_at,
                   f.acceptance_clock, f.vintage_risk, f.accession, f.available_at, 1,
                   (SELECT min(first_session) FROM cs WHERE cs.cik = f.cik
                      AND cs.first_session BETWEEN f.filing_date AND f.filing_date + INTERVAL 400 DAY),
                   NULL, false, 'first Form 10-12B whose text names a distribution; no line trading before'
            FROM (SELECT *, row_number() OVER (PARTITION BY cik ORDER BY available_at, accession) AS k
                  FROM f WHERE form = '10-12B') f LEFT JOIN cfirst cf USING (cik)
            WHERE f.k = 1 AND f.filing_date >= DATE '{START}'
              AND (cf.cik_first_session IS NULL OR cf.cik_first_session >= f.filing_date)
              AND f.accession IN (SELECT DISTINCT adsh FROM read_parquet([{sp}], union_by_name = true))""")
        con.execute("UPDATE ev SET listed = first_session IS NOT NULL WHERE event_type = 'spin_off'")
    EC.attach_security(con, "ev", "ev2", date_col="filing_date")
    out = C.stage_dir(EC.STAGE) / f"{TABLE}.parquet"
    rows = C.copy_to_parquet(con, """
        SELECT cik, security_id, link_tier, link_basis, is_member_issuer, accession, form, event_type, filing_date,
               event_date, available_at, acceptance_clock, vintage_risk, last_accession, last_available_at,
               n_filings, first_session, listed, is_spac, evidence
        FROM ev2 ORDER BY cik, available_at, accession, event_type""", out)
    o = out.as_posix()
    per_year: dict[str, Any] = {}
    for y, et, n, m, lst in con.execute(f"""
            SELECT year(filing_date), event_type, count(*), count(*) FILTER (WHERE is_member_issuer),
                   count(*) FILTER (WHERE listed) FROM read_parquet('{o}') GROUP BY 1, 2 ORDER BY 1, 2""").fetchall():
        per_year.setdefault(str(y), {})[et] = {"events": int(n), "member_issuer": int(m), "listed": int(lst)}
    receipt = {"rows": rows, "per_year": per_year,
               "fts_hit_files": {q: ES.fts_hits_path(q).exists() for q in (*CONVERT_QIDS, "spinoff")},
               "link": dict(con.execute(f"SELECT link_basis, count(*) FROM read_parquet('{o}') GROUP BY 1").fetchall())}
    con.close()
    EC.publish_table(TABLE, ("events_capital", "events_common", "events_sources", "common"), {
        "schema": SCHEMA, "rule": __doc__, **receipt, "peak_memory": ES.peak_memory()})
    return receipt


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.parse_args(argv)
    print(json.dumps(build(), indent=1, default=str), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
