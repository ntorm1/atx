"""P11: foreign/IFRS filers are labeled from retained evidence, never silently missing."""

from __future__ import annotations

import datetime as dt
import json
import zipfile
from pathlib import Path
from typing import Any

import duckdb

from atx_db.foreign_filers import (
    IFRS_REPORTER_REASON,
    NO_XBRL_FINANCIALS_REASON,
    build_foreign_filer_disclosure,
    materialize_taxonomy_scan,
    reason_interval_predicate,
    scan_companyfacts_taxonomies,
)

US_GAAP_20F, IFRS_ONLY_20F, GAAP_TO_IFRS, NO_MEMBER_40F = "0000000001", "0000000002", "0000000003", "0000000004"
# Quote-bearing structure inside a string is escaped by JSON, so the byte scanner must not count it.
ADVERSARIAL = 'x ]}}},"us-gaap":{"Assets":{"units":{"USD":[{"accn":"0-0-0","form":"10-K"}]}}}'


def _fact(accn: str, filed: str, form: str = "20-F") -> dict[str, Any]:
    return {"start": "2018-01-01", "end": "2018-12-31", "val": 1, "accn": accn, "fy": 2019, "fp": "FY",
            "form": form, "filed": filed, "frame": "CY2018"}


def _member(cik: str, taxonomies: dict[str, list[dict[str, Any]]]) -> str:
    facts: dict[str, Any] = {tax: {"Revenue": {"label": "Revenue", "description": ADVERSARIAL, "units": {"USD": rows}}}
                             for tax, rows in taxonomies.items()}
    if "dei" in facts:  # SEC emits concepts with empty units; the dei block then ends with {}}},"<taxonomy>"
        facts["dei"]["Empty"] = {"label": "", "description": "", "units": {}}
    return json.dumps({"cik": int(cik), "entityName": cik, "facts": facts}, separators=(",", ":"))


def test_us_gaap_20f_kept_ifrs_reporters_flagged_point_in_time(tmp_path: Path) -> None:
    cf_zip = tmp_path / "companyfacts.zip"
    with zipfile.ZipFile(cf_zip, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(f"CIK{US_GAAP_20F}.json", _member(US_GAAP_20F, {
            "dei": [_fact("a-20", "2020-04-28")], "us-gaap": [_fact("a-19", "2019-04-30"), _fact("a-20", "2020-04-28")]}))
        # Always IFRS; one stray us-gaap fact inside its IFRS 20-F must not make it a switcher.
        archive.writestr(f"CIK{IFRS_ONLY_20F}.json", _member(IFRS_ONLY_20F, {
            "dei": [_fact("b-20", "2020-04-14")], "us-gaap": [_fact("b-20", "2020-04-14")],
            "ifrs-full": [_fact("b-19", "2019-04-15")] + [_fact("b-20", "2020-04-14")] * 2}))
        # US-GAAP 20-F (50 facts, decisive) -> IFRS 20-F; later a proxy and a partially tagged 20-F with
        # us-gaap facts only are not decisive and must not close the IFRS interval.
        archive.writestr(f"CIK{GAAP_TO_IFRS}.json", _member(GAAP_TO_IFRS, {
            "ifrs-full": [_fact("c-19", "2019-04-18")],
            "us-gaap": [_fact("c-17", "2017-04-20")] * 50 + [_fact("c-24", "2024-05-01", "DEF 14A")] * 60
                       + [_fact("c-25", "2025-04-20")] * 13}))
    con = duckdb.connect(":memory:")
    con.execute("SET memory_limit='256MB'; SET threads=1")
    # sec_submissions shape (warehouse column names); 40-F filer without any companyfacts member.
    con.execute("CREATE TABLE sec_submissions (cik VARCHAR, accession_number VARCHAR, form VARCHAR, filing_date DATE)")
    con.executemany("INSERT INTO sec_submissions VALUES (?, ?, ?, CAST(? AS DATE))", [
        (US_GAAP_20F, "a-20", "20-F", "2020-04-28"), (IFRS_ONLY_20F, "b-20", "20-F", "2020-04-14"),
        (GAAP_TO_IFRS, "c-19", "20-F", "2019-04-18"), (NO_MEMBER_40F, "d-12", "40-F", "2012-03-01"),
    ])
    scan = scan_companyfacts_taxonomies(cf_zip)
    assert [(p.us_gaap_facts, p.ifrs_facts, p.dei_facts) for p in scan.members] == [(2, 0, 1), (1, 3, 1), (123, 1, 0)]
    materialize_taxonomy_scan(con, scan)
    summary = build_foreign_filer_disclosure(con)

    rows = {cik: (basis, reason) for cik, basis, reason in con.execute(
        "SELECT cik, reporting_basis, reason_code FROM foreign_filer_disclosure").fetchall()}
    assert rows == {
        US_GAAP_20F: ("us_gaap", None),  # 20-F under US GAAP is standardized: kept, not flagged
        IFRS_ONLY_20F: ("ifrs_only", IFRS_REPORTER_REASON),
        GAAP_TO_IFRS: ("ifrs_after_us_gaap", IFRS_REPORTER_REASON),
        NO_MEMBER_40F: ("no_companyfacts_member", NO_XBRL_FINANCIALS_REASON),
    }
    assert (summary["flagged_ciks"], summary["foreign_form_filers_us_gaap_kept"]) == (2, 1)
    assert con.execute(f"""SELECT valid_from, valid_to FROM foreign_filer_reason_intervals
                           WHERE cik = '{GAAP_TO_IFRS}'""").fetchall() == [(dt.datetime(2019, 4, 19, 22), None)]
    # The reason starts when the first IFRS filing is available on the fundamentals clock (filed + 46h).
    labeled = con.execute(f"""
        SELECT d.ts, i.reason_code FROM (VALUES (TIMESTAMP '2019-04-19 21:59'), (TIMESTAMP '2019-04-19 22:00'))
            AS d(ts)
        LEFT JOIN foreign_filer_reason_intervals i ON {reason_interval_predicate('i', f"'{GAAP_TO_IFRS}'", 'd.ts')}
        ORDER BY d.ts""").fetchall()
    assert labeled == [(dt.datetime(2019, 4, 19, 21, 59), None), (dt.datetime(2019, 4, 19, 22), IFRS_REPORTER_REASON)]
