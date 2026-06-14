"""Facade for the multi-strategy meta-book: capital allocation across sleeves."""

from __future__ import annotations

import numpy as np

from . import _core


def sleeve_cov(sleeve_pnl):
    """S x S sleeve-return covariance Omega from per-sleeve PnL series (list of arrays)."""
    rows = [np.asarray(p, dtype=np.float64).ravel().tolist() for p in sleeve_pnl]
    return np.asarray(_core.sleeve_return_cov(rows), dtype=np.float64)


def meta_allocate(Omega, *, sleeve_vol=None, caps=None, method=None,
                  fractional_kelly=0.3, target_vol=0.0, max_gross=4.0, solve_iters=64):
    """Allocate per-sleeve capital weights from a sleeve-return covariance.

    Omega: S x S covariance. sleeve_vol defaults to sqrt(diag(Omega)); caps defaults
    to an effectively-uncapped box. Returns a float64 vector of capital weights.
    """
    Omega = np.ascontiguousarray(Omega, dtype=np.float64)
    s = Omega.shape[0]
    if sleeve_vol is None:
        sleeve_vol = np.sqrt(np.diag(Omega))
    if caps is None:
        caps = np.full(s, 1e18)
    cfg = _core.MetaAllocatorConfig()
    if method is not None:
        cfg.method = method
    cfg.fractional_kelly = fractional_kelly
    cfg.target_vol = target_vol
    cfg.max_gross = max_gross
    cfg.solve_iters = solve_iters
    alloc = _core.MetaAllocator(cfg)
    w = alloc.allocate(Omega,
                       np.asarray(sleeve_vol, dtype=np.float64).tolist(),
                       np.asarray(caps, dtype=np.float64).tolist())
    return np.asarray(w.c, dtype=np.float64)
