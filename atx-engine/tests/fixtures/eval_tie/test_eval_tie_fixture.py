"""The Python side of the statistics tie fixture (P9 lane T1; contract K-P9-5, gate G-P7; P9 ruling P11).

Recomputes every committed value with today's Python statistics of record (backtest_integrity, nav_summ, dsr_total)
from the committed inputs, and checks the committed draws are the ones those statistics consume. The engine side is
lane B2's EvalVerb gtest (wave 2), which reads the same files; nothing here is a C++ test.

  "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
      atx-engine/tests/fixtures/eval_tie/test_eval_tie_fixture.py
"""
from __future__ import annotations

import json
import math
from pathlib import Path
import sys

import numpy as np
import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import generate_eval_tie as G  # noqa: E402

EXPECTED = json.loads((HERE / G.EXPECTED).read_bytes())
COMMITTED = (G.SERIES, G.LEDGER, G.STARTS, G.EXPECTED)


def test_the_committed_values_are_todays_python():
    assert G.check() == []


def test_regenerating_writes_the_committed_bytes(tmp_path):
    if EXPECTED["numpy"] != np.__version__:
        pytest.skip(f"the series stream is numpy {EXPECTED['numpy']}'s; this is {np.__version__} (values still "
                    "checked by test_the_committed_values_are_todays_python)")
    G.write(tmp_path)
    for name in COMMITTED:
        assert (tmp_path / name).read_bytes() == (HERE / name).read_bytes(), name


def test_cbb_starts_are_the_resamples_paired_stats_draws():
    starts = G.read_starts()
    assert starts.shape == (G.DRAWS, G.CBB_BLOCKS) and int(starts.min()) >= 0 and int(starts.max()) < G.T
    drawn = G.NS.cbb_indices(G.T, G.DRAWS, G.BLOCK, np.random.default_rng(G.PAIRED_SEED))
    assert np.array_equal(G.cbb_from_starts(starts), drawn)


def test_onc_replay_consumes_every_recorded_pick_and_finds_the_committed_clusters():
    picks = EXPECTED["inputs"]["onc_picks"]["picks"]
    rng = G.ReplayRng(picks)
    replay = G.onc_run(G.read_series(), rng)
    assert rng.used == len(picks)
    assert replay["clusters"] == EXPECTED["values"]["onc"]["clusters"] == EXPECTED["values"]["onc_replay"]["clusters"]
    assert replay["silhouettes"] == EXPECTED["values"]["onc_replay"]["silhouettes"]


def test_replay_refuses_a_divergent_or_extra_call():
    rng = G.ReplayRng([["integers", 12, 3]])
    with pytest.raises(AssertionError):
        rng.choice(12, p=None)
    rng = G.ReplayRng([["integers", 12, 3]])
    assert rng.integers(12) == 3
    with pytest.raises(AssertionError):
        rng.integers(12)


def test_the_fixture_exercises_the_edges():
    """T leaves a CSCV tail and a truncated CBB block; ONC finds the 3 groups (book and reference together); the
    IS / OOS rank margins sit far above rounding, so the integer PBO results can tie at 0; both PSR benchmarks and
    the house DSR are defined."""
    v = EXPECTED["values"]
    assert G.T % G.PBO_BLOCKS and G.T % G.BLOCK and v["pbo"]["dropped_tail_sessions"] == G.T % G.PBO_BLOCKS
    assert v["pbo"]["exhaustive"] and v["pbo"]["splits"] == math.comb(G.PBO_BLOCKS, G.PBO_BLOCKS // 2)
    assert min(v["pbo_margins"].values()) > 1e-9
    assert v["onc"]["n_eff"] == G.GROUPS
    assert any({G.NAMES[G.BOOK], G.NAMES[G.REFERENCE]} <= set(c) for c in v["onc"]["clusters"])
    assert all(b["psr"] is not None and b["min_trl_sessions"] is not None for b in v["psr"]["benchmarks"])
    house = v["dsr_house_v1"]
    assert house["v8"]["n"] == len(G.read_records()) == G.K and house["v8"]["dsr"] is not None
    assert v["paired"]["draws"] == G.DRAWS and v["paired"]["block"] == G.BLOCK and v["paired"]["seed"] == G.PAIRED_SEED
    series = G.read_series()
    assert series.shape == (G.T, G.K) and bool(np.all(np.isfinite(series)))
    assert set(EXPECTED["tie_rules"]) == set(v) - {"pbo_margins"}


def test_compare_is_exact_on_the_recorded_numpy_and_relative_otherwise():
    assert G.compare({"a": 1.0}, {"a": 1.0}, exact=True) == []
    assert G.compare({"a": 1.0}, {"a": 1.0 + 2e-16}, exact=True)
    assert G.compare({"a": 1.0}, {"a": 1.0 + 2e-16}, exact=False) == []
    assert G.compare({"a": 1.0}, {"a": 1.0 + 1e-9}, exact=False)
    assert G.compare({"a": [1, 2]}, {"a": [1]}, exact=True)
    assert G.compare({"a": 1}, {"b": 1}, exact=True)
    assert G.compare({"a": 1}, {"a": True}, exact=True)
