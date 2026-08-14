#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <random>
#include "atx/vol/derivatives.hpp"
#include "atx/vol/detail/counters.hpp" // Task F-5: the dispatch-level strip-eval witness
#include "atx/vol/detail/rv_lognormal.hpp"
#include "deriv_fixtures.hpp" // Task 0: deriv_testkit::make_skew_surface (Task C-5's skewed fixture)
#include "support/deriv_test_fixture.hpp"

namespace {
using atx::vol::detail::gh_rule;
using atx::vol::detail::lognormal_call;
using atx::vol::detail::lognormal_expect;
using atx::vol::detail::lognormal_put;
using atx::vol::detail::lognormal_sqrt_moment;
using atx::vol::detail::lognormal_truncated_expect;
using atx::vol::detail::norm_cdf;
namespace ledger = atx::vol::counters::ledger;

// Task 3 (vol-of-vol knob + Carr-Lee-consistent auto-calibration): config
// validation and the closed-form identity resolve_vol_of_vol's auto path
// relies on. resolve_vol_of_vol itself is internal (derivatives.cpp anon
// namespace, Tasks 4-6's shared helper) with no public surface to unit-test
// directly here -- these two tests exercise it through the public entries
// that already exist (var_swap_fair_strike / vol_swap_fair_strike).
using atx::vol::CarrLeeForm;
using atx::vol::CurveSet;
using atx::vol::deriv_default_config;
using atx::vol::deriv_price;
using atx::vol::DerivConfig;
using atx::vol::DerivContract;
using atx::vol::DerivDiscreteCorrection;
using atx::vol::DerivEngine;
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

// ── Task C-5: Carr-Lee convexity refinement feeds xi auto-calibration ─────
//
// DELIBERATE DEVIATION from the brief's literal "refined K_vol => larger xi
// => richer caps" wording -- verified against resolve_vol_of_vol's own
// (pre-existing, un-modified-by-this-task) closed form and confirmed by this
// test's first failing run; task-C-5-report.md has the full derivation.
// Summary: xi solves s^2 = -8*ln(k_vol_target / sqrt(K_var)), i.e.
// xi = f(ratio) for a ratio = k_vol_target/sqrt(K_var) STRICTLY DECREASING
// in k_vol_target (d(xi)/dk < 0 for every k_vol_target in (0, sqrt(K_var)) --
// algebra in the report, not just this fixture). Refined k_vol is CLOSER to
// sqrt(K_var) than naive (CarrLee.RefinementOrderedUnderSkew), so it needs
// LESS lognormal dispersion to explain a SMALLER Jensen gap: refined xi is
// strictly SMALLER, not larger, and (cap options being vega-positive) so is
// the cap option value. Economically: the naive formula's own approximation
// shortfall gets misattributed by the auto-calibrator as if it were real
// vol-of-vol, inflating xi (and cap prices) above what the surface's actual
// convexity supports; feeding it the refined K_vol corrects PART of that
// inflation back down, same direction as the K_vol fix itself.
//
// A flat surface's convexity gap is real but tiny (~1e-5 vol, the
// ATMF-straddle formula's own O(sigma^3*T) approximation bias -- see
// CarrLee.RefinementVanishesOnFlat), too small to move vol_of_vol_used and
// cap_option_value_dec outside quadrature noise; the skewed fixture
// (rho ~= -0.7) gives LIT-4's cited magnitude instead.
TEST(Distribution, XiRespondsToForm) {
  const EssviSurface surf = atx::vol::deriv_testkit::make_skew_surface(0.20, -0.40, 0.35);
  const CurveSet cs = atx::vol::deriv_testkit::make_curves(100.0, 0.02, 0.01);

  DerivContract c{};
  c.kind = DerivKind::CappedVarSwap;
  c.maturity_t = 0.5;  // 6M, LIT-4's cited tenor
  c.notional = 1e6;
  c.cap_dec = 0.05;  // near-the-money cap: genuinely bites, never pins (unaged)
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 126u;  // unaged (n_obs_done defaults to 0)

  DerivConfig naive_cfg = deriv_default_config();
  naive_cfg.carr_lee_form = CarrLeeForm::Naive;
  const auto naive_q = deriv_price(surf, cs, c, naive_cfg);
  ASSERT_TRUE(naive_q.has_value());
  ASSERT_TRUE(has_flag(naive_q->flags, DerivFlags::VolOfVolCalibrated));

  DerivConfig refined_cfg = deriv_default_config();
  refined_cfg.carr_lee_form = CarrLeeForm::Refined;
  const auto refined_q = deriv_price(surf, cs, c, refined_cfg);
  ASSERT_TRUE(refined_q.has_value());
  ASSERT_TRUE(has_flag(refined_q->flags, DerivFlags::VolOfVolCalibrated));

  ASSERT_TRUE(std::isfinite(naive_q->vol_of_vol_used));
  ASSERT_TRUE(std::isfinite(refined_q->vol_of_vol_used));
  // Refined K_vol sits closer to sqrt(K_var) than naive -- see the
  // RefinementOrderedUnderSkew derivation above -- so it needs strictly LESS
  // inferred dispersion to explain the (now smaller) Jensen gap.
  EXPECT_LT(refined_q->vol_of_vol_used, naive_q->vol_of_vol_used);
  EXPECT_LT(refined_q->cap_option_value_dec, naive_q->cap_option_value_dec);
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

// Code review finding: the fully-aged, NOT-pinned exit (min(V,C) collapses to
// rv_done_dec because rv_done_dec < C, so no strip/model runs) was never
// exercised -- every prior fully-aged test also happened to land on the PIN
// path. rv_done = 0.04 < cap_dec = 0.09 here, so this is genuinely the other
// branch: fair_strike_dec == rv_done_dec exactly, no ModelProxy, no strip.
TEST(CappedVarSwap, FullyAgedBelowCapPaysRv) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::CappedVarSwap; c.maturity_t = 0.0; c.notional = 1e5;
  c.strike_dec = 0.03; c.cap_dec = 0.09;  // 0.09 > rv_done 0.04 -- NOT pinned
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u; c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.04;
  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value());
  EXPECT_NEAR(q->pv, 1e5 * (0.04 - 0.03), 1e-9);
  EXPECT_NEAR(q->fair_strike_dec, 0.04, 1e-15);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::FullyAged));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::CapPinned));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::CapApplied));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::ModelProxy));
  EXPECT_TRUE(std::isnan(q->vol_of_vol_used));
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

// ── Task 5: capped volatility swap ────────────────────────────────────────
//
// Numerical method note: E[min(sqrt V, c)] is NOT computed via raw
// Gauss-Hermite on min(sqrt V, c) -- that payoff is kinked in W and GH loses
// its spectral accuracy on kinked integrands (RvLognormal.CallMatchesQuadratureAndParity
// above is the same lesson for a call payoff). Instead the pricer splits the
// domain at the kink's standard-normal coordinate z* and integrates the
// smooth piece with lognormal_truncated_expect (GL-64), closing the tail
// analytically via norm_cdf. These tests exercise that split-domain formula.

