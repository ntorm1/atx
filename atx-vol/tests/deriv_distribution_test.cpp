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
using atx::vol::deriv_price;
using atx::vol::DerivConfig;
using atx::vol::DerivContract;
using atx::vol::DerivDiscreteCorrection;
using atx::vol::DerivFlags;
using atx::vol::DerivKind;
using atx::vol::ErrorCode;
using atx::vol::EssviSurface;
using atx::vol::has_flag;
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

// ── Task 4: capped variance swap ──────────────────────────────────────────

// Far-OTM cap: capped == uncapped to quadrature noise; CapApplied still
// stamped. Uses the DEFAULT config (vol_of_vol == 0), so this is also the
// direct auto-calibration coverage carried forward from the Task 3 review:
// on the flat fixture Carr-Lee convexity is always present (see
// VolOfVol.AutoCalibrationReproducesCarrLee above), so the auto path here is
// non-degenerate -- vol_of_vol_used must come back finite and > 0, and
// VolOfVolCalibrated must be stamped.
TEST(CappedVarSwap, FarOtmCapMatchesUncapped) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();
  DerivContract c{};
  c.kind = DerivKind::VarSwap; c.maturity_t = 0.25; c.notional = 1e6;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto plain = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(plain.has_value());
  c.kind = DerivKind::CappedVarSwap;
  c.cap_dec = 25.0;  // absurdly high variance cap
  const auto capped = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(capped.has_value());
  EXPECT_NEAR(capped->fair_strike_dec, plain->fair_strike_dec,
              1e-9 * plain->fair_strike_dec);
  EXPECT_TRUE(has_flag(capped->flags, DerivFlags::CapApplied));
  EXPECT_TRUE(has_flag(capped->flags, DerivFlags::ModelProxy));
  EXPECT_TRUE(has_flag(capped->flags, DerivFlags::VolOfVolCalibrated));
  EXPECT_TRUE(std::isfinite(capped->vol_of_vol_used));
  EXPECT_GT(capped->vol_of_vol_used, 0.0);
}

// Parity: capped expectation + cap option value == uncapped expectation.
// Explicit cfg.vol_of_vol = 0.80 also covers the explicit-xi resolve_vol_of_vol
// path end to end: vol_of_vol_used must come back exactly 0.80 (a pass-through,
// not a computation) and VolOfVolCalibrated must NOT be stamped.
TEST(CappedVarSwap, CapParityHolds) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.80;
  DerivContract c{};
  c.kind = DerivKind::CappedVarSwap; c.maturity_t = 0.25; c.notional = 1e6;
  c.cap_dec = 0.05;  // near-the-money cap vs K_var ~ 0.04
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  DerivContract u = c; u.kind = DerivKind::VarSwap; u.cap_dec = 0.0;
  const auto uq = deriv_price(surf, cs, u, cfg);
  ASSERT_TRUE(uq.has_value());
  EXPECT_GT(q->cap_option_value_dec, 0.0);
  EXPECT_NEAR(q->fair_strike_dec + q->cap_option_value_dec,
              uq->fair_strike_dec, 1e-12);
  EXPECT_LT(q->fair_strike_dec, uq->fair_strike_dec);  // cap lowers fair strike
  EXPECT_EQ(q->vol_of_vol_used, 0.80);
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::VolOfVolCalibrated));
}

// Accrued already above the cap: PV pinned at df*N*(C-K), CapPinned stamped,
// and the surface is never needed (mid-life, but the future leg is irrelevant).
TEST(CappedVarSwap, AccruedAboveCapPinsPv) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();
  DerivContract c{};
  c.kind = DerivKind::CappedVarSwap; c.maturity_t = 0.10; c.notional = 1e6;
  c.strike_dec = 0.04; c.cap_dec = 0.09;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 21u;
  c.rv_spec.rv_done_dec = 0.09 * 3.001;  // w_done*rv_done = (1/3)*0.27 > 0.09 = C
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  const double df = cs.yield.disc(0.10);
  EXPECT_NEAR(q->pv, df * 1e6 * (0.09 - 0.04), 1e-6);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::CapPinned));
  EXPECT_NEAR(q->fair_strike_dec, 0.09, 1e-15);
}

TEST(CappedVarSwap, ZeroCapRejected) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::CappedVarSwap; c.maturity_t = 0.25; c.notional = 1.0;
  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
}

// Review finding 1: price_var_swap scales the future leg by
// (1 + 1/n_obs_total) and stamps DiscreteCorrApplied under
// Diffusion1OverN; price_capped_var_swap must do the SAME thing to the SAME
// leg, or a plain VarSwap and a CappedVarSwap on the same underlying would
// disagree on K_var_future under this config. Far-OTM cap isolates that:
// with the cap effectively inert, the capped fair strike should equal the
// plain VarSwap's OWN corrected fair strike, not the uncorrected one.
TEST(CappedVarSwap, DiscreteCorrectionAppliesToFutureLegConsistently) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.discrete_correction_mode = DerivDiscreteCorrection::Diffusion1OverN;
  DerivContract c{};
  c.kind = DerivKind::VarSwap; c.maturity_t = 0.25; c.notional = 1e6;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto plain = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(plain.has_value());
  ASSERT_TRUE(has_flag(plain->flags, DerivFlags::DiscreteCorrApplied));

  c.kind = DerivKind::CappedVarSwap;
  c.cap_dec = 25.0;  // absurdly high variance cap -- effectively uncapped
  const auto capped = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(capped.has_value());
  EXPECT_TRUE(has_flag(capped->flags, DerivFlags::DiscreteCorrApplied));
  EXPECT_NEAR(capped->fair_strike_dec, plain->fair_strike_dec,
              1e-9 * plain->fair_strike_dec);
}

// Review finding 2: CappedVolSwap dispatch precedence was correct but had no
// direct coverage. Pins today's behavior (Task 5 implements the pricer and
// updates ValidCapStillNotImplemented then).
TEST(CappedVolSwap, ZeroCapRejected) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::CappedVolSwap; c.maturity_t = 0.25; c.notional = 1.0;
  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
}

TEST(CappedVolSwap, ValidCapStillNotImplemented) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::CappedVolSwap; c.maturity_t = 0.25; c.notional = 1.0;
  c.cap_dec = 0.50;
  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::NotImplemented);
}
}  // namespace
