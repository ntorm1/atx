"""Facade for portfolio risk: factor models + turnover-penalized optimization."""

from __future__ import annotations

import numpy as np

from . import _core


def factor_model(X, F, D, *, fit_begin: int = 0, fit_end: int = 0) -> "_core.FactorModel":
    """Build a FactorModel from exposures X (M x K), factor cov F (K x K), specific var D (M).

    X, F are 2D float64 arrays; D is a 1D float64 array of length M.
    """
    X = np.ascontiguousarray(X, dtype=np.float64)
    F = np.ascontiguousarray(F, dtype=np.float64)
    D = np.ascontiguousarray(D, dtype=np.float64).ravel()
    return _core.make_factor_model(X, F, D, fit_begin, fit_end)


def optimize(alpha, model, w_prev=None, *, risk_aversion=1.0, turnover_penalty=0.0,
             gross_leverage=1.0, name_cap=1.0, dollar_neutral=True, max_iters=64):
    """Solve for target weights given an alpha vector and a FactorModel.

    Returns a float64 ndarray of length M (the universe size).
    """
    cfg = _core.OptimizerConfig()
    cfg.risk_aversion = risk_aversion
    cfg.turnover_penalty = turnover_penalty
    cfg.gross_leverage = gross_leverage
    cfg.name_cap = name_cap
    cfg.dollar_neutral = dollar_neutral
    cfg.max_iters = max_iters
    opt = _core.PortfolioOptimizer(cfg)
    alpha = np.asarray(alpha, dtype=np.float64).tolist()
    prev = [] if w_prev is None else np.asarray(w_prev, dtype=np.float64).tolist()
    return np.asarray(opt.solve(alpha, model, prev), dtype=np.float64)
