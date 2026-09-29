"""Parity catalog completeness (plan 1.1 and design spec) and scorecard rendering from fixture measurements."""

from __future__ import annotations

import json
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

from atx_db.parity import catalog as C
from atx_db.parity.scorecard import Lake, measure_row, render


def test_committed_catalog_is_current_and_covers_plan_and_spec() -> None:
    rows = C.load()
    assert C.to_csv(C.build_rows()) == C.CSV_PATH.read_text(encoding="utf-8"), "run python -m atx_db.parity.catalog"
    assert tuple(rows[0]) == C.COLUMNS
    domains = C.plan_domains()
    assert len(domains) == 20
    got = {(r["origin"], r["item"]) for r in rows}
    for d in domains:
        assert (f"plan 1.1 #{d['n']}", "(domain)") in got
    items = {r["item"] for r in rows}
    missing = [it["item"] for it in C.spec_items() if it["item"] not in items]
    assert not missing, missing
    assert len({(r["domain"], r["item"]) for r in rows}) == len(rows)  # no duplicate (domain, item)


def test_spec_and_seed_fields_are_joined() -> None:
    rows = {(r["domain"][:2], r["item"]): r for r in C.load()}
    rev = rows[("06", "revenue")]
    assert rev["compustat_field"].split("|")[:2] == ["sale", "revt"] and rev["factset_field"] == "FF_SALES"
    assert rev["stage"] == "fundamentals" and rev["column"] == "sale_q|sale_ttm" and "0.96" in rev["target"]
    assert rows[("06", "gross_profit")]["factset_field"] == "FF_GROSS_INC"  # seed name gross_profit__1004
    assert rows[("02", "ret")]["crsp_field"] == "RET" and rows[("02", "ret")]["column"] == "ret"
    assert rows[("16", "ebitda")]["sprint"] == "S8"
    assert rows[("19", "piotroski_f")]["column"] == "fscore"


def _fixture_lake(tmp_path: Path) -> Path:
    root = tmp_path / "lake"
    (root / "fundamentals").mkdir(parents=True)
    (root / "fundamentals" / "manifest.json").write_text(json.dumps({"coverage_by_year": {"years": {"2025": {
        "items": {"sale_q": {"finite_ex_structural": 0.8471}, "at": {"finite_ex_structural": 0.9777}}}}}}))
    (root / "ftd").mkdir()
    (root / "ftd" / "manifest.json").write_text(json.dumps({"receipt": {"coverage_by_year": {"2025": {
        "share_with_mapped_ftd_row_last_30d": 0.9869}}}}))
    (root / "metrics").mkdir()
    pq.write_table(pa.table({"window": ["2025", "train_2020_2022", "2025"], "field": ["at", "at", "ret"],
                             "member_equity": [0.9, 0.88, 0.999], "linked_usd": [0.95, 0.93, None],
                             "excl_structural": [None, None, None]}), root / "metrics" / "coverage.parquet")
    return root


def _row(**kw: str) -> dict[str, str]:
    base = dict.fromkeys(C.COLUMNS, "")
    base.update(kw)
    return base


def test_measures_are_read_verbatim_with_basis(tmp_path: Path) -> None:
    lake = Lake(_fixture_lake(tmp_path))
    at = measure_row(_row(domain="06 F", item="total_assets", stage="fundamentals", column="at"), lake, "2025")
    assert [m.text for m in at] == ["at 0.95 (2025, linked_usd)", "at 0.93 (train_2020_2022, linked_usd)",
                                    "at 0.9777 (events 2025, ex-structural)"]
    ret = measure_row(_row(domain="02 P", item="ret", stage="prices", column="ret"), lake, "2025")
    assert [m.text for m in ret] == ["ret 0.999 (2025, member_equity)"]
    dom = measure_row(_row(domain="12 Short", item="(domain)", stage="ftd"), lake, "2025")
    assert dom[0].value == 0.9869 and "ftd/manifest.json" in dom[0].source
    assert measure_row(_row(domain="06 F", item="xrd", stage="fundamentals", column="xrd_ttm"), lake, "2025") == []


def test_render_marks_unmeasured_rows_and_lists_sources(tmp_path: Path) -> None:
    lake = Lake(_fixture_lake(tmp_path))
    rows = [_row(domain="06 Fundamentals", item="(domain)", stage="fundamentals", target="t", sprint="S4"),
            _row(domain="06 Fundamentals", item="revenue", compustat_field="sale|revt", stage="fundamentals",
                 column="sale_q|sale_ttm", target="t", sprint="S4"),
            _row(domain="16 Estimates", item="eps", target="t", sprint="S8")]
    text = render(rows, lake, "2025", "2026-09-29")
    assert "`metrics/coverage.parquet`" in text and "`fundamentals/manifest.json`" in text
    assert "sale\\|revt" in text and "sale_q 0.8471 (events 2025, ex-structural)" in text
    assert "| eps | - | - | - | - | t | not measured | S8 |" in text
    assert "1 of 2 event columns >= 0.90" in text
