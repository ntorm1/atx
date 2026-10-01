"""The mined-v1 label-overlap factors (lane MINE-FIX, Ruling E-32a, review MINE-6; re-derived by lane MINE-STAT at
the campaign's own level, lifting Ruling PM4-13).

mined-v1 reads one statistic twice: the marginal IC HAC t of a mined signal (research_ic_fitness.cpp: the daily
combine::marginal_rank_ic_day series, then combine::summarize_rank_ic at Bartlett lag 21, the small-sample corrected
eval::hac::mean_inference of the defined days). Its labels are h 21 forward returns, so neighbouring days share 20 of
21 returns; for a persistent signal the daily series is MA(20)-like and a Bartlett lag-21 standard error is too small
(review MINE-6). Ruling E-32a: every hurdle applies to t / F, F derived here under the null and pinned in
strategy_mine_rule.hpp.

The null (one draw): NAMES names, i.i.d. N(0, 1) returns per name and session; the label of decision row t is the sum
of the returns of sessions t + 2 .. t + 22 (execution delay 1, horizon 21: close[t+22] / close[t+1] - 1 has the same
ranks under log returns); the signal is constant over the window (full persistence, the worst case: its daily ICs
share label overlap at every lag below 21). The names are exchangeable, so the signal's ranks are fixed at 0 .. NAMES-1
without loss (the MINE-FIX script drew them at random; the distribution of t is the same). The daily statistic is
marginal_rank_ic_day with zero regressors on a full cross-section: Pearson of the signal's centred rank with the
label's ranks. t = summarize_rank_ic(daily, 21). Under the null t is symmetric (reversing the names negates every
daily IC), so the hurdles below are one-sided tails of t.

What each factor makes nominal (F = the quantile of t at the level over the normal quantile at the same level):
  discover  the shortlist reads f2 / F >= z(N) = -norm_ppf(alpha / (2 N)) (strategy_mine_rule mined_hurdle). With
            zero regressors f2 = |t|, so F(N) = Q(t, 1 - alpha / (2 N)) / z(N) makes the per-trial rejection rate
            P(|t| / F >= z(N)) the Bonferroni alpha / N. Derived at budgets 100, 1,000 and 10,000 on the discover
            floor (ROWS_DISCOVER = kMinedMinDiscoverRows); budget band (N_prev, N] reads F(N), which covers it
            because the ratio grows into the tail. With regressors f2 = sign(raw IC) x marginal t <= |marginal t|.
  confirm   the confirm read is t / Fc: confirmed iff t / Fc >= 2 and the Benjamini-Yekutieli adjusted p of
            Phi(-t / Fc) is at most .10 over the m reads. BY at .10 compares the k-th smallest p with
            .10 k / (m H(m)), H the harmonic number, and its proof needs P(p <= .10 k / (m H(m))) <= that level for
            every k: the deepest is k = 1. So Fc(m) = the larger of the gate's ratio (level Phi(-2)) and the ratio at
            .10 / (m H(m)), on the confirm floor (ROWS_CONFIRM = kMinedMinConfirmRows). Derived at m = 16 (the
            default shortlist cap), 64 and 256 (the configuration's bound); band (m_prev, m] reads Fc(m).
  four-year reference only: the discover levels on the whole of TRAIN (about 1,006 decision sessions on the 4-year
            role, 984 mature label rows), to show that a longer window needs a smaller factor, so the floor's table
            is conservative for every admitted window.

Tail estimation. A deep tail (2.5e-6 one-sided at N = 10,000) needs millions of plain draws; each cell instead
draws from an importance-sampling proposal and weighs every draw by its likelihood ratio, so the estimate stays
unbiased for the exact null above (the ranks, the label overlap, the verb's t): only the proposal changes. The
proposal is the dominating point of the linearised statistic: the daily IC is to first order a moving sum over the
label window of y(s) = the session's returns projected on the signal's ranks, so t ~ kappa Z / sqrt(sum_k lam_k z_k^2)
with Z = the projection of y on the window-weight direction (the mean) and z_k on the eigenvectors of the Bartlett
quadratic form (the HAC variance; lam_k its eigenvalues, normalised to the form's trace). For a target c the proposal
shifts Z by theta and shrinks each z_k to sd 1 / sqrt(1 + (c / kappa)^2 lam_k): the likeliest way the event happens
(a large mean and a small HAC variance). Each cell runs a pilot of PILOT_DRAWS to place c, then DRAWS[rows] draws;
the quantile's standard error is a bootstrap of the draws (BOOT resamples). factor = ceil(ratio + MARGIN_SE x se, 2
decimals): an upper confidence bound, rounded up.

Seeds: cell k draws its pilot from default_rng([SEED, k, 0]), its draws from [SEED, k, 1] and its bootstrap from
[SEED, k, 2], so any subset of cells reproduces the full run's numbers.

Run: "C:/Program Files/Python312/python.exe" atx-impl/tools/mine_overlap_factor.py   (prints the derivation as JSON;
about 5 minutes, under 1 GB; exit 0 iff it reproduces the pinned tables and record).
Test: atx-impl/tools/test_mine_overlap_factor.py (an importance-sampling check against plain draws and three cells
re-derived at their committed seeds, about 40 s).
"""
from __future__ import annotations

