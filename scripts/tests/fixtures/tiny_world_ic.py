"""tiny_world_ic: an independent numpy reference of the IC runner's rank-IC estimate on tiny_world (P9 lane T1).

The canary (scripts/tests/test_cycle_e2e.py) checks the u pass's per-candidate rank IC at the orientation horizon
against this reference, so a regression in the label, the rank correlation or the HAC rule is caught on the real
executable, and it asserts the planted members lie within a pre-registered band around the value the generator
planted (DS review section 6; PM ruling T1-SE: 2 SE).

  member_signal(world, member)        the member's DSL evaluated in numpy (dates x names; NaN before its window)
  rank_ic_series(signal, close, h)    the daily rank IC of the runner's recipe over tiny_world's mature score rows
  estimate(daily, h)                  mean, HAC standard error, t, lag: the runner's rule (ic_screen.cpp estimate)
  research_ic(world, member, h)       the three above for one member
  planted_rank_ic(member, h)          the population rank IC the generator plants (None for an unplanted member)
  planted_report(mean, se, planted, band_se)
                                      the distance of an estimate from its planted value in standard errors, and
                                      whether it lies inside the band
  min_relative_gap(signal)            the smallest gap between two names' signal values on a score row, relative
                                      to the row's range (a near tie could let the executable's arithmetic swap two
                                      ranks; the offline test asserts the reference is far from that)

The runner's recipe, as coded (atx-engine/src/factory/ic_screen.cpp, prepare_cache / evaluate_row / estimate; window
from research_window_ic_config over the role's score rows [score_begin, dates)):
  rows      decision d in [score_begin, score_begin + active), active = (dates - score_begin) - (1 + h): every label
            matures inside the window (execution delay 1)
  label     close(d + 1 + h) / close(d + 1) - 1 on the role's close (tiny_world: every name present, member and
            positive, so no pair is excluded)
  rank IC   Pearson correlation of the average-tied ranks of the signal and of the label over the paired names
  mean      ascending sum of the finite daily values / their count
  SE        max(HAC, HAC at lag 0) x n / valid; HAC = Bartlett, lag max(2h, floor(4 (n/100)^(2/9))), small-sample
            factor n / (n - 1), on the series centred by its mean (eval/hac.hpp mean_inference)
Summation follows the C++ order (ascending loops) where it is cheap; numpy reductions remain in the per-row
correlation, so the reference ties the executable to rounding, not to the bit.

The planted value (tiny_world.py module doc): r_i(t) = beta_i m(t) + sigma (c (zA_i(t-1) + zB_i(t-1)) + e_i(t)),
sigma = NOISE_SD, c = IC / sqrt(HORIZON), zA and zB AR(PHI) with unit variance. To first order the h-session label is
the sum of h returns, so for a signal s built from one planted state z
  Pearson  = sigma c cov(s, sum_{j=1..h} z(d + j)) / sqrt(var(s) var(label))
  var(label) = sigma^2 (h + 2 c^2 V_h) + var(beta) h MARKET_SD^2,  V_h = h + 2 sum_{k=1}^{h-1} (h - k) PHI^k
with var(beta) the population variance of the generator's U(.6, 1.4) betas, and the rank IC of a bivariate normal pair
is (6 / pi) asin(Pearson / 2). planted_b (rank of zB) gets cov = sum_j PHI^j; planted_a (rank of the 21-session
linear decay of zA, weights 1..21 oldest to newest) gets cov = sum_k w_k sum_j PHI^(j + k) and var(s) = sum_k sum_l
w_k w_l PHI^|k - l|. copy_b is planted_b's ranks; noise_c is independent of every return (planted value 0).
"""
from __future__ import annotations

import math

import numpy as np

import tiny_world as TW

DECAY = 21                      # planted_a's decay_linear window
EXECUTION_DELAY = 1             # the runner's label starts at the next session's close
UNPLANTED = {"noise_c": 0.0}
PLANTED_STATE = {"planted_a": ("za", DECAY), "planted_b": ("zb", 1), "copy_b": ("zb", 1)}


# ------------------------------------------------------------------ the members
def decay_linear(x: np.ndarray, window: int) -> np.ndarray:
    """Linear-decay mean over the last ``window`` rows, weights 1..window oldest to newest (alpha ts_ops)."""
    weights = np.arange(1, window + 1, dtype=np.float64)
    out = np.full_like(x, np.nan)
    for t in range(window - 1, x.shape[0]):
        out[t] = (weights[:, None] * x[t - window + 1:t + 1]).sum(axis=0) / weights.sum()
    return out


def member_signal(world: dict, member: str) -> np.ndarray:
    """The member's DSL (tiny_world.MEMBERS) in numpy, without the outer cross-sectional rank (monotone: it does
    not change a rank correlation)."""
    if member == "planted_a":
        return decay_linear(-1.0 * (world["si_shares"] / world["shares_out"]), DECAY)
    if member == "planted_b":
        return world["tiny_signal"].copy()
    if member == "noise_c":
        return world["shares_out"].copy()
    if member == "copy_b":
        return 2.0 * world["tiny_signal"]
    raise KeyError(f"tiny_world_ic: unknown member {member!r}")


# ------------------------------------------------------------------ the daily rank IC
def average_ranks(x: np.ndarray) -> np.ndarray:
    """1-based ranks with ties averaged."""
    order = np.argsort(x, kind="mergesort")
    ranks = np.empty(x.size, dtype=np.float64)
    sorted_x = x[order]
    k = 0
    while k < x.size:
        j = k
        while j + 1 < x.size and sorted_x[j + 1] == sorted_x[k]:
            j += 1
        ranks[order[k:j + 1]] = 0.5 * (k + j) + 1.0
        k = j + 1
    return ranks


