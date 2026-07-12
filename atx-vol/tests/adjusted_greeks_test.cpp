#include "atx/vol/adjusted_greeks.hpp"

#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/greeks.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

// Coverage for SpiderRock's skew-adjusted delta: Adjusted Delta = Delta +
// VegaSlope * Vega, VegaSlope = (1 - omega) * (-dSigma/dk) / S. Test 2
// hand-derives the raw-SVI analytic dw/dk and cross-checks it against
// `curve_skew_slope`'s central-FD path; two sign tests pin BOTH directions
// of the sticky-delta adjustment (a locally POSITIVE skew slope lowers the
// adjusted delta; the typical index put skew's globally NEGATIVE slope
// RAISES it); the rest exercise the sticky-delta / sticky-strike blend and
// documented edge behavior (S <= 0 / non-finite, NaN propagation, FD stencil
// straddling a LinearVarianceCurve wing clamp).

namespace {

using atx::vol::curve_skew_slope;
using atx::vol::Greeks;
using atx::vol::IVolCurve;
using atx::vol::LinearVarianceCurve;
using atx::vol::skew_adjusted;
using atx::vol::StickyParams;
using atx::vol::svi_total_w;
using atx::vol::SviCurve;
using atx::vol::SviParams;
using atx::vol::vega_slope_per_spot;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

}  // namespace

TEST(AdjustedGreeks, FlatSmileLeavesDeltaUnchanged) {
  // Flat total variance across all nodes -> w'(k) == 0 everywhere -> zero
  // skew slope -> zero VegaSlope regardless of sticky control.
  const LinearVarianceCurve curve(0.5, 100.0, 0.99, std::vector<double>{-1.0, 0.0, 1.0},
                                  std::vector<double>{0.04, 0.04, 0.04});
  const double k_log = 0.3;

  EXPECT_DOUBLE_EQ(curve_skew_slope(curve, k_log), 0.0);
  EXPECT_DOUBLE_EQ(vega_slope_per_spot(curve, k_log, 100.0), 0.0);

  const Greeks g{0.55, 0.01, 20.0, -5.0, 3.0, 0.1, 0.2, 0.05};
  const Greeks adj = skew_adjusted(g, vega_slope_per_spot(curve, k_log, 100.0));

  EXPECT_DOUBLE_EQ(adj.delta, g.delta);
  EXPECT_DOUBLE_EQ(adj.gamma, g.gamma);
  EXPECT_DOUBLE_EQ(adj.vega, g.vega);
  EXPECT_DOUBLE_EQ(adj.theta, g.theta);
  EXPECT_DOUBLE_EQ(adj.rho, g.rho);
  EXPECT_DOUBLE_EQ(adj.vanna, g.vanna);
  EXPECT_DOUBLE_EQ(adj.volga, g.volga);
  EXPECT_DOUBLE_EQ(adj.charm, g.charm);
}

TEST(AdjustedGreeks, SviSlopeMatchesAnalytic) {
  // Raw-SVI: w(k) = a + b*(rho*(k-m) + sqrt((k-m)^2 + sigma^2)), so
  //   dw/dk = b*(rho + (k-m)/sqrt((k-m)^2 + sigma^2))
  // and dSigma/dk = dw/dk / (2*sigma_iv*T), sigma_iv = sqrt(w(k)/T). Hand
  // derive this directly from SviParams (independent of curve_skew_slope's
  // FD implementation) and compare, tol 1e-6.
  const SviParams p{0.02, 0.15, -0.4, 0.05, 0.2, 0.5, 100.0};
  const SviCurve curve(p, 0.99);
  const double k_log = 0.3;

  const double x = k_log - p.m;
  const double dw_dk_analytic = p.b * (p.rho + x / std::sqrt(x * x + p.sigma * p.sigma));
  const double w_at_k = svi_total_w(p, k_log);
  const double sigma_iv = std::sqrt(w_at_k / p.T);
  const double expected = dw_dk_analytic / (2.0 * sigma_iv * p.T);

  EXPECT_NEAR(curve_skew_slope(curve, k_log), expected, 1e-6);
}

