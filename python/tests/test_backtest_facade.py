import numpy as np
import pandas as pd

import atxpy


def _bars_df(symbols, n=5):
    rows = []
    for s_idx, sym in enumerate(symbols):
        for i in range(n):
            rows.append(
                dict(symbol=sym, timestamp=i * 86_400_000_000_000,
                     open=100.0, high=101.0, low=99.0,
                     close=100.0 + i + s_idx, volume=1_000_000.0)
            )
    return pd.DataFrame(rows)


def test_facade_runs_and_returns_frames():
    syms = ["AAA", "BBB"]
    bars = _bars_df(syms, 5)
    signals = np.tile([1.0, -1.0], (5, 1))  # long AAA / short BBB each rebalance
    res = atxpy.run_backtest(bars, signals, symbols=syms, starting_cash="1000000")
    assert isinstance(res.equity_curve, pd.DataFrame)
    assert list(res.equity_curve.columns) == ["timestamp", "equity", "gross", "net"]
    assert len(res.equity_curve) == 5
    assert isinstance(res.fills, pd.DataFrame)
    assert {"symbol", "qty", "price", "fee", "impact", "timestamp"} <= set(res.fills.columns)
    assert res.slices == 5
    # symbols in fills come back as names from the universe
    assert set(res.fills["symbol"]) <= set(syms)


def test_facade_determinism():
    syms = ["AAA", "BBB"]
    bars = _bars_df(syms, 8)
    signals = np.tile([1.0, -1.0], (8, 1))
    a = atxpy.run_backtest(bars, signals, symbols=syms)
    b = atxpy.run_backtest(bars, signals, symbols=syms)
    assert a.final_cash.raw() == b.final_cash.raw()
    assert np.array_equal(a.equity_curve["equity"].to_numpy(),
                          b.equity_curve["equity"].to_numpy())