import functools
import json
import math
from statistics import NormalDist
import sys
import time

import numpy as np

SEED = 20261001            # the derivation's seed (cell k: [SEED, k, stage])
HORIZON = 21               # label sessions (research IC h 21)
DELAY = 1                  # execution delay: the label's first return is session t + 2
SPAN = DELAY + HORIZON
HAC_LAG = 21               # factory::kResearchIcHacLag
NAMES = 30
ROWS_CONFIRM = 200         # strategy_mine_rule.hpp kMinedMinConfirmRows
ROWS_DISCOVER = 504        # strategy_mine_rule.hpp kMinedMinDiscoverRows
ROWS_FOUR_YEAR = 984       # TRAIN on the 4-year role: about 1,006 decision sessions, 22 fewer mature label rows
FAMILY_ALPHA = 0.05        # strategy_mine_rule.hpp kMinedFamilyAlpha
CONFIRM_T = 2.0            # kMinedConfirmT
CONFIRM_BY = 0.10          # kMinedConfirmBy
BUDGETS = (100, 1_000, 10_000)
CONFIRM_READS = (16, 64, 256)  # the default --max-promotions, then up to the configuration's bound 256
DRAWS = {ROWS_CONFIRM: 20_000, ROWS_DISCOVER: 20_000, ROWS_FOUR_YEAR: 10_000}
PILOT_DRAWS = 1_000
BOOT = 200
MARGIN_SE = 2.0
CHUNK_CELLS = 250_000      # label rows per simulation chunk (about 300 MB of working arrays)

# Pinned: strategy_mine_rule.hpp kMinedOverlapBands and kMinedConfirmBands, (band top, factor).
OVERLAP_BANDS = ((100, 1.47), (1_000, 1.54), (10_000, 1.63))
CONFIRM_BANDS = ((16, 1.77), (64, 1.96), (256, 2.15))
MAX_BUDGET = OVERLAP_BANDS[-1][0]  # kMinedMaxBudget: the largest budget the table covers

# The cells, in seed order: (kind, label rows, budget N or confirm reads m; 0 for the gate).
CELLS = (("confirm-gate", ROWS_CONFIRM, 0),
         *(("confirm-by", ROWS_CONFIRM, m) for m in CONFIRM_READS),
         *(("discover", ROWS_DISCOVER, n) for n in BUDGETS),
         *(("four-year", ROWS_FOUR_YEAR, n) for n in BUDGETS))

# Pinned: the full run at SEED (ratio, its standard error), 6 decimals.
DERIVED = {
    "confirm-gate:0": (1.534716, 0.004969),
    "confirm-by:16": (1.756336, 0.006818),
    "confirm-by:64": (1.940199, 0.004951),
    "confirm-by:256": (2.121859, 0.012160),
    "discover:100": (1.457816, 0.002962),
    "discover:1000": (1.530777, 0.002708),
    "discover:10000": (1.622871, 0.002926),
    "four-year:100": (1.327235, 0.003242),
    "four-year:1000": (1.362150, 0.003064),
    "four-year:10000": (1.401974, 0.002970),
}

CENTRED = np.arange(NAMES, dtype=np.float64) - (NAMES - 1) / 2.0   # the signal's centred ranks
CENTRED_SS = float(CENTRED @ CENTRED)
UNIT = CENTRED / math.sqrt(CENTRED_SS)


def hac_t(series: np.ndarray, lag: int = HAC_LAG) -> np.ndarray:
    """summarize_rank_ic's t for each row of ``series`` (every value finite): eval::hac::mean_inference with the
    Bartlett kernel w_j = 1 - j / (L + 1), L = min(lag, n - 1), and the small-sample factor n / (n - 1)."""
    x = np.atleast_2d(np.asarray(series, dtype=np.float64))
    n = x.shape[1]
    mean = x.mean(axis=1)
    e = x - mean[:, None]
    lags = min(lag, n - 1)
    s = (e * e).sum(axis=1)
    for j in range(1, lags + 1):
        s = s + 2.0 * (1.0 - j / (lags + 1.0)) * (e[:, j:] * e[:, :-j]).sum(axis=1)
    var = s / (n * n) * (n / (n - 1.0))
    return mean / np.sqrt(var)


