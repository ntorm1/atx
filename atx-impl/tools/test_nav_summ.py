"""Synthetic postimplementation fixtures for nav_summ.py (T31: netting ratio, construction stats, paired dSR).

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_nav_summ.py

No real data: every NAV dir is written here (summary.json + a daily CSV carrying only the columns nav_summ reads,
named as strategy_nav_replay.cpp writes them).
"""
from __future__ import annotations

import csv
import hashlib
import importlib.util
import json
import math
import re
import subprocess
from pathlib import Path

import numpy as np
import pytest

import nav_summ as NS  # noqa: E402

SCEN = "modeled-1bn-stale5-v1+swap-fin-v1"
COLUMNS = ("session_index", "session_ns", "executed", "return_observation", "net_return", "gross_return",
           "trade_cost_return", "traded_dollars", "trade_cost_dollars", "pretrade_gross_dollars", "gross_leverage",
           "net_leverage", "held_names", "one_way_turnover_gmv", "neutralize", "pretrade_nav")
PRE_T41_BLOB = "5c414e38e45979c8cad616522550dfedb4d46452"  # nav_summ.py at faf5943f (T31 fix 1, c8358931)
PRE_V6_BLOB = "73fbb7497a639831ffdf0b0c17f84a153cb4d836"   # nav_summ.py at 04e9d5bc (T41 fix wave, 1b68ed90)
V6_KEYS = {"mean_gross_leverage_post_ramp", "mean_net_leverage_post_ramp", "post_ramp_rows"}
NEW_KEYS = {"mean_gross_leverage_all_rows", "mean_net_leverage_all_rows", "csv_rows", "leverage_gate_basis",
            "nav_summ_run"} | V6_KEYS
V6_LINE = "   leverage post-ramp "
DAY = 86_400_000_000_000
T0 = 1_577_923_200_000_000_000


def eps_alt(t):
    return np.array([1.0 if k % 2 == 0 else -1.0 for k in range(t)])


def eps_pair(t):
    return np.array([1.0 if k % 4 in (0, 1) else -1.0 for k in range(t)])


def write_nav(d: Path, nets, *, weights_sha="0" * 64, tau=None, gross=None, net=None, pretrade=None,
              cost_return=None, traded=None, cost_dollars=None, v5=None, summary_tau=None, net_sharpe=0.5,
              executed=None, pretrade_nav=None) -> Path:
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
    executed = [int(k >= 1) for k in range(t)] if executed is None else list(executed)
    pretrade_nav = np.full(t, 1e9) if pretrade_nav is None else np.asarray(pretrade_nav, dtype=float)
    with (d / f"daily_{SCEN}.csv").open("w", newline="", encoding="utf-8") as stream:
        w = csv.writer(stream)
        w.writerow(COLUMNS)
        for k in range(t):
            w.writerow([399 + k, T0 + k * DAY, executed[k], int(k >= 2), nets[k - 2] if k >= 2 else 0, 0,
                        cost_return[k], traded[k], cost_dollars[k], pretrade[k], gross[k], net[k], 100 + k,
                        "nan" if not math.isfinite(tau[k]) else tau[k], "applied", pretrade_nav[k]])
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
    # T41 M1: the ruled D1/R6' gate statistic is the mean over every CSV row (rows 0 and 1 included)
    assert s["csv_rows"] == 8 and s["leverage_gate_basis"].startswith("all_rows")
    assert s["mean_gross_leverage_all_rows"] == pytest.approx(np.mean(gross), abs=1e-15)
    assert s["mean_net_leverage_all_rows"] == pytest.approx(np.mean(net), abs=1e-15)
    # T41 M2: cost over the tau sessions only (rows 2, 3, 5, 6, 7 at the default 2e3 $ / 1e9 NAV; the deployment
    # row 1's 1e5 $ is excluded like its tau); trade_cost_return is no longer read
    assert s["cost_per_gmv_turnover"] == pytest.approx(5 * 2e3 / 1e9 / kept.sum(), rel=1e-12)
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


