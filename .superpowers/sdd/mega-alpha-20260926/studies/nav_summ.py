"""Compact summary of NAV replay output dirs (root helper): one line per scenario, plus (v5, T31) construction
stats, the netting ratio and the paired dSR(net) against a reference cell.

Usage:
  nav_summ.py NAV_DIR [NAV_DIR ...] [--weights W ...] [--reference REF_NAV_DIR] [--scenario NAME]
              [--draws 2000] [--block 21] [--seed 20260927] [--json OUT.json]

Per NAV dir, for the primary scenario (or --scenario), from its daily CSV (columns as strategy_nav_replay.cpp
writes them; rows and definitions mirror its summary):
  construction   mean gross / net / |net| leverage (the previous session's closing book over return rows, as the
                 summary's mean_gross_leverage), held_share (summary construction.v5.mean_held_share, T30; "na"
                 without it) and mean held names; tau_t = one_way_turnover_gmv over executed sessions with positive
                 pre-trade gross, the deployment session (first executed session with a fill) excluded: mean, p95
                 (numpy linear); cost per unit GMV turnover = sum trade_cost_return (return rows) / sum tau_t;
                 cost_bps_traded = 1e4 * sum trade_cost_dollars / sum traded_dollars (executed sessions).
  netting ratio  NR = tau mean / weighted_standalone_turnover of the weights file whose SHA-256 equals the NAV
                 summary's composition_weights_sha256 (--weights may repeat; a single unmatched file is used
                 with a warning). NR = tau_book / sum_k w_k tau_k (lane contract), reported on TRAIN.
  paired dSR     --reference: net returns of both cells on their common return sessions (session_ns);
                 dSR = SR(cell) - SR(ref), SR = mean / sd(ddof 1) * sqrt(252) (the summary's net_sharpe);
                 rho = Pearson correlation of the daily nets; Memmel (2003) SE:
                 Var = [2 - 2 rho + (SR_a^2 + SR_b^2 - 2 SR_a SR_b rho^2) / 2] / T with per-session SRs, x sqrt(252);
                 circular block bootstrap (Politis-Romano 1992; --draws, --block, fixed --seed) percentile 95% CI of dSR;
                 Ledoit-Wolf (2008) studentized circular block bootstrap 95% CI and p-value (R6'): delta-method SE of
                 dSR (population moments) with a Bartlett HAC (lag = block - 1) on the sample and the block
                 estimator Psi* = sum_blocks S_j S_j' / T on each resample, symmetric |t*| quantile.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import sys
from pathlib import Path

import numpy as np

ANNUAL = 252
DEFAULT_DRAWS, DEFAULT_BLOCK, DEFAULT_SEED = 2000, 21, 20260927


def fmt(v, spec: str) -> str:
    return format(v, spec) if isinstance(v, (int, float)) else "na"


def load_summary(d: Path) -> dict:
    return json.loads((Path(d) / "summary.json").read_text(encoding="utf-8"))


def scenario_of(summary: dict, name: str | None = None) -> dict:
    wanted = name or summary.get("primary_scenario")
    for v in summary["scenarios"]:
        if v["scenario"] == wanted:
            return v
    raise SystemExit(f"nav_summ: scenario {wanted!r} not in summary")


def print_scenarios(summary: dict) -> None:
    """The pre-T31 one-line-per-scenario view (same layout; a null statistic prints "na")."""
    print(f"rule={summary.get('rule')} status={summary.get('status')} primary={summary.get('primary_scenario')}")
    for v in summary["scenarios"]:
        yrs = v.get("calendar_year_returns") or []
        ys = " ".join(
            f"{y.get('year')}:{(y.get('net_compounded_return') or 0):+.3f}" if isinstance(y, dict) else str(y)
            for y in yrs
        )
        t = v.get("daily_turnover_gmv") or {}
        c = v.get("costs", {})
        fin = v.get("financing") or {}
        print(
            f"{v['scenario'][:34]:34s} net {fmt(v.get('net_sharpe'), '+.3f')} gross {fmt(v.get('gross_sharpe'), '+.3f')} "
            f"hac {fmt(v.get('hac_t'), '+.2f')} mu {fmt(v.get('ann_mean'), '+.4f')} vol {fmt(v.get('ann_vol'), '.4f')} "
            f"mdd {fmt(v.get('max_drawdown'), '.3f')} "
            f"tau_gmv mean {t.get('mean', float('nan')):.4f} p95 {t.get('p95', float('nan')):.4f} "
            f"tc {c.get('summed_trade_cost_return', 0):.4f} brw {c.get('summed_borrow_return', 0):.4f} "
            f"meets_d {v.get('meets_daily_turnover_mean')}/{v.get('meets_daily_turnover_p95')} | {ys}"
        )
        if fin:
            print("   financing:", json.dumps(fin)[:400])
        nz = v.get("neutralize") or v.get("neutralization")
        if nz:
            print("   neutralize:", json.dumps(nz)[:300])


# ------------------------------------------------------------------ daily CSV
def load_daily(d: Path, scenario: str) -> dict:
    """Columns of daily_<scenario>.csv: numeric columns as float arrays ("nan" -> NaN), others as lists."""
    path = Path(d) / f"daily_{scenario}.csv"
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.reader(stream))
    header, body = rows[0], rows[1:]
    out: dict = {}
    for n, name in enumerate(header):
        col = [r[n] for r in body]
        try:
            out[name] = np.array([float(x) for x in col], dtype=np.float64)
        except ValueError:
            out[name] = col
    return out


def return_mask(daily: dict) -> np.ndarray:
    """Rows that enter the summary's return statistics: return_observation and not the first row."""
    mask = daily["return_observation"] == 1
    mask[:1] = False
    return mask


