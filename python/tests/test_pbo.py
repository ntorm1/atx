"""Tests for atxpy.pbo — the CSCV probability-of-backtest-overfitting harness.

Pure numpy/pandas: no engine bindings, no atxpy.tracks/D4 lakehouse loader
involved. `cscv_pbo` takes a plain T x N daily-returns DataFrame.
"""

from __future__ import annotations

import math

import numpy as np
import pandas as pd

from atxpy.pbo import PboResult, cscv_pbo

N_BLOCKS = 16
T_ROWS = 960  # multiple of N_BLOCKS -> equal-size blocks
N_CONFIGS = 20


def _noise_returns(seed: int = 0, t=T_ROWS, n=N_CONFIGS) -> pd.DataFrame:
    rng = np.random.default_rng(seed)
    data = rng.normal(loc=0.0, scale=1.0, size=(t, n))
    return pd.DataFrame(data, columns=[f"cfg{i}" for i in range(n)])


def _one_dominant_returns(seed: int = 0, t=T_ROWS, n=N_CONFIGS, shift=0.5) -> pd.DataFrame:
    rng = np.random.default_rng(seed)
    data = rng.normal(loc=0.0, scale=1.0, size=(t, n))
    data[:, 0] += shift  # config 0 genuinely, persistently dominates
    return pd.DataFrame(data, columns=[f"cfg{i}" for i in range(n)])


NOISE_SEEDS = list(range(10))


def test_pure_noise_mean_pbo_near_half():
    # PBO on a *single* iid-noise realization is a rank statistic over only
    # N_CONFIGS items; unlike a mean, its sampling variance does not shrink
    # with T (confirmed empirically: T=960 vs T=76800 give the same spread,
    # since it's fundamentally about ranking N items, not estimating a
    # mean) -- individual seeds land anywhere from ~0.15 to ~0.97 even
    # though the estimator is unbiased around 0.5 on average. A single
    # hand-picked seed can't distinguish a correct implementation from a
    # moderately-biased one, so average PBO over NOISE_SEEDS independent
    # draws instead: this cuts the statistic's spread by roughly sqrt(10)
    # while keeping the brief's exact [0.35, 0.65] band, and stays fast
    # since the vectorized 12870-split path is ~0.1-0.2s per draw.
    pbos = [cscv_pbo(_noise_returns(seed=s), n_blocks=N_BLOCKS).pbo for s in NOISE_SEEDS]
    mean_pbo = sum(pbos) / len(pbos)
    assert 0.35 <= mean_pbo <= 0.65


def test_pure_noise_deterministic_for_fixed_seed():
    # Same input -> bit-identical output (no algorithmic randomness beyond
    # the caller-supplied data). Kept separate from the band assertion
    # above, which is inherently a statistical (multi-seed) claim.
    returns = _noise_returns(seed=0)
    a = cscv_pbo(returns, n_blocks=N_BLOCKS)
    b = cscv_pbo(returns, n_blocks=N_BLOCKS)
    assert isinstance(a, PboResult)
    assert a.pbo == b.pbo
    assert a.lambdas == b.lambdas


def test_one_dominant_config_low_pbo():
    returns = _one_dominant_returns(seed=7)
    result = cscv_pbo(returns, n_blocks=N_BLOCKS)
    assert result.pbo < 0.1


def test_lambda_count_matches_c_16_8():
    returns = _noise_returns(seed=1, t=64, n=5)
    result = cscv_pbo(returns, n_blocks=N_BLOCKS)
    assert len(result.lambdas) == math.comb(16, 8) == 12870


def test_lambdas_all_finite():
    # omega is deliberately clamped away from {0, 1} (rank/(N+1)) so every
    # lambda = ln(omega/(1-omega)) must stay finite, never +-inf/nan.
    returns = _noise_returns(seed=2, t=128, n=4)
    result = cscv_pbo(returns, n_blocks=N_BLOCKS)
    lambdas = np.asarray(result.lambdas)
    assert np.all(np.isfinite(lambdas))


def test_result_fields_present_and_typed():
    returns = _noise_returns(seed=3, t=64, n=5)
    result = cscv_pbo(returns, n_blocks=N_BLOCKS)
    assert isinstance(result.pbo, float)
    assert isinstance(result.degradation_slope, float)
    assert isinstance(result.p_oos_loss, float)
    assert 0.0 <= result.pbo <= 1.0
    assert 0.0 <= result.p_oos_loss <= 1.0


def test_default_n_blocks_is_16():
    returns = _noise_returns(seed=4, t=64, n=5)
    result = cscv_pbo(returns)  # no n_blocks passed
    assert len(result.lambdas) == math.comb(16, 8)


def test_rejects_odd_n_blocks():
    returns = _noise_returns(seed=5, t=64, n=5)
    try:
        cscv_pbo(returns, n_blocks=15)
    except ValueError:
        pass
    else:
        raise AssertionError("expected ValueError for odd n_blocks")
