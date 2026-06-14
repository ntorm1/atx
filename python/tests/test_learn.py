import numpy as np

import atxpy


def test_elastic_net_recovers_linear_signal():
    rng = np.random.default_rng(0)
    n, p = 200, 4
    X = rng.normal(size=(n, p))
    true_beta = np.array([2.0, -1.0, 0.0, 0.5])
    y = X @ true_beta + rng.normal(scale=0.01, size=n)
    beta = atxpy.learn.elastic_net(X, y, lambda_=1e-4, alpha=0.0)  # ridge-ish
    assert beta.shape == (p,)
    # signs of the strong coefficients should match
    assert beta[0] > 0 and beta[1] < 0


def test_elastic_net_deterministic():
    rng = np.random.default_rng(1)
    X = rng.normal(size=(50, 3))
    y = rng.normal(size=50)
    a = atxpy.learn.elastic_net(X, y)
    b = atxpy.learn.elastic_net(X, y)
    assert np.array_equal(a, b)


def test_fit_linear_predicts():
    # Synthetic panel: 30 dates x 4 instruments, 2 features, 1-step fwd-return label.
    rng = np.random.default_rng(2)
    n_dates, n_inst, n_feat = 30, 4, 2
    rows_X, rows_date, rows_inst, label = [], [], [], []
    for d in range(n_dates):
        for i in range(n_inst):
            feats = rng.normal(size=n_feat)
            rows_X.append(feats)
            rows_date.append(d)
            rows_inst.append(i)
            # label correlated with feature 0 (a learnable edge)
            label.append(0.8 * feats[0] + rng.normal(scale=0.1))
    X = np.array(rows_X)
    fm = atxpy.learn.make_feature_matrix(
        X, {1: np.array(label)}, rows_date, rows_inst,
        n_dates=n_dates, n_instruments=n_inst,
    )
    assert fm.n_rows() == n_dates * n_inst
    model = atxpy.learn.fit_linear(fm, horizons=(1,), n_groups=5, n_test_groups=1)
    assert list(model.horizons) == [1]
    pred = atxpy.learn.predict_at(model, fm, 10)
    assert pred.shape[0] >= 1
    assert np.all(np.isfinite(pred))
