"""B3 ``compustat_std_parts``: year-of-``available_at`` parts of the pinned funda, C5 manifest, acceptance checks."""

from __future__ import annotations

import datetime as dt
import hashlib
import json
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import compustat_std_parts as P

T = dt.datetime
D = dt.date


def _row(cik, datadate, accession, available_at, *, fyear=None, vintage_kind="filing", first_reported=True,
         xsga=10.0, xrd=2.0, fin_template="other"):
    return {"cik": cik, "datadate": datadate, "fyear": fyear or datadate.year, "fyr": 12, "accession": accession,
            "available_at": available_at, "vintage_kind": vintage_kind, "first_reported": first_reported,
            "xsga": xsga, "xrd": xrd, "gdwl": None, "seq": 50.0, "ceq": 45.0, "pstk": 5.0, "txditc": 1.0,
            "at": 100.0, "lt": 50.0, "fin_template": fin_template, "curcd": "USD", "fx_converted": False,
            "form": "10-K", "is_amendment": False, "sale": 77.0}


ROWS = [
    _row(1, D(2022, 12, 31), "a-1", T(2023, 12, 31, 23, 59, 59)),
    _row(2, D(2022, 12, 31), "a-2", T(2024, 1, 1, 0, 0, 0)),
    # a restatement vintage of cik 1's datadate in the same year as the filing: both rows stay
    _row(1, D(2022, 12, 31), "a-3", T(2023, 12, 31, 23, 59, 59), vintage_kind="restatement", first_reported=False,
         xsga=11.0),
    _row(3, D(2009, 12, 31), "a-4", T(2010, 3, 1, 21, 0)),
    _row(4, D(2010, 12, 31), "a-5", None),                       # NULL clock: excluded and counted
    _row(5, D(2011, 6, 30), "a-6", T(2011, 9, 1, 22, 0), xsga=None, fin_template="bank"),
]

SCHEMA = pa.schema([("cik", pa.int64()), ("datadate", pa.date32()), ("fyear", pa.int64()), ("fyr", pa.int64()),
                    ("accession", pa.string()), ("available_at", pa.timestamp("us")),
                    ("vintage_kind", pa.string()), ("first_reported", pa.bool_()), ("xsga", pa.float64()),
                    ("xrd", pa.float64()), ("gdwl", pa.float64()), ("seq", pa.float64()), ("ceq", pa.float64()),
                    ("pstk", pa.float64()), ("txditc", pa.float64()), ("at", pa.float64()), ("lt", pa.float64()),
                    ("fin_template", pa.string()), ("curcd", pa.string()), ("fx_converted", pa.bool_()),
                    ("form", pa.string()), ("is_amendment", pa.bool_()), ("sale", pa.float64())])


def _upstream(root: Path, rows=ROWS) -> str:
    """A published-looking ``compustat_std`` stage; returns the manifest SHA-256 (the pin)."""
    d = root / "compustat_std"
    d.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.Table.from_pylist(rows, schema=SCHEMA), d / "funda.parquet")
    manifest = {"schema": "atx.alpha-panel.compustat-std/v2", "status": "complete", "rule": "cstd-pit-v3",
                "fund_stage": "fundamentals_v10", "build_date": "2026-10-03", "gate_status": {"coverage": "PASS"},
                "code": {"git_head": "abc"},
                "files": {"funda.parquet": {"bytes": (d / "funda.parquet").stat().st_size,
                                            "sha256": C.sha256_file(d / "funda.parquet")}}}
    C.write_json_atomic(d / "manifest.json", manifest)
    return C.sha256_file(d / "manifest.json")


