"""Facade for performance evaluation: metrics, deflated Sharpe, PBO, CPCV."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from . import _core


@dataclass
class Metrics:
    sharpe: float
    sortino: float
    max_dd: float
    calmar: float
    ir: float
    appraisal: float
    hit_rate: float


def _to_metrics(rm) -> Metrics:
    return Metrics(rm.sharpe, rm.sortino, rm.max_dd, rm.calmar, rm.ir, rm.appraisal, rm.hit_rate)


def return_metrics(pnl, periods_per_year: float = 252.0) -> Metrics:
    """Metrics from a per-period RETURN series (not equity levels)."""
    pnl = np.asarray(pnl, dtype=np.float64).tolist()
    return _to_metrics(_core.compute_return_metrics(pnl, periods_per_year))


def performance(result_or_equity, periods_per_year: float = 252.0) -> Metrics:
    """Metrics from a BacktestResult or an equity-level series.

    Accepts an `atxpy.BacktestResult` (uses its equity_curve), a pandas Series, or
    any array-like of equity levels; converts to per-period returns first.
    """
    equity = getattr(result_or_equity, "equity_curve", None)
    if equity is not None:  # a BacktestResult facade object
        equity = equity["equity"].to_numpy(dtype=np.float64)
    else:
        equity = np.asarray(result_or_equity, dtype=np.float64)
    pnl = _core.returns_from_equity(equity.tolist())
    return _to_metrics(_core.compute_return_metrics(pnl, periods_per_year))


def deflated_sharpe(sr, n_obs, *, skew=0.0, excess_kurtosis=0.0, n_trials=1, variance=None):
    """Deflated Sharpe ratio across n_trials candidate strategies."""
    return _core.deflated_sharpe(sr, n_obs, skew, excess_kurtosis, n_trials, variance)


def pbo(perf, n_candidates, n_splits):
    """Probability of backtest overfitting (CSCV). `perf` is candidate-major flattened."""
    perf = np.asarray(perf, dtype=np.float64).ravel().tolist()
    return _core.pbo_cscv(perf, n_candidates, n_splits)


def cpcv_folds(spans, *, n_groups=6, n_test_groups=2, embargo=0.01):
    """Combinatorial purged CV folds. `spans` is a list of (t0, t1) tuples."""
    cfg = _core.CpcvConfig()
    cfg.n_groups = n_groups
    cfg.n_test_groups = n_test_groups
    cfg.embargo = embargo
    label_spans = [_core.LabelSpan(int(t0), int(t1)) for (t0, t1) in spans]
    return _core.cpcv_folds(label_spans, cfg)
