// combine_combiner_test.cpp — P4-4: AlphaCombiner (progressive blend-weight fit).
//
// AlphaCombiner::fit(pool, fit_begin, fit_end) fits per-alpha blend weights over
// the gated pool on the trailing window [fit_begin, fit_end), via one of five
// §5.3 methods (default ShrinkageMv). The fit/apply firewall (§3.1) is the central
// rail: fit() reads ONLY the row sub-span [fit_begin, fit_end) of each alpha's PnL,
// so rows >= fit_end are provably invisible (the truncation-invariance test pins
// this). Combination carries [fit_begin, fit_end) for the apply-side firewall
// (enforced by P4-5, not here).
//
// Coverage (plan §8 P4-4):
//   * EqualWeight        -> uniform 1/N (N=3 => 1/3 each)
//   * RankAverage        -> uniform 1/N (the METHOD flag, not the weights, differs)
//   * IcWeighted         -> w_i ∝ max(window-sharpe_i, 0), Σw=1 (hand ratio)
//   * ShrinkageMv identical alphas -> equal weights, Σ|w|=1
//   * ShrinkageMv anti-correlated equal-Sharpe -> equal weights (risk-balanced)
//   * ShrinkageMv T<N (singular S) -> finite weights, Σ|w|≈1 (shrinkage rescues)
//   * BoundedRegression respects weight_bound (max|w_i| <= bound + tol)
//   * fit-window truncation-invariance (weights on [a,b) byte-identical after rows>=b mutate)
//   * single alpha -> w=[1]
//   * empty pool -> Err
//   * T<2 (fit_begin==fit_end or diff 1) -> Err

#include <cmath>  // std::isnan, std::sqrt, std::abs
#include <limits> // std::numeric_limits
#include <span>   // std::span
#include <vector> // std::vector (fixture storage)

#include <gtest/gtest.h>

#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/combine/combiner.hpp" // CombineMethod, CombinerConfig, Combination, AlphaCombiner
#include "atx/engine/combine/metrics.hpp" // AlphaMetrics
#include "atx/engine/combine/store.hpp"   // AlphaStore

namespace atxtest_combine_combiner_test {

using atx::f64;
using atx::usize;
using atx::engine::combine::AlphaCombiner;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;
using atx::engine::combine::Combination;
using atx::engine::combine::CombineMethod;
using atx::engine::combine::CombinerConfig;

// Insert one PnL row into the pool with dummy metrics + nullptr source + flat
// positions (the combiner reads only the PnL rows; metrics/positions are inert
// filler of the right length — index 0 is the AlphaStreams structural zero).
void insert_pnl(AlphaStore &pool, std::span<const f64> pnl) {
  const std::vector<f64> pos(pnl.size(), 0.0); // insts == 1 -> period-major == pnl length
  const auto r = pool.insert(/*source*/ nullptr, pnl, pos, AlphaMetrics{});
  ASSERT_TRUE(r.has_value());
}

// Sum of |w_i| — the ShrinkageMv/BoundedRegression gross-normalization target.
[[nodiscard]] f64 abs_sum(const std::vector<f64> &w) {
  f64 s = 0.0;
  for (const f64 x : w) {
    s += std::abs(x);
  }
  return s;
}

// Population sharpe (mean/std, N divisor) over a window, EXCLUDING the structural
// index 0 if it falls in the window — mirrors the combiner's IcWeighted proxy and
// P4-2's §0-F convention. Used only to hand-check the IcWeighted ratio.
[[nodiscard]] f64 window_sharpe(std::span<const f64> pnl, usize a, usize b) {
  f64 sum = 0.0;
  usize n = 0;
  for (usize t = a; t < b; ++t) {
    if (t == 0) {
      continue; // structural zero excluded
    }
    sum += pnl[t];
    ++n;
  }
  if (n == 0) {
    return 0.0;
  }
  const f64 mean = sum / static_cast<f64>(n);
  f64 ss = 0.0;
  for (usize t = a; t < b; ++t) {
    if (t == 0) {
      continue;
    }
    ss += (pnl[t] - mean) * (pnl[t] - mean);
  }
  const f64 var = ss / static_cast<f64>(n); // population
  return (var <= 0.0) ? 0.0 : mean / std::sqrt(var);
}

// ===========================================================================
//  EqualWeight / RankAverage — uniform 1/N
// ===========================================================================

TEST(AlphaCombiner, EqualWeightUniform) {
  AlphaStore pool;
  const std::vector<f64> a{0.0, 0.01, 0.02, -0.01};
  const std::vector<f64> b{0.0, -0.02, 0.03, 0.01};
  const std::vector<f64> c{0.0, 0.04, -0.01, 0.02};
  insert_pnl(pool, a);
  insert_pnl(pool, b);
  insert_pnl(pool, c);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::EqualWeight;
  const auto r = comb.fit(pool, 0, 4);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->weights.size(), 3U);
  for (const f64 w : r->weights) {
    EXPECT_NEAR(w, 1.0 / 3.0, 1e-12);
  }
  EXPECT_EQ(r->fit_begin, 0U);
  EXPECT_EQ(r->fit_end, 4U);
}