def deployment_row(daily: dict) -> int | None:
    fills = np.flatnonzero((daily["executed"] == 1) & (daily["traded_dollars"] > 0))
    return int(fills[0]) if fills.size else None


def turnover_gmv(daily: dict) -> np.ndarray:
    """Daily one-way GMV turnover over executed sessions with positive pre-trade gross, deployment excluded."""
    keep = (daily["executed"] == 1) & (daily["pretrade_gross_dollars"] > 0)
    dep = deployment_row(daily)
    if dep is not None:
        keep[dep] = False
    return daily["one_way_turnover_gmv"][keep]


def construction_stats(daily: dict, scenario: dict) -> dict:
    ret = return_mask(daily)
    prev = np.flatnonzero(ret) - 1  # exposure that earned a return row: the previous session's closing book
    tau = turnover_gmv(daily)
    executed = daily["executed"] == 1
    v5 = (scenario.get("construction") or {}).get("v5") or {}
    traded = float(daily["traded_dollars"][executed].sum())
    tau_sum = float(tau.sum())
    any_prev = prev.size > 0
    return {
        "mean_gross_leverage": float(daily["gross_leverage"][prev].mean()) if any_prev else None,
        "mean_net_leverage": float(daily["net_leverage"][prev].mean()) if any_prev else None,
        "mean_abs_net_leverage": float(np.abs(daily["net_leverage"][prev]).mean()) if any_prev else None,
        "mean_held_names": float(daily["held_names"][prev].mean()) if any_prev else None,
        "held_share": v5.get("mean_held_share"),
        "tau_gmv_mean": float(tau.mean()) if tau.size else None,
        "tau_gmv_p95": float(np.quantile(tau, 0.95)) if tau.size else None,
        "tau_gmv_sessions": int(tau.size),
        "cost_per_gmv_turnover": (float(daily["trade_cost_return"][ret].sum()) / tau_sum) if tau_sum > 0 else None,
        "cost_bps_traded": (1e4 * float(daily["trade_cost_dollars"][executed].sum()) / traded) if traded > 0 else None,
        "return_rows": int(ret.sum()),
    }


def net_series(daily: dict) -> dict:
    ret = return_mask(daily)
    return dict(zip(daily["session_ns"][ret].astype(np.int64).tolist(), daily["net_return"][ret].tolist()))


# ------------------------------------------------------------------ statistics
def sharpe(x: np.ndarray) -> float:
    x = np.asarray(x, dtype=np.float64)
    sd = x.std(ddof=1) if x.size > 1 else 0.0
    return float(x.mean() / sd * math.sqrt(ANNUAL)) if sd > 0 else float("nan")


