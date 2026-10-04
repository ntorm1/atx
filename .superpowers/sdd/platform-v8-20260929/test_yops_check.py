"""Tests of lane YOPS's checker (yops_check.py): synthetic data only, no data payload is read.

Run from the repository root:
    "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider \
        .superpowers/sdd/platform-v8-20260929/test_yops_check.py
"""
from __future__ import annotations

import hashlib

import numpy as np
import pytest

import yops_check as y

N = np.nan
FROZEN_SHA256 = {  # task-YOPS-report.md section 4; atx-engine/tests/alpha/alpha_formulaic_ops_test.cpp pins the same
    1: "cd6d1589a3756d3326ef8fffee724af2ae9c43b00dda1f3767702054edb748ee",
    3: "c8016b408c87f2c9add9b834968ef7f13e77ba040e5989402a6f89ae334f96d6",
    4: "1a4f1e40c0ee80ea68ba2e9d63a598907cfea0ab367a90f0f7c2d2aae7d1a151",
    13: "036fdbcf75b39e15169897c8f40831c72f7f15ef1339d40edef247095a1bec8f",
    15: "74dc73d5e6f31136798ba6b1498343f06705dddda63cd073086c392f843001b8",
    16: "10e331f2a68f806c5fa0408f6793b17f61d34e9d115354ca5af7ad05c827d687",
    29: "a3b9ef0a636e1e748de2915ab2491850d4aea4c4057f1bb058ad2630d0da4b6c",
    31: "d844365bb80a187e3f0eeeee86f43a9a6fe0b53316e4fc462a224b2eea46375b",
    80: "307be4989b6f7e616e612b43f91971cbd3f7100d41f604054cb213fff3b402e3",
    88: "0e4bd47e30e5acbf882519b680cf1353c27e1ed744c5aa24eb396df04a59da55",
}

y.xs.FIELDS.update({"x", "w", "one", "grp_g", "grp_one", "grp_each"})


@pytest.fixture(scope="module")
def wd():
    return y.world()


def run_small(expr, env):
    """The interpreter on a hand-written panel (it sizes the output from env['close'])."""
    return y.run(expr, {**env, "close": next(iter(env.values()))})


# ------------------------------------------------------------------------------------------------ registry and strings
def test_registry_rows_match_registry_cpp():
    y.registry_rows()
    assert set(y.REGISTERED) == {"group_sum", "group_delay", "asof_rank_ts_rank", "asof_rank_ts_min",
                                 "asof_rank_decay_linear", "asof_rank_correlation", "asof_rank_covariance"}


def test_frozen_strings_are_byte_pinned_and_under_the_dsl_limit():
    assert set(y.TRANSCRIPTIONS) == set(FROZEN_SHA256)
    for n, tpl in y.TRANSCRIPTIONS.items():
        text = y.t(tpl)
        assert hashlib.sha256(text.encode()).hexdigest() == FROZEN_SHA256[n], f"#{n} drifted"
        assert len(text.encode()) <= 4096
        assert y.figures(text)["sha256"] == FROZEN_SHA256[n]


# ------------------------------------------------------------------------------------------------ the op oracles
def test_rank2d_average_ties_and_singleton():
    r = y.rank2d(np.array([[3.0, 1.0, 3.0, N, 2.0], [N, N, 7.0, N, N]]))
    assert r[0].tolist()[:3] == [5.0 / 6.0, 0.0, 5.0 / 6.0] and np.isnan(r[0, 3]) and r[0, 4] == 1.0 / 3.0
    assert r[1, 2] == 0.5 and np.isnan(r[1, [0, 1, 3, 4]]).all()


