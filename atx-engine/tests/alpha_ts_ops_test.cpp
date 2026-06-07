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