# ------------------------------------------------ fix round 1: deflated Sharpe ratio (R6', plan-s4 4.E)
def longhand_dsr(sr, t, skew, kurt, variance, n):
    """The 4.E formulas written out with the standard library's NormalDist (independent of nav_summ's PhiInv)."""
    from statistics import NormalDist
    nd = NormalDist()
    gamma = 0.5772156649
    sr0 = math.sqrt(variance) * ((1 - gamma) * nd.inv_cdf(1 - 1 / n) + gamma * nd.inv_cdf(1 - 1 / (n * math.e)))
    z = (sr - sr0) * math.sqrt(t - 1) / math.sqrt(1 - skew * sr + (kurt - 1) * sr ** 2 / 4)
    return sr0, nd.cdf(z)


def test_normal_helpers_match_the_standard_library():
    from statistics import NormalDist
    nd = NormalDist()
    for p in (1e-6, 0.025, 0.5, 0.8, 0.9, 0.975, 1 - 1 / (10 * math.e), 1 - 1 / (20 * math.e)):
        assert NS.norm_ppf(p) == pytest.approx(nd.inv_cdf(p), abs=1e-12)
    for x in (-5.0, -1.3, 0.0, 0.7, 3.2):
        assert NS.norm_cdf(x) == pytest.approx(nd.cdf(x), abs=1e-15)
    with pytest.raises(ValueError):
        NS.norm_ppf(1.0)


def test_net_moments_hand_case():
    m = NS.net_moments(np.array([0.0, 0.0, 0.0, 1.0]))
    # mean .25, sample sd .5 -> SR .5; population m2 3/16, m3 3/32, m4 21/256 -> skew 2/sqrt(3), kurtosis 7/3
    assert m["sessions"] == 4 and m["sr_daily"] == pytest.approx(0.5, abs=1e-15)
    assert m["skew"] == pytest.approx(2 / math.sqrt(3), abs=1e-14)
    assert m["kurtosis"] == pytest.approx(7 / 3, abs=1e-14)
    assert NS.net_moments(np.full(5, 0.01))["sr_daily"] is None


def test_deflated_sharpe_hand_case_n10_t754():
    n, t, sr, skew, kurt, variance = 10, 754, 0.08, -0.3, 5.0, 0.0004   # per-session SR .08 (~1.27 annual)
    sr0_ref, dsr_ref = longhand_dsr(sr, t, skew, kurt, variance, n)
    sr0 = NS.expected_max_sr(variance, n)
    assert sr0 == pytest.approx(sr0_ref, abs=1e-12)
    assert NS.deflated_sharpe(sr, t, skew, kurt, sr0) == pytest.approx(dsr_ref, abs=1e-12)
    # pinned values (computed once with the longhand above): SR0 = 0.0314919660..., DSR = 0.9051249340...
    assert sr0 == pytest.approx(0.03149196602689943, abs=1e-6)
    assert NS.deflated_sharpe(sr, t, skew, kurt, sr0) == pytest.approx(0.9051249340733833, abs=1e-6)
    # a denominator <= 0 (extreme positive skew) has no DSR
    assert NS.deflated_sharpe(0.5, t, 10.0, 3.0, 0.0) is None