// Jensen ordering at inception with explicit vol-of-vol:
//   E[min(sqrt V, c)] <= E[sqrt V] <= sqrt(E[V])   (cap haircut, then concavity)
TEST(CappedVolSwap, JensenAndCapOrdering) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.80;
  DerivContract c{};
  c.kind = DerivKind::CappedVolSwap; c.maturity_t = 0.25; c.notional = 1e5;
  c.cap_dec = 0.22;  // near-the-money vol cap vs sigma = 0.20
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  const auto kv = var_swap_fair_strike(surf, cs, 0.25, cfg);
  ASSERT_TRUE(kv.has_value());
  EXPECT_LT(q->fair_strike_dec, std::sqrt(kv->fair_strike_dec));
  EXPECT_GT(q->fair_strike_dec, 0.10);  // sane magnitude
  // Removing the cap (huge c) must recover the pure E[sqrt V] which exceeds
  // the capped strike.
  DerivContract un = c; un.cap_dec = 10.0;
  const auto uq = deriv_price(surf, cs, un, cfg);
  ASSERT_TRUE(uq.has_value());
  EXPECT_GT(uq->fair_strike_dec, q->fair_strike_dec);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::CapApplied));
}

// GH consistency: with a far-OTM cap and zero accrual the capped vol swap must
// equal the exact lognormal sqrt moment.
TEST(CappedVolSwap, FarOtmCapMatchesLognormalSqrtMoment) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.60;
  DerivContract c{};
  c.kind = DerivKind::CappedVolSwap; c.maturity_t = 0.25; c.notional = 1e5;
  c.cap_dec = 10.0;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  const auto kv = var_swap_fair_strike(surf, cs, 0.25, cfg);
  ASSERT_TRUE(kv.has_value());
  const double s = 0.60 * std::sqrt(0.25);
  const double truth =
      atx::vol::detail::lognormal_sqrt_moment(kv->fair_strike_dec, s);
  EXPECT_NEAR(q->fair_strike_dec, truth, 1e-8);
}

// Fully aged: payoff-exact min(sqrt(rv), c). rv_done_dec = 0.09 -> sqrt =
// 0.30 > cap 0.25, i.e. the accrued leg alone (a == rv_done_dec here) also
// exceeds C == cap^2 == 0.0625, so this lands on the PIN path -- same PV
// formula, exercised at full aging (T == 0) rather than mid-life.
TEST(CappedVolSwap, FullyAgedPaysMinSqrtRvCap) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::CappedVolSwap; c.maturity_t = 0.0; c.notional = 1e5;
  c.strike_dec = 0.20; c.cap_dec = 0.25;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u; c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.09;  // sqrt = 0.30 > cap 0.25
  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value());
  EXPECT_NEAR(q->pv, 1e5 * (0.25 - 0.20), 1e-9);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::FullyAged));
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::CapPinned));
}

// Mid-life pin: analogous to Task 4's CappedVarSwap.AccruedAboveCapPinsPv but
// for the vol cap, in variance-space against C = cap_dec^2. n_done=21/63 ->
// w_done=1/3; rv_done=0.27 -> a=(1/3)*0.27=0.09 > 0.0625=c^2 (c=0.25): the
// accrued leg alone already exceeds the cap, so PV pins at df*N*(c-K) with no
// strip call and the surface is never needed (mid-life, not fully aged).
TEST(CappedVolSwap, AccruedAboveCapPinsPv) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();
  DerivContract c{};
  c.kind = DerivKind::CappedVolSwap; c.maturity_t = 0.10; c.notional = 1e6;
  c.strike_dec = 0.20; c.cap_dec = 0.25;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 21u;
  c.rv_spec.rv_done_dec = 0.27;  // w_done*rv_done = (1/3)*0.27 = 0.09 > 0.0625 = c^2
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  const double df = cs.yield.disc(0.10);
  EXPECT_NEAR(q->pv, df * 1e6 * (0.25 - 0.20), 1e-6);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::CapPinned));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::FullyAged));
  EXPECT_NEAR(q->fair_strike_dec, 0.25, 1e-15);
}

// Code review finding: the fully-aged, NOT-pinned exit (min(sqrt V,c)
// collapses to sqrt(rv_done_dec) because rv_done_dec < C == cap_dec^2, so no
// strip/model runs) was never exercised -- FullyAgedPaysMinSqrtRvCap above
// lands on the PIN path instead (its accrued a also happens to exceed C).
// Here rv_done = 0.04 (sqrt = 0.20) and C = 0.30^2 = 0.09 > 0.04, so this is
// genuinely the other branch: fair_strike_dec == sqrt(rv_done_dec) exactly,
// no ModelProxy, no strip, vol_of_vol_used left at its NaN default.
TEST(CappedVolSwap, FullyAgedBelowCapPaysSqrtRv) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::CappedVolSwap; c.maturity_t = 0.0; c.notional = 1e5;
  c.strike_dec = 0.18; c.cap_dec = 0.30;  // C = 0.09 > a = 0.04 -- NOT pinned
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u; c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.04;  // sqrt = 0.20 < cap 0.30
  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value());
  EXPECT_NEAR(q->pv, 1e5 * (0.20 - 0.18), 1e-9);
  EXPECT_NEAR(q->fair_strike_dec, 0.20, 1e-15);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::FullyAged));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::CapPinned));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::CapApplied));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::ModelProxy));
  EXPECT_TRUE(std::isnan(q->vol_of_vol_used));
}

