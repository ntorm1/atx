"""The mined-v1 label-overlap factor (platform v8 lane MINE-FIX; Ruling E-32a, review MINE-6).

mined-v1 reads one statistic twice: the marginal IC HAC t of a mined signal (research_ic_fitness.cpp: the daily
combine::marginal_rank_ic_day series, then combine::summarize_rank_ic at Bartlett lag 21, the small-sample corrected
eval::hac::mean_inference of the defined days). Its labels are h 21 forward returns, so neighbouring days share 20 of
21 returns; for a persistent signal the daily series is MA(20)-like and a Bartlett lag-21 standard error is too small
(review MINE-6). Ruling E-32a: the hurdles apply to t / F, F derived here by simulation under the null and pinned as
strategy_mine_rule.hpp kMinedOverlapFactor.

The null (one replication):
  NAMES names, i.i.d. N(0, 1) returns per name and session; the label of decision row t is the sum of the returns of
  sessions t + 2 .. t + 22 (execution delay 1, horizon 21: close[t+22] / close[t+1] - 1 has the same ranks under
  log returns); the signal is i.i.d. N(0, 1) per name and constant over the window -- full persistence, the worst
  case, since its daily ICs share label overlap at every lag below 21 (an i.i.d. signal shares none).
  The daily statistic is marginal_rank_ic_day with zero regressors on a full cross-section: Pearson of the signal's
  centred rank (its intercept-only residual) with the label's ranks. t = summarize_rank_ic(daily, 21).

F is the larger of two quantile ratios, rounded up to two decimals:
  (a) the confirm gate: ROWS_CONFIRM = kMinedMinConfirmRows label rows, the shortest window the rule reads at the
      confirm; the (1 - 2 Phi(-2)) quantile of |t| over 2, so that t / F >= 2 has the nominal one-sided level Phi(-2)
      of the confirm read's "HAC t 2.0";
  (b) the discover tail: ROWS_DISCOVER = kMinedMinDiscoverRows label rows, the shortest discover window; the 99.8%
      quantile of |t| over z(.999). The Bonferroni hurdles lie further out (z 3.48 at N 100), where the simulation
      cannot resolve the tail at test time; at these window lengths the ratio rises slowly into the tail (the report
      gives the shape), so (a) is the binding term and the hurdle is conservative to about the 99.8% level.

Run: "C:/Program Files/Python312/python.exe" atx-impl/tools/mine_overlap_factor.py   (prints the derivation as JSON;
exit 0 iff it reproduces FACTOR). Test: atx-impl/tools/test_mine_overlap_factor.py (runs it, about 15 s).
"""
from __future__ import annotations

import json
import math
from statistics import NormalDist
import sys

import numpy as np

SEED = 20260930            # the derivation's seed: (a) uses SEED, (b) SEED + 1
HORIZON = 21               # label sessions (research IC h 21)
DELAY = 1                  # execution delay: the label's first return is session t + 2
HAC_LAG = 21               # factory::kResearchIcHacLag
NAMES = 30
ROWS_CONFIRM = 200         # strategy_mine_rule.hpp kMinedMinConfirmRows
REPS_CONFIRM = 20_000
ROWS_DISCOVER = 504        # strategy_mine_rule.hpp kMinedMinDiscoverRows
REPS_DISCOVER = 10_000
DISCOVER_LEVEL = 0.998     # two-sided level of the discover-tail ratio
CHUNK = 1_000
FACTOR = 1.55              # pinned: strategy_mine_rule.hpp kMinedOverlapFactor


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


def null_t(rng: np.random.Generator, reps: int, rows: int) -> np.ndarray:
    """``reps`` null draws of the verb's t on ``rows`` label rows (module docstring)."""
    out = np.empty(reps)
    centred = np.arange(NAMES, dtype=np.float64) - (NAMES - 1) / 2.0   # ranks, centred (correlation is scale-free)
    span = DELAY + HORIZON
    for c0 in range(0, reps, CHUNK):
        r = min(CHUNK, reps - c0)
        returns = rng.standard_normal((r, rows + span, NAMES))
        level = np.cumsum(returns, axis=1)
        labels = level[:, span:span + rows, :] - level[:, DELAY:DELAY + rows, :]   # sessions t+2 .. t+22
        label_rank = np.empty_like(labels)
        np.put_along_axis(label_rank, np.argsort(labels, axis=-1), np.broadcast_to(centred, labels.shape), axis=-1)
        signal = rng.standard_normal((r, NAMES))
        signal_rank = np.empty_like(signal)
        np.put_along_axis(signal_rank, np.argsort(signal, axis=-1), np.broadcast_to(centred, signal.shape), axis=-1)
        num = np.einsum("rtn,rn->rt", label_rank, signal_rank)
        den = np.sqrt((signal_rank * signal_rank).sum(axis=-1))[:, None] * np.sqrt((label_rank ** 2).sum(axis=-1))
        out[c0:c0 + r] = hac_t(num / den)
    return out


def quantile_ratio(t: np.ndarray, two_sided_level: float) -> float:
    """Q_level(|t|) / z, z the standard normal's two-sided quantile at the same level."""
    z = NormalDist().inv_cdf(0.5 + two_sided_level / 2.0)
    return float(np.quantile(np.abs(t), two_sided_level)) / z


def derive() -> dict:
    confirm_level = 1.0 - 2.0 * NormalDist().cdf(-2.0)
    t_confirm = null_t(np.random.default_rng(SEED), REPS_CONFIRM, ROWS_CONFIRM)
    t_discover = null_t(np.random.default_rng(SEED + 1), REPS_DISCOVER, ROWS_DISCOVER)
    a = quantile_ratio(t_confirm, confirm_level)
    b = quantile_ratio(t_discover, DISCOVER_LEVEL)
    shape = {f"{lvl}": round(quantile_ratio(t, lvl), 4) for t in (t_confirm,) for lvl in (0.9, 0.99, 0.998)}
    tail = {f"{lvl}": round(quantile_ratio(t_discover, lvl), 4) for lvl in (0.9, 0.99, 0.998)}
    factor = math.ceil(max(a, b) * 100.0 - 1e-9) / 100.0
    return {"seed": SEED, "names": NAMES, "horizon": HORIZON, "hac_lag": HAC_LAG,
            "confirm": {"rows": ROWS_CONFIRM, "reps": REPS_CONFIRM, "level": round(confirm_level, 6),
                        "ratio": round(a, 4), "sd": round(float(t_confirm.std(ddof=1)), 4), "shape": shape},
            "discover": {"rows": ROWS_DISCOVER, "reps": REPS_DISCOVER, "level": DISCOVER_LEVEL,
                         "ratio": round(b, 4), "sd": round(float(t_discover.std(ddof=1)), 4), "shape": tail},
            "factor": factor, "pinned": FACTOR}


def main() -> int:
    out = derive()
    print(json.dumps(out, indent=2))
    return 0 if out["factor"] == FACTOR else 1


if __name__ == "__main__":
    sys.exit(main())
