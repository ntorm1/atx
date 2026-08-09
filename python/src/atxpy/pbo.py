"""CSCV probability-of-backtest-overfitting (PBO) harness.

Pure numpy/pandas implementation of Combinatorially Symmetric Cross-Validation
(CSCV) per Bailey, Borwein, Lopez de Prado & Zhu, "The Probability of Backtest
Overfitting" (2017). Deliberately standalone: it consumes a plain T x N
daily-returns DataFrame and has no dependency on the C++ engine bindings
(`atxpy._core`) or on any lakehouse/track-loading facility.

Algorithm
---------
1. Split the T rows into S = ``n_blocks`` contiguous row-blocks.
2. Enumerate every size-(S/2) subset of blocks via ``itertools.combinations``
   -- there are C(S, S/2) of them. Each subset is one train (in-sample, IS)
   selection; its complement is the paired test (out-of-sample, OOS)
   selection. Because a subset and its complement are both size S/2, both
   appear as separate entries in the full enumeration, so iterating over all
   C(S, S/2) subsets automatically covers every symmetric train/test split
   without double-processing -- this is exactly the CSCV construction.
3. Per split: pick the IS winner (highest in-sample Sharpe among the N
   configs), then find where that winner's OOS Sharpe ranks among the N OOS
   Sharpes. Map the rank to a relative rank omega in (0, 1) and then to a
   logit lambda = ln(omega / (1 - omega)).
4. PBO is the fraction of splits with lambda <= 0, i.e. the fraction of
   splits where the IS-selected config performs at or below the OOS median.

Rank convention (omega)
------------------------
Ranks are 1-based and ascending (rank 1 = worst OOS Sharpe, rank N = best),
using the standard average-rank tie treatment (a group of k tied values each
gets the mean of the k rank slots they jointly occupy). omega is then
``rank / (N + 1)`` rather than ``rank / N``: dividing by N would let the
best-ranked config hit omega = 1 and the worst hit omega -> 0 in the
zero-tie case, sending lambda = ln(omega/(1-omega)) to +/-inf. Dividing by
(N + 1) instead keeps omega strictly inside (1/(N+1), N/(N+1)) subset of
(0, 1) for every possible rank in {1, ..., N}, so lambda is always finite by
construction -- no clipping needed.

Vectorization
-------------
Per-config sums and sums-of-squares are precomputed once per block (S x N
each). Every split's IS/OOS mean and variance are then obtained by
contracting an (n_splits x S) 0/1 block-membership indicator against those
(S x N) block aggregates (matrix multiply) -- O(N) work per split from block
aggregates, not O(T x N). All C(S, S/2) splits are evaluated together via a
handful of vectorized numpy array ops.
"""

from __future__ import annotations

import itertools
from dataclasses import dataclass, field

import numpy as np
import pandas as pd

_EPS = 1e-12


@dataclass
class PboResult:
    """Result of a CSCV probability-of-backtest-overfitting run."""

    pbo: float
    degradation_slope: float
    p_oos_loss: float
    lambdas: list[float] = field(default_factory=list)