def test_deflated_sharpe_gaussian_case():
    # exact Gaussian moments: the denominator is sqrt(1 + SR^2 / 2)
    sr, t, sr0 = 0.06, 754, 0.03
    want = NS.norm_cdf((sr - sr0) * math.sqrt(t - 1) / math.sqrt(1 + sr * sr / 2))
    assert NS.deflated_sharpe(sr, t, 0.0, 3.0, sr0) == pytest.approx(want, abs=1e-15)
    # simulated Gaussian nets: sample skew ~ 0, kurtosis ~ 3, denominator ~ sqrt(1 + SR^2 / 2)
    x = 0.0005 + 0.01 * np.random.default_rng(12).normal(size=200_000)
    m = NS.net_moments(x)
    assert abs(m["skew"]) < 0.03 and abs(m["kurtosis"] - 3) < 0.06
    s = m["sr_daily"]
    denom = math.sqrt(1 - m["skew"] * s + (m["kurtosis"] - 1) * s * s / 4)
    assert denom == pytest.approx(math.sqrt(1 + s * s / 2), rel=1e-3)
    # plan-s4 4.E: expected max annual SR under the null over 3 years (Lo variance at SR 0, T = 756)
    for n, want_annual in ((5, 0.69), (10, 0.91), (20, 1.10)):
        assert NS.expected_max_sr(1 / 756, n) * math.sqrt(252) == pytest.approx(want_annual, abs=0.005)


def test_dsr_rows_cross_cell_and_single_cell_fallback():
    cells = [{"sessions": 754, "sr_daily": s, "skew": -0.2, "kurtosis": 4.0} for s in (0.02, 0.05, 0.08)]
    rows = NS.dsr_rows(cells, 10)
    v = float(np.var([0.02, 0.05, 0.08], ddof=1))
    for row, cell in zip(rows, cells):
        sr0_ref, dsr_ref = longhand_dsr(cell["sr_daily"], 754, -0.2, 4.0, v, 10)
        assert row["variance_sr"] == pytest.approx(v, abs=1e-18) and "cross-cell" in row["variance_source"]
        assert row["sr0_daily"] == pytest.approx(sr0_ref, abs=1e-12)
        assert row["dsr"] == pytest.approx(dsr_ref, abs=1e-12)
        assert row["sr0_annual"] == pytest.approx(sr0_ref * math.sqrt(252), abs=1e-12) and row["n"] == 10
    assert rows[0]["dsr"] < rows[1]["dsr"] < rows[2]["dsr"]
    single = NS.dsr_rows(cells[1:2], 10)[0]
    assert "Lo (2002)" in single["variance_source"]
    assert single["variance_sr"] == pytest.approx((1 + 0.05 ** 2 / 2) / 754, abs=1e-18)
    undefined = NS.dsr_rows([cells[0], {"sessions": 5, "sr_daily": None, "skew": None, "kurtosis": None}], 10)
    assert undefined[1]["dsr"] is None and "Lo (2002)" in undefined[0]["variance_source"]  # one defined SR only
    with pytest.raises(ValueError):
        NS.dsr_rows(cells, 1)


def test_main_reports_dsr_for_every_cell(tmp_path, capsys):
    rng = np.random.default_rng(21)
    dirs = []
    for k, mu in enumerate((0.0002, 0.0005, 0.0009)):
        nets = list(mu + 0.01 * rng.normal(size=300))
        dirs.append(str(write_nav(tmp_path / f"c{k}", nets, summary_tau=0.05)))
    out = tmp_path / "res.json"
    assert NS.main(dirs + ["--reference", dirs[0], "--draws", "100", "--json", str(out)]) == 0
    text = capsys.readouterr().out
    assert text.count("deflated SR (N=10): DSR") == 3 and "cross-cell sample variance of 3 per-session SRs" in text
    res = json.loads(out.read_text())
    moments = [NS.net_moments(np.array(list(NS.net_series(NS.load_daily(Path(d), SCEN)).values()))) for d in dirs]
    v = float(np.var([m["sr_daily"] for m in moments], ddof=1))
    for r, m in zip(res, moments):
        q = r["deflated"]
        assert set(q) >= {"dsr", "sr0_daily", "sr0_annual", "skew", "kurtosis", "n", "variance_sr", "sessions"}
        assert q["sessions"] == 300 and q["variance_sr"] == pytest.approx(v, rel=1e-12)
        assert q["dsr"] == pytest.approx(longhand_dsr(m["sr_daily"], 300, m["skew"], m["kurtosis"], v, 10)[1], abs=1e-12)
        assert "paired" in r                                   # dSR vs the reference alongside
    NS.main([dirs[2], "--dsr-n", "5"])
    single = capsys.readouterr().out
    assert "deflated SR (N=5)" in single and "Lo (2002) sampling variance" in single


