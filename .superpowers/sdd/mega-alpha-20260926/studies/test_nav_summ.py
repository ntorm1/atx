"""Synthetic postimplementation fixtures for nav_summ.py (T31: netting ratio, construction stats, paired dSR).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider .superpowers/sdd/mega-alpha-20260926/studies/test_nav_summ.py

No real data: every NAV dir is written here (summary.json + a daily CSV carrying only the columns nav_summ reads,
named as strategy_nav_replay.cpp writes them).
"""
from __future__ import annotations

import csv
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import pytest

import nav_summ as NS  # noqa: E402

SCEN = "modeled-1bn-stale5-v1+swap-fin-v1"
COLUMNS = ("session_index", "session_ns", "executed", "return_observation", "net_return", "gross_return",
           "trade_cost_return", "traded_dollars", "trade_cost_dollars", "pretrade_gross_dollars", "gross_leverage",
           "net_leverage", "held_names", "one_way_turnover_gmv", "neutralize")
DAY = 86_400_000_000_000
T0 = 1_577_923_200_000_000_000


def eps_alt(t):
    return np.array([1.0 if k % 2 == 0 else -1.0 for k in range(t)])


def eps_pair(t):
    return np.array([1.0 if k % 4 in (0, 1) else -1.0 for k in range(t)])


def write_nav(d: Path, nets, *, weights_sha="0" * 64, tau=None, gross=None, net=None, pretrade=None,
              cost_return=None, traded=None, cost_dollars=None, v5=None, summary_tau=None, net_sharpe=0.5) -> Path:
    """Row 0: nothing; row 1: deployment (first fill, not a return row); rows 2..: return rows carrying ``nets``."""
    t = len(nets) + 2
    d.mkdir(parents=True)
    tau = np.full(t, 0.05) if tau is None else np.asarray(tau, dtype=float)
    gross = np.ones(t) if gross is None else np.asarray(gross, dtype=float)
    net = np.zeros(t) if net is None else np.asarray(net, dtype=float)
    pretrade = np.r_[0.0, 0.0, np.full(t - 2, 1e9)] if pretrade is None else np.asarray(pretrade, dtype=float)
    cost_return = np.zeros(t) if cost_return is None else np.asarray(cost_return, dtype=float)
    traded = np.r_[0.0, 5e8, np.full(t - 2, 1e7)] if traded is None else np.asarray(traded, dtype=float)
    cost_dollars = np.r_[0.0, 1e5, np.full(t - 2, 2e3)] if cost_dollars is None else np.asarray(cost_dollars, dtype=float)
    with (d / f"daily_{SCEN}.csv").open("w", newline="", encoding="utf-8") as stream:
        w = csv.writer(stream)
        w.writerow(COLUMNS)
        for k in range(t):
            w.writerow([399 + k, T0 + k * DAY, int(k >= 1), int(k >= 2), nets[k - 2] if k >= 2 else 0, 0,
                        cost_return[k], traded[k], cost_dollars[k], pretrade[k], gross[k], net[k], 100 + k,
                        "nan" if not math.isfinite(tau[k]) else tau[k], "applied"])
    scen = {"scenario": SCEN, "primary": True, "net_sharpe": net_sharpe, "gross_sharpe": None, "hac_t": 1.0,
            "ann_mean": 0.01, "ann_vol": 0.02, "max_drawdown": 0.01, "costs": {}, "calendar_year_returns": [],
            "construction": {"v5": v5} if v5 is not None else {"band_multiple": 0.0}}
    if summary_tau is not None:
        scen["daily_turnover_gmv"] = {"mean": summary_tau, "p95": 0.1}
    summary = {"rule": "aim-partial-v5", "status": "complete", "primary_scenario": SCEN, "scenarios": [scen],
               "composition_weights_sha256": weights_sha}
    (d / "summary.json").write_text(json.dumps(summary), encoding="utf-8")
    return d