// Parity: E[min(sqrt V, c)] + cap_option_value_dec == E[sqrt V] (uncapped),
// cross-checked against an INDEPENDENT smooth-integrand GH-21 oracle (NOT the
// GL-64 split-domain nodes the pricer itself uses for the capped side) --
// sqrt(a + b*W) has no kink, so lognormal_expect (GH-21) is spectrally
// accurate on it and serves as an accuracy oracle exactly the way
// RvLognormal.ExpectRecoversMeanAndSqrtMoment uses it above. Per the
// controller amendment this is NOT a same-nodes-exact identity (the capped
// side's tail term is a closed-form 1-Phi(z*), not a quadrature over the same
// nodes as the uncapped GH-21 sum), so the tolerance is 1e-9, not machine
// epsilon. Also covers: cap_option_value_dec > 0 when the cap clearly binds
// (near-the-money cap_dec == 0.22 vs sigma == 0.20) and ~= 0 far OTM.
TEST(CappedVolSwap, CapOptionValueParity) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.80;
  DerivContract c{};
  c.kind = DerivKind::CappedVolSwap; c.maturity_t = 0.25; c.notional = 1e5;
  c.cap_dec = 0.22;  // near-the-money -- cap clearly binds
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  EXPECT_GT(q->cap_option_value_dec, 0.0);

  const auto kv = var_swap_fair_strike(surf, cs, 0.25, cfg);
  ASSERT_TRUE(kv.has_value());
  const double m = kv->fair_strike_dec;  // a == 0, b == 1 here (n_obs_done == 0)
  const double s = 0.80 * std::sqrt(0.25);
  const double oracle = lognormal_expect(m, s, [](double w) { return std::sqrt(w); });
  EXPECT_NEAR(q->fair_strike_dec + q->cap_option_value_dec, oracle, 1e-9);

  // Far-OTM cap: the option value collapses to ~0 (payoff never binds).
  DerivContract un = c; un.cap_dec = 10.0;
  const auto uq = deriv_price(surf, cs, un, cfg);
  ASSERT_TRUE(uq.has_value());
  EXPECT_NEAR(uq->cap_option_value_dec, 0.0, 1e-8);
}

// ── Task 6: mid-life vol swap dispatch ────────────────────────────────────
//
// V = a + b*W: a = w_done*rv_done_dec, b = w_future, W lognormal at the
// strip's own mean m (residual maturity_t) and log-stdev xi*sqrt(maturity_t).
// sqrt(a+b*w) is SMOOTH in w (a, b >= 0), so E[sqrt(V)] is priced by GH-21
// (detail::lognormal_expect) -- unlike the capped payoffs above, there is no
// kink to split around.

// Mid-life continuity: as n_done -> 0 the mid-life price approaches the
// inception Carr-Lee price (auto-calibrated xi makes them agree by construction).
TEST(MidLifeVolSwap, ContinuousWithInceptionAtZeroAccrual) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();
  DerivContract c{};
  c.kind = DerivKind::VolSwap; c.maturity_t = 0.25; c.notional = 1e5;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 0u;
  const auto q0 = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q0.has_value());
  // One observation, realized exactly at the implied level: the blend barely moves.
  c.rv_spec.n_obs_done = 1u;
  c.rv_spec.rv_done_dec = q0->uncapped_var_dec;
  const auto q1 = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q1.has_value()) << q1.error().to_string();
  EXPECT_NEAR(q1->fair_strike_dec, q0->fair_strike_dec,
              2e-3 * q0->fair_strike_dec);
  EXPECT_TRUE(has_flag(q1->flags, DerivFlags::Aged));
  EXPECT_TRUE(has_flag(q1->flags, DerivFlags::ModelProxy));
}

// Mid-life monotonicity: higher accrued realized => higher vol-swap mark.
TEST(MidLifeVolSwap, MonotoneInAccruedRealized) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();
  DerivContract c{};
  c.kind = DerivKind::VolSwap; c.maturity_t = 0.10; c.notional = 1e5;
  c.strike_dec = 0.20;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 42u;
  c.rv_spec.rv_done_dec = 0.02;
  const auto lo = deriv_price(surf, cs, c, cfg);
  c.rv_spec.rv_done_dec = 0.09;
  const auto hi = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(lo.has_value());
  ASSERT_TRUE(hi.has_value());
  EXPECT_GT(hi->fair_strike_dec, lo->fair_strike_dec);
  EXPECT_GT(hi->pv, lo->pv);
  // Deterministic-floor sanity: strike can never fall below sqrt(a) and never
  // exceed sqrt(a + b*m) (Jensen).
  EXPECT_GE(hi->fair_strike_dec, std::sqrt((42.0 / 63.0) * 0.09) - 1e-12);
}

// At expiry the mid-life path must hand over to the fully-aged branch exactly.
TEST(MidLifeVolSwap, HandsOverToFullyAgedAtExpiry) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VolSwap; c.maturity_t = 0.0; c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u; c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;  // sqrt = 0.21
  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value());
  EXPECT_NEAR(q->pv, 1e5 * (0.21 - 0.18), 1e-9);
}

// Engine matrix: an explicit Carr-Lee engine cannot blend an already-accrued
// leg -- Carr-Lee is a pure inception (unaged) formula with no accrual model.
TEST(MidLifeVolSwap, ExplicitVolCarrLeeMidLifeRejected) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.engine = DerivEngine::VolCarrLee;
  DerivContract c{};
  c.kind = DerivKind::VolSwap; c.maturity_t = 0.10; c.notional = 1e5;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 21u; c.rv_spec.rv_done_dec = 0.03;
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
}

// Engine matrix: explicit RvDistributionProxy at inception (n_done == 0)
// prices the vol swap via the SAME distribution model as mid-life, end to
// end (a = 0, b = 1 -- E[sqrt(W)] by GH), not Carr-Lee. Auto-calibrated xi
// makes the lognormal reproduce Carr-Lee's OWN K_vol exactly in closed form
// (sqrt(m)*exp(-s^2/8) == k_vol_cl, see resolve_vol_of_vol's derivation), so
// GH-21's numerical E[sqrt(W)] must match that closed form -- and therefore
// the plain Carr-Lee quote -- to ~1e-9 relative (RvLognormal.
// ExpectRecoversMeanAndSqrtMoment establishes the same order of GH-21
// accuracy on this exact smooth integrand). NOT 1e-12: that would claim GH-21
// is exact, which it is not (see the header's Kernel note / Task 2).
TEST(MidLifeVolSwap, RvDistributionProxyUnagedMatchesCarrLee) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VolSwap; c.maturity_t = 0.25; c.notional = 1e5;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 0u;

  const auto cl = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(cl.has_value());

  DerivConfig cfg = deriv_default_config();
  cfg.engine = DerivEngine::RvDistributionProxy;
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value()) << q.error().to_string();

  EXPECT_NEAR(q->fair_strike_dec, cl->fair_strike_dec, 1e-9 * cl->fair_strike_dec);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::ModelProxy));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::Aged));
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::VolOfVolCalibrated));
  EXPECT_TRUE(std::isfinite(q->vol_of_vol_used));
  EXPECT_GT(q->vol_of_vol_used, 0.0);
}

// Engine matrix: explicit RvDistributionProxy on a FULLY AGED contract has
// nothing left for the model to do -- keep the exact deterministic branch
// (same as Auto), not the distribution model.
TEST(MidLifeVolSwap, RvDistributionProxyFullyAgedKeepsExactBranch) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.engine = DerivEngine::RvDistributionProxy;
  DerivContract c{};
  c.kind = DerivKind::VolSwap; c.maturity_t = 0.0; c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u; c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;  // sqrt = 0.21
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  EXPECT_NEAR(q->pv, 1e5 * (0.21 - 0.18), 1e-9);
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::ModelProxy));
  EXPECT_TRUE(std::isnan(q->integration_error_est));
}

