"""book_diagnostics.py (platform v8 lane G): one test per diagnostic on the tiny_world fixture and small synthetic
arrays, the lag-combined writer, the CLI end to end on a synthetic cell, the skip rule and the seal.

Run: "C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider atx-impl/tools/test_book_diagnostics.py

No real data. The research window is task W0-1's (atx-engine/tools/research_window.py).
"""
from __future__ import annotations

import contextlib
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import sys

import numpy as np
import pytest

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parents[1]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(REPO / "scripts" / "tests" / "fixtures"))
import alpha_report_card as arc  # noqa: E402
import backtest_integrity as BI  # noqa: E402
import book_diagnostics as BD  # noqa: E402
import fit_composition_weights as fcw  # noqa: E402
from test_alpha_report_card import CardWorld, make_panel  # noqa: E402
import tiny_world as TW  # noqa: E402

DAY = fcw.DAY_NS
S2 = "modeled-1bn-stale5-v1+swap-fin-v1"


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ================================================================================================= G-1a
def card(cid: str, decay, *, theme="value", ic21=0.02, ic_theta=None, f_theta=None, marginal=None) -> dict:
    c = {"schema": "atx.alpha-report-card/v1", "id": cid, "theme": theme, "decay": {"ic": list(decay)},
         "runner": {"horizons": {"21": {"mean": ic21}}}, "admission": {}}
    if ic_theta is not None:
        c["horizon"] = {"ic_theta": ic_theta}
    if f_theta is not None:
        c["admission"].update(f_theta=f_theta[0], f_theta_hac_t=f_theta[1])
    if marginal is not None:
        c["marginal_ic"] = marginal
    return c


def test_member_horizon_ic_theta_marginal_and_unscored():
    hs = np.arange(1, 64)
    slow = 0.02 * np.ones(63)                    # no decay: retention 1
    fast = 0.04 * 0.5 ** (hs - 1.0)              # a one-day signal the theta book cannot hold
    k6 = {"slow": {"id": "slow", "ic21": 0.03, "ic21_hac_t": 2.0, "marginal_ic21": 0.01, "marginal_hac_t": 1.5,
                   "max_abs_rho": 0.4, "max_rho_member": "fast"},
          "fast": {"id": "fast", "ic21": -0.01, "ic21_hac_t": -1.0, "marginal_ic21": -0.02, "marginal_hac_t": -2.5,
                   "max_abs_rho": 0.4, "max_rho_member": "slow"}}
    cards = {"slow": card("slow", slow, f_theta=(1e-4, 1.8)), "fast": card("fast", fast, ic_theta=0.0021),
             "unscored": card("unscored", [None] * 63)}
    book = {"slow": {"weight": 0.5, "sign": 1, "theme": "value"}, "fast": {"weight": 0.3, "sign": -1, "theme": "rev"},
            "unscored": {"weight": 0.2, "sign": 1, "theme": "quality"}, "dropped": {"weight": 0.0, "sign": 1}}
    res = BD.member_horizon(cards, book, k6)
    rows = {r["id"]: r for r in res["members"]}
    assert set(rows) == {"slow", "fast", "unscored"}                              # weight 0 is not a member
    w = 1 - 0.95 ** 63
    assert rows["slow"]["ic_theta"] == pytest.approx(0.02 * w, abs=1e-15)          # computed from the decay curve
    assert rows["slow"]["ic_theta_source"] == "decay curve" and rows["slow"]["retention"] == pytest.approx(1.0)
    assert rows["fast"]["ic_theta"] == pytest.approx(-0.0021) and rows["fast"]["ic_theta_source"] == "card"
    assert rows["fast"]["ic1"] == pytest.approx(-0.04)                             # oriented by the book's sign
    assert rows["fast"]["retention"] == pytest.approx(0.0021 / (0.04 * w))
    assert rows["fast"]["marginal_hac_t"] == pytest.approx(2.5) and rows["slow"]["marginal_ic21"] == 0.01
    assert (rows["slow"]["f_theta"], rows["slow"]["f_theta_hac_t"]) == (1e-4, 1.8)
    assert res["negative_at_theta"] == ["fast"] and res["unscored"] == ["unscored"]
    assert res["unscored_weight"] == pytest.approx(0.2)
    gross = 0.5 * 0.02 * w + 0.3 * 0.0021
    assert rows["slow"]["contribution_share_abs"] == pytest.approx(0.5 * 0.02 * w / gross)
    assert rows["fast"]["contribution_share_abs"] == pytest.approx(-0.3 * 0.0021 / gross)
    assert res["weighted_ic_theta"] == pytest.approx(0.5 * 0.02 * w - 0.3 * 0.0021)


# ================================================================================================= G-1b, G-1c
def reversal_world():
    """tiny_world's planted states as member signals, plus a planted fast member (the one-day reversal -r)."""
    w = TW.simulate(TW.DEFAULT_SEED)
    close = w["close"]
    r = np.zeros_like(close)
    r[1:] = close[1:] / close[:-1] - 1.0
    signals = {"planted_a": -(w["si_shares"] / w["shares_out"]), "planted_b": w["tiny_signal"],
               "copy_b": 2.0 * w["tiny_signal"], "noise_c": w["shares_out"], "fast_d": -r}
    members = [{"id": "planted_a", "weight": 0.25, "sign": 1, "theme": "short_interest"},
               {"id": "planted_b", "weight": 0.125, "sign": 1, "theme": "earnings_momentum"},
               {"id": "copy_b", "weight": 0.125, "sign": 1, "theme": "earnings_momentum"},
               {"id": "noise_c", "weight": 0.25, "sign": 1, "theme": "value"},
               {"id": "fast_d", "weight": 0.25, "sign": 1, "theme": "reversal_seasonality"}]
    rows = slice(TW.MEMBER_WARMUP, TW.DATES)
    return {k: v[rows] for k, v in signals.items()}, members, w["member"][rows].astype(bool)


