import numpy as np

import atxpy
from atxpy import _core


def _momentum_panel(dates=120, instruments=8, seed=0xA11C):
    # Deterministic pseudo-random close + a reversal field, shaped so some
    # cross-sectional alphas clear the gate.
    rng = np.random.default_rng(seed)
    steps = rng.normal(0.0, 1.0, size=(dates, instruments))
    close = 100.0 + np.cumsum(steps, axis=0)
    rev = -steps
    return {"close": close, "rev": rev}


def test_mine_runs_and_reports():
    panel = _momentum_panel()
    res = atxpy.mine_alphas(
        panel,
        seed_exprs=["rank(close)", "rank(rev)", "ts_mean(close, 5)", "delta(close, 1)"],
        panel_fields=["close", "rev"],
        master_seed=1, population=12, generations=3, trial_count=4,
    )
    assert res.report.evaluated > 0
    assert res.report.trials == res.report.evaluated
    assert res.pool.n_alphas == res.report.admitted
    # digest is a stable run fingerprint
    assert isinstance(res.report.digest, int)


def test_mine_deterministic():
    panel = _momentum_panel()
    kw = dict(seed_exprs=["rank(close)", "rank(rev)"], panel_fields=["close", "rev"],
              master_seed=7, population=10, generations=2, trial_count=4)
    a = atxpy.mine_alphas(panel, **kw)
    b = atxpy.mine_alphas(panel, **kw)
    assert a.report.digest == b.report.digest
    assert a.report.admitted == b.report.admitted


def test_pool_pnl_and_metrics_accessible():
    panel = _momentum_panel()
    res = atxpy.mine_alphas(panel, seed_exprs=["rank(close)", "rank(rev)"],
                            panel_fields=["close", "rev"], master_seed=3,
                            population=12, generations=3, trial_count=4,
                            min_sharpe=0.0, min_fitness=0.0)  # relax gate to admit some
    if res.pool.n_alphas > 0:
        pnl = res.pool.pnl(0)
        assert pnl.shape == (res.pool.n_periods,)
        mt = res.pool.metrics(0)
        assert np.isfinite(mt.sharpe) or np.isnan(mt.sharpe)


def test_combine_pool_weights():
    panel = _momentum_panel()
    res = atxpy.mine_alphas(panel, seed_exprs=["rank(close)", "rank(rev)", "delta(close,1)"],
                            panel_fields=["close", "rev"], master_seed=5,
                            population=12, generations=3, trial_count=4,
                            min_sharpe=0.0, min_fitness=0.0)
    n = res.pool.n_alphas
    if n >= 2:
        w = atxpy.combine_pool(res.pool, method=_core.CombineMethod.EqualWeight)
        assert w.shape == (n,)
        assert np.allclose(w, 1.0 / n)