// Carry-forward fix (Task 1 review, ledgered): the standalone Carr-Lee vol
// strike runs no strip, so integration_error_est must stay NaN ("not
// estimated"), never the struct's raw 0.0 default.
TEST(MidLifeVolSwap, StandaloneVolStrikeIntegrationErrorIsNaN) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const auto q = vol_swap_fair_strike(surf, cs, 0.25, deriv_default_config());
  ASSERT_TRUE(q.has_value());
  EXPECT_TRUE(std::isnan(q->integration_error_est));
}

// Carry-forward fix: the new mid-life branch propagates the strip's own
// Richardson error estimate. Standard quality lands on a 4m+1 node grid (Task
// 1 guarantees the estimate is finite there), so this must come back finite
// -- not NaN and not the struct's raw 0.0 default.
TEST(MidLifeVolSwap, IntegrationErrorEstimateFiniteUnderStandardQuality) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();  // Standard quality (default)
  DerivContract c{};
  c.kind = DerivKind::VolSwap; c.maturity_t = 0.10; c.notional = 1e5;
  c.rv_spec.annualization = 252.0; c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 21u; c.rv_spec.rv_done_dec = 0.03;
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  EXPECT_TRUE(std::isfinite(q->integration_error_est));
}

// ── Task F-5: options on realized variance ────────────────────────────────
//
// PV = df*N*E[(V-K)+] (call) / df*N*E[(K-V)+] (put) over the SAME blended
// variance V = a + b*W the capped kinds price. `strike_dec` is the OPTION
// strike; `cap_dec` names nothing and is rejected. Engines: Auto or
// RvDistributionProxy.
//
// The quote's `fair_strike_dec` is the option PREMIUM, and `pv` does NOT
// subtract `strike_dec` -- K is already inside the payoff. `VarOption.PvIsThe
// UndiscountedPremiumTimesDf` below is what pins that, because a double
// subtraction would stay plausible in both sign and magnitude.

// A shared fixture for the option tests: mid-life, so a > 0 and b < 1 and the
// DISPLACEMENT is actually exercised. An unaged fixture (a == 0, b == 1) makes
// V == W and every test built on it blind to the blend -- the same
// fixture-hides-the-parameter trap `make_flat_curves`' rate argument records.
[[nodiscard]] DerivContract mid_life_option(DerivKind kind, double strike_dec) {
  DerivContract c{};
  c.kind = kind;
  c.maturity_t = 0.25;
  c.strike_dec = strike_dec;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 21u;      // w_done = 1/3, so a = rv_done/3 and b = 2/3
  c.rv_spec.rv_done_dec = 0.0324;  // 18 vol realized so far
  return c;
}

// ORACLE 1. Put-call parity, E[(W-k)+] - E[(k-W)+] == m - k, at the CLOSED-FORM
// level. This is a real test only because `lognormal_put` is its own formula
// (k*Phi(-d2) - m*Phi(-d1)) rather than a rearrangement of `lognormal_call`'s
// result -- had the pricer computed the put as call - (m-k), this identity
// would hold by construction and assert nothing.
//
// Swept across moneyness AND both degenerate edges (k <= 0, s <= 0), because
// those take different branches in each formula and a parity break there would
// be invisible to an at-the-money-only check.
TEST(VarOption, PutCallParity) {
  for (const double m : {0.01, 0.04, 0.16}) {
    for (const double s : {0.0, 0.05, 0.40, 1.20}) {
      for (const double k : {-0.02, 0.0, 0.001, 0.02, 0.04, 0.09, 0.50}) {
        const double call = lognormal_call(m, s, k);
        const double put = lognormal_put(m, s, k);
        EXPECT_NEAR(call - put, m - k, 1e-12)
            << "m=" << m << " s=" << s << " k=" << k;
        // Both legs are expectations of non-negative payoffs.
        EXPECT_GE(call, 0.0) << "m=" << m << " s=" << s << " k=" << k;
        EXPECT_GE(put, 0.0) << "m=" << m << " s=" << s << " k=" << k;
      }
    }
  }
}

// The same identity one level up, through `deriv_price`: a variance call minus
// a variance put on the identical contract is a variance SWAP struck at K, so
//     call_premium - put_premium == E[V] - K
// with E[V] taken from the plain VarSwap arm -- a third dispatch arm and a
// different function, which is what makes this more than an algebraic restating
// of the test above.
TEST(VarOption, PutCallParityThroughDispatch) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.80;  // explicit xi: no calibration variance between arms

  // LOAD-BEARING SWEEP MEMBER, do not narrow (Task F-5 fix round 1). The
  // fixture's accrued leg is a = (21/63)*0.0324 = 0.0108, and `k = 0.005` is the
  // only strike below it. There the call is deep in the money, so its premium is
  // the LINEAR a + b*m - K -- which is what makes this test measure the future
  // leg b*m directly rather than merely observe that some option was priced.
  //
  // Round 1's review established that by execution: patching in the WRONG
  // "call pins to a - K at a >= K" behaviour the task brief specified makes
  // three tests fail here, and the quantity dropped measures 0.0266666671 --
  // exactly b*m -- against a 0.01 intrinsic. Drop 0.005 from this list and that
  // catch disappears silently, leaving b*m pinned only by the call's T == 0
  // refusal in `PutPinsWhenAccruedPassesTheStrikeButCallDoesNot`, which is a
  // strictly weaker statement.
  for (const double k : {0.005, 0.02, 0.036, 0.05, 0.12}) {
    const auto call = deriv_price(surf, cs, mid_life_option(DerivKind::VarianceCall, k), cfg);
    const auto put = deriv_price(surf, cs, mid_life_option(DerivKind::VariancePut, k), cfg);
    ASSERT_TRUE(call.has_value()) << call.error().to_string();
    ASSERT_TRUE(put.has_value()) << put.error().to_string();

    DerivContract sw = mid_life_option(DerivKind::VarSwap, k);
    const auto swap = deriv_price(surf, cs, sw, cfg);
    ASSERT_TRUE(swap.has_value()) << swap.error().to_string();

    // E[V] is the swap's fair strike; K is the option strike.
    EXPECT_NEAR(call->fair_strike_dec - put->fair_strike_dec,
                swap->fair_strike_dec - k, 1e-12)
        << "k=" << k;
    // And the option quote's own decomposition agrees with the swap's E[V].
    EXPECT_NEAR(call->accrued_component_dec + call->future_component_dec,
                swap->fair_strike_dec, 1e-12)
        << "k=" << k;
  }
}