TEST(AlphaCombiner, RankAverageUniform) {
  // RankAverage returns uniform 1/N here; the rank-space combination is P4-5's job
  // (the METHOD flag differs, not the weights).
  AlphaStore pool;
  const std::vector<f64> a{0.0, 0.01, 0.02};
  const std::vector<f64> b{0.0, -0.02, 0.03};
  insert_pnl(pool, a);
  insert_pnl(pool, b);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::RankAverage;
  const auto r = comb.fit(pool, 0, 3);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->weights.size(), 2U);
  EXPECT_NEAR(r->weights[0], 0.5, 1e-12);
  EXPECT_NEAR(r->weights[1], 0.5, 1e-12);
}

// ===========================================================================
//  IcWeighted — w_i ∝ max(window-sharpe_i, 0), Σw=1
// ===========================================================================

TEST(AlphaCombiner, IcWeightedProportionalToWindowSharpe) {
  AlphaStore pool;
  // Two positive-sharpe alphas with DIFFERENT window sharpes (both with genuine
  // variance, so neither degenerates to a zero-variance/zero-sharpe stream) ->
  // weights ∝ sharpe. The window [0,5) excludes the structural index 0.
  const std::vector<f64> a{0.0, 0.03, 0.01, 0.03, 0.01}; // mean 0.02, low var
  const std::vector<f64> b{0.0, 0.05, 0.01, 0.05, 0.01}; // mean 0.03, higher var
  insert_pnl(pool, a);
  insert_pnl(pool, b);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::IcWeighted;
  const auto r = comb.fit(pool, 0, 5);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->weights.size(), 2U);

  const f64 sa = window_sharpe(a, 0, 5);
  const f64 sb = window_sharpe(b, 0, 5);
  ASSERT_GT(sa, 0.0);
  ASSERT_GT(sb, 0.0);
  const f64 denom = sa + sb;
  EXPECT_NEAR(r->weights[0], sa / denom, 1e-10);
  EXPECT_NEAR(r->weights[1], sb / denom, 1e-10);
  EXPECT_NEAR(r->weights[0] + r->weights[1], 1.0, 1e-12);
}

TEST(AlphaCombiner, IcWeightedNegativeSharpeClippedToZero) {
  // One positive-sharpe, one negative-sharpe alpha. max(sharpe,0) clips the
  // negative leg to 0, so the positive alpha gets all the weight.
  AlphaStore pool;
  // Both streams carry genuine variance (so window-sharpe is well-defined): `pos`
  // has a positive mean, `neg` a negative mean -> negative sharpe, clipped to 0.
  const std::vector<f64> pos{0.0, 0.03, 0.01, 0.03, 0.01};     // mean +0.02
  const std::vector<f64> neg{0.0, -0.03, -0.01, -0.03, -0.01}; // mean -0.02 -> sharpe < 0
  insert_pnl(pool, pos);
  insert_pnl(pool, neg);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::IcWeighted;
  const auto r = comb.fit(pool, 0, 5);
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(r->weights[0], 1.0, 1e-12);
  EXPECT_NEAR(r->weights[1], 0.0, 1e-12);
}

// ===========================================================================
//  ShrinkageMv — Ledoit-Wolf shrunk MV weights
// ===========================================================================

