#!/usr/bin/env python
"""Null false-qualification and power of qualification policy v4, calibrated on the retained price file.

Tier-1 v2 node 1.10 (policy ``r4-qualification-v4``, content sha256 776db445df6323c5...). The study answers:
under the frozen v4 gates, how often does a signal with no predictive power qualify (per evidence class),
and how strong must a true signal be (ICIR of its 3-month rank IC) to pass the selection gate, the old
v3 holdout gate and the new v4 holdout rule, with the sample this project actually has?

Real data (read-only, no warehouse)
    * ``TickerHistory3.parquet`` (the retained vendor price file), **bars dated <= 2023-12-31 only**:
      policy v4's holdout starts 2024-01 and stays sealed, so no holdout-period price is loaded
      (the scan filters ``tradingDate <= 2023-12-31`` and every requested exit is asserted < 2024-01-01).
    * The Fama-French NYSE ME breakpoints (U6, ``bench_french_me_breakpoints.parquet``) for the
      investable slice (price >= $5, ME >= NYSE 20th percentile) and the small / large buckets (NYSE 20/50).
    * Sessions: the rule-based XNYS calendar (``atx_db.calendar.xnys_sessions``) intersected with the bar
      dates; stray bars on closures are dropped and listed.
    Month-end formations with a 3-month label (entry at the close of the session after the month-end,
    exit at the close of the session after the third month-end, adjusted close = close x
    cumulReturnFactor) ending before 2024-01-01 form the selection sample (the policy's purge rule).
    Universe proxy (labeled): lines with an earnings event in the vendor's prior 504 sessions
    (``nEarnCnt_504d > 0``: operating companies, no ETFs/funds; on a formation session where the vendor
    published no earnings counters -- 2012-06-29 -- the latest earlier session that has them), a positive
    close and share count and a valid 3-month return; ME = close x vendor shares (thousands) / 1000 in $M
    (vendor shares are current, A8/A9 caveats: only the slice membership of this study uses them).
    Duplicate vendor keys (date, securityID) are dropped, never picked.

Null and planted features (the Gaussian-copula model)
    A null feature is a latent AR(1) Gaussian per line (monthly persistence rho; its rank score has
    Spearman autocorrelation (6/pi) asin(rho^k / 2) at lag k) independent of returns. Its per-formation
    rank IC in slice A (all / investable / small / large) is IC_A,t = sum_i g_i,t u^A_i,t / n_A,t (u = the
    standardized rank score of the real 3-month return within A) and its EW decile long-short is
    LS_t = sum_i D(g_i,t) r_i,t / (0.1 n_t) (r = the real 3-month return, 0.5/99.5% winsorized within the
    formation, demeaned). Conditional on the real returns the vector (IC_all, IC_inv, IC_small, IC_large,
    LS) over every selection formation is mean zero with covariance
        Cov(X_a,t, X_b,s) = k_ab(rho^|t-s|) * sum_i w^a_i,t w^b_i,s        (Schur product of two PSD matrices)
    with k = the rank-score / decile-indicator cross-moments of a bivariate normal (computed by
    quadrature) and w the real per-line weights: every cross-sectional and serial dependence of the real
    return panel (overlapping 3-month windows, names entering and leaving, dispersion that varies by month)
    enters through sum_i w w. Draws are exact Gaussian draws from that covariance (Cholesky); a direct
    simulation (latent features ranked against the real returns with the evaluation's own grouped rank
    correlation) checks the model. A planted signal shifts every IC slice by mu = ICIR x sd(IC_all) and
    its long-short by the linear-projection amount mu * E[s D(s)] * sum_i u_i r_i / (0.1 n); ICIR_3m =
    mean / sd of the monthly-sampled 3-month rank IC. The holdout (T = 30) is drawn independently from the
    covariance of the last 30 selection formations (a stand-in window; the sealed holdout is never read).

Grading (the production code)
    Per run, 200 features (``--planted`` of them true) get the EWC fixed-b statistics of
    ``stats.mean_inference`` (vectorized here; checked equal on a sample) and are graded by
    ``qualification.grade_v4`` under the frozen policy (hash checked): replication = pre-registered +1 sign,
    discovery = two-sided; DSR ``n_trials`` = ``--n-trials`` (default 4,800 = the 200 features x 6
    variants x 4 horizons a wave would register), sharpe variance = the run's cross-feature variance;
    ``grade_holdout=True`` with the stand-in holdout. The v3 comparison paths: HLZ |z| >= 3 alone,
    v3 significance (HLZ, or two-sided BH q <= 0.05 over the 200 and |z| >= 2) and the v3 holdout gate
    (holdout z >= 1.5 in the fixed direction).

Run (OPENBLAS_NUM_THREADS=1; DuckDB 256MB / 1 thread, one extraction query per calendar year; peak ~0.35 GiB):
    python scripts/research_power_study.py [--runs 400] [--features 200] [--planted 20] [--seed 20260926]
"""

from __future__ import annotations

import argparse
import datetime as dt
import itertools
import json
import math
import os
import sys
import time
from collections.abc import Mapping, Sequence
from pathlib import Path
from statistics import NormalDist
from typing import Any

os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")

import duckdb
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from atx_db.calendar import xnys_sessions
from atx_db.research import evaluation as ev
from atx_db.research import qualification as rq
from atx_db.research import stats

ROOT = Path(__file__).resolve().parents[1]
PRICE_FILE = ROOT / "data" / "staging" / "broad-bars" / "2026-09-20-updated" / "TickerHistory3.parquet"
BREAKPOINTS_FILE = ROOT / "data" / "raw" / "benchmarks" / "parsed" / "bench_french_me_breakpoints.parquet"
POLICY_SHA256 = "776db445df6323c5d0dfd7db8e080631d94665c6a84e548f4b1d01b148546d9a"
LAST_READABLE_BAR = dt.date(2023, 12, 31)  # policy v4 holdout_start 2024-01: sealed
HOLDOUT_DATA_END = dt.date(2026, 9, 18)    # last session of the snapshot (holdout formations counted, not read)
H = 3
COMPONENTS = ("ic_all", "ic_inv", "ic_small", "ic_large", "ls")
ICIR_GRID = (0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 1.0, 1.2)
DECILE_A = NormalDist().inv_cdf(0.9)
_NORMAL = NormalDist()