def test_planted_fast_member_has_highest_turnover_share():
    signals, members, member = reversal_world()
    res, series = BD.turnover_attribution(signals, members, member, theta=0.05, leverage=1.25, score_from=63)
    m, th = res["members"], res["themes"]
    top = max(m, key=lambda k: m[k]["own_share"])
    assert top == "fast_d" and m["fast_d"]["own_share"] > 0.5                    # a quarter of the weight
    assert max(m, key=lambda k: m[k]["attributed_share"]["model"]) == "fast_d"
    assert max(th, key=lambda k: th[k]["own_share"]) == "reversal_seasonality"
    assert res["fast"]["ids"][0] == "fast_d" and res["top_by_own_share"][0] == "fast_d"
    assert sum(v["own_share"] for v in m.values()) == pytest.approx(1.0)
    shares = sum(v["attributed_share"]["model"] for v in m.values())
    assert shares + res["remainder"]["model"] == pytest.approx(1.0)
    assert sum(v["attributed_share"]["model"] for v in th.values()) == pytest.approx(shares)  # themes = members
    assert abs(res["remainder"]["model"]) < 0.25                                   # the re-rank is nearly linear
    assert m["planted_b"]["own_turnover"] == pytest.approx(m["copy_b"]["own_turnover"])  # the same ranks
    net = BD.netting_ratio(series["combined"]["model"], series["themes"], series["rows"])
    assert 0.0 < net["ratio"] < 1.0                                                # independent sleeves net
    again, _ = BD.turnover_attribution(signals, members, member, 0.05, 1.25, book_trades=None, score_from=63)
    assert again["members"]["fast_d"] == m["fast_d"]                               # deterministic


def test_netting_ratio_identical_and_independent_sleeves():
    rng = np.random.default_rng(3)
    t, n = 120, 80
    member = np.ones((t, n), dtype=bool)
    base = np.cumsum(rng.normal(size=(t, n)), axis=0)
    signals = {"a": base, "b": base.copy()}
    members = [{"id": "a", "weight": 0.6, "sign": 1, "theme": "x"}, {"id": "b", "weight": 0.4, "sign": 1, "theme": "y"}]
    res, series = BD.turnover_attribution(signals, members, member, theta=0.1, score_from=20)
    assert abs(res["remainder"]["model"]) < 1e-12                                  # identical ranks: exactly linear
    assert res["members"]["a"]["own_share"] == pytest.approx(0.6)
    assert res["members"]["a"]["attributed_share"]["model"] == pytest.approx(0.6)
    assert BD.netting_ratio(series["combined"]["model"], series["themes"], series["rows"])["ratio"] == \
        pytest.approx(1.0)                                                         # nothing cancels
    model_trades = BD.theta_trades(BD.desired_target(base, member), 0.1)
    res_b, _ = BD.turnover_attribution(signals, members, member, 0.1, book_trades=model_trades, score_from=20)
    assert res_b["members"]["a"]["attributed_share"]["book"] == pytest.approx(0.6)  # the book = the model here
    hand = BD.netting_ratio(np.array([1.0, 1.0]), {"a": np.array([1.0, 1.0]), "b": np.array([1.0, 1.0])})
    assert hand["ratio"] == 0.5 and hand["daily_ratio"]["median"] == 0.5
    signals["b"] = np.cumsum(rng.normal(size=(t, n)), axis=0)                      # an independent second sleeve
    _, series = BD.turnover_attribution(signals, members, member, theta=0.1, score_from=20)
    assert BD.netting_ratio(series["combined"]["model"], series["themes"], series["rows"])["ratio"] < 0.95


def test_theta_trades_and_desired_target_follow_the_construction():
    aim = np.array([[1.0, -1.0], [1.0, -1.0], [0.0, 0.0]])
    trades = BD.theta_trades(aim, 0.5)
    np.testing.assert_allclose(trades, [[0.5, -0.5], [0.25, -0.25], [-0.375, 0.375]])
    score = np.array([[3.0, 1.0, 2.0, np.nan]])
    member = np.array([[True, True, True, False]])
    d = BD.desired_target(score, member)
    np.testing.assert_allclose(d, [[0.5, -0.5, 0.0, 0.0]])                         # ranks .5, -.5, 0; gross 1