# ------------------------------------------------ T41 fix wave: M1 all-rows leverage, M2 cost basis, M5, M7
def lagged_cost_return(cost_dollars, nav):
    """MARK books session s's fill cost into row s + 1: trade_cost_return[s + 1] = cost$_s / NAVpre_s (row 0: 0)."""
    return [0.0] + [c / v for c, v in zip(cost_dollars[:-1], nav[:-1])]


def test_cost_per_gmv_turnover_excludes_the_deployment_cost_like_tau(tmp_path):
    # C++-shaped rows: row 0 before the first execution, row 1 the deployment, row 4 executed at zero pre-trade
    # gross (not a tau session), row 7 the last row (never executed: execution needs t + 2 <= end)
    nets = [0.001, -0.002, 0.003, 0.0, 0.001, 0.002]
    executed = [0, 1, 1, 1, 1, 1, 1, 0]
    tau = [np.nan, np.nan, 0.1, 0.2, np.nan, 0.4, 0.05, 0.0]
    pretrade = [0, 0, 1e9, 1e9, 0, 1e9, 1e9, 1e9]
    cost_dollars = [0.0, 1e5, 3e3, 1e3, 7e3, 2e3, 4e3, 0.0]
    nav = [1e9, 1e9, 1.1e9, 0.9e9, 1.2e9, 1.05e9, 0.95e9, 1e9]
    cost_return = lagged_cost_return(cost_dollars, nav)
    d = write_nav(tmp_path / "nav", nets, executed=executed, tau=tau, pretrade=pretrade, cost_dollars=cost_dollars,
                  pretrade_nav=nav, cost_return=cost_return)
    daily = NS.load_daily(d, SCEN)
    s = NS.construction_stats(daily, NS.scenario_of(NS.load_summary(d)))
    sessions = [2, 3, 5, 6]
    tau_sum = sum(tau[k] for k in sessions)
    assert NS.deployment_row(daily) == 1 and s["tau_gmv_sessions"] == 4
    want = sum(cost_dollars[k] / nav[k] for k in sessions) / tau_sum
    assert s["cost_per_gmv_turnover"] == pytest.approx(want, rel=1e-15)
    # the same sessions read through the lagged trade_cost_return column give the same number
    assert s["cost_per_gmv_turnover"] == pytest.approx(sum(cost_return[k + 1] for k in sessions) / tau_sum, rel=1e-15)
    # the pre-fix numerator (every return row) carried the deployment's and the zero-gross row's costs; the
    # deployment row's own trade_cost_return is 0, so dropping row 1 from that sum would have changed nothing
    old = sum(cost_return[2:]) / tau_sum
    assert cost_return[1] == 0.0
    assert (old - s["cost_per_gmv_turnover"]) * tau_sum == pytest.approx(1e5 / 1e9 + 7e3 / 1.2e9, rel=1e-12)
    # cost_bps_traded is unchanged (every executed session)
    traded = [0.0, 5e8] + [1e7] * 6
    assert s["cost_bps_traded"] == pytest.approx(1e4 * sum(cost_dollars[1:7]) / sum(traded[1:7]), rel=1e-12)