TEST(AdjustedGreeks, StickyStrikeOmegaOneIsRaw) {
  const SviParams p{0.02, 0.15, -0.4, 0.05, 0.2, 0.5, 100.0};
  const SviCurve curve(p, 0.99);
  const double k_log = 0.3;

  // Sanity: this curve has a genuinely nonzero skew slope at k_log (omega=0
  // sticky-delta case), so omega=1 collapsing to 0 is a real assertion, not
  // a vacuous one.
  const double vega_slope_delta = vega_slope_per_spot(curve, k_log, 100.0, StickyParams{0.0});
  EXPECT_NE(vega_slope_delta, 0.0);

  const double vega_slope_strike = vega_slope_per_spot(curve, k_log, 100.0, StickyParams{1.0});
  EXPECT_DOUBLE_EQ(vega_slope_strike, 0.0);
}

TEST(AdjustedGreeks, LocallyPositiveSkewSlopeLowersAdjustedDelta) {
  // Sign direction 1: where the smile's LOCAL slope is positive (dSigma/dk
  // > 0), sticky-delta gives VegaSlope = -dSigma/dk / S < 0, so the adjusted
  // delta drops below raw. The evaluated k=0.25 sits past the smile minimum
  // on the call wing where variance curls back up (0.04 -> 0.06 over
  // [0, 0.5]) — the SVI-style shape a real board's call wing exhibits. NOTE:
  // the curve's overall put skew (0.30 put wing vs 0.12 call wing, kept for
  // realism) is NOT what drives the sign here; a typical put-skewed strike
  // whose local slope is negative moves the OTHER way — see the companion
  // GlobalPutSkewRaisesAdjustedDelta test.
  const LinearVarianceCurve curve(
      0.5, 100.0, 0.99, std::vector<double>{-1.0, -0.3, 0.0, 0.5, 1.0},
      std::vector<double>{0.30, 0.10, 0.04, 0.06, 0.12});
  const double k_log = 0.25;
  const double S = 100.0;

  const double slope = curve_skew_slope(curve, k_log);
  ASSERT_GT(slope, 0.0);

  const double vega_slope = vega_slope_per_spot(curve, k_log, S, StickyParams{0.0});
  ASSERT_LT(vega_slope, 0.0);

  const Greeks call_g{0.4, 0.02, 15.0, -3.0, 2.0, 0.05, 0.1, 0.02};
  const Greeks adj = skew_adjusted(call_g, vega_slope);

  EXPECT_LT(adj.delta, call_g.delta);
}

TEST(AdjustedGreeks, GlobalPutSkewRaisesAdjustedDelta) {
  // Sign direction 2 — the REAL common case (typical index put skew):
  // total variance falls monotonically with k, so dSigma/dk < 0 everywhere,
  // VegaSlope = -dSigma/dk / S > 0 under omega=0 sticky-delta, and the
  // adjusted call delta RISES above raw (as spot rallies, the sliding smile
  // re-marks a fixed strike to a HIGHER vol, adding positive spot exposure
  // through vega). Hand-derived expected values (curve is piecewise-linear
  // and k=0.25 is interior, so the central FD is exact, not approximate):
  //   dw/dk        = (0.04 - 0.16) / (1.0 - 0.0)        = -0.12
  //   w(0.25)      = 0.16 + 0.25*(-0.12)                =  0.13
  //   sigma        = sqrt(0.13 / 0.5)  = sqrt(0.26)     ~  0.509902
  //   dSigma/dk    = -0.12 / (2 * 0.509902 * 0.5)       ~ -0.235339
  //   vega_slope   = -(-0.235339) / 100                 ~ +0.00235339
  //   adj delta    = 0.4 + 0.00235339 * 15              ~  0.435301
  const double T = 0.5;
  const LinearVarianceCurve curve(T, 100.0, 0.99, std::vector<double>{-1.0, 0.0, 1.0},
                                  std::vector<double>{0.30, 0.16, 0.04});
  const double k_log = 0.25;
  const double S = 100.0;

  const double w_at_k = 0.16 + 0.25 * (0.04 - 0.16);
  const double sigma = std::sqrt(w_at_k / T);
  const double expected_skew_slope = -0.12 / (2.0 * sigma * T);
  ASSERT_LT(expected_skew_slope, 0.0);
  EXPECT_NEAR(curve_skew_slope(curve, k_log), expected_skew_slope, 1e-9);

  const double vega_slope = vega_slope_per_spot(curve, k_log, S, StickyParams{0.0});
  const double expected_vega_slope = -expected_skew_slope / S;
  ASSERT_GT(vega_slope, 0.0);
  EXPECT_NEAR(vega_slope, expected_vega_slope, 1e-12);

  const Greeks call_g{0.4, 0.02, 15.0, -3.0, 2.0, 0.05, 0.1, 0.02};
  const Greeks adj = skew_adjusted(call_g, vega_slope);

  EXPECT_GT(adj.delta, call_g.delta);
  EXPECT_NEAR(adj.delta, call_g.delta + expected_vega_slope * call_g.vega, 1e-10);
}