// ORACLE 2. A variance CALL struck at C reproduces a capped variance swap's
// `cap_option_value_dec` at cap C: both are E[(V-C)+] under the same model.
//
// NON-VACUITY. This is an IDENTITY between two expressions in one file, so it
// survives a reversion that aliases one pricer onto the other and CANNOT
// demonstrate on its own that the new code path ran. Five witnesses are
// asserted alongside it, none of them an error-string sentinel.
//
// READ W1-W4 FOR WHAT THEY ACTUALLY DISCRIMINATE, not as a count. Fix round 1
// falsified the original claim that they were four INDEPENDENT witnesses:
// injecting s = xi*T for xi*sqrt(T) into BOTH pricers left the identity, W1, W2
// and W3 all green, and only W4 fired -- by luck of sign, since that particular
// error shrank the value below its floor. The structural reason is that W1-W3
// are every one of them computed DOWNSTREAM of the same (m, s) resolution the
// identity itself depends on, so an error in that shared resolution moves them
// together and none can see it. An identity oracle plus witnesses drawn from
// the identity's own inputs cannot exceed the identity's own blind spot.
//
//   W1 DISPATCH-LEVEL. The solve ledger's VarSwapStripEvals must advance by
//      exactly 1 across the variance-call price. Catches a pricer that never
//      priced (0) or double-priced (2). Does NOT catch aliasing: the capped
//      pricer calls `var_swap_fair_strike` exactly once too.
//   W2 CROSS-ARM CONSISTENCY. E[V] - E[min(V,C)] == E[(V-C)+] across the
//      VarSwap, capped and option arms. Catches the three arms DISAGREEING
//      about m, the blend weights or the discrete correction. It says nothing
//      about the premium itself: substituting the arms reduces it to
//      capopt == capopt, an algebraic restating, not an independent check.
//   W3 STRIKE DISCRIMINATION. The identity must FAIL by orders of magnitude
//      more than its own tolerance when the call is struck anywhere other than
//      C, and the premium must fall in the strike. Catches a premium that
//      ignores `strike_dec`. Does NOT catch a correct-shaped curve over wrong
//      model parameters.
//   W4 NON-DEGENERACY. A floor, so none of the above is 0 == 0. Not a designed
//      discriminator; it caught round 1's injection only by accident of sign.
//   W5 PARAMETER-INDEPENDENT. The one witness NOT downstream of the pricer's
//      own (m, s) -- see its own block below for exactly what is rebuilt here
//      and what is still shared.
TEST(VarOption, CappedSwapIdentity) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.80;

  const double cap = 0.05;  // near the money against K_var ~ 0.04

  DerivContract capped = mid_life_option(DerivKind::CappedVarSwap, 0.0);
  capped.cap_dec = cap;
  const auto cq = deriv_price(surf, cs, capped, cfg);
  ASSERT_TRUE(cq.has_value()) << cq.error().to_string();

  // W1: measure the strip-eval delta ACROSS the variance-call price only.
  ledger::reset();
  const std::uint64_t before = ledger::snapshot().get(ledger::Solve::VarSwapStripEvals);
  const auto call = deriv_price(surf, cs, mid_life_option(DerivKind::VarianceCall, cap), cfg);
  const std::uint64_t after = ledger::snapshot().get(ledger::Solve::VarSwapStripEvals);
  ASSERT_TRUE(call.has_value()) << call.error().to_string();
  EXPECT_EQ(after - before, 1u)
      << "the variance-option arm must run exactly one variance strip of its own";

  // THE IDENTITY.
  EXPECT_NEAR(call->fair_strike_dec, cq->cap_option_value_dec, 1e-12);

  // W4: and it is a real number, not zero.
  EXPECT_GT(cq->cap_option_value_dec, 1e-4);

  // W2: E[V] from the VarSwap arm, minus E[min(V,C)] from the capped arm, is
  // the call premium. Three dispatch arms, one number.
  const auto swap = deriv_price(surf, cs, mid_life_option(DerivKind::VarSwap, 0.0), cfg);
  ASSERT_TRUE(swap.has_value()) << swap.error().to_string();
  EXPECT_NEAR(swap->fair_strike_dec - cq->fair_strike_dec, call->fair_strike_dec, 1e-12);

  // W3: struck anywhere else, the identity breaks by far more than 1e-12. The
  // margin is asserted against the tolerance, not eyeballed.
  for (const double k : {cap * 0.5, cap * 0.9, cap * 1.1, cap * 2.0}) {
    const auto off = deriv_price(surf, cs, mid_life_option(DerivKind::VarianceCall, k), cfg);
    ASSERT_TRUE(off.has_value()) << off.error().to_string();
    EXPECT_GT(std::abs(off->fair_strike_dec - cq->cap_option_value_dec), 1e-6)
        << "k=" << k << ": the premium is not a function of the option strike";
  }
  // ... and monotone decreasing in the strike, which a constant also fails.
  const auto lo = deriv_price(surf, cs, mid_life_option(DerivKind::VarianceCall, 0.03), cfg);
  const auto hi = deriv_price(surf, cs, mid_life_option(DerivKind::VarianceCall, 0.09), cfg);
  ASSERT_TRUE(lo.has_value());
  ASSERT_TRUE(hi.has_value());
  EXPECT_GT(lo->fair_strike_dec, hi->fair_strike_dec);

  // ── W5: the parameter-independent leg (fix round 2) ─────────────────────
  //
  // W1-W3 are all downstream of the pricer's own (m, s), which is why round 1's
  // s = xi*T injection walked through them. This leg rebuilds the model's inputs
  // HERE and prices the payoff by a DIFFERENT method, so it is not downstream of
  // anything the identity uses.
  //
  // INDEPENDENCE, ITEM BY ITEM. The question for each is "could the pricer get
  // this wrong without moving this assertion?" -- and the answer has to be no,
  // or the item is not independent and is labelled as such.
  //
  //   a, b, k_w  LITERALS, re-derived from `mid_life_option`'s own fixture
  //              constants. Nothing is read back off any quote.
  //   s          0.80*sqrt(0.25) from LITERALS. `resolve_vol_of_vol` is never
  //              called and no quote's `vol_of_vol_used` is read. This is the
  //              item that catches round 1's injection.
  //   payoff     GL-64 QUADRATURE of the smooth piece above the kink -- NOT
  //              `lognormal_call`. A different computational route, so an error
  //              inside the closed form is visible here. Same technique and same
  //              1e-9*m tolerance `RvLognormal.CallMatchesQuadratureAndParity`
  //              already uses to validate that closed form directly.
  //   m          NOT INDEPENDENT on its own -- it comes from
  //              `var_swap_fair_strike`, the same function the pricer calls.
  //              Said plainly rather than glossed, because a witness that reads
  //              as independent while sharing an input is the exact defect this
  //              leg exists to correct. It is tied down SEPARATELY below,
  //              against the flat fixture's analytic truth sigma^2 = 0.04, which
  //              is what makes a broken strip visible here too.
  //
  // WHAT IT STILL SHARES, stated for the same reason: `lognormal_truncated_
  // expect` and, beneath it, `norm_cdf`. Those are pinned independently by
  // `TruncatedExpect.FullIntervalAndSplitRecoverMean` and
  // `RvLognormal.NormCdfKnownValues`. W5 therefore does NOT subsume
  // `VarOption.MCOracle`, which remains the only check that DRAWS from the
  // distribution rather than integrating it.
  const double w_done = 21.0 / 63.0;      // mid_life_option's own fixture
  const double b_leg = 1.0 - w_done;
  const double a_leg = w_done * 0.0324;   // ditto: rv_done_dec
  const double s_leg = 0.80 * std::sqrt(0.25);  // cfg.vol_of_vol * sqrt(T)

  const auto kv = var_swap_fair_strike(surf, cs, 0.25, cfg);
  ASSERT_TRUE(kv.has_value()) << kv.error().to_string();
  const double m_leg = kv->fair_strike_dec;
  // m's own independent tie-down: on a flat sigma = 0.20 surface the model-free
  // variance strip integrates to sigma^2 = 0.04 in the continuum, so the only
  // gap is this strip's quadrature/truncation error. The tolerance is the
  // MEASURED deviation with headroom, not a guess: tightening this assertion to
  // failure reports 7.4132445321284379e-10 at Standard quality on this fixture,
  // so 2e-9 is ~2.7x that -- tight enough that a strip returning the wrong LEVEL
  // fails by orders of magnitude, loose enough not to pin quadrature noise.
  EXPECT_NEAR(m_leg, 0.04, 2e-9);

  const double k_w = (cap - a_leg) / b_leg;
  // The kink's standard-normal coordinate: W(z) = m*exp(s*z - s^2/2) == k_w.
  const double z_star = (std::log(k_w / m_leg) + 0.5 * s_leg * s_leg) / s_leg;
  const double premium_quad =
      b_leg * lognormal_truncated_expect(m_leg, s_leg, z_star, 8.0,
                                         [k_w](double w) { return w - k_w; });
  // Tolerance is again the MEASURED residual with headroom: tightening this to
  // failure reports 3.7014662168655121e-16 -- essentially machine epsilon
  // against a ~1.14e-3 premium, because GL-64 resolves this smooth integrand
  // that well. 1e-14 is ~27x that. It is deliberately far TIGHTER than the
  // 1e-9*m the sibling `RvLognormal.CallMatchesQuadratureAndParity` uses,
  // because a loose tolerance here would blunt the one witness that can see a
  // wrong (m, s).
  //
  // MEASURED DISCRIMINATION. Round 1's s = xi*T injection moves the priced
  // premium from 1.1408537360273927e-3 to 6.6480398422724785e-05 while this
  // leg's expected value -- built from s = 0.80*sqrt(0.25) HERE -- does not
  // move at all. The gap W5 would report is 1.074373337604668e-3, i.e. 1.07e11
  // times this tolerance. (Same measurement also confirms W4's catch was luck:
  // the injected premium lands at 6.65e-5, just under W4's 1e-4 floor. An
  // injection of the opposite sign raises the premium and W4 passes.)
  EXPECT_NEAR(call->fair_strike_dec, premium_quad, 1e-14);
}