def memmel_se(sr_a: float, sr_b: float, rho: float, t: int) -> float:
    """Annualized SE of SR_a - SR_b (Memmel 2003 correction of Jobson-Korkie); SRs annualized in, per-session inside."""
    a, b = sr_a / math.sqrt(ANNUAL), sr_b / math.sqrt(ANNUAL)
    var = (2.0 - 2.0 * rho + 0.5 * (a * a + b * b - 2.0 * a * b * rho * rho)) / t
    return math.sqrt(max(var, 0.0) * ANNUAL)


def cbb_indices(t: int, draws: int, block: int, rng: np.random.Generator) -> np.ndarray:
    """(draws, t) circular block bootstrap indices: ceil(t / block) uniform starts, blocks wrap, truncated to t."""
    blocks = -(-t // block)
    starts = rng.integers(0, t, size=(draws, blocks))
    idx = (starts[:, :, None] + np.arange(block)[None, None, :]).reshape(draws, blocks * block)[:, :t]
    return idx % t


def _dsr_population(m: np.ndarray):
    """dSR (per session, population SD) and its gradient in (mu_a, mu_b, E a^2, E b^2); m has shape (..., 4)."""
    mu_a, mu_b, g_a, g_b = m[..., 0], m[..., 1], m[..., 2], m[..., 3]
    va, vb = g_a - mu_a * mu_a, g_b - mu_b * mu_b
    with np.errstate(invalid="ignore", divide="ignore"):
        d = mu_a / np.sqrt(va) - mu_b / np.sqrt(vb)
        grad = np.stack([g_a / va ** 1.5, -g_b / vb ** 1.5, -mu_a / (2 * va ** 1.5), mu_b / (2 * vb ** 1.5)], axis=-1)
    return d, grad


def bartlett_psi(y: np.ndarray, lag: int) -> np.ndarray:
    """Bartlett-kernel HAC long-run covariance of the rows of y (T x k), autocovariances / T."""
    e = y - y.mean(axis=0)
    t = e.shape[0]
    psi = e.T @ e / t
    for ell in range(1, min(lag, t - 1) + 1):
        g = e[ell:].T @ e[:-ell] / t
        psi += (1.0 - ell / (lag + 1.0)) * (g + g.T)
    return psi


def paired_stats(a: np.ndarray, b: np.ndarray, draws: int = DEFAULT_DRAWS, block: int = DEFAULT_BLOCK,
                 seed: int = DEFAULT_SEED) -> dict:
    """Paired dSR(net) = SR(a) - SR(b) on aligned daily nets: Memmel SE, CBB percentile CI, LW studentized CI."""
    a, b = np.asarray(a, dtype=np.float64), np.asarray(b, dtype=np.float64)
    t = a.size
    if t != b.size or t < 3:
        raise ValueError("paired_stats: need two aligned series of >= 3 sessions")
    sr_a, sr_b = sharpe(a), sharpe(b)
    sa, sb = a.std(ddof=1), b.std(ddof=1)
    rho = float(np.corrcoef(a, b)[0, 1]) if sa > 0 and sb > 0 else float("nan")
    dsr = sr_a - sr_b
    se = memmel_se(sr_a, sr_b, rho, t)
    rng = np.random.default_rng(seed)
    idx = cbb_indices(t, draws, block, rng)
    xa, xb = a[idx], b[idx]
    with np.errstate(invalid="ignore", divide="ignore"):
        boot = (xa.mean(axis=1) / xa.std(axis=1, ddof=1) - xb.mean(axis=1) / xb.std(axis=1, ddof=1)) * math.sqrt(ANNUAL)
    ok = np.isfinite(boot)
    lo, hi = np.quantile(boot[ok], [0.025, 0.975]) if ok.any() else (float("nan"), float("nan"))
    # Ledoit-Wolf studentized: population-moment dSR, delta-method SE
    y = np.column_stack([a, b, a * a, b * b])
    d0, g0 = _dsr_population(y.mean(axis=0))
    d0 = float(d0)
    s0 = math.sqrt(max(float(g0 @ bartlett_psi(y, block - 1) @ g0), 0.0) / t) if np.all(np.isfinite(g0)) else 0.0
    ys = y[idx]                                                  # draws x t x 4
    ms = ys.mean(axis=1)
    ds, gs = _dsr_population(ms)
    blocks = -(-t // block)
    padded = np.zeros((draws, blocks * block, 4))
    padded[:, :t] = ys - ms[:, None, :]
    sums = padded.reshape(draws, blocks, block, 4).sum(axis=2)  # block sums S_j of the centered resample
    psi = np.einsum("dni,dnj->dij", sums, sums) / t
    with np.errstate(invalid="ignore", divide="ignore"):
        ss = np.sqrt(np.einsum("di,dij,dj->d", gs, psi, gs) / t)
        tstar = np.abs(ds - d0) / ss
    good = np.isfinite(tstar) & (ss > 0)
    lw = {"dsr_population": d0 * math.sqrt(ANNUAL), "se": s0 * math.sqrt(ANNUAL), "ci95": [None, None],
          "p_value": None, "valid_draws": int(good.sum())}
    if good.any() and s0 > 0 and math.isfinite(d0):
        c = float(np.quantile(tstar[good], 0.95))
        lw["ci95"] = [(d0 - c * s0) * math.sqrt(ANNUAL), (d0 + c * s0) * math.sqrt(ANNUAL)]
        lw["p_value"] = (int((tstar[good] >= abs(d0) / s0).sum()) + 1) / (int(good.sum()) + 1)
    return {"sessions": t, "sr": sr_a, "sr_reference": sr_b, "dsr": dsr, "rho": rho, "memmel_se": se,
            "t": dsr / se if se > 0 else None, "cbb_ci95": [float(lo), float(hi)], "cbb_valid_draws": int(ok.sum()),
            "lw": lw, "draws": draws, "block": block, "seed": seed}


def align(cell: dict, reference: dict) -> tuple[np.ndarray, np.ndarray]:
    common = sorted(set(cell) & set(reference))
    return np.array([cell[s] for s in common]), np.array([reference[s] for s in common])


# ------------------------------------------------------------------ weights
def load_weights(paths: list[str]) -> list[dict]:
    out = []
    for p in paths:
        path = Path(p)
        if path.is_dir():
            path = path / "composition_weights.json"
        data = path.read_bytes()
        doc = json.loads(data)
        out.append({"path": str(path), "sha256": hashlib.sha256(data).hexdigest(),
                    "weighted_standalone_turnover": doc["provenance"]["weighted_standalone_turnover"],
                    "rule": doc["provenance"].get("rule")})
    return out


def match_weights(weights: list[dict], summary: dict) -> tuple[dict | None, str]:
    want = summary.get("composition_weights_sha256")
    for w in weights:
        if w["sha256"] == want:
            return w, "matched"
    if len(weights) == 1:
        return weights[0], f"UNMATCHED (NAV composition_weights_sha256 {str(want)[:8]})"
    return None, "none"


def netting_ratio(tau_mean: float | None, weighted_standalone_turnover: float) -> float | None:
    if tau_mean is None or not weighted_standalone_turnover > 0:
        return None
    return tau_mean / weighted_standalone_turnover


# ------------------------------------------------------------------ driver
def analyse(d: Path, args, weights: list[dict], ref_nets: dict | None) -> dict:
    summary = load_summary(d)
    scen = scenario_of(summary, args.scenario)
    daily = load_daily(d, scen["scenario"])
    stats = construction_stats(daily, scen)
    out = {"dir": str(d), "scenario": scen["scenario"], "rule": summary.get("rule"),
           "net_sharpe": scen.get("net_sharpe"), "gross_sharpe": scen.get("gross_sharpe"), "hac_t": scen.get("hac_t"),
           **stats}
    summ_tau = (scen.get("daily_turnover_gmv") or {}).get("mean")
    if summ_tau is not None and stats["tau_gmv_mean"] is not None and abs(summ_tau - stats["tau_gmv_mean"]) > 1e-9:
        out["warning_tau"] = f"CSV tau mean {stats['tau_gmv_mean']:.6f} != summary {summ_tau:.6f}"
    tau_book = summ_tau if summ_tau is not None else stats["tau_gmv_mean"]
    if weights:
        w, how = match_weights(weights, summary)
        out["weights_match"] = how
        if w is not None:
            out.update(netting_ratio=netting_ratio(tau_book, w["weighted_standalone_turnover"]),
                       weighted_standalone_turnover=w["weighted_standalone_turnover"], tau_book=tau_book,
                       weights_sha256=w["sha256"])
    if ref_nets is not None:
        a, b = align(net_series(daily), ref_nets)
        out["paired"] = paired_stats(a, b, args.draws, args.block, args.seed)
    return out


def print_analysis(r: dict, reference: str | None) -> None:
    print(f"== {r['dir']} [{r['scenario']}] rule={r['rule']}")
    print(f"   construction: gross_lev {fmt(r['mean_gross_leverage'], '.4f')} net_lev {fmt(r['mean_net_leverage'], '+.4f')} "
          f"|net| {fmt(r['mean_abs_net_leverage'], '.4f')} held_share {fmt(r['held_share'], '.4f')} "
          f"held_names {fmt(r['mean_held_names'], '.1f')} tau_gmv mean {fmt(r['tau_gmv_mean'], '.4f')} "
          f"p95 {fmt(r['tau_gmv_p95'], '.4f')} ({r['tau_gmv_sessions']} sessions) "
          f"cost/GMV-tau {fmt(r['cost_per_gmv_turnover'], '.5f')} cost_bps_traded {fmt(r['cost_bps_traded'], '.2f')}")
    if "warning_tau" in r:
        print(f"   WARNING {r['warning_tau']}")
    if "weights_match" in r:
        if "netting_ratio" not in r:
            print("   netting_ratio: na (no --weights file matches this NAV's composition_weights_sha256)")
        else:
            print(f"   netting_ratio = tau_book / weighted_standalone_turnover = {fmt(r['tau_book'], '.4f')} / "
                  f"{fmt(r['weighted_standalone_turnover'], '.4f')} = {fmt(r['netting_ratio'], '.3f')} "
                  f"(weights {r['weights_sha256'][:8]} {r['weights_match']})")
    p = r.get("paired")
    if p:
        lw = p["lw"]
        print(f"   paired vs {reference}: dSR(net) {p['dsr']:+.3f} = SR {p['sr']:+.3f} - SR {p['sr_reference']:+.3f} | "
              f"rho {fmt(p['rho'], '.3f')} | T {p['sessions']} | Memmel SE {p['memmel_se']:.3f} t {fmt(p['t'], '+.2f')} | "
              f"CBB 95% [{p['cbb_ci95'][0]:+.3f}, {p['cbb_ci95'][1]:+.3f}] | LW studentized 95% "
              f"[{fmt(lw['ci95'][0], '+.3f')}, {fmt(lw['ci95'][1], '+.3f')}] p {fmt(lw['p_value'], '.3f')} "
              f"(dSR_pop {lw['dsr_population']:+.3f}; {p['draws']} draws, block {p['block']}, seed {p['seed']})")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("dirs", nargs="+", help="NAV output directories (summary.json + daily_<scenario>.csv)")
    ap.add_argument("--weights", action="append", default=[], help="composition_weights.json (or its dir); repeatable")
    ap.add_argument("--reference", default=None, help="reference NAV dir for the paired dSR(net)")
    ap.add_argument("--scenario", default=None, help="scenario name (default: each summary's primary)")
    ap.add_argument("--draws", type=int, default=DEFAULT_DRAWS)
    ap.add_argument("--block", type=int, default=DEFAULT_BLOCK)
    ap.add_argument("--seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--json", default=None, help="also write the per-dir results as JSON")
    args = ap.parse_args(argv)
    weights = load_weights(args.weights)
    ref_nets = None
    if args.reference:
        ref_summary = load_summary(Path(args.reference))
        ref_nets = net_series(load_daily(Path(args.reference), scenario_of(ref_summary, args.scenario)["scenario"]))
    results = []
    for d in args.dirs:
        print_scenarios(load_summary(Path(d)))
        r = analyse(Path(d), args, weights, ref_nets)
        print_analysis(r, args.reference)
        results.append(r)
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
