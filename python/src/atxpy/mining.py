"""Facade for evolutionary alpha mining + combination."""

from __future__ import annotations

import numpy as np

from . import _core


def mine_alphas(panel, *, seed_exprs, panel_fields=None, master_seed=0,
                population=16, generations=5, elites=2, k_tournament=3,
                p_cross=0.5, novelty_w=0.1, max_lookback=250, trial_count=4,
                min_dsr=0.5, book_size=1.0, min_sharpe=1.0, min_fitness=1.0,
                max_turnover=0.70, max_pool_corr=0.7):
    """Run an evolutionary alpha-mining search over a research panel.

    panel: dict {field_name: 2D ndarray [dates, instruments]}.
    seed_exprs: in-grammar starting expressions (e.g. ["rank(close)", "ts_mean(close, 5)"]).
    Returns a _core.MineResult with .report (telemetry) and .pool (admitted alphas).
    """
    if not panel:
        raise ValueError("panel needs at least one field")
    items = list(panel.items())
    first = np.ascontiguousarray(items[0][1], dtype=np.float64)
    if first.ndim != 2:
        raise ValueError("each panel field must be a 2D [dates, instruments] array")
    dates, instruments = first.shape

    p = _core.MineParams()
    p.dates = int(dates)
    p.instruments = int(instruments)
    names, cols = [], []
    for name, arr in items:
        a = np.ascontiguousarray(arr, dtype=np.float64)
        if a.shape != (dates, instruments):
            raise ValueError("all panel fields must share the same [dates, instruments] shape")
        names.append(name)
        cols.append(a.reshape(-1).tolist())  # date-major flat
    p.field_names = names
    p.field_columns = cols
    p.seed_exprs = list(seed_exprs)
    p.panel_fields = list(panel_fields) if panel_fields is not None else list(names)
    p.master_seed = int(master_seed)
    p.population = int(population)
    p.generations = int(generations)
    p.elites = int(elites)
    p.k_tournament = int(k_tournament)
    p.p_cross = float(p_cross)
    p.novelty_w = float(novelty_w)
    p.max_lookback = int(max_lookback)
    p.trial_count = int(trial_count)
    p.min_dsr = float(min_dsr)
    p.book_size = float(book_size)
    p.min_sharpe = float(min_sharpe)
    p.min_fitness = float(min_fitness)
    p.max_turnover = float(max_turnover)
    p.max_pool_corr = float(max_pool_corr)
    return _core.mine_alphas(p)


def combine_pool(pool, fit_begin=0, fit_end=None, *, method=None, shrinkage=-1.0,
                 weight_bound=0.10, ridge_lambda=1e-3, n_pcs=0):
    """Fit blend weights over a mined alpha pool. Returns a NumPy weight vector."""
    cfg = _core.CombinerConfig()
    if method is not None:
        cfg.method = method
    cfg.shrinkage = shrinkage
    cfg.weight_bound = weight_bound
    cfg.ridge_lambda = ridge_lambda
    cfg.n_pcs = n_pcs
    combiner = _core.AlphaCombiner(cfg)
    if fit_end is None:
        fit_end = pool.n_periods
    combo = combiner.fit(pool, int(fit_begin), int(fit_end))
    return np.asarray(combo.weights, dtype=np.float64)