// ORACLE 3. Model-vs-MC on the LOGNORMAL's own terms: W is drawn directly from
// the lognormal the pricer assumes, NOT from Black-Scholes spot paths. That is
// deliberate and it is what makes this a test of the PRICER rather than of the
// model -- a BS-path harness would fold the (real, documented, LIT-5) gap
// between realized-variance dynamics and the lognormal proxy into the same
// residual as an arithmetic error in the closed form, and the two are not
// separable at any feasible path count.
//
// Both kinds and both aging regimes are swept, at a strike that is neither
// deep in nor deep out of the money, so the MC residual is dominated by the
// payoff's own variance rather than by rare-event noise.
TEST(VarOption, MCOracle) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.80;  // explicit, so s is known here without reading it back

  const double T = 0.25;
  const double s = 0.80 * std::sqrt(T);
  const auto kv = var_swap_fair_strike(surf, cs, T, cfg);
  ASSERT_TRUE(kv.has_value());
  const double m = kv->fair_strike_dec;  // the same mean the pricer resolves

  constexpr int kPaths = 400000;
  for (const bool aged : {false, true}) {
    // a and b exactly as the pricer's own blend forms them.
    const double w_done = aged ? 21.0 / 63.0 : 0.0;
    const double a = aged ? w_done * 0.0324 : 0.0;
    const double b = 1.0 - w_done;

    for (const DerivKind kind : {DerivKind::VarianceCall, DerivKind::VariancePut}) {
      DerivContract c = mid_life_option(kind, 0.045);
      if (!aged) {
        c.rv_spec.n_obs_done = 0u;
        c.rv_spec.rv_done_dec = 0.0;
      }
      const auto q = deriv_price(surf, cs, c, cfg);
      ASSERT_TRUE(q.has_value()) << q.error().to_string();

      std::mt19937_64 rng(20260814u);
      std::normal_distribution<double> z(0.0, 1.0);
      double sum = 0.0;
      double sum_sq = 0.0;
      for (int i = 0; i < kPaths; ++i) {
        const double w = m * std::exp(s * z(rng) - 0.5 * s * s);
        const double v = a + b * w;
        const double payoff = (kind == DerivKind::VarianceCall) ? std::fmax(v - 0.045, 0.0)
                                                                : std::fmax(0.045 - v, 0.0);
        sum += payoff;
        sum_sq += payoff * payoff;
      }
      const double mean = sum / kPaths;
      const double var = sum_sq / kPaths - mean * mean;
      const double se = std::sqrt(std::fmax(var, 0.0) / kPaths);
      ASSERT_GT(se, 0.0);
      EXPECT_LT(std::abs(q->fair_strike_dec - mean), 3.0 * se)
          << "kind=" << static_cast<int>(kind) << " aged=" << aged
          << " model=" << q->fair_strike_dec << " mc=" << mean << " se=" << se;
    }
  }
}

// PV is df*N*premium with NO strike subtraction -- the one place these kinds
// depart from every swap's `df*N*(expectation - strike)` bookkeeping. A double
// subtraction would leave both sign and magnitude plausible, so it is pinned
// against an independently-computed df rather than inferred from a ratio.
TEST(VarOption, PvIsTheUndiscountedPremiumTimesDf) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, /*rate=*/0.03);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.80;
  const DerivContract c = mid_life_option(DerivKind::VarianceCall, 0.045);
  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value()) << q.error().to_string();

  const double df = cs.yield.disc(0.25);
  EXPECT_LT(df, 1.0);  // the fixture really discounts; otherwise this is blind
  EXPECT_NEAR(q->pv, df * 1e6 * q->fair_strike_dec, 1e-9);
  // The headline field and the expectation field are the same number, as on
  // every other kind.
  EXPECT_EQ(q->fair_strike_dec, q->undiscounted_expectation_dec);
  EXPECT_NEAR(q->fair_strike_points, 1.0e4 * q->fair_strike_dec, 1e-12);
  // Uncapped kind: no cap haircut is ever computed.
  EXPECT_EQ(q->cap_option_value_dec, 0.0);
}