def test_all_rows_leverage_in_text_and_json(tmp_path, capsys):
    nets = list(0.0004 + 0.01 * np.random.default_rng(41).normal(size=40))
    t = len(nets) + 2
    gross = np.r_[0.0, 0.3, np.linspace(0.9, 1.1, t - 2)]
    net = np.r_[0.0, 0.05, np.linspace(-0.02, 0.04, t - 2)]
    d = write_nav(tmp_path / "c", nets, gross=gross, net=net, summary_tau=0.05)
    out = tmp_path / "res.json"
    assert NS.main([str(d), "--dsr-n", "2", "--json", str(out)]) == 0
    text = capsys.readouterr().out
    r = json.loads(out.read_text())[0]
    assert r["csv_rows"] == t
    assert r["mean_gross_leverage_all_rows"] == pytest.approx(gross.mean(), abs=1e-15)
    assert r["mean_net_leverage_all_rows"] == pytest.approx(net.mean(), abs=1e-15)
    assert r["mean_gross_leverage"] == pytest.approx(gross[1:t - 1].mean(), abs=1e-15)  # return rows: previous close
    assert r["mean_gross_leverage"] != pytest.approx(r["mean_gross_leverage_all_rows"], abs=1e-6)
    assert "mean_gross_leverage_all_rows" in r["leverage_gate_basis"] and "R6'" in r["leverage_gate_basis"]
    assert (f"   leverage over all {t} CSV rows [R6' mechanics gate]: gross_lev_all_rows {gross.mean():.4f} "
            f"net_lev_all_rows {net.mean():+.4f} (construction gross_lev/net_lev: previous close over return rows)"
            in text.splitlines())


def test_same_series():
    a = {1: 0.1, 2: float("nan"), 3: -0.2}
    assert NS.same_series(a, dict(a)) and NS.same_series({}, {})
    assert not NS.same_series(a, {1: 0.1, 2: float("nan"), 3: -0.2000001})
    assert not NS.same_series(a, {1: 0.1, 2: float("nan")})
    assert not NS.same_series(a, {1: 0.1, 3: -0.2, 2: float("nan")})     # other session order
    assert not NS.same_series(a, {1: 0.1, 2: float("nan"), 4: -0.2})     # other sessions


def test_listing_warnings_go_to_stderr_and_never_refuse(tmp_path, capsys):
    rng = np.random.default_rng(31)
    a = list(0.0004 + 0.01 * rng.normal(size=200))
    b = list(0.0002 + 0.01 * rng.normal(size=200))
    d1, d2 = write_nav(tmp_path / "a", a), write_nav(tmp_path / "b", b)
    d3 = write_nav(tmp_path / "a-check", a)                   # a byte-identical check cell
    out = tmp_path / "res.json"
    assert NS.main([str(d1), str(d2), str(d3), "--dsr-n", "3", "--draws", "50", "--json", str(out)]) == 0
    cap = capsys.readouterr()
    dup = f"identical daily net series: {d1} and {d3} (a duplicate cell shrinks V[SR_n])"
    assert cap.err.splitlines() == [f"nav_summ: WARNING {dup}"]         # 3 defined SRs == N: no count warning
    assert "WARNING" not in cap.out
    res = json.loads(out.read_text())
    assert len(res) == 3 and all(r["deflated"]["dsr"] is not None for r in res)  # warned, not refused
    assert all(r["nav_summ_run"]["warnings"] == [dup] for r in res)
    # N differs from the number of dirs with a defined SR (default N = 10)
    assert NS.main([str(d1), str(d2)]) == 0
    err = capsys.readouterr().err
    assert "nav_summ: WARNING 2 listed dir(s) have a defined SR but --dsr-n is 10" in err and "identical" not in err
    # a constant net series has no SR and does not count
    flat = write_nav(tmp_path / "flat", [0.001] * 50)
    assert NS.main([str(d1), str(d2), str(flat), "--dsr-n", "3"]) == 0
    assert "2 listed dir(s) have a defined SR but --dsr-n is 3" in capsys.readouterr().err
    # a clean listing warns nothing
    assert NS.main([str(d1), str(d2), "--dsr-n", "2"]) == 0
    assert capsys.readouterr().err == ""


