"""The Python side of the holdings identity fixture (P9 lane A4, migration slice 5).

Re-runs make_holdings_fixture.py: lane T2's re-dated holdings stage writers and the EXISTING Python builder
(``prepare_research_fields.run`` with ``research_fields_holdings``) reproduce every committed byte (stages, role,
shares_out input, expected payloads, normalized manifest). The C++ side is the gtest ResearchFieldsHoldings*
(atx-engine-research-fields-tests), which reads the same committed files.

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
      atx-engine/tests/fixtures/research_fields_holdings/test_holdings_fixture.py
"""
from __future__ import annotations

import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import make_holdings_fixture as fx  # noqa: E402

COMMITTED = sorted(p.relative_to(HERE).as_posix() for d in fx.KEPT for p in (HERE / d).rglob("*") if p.is_file())
# The holdings fields the engine ports (holdings_fields.hpp); sv_offexchange_share126 and exch_up_365d stay Python.
PORTED = ("ftd_shares_ratio21", "regsho_threshold_days63")


def test_the_fixture_is_the_python_builders_output(tmp_path):
    out = tmp_path / "holdings"
    fx.build(out)
    produced = sorted(p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file())
    assert produced == COMMITTED
    for rel in COMMITTED:
        assert (out / rel).read_bytes() == (HERE / rel).read_bytes(), rel


def test_the_fixture_exercises_the_ported_rules():
    m = json.loads((HERE / "expected" / "manifest.normalized.json").read_text(encoding="utf-8"))
    e = {f["name"]: f for f in m["fields"]}
    h = m["source_checks"]["holdings"]
    ftd = h["ftd"]
    assert ftd["rows_collision_excluded"] > 0 and ftd["rows_unmapped"] > 0 and ftd["role_rows_summed_duplicates"] > 0
    reasons = e["ftd_shares_ratio21"]["nan_reasons_member_cells"]
    assert reasons["no_visible_window"] > 0 and reasons["stale"] > 0 and reasons["shares_out_nan"] > 0
    assert e["ftd_shares_ratio21"]["coverage"]["finite_member_cells"] > 0
    reg = h["regsho_threshold"]
    assert reg["lists_outside_the_window_calendar"] > 0 and reg["prefix_sessions"] == 65
    reasons = e["regsho_threshold_days63"]["nan_reasons_member_cells"]
    assert reasons["market_unknown"] > 0 and reasons["market_lists_incomplete"] > 0
    assert reg["finite_member_cells_by_listing_market"]["nasdaq"] > 0
    assert all(e[x]["coverage"]["finite_member_cells"] > 0 for x in PORTED)
    # every row of the role is before the research seal; the stages' seal probes are the test module's
    days = Path(HERE / "role" / "sessions.i64").read_bytes()
    assert len(days) == 8 * e["ftd_shares_ratio21"]["shape"][0]