TEST(AlphaCombiner, ShrinkageMvIdenticalAlphasEqualWeights) {
  // Two IDENTICAL alphas: S is rank-1 (singular); shrinkage rescues. By symmetry
  // the weights are equal; Σ|w| = 1 -> each 0.5.
  AlphaStore pool;
  const std::vector<f64> a{0.0, 0.01, -0.02, 0.03, 0.01, -0.01};
  insert_pnl(pool, a);
  insert_pnl(pool, a); // identical
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::ShrinkageMv;
  const auto r = comb.fit(pool, 0, 6);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->weights.size(), 2U);
  for (const f64 w : r->weights) {
    EXPECT_TRUE(std::isfinite(w));
  }
  EXPECT_NEAR(r->weights[0], r->weights[1], 1e-9);
  EXPECT_NEAR(abs_sum(r->weights), 1.0, 1e-9);
}

TEST(AlphaCombiner, ShrinkageMvAntiCorrelatedEqualSharpeEqualWeights) {
  // Two anti-correlated alphas with the SAME variance and SAME |mean| (equal
  // Sharpe magnitude, opposite covariance sign). By symmetry the risk-balanced
  // MV weights are equal after Σ|w|=1 normalization.
  AlphaStore pool;
  const std::vector<f64> a{0.0, 0.02, -0.02, 0.02, -0.02, 0.02};
  std::vector<f64> b(a.size());
  for (usize i = 0; i < a.size(); ++i) {
    b[i] = -a[i]; // exact negation -> same var, anti-correlated, same |mean|
  }
  insert_pnl(pool, a);
  insert_pnl(pool, b);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::ShrinkageMv;
  const auto r = comb.fit(pool, 0, 6);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->weights.size(), 2U);
  for (const f64 w : r->weights) {
    EXPECT_TRUE(std::isfinite(w));
  }
  EXPECT_NEAR(std::abs(r->weights[0]), std::abs(r->weights[1]), 1e-9);
  EXPECT_NEAR(abs_sum(r->weights), 1.0, 1e-9);
}

TEST(AlphaCombiner, ShrinkageMvSingularScmRescuedByShrinkage) {
  // T < N: T=2 observations (after the structural-zero exclusion the window still
  // has at least 2 rows), N=3 alphas -> the sample covariance S is singular.
  // Ledoit-Wolf shrinkage toward (tr(S)/N)·I makes Σ̂ SPD, so fit() returns finite
  // weights (NOT Err) with Σ|w| ≈ 1.
  AlphaStore pool;
  const std::vector<f64> a{0.0, 0.01, 0.03, -0.02};
  const std::vector<f64> b{0.0, -0.01, 0.02, 0.04};
  const std::vector<f64> c{0.0, 0.02, -0.03, 0.01};
  insert_pnl(pool, a);
  insert_pnl(pool, b);
  insert_pnl(pool, c);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::ShrinkageMv;
  // Window [2,4): two observations, N=3 -> singular S. The canonical LW δ comes out
  // ~0 here (the populated subspace looks near-spherical, so there is little
  // shrinkage signal); the kLwSpdFloor numerical guard lifts every null-space
  // eigenvalue above 0, so Σ̂ is SPD and fit() returns finite weights (NOT Err).
  const auto r = comb.fit(pool, 2, 4);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->weights.size(), 3U);
  for (const f64 w : r->weights) {
    EXPECT_TRUE(std::isfinite(w));
  }
  EXPECT_NEAR(abs_sum(r->weights), 1.0, 1e-9);
}

TEST(AlphaCombiner, ShrinkageMvFixedIntensityRespected) {
  // A fixed shrinkage intensity in [0,1] is honored (not the AUTO LW path).
  AlphaStore pool;
  const std::vector<f64> a{0.0, 0.01, -0.02, 0.03, 0.01};
  const std::vector<f64> b{0.0, 0.02, 0.01, -0.01, 0.02};
  insert_pnl(pool, a);
  insert_pnl(pool, b);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::ShrinkageMv;
  comb.cfg.shrinkage = 0.5; // fixed intensity
  const auto r = comb.fit(pool, 0, 5);
  ASSERT_TRUE(r.has_value());
  for (const f64 w : r->weights) {
    EXPECT_TRUE(std::isfinite(w));
  }
  EXPECT_NEAR(abs_sum(r->weights), 1.0, 1e-9);
}

