import numpy as np

from _helpers import one_symbol_params, two_symbol_params

from atxpy import _core


def test_run_returns_result_shape():
    p = one_symbol_params(5)
    r = _core.run_backtest(p)
    assert r.slices == 5
    assert r.rebalances == 5
    ts, eq, gross, net = r.equity_columns()
    assert isinstance(eq, np.ndarray)
    assert eq.shape == (5,)
    assert ts.dtype == np.int64
    assert eq.dtype == np.float64
    assert np.all(np.isfinite(eq))
    assert eq[0] > 0.0


def test_fills_columns_present():
    # Two names long/short -> non-zero cross-sectional weights -> real trades.
    p = two_symbol_params(5)
    r = _core.run_backtest(p)
    fid, fqty, fprice, ffee, fimpact, ft = r.fills_columns()
    assert fid.dtype == np.int64
    assert fqty.dtype == np.int64
    assert fprice.dtype == np.float64
    assert fqty.size >= 1
    assert set(fid.tolist()) <= {0, 1}  # symbol ids map to universe columns
    assert np.all(fprice > 0.0)          # fills priced at the bar
    assert np.all(ffee >= 0.0)           # commission is never negative


def test_single_name_is_flat():
    # A lone instrument cannot be dollar-neutral with non-zero weight -> no fills.
    r = _core.run_backtest(one_symbol_params(5))
    _, fqty, *_ = r.fills_columns()
    assert fqty.size == 0


def test_max_lookback_dispatch_large():
    # Requesting a deep lookback selects a larger precompiled Cap; must still run.
    p = one_symbol_params(5)
    p.max_lookback = 100  # -> Cap 128
    r = _core.run_backtest(p)
    assert r.slices == 5