# ================================================================================================= G-2a
def test_variance_split_hand_case():
    k = BD.RISK_FACTORS
    cov = np.zeros((k, k))
    cov[0, 0] = 1e-4
    cov[1, 1], cov[2, 2], cov[51, 51] = 4e-5, 9e-5, 2.5e-5                        # ind_ff1, ind_ff2, style size
    cov[1, 51] = cov[51, 1] = 1e-5
    cov[60, :] = cov[:, 60] = np.nan                                               # an unforecast factor, unexposed
    w = np.array([0.5, -0.3, -0.2, 0.1])
    slot = np.array([0, 0, 1, BD.NO_EXPOSURE], dtype=np.uint8)
    styles = np.zeros((4, BD.RISK_STYLES))
    styles[:, 0] = [1.0, -1.0, 0.0, 0.0]
    spec = np.array([4e-4, 9e-4, 1e-4, 1e-4])
    s = BD.variance_split_session(w, slot, styles, cov, spec)
    assert s["market"] == pytest.approx(0.0, abs=1e-20)                             # x_market = 0
    assert s["industry"] == pytest.approx(0.2 * (4e-5 * 0.2 + 1e-5 * 0.8) + 3.6e-6)
    assert s["style"] == pytest.approx(0.8 * (2.5e-5 * 0.8 + 1e-5 * 0.2))
    assert s["industry"] + s["style"] == pytest.approx(2.44e-5)                    # x'Fx
    assert s["specific"] == pytest.approx(1.85e-4) and s["total"] == pytest.approx(2.094e-4)
    assert s["uncovered_gross"] == pytest.approx(0.1) and s["styles"]["size"] == pytest.approx(s["style"])
    cov[51, 51] = np.nan                                                            # an exposed factor unforecast
    assert BD.variance_split_session(w, slot, styles, cov, spec) is None
    agg = BD.variance_split([s, None, s], [2021, 2021, 2022])
    assert agg["complete_sessions"] == 2 and agg["incomplete_sessions"] == 1
    assert agg["mean_share"]["specific"] == pytest.approx(1.85e-4 / 2.094e-4)
    assert agg["mean_ex_ante_vol_annual"] == pytest.approx(math.sqrt(252 * 2.094e-4))
    assert set(agg["by_year"]) == {"2021", "2022"}


# ================================================================================================= G-2b
def test_ic_by_groups_planted_high_vol_tercile():
    rng = np.random.default_rng(11)
    t, n = 40, 300
    label = rng.normal(size=(t, n))
    vol = np.broadcast_to(np.arange(n, dtype=float), (t, n)).copy()                # names 200.. are the high tercile
    signal = np.where(np.arange(n) >= 200, label, rng.normal(size=(t, n)))
    valid = np.ones((t, n), dtype=bool)
    groups = BD.terciles(vol, valid)
    assert (groups[0, :100] == 0).all() and (groups[0, 100:200] == 1).all() and (groups[0, 200:] == 2).all()
    top = BD.top_n(vol, valid, 50)
    assert (top[0, 250:] == 0).all() and (top[0, :250] == 1).all()
    res = BD.ic_by_groups({"m": signal}, [{"id": "m", "weight": 1.0, "sign": 1}], label,
                          {"volatility": (groups, ["low", "mid", "high"]), "size_top50": (top, ["top50", "rest"])})
    vol_ic = res["members"]["m"]["volatility"]
    assert vol_ic["high"]["mean"] == pytest.approx(1.0) and abs(vol_ic["low"]["mean"]) < 0.1
    assert vol_ic["high"]["mean_names"] == 100 and res["members"]["m"]["size_top50"]["top50"]["mean"] == \
        pytest.approx(1.0)
    assert res["weighted_mean_ic"]["volatility"]["high"] == pytest.approx(1.0)
    neg = BD.ic_by_groups({"m": signal}, [{"id": "m", "weight": 1.0, "sign": -1}], label,
                          {"volatility": (groups, ["low", "mid", "high"])})
    assert neg["members"]["m"]["volatility"]["high"]["mean"] == pytest.approx(-1.0)  # oriented by the sign


# ================================================================================================= G-2c
def test_holding_over_adv_quantiles():
    res = BD.holding_over_adv(np.array([1.0, 2.0, -3.0, 0.0, 5.0]), np.array([10.0, 10.0, 10.0, 10.0, 0.0]))
    assert (res["cells"], res["measured"], res["no_adv"]) == (4, 3, 1)
    assert res["p50"] == pytest.approx(0.2) and res["max"] == pytest.approx(0.3)
    assert res["share_above_q"] == pytest.approx(2 / 3) and res["gross_share_above_q"] == pytest.approx(5 / 6)
    adv = BD.rolling_mean(np.array([[1.0], [2.0], [3.0], [4.0]]), 2, include_current=False)
    np.testing.assert_allclose(adv[:, 0], [np.nan, np.nan, 1.5, 2.5])               # [t-w, t), the NAV's ADV
    card_adv = BD.rolling_mean(np.array([[1.0], [2.0], [3.0], [4.0]]), 2, include_current=True)
    np.testing.assert_allclose(card_adv[:, 0], [np.nan, 1.5, 2.5, 3.5])


# ================================================================================================= G-3a
def test_planted_fee_schedule_reproduces_hand_drag():
    short = np.full((1, 12), 1e6)
    short[0, 11] = 0.0                                                             # not short: no fee
    ratio = np.array([[0.9, 0.1, 0.5, 0.3, 0.7, 0.2, 0.6, 0.4, 1.0, 0.8, np.nan, 5.0]])
    dec = BD.fee_deciles(short, ratio)
    np.testing.assert_array_equal(dec[0], [8, 0, 4, 2, 6, 1, 5, 3, 9, 7, -1, -2])
    res = BD.borrow_fee_drag(short, ratio, np.array([3.0 / 360.0]), np.array([1e9]))
    fees = sum(BD.FEE_SCHEDULE_BPS) + 25.0                                          # ten deciles + the median fee
    hand = 975.0 * 1e-4 * 1e6 * (3.0 / 360.0) / 1e9                                 # 975 bps on $1M, 3 days
    assert fees == 975.0 and res["drag"][0] == pytest.approx(hand, rel=1e-14)
    assert res["median_fee_bps"] == 25.0 and res["missing_ratio_short_share"] == pytest.approx(1 / 11)
    assert res["decile_short_share"] == pytest.approx([1 / 11] * 10)
    net = np.array([0.001, -0.002, 0.0015, 0.0005])
    charged = np.full(4, 1e-5)
    rs = BD.restate_net(net, charged, np.full(4, 3e-5))
    assert rs["restated_net_sharpe"] == pytest.approx(BD.sharpe(net - 2e-5))
    assert rs["delta"] < 0 and rs["stress_fee_drag_annual"] == pytest.approx(252 * 3e-5)