TEST(AlphaCombiner, ShrinkageMvWellConditionedInverseVarianceTilt) {
  // REGRESSION TEST for the Ledoit-Wolf 1/T² normalization defect: an earlier
  // revision saturated the intensity δ -> 1 (full shrinkage to μ·I) on every
  // realistic window, which discards the SCM off-diagonal/variance structure and
  // collapses ShrinkageMv to EqualWeight. On a WELL-CONDITIONED window (T≫N) the
  // canonical LW δ is small, so the inverse-variance tilt MUST show through:
  // equal expected returns but distinct variances => the lowest-variance alpha
  // earns a strictly larger |weight| than the highest-variance one.
  //
  // Deterministic fixture (no RNG): three mutually-orthogonal Walsh square waves
  // (periods 2, 4, 8 over T=64, a multiple of 8 => exact orthogonality + zero mean
  // per wave) with DISTINCT amplitudes (=> distinct variances, ~zero cross-corr),
  // each plus the SAME positive drift (=> equal mean returns μ_i). With a near-
  // diagonal Σ̂ and equal μ, MV weight w_i ∝ μ / σ²_i, so amp ascending => |w|
  // descending.
  constexpr usize T = 64;
  constexpr f64 drift = 0.01;
  const f64 amp[3] = {0.005, 0.02, 0.05}; // distinct vols: low, mid, high
  std::vector<f64> rows[3];
  for (usize i = 0; i < 3; ++i) {
    rows[i].resize(T);
    const usize period = (i == 0) ? 2 : (i == 1) ? 4 : 8; // Walsh periods 2,4,8
    for (usize t = 0; t < T; ++t) {
      const bool hi = ((t / (period / 2)) % 2) == 0; // square wave, zero mean
      rows[i][t] = drift + (hi ? amp[i] : -amp[i]);
    }
  }
  AlphaStore pool;
  for (usize i = 0; i < 3; ++i) {
    insert_pnl(pool, rows[i]);
  }
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::ShrinkageMv; // AUTO LW (cfg.shrinkage < 0)
  const auto r = comb.fit(pool, 0, T);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->weights.size(), 3U);
  for (const f64 w : r->weights) {
    EXPECT_TRUE(std::isfinite(w));
  }
  EXPECT_NEAR(abs_sum(r->weights), 1.0, 1e-9);

  // The fit is NOT EqualWeight: the inverse-variance tilt is visible.
  EXPECT_GT(std::abs(std::abs(r->weights[0]) - 1.0 / 3.0), 1e-3);
  // Lowest-variance alpha (index 0) earns strictly more |weight| than the highest
  // (index 2) — the structure the saturated-δ bug would have erased.
  EXPECT_GT(std::abs(r->weights[0]), std::abs(r->weights[2]));

  // δ < 1 (and in fact small) on this well-conditioned window — pins the
  // normalization directly. Expose δ via the testable detail helper.
  const auto sc =
      atx::engine::combine::detail::shrunk_covariance(pool, /*n*/ 3, /*fit_begin*/ 0, /*t*/ T,
                                                      /*cfg_shrinkage*/ -1.0);
  // Measured δ ≈ 0.0094 on this fixture (≪ 1 — the saturation defect is gone);
  // the assertion uses the loose < 0.5 bound to stay robust across platforms.
  EXPECT_LT(sc.delta, 0.5);
  EXPECT_GE(sc.delta, 0.0);
}

// ===========================================================================
//  BoundedRegression — clip |w_i| <= weight_bound, re-project
// ===========================================================================

TEST(AlphaCombiner, BoundedRegressionRespectsWeightBound) {
  AlphaStore pool;
  // Five alphas so a tight bound (0.25) genuinely binds (1/N = 0.2 < 0.25 but an
  // unconstrained MV fit would exceed it on the dominant directions).
  const std::vector<f64> a{0.0, 0.05, -0.02, 0.03, 0.01, 0.02};
  const std::vector<f64> b{0.0, -0.01, 0.04, 0.02, -0.03, 0.01};
  const std::vector<f64> c{0.0, 0.02, 0.01, -0.04, 0.03, -0.02};
  const std::vector<f64> d{0.0, 0.03, -0.03, 0.01, 0.02, 0.04};
  const std::vector<f64> e{0.0, -0.02, 0.02, 0.03, -0.01, 0.01};
  insert_pnl(pool, a);
  insert_pnl(pool, b);
  insert_pnl(pool, c);
  insert_pnl(pool, d);
  insert_pnl(pool, e);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::BoundedRegression;
  comb.cfg.weight_bound = 0.25;
  const auto r = comb.fit(pool, 0, 6);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->weights.size(), 5U);
  for (const f64 w : r->weights) {
    EXPECT_TRUE(std::isfinite(w));
    // Fixed clip/renorm passes leave a bounded residual overshoot; the tolerance
    // accounts for the final renorm after the last clip (see header iteration note).
    EXPECT_LE(std::abs(w), comb.cfg.weight_bound + 1e-6);
  }
}