def correlation(x: np.ndarray, y: np.ndarray) -> float:
    dx, dy = x - x.mean(), y - y.mean()
    den = math.sqrt(float((dx * dx).sum()) * float((dy * dy).sum()))
    return float((dx * dy).sum()) / den if den > 0 else math.nan


def score_rows(horizon: int) -> range:
    """The decision rows whose h-session label matures inside the score window."""
    available = TW.DATES - TW.SCORE_BEGIN
    lag = EXECUTION_DELAY + horizon
    return range(TW.SCORE_BEGIN, TW.SCORE_BEGIN + max(available - lag, 0))


def rank_ic_series(signal: np.ndarray, close: np.ndarray, horizon: int) -> np.ndarray:
    rows = score_rows(horizon)
    out = np.full(len(rows), np.nan)
    for k, d in enumerate(rows):
        entry = d + EXECUTION_DELAY
        label = close[entry + horizon] / close[entry] - 1.0
        keep = np.isfinite(signal[d]) & np.isfinite(label)
        if int(keep.sum()) >= 3:
            out[k] = correlation(average_ranks(signal[d][keep]), average_ranks(label[keep]))
    return out


# ------------------------------------------------------------------ the estimate (eval/hac.hpp, ic_screen estimate)
def rule_of_thumb_lag(n: int) -> int:
    if n < 2:
        return 0
    return min(int(math.floor(4.0 * math.pow(n / 100.0, 2.0 / 9.0))), n - 1)


def hac_se(x: list[float], lag: int) -> float:
    """mean_inference(x, Bartlett, lag, small_sample_correction=True).se, ascending accumulation."""
    n = len(x)
    m = 0.0
    for v in x:
        m += v
    m /= n
    lag = min(lag, n - 1)

    def cross(j: int) -> float:
        acc = 0.0
        for t in range(j, n):
            acc += (x[t] - m) * (x[t - j] - m)
        return acc
    s = cross(0)
    for j in range(1, lag + 1):
        s += 2.0 * (1.0 - j / (lag + 1.0)) * cross(j)
    var = s / (float(n) * float(n)) * (n / (n - 1.0))
    return math.sqrt(var) if var > 0 else math.nan


def estimate(daily: np.ndarray, horizon: int) -> dict:
    values = [float(v) for v in daily]
    finite = [v for v in values if math.isfinite(v)]
    valid, n = len(finite), len(values)
    total = 0.0
    for v in finite:
        total += v
    mean = total / valid
    lag = max(2 * horizon, rule_of_thumb_lag(n))
    work = [v - mean if math.isfinite(v) else 0.0 for v in values]
    scale = n / valid
    se = max(hac_se(work, lag) * scale, hac_se(work, 0) * scale)
    return {"mean": mean, "standard_error": se, "t": mean / se, "valid_dates": valid, "calendar_dates": n,
            "hac_lag": lag}


def research_ic(world: dict, member: str, horizon: int = TW.HORIZON) -> dict:
    return estimate(rank_ic_series(member_signal(world, member), world["close"], horizon), horizon)


def min_relative_gap(signal: np.ndarray, horizon: int = TW.HORIZON) -> float:
    gaps = []
    for d in score_rows(horizon):
        row = np.sort(signal[d][np.isfinite(signal[d])])
        span = float(row[-1] - row[0])
        gaps.append(float(np.diff(row).min()) / span if span > 0 else 0.0)
    return min(gaps)


# ------------------------------------------------------------------ the planted value
def label_variance(horizon: int) -> float:
    c = TW.IC / math.sqrt(TW.HORIZON)
    v_h = horizon + 2.0 * sum((horizon - k) * TW.PHI ** k for k in range(1, horizon))
    var_beta = 0.8 ** 2 / 12.0                      # U(.6, 1.4): beta = .6 + .8 u
    return TW.NOISE_SD ** 2 * (horizon + 2.0 * c * c * v_h) + var_beta * horizon * TW.MARKET_SD ** 2


def planted_rank_ic(member: str, horizon: int = TW.HORIZON) -> float | None:
    """The population rank IC at ``horizon`` the generator plants in ``member`` (0 for noise_c, None if unknown)."""
    if member in UNPLANTED:
        return UNPLANTED[member]
    if member not in PLANTED_STATE:
        return None
    _state, window = PLANTED_STATE[member]
    w = np.arange(window, 0, -1, dtype=np.float64)   # w_k for age k = 0 (newest) .. window - 1
    w /= w.sum()
    phi = TW.PHI
    cov = sum(w[k] * sum(phi ** (j + k) for j in range(1, horizon + 1)) for k in range(window))
    var_s = sum(w[k] * w[m] * phi ** abs(k - m) for k in range(window) for m in range(window))
    c = TW.IC / math.sqrt(TW.HORIZON)
    pearson = TW.NOISE_SD * c * cov / math.sqrt(var_s * label_variance(horizon))
    return 6.0 / math.pi * math.asin(pearson / 2.0)


def planted_report(mean: float, se: float, planted: float, band_se: float) -> dict:
    """An estimate against its planted value: t, planted t, z = (mean - planted) / SE, and whether |z| <= band_se."""
    z = (mean - planted) / se
    return {"mean": mean, "standard_error": se, "t": mean / se, "planted": planted, "planted_t": planted / se,
            "z": z, "band_se": band_se, "within_band": abs(z) <= band_se}