def write_weights(path: Path, wst: float) -> str:
    data = json.dumps({"schema": "atx.dsl-composition-weights/v1", "weights": {},
                       "provenance": {"rule": "ew-theme-aim-v1", "weighted_standalone_turnover": wst}}).encode()
    path.write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def test_paired_closed_form_uncorrelated_and_identical():
    t = 1000
    a = 0.001 + 0.01 * eps_alt(t)
    b = 0.0005 + 0.02 * eps_pair(t)          # orthogonal to the alternating pattern: rho = 0 exactly
    p = NS.paired_stats(a, b, draws=300)
    scale = math.sqrt(t / (t - 1))           # sample SD (ddof 1) of m + s * (+-1) is s * sqrt(T / (T-1))
    sr_a = 0.001 / (0.01 * scale) * math.sqrt(252)
    sr_b = 0.0005 / (0.02 * scale) * math.sqrt(252)
    assert p["sr"] == pytest.approx(sr_a, rel=1e-12) and p["sr_reference"] == pytest.approx(sr_b, rel=1e-12)
    assert p["dsr"] == pytest.approx(sr_a - sr_b, rel=1e-12)
    assert abs(p["rho"]) < 1e-12
    pa, pb = sr_a / math.sqrt(252), sr_b / math.sqrt(252)
    memmel = math.sqrt((2 + 0.5 * (pa * pa + pb * pb)) / t * 252)
    assert p["memmel_se"] == pytest.approx(memmel, rel=1e-9)
    assert p["t"] == pytest.approx((sr_a - sr_b) / memmel, rel=1e-9)
    assert p["sessions"] == t and p["draws"] == 300 and p["block"] == 21
    # identical series: every resample difference is exactly 0; the studentized SE is 0 -> no LW interval
    q = NS.paired_stats(a, a.copy(), draws=200)
    assert q["dsr"] == 0.0 and q["rho"] == pytest.approx(1.0, abs=1e-12) and q["memmel_se"] < 1e-6
    assert q["cbb_ci95"] == [0.0, 0.0]
    assert q["lw"]["ci95"] == [None, None]
    # Memmel with rho = 1 and different SRs: only the SR-dispersion term remains
    assert NS.memmel_se(1.0, 0.5, 1.0, 100) == pytest.approx(
        math.sqrt(0.5 * (1 / 252 + 0.25 / 252 - 2 * 0.5 / 252) / 100 * 252), rel=1e-12)


def test_circular_block_indices():
    rng = np.random.default_rng(1)
    idx = NS.cbb_indices(50, 7, 21, rng)
    assert idx.shape == (7, 50) and idx.min() >= 0 and idx.max() < 50
    # within a block consecutive (mod T), each block starts fresh at 0, 21, 42
    for row in idx:
        for start in (0, 21, 42):
            blk = row[start:start + 21]
            assert np.all(np.diff(blk) % 50 == 1)


def test_bootstrap_is_seeded_and_calibrated_against_memmel():
    rng = np.random.default_rng(7)
    t = 1500
    e1, e2 = rng.normal(size=t), rng.normal(size=t)
    a = 0.0008 + 0.01 * e1
    b = 0.0003 + 0.01 * (0.95 * e1 + math.sqrt(1 - 0.95 ** 2) * e2)
    p = NS.paired_stats(a, b)
    assert p["draws"] == 2000 and p["cbb_valid_draws"] == 2000 and p["lw"]["valid_draws"] == 2000
    assert p["rho"] == pytest.approx(0.95, abs=0.02)
    lo, hi = p["cbb_ci95"]
    assert lo < p["dsr"] < hi
    ratio = (hi - lo) / (2 * 1.96 * p["memmel_se"])
    assert 0.75 < ratio < 1.35, ratio            # iid normal: the block bootstrap spread ~ the Memmel SE
    llo, lhi = p["lw"]["ci95"]
    assert llo < p["lw"]["dsr_population"] < lhi
    assert 0.8 < p["lw"]["se"] / p["memmel_se"] < 1.25
    assert 0.75 < (lhi - llo) / (2 * 1.96 * p["memmel_se"]) < 1.5
    assert abs(p["lw"]["dsr_population"] - p["dsr"]) < 5e-3  # population vs sample SD: O(1/T)
    assert 0 < p["lw"]["p_value"] <= 1
    again = NS.paired_stats(a, b)
    assert again["cbb_ci95"] == p["cbb_ci95"] and again["lw"] == p["lw"]
    other = NS.paired_stats(a, b, seed=1)
    assert other["cbb_ci95"] != p["cbb_ci95"]


def test_lw_p_value_small_for_a_clear_difference():
    rng = np.random.default_rng(3)
    t = 756
    e = rng.normal(size=t)
    a = 0.002 + 0.01 * e                              # SR ~ 3.2
    b = 0.0 + 0.01 * (0.98 * e + math.sqrt(1 - 0.98 ** 2) * rng.normal(size=t))
    p = NS.paired_stats(a, b)
    assert p["dsr"] > 2.5 and p["lw"]["p_value"] < 0.01 and p["cbb_ci95"][0] > 0 and p["lw"]["ci95"][0] > 0


