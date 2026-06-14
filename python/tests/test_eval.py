import numpy as np

import atxpy
from atxpy import _core


def test_return_metrics_basic():
    pnl = [0.0, 0.01, -0.005, 0.02, 0.0, 0.015]
    m = atxpy.return_metrics(pnl)
    assert np.isfinite(m.sharpe)
    assert -1.0 <= m.max_dd <= 1.0
    assert 0.0 <= m.hit_rate <= 1.0


def test_performance_from_backtest_result():
    import pandas as pd
    rows = []
    for s, sym in enumerate(["AAA", "BBB"]):
        for i in range(10):
            rows.append(dict(symbol=sym, timestamp=i * 86_400_000_000_000,
                             open=100., high=101., low=99.,
                             close=100. + (i if s == 0 else -i), volume=1e6))
    res = atxpy.run_backtest(pd.DataFrame(rows), symbols=["AAA", "BBB"],
                             alpha="rank(delta(close, 1))")
    m = atxpy.performance(res)
    assert np.isfinite(m.sharpe)


def test_deflated_sharpe_monotone_in_trials():
    d1 = atxpy.eval.deflated_sharpe(0.10, 250, n_trials=1)
    d100 = atxpy.eval.deflated_sharpe(0.10, 250, n_trials=100)
    # More trials -> higher selection bar -> lower (or equal) deflated sharpe.
    assert d100.dsr <= d1.dsr + 1e-9
    assert d100.sr_star >= d1.sr_star - 1e-9


def test_stats_helpers():
    r = [1.0, 2.0, 3.0, 4.0]
    ms = _core.mean_std_pop(r)
    assert abs(ms.mean - 2.5) < 1e-12
    assert _core.median(r) == 2.5
    rets = _core.returns_from_equity([100.0, 110.0, 99.0])
    assert len(rets) == 2
    assert abs(rets[0] - 0.10) < 1e-12


def test_cpcv_folds():
    spans = [(i, i + 1) for i in range(12)]
    folds = atxpy.eval.cpcv_folds(spans, n_groups=4, n_test_groups=2)
    assert len(folds) == 6  # C(4,2)
    assert all(len(f.test_idx) > 0 for f in folds)