# ================================================================================================= G-3b
def test_projection_removes_the_beta_ic_and_keeps_the_alpha_ic():
    rng = np.random.default_rng(5)
    t, m = 80, 400
    beta = rng.normal(size=(t, m))
    alpha = rng.normal(size=(t, m))
    label = beta + alpha + 0.5 * rng.normal(size=(t, m))
    support = np.ones((t, m), dtype=bool)

    def project(q):
        out = np.empty_like(q)
        for d in range(t):
            x = np.column_stack([np.ones(m), beta[d]])
            out[d] = q[d] - x @ np.linalg.lstsq(x, q[d], rcond=None)[0]
        return out
    for name, signal in (("risk", beta + 0.3 * rng.normal(size=(t, m))), ("alpha", alpha)):
        raw = fcw.centered_tied_ranks(signal, support)
        res = BD.projection_ic(raw, project(raw), support, label)
        if name == "risk":
            assert res["before"]["mean"] > 0.4 and abs(res["after"]["mean"]) < 0.05
            assert res["retained"] < 0.15
        else:
            assert res["before"]["mean"] > 0.4 and res["retained"] > 0.9
    assert res["before"]["n"] == t and res["before"]["hac_t"] is not None


# ================================================================================================= G-3c
def test_signal_lag_costs():
    rng = np.random.default_rng(8)
    sessions = [BI.research_window().TRAIN_BEGIN_NS + k * DAY for k in range(500)]
    noise = 0.01 * rng.normal(size=500)
    base = dict(zip(sessions, 0.001 + noise))
    lagged = {k: dict(zip(sessions, 0.001 - 0.0003 * k + noise)) for k in (1, 2, 3)}
    del lagged[3][sessions[0]]                                                     # a missing session: common only
    res = BD.signal_lag(base, lagged)
    deltas = [res[str(k)]["delta"] for k in (1, 2, 3)]
    assert deltas[0] < 0 and deltas[0] > deltas[1] > deltas[2]
    x = np.array([lagged[1][s] for s in sessions])
    assert res["1"]["net_sharpe"] == pytest.approx(x.mean() / x.std(ddof=1) * math.sqrt(252))
    assert res["3"]["sessions"] == 499 and res["1"]["rho"] == pytest.approx(1.0)


# ================================================================================================= G-3d
def planted_pnl(merge_c_into_a: bool, seed: int = 4) -> tuple[np.ndarray, list[str], list[str]]:
    rng = np.random.default_rng(seed)
    t = 600
    f = {th: rng.normal(size=t) for th in ("a", "b", "c")}
    if merge_c_into_a:
        f["c"] = f["a"] + 0.05 * rng.normal(size=t)
    names, themes, cols = [], [], []
    for th in ("a", "b", "c"):
        for j in range(3):
            names.append(f"{th}{j}")
            themes.append(th)
            cols.append(f[th] + 0.3 * rng.normal(size=t))
    pnl = np.column_stack(cols)
    pnl[:10, 0] = np.nan                                                           # a sleeve with missing days
    return pnl, names, themes


def test_cluster_map_recovers_planted_themes():
    pnl, names, themes = planted_pnl(False)
    res = BD.cluster_map(pnl, names, themes)
    assert res["adjusted_rand_index"] == pytest.approx(1.0) and res["clusters_cut"] == 3
    assert res["mean_rho_within_theme"] > 0.8 and abs(res["mean_rho_between_themes"]) < 0.1
    assert res["effective_bets_themes"] == pytest.approx(3.0, abs=0.1)
    assert len(res["merges"]) == 8 and res["merges"][-1]["size"] == 9
    merged = BD.cluster_map(*planted_pnl(True))                                    # two labels, one bet
    assert merged["adjusted_rand_index"] < 0.7
    assert merged["effective_bets_themes"] == pytest.approx(1.8, abs=0.1)         # eigenvalues 2, 1, 0: 9 / 5
    assert BD.adjusted_rand([0, 0, 1, 1], ["x", "x", "y", "y"]) == 1.0
    assert BD.cut_clusters(BD.average_linkage(np.array([[0, 1, 5], [1, 0, 5], [5, 5, 0.0]])), 3, 2) == [0, 0, 1]


