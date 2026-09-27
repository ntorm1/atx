"""Synthetic fixtures for statarb_cluster_study numerics (no real data)."""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import statarb_cluster_study as S  # noqa: E402


def test_rsum_alignment_and_min_finite():
    a = np.arange(1.0, 11.0)[:, None]
    a[4, 0] = np.nan
    out = S.rsum(a, 3, 3)
    assert np.isnan(out[:2, 0]).all()
    assert out[2, 0] == 1 + 2 + 3
    assert np.isnan(out[4, 0]) and np.isnan(out[6, 0])  # windows containing the NaN have only 2 finite rows
    assert out[7, 0] == 6 + 7 + 8
    assert S.rsum(a, 3, 2)[4, 0] == 3 + 4


def test_balanced_kmeans_recovers_planted_groups_with_capacity():
    rng = np.random.default_rng(1)
    centres = rng.normal(size=(12, 8))
    truth = np.repeat(np.arange(12), 25)
    emb = centres[truth] + 0.15 * rng.normal(size=(truth.size, 8))
    emb /= np.linalg.norm(emb, axis=1, keepdims=True)
    lab = S.balanced_kmeans(emb, 25, np.random.default_rng(0))
    sizes = np.bincount(lab)
    assert sizes.size == 12 and sizes.max() <= 25
    from sklearn.metrics import adjusted_rand_score
    assert adjusted_rand_score(truth, lab) > 0.95


def test_ou_stock_sign_and_group_beta():
    rng = np.random.default_rng(2)
    t, n = 60, 400
    grp = rng.normal(0, 0.01, (t, 1)) * np.ones((1, n))
    x = np.zeros((t, n))
    for i in range(1, t):  # OU residual level, b = 0.8
        x[i] = 0.8 * x[i - 1] + rng.normal(0, 0.01, n)
    shock = np.linspace(-0.05, 0.05, n)
    x[-1] += shock  # last-day residual displacement: higher level => higher s-score
    y = 1.3 * grp + np.diff(np.vstack([np.zeros((1, n)), x]), axis=0)
    s, kap, beta, coh, sd = S.ou_stock(y, grp)
    assert np.nanmedian(np.abs(beta - 1.3)) < 0.35
    assert np.isfinite(s).mean() > 0.9 and np.nanmedian(kap) > 5
    assert np.corrcoef(np.nan_to_num(s), shock)[0, 1] > 0.5


def test_gauss_and_nw_t():
    g = S.gauss(np.array([3.0, np.nan, 1.0, 2.0, 2.0]))
    assert np.isnan(g[1]) and g[2] < g[3] == g[4] < g[0] and abs(np.nansum(g)) < 1e-12
    rng = np.random.default_rng(3)
    assert abs(S.nw_t(rng.normal(0, 1, 2000), 5)) < 4
    assert S.nw_t(rng.normal(0.2, 1, 2000), 5) > 5
