import numpy as np

import atxpy
from atxpy import _core


def _sleeve_pnls(s=3, t=100, seed=0):
    rng = np.random.default_rng(seed)
    # Three sleeves with different vols, mildly correlated.
    base = rng.normal(size=(t,))
    out = []
    for k in range(s):
        out.append(0.5 * base + rng.normal(scale=0.5 + 0.5 * k, size=t))
    return out


def test_sleeve_cov_shape_symmetric():
    pnls = _sleeve_pnls(3, 120)
    omega = atxpy.fund.sleeve_cov(pnls)
    assert omega.shape == (3, 3)
    assert np.allclose(omega, omega.T)
    assert np.all(np.diag(omega) > 0)


def test_meta_allocate_within_gross_cap():
    pnls = _sleeve_pnls(3, 120)
    omega = atxpy.fund.sleeve_cov(pnls)
    c = atxpy.fund.meta_allocate(omega, max_gross=2.0,
                                 method=_core.RiskBudgetMethod.EqualRiskContribution)
    assert c.shape == (3,)
    assert np.all(c >= -1e-9)               # non-negative capital
    assert np.sum(np.abs(c)) <= 2.0 + 1e-6  # within gross cap


def test_meta_allocate_deterministic():
    pnls = _sleeve_pnls(3, 120, seed=4)
    omega = atxpy.fund.sleeve_cov(pnls)
    a = atxpy.fund.meta_allocate(omega)
    b = atxpy.fund.meta_allocate(omega)
    assert np.array_equal(a, b)
