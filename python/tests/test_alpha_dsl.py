import numpy as np
import pytest

from atxpy import _core


def test_compile_alpha_simple():
    prog = _core.compile_alpha("rank(close)")
    assert prog.required_lookback == 0
    assert prog.num_slots >= 1
    assert "close" in prog.fields


def test_compile_alpha_timeseries_lookback():
    prog = _core.compile_alpha("delta(close, 5)")
    assert prog.required_lookback >= 5


def test_parse_error_raises():
    with pytest.raises(_core.AtxError):
        _core.compile_alpha("close +")  # malformed


def test_pipeline_steps():
    lib = _core.Library()
    ast = _core.parse_expr("rank(close)", lib)
    analysis = _core.analyze(ast)
    assert analysis.required_lookback == 0
    prog = _core.compile(ast, analysis)
    assert "close" in prog.fields


def test_evaluate_rank_close():
    # 3 dates x 3 instruments; rank(close) on the last date -> 0, 0.5, 1
    closes = np.array([[10., 20., 30.],
                       [11., 19., 33.],
                       [12., 25., 31.]], dtype=np.float64)
    prog = _core.compile_alpha("rank(close)")
    out = _core.evaluate_program(prog, {"close": closes})
    assert out.shape == (3, 3)
    last = out[-1]
    # 12 < 25 < 31 -> normalized ranks 0, 0.5, 1
    assert np.allclose(last, np.array([0.0, 0.5, 1.0]))