def summarize_t(daily: list[float], lag: int = HAC_LAG) -> float:
    """summarize_rank_ic(daily, lag).hac_t: the defined (finite) days compacted in order, then hac_t."""
    kept = np.array([v for v in daily if math.isfinite(v)], dtype=np.float64)
    return float(hac_t(kept[None, :], lag)[0])


def cell_name(kind: str, key: int) -> str:
    return f"{kind}:{key}"


def cell_level(kind: str, key: int) -> float:
    """The one-sided level P(t > c) the cell's factor makes nominal (module docstring)."""
    if kind == "confirm-gate":
        return NormalDist().cdf(-CONFIRM_T)
    if kind == "confirm-by":
        return CONFIRM_BY / (key * sum(1.0 / i for i in range(1, key + 1)))
    return FAMILY_ALPHA / (2.0 * key)


@functools.lru_cache(maxsize=None)
def label_geometry(rows: int) -> tuple[np.ndarray, np.ndarray, np.ndarray, float]:
    """The linearised statistic on ``rows`` label rows: the mean's direction over the sessions, the eigenvectors of
    the Bartlett form on its complement with their trace-normalised eigenvalues (those above 1e-4), and kappa, the
    ratio of the mean's sd to the expected HAC sd (about 1.2: the lag-21 under-correction)."""
    sessions = rows + SPAN
    window = np.zeros((rows, sessions))
    for t in range(rows):
        window[t, t + DELAY + 1:t + SPAN + 1] = 1.0
    demean = np.eye(rows) - 1.0 / rows
    lag = min(HAC_LAG, rows - 1)
    gap = np.abs(np.subtract.outer(np.arange(rows), np.arange(rows)))
    bartlett = np.where(gap <= lag, 1.0 - gap / (lag + 1.0), 0.0)
    form = window.T @ demean @ bartlett @ demean @ window
    trace = float(np.trace(form))
    mean_dir = window.sum(axis=0)
    kappa = math.sqrt((mean_dir @ mean_dir / rows ** 2) / (trace / (rows * (rows - 1.0))))
    mean_dir = mean_dir / np.linalg.norm(mean_dir)
    complement = np.eye(sessions) - np.outer(mean_dir, mean_dir)
    values, vectors = np.linalg.eigh(complement @ form @ complement)
    lam = values[::-1] / trace
    keep = lam > 1e-4
    return mean_dir, vectors[:, ::-1][:, keep], lam[keep], kappa


def proposal(rows: int, target: float) -> tuple[np.ndarray, np.ndarray, np.ndarray, float]:
    """The dominating-point proposal for P(t > target): (mean direction, variance directions, their sd, shift)."""
    mean_dir, dirs, lam, kappa = label_geometry(rows)
    ct = target / kappa
    scale = 1.0 / np.sqrt(1.0 + ct * ct * lam)
    shift = ct * math.sqrt(float((lam * scale * scale).sum()) + max(0.0, 1.0 - float(lam.sum())))
    return mean_dir, dirs, scale, shift