def test_construction_stats_and_netting_ratio_by_hand(tmp_path):
    nets = [0.001, -0.002, 0.003, 0.0, 0.001, 0.002]
    tau = [np.nan, 0.9, 0.1, 0.2, np.nan, 0.4, 0.05, 0.3]           # row 1 deployment, row 4 zero pre-trade gross
    pretrade = [0, 0, 1e9, 1e9, 0, 1e9, 1e9, 1e9]
    gross = [0.0, 0.5, 0.9, 1.0, 1.1, 0.95, 1.05, 1.0]
    net = [0.0, 0.01, -0.02, 0.03, 0.0, -0.01, 0.02, 0.0]
    cost = [0, 0, 1e-4, 2e-4, 0, 1e-4, 5e-5, 5e-5]
    d = write_nav(tmp_path / "nav", nets, tau=tau, pretrade=pretrade, gross=gross, net=net, cost_return=cost,
                  v5={"mean_held_share": 0.95}, summary_tau=0.21)
    daily = NS.load_daily(d, SCEN)
    assert daily["neutralize"][0] == "applied" and np.isnan(daily["one_way_turnover_gmv"][0])
    scen = NS.scenario_of(NS.load_summary(d))
    s = NS.construction_stats(daily, scen)
    kept = np.array([0.1, 0.2, 0.4, 0.05, 0.3])
    assert s["tau_gmv_sessions"] == 5
    assert s["tau_gmv_mean"] == pytest.approx(0.21, abs=1e-15)
    assert s["tau_gmv_p95"] == pytest.approx(float(np.quantile(kept, 0.95)), abs=1e-15)
    assert s["mean_gross_leverage"] == pytest.approx(np.mean(gross[1:7]), abs=1e-15)  # previous rows of return rows
    assert s["mean_net_leverage"] == pytest.approx(np.mean(net[1:7]), abs=1e-15)
    assert s["mean_abs_net_leverage"] == pytest.approx(np.mean(np.abs(net[1:7])), abs=1e-15)
    assert s["held_share"] == 0.95 and s["return_rows"] == 6
    assert s["cost_per_gmv_turnover"] == pytest.approx(sum(cost[2:]) / kept.sum(), rel=1e-12)
    assert s["cost_bps_traded"] == pytest.approx(1e4 * (1e5 + 6 * 2e3) / (5e8 + 6 * 1e7), rel=1e-12)
    assert NS.netting_ratio(0.21, 0.07) == pytest.approx(3.0, rel=1e-12)
    assert NS.netting_ratio(0.21, 0.0) is None and NS.netting_ratio(None, 0.07) is None
    series = NS.net_series(daily)
    assert list(series.values()) == nets and min(series) == T0 + 2 * DAY


def test_main_end_to_end(tmp_path, capsys):
    t = 400
    rng = np.random.default_rng(11)
    e = rng.normal(size=t)
    ref_nets = 0.0003 + 0.01 * e
    cell_nets = 0.0006 + 0.01 * (0.9 * e + math.sqrt(1 - 0.81) * rng.normal(size=t))
    w_sha = write_weights(tmp_path / "w.json", 0.05)
    other = tmp_path / "other"
    other.mkdir()
    write_weights(other / "composition_weights.json", 0.08)
    ref = write_nav(tmp_path / "ref", list(ref_nets), weights_sha="f" * 64, summary_tau=0.05)
    cell = write_nav(tmp_path / "cell", list(cell_nets[5:]) , weights_sha=w_sha, summary_tau=0.05,
                     v5={"mean_held_share": 0.9}, net_sharpe=None)
    out = tmp_path / "res.json"
    assert NS.main([str(cell), str(ref), "--weights", str(tmp_path / "w.json"), "--weights", str(other),
                    "--reference", str(ref), "--draws", "300", "--json", str(out)]) == 0
    text = capsys.readouterr().out
    assert "net na gross na" in text                      # null statistics print "na" instead of crashing
    assert "netting_ratio = tau_book / weighted_standalone_turnover = 0.0500 / 0.0500 = 1.000" in text
    assert f"(weights {w_sha[:8]} matched)" in text
    assert "netting_ratio: na" in text                    # the reference NAV's weights SHA matches neither file
    assert text.count("paired vs") == 2 and "held_share 0.9000" in text
    res = json.loads(out.read_text())
    cell_res, ref_res = res
    assert cell_res["paired"]["sessions"] == t - 5       # aligned on common sessions only
    a, b = NS.align(NS.net_series(NS.load_daily(cell, SCEN)), NS.net_series(NS.load_daily(ref, SCEN)))
    assert cell_res["paired"]["dsr"] == pytest.approx(NS.sharpe(a) - NS.sharpe(b), rel=1e-12)
    assert ref_res["paired"]["dsr"] == 0.0 and ref_res["weights_match"] == "none"
    # a single unmatched weights file is still used, flagged
    NS.main([str(ref), "--weights", str(tmp_path / "w.json")])
    assert "UNMATCHED" in capsys.readouterr().out
