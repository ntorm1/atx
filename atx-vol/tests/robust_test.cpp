#include <gtest/gtest.h>

#include <array>
#include <span>
#include <vector>

#include "fitting/robust.hpp"

// Robust-statistics helper coverage, ported from the C ats-vol Huber helper
// (ats_vol_huber.c) plus the textbook Huber M-estimator primitives:
//   - huber_weight  at |r| < k, |r| == k, |r| > k
//   - huber_loss    quadratic core / linear tail / C1 transition
//   - quantile_sorted_lower vs hand-computed order statistics
//   - huber_weights_strided q90-anchored reweighting (small chain, striding,
//     default-k fallback, degenerate inputs)

namespace {

using atx::vol::detail::huber_loss;
using atx::vol::detail::huber_weight;
using atx::vol::detail::huber_weights_strided;
using atx::vol::detail::quantile_sorted_lower;

// ── huber_weight ────────────────────────────────────────────────────────

TEST(Robust, HuberWeight_ResidualBelowK_ReturnsOne) {
  EXPECT_DOUBLE_EQ(huber_weight(0.5, 1.0), 1.0);
}

TEST(Robust, HuberWeight_ResidualAtK_ReturnsOne) {
  // |r| == k sits on the closed side of the quadratic core.
  EXPECT_DOUBLE_EQ(huber_weight(1.0, 1.0), 1.0);
  EXPECT_DOUBLE_EQ(huber_weight(-1.0, 1.0), 1.0);
}

TEST(Robust, HuberWeight_ResidualAboveK_ReturnsKOverAbsR) {
  EXPECT_DOUBLE_EQ(huber_weight(2.0, 1.0), 0.5);   // k/|r| = 1/2
  EXPECT_DOUBLE_EQ(huber_weight(-4.0, 1.0), 0.25); // magnitude only
}

TEST(Robust, HuberWeight_ZeroResidual_ReturnsOne) {
  EXPECT_DOUBLE_EQ(huber_weight(0.0, 1.0), 1.0);
}

// ── huber_loss ──────────────────────────────────────────────────────────

TEST(Robust, HuberLoss_BelowThreshold_IsQuadratic) {
  // rho(r) = ½·r²  ->  ½·1² = 0.5
  EXPECT_DOUBLE_EQ(huber_loss(1.0, 2.0), 0.5);
  EXPECT_DOUBLE_EQ(huber_loss(-1.0, 2.0), 0.5);
}

TEST(Robust, HuberLoss_AtThreshold_QuadraticEqualsLinear) {
  // At |r| == k the quadratic (½·k²) and linear (k·(k−½·k)) branches agree.
  const double k = 2.0;
  EXPECT_DOUBLE_EQ(huber_loss(k, k), 0.5 * k * k);      // 2.0
  EXPECT_DOUBLE_EQ(huber_loss(k, k), k * (k - 0.5 * k)); // 2.0
}

TEST(Robust, HuberLoss_AboveThreshold_IsLinear) {
  // rho(r) = k·(|r| − ½·k) = 2·(5 − 1) = 8
  EXPECT_DOUBLE_EQ(huber_loss(5.0, 2.0), 8.0);
  EXPECT_DOUBLE_EQ(huber_loss(-5.0, 2.0), 8.0);
}

// ── quantile_sorted_lower ───────────────────────────────────────────────

TEST(Robust, QuantileSortedLower_Median_ReturnsMiddleOrderStat) {
  const std::array<double, 5> s{1.0, 2.0, 3.0, 4.0, 5.0};
  // idx = floor(0.5·4) = 2 -> s[2]
  EXPECT_DOUBLE_EQ(quantile_sorted_lower<double>(std::span<const double>{s}, 0.5),
                   3.0);
}

TEST(Robust, QuantileSortedLower_Q90_MatchesTruncatedIndex) {
  const std::array<double, 10> s{1.0, 2.0, 3.0, 4.0, 5.0,
                                 6.0, 7.0, 8.0, 9.0, 10.0};
  // idx = floor(0.9·9) = floor(8.1) = 8 -> s[8] (no interpolation)
  EXPECT_DOUBLE_EQ(quantile_sorted_lower<double>(std::span<const double>{s}, 0.9),
                   9.0);
}

TEST(Robust, QuantileSortedLower_Extremes_ReturnMinAndMax) {
  const std::array<double, 4> s{-2.0, 0.0, 3.5, 7.0};
  EXPECT_DOUBLE_EQ(quantile_sorted_lower<double>(std::span<const double>{s}, 0.0),
                   -2.0);
  EXPECT_DOUBLE_EQ(quantile_sorted_lower<double>(std::span<const double>{s}, 1.0),
                   7.0);
}

TEST(Robust, QuantileSortedLower_SingleElement_ReturnsThatElement) {
  const std::array<double, 1> s{42.0};
  EXPECT_DOUBLE_EQ(quantile_sorted_lower<double>(std::span<const double>{s}, 0.9),
                   42.0);
}

TEST(Robust, QuantileSortedLower_EmptyRange_ReturnsZero) {
  const std::span<const double> empty{};
  EXPECT_DOUBLE_EQ(quantile_sorted_lower<double>(empty, 0.5), 0.0);
}

// ── huber_weights_strided ───────────────────────────────────────────────

TEST(Robust, HuberWeightsStrided_UniformResiduals_AllWeightsOne) {
  // Every residual equals the q90, so rr == 1 < k and no observation is
  // down-weighted, regardless of the (n > 64) striding path.
  std::vector<double> r_abs(100, 2.0);
  std::vector<double> w(r_abs.size(), -1.0);
  huber_weights_strided<double>(r_abs, w);
  for (const double wi : w) {
    EXPECT_DOUBLE_EQ(wi, 1.0);
  }
}

TEST(Robust, HuberWeightsStrided_Outlier_DownweightedToHandComputed) {
  // Nine tight residuals + one outlier; k = 1.0 for clean arithmetic.
  // Sorted: [1×9, 3]; q90 idx = floor(0.9·9) = 8 -> 1.0, so scale = 1.0.
  //   inliers: rr = 1  -> excess 0        -> w = 1
  //   outlier: rr = 3  -> excess 3−1 = 2  -> w = 1/(1+2)² = 1/9
  const std::array<double, 10> r_abs{1.0, 1.0, 1.0, 1.0, 1.0,
                                     1.0, 1.0, 1.0, 1.0, 3.0};
  std::array<double, 10> w{};
  huber_weights_strided<double>(std::span<const double>{r_abs},
                                std::span<double>{w}, 1.0);
  for (std::size_t i = 0U; i < 9U; ++i) {
    EXPECT_DOUBLE_EQ(w[i], 1.0);
  }
  EXPECT_NEAR(w[9], 1.0 / 9.0, 1e-15);
}

TEST(Robust, HuberWeightsStrided_DefaultK_MatchesExplicit1345) {
  const std::array<double, 6> r_abs{0.2, 0.5, 0.1, 4.0, 0.3, 0.25};
  std::array<double, 6> w_default{};
  std::array<double, 6> w_explicit{};
  // k <= 0 (and the omitted-argument default) both select 1.345.
  huber_weights_strided<double>(std::span<const double>{r_abs},
                                std::span<double>{w_default}, -1.0);
  huber_weights_strided<double>(std::span<const double>{r_abs},
                                std::span<double>{w_explicit}, 1.345);
  for (std::size_t i = 0U; i < r_abs.size(); ++i) {
    EXPECT_DOUBLE_EQ(w_default[i], w_explicit[i]);
  }
}

TEST(Robust, HuberWeightsStrided_LargeChainOutlier_HandComputed) {
  // n > 64 exercises the strided q90 subsample. All inliers are 1.0 so q90 == 1
  // (one outlier cannot move the 90th percentile), scale = 1.0, k = 1.345.
  //   outlier rr = 10 -> excess = 10 − 1.345 = 8.655 -> w = 1/(9.655)²
  std::vector<double> r_abs(128, 1.0);
  r_abs[0] = 10.0;
  std::vector<double> w(r_abs.size(), -1.0);
  huber_weights_strided<double>(r_abs, w); // default k = 1.345
  const double denom = 1.0 + (10.0 - 1.345);
  EXPECT_NEAR(w[0], 1.0 / (denom * denom), 1e-15);
  for (std::size_t i = 1U; i < r_abs.size(); ++i) {
    EXPECT_DOUBLE_EQ(w[i], 1.0);
  }
}

TEST(Robust, HuberWeightsStrided_AllZeroResiduals_AllWeightsOne) {
  // Degenerate chain: q90 == 0 clamps to the 1e-9 scale floor; rr == 0, so
  // every weight is 1 (and, crucially, no division by zero).
  std::array<double, 8> r_abs{};
  std::array<double, 8> w{};
  huber_weights_strided<double>(std::span<const double>{r_abs},
                                std::span<double>{w}, 1.345);
  for (const double wi : w) {
    EXPECT_DOUBLE_EQ(wi, 1.0);
  }
}

TEST(Robust, HuberWeightsStrided_MismatchedSizes_LeavesOutputUntouched) {
  const std::array<double, 3> r_abs{1.0, 2.0, 3.0};
  std::array<double, 2> w{-1.0, -1.0}; // wrong length -> no-op
  huber_weights_strided<double>(std::span<const double>{r_abs},
                                std::span<double>{w}, 1.345);
  EXPECT_DOUBLE_EQ(w[0], -1.0);
  EXPECT_DOUBLE_EQ(w[1], -1.0);
}

} // namespace
