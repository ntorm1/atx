"""The Python side of the vendor-panel identity fixture (P9 lane A3, migration slice 4).

Re-runs make_vendor_panel_fixture.py: the generator writes the committed vendor parquet byte for byte, and the EXISTING
Python builder (research_fields_price.py, research_fields_ohlc.py) re-run on the committed parquet reproduces every
committed role and expected byte. The C++ side is the gtest ResearchFieldsVendorFixture.* / ResearchFieldsVendorPanel.*
(atx-engine-research-fields-tests), which reads the same committed files.

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
      atx-engine/tests/fixtures/research_fields/test_vendor_panel_fixture.py
"""
from __future__ import annotations

import datetime as dt
import json
from pathlib import Path
import sys

import pyarrow.parquet as pq

HERE = Path(__file__).resolve().parent
VENDOR = HERE / "vendor"
sys.path.insert(0, str(HERE))
import make_vendor_panel_fixture as fx  # noqa: E402

COMMITTED = sorted(p.relative_to(VENDOR).as_posix() for p in VENDOR.rglob("*") if p.is_file())


def test_the_generator_writes_the_committed_parquet(tmp_path):
    fx.write_vendor(tmp_path / "th.parquet")
    assert (tmp_path / "th.parquet").read_bytes() == (VENDOR / "th.parquet").read_bytes()


def test_the_fixture_is_the_python_builders_output(tmp_path):
    out = tmp_path / "vendor"
    fx.build(out, vendor=VENDOR / "th.parquet")
    produced = sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file())
    assert produced == COMMITTED
    for rel in COMMITTED:
        assert (out / rel).read_bytes() == (VENDOR / rel).read_bytes(), rel


def test_the_fixture_exercises_every_rule():
    m = json.loads((VENDOR / "expected" / "manifest.normalized.json").read_text(encoding="utf-8"))
    price, bars = m["source_checks"]["price"]["source"], m["source_checks"]["ohlc"]["source"]
    assert price["factor_break"]["mass_sessions"] == ["2021-01-04"]
    assert price["factor_break"]["repaired_steps"] > 0 and price["factor_break"]["kept_gap_steps"] == 1
    assert price["sessions_before_role"] == 1600 and price["first_session"] == "2014-07-25"
    assert price["shares_rows_above_a9_ceiling"] == 1 and price["shares_lines_withheld_c81"] == 1
    for checks in (price, bars):
        assert checks["duplicate_keys_quarantined"] == 1 and checks["rows_off_calendar"] == 1
        assert checks["rows_on_or_after_seal_skipped"] == len(fx.SEALED) * len(fx.LONG) + 2 * 3   # sealed + straddle
    e = {f["name"]: f for f in m["fields"]}
    assert e["ret_overnight"]["guarded_member_cells"] > 0 and e["ret_intraday"]["guarded_member_cells"] > 0
    assert e["ceq_iss_5y"]["outside_domain_member_cells"] > 0
    assert e["open_adj"]["order_violation_member_cells"] > 0 and e["open_adj"]["missing_bar_member_cells"] > 0
    assert all(e[x]["coverage"]["finite_member_cells"] > 0 for x in fx.FIELDS)
    # The row groups the seal push-down gtest relies on: one wholly sealed, one straddling the seal.
    meta = pq.ParquetFile(VENDOR / "th.parquet").metadata
    seal = dt.date(2024, 1, 1)
    bounds = []
    for g in range(meta.num_row_groups):
        stats = meta.row_group(g).column(0).statistics
        bounds.append((stats.min, stats.max))
    assert sum(lo >= seal for lo, _ in bounds) == 1
    assert sum(lo < seal <= hi for lo, hi in bounds) == 1