# ================================================================================================= lag-combined
def write_combined(root: Path, seed: int = 2, d: int = 8, n: int = 5) -> tuple[Path, str, dict]:
    rng = np.random.default_rng(seed)
    root.mkdir(parents=True)
    member = rng.random((d, n)) < 0.8
    signal = np.where(member, rng.normal(size=(d, n)), np.nan)
    signal[3, 0] = 0.0 if member[3, 0] else np.nan
    sessions = np.array([BI.research_window().TRAIN_BEGIN_NS + k * DAY for k in range(d)], dtype="<i8")
    payload = {"train_combined.f64": signal.astype("<f8").tobytes(), "train_combined_member.u8":
               member.astype("u1").tobytes(), "train_combined_finite.u8": np.isfinite(signal).astype("u1").tobytes(),
               "train_combined_sessions.i64": sessions.tobytes(),
               "train_combined_ids.u64": np.arange(1, n + 1, dtype="<u8").tobytes()}
    files = {}
    for name, data in payload.items():
        (root / name).write_bytes(data)
        files[name] = {"bytes": len(data), "sha256": sha(data)}
    manifest = {"schema": BD.COMBINED_SCHEMA, "status": "complete", "role": "train", "layout": "date-major-little-endian",
                "dates": d, "instruments": n, "score_begin": 2, "score_end": d, "finite_cells": int(member.sum()),
                "member_cells": int(member.sum()), "files": files, "role_window_required": True,
                "actual_trades_or_returns": False, "role_manifest_sha256": "ab" * 32}
    text = (json.dumps(manifest, indent=2) + "\n").encode()
    (root / "train_combined.json").write_bytes(text)
    return root / "train_combined.json", sha(text), {"signal": signal, "member": member, "payload": payload}


def test_lag_combined_shifts_and_keeps_the_saved_blend_contract(tmp_path):
    path, pin, src = write_combined(tmp_path / "comb")
    out = tmp_path / "lag2"
    assert BD.main(["lag-combined", "--combined", str(path), "--combined-sha256", pin, "--lag", "2",
                    "--output", str(out)]) == 0
    m = json.loads((out / "train_combined.json").read_bytes())
    lagged = np.frombuffer((out / "train_combined.f64").read_bytes(), dtype="<f8").reshape(8, 5)
    member, signal = src["member"], src["signal"]
    for d in range(8):
        for i in range(5):
            if not member[d, i]:
                assert math.isnan(lagged[d, i])
            elif d < 2 or not np.isfinite(signal[d - 2, i]):
                assert lagged[d, i] == 0.0                                          # neutral
            else:
                assert lagged[d, i] == signal[d - 2, i]
    for name, rec in m["files"].items():
        data = (out / name).read_bytes()
        assert rec == {"bytes": len(data), "sha256": sha(data)}
    for name in ("train_combined_member.u8", "train_combined_sessions.i64", "train_combined_ids.u64"):
        assert (out / name).read_bytes() == src["payload"][name]
    assert m["finite_cells"] == m["member_cells"] == int(member.sum()) and m["signal_lag_sessions"] == 2
    assert m["derived_from"]["sha256"] == pin and m["role_manifest_sha256"] == "ab" * 32
    assert BD.main(["lag-combined", "--combined", str(path), "--combined-sha256", "0" * 64, "--lag", "1",
                    "--output", str(tmp_path / "bad")]) == BD.EXIT_REFUSED
    assert not (tmp_path / "bad").exists()