def null_t(rng: np.random.Generator, reps: int, rows: int,
           target: float | None = None) -> tuple[np.ndarray, np.ndarray]:
    """``reps`` draws of the verb's t on ``rows`` label rows and their log likelihood ratios: plain null draws
    (log weight 0) without ``target``, else draws from proposal(rows, target)."""
    t = np.empty(reps)
    logw = np.zeros(reps)
    tilt = proposal(rows, target) if target is not None else None
    chunk = max(1, CHUNK_CELLS // rows)
    for c0 in range(0, reps, chunk):
        r = min(chunk, reps - c0)
        returns = rng.standard_normal((r, rows + SPAN, NAMES))
        if tilt is not None:
            mean_dir, dirs, scale, shift = tilt
            along = returns @ UNIT                    # each session's return along the signal's ranks
            z0 = along @ mean_dir
            z = along @ dirs
            move = shift * mean_dir[None, :] + (z * (scale - 1.0)) @ dirs.T
            returns += move[:, :, None] * UNIT[None, None, :]
            logw[c0:c0 + r] = (-shift * z0 - 0.5 * shift * shift + float(np.log(scale).sum())
                               + 0.5 * ((1.0 - scale * scale) * z * z).sum(axis=1))
        level = np.cumsum(returns, axis=1)
        labels = level[:, SPAN:SPAN + rows, :] - level[:, DELAY:DELAY + rows, :]   # sessions t+2 .. t+22
        # Pearson(centred signal rank, label rank) = sum_j c[order_j] c_j / sum c^2 (ranks are a permutation).
        daily = CENTRED[np.argsort(labels, axis=-1)] @ CENTRED / CENTRED_SS
        t[c0:c0 + r] = hac_t(daily)
    return t, logw


def tail_quantile(t: np.ndarray, logw: np.ndarray, level: float) -> float:
    """The c with estimated P(t > c) = level: the weighted tail mass (1 / n) sum w_k 1(t_k >= c) over the draws,
    linear between neighbouring draws (NaN when the draws carry less mass than ``level``)."""
    finite = np.isfinite(t)
    order = np.argsort(-t[finite], kind="stable")
    ts = t[finite][order]
    mass = np.cumsum(np.exp(logw[finite][order])) / t.size
    k = int(np.searchsorted(mass, level))
    if k == 0:
        return float(ts[0])
    if k >= ts.size:
        return math.nan
    f = (level - mass[k - 1]) / (mass[k] - mass[k - 1])
    return float(ts[k - 1] + f * (ts[k] - ts[k - 1]))


def quantile_se(t: np.ndarray, logw: np.ndarray, level: float, rng: np.random.Generator) -> float:
    """Bootstrap standard error of tail_quantile (BOOT resamples of the draws)."""
    picks = (rng.integers(0, t.size, t.size) for _ in range(BOOT))
    return float(np.std([tail_quantile(t[i], logw[i], level) for i in picks], ddof=1))


def derive_cell(index: int) -> dict:
    """One cell of CELLS at its committed seed: pilot, draws, the quantile ratio and its standard error."""
    kind, rows, key = CELLS[index]
    level = cell_level(kind, key)
    z = NormalDist().inv_cdf(1.0 - level)
    kappa = label_geometry(rows)[3]
    guess = 1.25 * kappa * z
    pilot = null_t(np.random.default_rng([SEED, index, 0]), PILOT_DRAWS, rows, guess)
    target = tail_quantile(*pilot, level)
    if not math.isfinite(target):
        target = guess
    draws = DRAWS[rows]
    t, logw = null_t(np.random.default_rng([SEED, index, 1]), draws, rows, target)
    q = tail_quantile(t, logw, level)
    se = quantile_se(t, logw, level, np.random.default_rng([SEED, index, 2]))
    hit = np.where(np.isfinite(t) & (t >= q), np.exp(logw), 0.0)
    tail_rel_se = float(hit.std(ddof=1) / math.sqrt(draws) / level)
    return {"cell": cell_name(kind, key), "kind": kind, "rows": rows, "key": key, "level": level,
            "z": round(z, 6), "draws": draws, "pilot_draws": PILOT_DRAWS, "target": round(target, 4),
            "quantile": round(q, 6), "quantile_se": round(se, 6), "ratio": round(q / z, 6),
            "ratio_se": round(se / z, 6), "upper": round((q + MARGIN_SE * se) / z, 6),
            "tail_rel_se": round(tail_rel_se, 4),
            # plain draws needed for the same relative standard error of the tail probability
            "plain_equivalent": int((1.0 - level) / (level * tail_rel_se ** 2))}


def factor_of(upper: float) -> float:
    return math.ceil(upper * 100.0 - 1e-9) / 100.0


def tables(cells: dict[str, dict]) -> dict:
    """The rule's tables from the cells (module docstring)."""
    overlap = [[n, factor_of(cells[cell_name("discover", n)]["upper"])] for n in BUDGETS]
    gate = cells[cell_name("confirm-gate", 0)]["upper"]
    confirm = [[m, factor_of(max(gate, cells[cell_name("confirm-by", m)]["upper"]))] for m in CONFIRM_READS]
    return {"overlap_bands": overlap, "confirm_bands": confirm}


def derive(indices: list[int] | None = None) -> dict:
    started = time.perf_counter()
    cells = {}
    for index in range(len(CELLS)) if indices is None else indices:
        out = derive_cell(index)
        cells[out["cell"]] = out
    result = {"seed": SEED, "names": NAMES, "horizon": HORIZON, "hac_lag": HAC_LAG, "margin_se": MARGIN_SE,
              "cells": cells}
    if indices is None:
        result.update(tables(cells))
    result["seconds"] = round(time.perf_counter() - started, 1)
    return result


def main() -> int:
    out = derive()
    print(json.dumps(out, indent=2))
    pinned = {"overlap_bands": [list(b) for b in OVERLAP_BANDS], "confirm_bands": [list(b) for b in CONFIRM_BANDS]}
    record = all(math.isclose(out["cells"][name]["ratio"], ratio, abs_tol=1e-5) and
                 math.isclose(out["cells"][name]["ratio_se"], se, abs_tol=1e-5)
                 for name, (ratio, se) in DERIVED.items())
    same = all(out[key] == value for key, value in pinned.items())
    return 0 if same and record else 1


if __name__ == "__main__":
    sys.exit(main())