# ---------------------------------------------------------------------------
# Real-data extraction (selection window only)
# ---------------------------------------------------------------------------

def _connect() -> duckdb.DuckDBPyConnection:
    temp = ROOT / "data" / "tmp" / "duckdb"
    temp.mkdir(parents=True, exist_ok=True)
    return duckdb.connect(config={"memory_limit": "256MB", "threads": 1, "preserve_insertion_order": False,
                                  "temp_directory": str(temp), "max_temp_directory_size": "4GB"})


_CHUNK_SQL = """
    WITH d(m, f, u, e, x) AS (VALUES {values}),
    needed AS (SELECT f AS day FROM d UNION SELECT u FROM d UNION SELECT e FROM d UNION SELECT x FROM d),
    bars AS (
        SELECT tradingDate AS day, securityID AS sid, close, shares, nEarnCnt_504d AS e504,
               close * cumulReturnFactor AS adj
        FROM read_parquet(?) WHERE tradingDate <= DATE '{last}' AND tradingDate IN (SELECT day FROM needed)),
    dup AS (SELECT day, sid FROM bars GROUP BY day, sid HAVING count(*) > 1),
    clean AS (SELECT * FROM bars ANTI JOIN dup USING (day, sid)),
    base AS (
        SELECT d.m, b0.sid, b0.close AS price, b0.close * b0.shares / 1000.0 AS me, be.adj AS adj_entry,
               bx.adj AS adj_exit
        FROM d JOIN clean b0 ON b0.day = d.f
        JOIN clean bu ON bu.day = d.u AND bu.sid = b0.sid AND bu.e504 > 0
        JOIN clean be ON be.day = d.e AND be.sid = b0.sid
        LEFT JOIN clean bx ON bx.day = d.x AND bx.sid = b0.sid
        WHERE b0.close > 0 AND b0.shares > 0)
    SELECT m, sid, price, me,
           CASE WHEN adj_exit > 0 AND adj_entry > 0 THEN adj_exit / adj_entry - 1.0
                ELSE CAST('NaN' AS DOUBLE) END AS ret
    FROM base ORDER BY m, sid"""
_DUP_SQL = """
    SELECT tradingDate, count(*) FROM (
        SELECT tradingDate, securityID FROM read_parquet(?)
        WHERE tradingDate <= DATE '{last}' AND tradingDate IN (SELECT unnest(?::DATE[]))
        GROUP BY 1, 2 HAVING count(*) > 1) GROUP BY 1"""


def month_end_formations(sessions: Sequence[dt.date], last_exit: dt.date) -> list[tuple[dt.date, dt.date, dt.date]]:
    """(formation = last session of a month, entry = next session, exit = session after the 3rd month-end)."""
    by_month: dict[tuple[int, int], dt.date] = {}
    for day in sessions:
        by_month[(day.year, day.month)] = max(day, by_month.get((day.year, day.month), day))
    index = {day: i for i, day in enumerate(sessions)}
    months = sorted(by_month)
    out = []
    for i, key in enumerate(months):
        if i + H >= len(months):
            break
        formation, horizon_end = by_month[key], by_month[months[i + H]]
        if index[horizon_end] + 1 >= len(sessions) or index[formation] + 1 >= len(sessions):
            break
        entry, exit_day = sessions[index[formation] + 1], sessions[index[horizon_end] + 1]
        if exit_day > last_exit:
            break
        out.append((formation, entry, exit_day))
    return out


def load_selection_panel(price_file: Path, breakpoints_file: Path) -> dict[str, Any]:
    """Per selection formation: line ids, price, ME ($M), 3-month return; plus calendar facts."""
    con = _connect()
    try:
        bar_days = [row[0] for row in con.execute(
            "SELECT DISTINCT tradingDate FROM read_parquet(?) WHERE tradingDate <= ? ORDER BY 1",
            [str(price_file), LAST_READABLE_BAR]).fetchall()]
        calendar = set(xnys_sessions(bar_days[0], LAST_READABLE_BAR))
        sessions = sorted(calendar & set(bar_days))
        extra = sorted(set(bar_days) - calendar)
        missing = sorted(d for d in calendar - set(bar_days) if d >= bar_days[0])
        formations = month_end_formations(sessions, LAST_READABLE_BAR)
        assert formations and max(x for _, _, x in formations) < dt.date(2024, 1, 1)
        holdout_sessions = xnys_sessions(dt.date(2023, 10, 1), HOLDOUT_DATA_END)  # calendar only, no data read
        holdout = [f for f in month_end_formations(holdout_sessions, HOLDOUT_DATA_END) if f[0] >= dt.date(2024, 1, 1)]
        # Universe day: the formation session, else the latest earlier session on which the vendor published
        # its earnings counters (a vendor gap in its derived columns; point in time either way).
        covered = dict(con.execute(
            "SELECT tradingDate, count(*) FILTER (WHERE nEarnCnt_504d > 0) FROM read_parquet(?) "
            "WHERE tradingDate <= ? GROUP BY 1", [str(price_file), LAST_READABLE_BAR]).fetchall())
        position = {day: i for i, day in enumerate(sessions)}

        def universe_session(day: dt.date) -> dt.date:
            i = position[day]
            while i >= 0 and not covered.get(sessions[i], 0):
                i -= 1
            if i < 0 or position[day] - i > 42:
                raise SystemExit(f"no vendor earnings counters within 42 sessions before {day}")
            return sessions[i]

        universe_day = {f: universe_session(f) for f, _, _ in formations}
        gaps = sorted((f, universe_day[f]) for f in universe_day if universe_day[f] != f)
        # M8: one bounded query per formation year (a whole-sample 4-way self-join overran 256MB).
        parts: list[dict[str, np.ndarray]] = []
        dup_by_day: dict[dt.date, int] = {}
        for year in sorted({f.year for f, _, _ in formations}):
            chunk = [(m, f, e, x) for m, (f, e, x) in enumerate(formations) if f.year == year]
            values = ", ".join(f"({m}, DATE '{f}', DATE '{universe_day[f]}', DATE '{e}', DATE '{x}')"
                               for m, f, e, x in chunk)
            parts.append(con.execute(_CHUNK_SQL.format(values=values, last=LAST_READABLE_BAR),
                                     [str(price_file)]).fetchnumpy())
            days = sorted({d for _, f, e, x in chunk for d in (f, universe_day[f], e, x)})
            dup_by_day.update(dict(con.execute(_DUP_SQL.format(last=LAST_READABLE_BAR),
                                               [str(price_file), days]).fetchall()))
        rows = {key: np.concatenate([np.asarray(part[key]) for part in parts]) for key in parts[0]}
        breaks = con.execute("SELECT yyyymm, me_p20_musd, me_p50_musd FROM read_parquet(?)",
                             [str(breakpoints_file)]).fetchall()
    finally:
        con.close()
    bp = {int(y): (float(p20), float(p50)) for y, p20, p50 in breaks}
    return {"formations": formations, "holdout_formations": holdout, "extra_bar_days": extra,
            "missing_sessions": missing, "sessions": len(sessions), "rows": rows, "breakpoints": bp,
            "universe_day_gaps": gaps, "dup_keys": int(sum(dup_by_day.values()))}