def cscv_pbo(returns: pd.DataFrame, n_blocks: int = 16) -> PboResult:
    """Combinatorially Symmetric Cross-Validation probability of overfitting.

    Parameters
    ----------
    returns:
        T x N DataFrame of per-period returns, one column per candidate
        configuration, rows in time order.
    n_blocks:
        S, the number of contiguous row-blocks to split `returns` into.
        Must be a positive even integer (splits are size S/2). Defaults to
        16, giving C(16, 8) = 12870 train/test splits.

    Returns
    -------
    PboResult(pbo, degradation_slope, p_oos_loss, lambdas)
        - pbo: fraction of splits with lambda <= 0 (IS winner performs at or
          below the OOS median) -- the probability of backtest overfitting.
        - degradation_slope: OLS slope of (OOS Sharpe of the IS winner) on
          (IS Sharpe of the IS winner) across all splits. Because IS and OOS
          are *complementary* blocks of one fixed dataset (IS_sum + OOS_sum
          is constant per config), this slope is mechanically pulled toward
          -1 regardless of genuine skill -- it is not a "near +1 = good"
          signal. What it usefully separates is *how tightly* that negative
          relationship holds: a single durably-dominant config wins nearly
          every split and its winner-pairs trace one clean near -1 line
          (e.g. slope ~= -0.997, low split-to-split scatter); pure noise
          hands the win to a *different* config most splits, so the pooled
          winner-pairs are a noisier mix of those per-config lines (still
          typically negative, but shallower and far more seed-dependent,
          e.g. -0.2 to -0.9 across reruns). Use it comparatively (vs. a
          null/shuffled baseline), not against a fixed sign threshold.
        - p_oos_loss: fraction of splits where the IS winner's OOS Sharpe is
          negative.
        - lambdas: the length-C(S, S/2) list of per-split logits.
    """
    if n_blocks < 2 or n_blocks % 2 != 0:
        raise ValueError(f"n_blocks must be a positive even integer, got {n_blocks}")

    values = np.asarray(returns, dtype=np.float64)
    if values.ndim != 2:
        raise ValueError("returns must be a 2D (T x N) DataFrame/array")
    t_rows, n_configs = values.shape
    if t_rows < n_blocks:
        raise ValueError(f"need at least n_blocks={n_blocks} rows, got T={t_rows}")
    if n_configs < 2:
        raise ValueError("need at least 2 candidate configs (columns) to rank")

    row_blocks = np.array_split(np.arange(t_rows), n_blocks)
    block_len = np.array([len(b) for b in row_blocks], dtype=np.float64)  # (S,)
    block_sum = np.stack([values[b].sum(axis=0) for b in row_blocks])  # (S, N)
    block_sumsq = np.stack([np.square(values[b]).sum(axis=0) for b in row_blocks])  # (S, N)

    half = n_blocks // 2
    combos = list(itertools.combinations(range(n_blocks), half))
    n_splits = len(combos)  # C(n_blocks, half)

    indicator = np.zeros((n_splits, n_blocks), dtype=np.float64)
    for i, combo in enumerate(combos):
        indicator[i, combo] = 1.0

    total_len = block_len.sum()
    total_sum = block_sum.sum(axis=0)
    total_sumsq = block_sumsq.sum(axis=0)

    is_n = indicator @ block_len  # (n_splits,)
    is_sum = indicator @ block_sum  # (n_splits, N)
    is_sumsq = indicator @ block_sumsq  # (n_splits, N)

    oos_n = total_len - is_n
    oos_sum = total_sum - is_sum
    oos_sumsq = total_sumsq - is_sumsq

    is_sharpe = _sharpe_from_moments(is_sum, is_sumsq, is_n)
    oos_sharpe = _sharpe_from_moments(oos_sum, oos_sumsq, oos_n)

    winner = np.argmax(is_sharpe, axis=1)  # (n_splits,)
    rows = np.arange(n_splits)
    is_winner_sharpe = is_sharpe[rows, winner]
    oos_winner_sharpe = oos_sharpe[rows, winner]

    # Average-rank tie handling: rank = (# strictly less) + (# equal + 1) / 2.
    n_less = (oos_sharpe < oos_winner_sharpe[:, None]).sum(axis=1)
    n_equal = (oos_sharpe == oos_winner_sharpe[:, None]).sum(axis=1)  # includes self
    rank = n_less + (n_equal + 1) / 2.0
    omega = rank / (n_configs + 1)  # strictly in (0, 1) -> lambda always finite
    lam = np.log(omega / (1.0 - omega))

    pbo = float(np.mean(lam <= 0.0))
    p_oos_loss = float(np.mean(oos_winner_sharpe < 0.0))

    is_var = np.var(is_winner_sharpe)
    if is_var > _EPS:
        slope = float(np.cov(is_winner_sharpe, oos_winner_sharpe, ddof=0)[0, 1] / is_var)
    else:
        slope = 0.0

    return PboResult(pbo=pbo, degradation_slope=slope, p_oos_loss=p_oos_loss, lambdas=lam.tolist())


def _sharpe_from_moments(sums: np.ndarray, sumsqs: np.ndarray, n: np.ndarray) -> np.ndarray:
    """Per-column Sharpe (mean / population std, ddof=0) from block-aggregated moments.

    `sums`, `sumsqs` are (n_splits, N); `n` is (n_splits,) row counts per split.
    Population (ddof=0) variance is used deliberately: it needs only the
    block-level sum/sumsq/n aggregates (no per-row second pass), matches
    across IS/IOS halves that may differ by one row's worth of blocks when T
    isn't evenly divisible by n_blocks, and never risks an n-1=0 division.
    The ddof convention is a fixed scale factor shared by every config in a
    given split, so it cannot change the argmax (IS winner selection) or the
    rank of the OOS winner -- only the raw Sharpe magnitude, which this
    module does not otherwise expose.
    """
    n_col = n[:, None]
    mean = sums / n_col
    var = sumsqs / n_col - mean * mean
    var = np.clip(var, a_min=0.0, a_max=None)  # guard fp round-off near 0
    std = np.sqrt(var)
    with np.errstate(divide="ignore", invalid="ignore"):
        sharpe = np.where(std > _EPS, mean / std, 0.0)
    return sharpe