# ================================================================================================= CLI
class Cell:
    """A synthetic finished cell on one role: u pass + cache + cards (the card tool), K6, weights, NAV with the
    financing columns, --emit-holdings (f64), research fields, a risk directory with --book-weights, lagged cells."""

    MEMBERS = {"slow_val": ("value", 0.3), "slow_mom": ("price_momentum", 0.3), "fast_rev": ("reversal", 0.2),
               "lowrisk": ("low_risk", 0.2)}

    def __init__(self, root: Path):
        self.root = root
        p = make_panel(dates=330, names=220, score_begin=140, seed=6)
        self.p = p
        d, n = p["close"].shape
        rng = np.random.default_rng(12)
        r = np.zeros((d, n))
        r[1:] = p["close"][1:] / p["close"][:-1] - 1.0
        state = np.cumsum(rng.normal(size=(d, n)), axis=0)
        signals = {"slow_val": state, "slow_mom": np.cumsum(rng.normal(size=(d, n)), axis=0),
                   "fast_rev": -r + 1e-9 * rng.normal(size=(d, n)), "lowrisk": -np.abs(state)}
        themes = {k: v[0] for k, v in self.MEMBERS.items()}
        self.world = CardWorld(root / "w", p, signals, themes=themes, min_names=100)
        self.cards = root / "cards"
        code, err = self.world.run(self.cards, extra=["--ic-theta"])
        assert code == 0, err
        self.role_sha = self.world.train_sha
        self.sessions, self.ids = p["sessions"], p["ids"]
        self.d, self.n = d, n
        self.weights = self.write_weights()
        self.k6 = self.write_k6()
        self.fields = self.write_fields(rng)
        self.nav, self.holdings = self.write_nav(root / "nav", np.random.default_rng(12), shift=0.0), root / "holdings"
        self.write_holdings(self.holdings, rng)
        self.lagged = {k: self.write_nav(root / f"nav-lag{k}", np.random.default_rng(12), shift=-0.0002 * k,
                                         recipe=False) for k in (1, 2)}
        self.risk = self.write_risk(root / "risk", rng)

    def write_weights(self) -> Path:
        cands = [{"id": k, "theme": th, "sign": 1, "tau": tau, "weight": w}
                 for (k, (th, w)), tau in zip(self.MEMBERS.items(), (0.02, 0.03, 0.6, 0.05))]
        doc = {"schema": fcw.WEIGHTS_SCHEMA, "weights": {k: w for k, (_, w) in self.MEMBERS.items()},
               "signs": {k: 1 for k in self.MEMBERS}, "provenance": {"candidates": cands}}
        path = self.root / "composition_weights.json"
        path.write_text(json.dumps(doc))
        return path

    def write_k6(self) -> Path:
        rows = [{"id": k, "ic21": 0.01, "ic21_hac_t": 1.0, "marginal_ic21": 0.005, "marginal_hac_t": 0.5,
                 "max_abs_rho": 0.3, "max_rho_member": "slow_val"} for k in self.MEMBERS]
        path = self.root / "marginal_ic.json"
        path.write_text(json.dumps({"schema": "atx.marginal-ic/v1", "candidates": rows,
                                    "window": {"last_decision_session_ns": int(self.sessions[-23])}}))
        return path

    def write_fields(self, rng) -> Path:
        d, n = self.d, self.n
        so = np.full((d, n), 1e8)
        arrays = {"shares_out": so, "si_shares": so * rng.uniform(0.01, 0.2, size=(d, n)),
                  "inst_own_share": rng.uniform(0.2, 0.9, size=(d, n)), "me_company": self.p["size"]}
        arrays["inst_own_share"][:, 5] = np.nan                                     # missing ratio: median fee
        fd = self.root / "fields"
        fd.mkdir()
        files = {}
        for name, arr in arrays.items():
            data = arr.astype("<f8").tobytes()
            (fd / f"{name}.f64").write_bytes(data)
            files[f"{name}.f64"] = {"bytes": len(data), "sha256": sha(data)}
        (fd / "manifest.json").write_text(json.dumps({"schema": "atx.research-role-fields/v1", "status": "complete",
                                                      "role": {"manifest_sha256": self.role_sha}, "files": files}))
        return fd

    def rows(self):
        return range(self.p["score_begin"] - 3, self.d)

    def write_nav(self, nd: Path, rng, shift: float, recipe: bool = True) -> Path:
        nd.mkdir()
        cols = ["session_index", "session_ns", "decision", "rebalance", "executed", "return_observation",
                "pretrade_nav", "posttrade_nav", "net_return", "short_gc_dollars", "short_warm_dollars",
                "short_special_dollars"]
        with (nd / f"daily_{S2}.csv").open("w", newline="") as f:
            w = csv.writer(f)
            w.writerow(cols)
            for k, t in enumerate(self.rows()):
                w.writerow([t, int(self.sessions[t]), 1, 1, 1, int(k >= 1), 1e9, 1e9,
                            0.0005 + shift + 0.01 * rng.normal() if k >= 1 else 0.0, 3e8, 5e7, 1e6])
        scen = {"scenario": S2, "primary": True, "financing": {"spec": {
            "tier_fee_bps": {"gc": 30, "warm": 100, "special": 500}, "short_spread_bps": 20, "day_count": "ACT/360"}}}
        (nd / "summary.json").write_text(json.dumps({"status": "complete", "primary_scenario": S2,
                                                     "scenarios": [scen], "combined_sha256": sha(nd.name.encode())}))
        if recipe:
            (nd / "recipe.json").write_text(json.dumps({"trade_fraction": 0.05, "aim_leverage": 1.2,
                                                        "liquidity_window": 63}))
        return nd

    def write_holdings(self, hd: Path, rng) -> None:
        hd.mkdir()
        rows, table, first = [], [], 0
        for k, t in enumerate(self.rows()):
            names = np.flatnonzero(rng.random(self.n) < 0.5)
            held = rng.normal(scale=1e6, size=names.size)
            for i, h in zip(names, held):
                rows.append([k, i, 1, h, 0, 0, 0, h / 1.2e9, np.nan, h / 1e9 + 1e-5, np.nan])
            table.append([t, int(self.sessions[t]), int(np.float64(1e9).view(np.uint64)), 1, first, names.size])
            first += names.size
        data = np.array(rows, dtype="<f8").tobytes()
        (hd / "holdings.f64").write_bytes(data)
        index = {"schema": "atx.nav-holdings-f64/v1",
                 "data": {"file": "holdings.f64", "dtype": "<f8", "layout": "row-major", "row_width": 11,
                          "columns": ["session", "name", "flags", "held_dollars", "filled_dollars",
                                      "fill_cost_dollars", "unfilled_dollars", "desired_weight", "rule_weight",
                                      "target_weight", "order_dollars"], "rows": len(rows), "bytes": len(data),
                          "sha256": sha(data)},
                 "instrument_ids": [int(x) for x in self.ids], "session_columns":
                 "session_index,session_ns,nav_post_bits,decision,first_row,rows", "sessions": table}
        text = (json.dumps(index, indent=1) + "\n").encode()
        (hd / "holdings_index.json").write_bytes(text)
        (hd / "manifest.json").write_text(json.dumps({"schema": "atx.nav-holdings/v2", "status": "complete",
                                                      "files": {"holdings.f64": sha(data),
                                                                "holdings_index.json": sha(text)}}))

    def write_risk(self, rd: Path, rng) -> Path:
        rd.mkdir()
        d, n, k = self.d, self.n, BD.RISK_FACTORS
        a = rng.normal(scale=0.01, size=(k, k))
        cov = np.broadcast_to(a @ a.T / k + 1e-6 * np.eye(k), (d, k, k)).copy()
        arrays = {"factor_covariance.f64": cov.astype("<f8"),
                  "specific_variance.f64": np.full((d, n), 4e-4).astype("<f8"),
                  "style_exposures.f32": rng.normal(size=(d, n, BD.RISK_STYLES)).astype("<f4"),
                  "industry_slot.u8": np.broadcast_to((np.arange(n) % 12).astype("u1"), (d, n)).copy()}
        files = {}
        for name, arr in arrays.items():
            data = np.ascontiguousarray(arr).tobytes()
            (rd / name).write_bytes(data)
            files[name] = {"sha256": sha(data), "bytes": len(data)}
        book = rd.parent / "book.csv"
        lines = ["session_ns,instrument_id,held_weight"]
        for t in range(self.d - 5, self.d):
            lines += [f"{int(self.sessions[t])},{int(self.ids[i])},{w!r}" for i, w in
                      zip(range(0, self.n, 3), rng.normal(scale=1e-3, size=len(range(0, self.n, 3))))]
        book.write_text("\n".join(lines) + "\n")
        (rd / "manifest.json").write_text(json.dumps({
            "schema": "atx.risk-model/v1", "status": "complete",
            "geometry": {"dates": d, "instruments": n, "factors": k, "styles": BD.RISK_STYLES},
            "role": {"path": str(self.world.manifest), "manifest_sha256": self.role_sha},
            "book_weights": {"path": str(book), "sha256": sha(book.read_bytes()), "rows": len(lines) - 1},
            "files": files}))
        return rd

    def argv(self, out: Path, *extra: str) -> list[str]:
        return ["run", "--output", str(out), "--cards", str(self.cards), "--marginal-ic", str(self.k6),
                "--weights", str(self.weights), "--nav", str(self.nav), "--holdings", str(self.holdings),
                "--u-pass", str(self.world.u), "--role", str(self.world.manifest), "--role-sha256", self.role_sha,
                "--fields", str(self.fields), "--risk", str(self.risk),
                *[f"--lagged={k}={v}" for k, v in self.lagged.items()], *extra]