def rank_scores(values: np.ndarray) -> np.ndarray:
    """Standardized average-rank scores (mean 0, population variance 1)."""
    ranks = ev.grouped_average_ranks(np.zeros(len(values), np.int64), values, 1)
    centered = ranks - ranks.mean()
    return centered / math.sqrt(float(np.mean(centered * centered)))


def build_weights(panel: Mapping[str, Any]) -> dict[str, Any]:
    """Per-line weights W (lines x components*T) of every component, slice counts and planted-LS moments."""
    rows = panel["rows"]
    formations = panel["formations"]
    months = np.asarray(rows["m"], dtype=np.int64)
    sid = np.asarray(rows["sid"], dtype=np.int64)
    price = np.asarray(rows["price"], dtype=float)
    me = np.asarray(rows["me"], dtype=float)
    ret = np.asarray(rows["ret"], dtype=float)
    lines, line_index = np.unique(sid, return_inverse=True)
    t_count = len(formations)
    weights = np.zeros((len(lines), len(COMPONENTS) * t_count))
    counts = {name: np.zeros(t_count, np.int64) for name in ("all", "inv", "small", "large", "terminal", "no_bp")}
    planted_ls = np.zeros(t_count)
    starts = np.searchsorted(months, np.arange(t_count + 1))
    for t, (formation, _, _) in enumerate(formations):
        lo, hi = starts[t], starts[t + 1]
        r, p, cap, idx = ret[lo:hi], price[lo:hi], me[lo:hi], line_index[lo:hi]
        valid = np.isfinite(r)
        counts["terminal"][t] = int((~valid).sum())
        r, p, cap, idx = r[valid], p[valid], cap[valid], idx[valid]
        p20, p50 = panel["breakpoints"].get(formation.year * 100 + formation.month, (math.nan, math.nan))
        if not math.isfinite(p20):
            counts["no_bp"][t] = 1
        slices = {"all": np.ones(len(r), bool), "inv": (p >= 5.0) & (cap >= p20),
                  "small": (cap >= p20) & (cap < p50), "large": cap >= p50}
        for c, name in enumerate(("all", "inv", "small", "large")):
            chosen = slices[name]
            n = int(chosen.sum())
            counts[name][t] = n
            if n >= 3:
                weights[idx[chosen], c * t_count + t] = rank_scores(r[chosen]) / n
        low, high = np.percentile(r, [0.5, 99.5])
        clipped = np.clip(r, low, high)
        centered = clipped - clipped.mean()
        n_all = len(r)
        weights[idx, 4 * t_count + t] = centered / (0.1 * n_all)
        planted_ls[t] = float(np.dot(rank_scores(r), centered)) / (0.1 * n_all)
    return {"W": weights, "counts": counts, "planted_ls": planted_ls, "lines": len(lines), "T": t_count}


# ---------------------------------------------------------------------------
# Gaussian-copula kernels and covariance
# ---------------------------------------------------------------------------

_GRID = np.linspace(DECILE_A, 9.0, 12001)
_PHI_GRID = np.exp(-0.5 * _GRID * _GRID) / math.sqrt(2.0 * math.pi)
_erf = np.frompyfunc(math.erf, 1, 1)


def _cdf(x: np.ndarray) -> np.ndarray:
    return 0.5 * (1.0 + _erf(np.asarray(x, dtype=float) / math.sqrt(2.0)).astype(float))


def kernel(r: float) -> tuple[float, float, float]:
    """(kappa, lambda, c) at latent correlation r: Corr of rank scores, E[s(X) D(Y)], E[D(X) D(Y)]."""
    kappa = 6.0 / math.pi * math.asin(r / 2.0)
    shift = r * _GRID / math.sqrt(2.0 - r * r)
    lam = 2.0 * math.sqrt(3.0) * float(np.trapezoid((2.0 * _cdf(shift) - 1.0) * _PHI_GRID, _GRID))
    if r >= 1.0 - 1e-12:
        c = 0.2
    else:
        s = math.sqrt(1.0 - r * r)
        c = 2.0 * float(np.trapezoid((_cdf((r * _GRID - DECILE_A) / s) - _cdf((-DECILE_A - r * _GRID) / s))
                                     * _PHI_GRID, _GRID))
    return kappa, lam, c


