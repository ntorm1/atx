"""Traded-horizon statistics at the construction's partial-adjustment rate theta (v8 C-2; report only).

The traded book moves a fraction theta of the way to its aim each session (aim-partial, strategy_target_replay.cpp:
``next = current + theta * (aim - current)``), so a position held at session d is the geometric average of past aims:
b(d) = theta * sum_{j>=0} (1 - theta)^j q(d - j). Two report-only statistics follow:

* ``ic_theta = sum_{h=1..63} theta (1 - theta)^(h-1) m(h)``, m(h) the lagged one-day rank IC at horizon h (entry
  shifted by h - 1, the report card's decay curve): the one-day IC of the theta-averaged position. The sum is
  truncated at h = 63 and not renormalised (the weights sum to 1 - (1 - theta)^63).
* the theta-averaged sleeve book b(d) = (1 - theta) b(d - 1) + theta q(d) from a flat start, and its factor return
  f_theta(d) = sum_i b(d)_i r_i(d), with r the book's forward return; a decision where b is flat is not live.

Nothing here gates, selects or weights anything (v8-prereg rule 8). Numpy only; imported by the fitter, the report
card and the diagnostics lane.
"""
from __future__ import annotations

import numpy as np

HORIZON_THETA = 0.05                       # the reference construction theta (fit_composition_weights.AIM_THETA)
IC_THETA_HORIZONS = tuple(range(1, 64))    # the report card's decay horizons h = 1..63


def theta_weights(horizons=IC_THETA_HORIZONS, theta: float = HORIZON_THETA) -> np.ndarray:
    """theta (1 - theta)^(h - 1) for each horizon h."""
    h = np.asarray(horizons, dtype=np.float64)
    return theta * (1.0 - theta) ** (h - 1.0)


def ic_theta(m, horizons=IC_THETA_HORIZONS, theta: float = HORIZON_THETA) -> float | None:
    """sum_h theta (1 - theta)^(h - 1) m(h); None when m has another length or any m(h) is undefined (NaN/None)."""
    values = np.array([np.nan if x is None else x for x in m], dtype=np.float64)
    if values.shape != (len(horizons),) or not bool(np.all(np.isfinite(values))):
        return None
    return float(theta_weights(horizons, theta) @ values)


def theta_book_returns(q: np.ndarray, forward: np.ndarray, theta: float = HORIZON_THETA) -> tuple[np.ndarray, np.ndarray]:
    """(f_theta, live) for books q and forward returns (decisions x names, aligned): b(d) = (1 - theta) b(d - 1) +
    theta q(d) with b = 0 before the first decision; f_theta(d) = sum_i b(d)_i forward(d)_i. A decision is live from
    the first decision whose q row is not all zero (before it b is flat); f_theta is NaN where not live."""
    q = np.asarray(q, dtype=np.float64)
    forward = np.asarray(forward, dtype=np.float64)
    if q.shape != forward.shape or q.ndim != 2:
        raise ValueError("theta_book_returns: q and forward must be aligned 2-d arrays")
    t = q.shape[0]
    live = np.logical_or.accumulate(np.any(q != 0, axis=1)) if t else np.zeros(0, dtype=bool)
    f = np.full(t, np.nan)
    b, step = np.zeros(q.shape[1]), np.empty(q.shape[1])
    keep = 1.0 - theta
    for d in range(t):
        b *= keep
        np.multiply(q[d], theta, out=step)
        b += step
        if live[d]:
            f[d] = float(b @ forward[d])
    return f, live
