"""Compact summary of NAV replay output dirs (root helper): one line per scenario, plus (v5, T31) construction
stats, the netting ratio and the paired dSR(net) against a reference cell.

Usage:
  nav_summ.py NAV_DIR [NAV_DIR ...] [--weights W ...] [--reference REF_NAV_DIR] [--scenario NAME]
              [--draws 2000] [--block 21] [--seed 20260927] [--dsr-n 10] [--json OUT.json]

Per NAV dir, for the primary scenario (or --scenario), from its daily CSV (columns as strategy_nav_replay.cpp
writes them; rows and definitions mirror its summary):
  construction   mean gross / net / |net| leverage (the previous session's closing book over return rows, as the
                 summary's mean_gross_leverage), held_share (summary construction.v5.mean_held_share, T30; "na"
                 without it) and mean held names; tau_t = one_way_turnover_gmv over executed sessions with positive
                 pre-trade gross, the deployment session (first executed session with a fill) excluded: mean, p95
                 (numpy linear); cost per unit GMV turnover = sum over the same tau_t sessions s of
                 trade_cost_dollars_s / pretrade_nav_s (exactly the trade_cost_return MARK books into row s + 1, so
                 the deployment session's cost is excluded as it is from the denominator) / sum tau_t;
                 cost_bps_traded = 1e4 * sum trade_cost_dollars / sum traded_dollars (executed sessions).
  leverage gate  mean_gross_leverage_all_rows / mean_net_leverage_all_rows = mean of gross_leverage / net_leverage
                 over EVERY row of the daily CSV: the ruled D1/R6' definition the R6' mechanics gate uses (the
                 return-row means above are kept for continuity and equal the summary's mean_gross_leverage).
  post-ramp      (v6 prereg C4) mean_gross_leverage_post_ramp / mean_net_leverage_post_ramp = the same means over
                 the CSV rows after the first 63 (post_ramp_rows of them; null when there are none): the steady-state
                 book that L is calibrated on. The gate itself stays the all-rows mean.
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
  DSR            (R6', plan-s4 4.E; Bailey & Lopez de Prado 2014) for every NAV dir passed:
                 DSR = Phi[(SR - SR0) sqrt(T - 1) / sqrt(1 - g3 SR + (g4 - 1) SR^2 / 4)], SR the per-session net Sharpe
                 (ddof 1), T the return rows, g3 skewness and g4 (non-excess) kurtosis of the daily nets (population
                 moments); SR0 = sqrt(V[SR_n]) [(1 - gamma) PhiInv(1 - 1/N) + gamma PhiInv(1 - 1/(N e))],
                 gamma = 0.5772156649, N = --dsr-n (default 10). V[SR_n] = sample variance (ddof 1) of the per-session
                 SRs of the NAV dirs on the command line; with a single dir, the Lo (2002) sampling variance
                 (1 + SR^2 / 2) / T of that dir (flagged in the output). Phi via math.erfc, PhiInv by bisection on it.
                 Warns on stderr (never refuses) when the number of dirs with a defined SR differs from N, and when
                 two listed dirs have identical daily net series (both silently change V[SR_n], hence SR0).
  provenance     --json: every row carries nav_summ_run = {argv, script, script_sha256 (of this file), git_head
                 (git rev-parse HEAD of this file's checkout; null without git), warnings}. The top level stays the
                 per-dir list so existing consumers and field-by-field diffs are unchanged.

Backtest integrity (platform v7 lane L2; statistics in backtest_integrity.py, pure numpy). Every option below only
ADDS lines / JSON keys; without them the output is byte-identical to the v6.1 nav_summ (the numbers above never move):
  --ledger PATH          append each listed dir to the atx.trial-ledger/v1 JSONL (R5.1) as one trial of --ledger-kind
                         (admission|composition|construction|universe|data; default construction; --ledger-count per
                         line, --ledger-note), with its pins, TRAIN window, daily CSV path + SHA-256 and S2 net SR.
                         Idempotent on (kind, daily series SHA-256): an identity re-run adds no trial; a series with a
                         session outside the research window's TRAIN (research_window.py) is refused.
  --ledger-n PATH        print the Appendix A block: trials by kind and window from that ledger (no dirs needed).
  --effective-n SOURCE   effective-N DSR beside the cell-count DSR and the Lo null: ONC clusters (Lopez de Prado &
                         Lewis 2019) of the trial series of SOURCE = "dirs" (the listed dirs) or a ledger PATH (its
                         series lines), aligned on their common sessions; N_eff = clusters, V[SR_k] across the
                         clusters' inverse-variance series (--onc-init restarts, --onc-seed).
  --pbo [DIR ...]        CSCV PBO (Bailey et al. 2017) over the given cells (default: the listed dirs; >= 4 cells):
                         --pbo-blocks (16) blocks on the common sessions, all C(16,8) = 12,870 splits unless
                         --pbo-max-splits K (> 0) asks for a seeded subsample (then flagged); --pbo-json OUT.
  --psr                  PSR and MinTRL (Bailey & Lopez de Prado 2012) of every listed dir against --psr-benchmarks
                         (annualized SR*, default 0,0.5) at --psr-alpha (.05).

Validation kit (platform v8 V-1). Every option below is opt-in; without them the output is byte-identical to v7. The
research window (TRAIN and the seal) is read from atx-engine/tools/research_window.py; any NAV daily CSV with a session
at or after the seal is refused before a statistic is formed.
  --protocol v8          the v8 pre-registration (item 4): bootstrap seed 20260929 and 4,999 resamples (block 21) unless
                         --seed / --draws are given; a year table per dir; ledger lines carry origin (--origin required
                         with --ledger), window_id and the hash chain; --ledger-n adds the v8 Appendix A block.
  --origin CLASS         prior | grid | mined (contract K5), recorded in every appended ledger line.
  --rerun-of ID --rerun-basis window|blind|returns
                         the appended line re-runs trial ID: window = a ledgered cell re-run on a longer window (no new
                         trial); blind / returns = the defect rule (v8-prereg item 7): a re-run decided without seeing
                         returns replaces the invalid cell, one decided because the returns looked wrong is a new trial.
  --ledger-defect REASON the appended line is an invalid cell: logged and excluded from N.
  protocol lines         (W0-3; written by `research_cycle.py ledger-protocol`, lane A) kind protocol, no cell, count 0:
                         every reader here skips them when it lists cells or counts N, and the hash chain covers them.
  --year-table           per dir and calendar year: return rows, compounded net return, net Sharpe, volatility, mean
                         daily GMV turnover (the tau_t sessions) and cost per traded dollar (bps).
  --dsr-ledger PATH      DSR per dir with N = the ledger's construction trial count (+1 when the dir is not in it) and
                         V[SR] from the construction cells ledgered on the current window (OD-4; v8-prereg item 3); the
                         legacy variance (lines without a window_id) is reported beside it and gates nothing.
  --bundle BASE FINAL    the cumulative paired test FINAL vs BASE (e.g. V8-F vs B0c): paired dSR(net) with Memmel SE,
                         CBB percentile CI and the studentized CBB (Ledoit-Wolf) two-sided and one-sided p, a per-year
                         dSR table and the verdict dSR > 0 and one-sided p < --bundle-alpha (.10, the freeze gate);
                         --bundle-json OUT writes it.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

import backtest_integrity as BI

ANNUAL = 252
DEFAULT_DRAWS, DEFAULT_BLOCK, DEFAULT_SEED = 2000, 21, 20260927
V8_DRAWS, V8_SEED = 4999, 20260929  # v8-prereg item 4 (block stays DEFAULT_BLOCK = 21)
PROTOCOLS = ("v7", "v8")
BUNDLE_ALPHA = 0.10  # v8-prereg item 9: the freeze gate's cumulative paired test, bootstrap p < .10
DEFAULT_DSR_N = 10  # R6': N = 10 construction cells
EULER_GAMMA = 0.5772156649015329
LEVERAGE_GATE_BASIS = ("all_rows: the R6' mechanics gate (ruled D1/R6') reads mean_gross_leverage_all_rows / "
                       "mean_net_leverage_all_rows; mean_gross_leverage / mean_net_leverage are return-row means "
                       "(previous close), kept for continuity")
RAMP_ROWS = 63  # v6 prereg C4: the theta-ramp rows dropped from the post-ramp (L calibration) means


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
    return load_daily_csv(Path(d) / f"daily_{scenario}.csv")


def load_daily_csv(path: Path, *, allow_sealed: bool = False) -> dict:
    """``load_daily`` of an explicit CSV path (the trial ledger records the path).

    The session column is checked against the research seal before any other column is parsed: a session at or after
    the seal refuses the file (SystemExit). Only holdout_gate.py, the owner-ruled reader of the hidden blocks, passes
    ``allow_sealed``."""
    with Path(path).open(newline="", encoding="utf-8") as stream:
        rows = list(csv.reader(stream))
    header, body = rows[0], rows[1:]
    if "session_ns" in header and not allow_sealed:
        k = header.index("session_ns")
        try:
            BI.refuse_sealed([int(float(r[k])) for r in body], f"nav_summ: {path}")
        except ValueError as exc:
            raise SystemExit(str(exc)) from exc
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


def turnover_rows(daily: dict) -> np.ndarray:
    """The tau_t sessions: executed with positive pre-trade gross, the deployment session excluded (bool mask)."""
    keep = (daily["executed"] == 1) & (daily["pretrade_gross_dollars"] > 0)
    dep = deployment_row(daily)
    if dep is not None:
        keep[dep] = False
    return keep


def turnover_gmv(daily: dict) -> np.ndarray:
    """Daily one-way GMV turnover over executed sessions with positive pre-trade gross, deployment excluded."""
    return daily["one_way_turnover_gmv"][turnover_rows(daily)]


def construction_stats(daily: dict, scenario: dict) -> dict:
    ret = return_mask(daily)
    prev = np.flatnonzero(ret) - 1  # exposure that earned a return row: the previous session's closing book
    keep = turnover_rows(daily)
    tau = daily["one_way_turnover_gmv"][keep]
    executed = daily["executed"] == 1
    v5 = (scenario.get("construction") or {}).get("v5") or {}
    traded = float(daily["traded_dollars"][executed].sum())
    tau_sum = float(tau.sum())
    # Session s's fill cost in NAV-return units, over the tau_t sessions only: trade_cost_dollars_s / pretrade_nav_s
    # is bit-for-bit the trade_cost_return MARK books into row s + 1 (the fill costs of t-1 land in r_t), so the
    # numerator excludes the deployment session exactly as the denominator does (T31 Minor 3, T41 M2).
    cost_tau = float((daily["trade_cost_dollars"][keep] / daily["pretrade_nav"][keep]).sum())
    rows = int(daily["gross_leverage"].size)
    any_prev = prev.size > 0
    post_ramp = rows > RAMP_ROWS
    return {
        "mean_gross_leverage_post_ramp": float(daily["gross_leverage"][RAMP_ROWS:].mean()) if post_ramp else None,
        "mean_net_leverage_post_ramp": float(daily["net_leverage"][RAMP_ROWS:].mean()) if post_ramp else None,
        "post_ramp_rows": max(rows - RAMP_ROWS, 0),
        "mean_gross_leverage": float(daily["gross_leverage"][prev].mean()) if any_prev else None,
        "mean_net_leverage": float(daily["net_leverage"][prev].mean()) if any_prev else None,
        "mean_abs_net_leverage": float(np.abs(daily["net_leverage"][prev]).mean()) if any_prev else None,
        "mean_gross_leverage_all_rows": float(daily["gross_leverage"].mean()) if rows else None,
        "mean_net_leverage_all_rows": float(daily["net_leverage"].mean()) if rows else None,
        "csv_rows": rows,
        "leverage_gate_basis": LEVERAGE_GATE_BASIS,
        "mean_held_names": float(daily["held_names"][prev].mean()) if any_prev else None,
        "held_share": v5.get("mean_held_share"),
        "tau_gmv_mean": float(tau.mean()) if tau.size else None,
        "tau_gmv_p95": float(np.quantile(tau, 0.95)) if tau.size else None,
        "tau_gmv_sessions": int(tau.size),
        "cost_per_gmv_turnover": cost_tau / tau_sum if tau_sum > 0 else None,
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
                 seed: int = DEFAULT_SEED, one_sided: bool = False) -> dict:
    """Paired dSR(net) = SR(a) - SR(b) on aligned daily nets: Memmel SE, CBB percentile CI, LW studentized CI.

    ``one_sided`` adds lw["p_one_sided"] (H0: dSR <= 0) from the same resamples: the share of signed studentized draws
    (ds - d0) / ss at or above d0 / s0, (k + 1) / (valid + 1); without it the result is unchanged."""
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
    if one_sided:
        lw["p_one_sided"] = None
        if good.any() and s0 > 0 and math.isfinite(d0):
            signed = (ds[good] - d0) / ss[good]
            lw["p_one_sided"] = (int((signed >= d0 / s0).sum()) + 1) / (int(good.sum()) + 1)
    return {"sessions": t, "sr": sr_a, "sr_reference": sr_b, "dsr": dsr, "rho": rho, "memmel_se": se,
            "t": dsr / se if se > 0 else None, "cbb_ci95": [float(lo), float(hi)], "cbb_valid_draws": int(ok.sum()),
            "lw": lw, "draws": draws, "block": block, "seed": seed}


def norm_cdf(x: float) -> float:
    return 0.5 * math.erfc(-x / math.sqrt(2.0))


def norm_ppf(p: float) -> float:
    """Standard normal quantile by bisection on ``norm_cdf`` (to double precision; no scipy)."""
    if not 0.0 < p < 1.0:
        raise ValueError(f"norm_ppf: p must be in (0, 1), got {p}")
    lo, hi = -40.0, 40.0
    for _ in range(200):
        mid = 0.5 * (lo + hi)
        if norm_cdf(mid) < p:
            lo = mid
        else:
            hi = mid
        if hi - lo <= 1e-15 * max(1.0, abs(mid)):
            break
    return 0.5 * (lo + hi)


def net_moments(x: np.ndarray) -> dict:
    """Per-session Sharpe (ddof 1), skewness and non-excess kurtosis (population moments) of the daily nets."""
    x = np.asarray(x, dtype=np.float64)
    t = int(x.size)
    e = x - x.mean() if t else x
    m2 = float((e ** 2).mean()) if t else 0.0
    sd = float(x.std(ddof=1)) if t > 1 else 0.0
    return {"sessions": t, "sr_daily": float(x.mean()) / sd if sd > 0 else None,
            "skew": float((e ** 3).mean()) / m2 ** 1.5 if m2 > 0 else None,
            "kurtosis": float((e ** 4).mean()) / m2 ** 2 if m2 > 0 else None}


def expected_max_sr(variance: float, n: int) -> float:
    """SR0 = sqrt(V[SR_n]) [(1 - gamma) PhiInv(1 - 1/N) + gamma PhiInv(1 - 1/(N e))] (same units as V's SRs)."""
    return math.sqrt(variance) * ((1.0 - EULER_GAMMA) * norm_ppf(1.0 - 1.0 / n) +
                                  EULER_GAMMA * norm_ppf(1.0 - 1.0 / (n * math.e)))


def deflated_sharpe(sr: float, t: int, skew: float, kurtosis: float, sr0: float) -> float | None:
    """DSR = Phi[(SR - SR0) sqrt(T - 1) / sqrt(1 - g3 SR + (g4 - 1) SR^2 / 4)] with per-session SR and SR0."""
    denom = 1.0 - skew * sr + (kurtosis - 1.0) * sr * sr / 4.0
    if not (denom > 0 and t > 1):
        return None
    return norm_cdf((sr - sr0) * math.sqrt(t - 1) / math.sqrt(denom))


def dsr_rows(moments: list[dict], n: int) -> list[dict]:
    """DSR per NAV dir; V[SR_n] from the dirs' per-session SRs (ddof 1), or Lo (2002) with a single dir."""
    if n < 2:
        raise ValueError("DSR needs N >= 2")
    srs = [m["sr_daily"] for m in moments if m["sr_daily"] is not None]
    cross = len(moments) >= 2 and len(srs) >= 2
    v_cross = float(np.var(srs, ddof=1)) if cross else None
    out = []
    for m in moments:
        sr, t = m["sr_daily"], m["sessions"]
        row = {"n": n, "sessions": t, "sr_daily": sr, "skew": m["skew"], "kurtosis": m["kurtosis"],
               "sr_annual": sr * math.sqrt(ANNUAL) if sr is not None else None}
        if sr is None or m["skew"] is None or t < 2:
            out.append(dict(row, dsr=None, sr0_daily=None, sr0_annual=None, variance_sr=None,
                            variance_source="undefined (constant or too short net series)"))
            continue
        if cross:
            variance, source = v_cross, f"cross-cell sample variance of {len(srs)} per-session SRs"
        else:
            variance, source = (1.0 + sr * sr / 2.0) / t, "Lo (2002) sampling variance (1 + SR^2/2)/T: single cell"
        sr0 = expected_max_sr(variance, n)
        out.append(dict(row, dsr=deflated_sharpe(sr, t, m["skew"], m["kurtosis"], sr0), sr0_daily=sr0,
                        sr0_annual=sr0 * math.sqrt(ANNUAL), variance_sr=variance, variance_source=source))
    return out


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


# ------------------------------------------------------------------ listing checks and provenance (T41 M5, M7)
def same_series(a: dict, b: dict) -> bool:
    """Two net series (session_ns -> net) with the same sessions in the same order and equal values (NaN == NaN)."""
    if list(a) != list(b):
        return False
    va = np.fromiter(a.values(), dtype=np.float64, count=len(a))
    vb = np.fromiter(b.values(), dtype=np.float64, count=len(b))
    return bool(np.array_equal(va, vb, equal_nan=True))


def listing_warnings(dirs: list[str], results: list[dict], nets: list[dict], n: int) -> list[str]:
    """Warnings (never refusals) for a listing that silently moves V[SR_n] away from the declared N trials."""
    out = []
    defined = sum(1 for r in results if r["net_moments"]["sr_daily"] is not None)
    if defined != n:
        out.append(f"{defined} listed dir(s) have a defined SR but --dsr-n is {n}: V[SR_n] (hence SR0 and every "
                   "DSR) comes from the listed dirs, not from N")
    for i in range(len(nets)):
        for j in range(i + 1, len(nets)):
            if same_series(nets[i], nets[j]):
                out.append(f"identical daily net series: {dirs[i]} and {dirs[j]} (a duplicate cell shrinks V[SR_n])")
    return out


def git_head(where: Path) -> str | None:
    """``git rev-parse HEAD`` of the checkout holding ``where``; None when git or the repository is unavailable."""
    try:
        done = subprocess.run(["git", "rev-parse", "HEAD"], cwd=where, capture_output=True, text=True, timeout=30,
                              check=False)
    except (OSError, subprocess.SubprocessError):
        return None
    head = done.stdout.strip()
    return head if done.returncode == 0 and re.fullmatch(r"[0-9a-f]{40}(?:[0-9a-f]{24})?", head) else None


def run_provenance(argv: list[str], warnings: list[str]) -> dict:
    script = Path(__file__).resolve()
    return {"argv": list(argv), "script": str(script), "script_sha256": hashlib.sha256(script.read_bytes()).hexdigest(),
            "git_head": git_head(script.parent), "warnings": list(warnings)}


# ------------------------------------------------------------------ validation kit (platform v8 V-1)
def finite_or_none(x):
    return float(x) if x is not None and math.isfinite(x) else None


def year_table(daily: dict) -> list[dict]:
    """Per calendar year (UTC date of session_ns) of the return rows: return rows, compounded net return, net Sharpe
    (mean / sd ddof 1 x sqrt 252), annualized volatility, mean daily GMV turnover over the tau_t sessions of the year
    (``turnover_rows``) and cost per traded dollar in bps over the year's executed sessions."""
    ret, keep = return_mask(daily), turnover_rows(daily)
    executed = daily["executed"] == 1
    years = np.array([BI.session_date(int(s)).year for s in daily["session_ns"]])
    rows = []
    for y in sorted(set(years[ret].tolist())):
        in_y = years == y
        x = daily["net_return"][ret & in_y]
        tau = daily["one_way_turnover_gmv"][keep & in_y]
        traded = float(daily["traded_dollars"][executed & in_y].sum())
        cost = float(daily["trade_cost_dollars"][executed & in_y].sum())
        sd = float(x.std(ddof=1)) if x.size > 1 else 0.0
        rows.append({"year": int(y), "return_rows": int(x.size), "net_return": float(np.prod(1.0 + x) - 1.0),
                     "net_sharpe": float(x.mean()) / sd * math.sqrt(ANNUAL) if sd > 0 else None,
                     "ann_vol": sd * math.sqrt(ANNUAL) if x.size > 1 else None,
                     "tau_gmv_mean": float(tau.mean()) if tau.size else None,
                     "cost_bps_traded": 1e4 * cost / traded if traded > 0 else None})
    return rows


def print_year_table(rows: list[dict], indent: str = "   ") -> None:
    print(f"{indent}year table (return rows, compounded net return, net Sharpe, volatility, tau_gmv mean, cost bps "
          "per traded dollar):")
    for y in rows:
        print(f"{indent}  {y['year']} rows {y['return_rows']:4d} net {fmt(y['net_return'], '+.4f')} sharpe "
              f"{fmt(y['net_sharpe'], '+.3f')} vol {fmt(y['ann_vol'], '.4f')} tau {fmt(y['tau_gmv_mean'], '.4f')} "
              f"cost_bps {fmt(y['cost_bps_traded'], '.2f')}")


def ledger_dsr(moments: dict, records: list[dict], in_ledger: bool, research_window_id: str) -> dict:
    """OD-4 DSR of one dir: N = the ledger's construction trials (+1 when the dir is not in the ledger), V[SR] from the
    construction cells ledgered on ``research_window_id``; the legacy variance's DSR beside it (gates nothing)."""
    v = BI.dsr_variance(records, research_window_id)
    n = v["n"] + (0 if in_ledger else 1)
    row = dict(v, n=n, n_rule="construction trials of the ledger by the defect rule" +
               ("" if in_ledger else " + 1 (this cell, not yet ledgered)"),
               dsr=None, sr0_daily=None, sr0_annual=None, legacy_dsr=None, legacy_sr0_annual=None)
    sr, t = moments["sr_daily"], moments["sessions"]
    if sr is None or moments["skew"] is None or t < 2 or n < 2:
        return row
    for key, variance in (("", v["variance_sr"]), ("legacy_", v["legacy_variance_sr"])):
        if variance is None:
            continue
        sr0 = expected_max_sr(variance, n)
        row[f"{key}sr0_annual"] = sr0 * math.sqrt(ANNUAL)
        row[f"{key}dsr"] = deflated_sharpe(sr, t, moments["skew"], moments["kurtosis"], sr0)
        if not key:
            row["sr0_daily"] = sr0
    return row


def print_ledger_dsr(q: dict) -> None:
    print(f"   ledger DSR (N={q['n']}, {q['window_id']}): DSR {fmt(q['dsr'], '.4f')} vs SR0 "
          f"{fmt(q['sr0_annual'], '.3f')} ann | V[SR] from {q['cells']} cells ledgered on {q['window_id']} "
          f"({fmt(q['variance_sr'], '.3e')}) | beside: legacy variance ({q['legacy_cells']} cells) DSR "
          f"{fmt(q['legacy_dsr'], '.4f')} vs SR0 {fmt(q['legacy_sr0_annual'], '.3f')} ann")


def bundle(base: Path, final: Path, args) -> dict:
    """The cumulative paired test FINAL vs BASE (v8-prereg items 4 and 9): paired dSR with the one-sided studentized
    bootstrap p, a per-year dSR table, both cells' year tables and the verdict dSR > 0 and p_one_sided < alpha."""
    nets, tables, scenarios = {}, {}, {}
    for tag, d in (("base", base), ("final", final)):
        scen = scenario_of(load_summary(d), args.scenario)["scenario"]
        daily = load_daily(d, scen)
        nets[tag], tables[tag], scenarios[tag] = net_series(daily), year_table(daily), scen
    a, b = align(nets["final"], nets["base"])
    paired = paired_stats(a, b, args.draws, args.block, args.seed, one_sided=True)
    common = sorted(set(nets["final"]) & set(nets["base"]))
    years = []
    for y in sorted({BI.session_date(s).year for s in common}):
        sessions = [s for s in common if BI.session_date(s).year == y]
        sr_f = finite_or_none(sharpe(np.array([nets["final"][s] for s in sessions])))
        sr_b = finite_or_none(sharpe(np.array([nets["base"][s] for s in sessions])))
        years.append({"year": y, "sessions": len(sessions), "sr_final": sr_f, "sr_base": sr_b,
                      "dsr": sr_f - sr_b if sr_f is not None and sr_b is not None else None})
    p1 = paired["lw"].get("p_one_sided")
    positive = bool(math.isfinite(paired["dsr"]) and paired["dsr"] > 0)
    verdict = {"dsr_positive": positive, "p_one_sided": p1, "alpha": args.bundle_alpha,
               "pass": bool(positive and p1 is not None and p1 < args.bundle_alpha),
               "rule": "cumulative paired S2 net dSR(FINAL - BASE) > 0 and studentized circular-block-bootstrap "
                       "one-sided p < alpha (v8-prereg item 9: this part of the freeze gate only)"}
    return {"base": str(base), "final": str(final), "scenarios": scenarios, "paired": paired, "years": years,
            "year_table": tables, "verdict": verdict}


def print_bundle(res: dict) -> None:
    p, lw, v = res["paired"], res["paired"]["lw"], res["verdict"]
    print(f"== bundle {res['final']} vs {res['base']} [{res['scenarios']['final']}]: dSR(net) {p['dsr']:+.3f} = SR "
          f"{p['sr']:+.3f} - SR {p['sr_reference']:+.3f} | rho {fmt(p['rho'], '.3f')} | T {p['sessions']} | Memmel SE "
          f"{p['memmel_se']:.3f} | CBB 95% [{p['cbb_ci95'][0]:+.3f}, {p['cbb_ci95'][1]:+.3f}] | LW studentized 95% "
          f"[{fmt(lw['ci95'][0], '+.3f')}, {fmt(lw['ci95'][1], '+.3f')}] p two-sided {fmt(lw['p_value'], '.4f')} "
          f"one-sided {fmt(lw['p_one_sided'], '.4f')} ({p['draws']} draws, block {p['block']}, seed {p['seed']})")
    print(f"   verdict {'PASS' if v['pass'] else 'FAIL'}: dSR > 0 {v['dsr_positive']}; one-sided p "
          f"{fmt(v['p_one_sided'], '.4f')} < {v['alpha']}")
    for y in res["years"]:
        print(f"   {y['year']}: dSR {fmt(y['dsr'], '+.3f')} (SR final {fmt(y['sr_final'], '+.3f')}, SR base "
              f"{fmt(y['sr_base'], '+.3f')}, {y['sessions']} sessions)")


# ------------------------------------------------------------------ driver
def analyse(d: Path, args, weights: list[dict], ref_nets: dict | None) -> tuple[dict, dict]:
    """The dir's result row and its net series (session_ns -> net return over the return rows)."""
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
    nets = net_series(daily)
    out["net_moments"] = net_moments(np.array(list(nets.values()), dtype=np.float64))
    if ref_nets is not None:
        a, b = align(nets, ref_nets)
        out["paired"] = paired_stats(a, b, args.draws, args.block, args.seed)
    if getattr(args, "year_table", False):
        out["year_table"] = year_table(daily)
    return out, nets


def print_analysis(r: dict, reference: str | None) -> None:
    print(f"== {r['dir']} [{r['scenario']}] rule={r['rule']}")
    print(f"   construction: gross_lev {fmt(r['mean_gross_leverage'], '.4f')} net_lev {fmt(r['mean_net_leverage'], '+.4f')} "
          f"|net| {fmt(r['mean_abs_net_leverage'], '.4f')} held_share {fmt(r['held_share'], '.4f')} "
          f"held_names {fmt(r['mean_held_names'], '.1f')} tau_gmv mean {fmt(r['tau_gmv_mean'], '.4f')} "
          f"p95 {fmt(r['tau_gmv_p95'], '.4f')} ({r['tau_gmv_sessions']} sessions) "
          f"cost/GMV-tau {fmt(r['cost_per_gmv_turnover'], '.5f')} cost_bps_traded {fmt(r['cost_bps_traded'], '.2f')}")
    print(f"   leverage over all {r['csv_rows']} CSV rows [R6' mechanics gate]: "
          f"gross_lev_all_rows {fmt(r['mean_gross_leverage_all_rows'], '.4f')} "
          f"net_lev_all_rows {fmt(r['mean_net_leverage_all_rows'], '+.4f')} "
          "(construction gross_lev/net_lev: previous close over return rows)")
    print(f"   leverage post-ramp over the {r['post_ramp_rows']} CSV rows after the first {RAMP_ROWS} "
          f"[v6 C4 L calibration; gate stays all rows]: gross_lev_post_ramp "
          f"{fmt(r['mean_gross_leverage_post_ramp'], '.4f')} net_lev_post_ramp "
          f"{fmt(r['mean_net_leverage_post_ramp'], '+.4f')}")
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
    q = r.get("deflated")
    if q:
        print(f"   deflated SR (N={q['n']}): DSR {fmt(q['dsr'], '.4f')} | SR {fmt(q['sr_daily'], '+.5f')}/session "
              f"({fmt(q['sr_annual'], '+.3f')} ann) vs SR0 {fmt(q['sr0_daily'], '.5f')}/session "
              f"({fmt(q['sr0_annual'], '.3f')} ann) | skew {fmt(q['skew'], '+.3f')} kurtosis {fmt(q['kurtosis'], '.3f')} | "
              f"T {q['sessions']} | V[SR_n] {fmt(q['variance_sr'], '.3e')} ({q['variance_source']})")


def print_integrity(r: dict) -> None:
    """The v7 integrity lines of one dir (only when their options were given; legacy output is untouched)."""
    e = r.get("deflated_effective_n")
    if e:
        q = r.get("deflated") or {}
        lo = r.get("deflated_lo_null") or {}
        print(f"   effective-N DSR (ONC N_eff={e['n_eff']} of {e['n_trials']} trial series): DSR {fmt(e['dsr'], '.4f')} "
              f"vs SR0 {fmt(e['sr0_daily'], '.5f')}/session ({fmt(e['sr0_annual'], '.3f')} ann) | V[SR_k] "
              f"{fmt(e['variance_sr'], '.3e')} | beside: cell-count DSR (N={q.get('n')}) {fmt(q.get('dsr'), '.4f')}, "
              f"Lo null (N={lo.get('n')}) DSR {fmt(lo.get('dsr'), '.4f')} (SR0 {fmt(lo.get('sr0_annual'), '.3f')} ann)")
    p = r.get("psr")
    if p:
        for b in p["benchmarks"]:
            print(f"   PSR vs SR* {b['sr_star_annual']:+.2f} ann: {fmt(b['psr'], '.4f')} | MinTRL at {1 - p['alpha']:.0%}: "
                  f"{fmt(b['min_trl_sessions'], '.0f')} sessions ({fmt(b['min_trl_years'], '.2f')} y) | SR "
                  f"{fmt(p['sr_annual'], '+.3f')} ann, skew {fmt(p['skew'], '+.3f')} kurtosis {fmt(p['kurtosis'], '.3f')}, "
                  f"T {p['sessions']}")


def print_pbo(res: dict, cells: list[str], common: list[int]) -> None:
    q = res["logit_quantiles"]
    how = "exhaustive" if res["exhaustive"] else f"SEEDED SUBSAMPLE (seed {res['seed']})"
    span = (f"{BI.session_date(common[0]).isoformat()}..{BI.session_date(common[-1]).isoformat()}" if common else "none")
    print(f"== CSCV PBO (Bailey-Borwein-Lopez de Prado-Zhu 2017) over {res['n_candidates']} cells: PBO {res['pbo']:.4f} | "
          f"{res['splits']} of {res['splits_total']} splits ({how}) | {res['blocks']} blocks x {res['block_width']} "
          f"sessions of {res['sessions']} common ({span}; {res['dropped_tail_sessions']} trailing dropped)")
    print(f"   logit mean {res['logit_mean']:+.3f} p5 {q['p5']:+.3f} p25 {q['p25']:+.3f} p50 {q['p50']:+.3f} "
          f"p75 {q['p75']:+.3f} p95 {q['p95']:+.3f} | IS winner SR {res['winner_is_sr_annual_mean']:+.3f} ann -> OOS "
          f"{res['winner_oos_sr_annual_mean']:+.3f} ann, P[OOS SR < 0] {res['prob_winner_oos_loss']:.3f}, degradation "
          f"slope {fmt(res['degradation_slope'], '+.3f')}")
    dist = res["logit_distribution"]
    print(f"   logit distribution (IS winner's OOS rank 0 = worst .. {res['n_candidates'] - 1} = best; logit "
          f"{dist['logit'][0]:+.3f} .. {dist['logit'][-1]:+.3f}): splits {dist['splits']}")
    for c, w in zip(cells, res["winner_counts"]):
        print(f"   IS winner {w:6d}x  {c}")


def print_effective(eff: dict, source: str) -> None:
    print(f"== effective N (ONC, Lopez de Prado-Lewis 2019) from {source}: {eff['n_trials']} trial series, "
          f"{eff['sessions']} common sessions, mean off-diagonal rho {eff['mean_offdiag_corr']:.3f} -> N_eff "
          f"{eff['n_eff']} (n_init {eff['n_init']}, seed {eff['seed']}); V[SR_k] {fmt(eff['variance_sr'], '.3e')}")
    for k, (cl, sr) in enumerate(zip(eff["clusters"], eff["cluster_sr_daily"])):
        ann = sr * math.sqrt(ANNUAL) if sr is not None else None
        print(f"   cluster {k}: {len(cl)} series, IVP SR {fmt(ann, '+.3f')} ann: {', '.join(cl)}")


def ledger_fields(args) -> dict:
    """The v8 ledger-line fields the options ask for (empty without them: the line keeps the v7 layout)."""
    out = {}
    if getattr(args, "origin", None):
        out["origin"] = args.origin
    if getattr(args, "protocol", "v7") == "v8":
        out["research_window_id"] = BI.window_id()
    if getattr(args, "rerun_of", None):
        out.update(rerun_of=args.rerun_of, rerun_basis=args.rerun_basis)
    if getattr(args, "ledger_defect", None):
        out["defect"] = args.ledger_defect
    return out


def integrity(args, argv, results, analysed) -> None:
    """--ledger / --psr / --effective-n / --pbo / --ledger-n, after the legacy per-dir analysis (v8: the v8 ledger
    fields and chain, --dsr-ledger and the v8 Appendix A block). Every ledger reader skips protocol lines."""
    nets_by = {d: nets for d, (_, nets) in zip(args.dirs, analysed)}
    v8 = getattr(args, "protocol", "v7") == "v8"
    if args.ledger and args.dirs:
        run = {k: v for k, v in run_provenance(argv, []).items() if k in ("script_sha256", "git_head")}
        recs = []
        for d, r in zip(args.dirs, results):
            daily = Path(d) / f"daily_{r['scenario']}.csv"
            try:
                recs.append(BI.ledger_record(args.ledger_kind, d, Path(d) / "summary.json", daily, r["scenario"],
                                             nets_by[d], r["net_sharpe"], count=args.ledger_count,
                                             note=args.ledger_note, run=run, **ledger_fields(args)))
            except ValueError as exc:
                raise SystemExit(f"nav_summ: {exc}") from exc
        try:
            added, skipped = BI.ledger_append(Path(args.ledger), recs, chain=v8)
        except ValueError as exc:
            raise SystemExit(f"nav_summ: {exc}") from exc
        added_ids = {a["trial_id"] for a in added}
        for rec, r in zip(recs, results):
            r["ledger"] = {"path": args.ledger, "trial_id": rec["trial_id"], "kind": rec["kind"],
                           "appended": rec["trial_id"] in added_ids}
        print(f"== ledger {args.ledger}: appended {len(added)}, skipped {len(skipped)} already ledgered "
              f"(kind {args.ledger_kind})")
    if args.psr:
        for d, r in zip(args.dirs, results):
            r["psr"] = BI.psr_report(np.array(list(nets_by[d].values()), dtype=np.float64), args.psr_benchmarks,
                                     args.psr_alpha)
    eff = None
    if args.effective_n:
        if args.effective_n == "dirs":
            names, series = list(args.dirs), [nets_by[d] for d in args.dirs]
        else:
            records = BI.ledger_read(Path(args.effective_n))
            names, series = BI.ledger_net_series(
                records, lambda rec: net_series(load_daily_csv(Path(rec["series"]["path"]))))
        _, mat = BI.align_many(series)
        eff = BI.effective_trials(mat, names, n_init=args.onc_init, seed=args.onc_seed)
        for r in results:
            r["deflated_effective_n"] = dict(BI.effective_n_dsr(r["net_moments"], eff), clusters=eff["clusters"],
                                             source=args.effective_n)
            r["deflated_lo_null"] = BI.lo_null_dsr(r["net_moments"], args.dsr_n)
    for d, r in zip(args.dirs, results):
        if "deflated_effective_n" in r or "psr" in r:
            print(f"== integrity {d}")
            print_integrity(r)
    if eff is not None:
        print_effective(eff, args.effective_n)
    if args.pbo is not None:
        cells = list(args.pbo) or list(args.dirs)
        if len(cells) < BI.PBO_MIN_CELLS:
            raise SystemExit(f"nav_summ: --pbo needs a grid of >= {BI.PBO_MIN_CELLS} cells, got {len(cells)}")
        series = [nets_by[c] if c in nets_by else
                  net_series(load_daily(Path(c), scenario_of(load_summary(Path(c)), args.scenario)["scenario"]))
                  for c in cells]
        common, mat = BI.align_many(series)
        res = BI.cscv_pbo(mat, blocks=args.pbo_blocks, max_splits=args.pbo_max_splits or None, seed=args.seed)
        print_pbo(res, cells, common)
        if args.pbo_json:
            doc = dict(res, logits=res["logits"].tolist(), cells=cells,
                       common_sessions=[BI.session_date(common[0]).isoformat(), BI.session_date(common[-1]).isoformat()],
                       nav_summ_run=run_provenance(argv, []))
            Path(args.pbo_json).write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if getattr(args, "dsr_ledger", None):
        records = BI.ledger_read(Path(args.dsr_ledger))
        present = {r.get("trial_id") for r in records if r.get("kind") == "construction"}
        wid = BI.window_id()
        for d, r in zip(args.dirs, results):
            series_sha = BI.sha256_file(Path(d) / f"daily_{r['scenario']}.csv")
            r["deflated_ledger"] = ledger_dsr(r["net_moments"], records,
                                              BI.trial_id("construction", series_sha) in present, wid)
            print(f"== ledger DSR {d}")
            print_ledger_dsr(r["deflated_ledger"])
    if args.ledger_n:
        records = BI.ledger_read(Path(args.ledger_n))
        for line in BI.appendix_a(records, args.ledger_n):
            print(line)
        if v8:
            print(BI.appendix_a_v8(records))


def float_list(text: str) -> list[float]:
    return [float(x) for x in text.split(",") if x.strip()]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("dirs", nargs="*", help="NAV output directories (summary.json + daily_<scenario>.csv)")
    ap.add_argument("--weights", action="append", default=[], help="composition_weights.json (or its dir); repeatable")
    ap.add_argument("--reference", default=None, help="reference NAV dir for the paired dSR(net)")
    ap.add_argument("--scenario", default=None, help="scenario name (default: each summary's primary)")
    ap.add_argument("--draws", type=int, default=None, help=f"bootstrap resamples ({DEFAULT_DRAWS}; v8: {V8_DRAWS})")
    ap.add_argument("--block", type=int, default=DEFAULT_BLOCK)
    ap.add_argument("--seed", type=int, default=None, help=f"bootstrap seed ({DEFAULT_SEED}; v8: {V8_SEED})")
    ap.add_argument("--dsr-n", type=int, default=DEFAULT_DSR_N,
                    help="trials N of the deflated Sharpe ratio (R6': 10; >= 2); V[SR_n] comes from the dirs given")
    ap.add_argument("--json", default=None, help="also write the per-dir results as JSON")
    ap.add_argument("--ledger", default=None, help="append the listed dirs to this atx.trial-ledger/v1 JSONL")
    ap.add_argument("--ledger-kind", default="construction", choices=BI.LEDGER_KINDS)
    ap.add_argument("--ledger-count", type=int, default=1, help="trials each appended line stands for (default 1)")
    ap.add_argument("--ledger-note", default=None)
    ap.add_argument("--ledger-n", default=None, help="print the Appendix A trial counts (kind x window) of a ledger")
    ap.add_argument("--effective-n", default=None, help='effective-N DSR from "dirs" or a ledger path')
    ap.add_argument("--onc-init", type=int, default=10, help="ONC k-means restarts (default 10)")
    ap.add_argument("--onc-seed", type=int, default=DEFAULT_SEED)
    ap.add_argument("--pbo", nargs="*", default=None, help="CSCV PBO over these cells (default: the listed dirs)")
    ap.add_argument("--pbo-blocks", type=int, default=BI.PBO_BLOCKS)
    ap.add_argument("--pbo-max-splits", type=int, default=0, help="0 = every split; K > 0 = seeded subsample (--seed)")
    ap.add_argument("--pbo-json", default=None, help="write the PBO result (with every split logit) as JSON")
    ap.add_argument("--psr", action="store_true", help="PSR and MinTRL of every listed dir")
    ap.add_argument("--psr-benchmarks", type=float_list, default=[0.0, 0.5], help="annualized SR* list (0,0.5)")
    ap.add_argument("--psr-alpha", type=float, default=0.05)
    ap.add_argument("--protocol", choices=PROTOCOLS, default="v7",
                    help="v8: pre-registered bootstrap (seed 20260929, 4,999 draws), year tables, origin + window_id + "
                         "hash chain in ledger lines, v8 Appendix A block")
    ap.add_argument("--origin", choices=BI.ORIGINS, default=None, help="origin class of the ledgered cells (K5)")
    ap.add_argument("--rerun-of", default=None, help="the appended line re-runs this trial_id")
    ap.add_argument("--rerun-basis", choices=BI.RERUN_BASES, default=None,
                    help="window: longer window, no new trial; blind / returns: the defect rule (v8-prereg item 7)")
    ap.add_argument("--ledger-defect", default=None, help="REASON: the appended line is an invalid cell (N excludes it)")
    ap.add_argument("--year-table", action="store_true", help="per-year table of every listed dir")
    ap.add_argument("--dsr-ledger", default=None, help="DSR with N and V[SR] from this ledger (OD-4)")
    ap.add_argument("--bundle", nargs=2, default=None, metavar=("BASE", "FINAL"),
                    help="cumulative paired test FINAL vs BASE with its verdict")
    ap.add_argument("--bundle-alpha", type=float, default=BUNDLE_ALPHA)
    ap.add_argument("--bundle-json", default=None, help="write the --bundle result as JSON")
    argv = list(sys.argv[1:] if argv is None else argv)
    args = ap.parse_args(argv)
    v8 = args.protocol == "v8"
    args.draws = (V8_DRAWS if v8 else DEFAULT_DRAWS) if args.draws is None else args.draws
    args.seed = (V8_SEED if v8 else DEFAULT_SEED) if args.seed is None else args.seed
    args.year_table = args.year_table or v8
    if args.dsr_n < 2:
        ap.error("--dsr-n must be >= 2")
    if not args.dirs and not args.ledger_n and not args.pbo and not args.bundle:
        ap.error("give NAV dirs (or --ledger-n LEDGER / --pbo CELL ...)")
    if args.ledger_count < 1 or args.onc_init < 1 or not 0 < args.psr_alpha < 0.5:
        ap.error("--ledger-count and --onc-init must be >= 1; --psr-alpha in (0, 0.5)")
    if (args.ledger or args.psr or args.effective_n == "dirs") and not args.dirs:
        ap.error("--ledger / --psr / --effective-n dirs need NAV dirs")
    if (args.rerun_of is None) != (args.rerun_basis is None):
        ap.error("--rerun-of and --rerun-basis go together")
    if (args.origin or args.rerun_of or args.ledger_defect) and not args.ledger:
        ap.error("--origin / --rerun-of / --ledger-defect need --ledger")
    if v8 and args.ledger and args.dirs and not args.origin:
        ap.error("--protocol v8 records the origin class in every ledger line: give --origin prior|grid|mined (K5)")
    if args.dsr_ledger and not args.dirs:
        ap.error("--dsr-ledger needs NAV dirs")
    if args.bundle_json and not args.bundle:
        ap.error("--bundle-json needs --bundle BASE FINAL")
    if not 0 < args.bundle_alpha < 0.5:
        ap.error("--bundle-alpha in (0, 0.5)")
    weights = load_weights(args.weights)
    ref_nets = None
    if args.reference:
        ref_summary = load_summary(Path(args.reference))
        ref_nets = net_series(load_daily(Path(args.reference), scenario_of(ref_summary, args.scenario)["scenario"]))
    # every dir first: V[SR_n] of the deflated Sharpe ratio spans all the NAV dirs on the command line
    analysed = [analyse(Path(d), args, weights, ref_nets) for d in args.dirs]
    results = [r for r, _ in analysed]
    warnings = []
    if results:
        for r, q in zip(results, dsr_rows([r["net_moments"] for r in results], args.dsr_n)):
            r["deflated"] = q
        warnings = listing_warnings(args.dirs, results, [nets for _, nets in analysed], args.dsr_n)
    for w in warnings:
        print(f"nav_summ: WARNING {w}", file=sys.stderr)
    for d, r in zip(args.dirs, results):
        print_scenarios(load_summary(Path(d)))
        print_analysis(r, args.reference)
        if "year_table" in r:
            print_year_table(r["year_table"])
    integrity(args, argv, results, analysed)
    if args.json:
        run = run_provenance(argv, warnings)
        for r in results:
            r["nav_summ_run"] = run
        Path(args.json).write_text(json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if args.bundle:
        res = bundle(Path(args.bundle[0]), Path(args.bundle[1]), args)
        print_bundle(res)
        if args.bundle_json:
            doc = dict(res, protocol=args.protocol, nav_summ_run=run_provenance(argv, []))
            Path(args.bundle_json).write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
