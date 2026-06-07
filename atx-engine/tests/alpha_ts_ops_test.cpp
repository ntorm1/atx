// atx::engine::alpha — unit tests for OU rolling-fit math kernels (P3d E1/E2).
//
// Tests ou_ar1_fit (AR(1) OLS over a trailing window) and the four derived-
// quantity mappers (ou_theta_of / ou_halflife_of / ou_mean_of / ou_zscore_of).
// These are pure math kernels in ts_ops.hpp — no VM wiring tested here.

#include "atx/engine/alpha/ts_ops.hpp"

#include <array>
#include <cmath>
#include <limits>

#include <gtest/gtest.h>

using namespace atx::engine::alpha::detail;

// ===========================================================================
//  E1 — OuAr1Fit: rolling AR(1) OLS fit helper
// ===========================================================================

TEST(OuAr1Fit, PerfectLineGivesUnitSlope) {
  // window x = [1,2,3,4,5]; pairs (1->2),(2->3),(3->4),(4->5):
  // x[s] = x[s-1] + 1 -> b=1, a=1, resid=0 for all pairs
  std::array<atx::f64, 5> w{1, 2, 3, 4, 5};
  OuAr1Fit f = ou_ar1_fit(std::span<const atx::f64>(w));
  EXPECT_NEAR(f.b, 1.0, 1e-12);
  EXPECT_NEAR(f.a, 1.0, 1e-12);
  EXPECT_NEAR(f.resid_std, 0.0, 1e-12);
  EXPECT_EQ(f.n, 4u);
}

TEST(OuAr1Fit, MeanRevertingSlopeInUnitInterval) {
  // Exact AR(1): x[t] = a_true + b_true*x[t-1] + 0 (no noise).
  // With b_true=0.5, a_true=5 -> mu=10: x[t] = 5 + 0.5*x[t-1].
  // Seed x[0]=20: x=[20,15,12.5,11.25,10.625].
  // OLS on these noiseless pairs recovers b=0.5 exactly.
  std::array<atx::f64, 5> w{20.0, 15.0, 12.5, 11.25, 10.625};
  OuAr1Fit f = ou_ar1_fit(std::span<const atx::f64>(w));
  EXPECT_GT(f.b, 0.0);
  EXPECT_LT(f.b, 1.0);
  EXPECT_NEAR(f.b, 0.5, 1e-9);
}

TEST(OuAr1Fit, TooFewPairsGivesNaN) {
  std::array<atx::f64, 1> w{5.0};
  OuAr1Fit f = ou_ar1_fit(std::span<const atx::f64>(w));
  EXPECT_TRUE(ts_is_nan(f.b)); // <2 pairs -> NaN fields
}

TEST(OuAr1Fit, NaNEndpointPairsSkipped) {
  // Window [NaN, 2, 3, 4]: only pairs (2->3),(3->4) are valid -> n=2
  const atx::f64 nan = kTsNaN;
  std::array<atx::f64, 4> w{nan, 2.0, 3.0, 4.0};
  OuAr1Fit f = ou_ar1_fit(std::span<const atx::f64>(w));
  EXPECT_EQ(f.n, 2u);
  EXPECT_FALSE(ts_is_nan(f.b));
}

TEST(OuAr1Fit, ZeroVariancePredictorGivesNaN) {
  // Constant series: predictor variance = 0 -> degenerate
  std::array<atx::f64, 4> w{5.0, 5.0, 5.0, 5.0};
  OuAr1Fit f = ou_ar1_fit(std::span<const atx::f64>(w));
  EXPECT_TRUE(ts_is_nan(f.b));
}

TEST(OuAr1Fit, EmptyWindowGivesNaN) {
  std::array<atx::f64, 0> w{};
  OuAr1Fit f = ou_ar1_fit(std::span<const atx::f64>(w));
  EXPECT_TRUE(ts_is_nan(f.b));
  EXPECT_EQ(f.n, 0u);
}

// ===========================================================================
//  E2 — OU derived-quantity mappers: theta / halflife / mean / zscore
// ===========================================================================

TEST(OuDeriv, ThetaHalflifeMeanFromKnownB) {
  // construct an OuAr1Fit directly with b=0.5, a=5
  // -> mu=a/(1-b)=10; theta=-ln(0.5)=ln2; halflife=ln2/theta=1
  OuAr1Fit f;
  f.a = 5.0;
  f.b = 0.5;
  f.resid_std = 1.0;
  f.n = 4;
  EXPECT_NEAR(ou_theta_of(f), std::log(2.0), 1e-12);
  EXPECT_NEAR(ou_halflife_of(f), 1.0, 1e-12);
  EXPECT_NEAR(ou_mean_of(f), 10.0, 1e-12);
  // sigma_eq = resid_std / sqrt(1-b^2) = 1/sqrt(0.75)
  // zscore at x_last=12: (12 - 10) / sigma_eq
  const double sig = 1.0 / std::sqrt(0.75);
  EXPECT_NEAR(ou_zscore_of(f, 12.0), (12.0 - 10.0) / sig, 1e-12);
}

TEST(OuDeriv, NonMeanRevertingGivesNaN) {
  OuAr1Fit f;
  f.a = 1.0;
  f.b = 1.5; // b >= 1 -> not mean-reverting
  f.resid_std = 1.0;
  f.n = 4;
  EXPECT_TRUE(ts_is_nan(ou_theta_of(f)));
  EXPECT_TRUE(ts_is_nan(ou_halflife_of(f)));
  EXPECT_TRUE(ts_is_nan(ou_mean_of(f)));
  EXPECT_TRUE(ts_is_nan(ou_zscore_of(f, 5.0)));
}

TEST(OuDeriv, NegativeBGivesNaNForThetaAndZscore) {
  // b < 0: oscillating, not OU-mean-reverting
  OuAr1Fit f;
  f.a = 0.0;
  f.b = -0.5;
  f.resid_std = 1.0;
  f.n = 4;
  EXPECT_TRUE(ts_is_nan(ou_theta_of(f)));
  EXPECT_TRUE(ts_is_nan(ou_halflife_of(f)));
  // ou_mean_of: b < 1 so a/(1-b) = 0/(1.5) = 0 (b<1 allowed for mean)
  EXPECT_FALSE(ts_is_nan(ou_mean_of(f)));
  EXPECT_TRUE(ts_is_nan(ou_zscore_of(f, 0.0)));
}

TEST(OuDeriv, ZeroResidStdGivesNaNZscore) {
  OuAr1Fit f;
  f.a = 5.0;
  f.b = 0.5;
  f.resid_std = 0.0; // sigma_eq = 0 -> zscore undefined
  f.n = 4;
  EXPECT_TRUE(ts_is_nan(ou_zscore_of(f, 10.0)));
}

TEST(OuDeriv, NaNBGivesNaNForAll) {
  OuAr1Fit f; // default-constructed: all NaN
  EXPECT_TRUE(ts_is_nan(ou_theta_of(f)));
  EXPECT_TRUE(ts_is_nan(ou_halflife_of(f)));
  EXPECT_TRUE(ts_is_nan(ou_mean_of(f)));
  EXPECT_TRUE(ts_is_nan(ou_zscore_of(f, 5.0)));
}