def run_main(argv: list[str]) -> tuple[int, str]:
    err = io.StringIO()
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(err):
        code = BD.main(argv)
    return code, err.getvalue()


@pytest.fixture(scope="module")
def cell(tmp_path_factory):
    return Cell(tmp_path_factory.mktemp("cell"))


def test_cli_end_to_end_every_diagnostic_on_a_synthetic_cell(cell, tmp_path):
    out = tmp_path / "diagnostics-v8.json"
    code, err = run_main(cell.argv(out))
    assert code == 0, err
    doc = json.loads(out.read_bytes())
    assert doc["schema"] == BD.SCHEMA and "no diagnostic gates, selects or re-weights anything" in doc["declaration"]
    assert doc["window_id"] == BI.window_id() and set(doc["diagnostics"]) == set(BD.DIAGNOSTICS)
    for gid, entry in doc["diagnostics"].items():
        assert entry["status"] == "ok", (gid, entry.get("reason"))
        assert entry["method"] and entry["inputs"] and entry["result"] is not None and entry["question"]
    g = doc["diagnostics"]
    assert len(g["G-1a"]["result"]["members"]) == 4 and g["G-1a"]["inputs"]["marginal_ic"]["sha256"] == \
        sha(cell.k6.read_bytes())
    assert all(r["ic_theta_source"] == "card" for r in g["G-1a"]["result"]["members"])  # --ic-theta cards
    g1b = g["G-1b"]["result"]
    assert g1b["fast"]["ids"][0] == "fast_rev" and g1b["theta"] == 0.05 and g1b["leverage"] == 1.2
    assert max(g1b["members"], key=lambda k: g1b["members"][k]["own_share"]) == "fast_rev"
    assert set(g1b["combined_turnover"]) == {"model", "book"} and g1b["construction_source"] == "nav recipe"
    assert 0 < g["G-1c"]["result"]["themes_model"]["ratio"] <= 1.0
    g2a = g["G-2a"]["result"]
    assert g2a["complete_sessions"] == 5 and sum(g2a["mean_share"].values()) == pytest.approx(1.0)
    assert set(g["G-2b"]["result"]["members"]["slow_val"]) == {"volatility", "adv", "size_top1000", "all"}
    assert g["G-2b"]["result"]["size_field"] == "me_company"
    g2c = g["G-2c"]["result"]
    assert g2c["held"]["measured"] > 0 and g2c["aim_leverage"] == 1.2 and g2c["long"]["cells"] > 0
    g3a = g["G-3a"]["result"]
    assert g3a["rows_without_holdings"] == 0 and g3a["rows"] == len(list(cell.rows())) - 1
    assert g3a["missing_ratio_short_share"] > 0 and g3a["restated_net_sharpe"] is not None
    assert len(g3a["series"]["s2_fee_return"]) == g3a["rows"]
    assert set(g["G-3b"]["result"]["members"]) == {"lowrisk"}
    assert set(g["G-3c"]["result"]["delays"]) == {"1", "2"} and g["G-3c"]["result"]["delays"]["2"]["delta"] < 0
    assert g["G-3d"]["result"]["clusters_cut"] == 4 and len(g["G-3d"]["result"]["members"]) == 4
    code, err = run_main(cell.argv(out))                                            # never overwritten
    assert code == BD.EXIT_REFUSED and "never overwritten" in err


