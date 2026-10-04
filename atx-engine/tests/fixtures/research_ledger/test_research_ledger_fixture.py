"""The Python side of the research ledger rules fixture (P9 lane B2; gtest ResearchLedger.TrialCountsEqualPython).

Pins expected.json to today's Python readers of record (backtest_integrity, dsr_total, era_pool) on the committed
ledger.jsonl, so the engine's research/ledger library is tied to the rules the ledger's Python writer and readers apply.

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
      atx-engine/tests/fixtures/research_ledger/test_research_ledger_fixture.py
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import generate_research_ledger as G  # noqa: E402

EXPECTED = json.loads((HERE / G.EXPECTED).read_bytes())


def test_the_committed_values_are_todays_python():
    assert G.check() == []


def test_the_ledger_bytes_are_pinned():
    assert hashlib.sha256((HERE / G.LEDGER).read_bytes()).hexdigest() == EXPECTED["ledger"]["sha256"]


def test_regenerating_writes_the_committed_bytes(tmp_path):
    G.write(tmp_path)
    for name in (G.LEDGER, G.EXPECTED):
        assert (tmp_path / name).read_bytes() == (HERE / name).read_bytes(), name


def test_the_fixture_exercises_every_rule():
    v = EXPECTED["values"]
    counts = v["trial_counts"]
    assert len(counts) == v["lines"] == 20 and 0 in counts and max(counts) > 1
    assert v["invalid_ids"] and v["excluded_lines"] and len(v["history_read_lines"]) == 2
    assert v["campaign_registry_count"] > 0 and v["trial_breakdown"]["n_tot"] > v["trial_breakdown"]["ledger_trials"]
    assert v["ledger_n"]["construction_out"] == v["ledger_n"]["construction_in"] + 1
    assert v["dsr_variance"][G.WINDOW]["variance_sr"] is not None
    assert v["dsr_variance"][G.OLD_WINDOW]["legacy_variance_sr"] is not None
    assert v["prefix_length"]["ffffffffffffffff"] is None
    assert v["prefix_heads"][-1] == v["ledger_head"] and len(set(v["prefix_heads"])) == v["lines"]
    text = (HERE / G.LEDGER).read_text(encoding="utf-8")
    assert "NaN" in text and "\n   \n" in text
