import numpy as np

from _helpers import one_symbol_params

from atxpy import _core


def test_byte_identical_repeat():
    p = one_symbol_params(10)
    a = _core.run_backtest(p)
    b = _core.run_backtest(p)
    ta, ea, ga, na = a.equity_columns()
    tb, eb, gb, nb = b.equity_columns()
    assert np.array_equal(ta, tb)
    assert np.array_equal(ea, eb)  # bit-identical equity
    assert a.final_cash.raw() == b.final_cash.raw()
    for ca, cb in zip(a.fills_columns(), b.fills_columns()):
        assert np.array_equal(ca, cb)


def test_no_look_ahead_prefix():
    # Truncating the feed after slice t leaves equity at <= t byte-identical.
    rf = _core.run_backtest(one_symbol_params(10))
    rs = _core.run_backtest(one_symbol_params(6))
    _, ef, _, _ = rf.equity_columns()
    _, es, _, _ = rs.equity_columns()
    assert es.shape == (6,)
    assert np.array_equal(ef[:6], es)  # the future is invisible to the past
