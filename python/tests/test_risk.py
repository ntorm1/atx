import numpy as np

import atxpy


def _simple_model(m=3, k=1):
    # M instruments, K factors. X exposures, F factor cov (SPD), D specific var.
    X = np.array([[1.0], [0.5], [-2.0]])[:m]
    F = np.array([[0.04]])[:k, :k]
    D = np.array([0.10, 0.20, 0.05])[:m]
    return atxpy.factor_model(X, F, D, fit_begin=0, fit_end=10)


def test_factor_model_shapes_and_risk():
    fm = _simple_model()
    assert fm.n_instruments == 3
    assert fm.n_factors == 1
    w = [1.0, 0.0, 0.0]
    risk = fm.risk(w)
    assert risk > 0.0  # variance of a single long name is positive


def test_optimize_dollar_neutral_and_leverage():
    fm = _simple_model()
    alpha = [0.02, -0.01, 0.03]
    w = atxpy.optimize(alpha, fm, risk_aversion=0.0, gross_leverage=1.0,
                       name_cap=10.0, dollar_neutral=True)
    assert w.shape == (3,)
    assert abs(np.sum(w)) < 1e-6        # dollar-neutral: sum ~ 0
    assert abs(np.sum(np.abs(w)) - 1.0) < 1e-6  # gross ~ leverage


def test_optimize_determinism():
    fm = _simple_model()
    alpha = [0.02, -0.01, 0.03]
    a = atxpy.optimize(alpha, fm)
    b = atxpy.optimize(alpha, fm)
    assert np.array_equal(a, b)