// Fully aged: V == a exactly, so both payoffs collapse to their intrinsic value
// with no strip and no model. Exercised at T == 0, the case that must not
// require a strip. Two strikes per kind, straddling a, so the max() actually
// binds in one direction and not the other.
TEST(VarOption, FullyAgedPaysIntrinsic) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const double rv = 0.0441;  // 21 vol realized; a == rv when fully aged

  struct Case {
    DerivKind kind;
    double strike;
    double expect;
  };
  for (const Case cs_i : {Case{DerivKind::VarianceCall, 0.0400, 0.0041},
                          Case{DerivKind::VarianceCall, 0.0500, 0.0},
                          Case{DerivKind::VariancePut, 0.0500, 0.0059},
                          Case{DerivKind::VariancePut, 0.0400, 0.0}}) {
    DerivContract c{};
    c.kind = cs_i.kind;
    c.maturity_t = 0.0;
    c.strike_dec = cs_i.strike;
    c.notional = 1e5;
    c.rv_spec.annualization = 252.0;
    c.rv_spec.n_obs_total = 63u;
    c.rv_spec.n_obs_done = 63u;
    c.rv_spec.rv_done_dec = rv;
    const auto q = deriv_price(surf, cs, c, deriv_default_config());
    ASSERT_TRUE(q.has_value()) << q.error().to_string();
    EXPECT_NEAR(q->fair_strike_dec, cs_i.expect, 1e-15)
        << "kind=" << static_cast<int>(cs_i.kind) << " k=" << cs_i.strike;
    EXPECT_NEAR(q->pv, 1e5 * cs_i.expect, 1e-9);
    EXPECT_TRUE(has_flag(q->flags, DerivFlags::FullyAged));
    EXPECT_FALSE(has_flag(q->flags, DerivFlags::ModelProxy));
    // FullyAged is its OWN exit, not the put pin: `OptionPinned` must stay
    // clear here even for the put rows, or the two deterministic paths become
    // indistinguishable and the flag stops meaning "a >= K mid-life".
    EXPECT_FALSE(has_flag(q->flags, DerivFlags::OptionPinned));
    EXPECT_TRUE(std::isnan(q->vol_of_vol_used));  // no distribution model ran
  }
}

// The PUT pin, and the asymmetry that is the easiest thing about these kinds to
// get backwards. A put whose accrued leg alone already reached its strike is
// worth exactly 0 -- deterministic, no strip, and valid at T == 0 where no
// strip could run. A CALL in the same position is NOT pinned: exercise is
// certain but the value is still a + b*m - K and needs the strip.
TEST(VarOption, PutPinsWhenAccruedPassesTheStrikeButCallDoesNot) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.80;

  // Mid-life, T == 0: no strip is available, so only a pinned path can answer.
  DerivContract put{};
  put.kind = DerivKind::VariancePut;
  put.maturity_t = 0.0;
  put.strike_dec = 0.02;
  put.notional = 1e6;
  put.rv_spec.annualization = 252.0;
  put.rv_spec.n_obs_total = 63u;
  put.rv_spec.n_obs_done = 21u;
  put.rv_spec.rv_done_dec = 0.09;  // a = 0.03 > 0.02 = K
  const auto pq = deriv_price(surf, cs, put, cfg);
  ASSERT_TRUE(pq.has_value()) << pq.error().to_string();
  EXPECT_EQ(pq->fair_strike_dec, 0.0);
  EXPECT_EQ(pq->pv, 0.0);
  EXPECT_FALSE(has_flag(pq->flags, DerivFlags::ModelProxy));
  // Fix round 1: the pin is OBSERVABLE, not merely correct. A caller must be
  // able to tell "dead by accrual" from "cheap by model" -- both quote ~0.
  EXPECT_TRUE(has_flag(pq->flags, DerivFlags::OptionPinned));
  EXPECT_FALSE(has_flag(pq->flags, DerivFlags::FullyAged));  // genuinely mid-life
  // And the flag marks the one path where E[V] != accrued + future: no strip
  // ran, so the future leg is 0 in the NOT-COMPUTED sense while b > 0.
  EXPECT_EQ(pq->future_component_dec, 0.0);
  EXPECT_GT(pq->accrued_component_dec, 0.0);

  // The call on the identical contract fails at T == 0 -- it genuinely needs
  // the future leg it cannot price. That refusal IS the asymmetry.
  DerivContract call = put;
  call.kind = DerivKind::VarianceCall;
  const auto cq0 = deriv_price(surf, cs, call, cfg);
  ASSERT_FALSE(cq0.has_value());
  EXPECT_EQ(cq0.error().code(), ErrorCode::InvalidArgument);

  // At T > 0 the call prices, and is worth strictly more than its intrinsic
  // a - K: certain exercise still carries the future leg's b*m.
  call.maturity_t = 0.25;
  const auto cq = deriv_price(surf, cs, call, cfg);
  ASSERT_TRUE(cq.has_value()) << cq.error().to_string();
  EXPECT_GT(cq->fair_strike_dec, 0.03 - 0.02);
  EXPECT_TRUE(has_flag(cq->flags, DerivFlags::ModelProxy));
  // The call at a >= K is NOT pinned -- that is the whole asymmetry, and the
  // flag has to say so or it would be recording "a >= K" rather than "pinned".
  EXPECT_FALSE(has_flag(cq->flags, DerivFlags::OptionPinned));
  // Deep in the money by construction, so it is exactly the linear value.
  EXPECT_NEAR(cq->fair_strike_dec,
              cq->accrued_component_dec + cq->future_component_dec - 0.02, 1e-12);
}