def covariance(weights: np.ndarray, t_count: int, rho: float) -> np.ndarray:
    """Sigma = K(rho) o (W' W) over the components x formations (see the module docstring)."""
    gram = weights.T @ weights
    lags = np.abs(np.subtract.outer(np.arange(t_count), np.arange(t_count)))
    table = np.array([kernel(rho ** k if rho > 0 else float(k == 0)) for k in range(t_count)])
    kappa, lam, c = (table[:, j][lags] for j in range(3))
    blocks = []
    for a in range(len(COMPONENTS)):
        row = []
        for b in range(len(COMPONENTS)):
            if a < 4 and b < 4:
                k = kappa
            elif a == 4 and b == 4:
                k = c
            else:
                k = lam
            row.append(k)
        blocks.append(row)
    return np.block(blocks) * gram


def restrict(sigma: np.ndarray, t_count: int, keep: np.ndarray, components: Sequence[int]) -> np.ndarray:
    index = np.concatenate([c * t_count + keep for c in components])
    return sigma[np.ix_(index, index)]


def cholesky(sigma: np.ndarray) -> np.ndarray:
    values, vectors = np.linalg.eigh((sigma + sigma.T) / 2.0)
    floor = max(float(values.max()) * 1e-12, 0.0)
    return vectors * np.sqrt(np.clip(values, floor, None))


# ---------------------------------------------------------------------------
# Vectorized EWC (== stats.mean_inference on complete series) and p tables
# ---------------------------------------------------------------------------

class PTable:
    """Two-sided Student-t p on a fine |t| grid (log-linear interpolation of stats.student_t_two_sided_p)."""

    def __init__(self, df: int) -> None:
        self.df = df
        self.grid = np.linspace(0.0, 60.0, 60001)
        self.logp = np.log(np.maximum([stats.student_t_two_sided_p(float(t), df) for t in self.grid], 1e-300))

    def __call__(self, t: np.ndarray) -> np.ndarray:
        return np.exp(np.interp(np.abs(t), self.grid, self.logp))


_TABLES: dict[int, PTable] = {}


def ewc(x: np.ndarray, h: int) -> tuple[np.ndarray, np.ndarray, int, np.ndarray]:
    """(mean, robust t, df, two-sided p) of each row of a complete series matrix (features x T)."""
    t_count = x.shape[1]
    df = stats.ewc_degrees_of_freedom(t_count, h)
    positions = np.arange(1, t_count + 1, dtype=float) - 0.5
    basis = math.sqrt(2.0 / t_count) * np.cos(np.pi * np.outer(positions, np.arange(1, df + 1)) / t_count)
    mean = x.mean(axis=1)
    loadings = (x - mean[:, None]) @ basis
    se = np.sqrt(t_count * np.mean(loadings * loadings, axis=1)) / t_count
    tstat = mean / se
    if df not in _TABLES:
        _TABLES[df] = PTable(df)
    return mean, tstat, df, _TABLES[df](tstat)


