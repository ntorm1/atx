import numpy as np
import pandas as pd

import atxpy


def test_evaluate_facade_dict():
    closes = np.array([[10., 20., 30.], [11., 19., 33.], [12., 25., 31.]])
    out = atxpy.evaluate_alpha("rank(close)", {"close": closes})
    assert out.shape == (3, 3)
    assert np.allclose(out[-1], np.array([0.0, 0.5, 1.0]))


def test_run_backtest_with_alpha():
    rows = []
    for s, sym in enumerate(["AAA", "BBB"]):
        for i in range(8):
            rows.append(dict(symbol=sym, timestamp=i * 86_400_000_000_000,
                             open=100., high=101., low=99.,
                             close=100. + (i if s == 0 else -i), volume=1e6))
    bars = pd.DataFrame(rows)
    res = atxpy.run_backtest(bars, symbols=["AAA", "BBB"],
                             alpha="rank(delta(close, 1))")
    assert res.slices == 8
    assert len(res.fills) >= 1


def test_run_backtest_requires_signals_or_alpha():
    bars = pd.DataFrame([dict(symbol="AAA", timestamp=0, open=1., high=1., low=1.,
                              close=1., volume=1.)])
    try:
        atxpy.run_backtest(bars, symbols=["AAA"])
        assert False, "expected ValueError"
    except ValueError:
        pass