def test_group_sum_closed_form_sizes_one_and_all():
    env = {"x": np.array([[1, 2, N, 4, 8, 16], [N, N, 5, 1, 1, 1]], float),
           "grp_g": np.array([[1, 1, 1, 2, 2, N], [9, 9, 7, 7, 3, 3]], float),
           "grp_one": np.ones((2, 6)), "grp_each": np.tile(np.arange(6.0), (2, 1))}
    want = np.array([[3, 3, N, 12, 12, N], [N, N, 6, 6, 2, 2]], float)
    assert y.same(run_small("group_sum(x, grp_g)", env), want)
    assert y.same(run_small("group_sum(x, grp_each)", env), env["x"])
    assert y.same(run_small("group_sum(x, grp_one)", env),
                  np.array([[31, 31, N, 31, 31, 31], [N, N, 8, 8, 8, 8]], float))


def test_asof_rebase_reranks_past_sessions():
    env = {"x": np.array([[10, 8, 12, 9]] * 3 + [[10, N, 12, 9]], float),
           "w": np.array([[1, 1, 1, N], [1, 1, 1, N], [1, 2, 1, N], [1, N, 1, N]], float)}
    got = run_small("asof_rank_ts_min(x, w, 3, 0)", env)
    assert y.same(got[2:], np.array([[0, 1, 0.5, N], [0, N, 0.5, N]], float)) and np.isnan(got[:2]).all()
    pit = run_small("ts_min(rank((x * w)), 3)", env)
    assert y.same(pit[2], np.array([0, 0, 0.5, N]))  # the point-in-time reading ranks B lowest
    lag = run_small("asof_rank_ts_min(x, w, 1, 1)", env)
    assert y.same(lag[1], np.array([0.5, 0, 1, N])) and y.same(lag[3], np.array([0, 1, 0.5, N]))


def test_asof_unit_factor_equals_house_composition(wd):
    env = y.house_env(wd)
    env["one"] = np.where(np.isnan(env["close"]), N, 1.0)
    for asof, house in [("asof_rank_ts_rank(close, one, 7, 0)", "ts_rank(rank(close), 7)"),
                        ("asof_rank_ts_min(close, one, 4, 3)", "delay(ts_min(rank(close), 4), 3)"),
                        ("asof_rank_decay_linear(close, one, 5, 2)", "delay(decay_linear(rank(close), 5), 2)"),
                        ("asof_rank_correlation(close, one, rank(volume), 6, 1)",
                         "delay(correlation(rank(close), rank(volume), 6), 1)"),
                        ("asof_rank_covariance(close, one, rank(volume), 5, 0)",
                         "covariance(rank(close), rank(volume), 5)")]:
        a, h = y.run(asof, env), y.run(house, env)
        assert y.same(a, h, 1e-12) and np.isfinite(a).any(), asof


def test_a_point_in_time_factor_is_caught(wd, monkeypatch):
    """A planted op error: re-base each window on its FIRST session's factor instead of the day's."""
    text = y.t(y.TRANSCRIPTIONS[4])
    ref, _ = y.check_formula(4, wd, text)
    real = y.asof_factor
    monkeypatch.setattr(y, "asof_factor", lambda w, t, lo: real(w, lo, lo))
    assert not y.same(y.run(text, y.house_env(wd)), ref)


# ------------------------------------------------------------------------------------------------ the transcriptions
@pytest.mark.parametrize("n", sorted(y.TRANSCRIPTIONS))
def test_transcription_equals_printed_formula_and_planted_errors_fail(wd, n):
    ref, cells = y.check_formula(n, wd, y.t(y.TRANSCRIPTIONS[n]))
    assert cells > 0
    assert y.probes(n, wd, ref) >= 3


def test_029_is_degenerate_as_printed(wd):
    assert y.degenerate_029(wd) > 0


def test_068_unrolled_matches_but_exceeds_the_byte_limit(wd):
    text = y.t(y.UNROLLED[68])
    _, cells = y.check_formula(68, wd, text)
    assert cells > 0 and len(text.encode()) > 4096
    assert 92 in y.BLOCKED and 68 in y.BLOCKED


def test_group_sum_value_weighted_peer_return(wd):
    lines = y.group_sum_checks(wd)
    assert "slots 7" in lines[1] and "slots 8" in lines[1]