def test_json_records_argv_script_sha_and_git_head(tmp_path, capsys, monkeypatch):
    d = write_nav(tmp_path / "c", list(0.0003 + 0.01 * np.random.default_rng(5).normal(size=60)))
    out = tmp_path / "res.json"
    argv = [str(d), "--dsr-n", "2", "--json", str(out)]
    assert NS.main(argv) == 0
    run = json.loads(out.read_text())[0]["nav_summ_run"]
    assert run["argv"] == argv
    assert Path(run["script"]) == Path(NS.__file__).resolve()
    assert run["script_sha256"] == hashlib.sha256(Path(NS.__file__).read_bytes()).hexdigest()
    assert run["git_head"] is None or re.fullmatch(r"[0-9a-f]{40}(?:[0-9a-f]{24})?", run["git_head"])
    sha = "0123456789abcdef0123456789abcdef01234567"
    monkeypatch.setattr(NS.subprocess, "run", lambda *a, **k: subprocess.CompletedProcess(a, 0, sha + "\n", ""))
    assert NS.git_head(tmp_path) == sha
    monkeypatch.setattr(NS.subprocess, "run", lambda *a, **k: subprocess.CompletedProcess(a, 128, "", "fatal"))
    assert NS.git_head(tmp_path) is None                                 # not a repository
    monkeypatch.setattr(NS.subprocess, "run", lambda *a, **k: subprocess.CompletedProcess(a, 0, "HEAD\n", ""))
    assert NS.git_head(tmp_path) is None                                 # not a SHA

    def missing(*a, **k):
        raise FileNotFoundError("git")

    monkeypatch.setattr(NS.subprocess, "run", missing)
    assert NS.git_head(tmp_path) is None
    assert NS.main(argv) == 0                                            # best effort: null, never a failure
    assert json.loads(out.read_text())[0]["nav_summ_run"]["git_head"] is None


def test_existing_fields_and_text_unchanged_against_the_pre_t41_blob(tmp_path, capsys):
    """Every pre-T41 JSON field and text line is byte-identical for the same inputs, except cost_per_gmv_turnover
    (T41 M2); the only additions are the all-rows leverage keys and line (M1), nav_summ_run (M7) and the v6 C4
    post-ramp keys and line."""
    try:
        old = subprocess.run(["git", "cat-file", "-p", PRE_T41_BLOB], capture_output=True, check=True,
                             cwd=Path(NS.__file__).resolve().parent).stdout
    except (OSError, subprocess.CalledProcessError):
        pytest.skip("git history with the pre-T41 nav_summ blob is unavailable")
    (tmp_path / "old").mkdir()
    path = tmp_path / "old" / "nav_summ_pre_t41.py"
    path.write_bytes(old)
    spec = importlib.util.spec_from_file_location("nav_summ_pre_t41", path)
    pre = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pre)
    rng = np.random.default_rng(17)
    t = 300
    e = rng.normal(size=t)
    w_sha = write_weights(tmp_path / "w.json", 0.06)
    nav = 1e9 * np.cumprod(np.r_[1.0, 1.0 + 0.001 * rng.normal(size=t + 1)])
    cost_dollars = np.r_[0.0, 2e5, 1e3 + 3e3 * rng.random(t - 1), 0.0]
    executed = [0] + [1] * t + [0]
    dirs = []
    for k, (mu, rho) in enumerate(((0.0003, 0.0), (0.0005, 0.9), (0.0007, 0.8))):
        nets = mu + 0.01 * (rho * e + math.sqrt(1 - rho * rho) * rng.normal(size=t))
        gross = np.r_[0.0, 0.2, 0.9 + 0.2 * rng.random(t)]
        net = np.r_[0.0, 0.01, 0.02 * rng.normal(size=t)]
        tau = np.r_[np.nan, np.nan, 0.05 + 0.1 * rng.random(t - 1), 0.0]
        dirs.append(str(write_nav(tmp_path / f"c{k}", list(nets), weights_sha=w_sha if k else "f" * 64,
                                  gross=gross, net=net, tau=tau, summary_tau=float(np.nanmean(tau[2:-1])),
                                  executed=executed, cost_dollars=cost_dollars, pretrade_nav=nav,
                                  cost_return=lagged_cost_return(list(cost_dollars), list(nav)),
                                  v5={"mean_held_share": 0.9})))
    texts, rows = {}, {}
    for tag, module in (("new", NS), ("old", pre)):
        out = tmp_path / f"{tag}.json"
        assert module.main(dirs + ["--weights", str(tmp_path / "w.json"), "--reference", dirs[0], "--draws", "200",
                                   "--dsr-n", "3", "--json", str(out)]) == 0
        texts[tag] = capsys.readouterr().out.splitlines()
        rows[tag] = json.loads(out.read_text())
    assert len(rows["new"]) == len(rows["old"]) == 3
    for new, old_row in zip(rows["new"], rows["old"]):
        assert set(new) - set(old_row) == NEW_KEYS and set(old_row) <= set(new)
        for key in old_row:
            if key != "cost_per_gmv_turnover":
                assert json.dumps(new[key], sort_keys=True) == json.dumps(old_row[key], sort_keys=True), key
        assert new["cost_per_gmv_turnover"] < old_row["cost_per_gmv_turnover"]   # the 2e5 $ deployment is out
    kept = [line for line in texts["new"] if not line.startswith(("   leverage over all ", V6_LINE))]
    assert len(texts["new"]) - len(kept) == 6 and len(kept) == len(texts["old"])
    cost = re.compile(r"cost/GMV-tau \S+")
    for new_line, old_line in zip(kept, texts["old"]):
        if "cost/GMV-tau" in old_line:
            # the M2 value may round alike at 5 decimals; the JSON check above pins that it moved
            assert cost.sub("cost/GMV-tau <M2>", new_line) == cost.sub("cost/GMV-tau <M2>", old_line)
        else:
            assert new_line == old_line