TEST(AdjustedGreeks, NonPositiveOrNonFiniteSpotYieldsNaN) {
  const SviParams p{0.02, 0.15, -0.4, 0.05, 0.2, 0.5, 100.0};
  const SviCurve curve(p, 0.99);
  const double k_log = 0.3;

  EXPECT_TRUE(std::isnan(vega_slope_per_spot(curve, k_log, 0.0)));
  EXPECT_TRUE(std::isnan(vega_slope_per_spot(curve, k_log, -50.0)));
  EXPECT_TRUE(std::isnan(vega_slope_per_spot(curve, k_log, kNaN)));
  EXPECT_TRUE(std::isnan(
      vega_slope_per_spot(curve, k_log, std::numeric_limits<double>::infinity())));
}

TEST(AdjustedGreeks, NaNVegaSlopePropagatesToAdjustedDeltaOnly) {
  const Greeks g{0.4, 0.02, 15.0, -3.0, 2.0, 0.05, 0.1, 0.02};
  const Greeks adj = skew_adjusted(g, kNaN);

  EXPECT_TRUE(std::isnan(adj.delta));
  EXPECT_DOUBLE_EQ(adj.gamma, g.gamma);
  EXPECT_DOUBLE_EQ(adj.vega, g.vega);
  EXPECT_DOUBLE_EQ(adj.theta, g.theta);
  EXPECT_DOUBLE_EQ(adj.rho, g.rho);
  EXPECT_DOUBLE_EQ(adj.vanna, g.vanna);
  EXPECT_DOUBLE_EQ(adj.volga, g.volga);
  EXPECT_DOUBLE_EQ(adj.charm, g.charm);
}

TEST(AdjustedGreeks, FdStencilAtWingClampGivesHalfSegmentSlope) {
  // Evaluated exactly at the curve's left node: the central FD stencil's
  // minus-side sample is flat-clamped (LinearVarianceCurve extrapolates
  // flat outside its node range) while the plus-side sample sits inside the
  // first interior segment, so the FD slope is documented to land at
  // exactly HALF the interior segment's true slope (average of a zero
  // flat-side slope and the sloped interior side).
  const std::vector<double> k{-1.0, -0.3, 0.0, 0.5, 1.0};
  const std::vector<double> w{0.30, 0.10, 0.04, 0.06, 0.12};
  const double T = 0.5;
  const LinearVarianceCurve curve(T, 100.0, 0.99, k, w);

  const double segment_slope = (w[1] - w[0]) / (k[1] - k[0]);
  const double expected_dw_dk = 0.5 * segment_slope;
  const double sigma = std::sqrt(w[0] / T);
  const double expected_slope = expected_dw_dk / (2.0 * sigma * T);

  EXPECT_NEAR(curve_skew_slope(curve, k[0]), expected_slope, 1e-9);
}