// C-4 matrix, both new kinds: {Auto, RvDistributionProxy} price, the other two
// engines are refused, and the two legal engines agree exactly (nothing in the
// pricer reads `cfg.engine`).
TEST(VarOption, DispatchMatrix) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  for (const DerivKind kind : {DerivKind::VarianceCall, DerivKind::VariancePut}) {
    const DerivContract c = mid_life_option(kind, 0.045);

    DerivConfig autocfg = deriv_default_config();
    DerivConfig proxy = deriv_default_config();
    proxy.engine = DerivEngine::RvDistributionProxy;
    const auto qa = deriv_price(surf, cs, c, autocfg);
    const auto qp = deriv_price(surf, cs, c, proxy);
    ASSERT_TRUE(qa.has_value()) << qa.error().to_string();
    ASSERT_TRUE(qp.has_value()) << qp.error().to_string();
    EXPECT_EQ(qa->fair_strike_dec, qp->fair_strike_dec);
    EXPECT_GT(qa->fair_strike_dec, 0.0);

    for (const DerivEngine e : {DerivEngine::StripLogContract, DerivEngine::VolCarrLee}) {
      DerivConfig bad = deriv_default_config();
      bad.engine = e;
      const auto q = deriv_price(surf, cs, c, bad);
      ASSERT_FALSE(q.has_value()) << "kind=" << static_cast<int>(kind);
      EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
    }
    for (const DerivEngine e : {DerivEngine::RvDistributionAffine, DerivEngine::McQe}) {
      DerivConfig bad = deriv_default_config();
      bad.engine = e;
      const auto q = deriv_price(surf, cs, c, bad);
      ASSERT_FALSE(q.has_value()) << "kind=" << static_cast<int>(kind);
      EXPECT_EQ(q.error().code(), ErrorCode::NotImplemented);
    }
  }
}

// Scope-gated fields: an option carries an option strike, never a cap and never
// corridor bounds. Both rules come free from `validate_deriv_dispatch`'s
// existing uncapped/non-corridor branches -- which is exactly why they are
// tested rather than assumed, since "free by omission" is how the corridor rule
// itself went missing on one lane at F-3.
TEST(VarOption, CapAndCorridorFieldsRejected) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  for (const DerivKind kind : {DerivKind::VarianceCall, DerivKind::VariancePut}) {
    DerivContract capped = mid_life_option(kind, 0.045);
    capped.cap_dec = 0.09;
    const auto q1 = deriv_price(surf, cs, capped, deriv_default_config());
    ASSERT_FALSE(q1.has_value()) << "kind=" << static_cast<int>(kind);
    EXPECT_EQ(q1.error().code(), ErrorCode::InvalidArgument);

    DerivContract corr = mid_life_option(kind, 0.045);
    corr.corridor_lo = 95.0;
    corr.corridor_hi = 105.0;
    const auto q2 = deriv_price(surf, cs, corr, deriv_default_config());
    ASSERT_FALSE(q2.has_value()) << "kind=" << static_cast<int>(kind);
    EXPECT_EQ(q2.error().code(), ErrorCode::InvalidArgument);
  }
}

// The auto-calibrated vol-of-vol path (cfg.vol_of_vol == 0), and the direction
// the model must move in xi: more dispersion in W is worth more to BOTH an
// out-of-the-money call and an out-of-the-money put. A pricer that ignored xi
// -- returning intrinsic, say -- passes the parity and identity oracles above
// and fails here.
TEST(VarOption, PremiumIncreasesWithVolOfVol) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivConfig autocfg = deriv_default_config();  // vol_of_vol == 0 -> calibrate
  const auto q_auto = deriv_price(surf, cs, mid_life_option(DerivKind::VarianceCall, 0.06),
                                  autocfg);
  ASSERT_TRUE(q_auto.has_value()) << q_auto.error().to_string();
  EXPECT_TRUE(has_flag(q_auto->flags, DerivFlags::VolOfVolCalibrated));
  EXPECT_GT(q_auto->vol_of_vol_used, 0.0);

  // STRIKE CHOICE, recorded because the first attempt got it wrong and the
  // test caught it: the fixture's accrued leg is a = (21/63)*0.0324 = 0.0108,
  // so a put struck at 0.01 is PINNED (a >= K) and worth exactly 0 at every xi.
  // That is correct pricer behaviour and a useless monotonicity fixture. The
  // put strike below sits strictly above a and strictly below E[V] ~ 0.0375, so
  // it is genuinely out of the money with real time value to gain.
  double prev_call = -1.0;
  double prev_put = -1.0;
  for (const double xi : {0.20, 0.60, 1.00}) {
    DerivConfig cfg = deriv_default_config();
    cfg.vol_of_vol = xi;
    const auto call = deriv_price(surf, cs, mid_life_option(DerivKind::VarianceCall, 0.06), cfg);
    const auto put = deriv_price(surf, cs, mid_life_option(DerivKind::VariancePut, 0.025), cfg);
    ASSERT_TRUE(call.has_value()) << call.error().to_string();
    ASSERT_TRUE(put.has_value()) << put.error().to_string();
    EXPECT_EQ(call->vol_of_vol_used, xi);
    EXPECT_FALSE(has_flag(call->flags, DerivFlags::VolOfVolCalibrated));
    EXPECT_GT(call->fair_strike_dec, prev_call) << "xi=" << xi;
    EXPECT_GT(put->fair_strike_dec, prev_put) << "xi=" << xi;
    prev_call = call->fair_strike_dec;
    prev_put = put->fair_strike_dec;
  }
}

// The discrete-monitoring correction reaches the option's future leg exactly as
// it reaches a plain VarSwap's -- the same consistency `CappedVarSwap.Discrete
// CorrectionAppliesToFutureLegConsistently` pins for the capped kinds. Read off
// a deep-in-the-money call, where the premium is linear in the corrected mean
// and the two must therefore agree to machine precision.
TEST(VarOption, DiscreteCorrectionReachesTheFutureLeg) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.vol_of_vol = 0.80;
  cfg.discrete_correction_mode = DerivDiscreteCorrection::Diffusion1OverN;

  const auto swap = deriv_price(surf, cs, mid_life_option(DerivKind::VarSwap, 0.0), cfg);
  ASSERT_TRUE(swap.has_value()) << swap.error().to_string();
  ASSERT_TRUE(has_flag(swap->flags, DerivFlags::DiscreteCorrApplied));

  // Strike 0: certain exercise, so premium == E[V] exactly.
  const auto call = deriv_price(surf, cs, mid_life_option(DerivKind::VarianceCall, 0.0), cfg);
  ASSERT_TRUE(call.has_value()) << call.error().to_string();
  EXPECT_TRUE(has_flag(call->flags, DerivFlags::DiscreteCorrApplied));
  EXPECT_NEAR(call->fair_strike_dec, swap->fair_strike_dec, 1e-12);

  // Without the correction the same call is a DIFFERENT number, so the
  // assertion above is not satisfied by the correction being a no-op.
  DerivConfig plain = cfg;
  plain.discrete_correction_mode = DerivDiscreteCorrection::None;
  const auto uncorrected = deriv_price(surf, cs, mid_life_option(DerivKind::VarianceCall, 0.0),
                                       plain);
  ASSERT_TRUE(uncorrected.has_value());
  EXPECT_GT(call->fair_strike_dec, uncorrected->fair_strike_dec);
}
}  // namespace