# ------------------------------------------------ v6 prereg C4: post-ramp leverage (L calibration rows)
def test_post_ramp_leverage_by_hand_in_text_and_json(tmp_path, capsys):
    nets = list(0.0003 + 0.01 * np.random.default_rng(51).normal(size=100))
    t = len(nets) + 2                                              # 102 CSV rows: 39 after the first 63
    gross = np.r_[0.0, 0.2, np.linspace(0.6, 1.1, t - 2)]
    net = np.r_[0.0, 0.03, np.linspace(0.02, -0.01, t - 2)]
    d = write_nav(tmp_path / "c", nets, gross=gross, net=net, summary_tau=0.05)
    out = tmp_path / "res.json"
    assert NS.main([str(d), "--dsr-n", "2", "--json", str(out)]) == 0
    text = capsys.readouterr().out
    r = json.loads(out.read_text())[0]
    assert NS.RAMP_ROWS == 63 and r["post_ramp_rows"] == t - 63 == 39
    assert r["mean_gross_leverage_post_ramp"] == pytest.approx(gross[63:].mean(), abs=1e-15)
    assert r["mean_net_leverage_post_ramp"] == pytest.approx(net[63:].mean(), abs=1e-15)
    assert r["mean_gross_leverage_post_ramp"] > r["mean_gross_leverage_all_rows"]   # the ramp deflates all rows
    assert r["mean_gross_leverage_all_rows"] == pytest.approx(gross.mean(), abs=1e-15)  # the gate is unchanged
    assert (f"   leverage post-ramp over the 39 CSV rows after the first 63 [v6 C4 L calibration; gate stays all "
            f"rows]: gross_lev_post_ramp {gross[63:].mean():.4f} net_lev_post_ramp {net[63:].mean():+.4f}"
            in text.splitlines())
    # 63 rows or fewer: no post-ramp rows, null means, "na" in the text
    short = write_nav(tmp_path / "s", nets[:61], summary_tau=0.05)   # exactly 63 CSV rows
    s = NS.construction_stats(NS.load_daily(short, SCEN), NS.scenario_of(NS.load_summary(short)))
    assert s["csv_rows"] == 63 and s["post_ramp_rows"] == 0
    assert s["mean_gross_leverage_post_ramp"] is None and s["mean_net_leverage_post_ramp"] is None
    assert NS.main([str(short), "--dsr-n", "2"]) == 0
    assert ("   leverage post-ramp over the 0 CSV rows after the first 63 [v6 C4 L calibration; gate stays all "
            "rows]: gross_lev_post_ramp na net_lev_post_ramp na") in capsys.readouterr().out.splitlines()