@pytest.fixture
def root(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    return tmp_path


def _built(root: Path) -> tuple[dict, Path]:
    pin = _upstream(root)
    P.build(upstream_manifest_sha256=pin)
    stage = root / P.STAGE
    return C.read_json(stage / "manifest.json"), stage


def _part_rows(stage: Path, year: int) -> list[dict]:
    return pq.read_table(stage / "parts" / f"year={year}" / "funda.parquet").to_pylist()


def test_part_is_the_utc_year_of_available_at(root):
    _, stage = _built(root)
    y23 = _part_rows(stage, 2023)
    y24 = _part_rows(stage, 2024)
    assert {(r["accession"], r["available_at"]) for r in y23} == {("a-1", T(2023, 12, 31, 23, 59, 59)),
                                                                   ("a-3", T(2023, 12, 31, 23, 59, 59))}
    assert [(r["accession"], r["available_at"]) for r in y24] == [("a-2", T(2024, 1, 1))]
    assert sorted(p.name for p in (stage / "parts").iterdir()) == ["year=2010", "year=2011", "year=2023", "year=2024"]


def test_parts_hold_exactly_the_requested_columns(root):
    _, stage = _built(root)
    for part in (stage / "parts").glob("year=*/funda.parquet"):
        assert pq.read_schema(part).names == list(P.COLUMNS)
    assert len(P.COLUMNS) == 20


def test_null_available_at_is_excluded_and_counted(root):
    m, stage = _built(root)
    exported = [r for y in (2010, 2011, 2023, 2024) for r in _part_rows(stage, y)]
    assert "a-5" not in {r["accession"] for r in exported}
    assert m["counts"]["excluded_null_available_at"] == 1
    assert m["counts"]["source_rows"] == len(ROWS)
    assert m["counts"]["exported_rows"] == len(ROWS) - 1
    assert m["excluded"] == [{"reason": P.NULL_CLOCK_REASON, "rows": 1}]


def test_every_vintage_row_is_kept_unchanged(root):
    _, stage = _built(root)
    y23 = sorted(_part_rows(stage, 2023), key=lambda r: r["accession"])
    assert [(r["cik"], r["datadate"], r["vintage_kind"], r["first_reported"], r["xsga"]) for r in y23] == [
        (1, D(2022, 12, 31), "filing", True, 10.0), (1, D(2022, 12, 31), "restatement", False, 11.0)]
    bank = _part_rows(stage, 2011)
    assert bank[0]["xsga"] is None and bank[0]["fin_template"] == "bank"


def test_manifest_follows_the_c5_pinned_stage_contract(root):
    m, stage = _built(root)
    assert m["schema"] == P.SCHEMA and m["status"] == "complete" and m["stage"] == P.STAGE
    assert isinstance(m["rule"], str) and m["rule"] == P.RULE
    assert isinstance(m["clock_rule"], str) and m["clock_rule"] == P.CLOCK_RULE
    assert type(m["staleness_days"]) is int and m["staleness_days"] == P.STALENESS_DAYS
    assert sorted(m["files"]) == [f"parts/year={y}/funda.parquet" for y in (2010, 2011, 2023, 2024)]
    for rel, ent in m["files"].items():
        assert set(ent) == {"bytes", "sha256"} and type(ent["bytes"]) is int and ent["bytes"] >= 0
        assert ent["sha256"] == ent["sha256"].lower() and len(ent["sha256"]) == 64
        assert ent["bytes"] == (stage / rel).stat().st_size and ent["sha256"] == C.sha256_file(stage / rel)
    assert m["counts"]["per_part"] == {"2010": 1, "2011": 1, "2023": 2, "2024": 1}
    up = root / "compustat_std" / "manifest.json"
    assert m["input_manifests_sha256"] == {"compustat_std/manifest.json": C.sha256_file(up)}
    assert m["code"]["compustat_std_parts.py"]["sha256"]
    assert m["columns"] == list(P.COLUMNS)
    # the raw manifest bytes parse back to the same object (the engine pins the raw bytes)
    raw = (stage / "manifest.json").read_bytes()
    assert json.loads(raw) == m and hashlib.sha256(raw).hexdigest() == C.sha256_file(stage / "manifest.json")


def test_self_check_passes_on_a_fresh_build(root):
    _built(root)
    res = P.check_stage(root / P.STAGE, P.CONTRACT)
    assert res["ok"], res
    assert res["files"] == 4 and res["verified"] == 4 and res["bad"] == [] and res["contract_errors"] == []
    assert res["rows_outside_year"] == 0


def test_self_check_does_not_open_an_invalid_manifest_path(root, monkeypatch):
    m, stage = _built(root)
    m["files"]["../outside.parquet"] = {"bytes": 0, "sha256": "0" * 64}
    C.write_json_atomic(stage / "manifest.json", m)
    original = C.sha256_file

    def hash_only_inside(path):
        assert path.resolve().is_relative_to(stage.resolve())
        return original(path)

    monkeypatch.setattr(C, "sha256_file", hash_only_inside)
    res = P.check_stage(stage, P.CONTRACT)
    assert not res["ok"]
    assert {"rel": "../outside.parquet", "why": "invalid part path"} in res["bad"]


def test_tampered_part_fails_the_self_check(root):
    _built(root)
    part = root / P.STAGE / "parts" / "year=2010" / "funda.parquet"
    blob = bytearray(part.read_bytes())
    blob[len(blob) // 2] ^= 0xFF
    part.write_bytes(bytes(blob))
    res = P.check_stage(root / P.STAGE, P.CONTRACT)
    assert not res["ok"]
    assert [b["rel"] for b in res["bad"]] == ["parts/year=2010/funda.parquet"]


def test_part_holding_a_row_of_another_year_fails_the_year_check(root):
    _built(root)
    stage = root / P.STAGE
    part = stage / "parts" / "year=2010" / "funda.parquet"
    tbl = pq.read_table(part)
    moved = tbl.set_column(tbl.schema.get_field_index("available_at"), "available_at",
                           pa.array([T(2011, 1, 1)], pa.timestamp("us")))
    pq.write_table(moved, part)
    m = C.read_json(stage / "manifest.json")             # re-seal so only the year check can catch it
    m["files"]["parts/year=2010/funda.parquet"] = {"bytes": part.stat().st_size, "sha256": C.sha256_file(part)}
    C.write_json_atomic(stage / "manifest.json", m)
    res = P.check_stage(stage, P.CONTRACT)
    assert res["bad"] == [] and res["rows_outside_year"] == 1 and not res["ok"]


def test_contract_violations_are_reported(root):
    _built(root)
    stage = root / P.STAGE
    m = C.read_json(stage / "manifest.json")
    m["staleness_days"] = 400.0
    m["rule"] = [P.RULE]
    m["status"] = "published_conditional_native"
    C.write_json_atomic(stage / "manifest.json", m)
    res = P.check_stage(stage, P.CONTRACT)
    assert not res["ok"]
    assert {e.split(":")[0] for e in res["contract_errors"]} == {"staleness_days", "rule", "status"}


def test_wrong_upstream_pin_is_refused(root):
    _upstream(root)
    with pytest.raises(RuntimeError, match="manifest sha256"):
        P.build(upstream_manifest_sha256="0" * 64)
    assert not (root / P.STAGE / "manifest.json").exists()


def test_upstream_funda_that_moved_is_refused(root):
    pin = _upstream(root)
    with (root / "compustat_std" / "funda.parquet").open("ab") as fh:
        fh.write(b"x")
    with pytest.raises(RuntimeError, match="funda.parquet"):
        P.build(upstream_manifest_sha256=pin)


def test_existing_published_stage_is_never_replaced(root):
    m, stage = _built(root)
    before = C.sha256_file(stage / "manifest.json")
    with pytest.raises(RuntimeError, match="already exists"):
        P.build(upstream_manifest_sha256=C.sha256_file(root / "compustat_std" / "manifest.json"))
    assert C.sha256_file(stage / "manifest.json") == before


def test_xsga_coverage_and_issuer_history_prints(root):
    _built(root)
    con = C.connect(memory="200MB", threads=1)
    try:
        cov = P.xsga_coverage(con, root / P.STAGE, years=range(2009, 2023))
        hist = P.first_reported_issuers(con, root / P.STAGE, years=range(2009, 2024))
        both = P.both_present(con, root / P.STAGE)
    finally:
        con.close()
    # first-reported, non-financial: cik 3 (FY2009) and cik 1/2 (FY2022); the bank row and the restatement are out
    assert cov["by_year"][2009] == {"rows": 1, "with_xsga": 1, "rows_pre2024": 1, "with_xsga_pre2024": 1}
    assert cov["by_year"][2022] == {"rows": 2, "with_xsga": 2, "rows_pre2024": 1, "with_xsga_pre2024": 1}
    assert cov["by_year"][2011] == {"rows": 0, "with_xsga": 0, "rows_pre2024": 0, "with_xsga_pre2024": 0}
    assert hist[2022] == {"issuers": 2, "issuers_pre2024": 1} and hist[2011] == {"issuers": 1, "issuers_pre2024": 1}
    assert hist[2023] == {"issuers": 0, "issuers_pre2024": 0}
    assert both["rows"] == 5 and both["both_present"] == 4 and both["xsga_and_xrd_positive"] == 4


# ---------------------------------------------------------------- SG&A convention (presentation + arithmetic)
def L(line, tag, value, plabel=""):
    return {"line": line, "tag": tag, "value": value, "plabel": plabel or tag}


def test_separate_face_lines_that_add_to_the_total_exclude_rd():
    lines = [L(1, "Revenues", 1000.0), L(2, "CostOfRevenue", 400.0), L(3, "GrossProfit", 600.0),
             L(4, "ResearchAndDevelopmentExpense", 100.0), L(5, "SellingGeneralAndAdministrativeExpense", 200.0),
             L(6, "OperatingExpenses", 300.0), L(7, "OperatingIncomeLoss", 300.0)]
    assert P.classify_sga(lines) == ("excludes_rd", "face_identity_total_equals_sga_plus_rd")


def test_sga_label_naming_rd_includes_rd():
    lines = [L(4, "SellingGeneralAndAdministrativeExpense", 300.0,
               "Selling, general and administrative (including research and development)")]
    assert P.classify_sga(lines) == ("includes_rd", "sga_face_label_names_rd")


def test_rd_shown_inside_sga_by_arithmetic_includes_rd():
    # R&D presented as an "of which" line: the total equals the block without counting R&D twice
    lines = [L(3, "GrossProfit", 600.0), L(4, "SellingGeneralAndAdministrativeExpense", 300.0),
             L(5, "ResearchAndDevelopmentExpense", 100.0), L(6, "OperatingExpenses", 300.0)]
    assert P.classify_sga(lines) == ("includes_rd", "face_identity_total_counts_rd_inside_sga")


def test_undecidable_filings_are_classified_unresolved():
    rd_in_notes = [L(4, "SellingGeneralAndAdministrativeExpense", 300.0), L(6, "OperatingExpenses", 300.0)]
    assert P.classify_sga(rd_in_notes) == ("unresolved", "rd_not_on_face")
    no_sga = [L(4, "ResearchAndDevelopmentExpense", 100.0), L(6, "OperatingExpenses", 300.0)]
    assert P.classify_sga(no_sga) == ("unresolved", "sga_not_on_face")
    no_total = [L(4, "ResearchAndDevelopmentExpense", 100.0), L(5, "SellingGeneralAndAdministrativeExpense", 200.0),
                L(6, "NetIncomeLoss", 300.0)]
    assert P.classify_sga(no_total) == ("unresolved", "no_total_line_after_sga_and_rd")
    # a gain inside the block breaks the simple identity: no guess
    gain = [L(3, "GrossProfit", 600.0), L(4, "ResearchAndDevelopmentExpense", 100.0),
            L(5, "SellingGeneralAndAdministrativeExpense", 200.0), L(6, "GainLossOnSaleOfOtherAssets", 50.0),
            L(7, "OperatingExpenses", 250.0)]
    assert P.classify_sga(gain) == ("unresolved", "face_identity_not_found")
    missing = [L(4, "ResearchAndDevelopmentExpense", None), L(5, "SellingGeneralAndAdministrativeExpense", 200.0),
               L(6, "OperatingExpenses", 300.0)]
    assert P.classify_sga(missing) == ("unresolved", "face_values_missing")
    assert P.classify_sga([]) == ("unresolved", "no_fsds_income_statement")


def test_house_fallback_components_count_as_sga_lines():
    lines = [L(3, "GrossProfit", 600.0), L(4, "ResearchAndDevelopmentExpense", 100.0),
             L(5, "SellingAndMarketingExpense", 120.0), L(6, "GeneralAndAdministrativeExpense", 80.0),
             L(7, "OperatingExpenses", 300.0)]
    assert P.classify_sga(lines) == ("excludes_rd", "face_identity_total_equals_sga_plus_rd")


def _fsds(root: Path) -> Path:
    fsds = root / "fsds"
    for t in ("sub", "pre", "num"):
        (fsds / t).mkdir(parents=True)
    pq.write_table(pa.Table.from_pylist([{"quarter": "2023q4", "adsh": "a-1"}]), fsds / "sub" / "2023q4.parquet")
    pre = [("a-1", 2, 3, "IS", False, "GrossProfit"), ("a-1", 2, 4, "IS", False, "ResearchAndDevelopmentExpense"),
           ("a-1", 2, 5, "IS", False, "SellingGeneralAndAdministrativeExpense"),
           ("a-1", 2, 6, "IS", False, "OperatingExpenses"),
           ("a-1", 3, 1, "IS", True, "SellingGeneralAndAdministrativeExpense"),     # parenthetical: ignored
           ("a-1", 4, 1, "BS", False, "Assets")]
    pq.write_table(pa.Table.from_pylist([{"quarter": "2023q4", "adsh": a, "report": r, "line": ln, "stmt": s,
                                          "inpth": i, "tag": t, "version": "us-gaap/2023", "plabel": t}
                                         for a, r, ln, s, i, t in pre]), fsds / "pre" / "2023q4.parquet")
    num = [("GrossProfit", D(2022, 12, 31), None, 600), ("ResearchAndDevelopmentExpense", D(2022, 12, 31), None, 100),
           ("SellingGeneralAndAdministrativeExpense", D(2022, 12, 31), None, 200),
           ("OperatingExpenses", D(2022, 12, 31), None, 300),
           ("SellingGeneralAndAdministrativeExpense", D(2021, 12, 31), None, 180),    # prior year: ignored
           ("SellingGeneralAndAdministrativeExpense", D(2022, 12, 31), "Segment=X", 999)]  # dimensional: ignored
    pq.write_table(pa.Table.from_pylist([{"quarter": "2023q4", "adsh": "a-1", "tag": t, "version": "us-gaap/2023",
                                          "ddate": d, "qtrs": 4, "uom": "USD", "segments": s, "coreg": None,
                                          "value": float(v)} for t, d, s, v in num]), fsds / "num" / "2023q4.parquet")
    return fsds


def test_sga_sample_reads_the_fsds_face_and_keeps_undecidable_issuers(root):
    _built(root)
    fsds = _fsds(root)
    con = C.connect(memory="200MB", threads=1)
    try:
        smp = P.sga_convention_sample(con, root / P.STAGE, fsds, n=50)
        again = P.sga_convention_sample(con, root / P.STAGE, fsds, n=50)
    finally:
        con.close()
    # issuers with a filing dated 2020 onward come first (cik 1, cik 2); cik 3 (2010 only) fills the sample
    assert smp["pool_issuers"] == 3 and smp["pool_target_issuers"] == 2 and smp["sampled"] == 3
    assert smp["sampled_target_filings"] == 2
    assert {i["cik"] for i in smp["issuers"][:2]} == {1, 2} and smp["issuers"][2]["cik"] == 3
    assert smp["counts"] == {"excludes_rd": 1, "unresolved:no_fsds_income_statement": 2}
    by = {i["accession"]: i for i in smp["issuers"]}
    assert by["a-1"]["face_sga"] == 200.0 and by["a-1"]["xsga_equals_face_sga"] is False and by["a-1"]["face_lines"] == 4
    assert by["a-1"]["in_fsds"] and not by["a-2"]["in_fsds"]
    assert by["a-4"]["face_lines"] == 0 and by["a-4"]["class"] == "unresolved"
    assert [i["cik"] for i in again["issuers"]] == [i["cik"] for i in smp["issuers"]]


def test_accept_prints_every_item_and_passes_on_a_fresh_build(root):
    _built(root)
    ok, lines = P.accept(fsds=_fsds(root))
    text = "\n".join(lines)
    assert ok
    for item in ("(i) C5 manifest self-check", "(iv) part year check", "(ii) xsga coverage",
                 "(iii) SG&A convention", "history: issuers with a first-reported row"):
        assert item in text
    assert "(i)+(iv) PASS" in text and "rows_outside_year=0" in text
    # 2020 onward through the latest data is the primary window; the original tables stay as supporting evidence
    assert "(ii) xsga coverage, primary window 2020-2024" in text
    assert "history: issuers with a first-reported row per fiscal year, primary window 2020-2024" in text
    assert "(ii) xsga coverage, original window 2009-2022 (supporting)" in text
    assert "data end (max available_at) 2024-01-01 00:00:00" in text
    assert "  2024 partial" in text and "  2023 partial" in text and "  2022 completeness_unverified" in text
    assert "filing_window" not in text


def test_sample_prefers_a_2020_onward_filing_in_fsds(root):
    rows = [_row(7, D(2019, 12, 31), "b-old", T(2020, 2, 1)),                     # 2020+ and in FSDS
            _row(7, D(2020, 12, 31), "b-mid", T(2021, 2, 1)),                     # 2020+ and in FSDS: latest such
            _row(7, D(2021, 12, 31), "b-new", T(2022, 2, 1)),                     # 2020+ but not in FSDS
            _row(8, D(2018, 12, 31), "b-pre", T(2019, 2, 1))]                     # no target-period filing
    _upstream(root, rows)
    P.build(upstream_manifest_sha256=C.sha256_file(root / "compustat_std" / "manifest.json"))
    fsds = root / "fsds"
    for t in ("sub", "pre", "num"):
        (fsds / t).mkdir(parents=True)
    pq.write_table(pa.Table.from_pylist([{"quarter": "2021q1", "adsh": a} for a in ("b-old", "b-mid", "b-pre")]),
                   fsds / "sub" / "2021q1.parquet")
    con = C.connect(memory="200MB", threads=1)
    try:
        smp = P.sga_convention_sample(con, root / P.STAGE, fsds, n=1)
    finally:
        con.close()
    assert [(i["cik"], i["accession"], i["in_fsds"]) for i in smp["issuers"]] == [(7, "b-mid", True)]
    assert smp["pool_issuers"] == 2 and smp["pool_target_issuers"] == 1


def test_fiscal_year_status_uses_only_the_data_end_and_open_fiscal_periods():
    # no filing-deadline model (ruling B3-2): partial while a fiscal period of the year (FYE Jun Y .. early Jun Y+1) is
    # still open at the source max available_at; every other year is reported as observed, completeness unverified
    end = T(2026, 9, 19, 22)
    assert P.fiscal_year_status(2026, end) == "partial"
    assert P.fiscal_year_status(2025, end) == "completeness_unverified"
    assert P.fiscal_year_status(2020, end) == "completeness_unverified"
    assert P.fiscal_year_status(2025, T(2026, 6, 7, 23, 59)) == "partial"           # 52/53-week FYE into June
    assert P.fiscal_year_status(2025, T(2026, 6, 8)) == "completeness_unverified"


def test_operating_income_identity_decides_when_no_expense_total_follows():
    # gross profit minus the separate SG&A and R&D lines equals operating income: both are additive
    lines = [L(1, "Revenues", 1000.0), L(2, "CostOfRevenue", 400.0), L(3, "GrossProfit", 600.0),
             L(4, "ResearchAndDevelopmentExpense", 100.0), L(5, "SellingGeneralAndAdministrativeExpense", 200.0),
             L(6, "OperatingIncomeLoss", 300.0)]
    assert P.classify_sga(lines) == ("excludes_rd", "face_identity_anchor_minus_sga_and_rd_equals_operating_income")
    # R&D printed below SG&A as an "of which" line: operating income only reconciles without it
    inside = [L(3, "GrossProfit", 600.0), L(4, "SellingGeneralAndAdministrativeExpense", 300.0),
              L(5, "ResearchAndDevelopmentExpense", 100.0), L(6, "OperatingIncomeLoss", 300.0)]
    assert P.classify_sga(inside) == ("includes_rd", "face_identity_operating_income_counts_rd_inside_sga")
    # revenue as the anchor, cost lines in the block
    rev = [L(1, "Revenues", 1000.0), L(2, "CostOfRevenue", 400.0), L(3, "ResearchAndDevelopmentExpense", 100.0),
           L(4, "SellingGeneralAndAdministrativeExpense", 200.0), L(5, "OperatingIncomeLoss", 300.0)]
    assert P.classify_sga(rev) == ("excludes_rd", "face_identity_anchor_minus_sga_and_rd_equals_operating_income")
    neither = [L(3, "GrossProfit", 600.0), L(4, "ResearchAndDevelopmentExpense", 100.0),
               L(5, "SellingGeneralAndAdministrativeExpense", 200.0), L(6, "OperatingIncomeLoss", 123.0)]
    assert P.classify_sga(neither) == ("unresolved", "face_identity_not_found")


def test_sga_sample_uses_the_filings_own_currency(root):
    _built(root)
    fsds = _fsds(root)
    num = pq.read_table(fsds / "num" / "2023q4.parquet")
    num = num.set_column(num.schema.get_field_index("uom"), "uom", pa.array(["EUR"] * num.num_rows))
    pq.write_table(num, fsds / "num" / "2023q4.parquet")
    con = C.connect(memory="200MB", threads=1)
    try:
        smp = P.sga_convention_sample(con, root / P.STAGE, fsds, n=50)
    finally:
        con.close()
    assert {i["accession"]: i["class"] for i in smp["issuers"]}["a-1"] == "excludes_rd"