def test_cli_skips_absent_inputs_with_reasons(cell, tmp_path):
    out = tmp_path / "partial.json"
    code, err = run_main(["run", "--output", str(out), "--cards", str(cell.cards), "--weights", str(cell.weights),
                          "--nav", str(cell.nav)])
    assert code == 0, err
    g = json.loads(out.read_bytes())["diagnostics"]
    assert {k for k, v in g.items() if v["status"] == "ok"} == {"G-1a", "G-3d"}
    assert g["G-1b"]["reason"] == "--role not given" and g["G-1c"]["reason"] == "G-1b did not run"
    assert g["G-2a"]["reason"] == "--risk not given" and g["G-3c"]["reason"].startswith("no --lagged")
    assert g["G-3a"]["reason"] == "--role not given" and g["G-3a"]["method"]
    assert g["G-1a"]["result"]["members"][0]["marginal_ic21"] is None             # no K6 given


HOLDINGS_CSV_COLUMNS = ("session_ns,instrument_id,member,stale,tier,tier_missing,side,held_dollars,held_weight,nav_post,"
                        "fill,filled_dollars,fill_cost_dollars,unfilled_dollars,desired_weight,rule_weight,"
                        "target_weight,planned_trade_weight,planned_trade_dollars,order_placed,locate_blocked,"
                        "order_working,order_dollars")


def test_book_csv_and_the_v1_holdings_csv_read_as_the_f64(cell, tmp_path):
    book = tmp_path / "book.csv"
    argv = ["book-csv", "--holdings", str(cell.holdings), "--role", str(cell.world.manifest), "--role-sha256",
            cell.role_sha, "--output", str(book)]
    code, err = run_main(argv)
    assert code == 0, err
    h = BD.Holdings(cell.holdings, cell.sessions, cell.ids)
    rows = list(csv.DictReader(book.open(newline="")))
    assert list(rows[0]) == ["session_ns", "instrument_id", "weight"] and len(rows) == int((h.held != 0).sum())
    r0 = h.rows(0)
    assert (int(rows[0]["session_ns"]), int(rows[0]["instrument_id"])) == (int(h.session_ns[0]),
                                                                            int(cell.ids[h.name[r0][0]]))
    assert float(rows[0]["weight"]) == h.held[r0][0] / h.nav_post[0]               # repr round-trips exactly
    assert run_main(argv)[0] == BD.EXIT_REFUSED                                      # never overwritten
    v1 = tmp_path / "holdings-csv"
    v1.mkdir()
    fmt = (lambda x: "nan" if not math.isfinite(x) else repr(float(x)))
    lines, days = [HOLDINGS_CSV_COLUMNS], ["session_ns,decision,posttrade_nav"]
    for k in range(len(h.t)):
        r = h.rows(k)
        days.append(f"{int(h.session_ns[k])},{int(h.decision[k])},{fmt(h.nav_post[k])}")
        for i, held, des, tgt in zip(h.name[r], h.held[r], h.desired[r], h.target[r]):
            lines.append(f"{int(h.session_ns[k])},{int(cell.ids[i])},1,0,gc,0,x,{fmt(held)},0,{fmt(h.nav_post[k])},"
                         f"none,0,0,0,{fmt(des)},nan,{fmt(tgt)},0,0,0,0,0,nan")
    (v1 / "holdings.csv").write_text("\n".join(lines) + "\n")
    (v1 / "holdings_days.csv").write_text("\n".join(days) + "\n")
    (v1 / "manifest.json").write_text(json.dumps({"schema": "atx.nav-holdings/v1", "status": "complete", "files": {
        "holdings.csv": sha((v1 / "holdings.csv").read_bytes()),
        "holdings_days.csv": sha((v1 / "holdings_days.csv").read_bytes())}}))
    h1 = BD.Holdings(v1, cell.sessions, cell.ids)
    for key in ("t", "session_ns", "nav_post", "decision", "first", "count", "name", "held", "desired", "target"):
        np.testing.assert_array_equal(getattr(h1, key), getattr(h, key), err_msg=key)


def test_seal_refuses_a_sealed_session(cell, tmp_path):
    rw = BI.research_window()
    bad = tmp_path / "nav-sealed"
    bad.mkdir()
    for name in ("summary.json", "recipe.json"):
        (bad / name).write_bytes((cell.nav / name).read_bytes())
    text = (cell.nav / f"daily_{S2}.csv").read_text().splitlines()
    head, last = text[0], text[-1].split(",")
    last[1] = str(rw.SEAL_NS)                                                       # a session on the seal date
    (bad / f"daily_{S2}.csv").write_text("\n".join([head, *text[1:-1], ",".join(last)]) + "\n")
    out = tmp_path / "sealed.json"
    code, err = run_main(["run", "--output", str(out), "--nav", str(bad), "--only", "G-3c", "--lagged=1=" + str(bad)])
    assert code == BD.EXIT_REFUSED and "seal" in err and not out.exists()
    k6 = json.loads(cell.k6.read_bytes())
    k6["window"]["last_decision_session_ns"] = rw.SEAL_NS + DAY
    (tmp_path / "k6.json").write_text(json.dumps(k6))
    code, err = run_main(["run", "--output", str(out), "--cards", str(cell.cards), "--marginal-ic",
                          str(tmp_path / "k6.json"), "--only", "G-1a"])
    assert code == BD.EXIT_REFUSED and "seal" in err and not out.exists()
