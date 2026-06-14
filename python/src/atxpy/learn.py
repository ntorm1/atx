"""Facade for ML alphas: elastic-net regression + linear ML-alpha training."""

from __future__ import annotations

import numpy as np

from . import _core


def elastic_net(X, y, *, lambda_=0.02, alpha=0.5, max_iter=1000, tol=1e-8):
    """Deterministic elastic-net regression. X (n x p), y (n) -> coefficients (p)."""
    cfg = _core.ElasticNetCfg()
    cfg.lambda_ = lambda_
    cfg.alpha = alpha
    cfg.max_iter = max_iter
    cfg.tol = tol
    X = np.ascontiguousarray(X, dtype=np.float64)
    y = np.ascontiguousarray(y, dtype=np.float64).ravel()
    return np.asarray(_core.elastic_net(X, y, cfg), dtype=np.float64)


def make_feature_matrix(X, Y, row_date, row_inst, *, n_dates, n_instruments, row_valid=None):
    """Build a _core.FeatureMatrix.

    X: (n_rows x n_features) float64. Y: dict {horizon: (n_rows,)} or list of label
    columns aligned to a horizons list. row_date / row_inst: (n_rows,) int indices.
    """
    X = np.ascontiguousarray(X, dtype=np.float64)
    n_rows, n_features = X.shape
    fm = _core.FeatureMatrix()
    fm.n_dates = int(n_dates)
    fm.n_instruments = int(n_instruments)
    fm.n_features = int(n_features)
    fm.row_date = np.asarray(row_date, dtype=np.uint64).tolist()
    fm.row_inst = np.asarray(row_inst, dtype=np.uint64).tolist()
    fm.X = X.reshape(-1).tolist()
    if isinstance(Y, dict):
        cols = [np.asarray(Y[h], dtype=np.float64).tolist() for h in sorted(Y)]
    else:
        cols = [np.asarray(c, dtype=np.float64).tolist() for c in Y]
    fm.Y = cols
    if row_valid is None:
        row_valid = np.ones(n_rows, dtype=np.uint8)
    fm.row_valid = np.asarray(row_valid, dtype=np.uint8).tolist()
    return fm


def fit_linear(features, *, horizons=(1,), lambda_=0.02, alpha=0.5, master_seed=12345,
               n_groups=5, n_test_groups=1, embargo=1.0, use_ridge_baseline=False):
    """Fit an elastic-net ML alpha over a FeatureMatrix with CPCV horizon blending."""
    cfg = _core.LinearAlphaCfg()
    cfg.en.lambda_ = lambda_
    cfg.en.alpha = alpha
    cfg.use_ridge_baseline = use_ridge_baseline
    cfg.master_seed = master_seed
    cfg.horizons = [int(h) for h in horizons]
    cfg.cpcv.n_groups = n_groups
    cfg.cpcv.n_test_groups = n_test_groups
    cfg.cpcv.embargo = embargo
    aug = _core.LatentAugmentation()
    return _core.fit_linear(features, aug, cfg)


def predict_at(model, features, date):
    """Horizon-blended prediction for in-universe instruments at `date`."""
    return np.asarray(_core.predict_at(model, features, int(date)), dtype=np.float64)
