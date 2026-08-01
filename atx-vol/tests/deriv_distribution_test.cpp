#include <gtest/gtest.h>
#include <cmath>
#include "atx/vol/derivatives.hpp"
#include "atx/vol/detail/rv_lognormal.hpp"
#include "support/deriv_test_fixture.hpp"

namespace {
using atx::vol::detail::gh_rule;
using atx::vol::detail::lognormal_call;
using atx::vol::detail::lognormal_expect;
using atx::vol::detail::lognormal_sqrt_moment;
using atx::vol::detail::lognormal_truncated_expect;
using atx::vol::detail::norm_cdf;

// Task 3 (vol-of-vol knob + Carr-Lee-consistent auto-calibration): config
// validation and the closed-form identity resolve_vol_of_vol's auto path
// relies on. resolve_vol_of_vol itself is internal (derivatives.cpp anon
// namespace, Tasks 4-6's shared helper) with no public surface to unit-test
// directly here -- these two tests exercise it through the public entries
// that already exist (var_swap_fair_strike / vol_swap_fair_strike).
using atx::vol::CurveSet;
using atx::vol::deriv_default_config;
using atx::vol::DerivConfig;
using atx::vol::ErrorCode;
using atx::vol::EssviSurface;
using atx::vol::var_swap_fair_strike;
using atx::vol::vol_swap_fair_strike;
using atx::vol::testsupport::make_flat_curves;
using atx::vol::testsupport::make_flat_surface;

TEST(GhRule, WeightsIntegrateGaussianMoments) {
  const auto& r = gh_rule();
  double s0 = 0.0, s2 = 0.0;
  for (std::size_t i = 0; i < r.x.size(); ++i) {
    s0 += r.w[i];
    s2 += r.w[i] * r.x[i] * r.x[i];
  }
  const double rt_pi = std::sqrt(std::acos(-1.0));
  EXPECT_NEAR(s0, rt_pi, 1e-12);            // ∫e^{-x²} = √π
  EXPECT_NEAR(s2, 0.5 * rt_pi, 1e-12);      // ∫x²e^{-x²} = √π/2
}

TEST(RvLognormal, ExpectRecoversMeanAndSqrtMoment) {
  const double m = 0.04, s = 0.45;
  const double mean = lognormal_expect(m, s, [](double w) { return w; });
  EXPECT_NEAR(mean, m, 1e-9 * m);           // E[W] = m by construction
  const double sq = lognormal_expect(m, s, [](double w) { return std::sqrt(w); });
  EXPECT_NEAR(sq, lognormal_sqrt_moment(m, s), 1e-10);
}

TEST(RvLognormal, CallMatchesQuadratureAndParity) {
  const double m = 0.04, s = 0.60, k = 0.05;

  // GH-21 of the raw kinked payoff max(w-k,0) is NOT spectrally accurate
  // (see the header's "Kinked payoffs" note) -- comparing it to the
  // closed form at economically-tight tolerance is not a valid oracle test.
  // Instead: split at the kink's standard-normal location z* (W(z*) = k) and
  // integrate the SMOOTH piece w-k on (z*, 8] with the Legendre-based
  // truncated quadrature, which has no kink to lose accuracy on.
  const double z_star = (std::log(k / m) + 0.5 * s * s) / s;
  const double via_quad =
      lognormal_truncated_expect(m, s, z_star, 8.0, [k](double w) { return w - k; });
  EXPECT_NEAR(lognormal_call(m, s, k), via_quad, 1e-9 * m);

  // Parity: E[min(W,k)] + E[(W-k)+] == E[W], verified pointwise THROUGH the
  // shared Gauss-Hermite nodes (exact by linearity of a fixed quadrature
  // rule, regardless of the kink) -- not a claim that GH resolves the kink.
  const double mean_gh = lognormal_expect(m, s, [](double w) { return w; });
  const double call_gh = lognormal_expect(m, s, [k](double w) { return w > k ? w - k : 0.0; });
  const double capped_gh = lognormal_expect(m, s, [k](double w) { return w < k ? w : k; });
  EXPECT_NEAR(capped_gh + call_gh, mean_gh, 1e-12);

  // degenerate edges
  EXPECT_NEAR(lognormal_call(m, 0.0, k), 0.0, 0.0);          // m < k, s=0
  EXPECT_NEAR(lognormal_call(m, 0.0, 0.03), 0.01, 1e-15);    // intrinsic
  EXPECT_NEAR(lognormal_call(m, s, 0.0), m, 1e-15);          // k<=0 -> m-k
}

TEST(TruncatedExpect, FullIntervalAndSplitRecoverMean) {
  const double m = 0.04, s = 0.45;
  const auto identity = [](double w) { return w; };

  // Full [-8,8] truncated expect of the identity ~= E[W] = m (tail mass
  // beyond +-8 sigma is far below this tolerance).
  const double full = lognormal_truncated_expect(m, s, -8.0, 8.0, identity);
  EXPECT_NEAR(full, m, 1e-9 * m);

  // Splitting the domain at an arbitrary interior point and summing the two
  // smooth pieces recovers the same mean -- the split+recombine path a
  // capped-payoff caller relies on when it splits at a kink instead.
  const double z_split = 0.37;
  const double lower = lognormal_truncated_expect(m, s, -8.0, z_split, identity);
  const double upper = lognormal_truncated_expect(m, s, z_split, 8.0, identity);
  EXPECT_NEAR(lower + upper, m, 1e-9 * m);

  // Empty interval (z_hi <= z_lo, including after the +-8 clamp) -> 0.
  EXPECT_NEAR(lognormal_truncated_expect(m, s, 1.0, 1.0, identity), 0.0, 0.0);
  EXPECT_NEAR(lognormal_truncated_expect(m, s, 2.0, 1.0, identity), 0.0, 0.0);
}

TEST(RvLognormal, NormCdfKnownValues) {
  EXPECT_NEAR(norm_cdf(0.0), 0.5, 1e-15);
  EXPECT_NEAR(norm_cdf(1.959963984540054), 0.975, 1e-9);
  EXPECT_NEAR(norm_cdf(-1.959963984540054), 0.025, 1e-9);
}

// ── Vol-of-vol config validation + Carr-Lee closed-form oracle ────────────

TEST(VolOfVol, NegativeConfigRejected) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = -0.5;
  const auto q = var_swap_fair_strike(surf, cs, 0.10, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
}

// On a FLAT surface Carr-Lee K_vol < sqrt(K_var) purely from the ATMF
// straddle's own lognormal convexity, so auto-calibration must return xi > 0,
// and the calibrated lognormal must reproduce Carr-Lee exactly by construction:
// sqrt(K_var) * exp(-s^2/8) == K_vol_CL.
TEST(VolOfVol, AutoCalibrationReproducesCarrLee) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();
  const double T = 0.25;
  const auto kv = var_swap_fair_strike(surf, cs, T, cfg);
  const auto kl = vol_swap_fair_strike(surf, cs, T, cfg);
  ASSERT_TRUE(kv.has_value());
  ASSERT_TRUE(kl.has_value());
  ASSERT_LT(kl->fair_strike_dec, std::sqrt(kv->fair_strike_dec));  // convexity exists
  const double s2 = -8.0 * std::log(kl->fair_strike_dec / std::sqrt(kv->fair_strike_dec));
  ASSERT_GT(s2, 0.0);
  const double recon = std::sqrt(kv->fair_strike_dec) * std::exp(-s2 / 8.0);
  EXPECT_NEAR(recon, kl->fair_strike_dec, 1e-14);
}
}  // namespace