def test_every_pre_v6_field_and_line_is_byte_identical(tmp_path, capsys):
    """Against nav_summ.py at 04e9d5bc: every JSON field (cost_per_gmv_turnover included; nav_summ_run is this
    run's provenance) and every text line is byte-identical for the same inputs; the only additions are the three
    post-ramp keys and one post-ramp line per dir."""
    try:
        old = subprocess.run(["git", "cat-file", "-p", PRE_V6_BLOB], capture_output=True, check=True,
                             cwd=Path(NS.__file__).resolve().parent).stdout
    except (OSError, subprocess.CalledProcessError):
        pytest.skip("git history with the pre-v6 nav_summ blob is unavailable")
    (tmp_path / "old").mkdir()
    path = tmp_path / "old" / "nav_summ_pre_v6.py"
    path.write_bytes(old)
    spec = importlib.util.spec_from_file_location("nav_summ_pre_v6", path)
    pre = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pre)
    rng = np.random.default_rng(29)
    t = 200
    e = rng.normal(size=t)
    w_sha = write_weights(tmp_path / "w.json", 0.06)
    nav = 1e9 * np.cumprod(np.r_[1.0, 1.0 + 0.001 * rng.normal(size=t + 1)])
    cost_dollars = np.r_[0.0, 2e5, 1e3 + 3e3 * rng.random(t - 1), 0.0]
    executed = [0] + [1] * t + [0]
    dirs = []
    for k, (mu, rho) in enumerate(((0.0003, 0.0), (0.0006, 0.8))):
        nets = mu + 0.01 * (rho * e + math.sqrt(1 - rho * rho) * rng.normal(size=t))
        gross = np.r_[0.0, 0.2, 0.9 + 0.2 * rng.random(t)]
        net = np.r_[0.0, 0.01, 0.02 * rng.normal(size=t)]
        tau = np.r_[np.nan, np.nan, 0.05 + 0.1 * rng.random(t - 1), 0.0]
        dirs.append(str(write_nav(tmp_path / f"c{k}", list(nets), weights_sha=w_sha, gross=gross, net=net, tau=tau,
                                  summary_tau=float(np.nanmean(tau[2:-1])), executed=executed,
                                  cost_dollars=cost_dollars, pretrade_nav=nav,
                                  cost_return=lagged_cost_return(list(cost_dollars), list(nav)),
                                  v5={"mean_held_share": 0.9})))
    texts, rows = {}, {}
    for tag, module in (("new", NS), ("old", pre)):
        out = tmp_path / f"{tag}.json"
        assert module.main(dirs + ["--weights", str(tmp_path / "w.json"), "--reference", dirs[0], "--draws", "200",
                                   "--dsr-n", "2", "--json", str(out)]) == 0
        texts[tag] = capsys.readouterr().out.splitlines()
        rows[tag] = json.loads(out.read_text())
    assert len(rows["new"]) == len(rows["old"]) == 2
    for new, old_row in zip(rows["new"], rows["old"]):
        assert set(new) - set(old_row) == V6_KEYS and set(old_row) <= set(new)
        for key in old_row:
            if key != "nav_summ_run":
                assert json.dumps(new[key], sort_keys=True) == json.dumps(old_row[key], sort_keys=True), key
        assert new["post_ramp_rows"] == new["csv_rows"] - 63
    kept = [line for line in texts["new"] if not line.startswith(V6_LINE)]
    assert len(texts["new"]) - len(kept) == 2 and kept == texts["old"]