// ===========================================================================
//  Fit-window firewall — truncation invariance (§3.1)
// ===========================================================================

TEST(AlphaCombiner, FitWindowTruncationInvariant) {
  // Fit on [1,4). Then build a SECOND pool whose rows >= 4 are garbage (and the
  // streams are shorter) and fit on the SAME [1,4). The weights must be
  // BYTE-IDENTICAL: rows >= fit_end are provably invisible to fit().
  AlphaStore full;
  const std::vector<f64> a{0.0, 0.01, -0.02, 0.03, 99.0, -88.0};
  const std::vector<f64> b{0.0, 0.02, 0.01, -0.01, 77.0, 66.0};
  insert_pnl(full, a);
  insert_pnl(full, b);

  AlphaStore truncated;
  // Rows < 4 identical; rows >= 4 mutated to wildly different garbage. The store
  // requires equal period counts across alphas, so keep the length but poison the
  // tail differently from `full`.
  const std::vector<f64> a2{0.0, 0.01, -0.02, 0.03, -123.0, 456.0};
  const std::vector<f64> b2{0.0, 0.02, 0.01, -0.01, 321.0, -654.0};
  insert_pnl(truncated, a2);
  insert_pnl(truncated, b2);

  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::ShrinkageMv; // a deterministic method that reads R
  const auto r_full = comb.fit(full, 1, 4);
  const auto r_trunc = comb.fit(truncated, 1, 4);
  ASSERT_TRUE(r_full.has_value());
  ASSERT_TRUE(r_trunc.has_value());
  ASSERT_EQ(r_full->weights.size(), r_trunc->weights.size());
  for (usize i = 0; i < r_full->weights.size(); ++i) {
    // BYTE-IDENTICAL: the bit patterns must match (future rows are invisible).
    EXPECT_EQ(r_full->weights[i], r_trunc->weights[i]);
  }
}

// ===========================================================================
//  Boundary cases
// ===========================================================================

TEST(AlphaCombiner, SingleAlphaWeightIsOne) {
  AlphaStore pool;
  const std::vector<f64> a{0.0, 0.01, -0.02, 0.03};
  insert_pnl(pool, a);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::ShrinkageMv;
  const auto r = comb.fit(pool, 0, 4);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->weights.size(), 1U);
  EXPECT_NEAR(r->weights[0], 1.0, 1e-12);
}

TEST(AlphaCombiner, SingleAlphaEqualWeightIsOne) {
  AlphaStore pool;
  const std::vector<f64> a{0.0, 0.01, -0.02, 0.03};
  insert_pnl(pool, a);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::EqualWeight;
  const auto r = comb.fit(pool, 0, 4);
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->weights.size(), 1U);
  EXPECT_NEAR(r->weights[0], 1.0, 1e-12);
}

TEST(AlphaCombiner, EmptyPoolReturnsErr) {
  const AlphaStore pool; // no alphas
  AlphaCombiner comb;
  const auto r = comb.fit(pool, 0, 0);
  EXPECT_FALSE(r.has_value());
}

TEST(AlphaCombiner, WindowTooShortReturnsErr) {
  // T < 2 (fit_end - fit_begin < 2) -> Err for every method.
  AlphaStore pool;
  const std::vector<f64> a{0.0, 0.01, -0.02, 0.03};
  const std::vector<f64> b{0.0, 0.02, 0.01, -0.01};
  insert_pnl(pool, a);
  insert_pnl(pool, b);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::ShrinkageMv;
  EXPECT_FALSE(comb.fit(pool, 1, 1).has_value()); // T == 0
  EXPECT_FALSE(comb.fit(pool, 1, 2).has_value()); // T == 1
}

TEST(AlphaCombiner, WindowBeyondPeriodsReturnsErr) {
  // fit_end past n_periods is a misuse -> Err (cannot read the requested window).
  AlphaStore pool;
  const std::vector<f64> a{0.0, 0.01, -0.02};
  insert_pnl(pool, a);
  AlphaCombiner comb;
  comb.cfg.method = CombineMethod::EqualWeight;
  EXPECT_FALSE(comb.fit(pool, 0, 10).has_value()); // window exceeds the streams
}


}  // namespace atxtest_combine_combiner_test