def sharpe_moments(x: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    mean = x.mean(axis=1)
    centered = x - mean[:, None]
    second = np.mean(centered ** 2, axis=1)
    sharpe = mean / x.std(axis=1, ddof=1)
    return sharpe, np.mean(centered ** 3, axis=1) / second ** 1.5, np.mean(centered ** 4, axis=1) / second ** 2


# ---------------------------------------------------------------------------
# One scenario
# ---------------------------------------------------------------------------

def wilson(k: int, n: int) -> tuple[float, float]:
    if n == 0:
        return math.nan, math.nan
    z = 1.959963984540054
    p = k / n
    centre = (p + z * z / (2 * n)) / (1 + z * z / n)
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / (1 + z * z / n)
    return centre - half, centre + half


def run_scenario(chol: np.ndarray, chol_hold: np.ndarray, t_count: int, t_hold: int, *, policy: Any,
                 icir: float, sigma_ic: float, planted_ls: np.ndarray, runs: int, features: int, planted: int,
                 n_trials: int, seed: int, classes: Sequence[str] = ("replication", "discovery"),
                 stress: float = 0.0) -> dict[str, Any]:
    rng = np.random.default_rng(seed)
    mu = icir * sigma_ic
    _, lam1, _ = kernel(1.0)
    counts: dict[str, dict[str, int]] = {}
    realized_icir, annual_sharpe = [], []

    def bump(group: str, name: str, flag: np.ndarray) -> None:
        counts.setdefault(group, {}).setdefault(name, 0)
        counts[group][name] += int(np.count_nonzero(flag))

    fwer: dict[str, int] = {}
    stress_scale = None
    for _ in range(runs):
        draw = (chol @ rng.standard_normal((chol.shape[1], features))).T  # features x 5T
        parts = {name: draw[:, c * t_count:(c + 1) * t_count] for c, name in enumerate(COMPONENTS)}
        if stress > 0:
            if stress_scale is None:
                stress_scale = {name: float(np.std(parts[name])) for name in COMPONENTS}
            eta = np.empty((features, t_count))
            eta[:, 0] = rng.standard_normal(features)
            for t in range(1, t_count):
                eta[:, t] = 0.5 * eta[:, t - 1] + math.sqrt(0.75) * rng.standard_normal(features)
            for name in COMPONENTS:
                parts[name] = parts[name] + math.sqrt(stress) * stress_scale[name] * eta
        hold = (chol_hold @ rng.standard_normal((chol_hold.shape[1], features))).T
        if planted:
            for name in COMPONENTS[:4]:
                parts[name][:planted] += mu
            parts["ls"][:planted] += mu * lam1 * planted_ls[-t_count:]
            hold[:planted] += mu
            ic_true = parts["ic_all"][:planted]
            realized_icir.extend((ic_true.mean(axis=1) / ic_true.std(axis=1, ddof=1)).tolist())
            ls_true = parts["ls"][:planted]
            annual_sharpe.extend((ls_true.mean(axis=1) / ls_true.std(axis=1, ddof=1) * math.sqrt(12 / H)).tolist())
        m_all, t_all, df_all, p_all = ewc(parts["ic_all"], H)
        m_inv, t_inv, _, p_inv = ewc(parts["ic_inv"], H)
        m_hold, t_hold_stat, df_hold, p_hold = ewc(hold, H)
        small_mean, large_mean = parts["ic_small"].mean(axis=1), parts["ic_large"].mean(axis=1)
        sharpe, skew, kurt = sharpe_moments(parts["ls"])
        variance = float(np.var(sharpe, ddof=1))
        is_true = np.arange(features) < planted
        z_all = np.array([math.copysign(stats.normal_equivalent_z(float(p)), float(t))
                          for p, t in zip(p_all, t_all, strict=True)])
        # v3 comparison paths (two-sided, sign-free significance; the v3 holdout gate in the fixed direction)
        bh = stats.benjamini_hochberg({i: float(p) for i, p in enumerate(p_all)})
        bh_q = np.array([bh[i] for i in range(features)])
        v3_sig = (np.abs(z_all) >= 3.0) | ((bh_q <= 0.05) & (np.abs(z_all) >= 2.0))
        z_hold = np.array([math.copysign(stats.normal_equivalent_z(float(p)), float(t))
                           for p, t in zip(p_hold, t_hold_stat, strict=True)])
        one_sided = np.array([rq.one_sided_p(float(p), float(t), 1) for p, t in zip(p_all, t_all, strict=True)])
        for group, mask in (("true", is_true), ("null", ~is_true)):
            bump(group, "hlz", (np.abs(z_all) >= 3.0) & mask)
            bump(group, "z2", (np.abs(z_all) >= 2.0) & mask)
            bump(group, "v3_significance", v3_sig & mask)
            bump(group, "v3_holdout_gate", (z_hold >= 1.5) & mask)  # direction +1 (planted sign)
            bump(group, "one_sided_p05", (one_sided <= 0.05) & mask)
            bump(group, "features", mask)
        for evidence_class in classes:
            rows = [rq.FeatureEvidence(
                feature_id=f"f{i:03d}", basis="reconstructed", evidence_class=evidence_class,
                expected_sign=1 if evidence_class == "replication" else 0, ic_mean=float(m_all[i]),
                ic_t=float(t_all[i]), ic_df=float(df_all), ic_p=float(p_all[i]), ic_n=t_count, coverage_share=1.0,
                coverage_formations=t_count, investable_mean=float(m_inv[i]), investable_t=float(t_inv[i]),
                investable_p=float(p_inv[i]), investable_formations=t_count, small_mean=float(small_mean[i]),
                small_formations=t_count, large_mean=float(large_mean[i]), large_formations=t_count,
                size_venue_basis="nyse_breakpoints", sharpe=float(sharpe[i]), sharpe_skew=float(skew[i]),
                sharpe_kurt=float(kurt[i]), sharpe_n=t_count, sharpe_variance=variance,
                holdout_mean=float(m_hold[i]), holdout_t=float(t_hold_stat[i]), holdout_df=float(df_hold),
                holdout_formations=t_hold) for i in range(features)]
            graded = rq.grade_v4(rows, policy, n_trials=n_trials, prior_gating_hypotheses=0, grade_holdout=True)
            selection = np.array([bool(r["gate_coverage"] and r["gate_sign"] and r["gate_significance"]
                                       and r["gate_investable"]
                                       and (evidence_class != "replication" or r["gate_size_buckets"]))
                                  for r in graded])
            final = np.array([r["status"] in rq.V4_PASSING for r in graded])
            significance = np.array([bool(r["gate_significance"]) for r in graded])
            dsr_pass = np.array([r["dsr"] is not None and r["dsr"] >= policy.discovery_dsr_min for r in graded])
            for group, mask in (("true", is_true), ("null", ~is_true)):
                bump(group, f"{evidence_class}_significance", significance & mask)
                bump(group, f"{evidence_class}_selection", selection & mask)
                bump(group, f"{evidence_class}_final", final & mask)
                bump(group, f"{evidence_class}_holdout_given_selection", final & selection & mask)
                bump(group, f"{evidence_class}_dsr", dsr_pass & mask)
            fwer[f"{evidence_class}_selection"] = fwer.get(f"{evidence_class}_selection", 0) + int(
                bool((selection & ~is_true).any()))
            fwer[f"{evidence_class}_final"] = fwer.get(f"{evidence_class}_final", 0) + int(bool((final & ~is_true).any()))
    return {"icir": icir, "mu": mu, "runs": runs, "features": features, "planted": planted, "counts": counts,
            "fwer": fwer, "realized_icir": float(np.mean(realized_icir)) if realized_icir else None,
            "annual_ls_sharpe": float(np.mean(annual_sharpe)) if annual_sharpe else None}


def rate(result: Mapping[str, Any], group: str, name: str, denominator: str = "features") -> float:
    counts = result["counts"].get(group, {})
    total = counts.get(denominator, 0)
    return counts.get(name, 0) / total if total else math.nan


def crossing(grid: Sequence[float], values: Sequence[float], level: float) -> float | None:
    for (x0, y0), (x1, y1) in itertools.pairwise(zip(grid, values, strict=True)):
        if y0 < level <= y1:
            return x0 + (level - y0) * (x1 - x0) / (y1 - y0)
    return grid[0] if values and values[0] >= level else None


# ---------------------------------------------------------------------------
# Direct-simulation check of the covariance model
# ---------------------------------------------------------------------------

def direct_check(panel: Mapping[str, Any], weights: Mapping[str, Any], sigma: np.ndarray, rho: float,
                 features: int, seed: int) -> dict[str, float]:
    """Latent AR(1) features ranked against the real returns (grouped rank correlation, deciles)."""
    rows = panel["rows"]
    months = np.asarray(rows["m"], dtype=np.int64)
    sid = np.asarray(rows["sid"], dtype=np.int64)
    ret = np.asarray(rows["ret"], dtype=float)
    lines, line_index = np.unique(sid, return_inverse=True)
    t_count = weights["T"]
    rng = np.random.default_rng(seed)
    starts = np.searchsorted(months, np.arange(t_count + 1))
    ic = np.full((features, t_count), np.nan)
    ls = np.full((features, t_count), np.nan)
    state = rng.standard_normal((len(lines), features))
    for t in range(t_count):
        if t:
            state = rho * state + math.sqrt(1.0 - rho * rho) * rng.standard_normal(state.shape)
        lo, hi = starts[t], starts[t + 1]
        r, idx = ret[lo:hi], line_index[lo:hi]
        valid = np.isfinite(r)
        r, idx = r[valid], idx[valid]
        n = len(r)
        low, high = np.percentile(r, [0.5, 99.5])
        clipped = np.clip(r, low, high)
        g = state[idx]
        group = np.repeat(np.arange(features), n)
        rho_ic, _ = ev.grouped_rank_correlation(group, g.T.ravel(), np.tile(r, features), features)
        ic[:, t] = rho_ic
        order = np.argsort(g, axis=0)
        k = round(0.1 * n)
        top = clipped[order[-k:]].mean(axis=0)
        bottom = clipped[order[:k]].mean(axis=0)
        ls[:, t] = top - bottom
    diag = np.diag(sigma)
    out = {"var_ic_direct": float(np.mean(np.var(ic, axis=1))), "var_ic_model": float(np.mean(diag[:t_count])),
           "var_ls_direct": float(np.mean(np.var(ls, axis=1))),
           "var_ls_model": float(np.mean(diag[4 * t_count:5 * t_count]))}
    centered = ic - ic.mean(axis=1, keepdims=True)
    for lag in (1, 2, 3, 6):
        direct = float(np.mean(centered[:, lag:] * centered[:, :-lag]))
        model = float(np.mean([sigma[t, t + lag] for t in range(t_count - lag)]))
        out[f"acov{lag}_ic_direct"], out[f"acov{lag}_ic_model"] = direct, model
    cross = np.mean([np.mean((ic[:, t] - ic[:, t].mean()) * (ls[:, t] - ls[:, t].mean())) for t in range(t_count)])
    out["cov_ic_ls_direct"] = float(cross)
    out["cov_ic_ls_model"] = float(np.mean([sigma[t, 4 * t_count + t] for t in range(t_count)]))
    return out


def peak_memory_mib() -> str:
    """Peak working set / peak private bytes of this process (Windows), else ru_maxrss."""
    try:
        import ctypes
        from ctypes import wintypes

        class Counters(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

        counters = Counters()
        counters.cb = ctypes.sizeof(Counters)
        kernel32 = ctypes.windll.kernel32
        kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        kernel32.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
        kernel32.K32GetProcessMemoryInfo.restype = wintypes.BOOL
        if not kernel32.K32GetProcessMemoryInfo(kernel32.GetCurrentProcess(), ctypes.byref(counters), counters.cb):
            return "peak memory unavailable"
        return (f"peak working set {counters.PeakWorkingSetSize / 2**20:.0f} MiB, "
                f"peak private {counters.PeakPagefileUsage / 2**20:.0f} MiB")
    except (AttributeError, OSError):
        import resource

        return f"max rss {resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024:.0f} MiB"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--price-file", type=Path, default=PRICE_FILE)
    parser.add_argument("--breakpoints", type=Path, default=BREAKPOINTS_FILE)
    parser.add_argument("--runs", type=int, default=400, help="runs per main scenario (>= 400)")
    parser.add_argument("--sensitivity-runs", type=int, default=200)
    parser.add_argument("--features", type=int, default=200)
    parser.add_argument("--planted", type=int, default=20, help="true signals among the features of a planted run")
    parser.add_argument("--t-selection", type=int, default=130)
    parser.add_argument("--t-holdout", type=int, default=30)
    parser.add_argument("--rho", type=float, default=0.9, help="main latent monthly persistence")
    parser.add_argument("--null-rhos", default="0.0,0.9,0.98")
    parser.add_argument("--n-trials", type=int, default=4800)
    parser.add_argument("--direct-features", type=int, default=200)
    parser.add_argument("--seed", type=int, default=20260926)
    parser.add_argument("--quick", action="store_true", help="skip the sensitivity scenarios")
    parser.add_argument("--json", type=Path, default=None, help="write every scenario result here")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    started = time.time()
    policy = rq.load_policy_v4()
    if policy.sha256 != POLICY_SHA256:
        raise SystemExit(f"policy v4 sha256 {policy.sha256} is not the frozen {POLICY_SHA256}")
    print(f"policy {policy.version} sha256 {policy.sha256} (pinned {rq.FROZEN_POLICY_SHA256[policy.version]})")
    print(f"first forward return read at {dt.datetime.now(dt.UTC).replace(tzinfo=None).isoformat(timespec='seconds')}Z")
    panel = load_selection_panel(args.price_file, args.breakpoints)
    formations = panel["formations"]
    weights = build_weights(panel)
    t_all = weights["T"]
    counts = weights["counts"]
    years = t_all / 12.0
    print(f"\n## Real sample (selection window, bars <= {LAST_READABLE_BAR})\n")
    print(f"- XNYS sessions with bars: {panel['sessions']}; stray bar dates on closures dropped: "
          f"{[d.isoformat() for d in panel['extra_bar_days']]}; sessions without bars: "
          f"{[d.isoformat() for d in panel['missing_sessions']]}; duplicate vendor keys dropped: {panel['dup_keys']}; "
          f"formations whose universe flag comes from an earlier session (vendor published no earnings counters): "
          f"{[(f.isoformat(), u.isoformat()) for f, u in panel['universe_day_gaps']]}")
    print(f"- selection formations (3-month label ends before 2024-01-01): {t_all} "
          f"({formations[0][0]} .. {formations[-1][0]}, last exit {formations[-1][2]}) = {years:.2f} years")
    print(f"- holdout formations 2024-01 .. with a 3-month label by {HOLDOUT_DATA_END} (calendar count, no data read): "
          f"{len(panel['holdout_formations'])} ({panel['holdout_formations'][0][0]} .. "
          f"{panel['holdout_formations'][-1][0]})")
    for name in ("all", "inv", "small", "large", "terminal"):
        c = counts[name]
        print(f"- names per formation, {name}: mean {c.mean():.0f}, min {c.min()}, max {c.max()}")
    print(f"- lines in the selection panel: {weights['lines']}; formations without NYSE breakpoints: "
          f"{int(counts['no_bp'].sum())}")
    print(f"- t > 3 over {years:.2f} years of monthly returns needs an annualized Sharpe of 3/sqrt(years) = "
          f"{3 / math.sqrt(years):.3f} (over {args.t_selection / 12:.2f} years: {3 / math.sqrt(args.t_selection / 12):.3f})")

    rhos = sorted({args.rho, *(float(r) for r in args.null_rhos.split(","))})
    sigmas = {rho: covariance(weights["W"], t_all, rho) for rho in rhos}
    comps = range(len(COMPONENTS))

    def window(rho: float, t_sel: int) -> tuple[np.ndarray, np.ndarray, float, np.ndarray]:
        keep = np.arange(t_all - t_sel, t_all)
        sel = restrict(sigmas[rho], t_all, keep, comps)
        hold = restrict(sigmas[rho], t_all, np.arange(t_all - args.t_holdout, t_all), [0])
        sigma_ic = math.sqrt(float(np.mean(np.diag(sel)[:t_sel])))
        return cholesky(sel), cholesky(hold), sigma_ic, weights["planted_ls"][keep]

    main_sel, main_hold, sigma_ic, planted_ls = window(args.rho, args.t_selection)
    sel_sigma = restrict(sigmas[args.rho], t_all, np.arange(t_all - args.t_selection, t_all), comps)
    ic_block = sel_sigma[:args.t_selection, :args.t_selection]
    sd = np.sqrt(np.diag(ic_block))
    corr = ic_block / np.outer(sd, sd)
    print(f"\n## Null IC model (rho {args.rho}, T {args.t_selection})\n")
    print(f"- sd of the null 3-month rank IC per formation: {sigma_ic:.5f}; mean autocorrelation at lags 1/2/3/6: "
          + "/".join(f"{np.mean(np.diag(corr, k)):.3f}" for k in (1, 2, 3, 6)))
    print(f"- EWC df at T={args.t_selection}, h=3: {stats.ewc_degrees_of_freedom(args.t_selection, H)}; "
          f"at T={args.t_holdout}: {stats.ewc_degrees_of_freedom(args.t_holdout, H)}")

    # Checks: vectorized EWC == stats.mean_inference; p table vs exact; direct simulation vs model.
    rng = np.random.default_rng(args.seed)
    sample = (main_sel[:args.t_selection] @ rng.standard_normal((main_sel.shape[1], 50))).T
    _, tstat, df, p = ewc(sample, H)
    exact = [stats.mean_inference(row, horizon_periods=H) for row in sample]
    worst_t = max(abs(e.robust_t - t) for e, t in zip(exact, tstat, strict=True))
    worst_p = max(abs(e.robust_p_value - q) for e, q in zip(exact, p, strict=True))
    grid_t = rng.uniform(0, 8, 2000)
    table = _TABLES[df]
    rel = max(abs(table(np.array([x]))[0] / stats.student_t_two_sided_p(float(x), df) - 1) for x in grid_t)
    print(f"- vectorized EWC vs stats.mean_inference on 50 series: max |dt| {worst_t:.2e}, max |dp| {worst_p:.2e}; "
          f"p table max relative error {rel:.2e}")
    check = direct_check(panel, weights, sigmas[args.rho], args.rho, args.direct_features, args.seed + 1)
    print("- direct simulation (latent AR(1) ranked against the real returns) vs model: "
          + ", ".join(f"{k} {v:.3e}" for k, v in check.items()))

    results: dict[str, Any] = {"sample": {"T": t_all, "years": years, "first": formations[0][0].isoformat(),
                                          "last": formations[-1][0].isoformat(),
                                          "holdout_formations": len(panel["holdout_formations"]),
                                          "names": {k: float(v.mean()) for k, v in counts.items()}},
                               "sigma_ic": sigma_ic, "direct_check": check, "scenarios": []}

    # 1. Null runs: false qualification per evidence class.
    print(f"\n## Null false-qualification ({args.runs} runs x {args.features} null features, "
          f"T = {args.t_selection} / holdout {args.t_holdout}, n_trials {args.n_trials})\n")
    print("| scenario | one-sided p<=.05 | HLZ \\|z\\|>=3 | replication selection | replication final | "
          "discovery selection | discovery final | FWER rep sel | FWER disc sel |")
    print("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    null_specs = [(f"rho {rho}", rho, 0.0) for rho in rhos] + [(f"rho {args.rho} + regime AR(0.5) stress", args.rho, 1.0)]
    for label, rho, stress in null_specs:
        chol, chol_h, s_ic, pls = window(rho, args.t_selection) if rho != args.rho else (main_sel, main_hold,
                                                                                            sigma_ic, planted_ls)
        result = run_scenario(chol, chol_h, args.t_selection, args.t_holdout, policy=policy, icir=0.0, sigma_ic=s_ic,
                              planted_ls=pls, runs=args.runs, features=args.features, planted=0,
                              n_trials=args.n_trials, seed=args.seed + int(rho * 1000) + int(stress * 7), stress=stress)
        result["label"] = label
        results["scenarios"].append(result)
        null_counts = result["counts"]["null"]
        cells = []
        for name in ("one_sided_p05", "hlz", "replication_selection", "replication_final", "discovery_selection",
                     "discovery_final"):
            k, n = null_counts.get(name, 0), null_counts["features"]
            low, high = wilson(k, n)
            cells.append(f"{k / n:.5f} [{low:.5f}, {high:.5f}]")
        print(f"| {label} | " + " | ".join(cells) + " | "
              f"{result['fwer'].get('replication_selection', 0) / args.runs:.4f} | "
              f"{result['fwer'].get('discovery_selection', 0) / args.runs:.4f} |")
    print("\nNominal per-feature rates: replication 0.05 (one-sided alpha), discovery 0.0027 (HLZ two-sided).")

    # 2. Power curves.
    def power_table(title: str, rho: float, t_sel: int, planted: int, runs: int, classes: Sequence[str], *,
                    grid: Sequence[float] = ICIR_GRID, n_trials: int | None = None) -> None:
        trials = args.n_trials if n_trials is None else n_trials
        chol, chol_h, s_ic, pls = window(rho, t_sel)
        print(f"\n## {title}\n")
        print("| ICIR_3m | realized ICIR | annual LS Sharpe | \\|z\\|>=2 | HLZ \\|z\\|>=3 | v3 significance | "
              "v3 holdout gate (T=30) | " + " | ".join(f"{c} selection | {c} final | {c} holdout pass given selection"
                                                     for c in classes) + f" | null FQR {classes[0]} sel (partial null) |")
        print("|" + "---:|" * (7 + 3 * len(classes) + 1))
        curves: dict[str, list[float]] = {}
        for icir in grid:
            result = run_scenario(chol, chol_h, t_sel, args.t_holdout, policy=policy, icir=icir, sigma_ic=s_ic,
                                  planted_ls=pls, runs=runs, features=args.features, planted=planted,
                                  n_trials=trials, seed=args.seed + int(icir * 100) + t_sel + planted + trials,
                                  classes=classes)
            result.update({"label": f"{title} ICIR {icir}", "rho": rho, "t_selection": t_sel, "n_trials": trials})
            results["scenarios"].append(result)
            values = {name: rate(result, "true", name) for name in ("z2", "hlz", "v3_significance", "v3_holdout_gate")}
            cells = [f"{values[name]:.3f}" for name in ("z2", "hlz", "v3_significance", "v3_holdout_gate")]
            for c in classes:
                sel = rate(result, "true", f"{c}_selection")
                fin = rate(result, "true", f"{c}_final")
                given = fin / sel if sel else math.nan
                values.update({f"{c}_selection": sel, f"{c}_final": fin})
                cells += [f"{sel:.3f}", f"{fin:.3f}", f"{given:.3f}"]
            for key, value in values.items():
                curves.setdefault(key, []).append(value)
            print(f"| {icir:.1f} | {result['realized_icir']:.3f} | {result['annual_ls_sharpe']:.2f} | "
                  + " | ".join(cells) + f" | {rate(result, 'null', f'{classes[0]}_selection'):.4f} |")
        print("\nICIR_3m at 50% / 80% power (linear interpolation on the grid):\n")
        for key, values in curves.items():
            fifty, eighty = crossing(grid, values, 0.5), crossing(grid, values, 0.8)
            print(f"- {key}: 50% at {'n/a' if fifty is None else f'{fifty:.2f}'}, "
                  f"80% at {'n/a' if eighty is None else f'{eighty:.2f}'}")

    wide = (*ICIR_GRID, 1.5, 2.0)
    power_table(f"Power (rho {args.rho}, T {args.t_selection}, holdout {args.t_holdout}, {args.planted} of "
                f"{args.features} true, n_trials {args.n_trials}, {args.runs} runs)", args.rho, args.t_selection,
                args.planted, args.runs, ("replication", "discovery"), grid=wide)
    if not args.quick:
        short = tuple(x for x in ICIR_GRID if x <= 1.0)
        for trials in (200, 900):
            power_table(f"Sensitivity: discovery DSR n_trials {trials} (rho {args.rho}, T {args.t_selection}, "
                        f"{args.sensitivity_runs} runs)", args.rho, args.t_selection, args.planted,
                        args.sensitivity_runs, ("discovery",), grid=wide, n_trials=trials)
        for t_sel in (128, 104, 80):
            power_table(f"Sensitivity: selection T {t_sel} (rho {args.rho}, {args.sensitivity_runs} runs)",
                        args.rho, t_sel, args.planted, args.sensitivity_runs, ("replication",), grid=short)
        for rho in rhos:
            if rho != args.rho:
                power_table(f"Sensitivity: persistence rho {rho} (T {args.t_selection}, {args.sensitivity_runs} runs)",
                            rho, args.t_selection, args.planted, args.sensitivity_runs, ("replication",), grid=short)
        for planted in (1, 100):
            power_table(f"Sensitivity: {planted} of {args.features} true (rho {args.rho}, T {args.t_selection}, "
                        f"{args.sensitivity_runs} runs)", args.rho, args.t_selection, planted, args.sensitivity_runs,
                        ("replication",), grid=short)
    elapsed = time.time() - started
    print(f"\nelapsed {elapsed:.0f} s; {peak_memory_mib()}")
    if args.json is not None:
        args.json.write_text(json.dumps(results, indent=1, default=str), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
