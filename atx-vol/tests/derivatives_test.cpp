#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/arb.hpp"                  // Task F-4: kCalendarTotalVarianceTol
#include "atx/vol/black76.hpp"              // WingClamp oracle repricing
#include "atx/vol/derivatives.hpp"
#include "atx/vol/detail/counters.hpp"      // ledger:: (Corridor dispatch witness)
#include "atx/vol/detail/strip_grid.hpp"    // strip::simpson_weight (WingClamp oracle)
#include "atx/vol/detail/legacy_surface.hpp"  // EssviSurface (demoted, S4-T21)
#include "atx/vol/detail/risk_surface_validation.hpp" // RiskSurfaceValidationConfig (MUST-FIX 2)
#include "atx/vol/priced_surface.hpp"  // E6: PricedSurface-native overloads
#include "atx/vol/rates_curve.hpp"
#include "atx/vol/surface.hpp"
#include "atx/vol/surface_policy.hpp" // FitQualityMode, certified_wing_half_band (Task C-6)
#include "atx/vol/vol_curve.hpp"    // CurveSurface, EssviCurve (Task C-6 PricedSurface fixture)
#include "atx/vol/vol_surface.hpp" // Tier-A instantiation set (closeout 1.2)
#include "deriv_fixtures.hpp" // Task 0: deriv_testkit::make_curves / MC oracle
#include "support/analytics_fixture.hpp" // E6: testkit::make_flat_surface

namespace atx::vol {
// Test-only seam (MUST-FIX 2 / C-6 I-6): `risk_validation_config` is pricer_
// fitter.cpp's own FitQualityMode -> k_min/k_max table (the fit-time band the
// independent risk oracle actually certifies); it has external linkage but no
// header declares it, since `surface_policy.hpp`'s `certified_wing_half_band`
// is a hand-kept COPY of its k_max column for the pricing-time read path, and
// only the Balanced case is statically cross-checked (derivatives.cpp) --
// Latency/Accuracy could drift apart from `risk_validation_config` with no
// test catching it, silently re-creating FIT-C7. Forward-declared here,
// exactly as `qp_active_set_for_test` reaches into `dense_slice.cpp` (C-7):
// no header touched, production call sites (pricer_fitter.cpp) untouched.
[[nodiscard]] RiskSurfaceValidationConfig
risk_validation_config(FitQualityMode quality_mode) noexcept;

namespace detail {
// Task P-3 test seam: forces `var_swap_fair_strike` back onto its original
// per-node scalar surface-read loop even when `SurfaceT` (a PricedSurface-
// backed strip view) exposes the batched `iv_batch` gather path -- see
// `has_strip_iv_batch`/`g_strip_batch_disabled` (derivatives.cpp). Same
// forward-declaration precedent as `risk_validation_config` above: external
// linkage, no header touched, production call sites never invoke it.
// `Strip.BatchedMatchesScalar*` below uses it to run the SAME (surface, T,
// cfg) through both the batched and scalar loops and assert exact bit
// equality.
void set_strip_batch_disabled_for_test(bool disabled) noexcept;
}  // namespace detail
}  // namespace atx::vol

// Vol-derivatives coverage, ported from the C ats-vol Sprint-22 tests:
//   test_vol_deriv_var_strip.c    -> VarStrip (flat-vol strip recovers sigma^2)
//   test_vol_deriv_aged.c         -> AgedDispatch (variance blend + discrete corr)
//   test_vol_deriv_marquee_pnl.c  -> Marquee (vol-swap PnL identity end to end)
//   test_realized_tracker.c       -> RealizedTracker (EWMA-free close-to-close RV)
//   test_deriv_reserved_validation.c -> ReservedValidation (non-zero reserved
//                                       fields rejected)
//
// The C fixtures built an eSSVI surface via the natural->reparam helper; this
// port sets theta/phi/rho directly on the base eSSVI slice (the atx surface
// evaluator consumes those), which is numerically the same flat surface.

namespace {

using atx::vol::CurveSet;
using atx::vol::deriv_default_config;
using atx::vol::deriv_price;
using atx::vol::DerivConfig;
using atx::vol::DerivContract;
using atx::vol::DerivDiscreteCorrection;
using atx::vol::DerivEngine;
using atx::vol::DerivFlags;
using atx::vol::DerivKind;
using atx::vol::DerivMarkingConvention;
using atx::vol::DerivQuality;
using atx::vol::ErrorCode;
using atx::vol::EssviSlice;
using atx::vol::EssviSurface;
using atx::vol::ForwardPoint;
using atx::vol::has_flag;
using atx::vol::RealizedTracker;
using atx::vol::RealizedVarianceSpec;
using atx::vol::StripWingMode;
using atx::vol::SviSlice;
using atx::vol::SviSurface;
using atx::vol::var_swap_fair_strike;
using atx::vol::vol_swap_fair_strike;

// Flat-vol synthetic surface: two eSSVI slices with theta = sigma^2 * T and
// near-zero curvature (phi ~ 0, rho = 0) so w(k) is essentially flat in k and
// iv(0, T) == sigma exactly for any T in [T_lo, T_hi].
EssviSurface make_flat_surface(double sigma, double T_lo, double T_hi) {
  EssviSurface surf(2);
  const EssviSlice s0{sigma * sigma * T_lo, 1.0e-6, 0.0, T_lo};
  const EssviSlice s1{sigma * sigma * T_hi, 1.0e-6, 0.0, T_hi};
  EXPECT_TRUE(surf.set_slice(0, s0).has_value());
  EXPECT_TRUE(surf.set_slice(1, s1).has_value());
  return surf;
}

// Zero-rate curve set with F == spot at both forward pillars (no carry).
CurveSet make_flat_curves(double spot, double T_lo, double T_hi) {
  CurveSet cs;
  cs.spot = spot;
  const double t[] = {T_lo * 0.5, (T_lo + T_hi) * 0.5, T_hi * 2.0};
  const double r[] = {0.0, 0.0, 0.0};
  EXPECT_TRUE(cs.set_yield(t, r).has_value());
  std::vector<ForwardPoint> pts(2);
  pts[0].T = T_lo;
  pts[0].F = spot;
  pts[1].T = T_hi;
  pts[1].F = spot;
  cs.forward.set(pts);
  return cs;
}

// ── Variance strip (test_vol_deriv_var_strip.c) ──────────────────────────

TEST(VarStrip, FlatVol_StandardQuality_RecoversSigmaSquared) {
  const double spot = 100.0;
  const double sigma_atm = 0.20;
  const double T_test = 0.10;  // ~25 trading days
  const EssviSurface surf = make_flat_surface(sigma_atm, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(spot, 0.01, 1.00);

  // Sanity: surface IV at ATM equals sigma_atm.
  EXPECT_LT(std::fabs(surf.iv(0.0, T_test) - sigma_atm), 1.0e-7);

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::Standard;

  const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
  ASSERT_TRUE(q.has_value());

  const double k_var_expected = sigma_atm * sigma_atm;
  EXPECT_LT(std::fabs(q->fair_strike_dec - k_var_expected), 5.0e-5);
  EXPECT_GT(q->fair_strike_dec, 0.0);
}

TEST(VarStrip, FlatVol_HighQuality_RecoversSigmaSquaredTighter) {
  const double sigma_atm = 0.20;
  const double T_test = 0.10;
  const EssviSurface surf = make_flat_surface(sigma_atm, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::High;

  const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
  ASSERT_TRUE(q.has_value());
  EXPECT_LT(std::fabs(q->fair_strike_dec - sigma_atm * sigma_atm), 1.0e-5);
}

// ── Tier-A instantiation set: VolSurface (closeout 1.2) ──────────────────
//
// `VolSurface` is the Tier-A member of the supported `SurfaceT` set (see the
// note above `deriv_price` in derivatives.hpp). These pin (a) that the
// instantiations LINK — a Tier-A caller holding a VolSurface + CurveSet can
// reach every templated entry without including a `detail/` header — and (b)
// that VolSurface satisfies the template's contract numerically, by agreeing
// with the demoted container on the same flat surface.

// The same flat two-slice eSSVI stack as `make_flat_surface`, in the Tier-A
// container: theta = sigma^2 * T, phi ~ 0, rho = 0.
atx::vol::VolSurface make_flat_vol_surface(double sigma, double T_lo, double T_hi) {
  atx::vol::VolSurface surf =
      atx::vol::VolSurface::create(11u, atx::vol::Parametrization::Essvi, 2).value();
  const double Ts[2] = {T_lo, T_hi};
  for (std::uint16_t i = 0; i < 2; ++i) {
    atx::vol::EssviParams s{};
    s.theta = sigma * sigma * Ts[i];
    s.phi = 1.0e-6;
    s.rho = 0.0;
    s.T = Ts[i];
    s.expiry_id = i;
    EXPECT_TRUE(surf.set_slice_essvi(i, s).has_value());
  }
  return surf;
}

TEST(DerivTierAInstantiation, VolSurfaceMatchesDemotedContainerOnFlatSurface) {
  const double spot = 100.0;
  const double sigma = 0.20;
  const double T_test = 0.10;
  const atx::vol::VolSurface vs = make_flat_vol_surface(sigma, 0.01, 1.00);
  const EssviSurface legacy = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(spot, 0.01, 1.00);

  EXPECT_LT(std::fabs(vs.iv(0.0, T_test) - sigma), 1.0e-7);

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::Standard;

  const auto q_vs = var_swap_fair_strike(vs, cs, T_test, cfg);
  const auto q_legacy = var_swap_fair_strike(legacy, cs, T_test, cfg);
  ASSERT_TRUE(q_vs.has_value());
  ASSERT_TRUE(q_legacy.has_value());
  EXPECT_LT(std::fabs(q_vs->fair_strike_dec - sigma * sigma), 5.0e-5);
  EXPECT_DOUBLE_EQ(q_vs->fair_strike_dec, q_legacy->fair_strike_dec);

  const auto v_vs = vol_swap_fair_strike(vs, cs, T_test, cfg);
  const auto v_legacy = vol_swap_fair_strike(legacy, cs, T_test, cfg);
  ASSERT_TRUE(v_vs.has_value());
  ASSERT_TRUE(v_legacy.has_value());
  EXPECT_DOUBLE_EQ(v_vs->fair_strike_dec, v_legacy->fair_strike_dec);
}

TEST(DerivTierAInstantiation, VolSurfacePricesAndDifferentiatesAContract) {
  const double sigma = 0.20;
  const atx::vol::VolSurface vs = make_flat_vol_surface(sigma, 0.01, 1.00);
  const EssviSurface legacy = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.25;
  c.notional = 1.0e6;
  c.strike_dec = sigma * sigma;

  const auto p_vs = deriv_price(vs, cs, c);
  const auto p_legacy = deriv_price(legacy, cs, c);
  ASSERT_TRUE(p_vs.has_value());
  ASSERT_TRUE(p_legacy.has_value());
  EXPECT_DOUBLE_EQ(p_vs->pv, p_legacy->pv);

  const auto g_vs = atx::vol::deriv_greeks(vs, cs, c);
  const auto g_legacy = atx::vol::deriv_greeks(legacy, cs, c);
  ASSERT_TRUE(g_vs.has_value());
  ASSERT_TRUE(g_legacy.has_value());
  EXPECT_DOUBLE_EQ(g_vs->pv, g_legacy->pv);
  EXPECT_DOUBLE_EQ(g_vs->vega, g_legacy->vega);
}

// ── E2 / AN-P1-2: adaptive var-strip wings ───────────────────────────────
//
// The quality tier fixed the log-strike span (Standard ±1.5) with no reference
// to σ√T. A σ = 60%, T = 1y name needs ±3.6 to reach 6σ√T, so the Standard
// strip integrated only the middle ~2.5σ of the distribution and reported
// K_var biased LOW — and, because a parametric eSSVI surface returns a finite
// IV at every k, the StripTruncated* flags never fired. Silently wrong.
//
// Truth for a flat-vol lognormal surface is K_var == σ² exactly.
TEST(VarStrip, HighVolLongTenor_AdaptiveWingsRecoverSigmaSquared) {
  const double spot = 100.0;
  const double sigma_atm = 0.60;
  const double T_test = 1.00; // 6σ√T = 3.6, far outside the Standard ±1.5
  const EssviSurface surf = make_flat_surface(sigma_atm, 0.01, 2.00);
  const CurveSet cs = make_flat_curves(spot, 0.01, 2.00);

  ASSERT_LT(std::fabs(surf.iv(0.0, T_test) - sigma_atm), 1.0e-7);

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::Standard;

  const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
  ASSERT_TRUE(q.has_value());

  const double truth = sigma_atm * sigma_atm; // 0.36
  // Gate: within 0.5 VARIANCE POINT of the closed form (1 var pt = 1e-4 in
  // decimal variance).
  EXPECT_NEAR(q->fair_strike_dec, truth, 0.5e-4)
      << "K_var=" << q->fair_strike_dec << " truth=" << truth
      << " bias=" << 1.0e4 * (q->fair_strike_dec - truth) << " var pts";

  // With the adaptive span the strip is complete, so neither wing is truncated.
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::StripTruncatedLeft));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::StripTruncatedRight));
}

// E2 / AN-P1-2 second half: truncation must be reported from SPAN COVERAGE, not
// from IV finiteness. A caller that pins a deliberately narrow span on the same
// high-vol tenor gets a biased K_var — that is its right — but it must be told.
TEST(VarStrip, PinnedNarrowSpanOnHighVolTenorFlagsBothWings) {
  const double sigma_atm = 0.60;
  const double T_test = 1.00;
  const EssviSurface surf = make_flat_surface(sigma_atm, 0.01, 2.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 2.00);

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::Standard;
  cfg.k_min_log = -0.60; // explicit span, far inside 6σ√T = 3.6
  cfg.k_max_log = 0.60;

  const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
  ASSERT_TRUE(q.has_value());

  // The IV is finite at both boundaries — the pre-E2 condition — so this can
  // only pass if truncation is decided by coverage.
  EXPECT_TRUE(std::isfinite(surf.iv(-0.60, T_test)));
  EXPECT_TRUE(std::isfinite(surf.iv(0.60, T_test)));
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::StripTruncatedLeft));
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::StripTruncatedRight));

  // And the pinned span really is biased low, which is what the flag warns of.
  EXPECT_LT(q->fair_strike_dec, sigma_atm * sigma_atm);
}

// ── E6 / AN-W: var swap reachable from a PricedSurface ───────────────────
//
// `derivatives.hpp` was templated on the LEGACY calibration-grade surface types
// (EssviSurface / SviSurface), so the modern fitted pipeline — which produces a
// PricedSurface — could not call `var_swap_fair_strike` without hand-converting
// slices. The module was consequently reachable only from this test file.
//
// The PricedSurface overload takes NO CurveSet: the surface's own fitted
// pillars supply forward and discount. On a flat-vol surface the var strike is
// still exactly sigma^2, which is what makes this an end-to-end check of the
// carry extraction and not just of the plumbing.
TEST(VarStrip, PricedSurfaceOverloadRecoversSigmaSquaredEndToEnd) {
  constexpr double kSigma = 0.30;
  const atx::vol::PricedSurface ps =
      atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, kSigma);
  const double T = 0.35; // a fitted pillar of the shared fixture grid

  const auto q = atx::vol::var_swap_fair_strike(ps, T);
  ASSERT_TRUE(q.has_value()) << q.error().to_string();

  const double truth = kSigma * kSigma;
  EXPECT_NEAR(q->fair_strike_dec, truth, 0.5e-4)
      << "K_var=" << q->fair_strike_dec << " truth=" << truth
      << " bias=" << 1.0e4 * (q->fair_strike_dec - truth) << " var pts";
  // 6*sigma*sqrt(T) = 1.065 < the Standard tier's 1.5 floor, so the adaptive
  // span leaves the tier span in place and neither wing is truncated.
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::StripTruncatedLeft));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::StripTruncatedRight));

  // The vol-swap and unified-price entry points are reachable the same way.
  const auto v = atx::vol::vol_swap_fair_strike(ps, T);
  ASSERT_TRUE(v.has_value()) << v.error().to_string();
  EXPECT_NEAR(v->fair_strike_dec, kSigma, 5.0e-3) << "K_vol=" << v->fair_strike_dec;

  atx::vol::DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T;
  c.notional = 1.0e6;
  c.strike_dec = q->fair_strike_dec; // struck fair => PV 0
  c.marking = DerivMarkingConvention::Otc;
  const auto priced = atx::vol::deriv_price(ps, c);
  ASSERT_TRUE(priced.has_value()) << priced.error().to_string();
  EXPECT_NEAR(priced->fair_strike_dec, q->fair_strike_dec, 1e-12);
  EXPECT_LT(std::fabs(priced->pv), 1.0e-6 * c.notional * q->fair_strike_dec);
}

// E6 fitted-range gate. Outside the fitted pillars the strip's forward clamps
// flat while `PricedSurface::forward_at` keeps extrapolating economically, so
// the strip's k = 0 would stop being the surface's ATM and K_var would be biased
// with no signal. These overloads refuse rather than serve that quietly; the
// templated CurveSet overload remains available for a caller that wants an
// extrapolated tenor and will own the choice.
TEST(VarStrip, PricedSurfaceOverloadRefusesTenorsOutsideTheFittedPillars) {
  const atx::vol::PricedSurface ps =
      atx::vol::testkit::make_flat_surface(8, 100.0, 100.0, 0.30);
  const auto ctx = ps.context();
  ASSERT_FALSE(ctx.empty());
  const double below = 0.5 * ctx.front().T;
  const double above = 2.0 * ctx.back().T;

  const auto lo = atx::vol::var_swap_fair_strike(ps, below);
  ASSERT_FALSE(lo.has_value()) << "below-range tenor must be refused, got "
                               << (lo.has_value() ? lo->fair_strike_dec : 0.0);
  EXPECT_EQ(lo.error().code(), ErrorCode::OutOfRange);

  const auto hi = atx::vol::var_swap_fair_strike(ps, above);
  ASSERT_FALSE(hi.has_value());
  EXPECT_EQ(hi.error().code(), ErrorCode::OutOfRange);

  // The in-range endpoints themselves are accepted (the gate is inclusive).
  EXPECT_TRUE(atx::vol::var_swap_fair_strike(ps, ctx.front().T).has_value());
  EXPECT_TRUE(atx::vol::var_swap_fair_strike(ps, ctx.back().T).has_value());
}

// Richardson half-grid estimate: |I_h - I_2h|/15. Finite and small on a smooth
// flat-vol integrand at every tier whose node count is 4m+1; and the estimate
// must bound the actual flat-vol truth error at Standard.
TEST(VarStrip, IntegrationErrorEstimate_FiniteAndBoundsFlatVolError) {
  const double sigma = 0.20;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::Standard;  // 257 = 4*64+1 nodes
  const auto q = var_swap_fair_strike(surf, cs, 0.10, cfg);
  ASSERT_TRUE(q.has_value());
  ASSERT_TRUE(std::isfinite(q->integration_error_est)) << "estimate not populated";
  EXPECT_GE(q->integration_error_est, 0.0);
  // Estimate is in K_var units and should be tiny on a flat surface.
  EXPECT_LT(q->integration_error_est, 1.0e-6);
}

// ── Task P-3 / PV-P4: batched strip reads (bit-identity gate) ─────────────
//
// var_swap_fair_strike gathers every distinct node's log-moneyness into a
// buffer and reads the surface via ONE `PricedSurface::iv_batch` call
// whenever `SurfaceT` (a PricedSurface-backed strip view) exposes it
// structurally; every other SurfaceT (the legacy VolSurface/EssviSurface/
// SviSurface instantiations above, and SurfaceRef-native strips) keeps the
// original per-node scalar loop untouched. `set_strip_batch_disabled_for_
// test` forces even a PricedSurface-backed strip back onto that identical
// scalar loop, so these tests run the SAME (surface, T, cfg) through both
// and assert EXACT bit equality -- not a tolerance -- across every quality
// tier on both a flat and a genuinely skewed surface, per the task's own
// acceptance gate.
namespace {

[[nodiscard]] bool bits_equal_or_both_nan(double a, double b) noexcept {
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// Always restores the batched (production default) path on scope exit, even
// across an ASSERT_* early return, so one failing iteration cannot leak the
// override into a later test.
struct StripBatchResetGuard {
  ~StripBatchResetGuard() { atx::vol::detail::set_strip_batch_disabled_for_test(false); }
};

void expect_batched_matches_scalar(const atx::vol::PricedSurface &ps, double T,
                                   const DerivConfig &cfg) {
  atx::vol::detail::set_strip_batch_disabled_for_test(true);
  const auto q_scalar = atx::vol::var_swap_fair_strike(ps, T, cfg);
  atx::vol::detail::set_strip_batch_disabled_for_test(false);
  const auto q_batched = atx::vol::var_swap_fair_strike(ps, T, cfg);
  ASSERT_TRUE(q_scalar.has_value()) << q_scalar.error().to_string();
  ASSERT_TRUE(q_batched.has_value()) << q_batched.error().to_string();

  EXPECT_TRUE(bits_equal_or_both_nan(q_scalar->fair_strike_dec, q_batched->fair_strike_dec))
      << "fair_strike_dec scalar=" << q_scalar->fair_strike_dec
      << " batched=" << q_batched->fair_strike_dec;
  EXPECT_TRUE(bits_equal_or_both_nan(q_scalar->uncapped_var_dec, q_batched->uncapped_var_dec));
  EXPECT_TRUE(
      bits_equal_or_both_nan(q_scalar->integration_error_est, q_batched->integration_error_est));
  EXPECT_TRUE(
      bits_equal_or_both_nan(q_scalar->resolved_wing_clamp, q_batched->resolved_wing_clamp));
  EXPECT_EQ(q_scalar->flags, q_batched->flags);
  EXPECT_EQ(q_scalar->strip_nodes_used, q_batched->strip_nodes_used);
  EXPECT_TRUE(bits_equal_or_both_nan(q_scalar->strip_k_lo_used, q_batched->strip_k_lo_used));
  EXPECT_TRUE(bits_equal_or_both_nan(q_scalar->strip_k_hi_used, q_batched->strip_k_hi_used));
}

}  // namespace

TEST(Strip, BatchedMatchesScalarFlatSurfaceAllTiers) {
  StripBatchResetGuard guard;
  const atx::vol::PricedSurface ps =
      atx::vol::testkit::make_flat_surface(30, 100.0, 100.0, 0.22);
  const double T = 0.35;  // a fitted pillar of the shared fixture grid
  for (const DerivQuality quality :
       {DerivQuality::Fast, DerivQuality::Standard, DerivQuality::High, DerivQuality::Audit}) {
    DerivConfig cfg = deriv_default_config();
    cfg.quality = quality;
    SCOPED_TRACE(static_cast<int>(quality));
    expect_batched_matches_scalar(ps, T, cfg);
  }
}

// A genuinely skewed/curved surface: multiple slices, non-flat smile, so the
// gathered nodes span both the put and call OTM branches and (at Audit's
// wide default span) the wing-clamp kink split -- the multi-panel code path
// the flat fixture above cannot exercise (a flat smile never wing-clamps
// within the certified band the same way).
TEST(Strip, BatchedMatchesScalarSkewSurfaceAllTiers) {
  StripBatchResetGuard guard;
  const atx::vol::PricedSurface ps = atx::vol::testkit::make_skewed_surface(31, 100.0, 100.0);
  const double T = 0.35;
  for (const DerivQuality quality :
       {DerivQuality::Fast, DerivQuality::Standard, DerivQuality::High, DerivQuality::Audit}) {
    DerivConfig cfg = deriv_default_config();
    cfg.quality = quality;
    SCOPED_TRACE(static_cast<int>(quality));
    expect_batched_matches_scalar(ps, T, cfg);
  }
}

// End-to-end through the public dispatch entries (deriv_price / deriv_greeks)
// on both fixtures at Audit quality (the richest/most panel-split-exercising
// tier): proves the batched path survives the FULL product dispatch
// (var_swap_fair_strike feeding price_var_swap's PV/flags assembly), not
// merely the strip primitive in isolation. `deriv_greeks`'s OWN center
// quote goes through the same unwrapped PricedSurfaceStripView (see
// derivatives.cpp's `deriv_greeks(const PricedSurface&, ...)` overload), so
// this also covers the batched path from inside the greek stencil's center
// evaluation. Since Review fix round 1 (I-7), `CachedBumpView::iv_batch`
// forwards the batched read too, so toggling `strip_batch_disabled_for_test`
// here also drives every BUMPED/rolled evaluation on and off the batched
// path -- this end-to-end comparison therefore also proves I-7's forwarding
// bit-identical, on top of the dedicated read-cache tests in
// deriv_greeks_test.cpp.
TEST(Strip, BatchedMatchesScalarViaDerivPriceAndGreeksAuditTier) {
  StripBatchResetGuard guard;
  const atx::vol::PricedSurface flat = atx::vol::testkit::make_flat_surface(32, 100.0, 100.0, 0.25);
  const atx::vol::PricedSurface skew = atx::vol::testkit::make_skewed_surface(33, 100.0, 100.0);
  const std::array<const atx::vol::PricedSurface *, 2> fixtures{&flat, &skew};

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::Audit;

  atx::vol::DerivContract contract{};
  contract.kind = DerivKind::VarSwap;
  contract.maturity_t = 0.35;
  contract.notional = 1.0e6;
  contract.strike_dec = 0.05;
  contract.marking = DerivMarkingConvention::Otc;

  for (const atx::vol::PricedSurface *ps : fixtures) {
    atx::vol::detail::set_strip_batch_disabled_for_test(true);
    const auto price_scalar = atx::vol::deriv_price(*ps, contract, cfg);
    const auto greeks_scalar = atx::vol::deriv_greeks(*ps, contract, cfg);

    atx::vol::detail::set_strip_batch_disabled_for_test(false);
    const auto price_batched = atx::vol::deriv_price(*ps, contract, cfg);
    const auto greeks_batched = atx::vol::deriv_greeks(*ps, contract, cfg);

    ASSERT_TRUE(price_scalar.has_value());
    ASSERT_TRUE(price_batched.has_value());
    EXPECT_TRUE(bits_equal_or_both_nan(price_scalar->pv, price_batched->pv));
    EXPECT_TRUE(
        bits_equal_or_both_nan(price_scalar->fair_strike_dec, price_batched->fair_strike_dec));

    ASSERT_TRUE(greeks_scalar.has_value());
    ASSERT_TRUE(greeks_batched.has_value());
    EXPECT_TRUE(bits_equal_or_both_nan(greeks_scalar->pv, greeks_batched->pv));
    EXPECT_TRUE(bits_equal_or_both_nan(greeks_scalar->delta, greeks_batched->delta));
    EXPECT_TRUE(bits_equal_or_both_nan(greeks_scalar->gamma, greeks_batched->gamma));
    EXPECT_TRUE(bits_equal_or_both_nan(greeks_scalar->vega, greeks_batched->vega));
    EXPECT_TRUE(bits_equal_or_both_nan(greeks_scalar->theta, greeks_batched->theta));
    EXPECT_TRUE(bits_equal_or_both_nan(greeks_scalar->vanna, greeks_batched->vanna));
    EXPECT_TRUE(bits_equal_or_both_nan(greeks_scalar->volga, greeks_batched->volga));
    EXPECT_TRUE(bits_equal_or_both_nan(greeks_scalar->charm, greeks_batched->charm));
  }
}

// ── Review fix round 1, CRITICAL-1 ────────────────────────────────────────
//
// `n` (== grid.n_nodes, the resolved distinct node count) is NOT bounded by
// `strip::kMaxStripNodes` on every path: the adaptive span rescale can raise
// it without a cap (unlike `dk_floor_nodes`), and a caller-PINNED
// `cfg.strip_nodes` is deliberately never clamped at all (see that block's
// own comment). Before this fix, either path resolving n > kMaxStripNodes
// overflowed the batched gather's fixed 2049-element stack buffers with no
// bound check and no assert. Pinning `strip_nodes` directly is the
// deterministic way to force n past the cap regardless of this fixture's own
// vol level (the adaptive-rescale route needs a high sigma*sqrt(T) Audit
// quote to reach the same n and is exercised implicitly by
// `StripResolution.DkFloorNodesCapsPastKMaxStripNodes` at the pure-function
// level already).
//
// `expect_batched_matches_scalar` is a strictly stronger check here than
// "does not crash": var_swap_fair_strike's own `n <= kMaxStripNodes` guard
// means that with n this large BOTH legs of the helper (batch-toggle on and
// off) take the identical scalar loop, so bit-identity between them is not
// merely expected but the only way this test could pass without the guard
// being missing or broken -- a resurrected overflow would corrupt the stack
// on the "batched" leg and make an accidental bit-identical match to the
// always-safe scalar leg vanishingly unlikely, not a certainty this test
// could rely on by chance.
TEST(Strip, BatchedPathFallsBackAboveMaxStripNodes) {
  StripBatchResetGuard guard;
  const atx::vol::PricedSurface ps = atx::vol::testkit::make_skewed_surface(34, 100.0, 100.0);
  const double T = 0.35;
  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::Audit;
  cfg.strip_nodes = 5001u;  // comfortably past strip::kMaxStripNodes (2049)
  expect_batched_matches_scalar(ps, T, cfg);

  const auto q = atx::vol::var_swap_fair_strike(ps, T, cfg);
  ASSERT_TRUE(q.has_value()) << q.error().to_string();
  EXPECT_EQ(q->strip_nodes_used, 5001u);
  EXPECT_GT(q->strip_nodes_used, atx::vol::strip::kMaxStripNodes);
}

// ── PV-2 / C-2: short-tenor strip resolution floor ───────────────────────
//
// The adaptive-wing rescale above only WIDENS the span for a high-vol/long-
// dated tenor; nothing rescaled the node count for the OPPOSITE regime, a
// short-tenor/low-vol quote that sits comfortably inside the tier's own span
// floor. The tier grids are sized for a roughly-1Y reference vol scale, so a
// T = 1 trading day quote resolves far coarser than its own sigma*sqrt(T)
// calls for -- the quadrature error this starves is dominated by the near-
// ATM curvature the strip integrates through, not by truncated wings (the
// coverage test above already covers that failure mode). Fast tier measured
// +6.06e-2 relative error pre-fix (task-C-2-report.md).
//
// Truth for a flat-vol lognormal surface is K_var == sigma^2 exactly.
TEST(StripResolution, OneDayTenorAccurateAtAllTiers) {
  const double spot = 100.0;
  const double sigma = 0.20;
  const double T_test = 1.0 / 252.0;  // one trading day
  // T_lo well below the 1-day tenor: T_test then sits strictly between the
  // two pillars, so the flat-surface identity theta == sigma^2*T holds by
  // exact linear interpolation rather than the surface's short-T
  // extrapolation guard (Surface<Slice>::w, T < 0.5*T0 -> NaN).
  const EssviSurface surf = make_flat_surface(sigma, T_test / 10.0, 1.00);
  const CurveSet cs = make_flat_curves(spot, T_test / 10.0, 1.00);
  ASSERT_LT(std::fabs(surf.iv(0.0, T_test) - sigma), 1.0e-9);

  const double truth = sigma * sigma;  // 0.04
  for (const DerivQuality q : {DerivQuality::Fast, DerivQuality::Standard,
                               DerivQuality::High, DerivQuality::Audit}) {
    DerivConfig cfg = deriv_default_config();
    cfg.quality = q;
    const auto quote = var_swap_fair_strike(surf, cs, T_test, cfg);
    ASSERT_TRUE(quote.has_value()) << static_cast<int>(q);
    const double rel_err = std::fabs(quote->fair_strike_dec - truth) / truth;
    EXPECT_LT(rel_err, 1.0e-3)
        << "quality=" << static_cast<int>(q) << " K_var=" << quote->fair_strike_dec
        << " truth=" << truth << " rel_err=" << rel_err;
    // The floor's node-count raise stays on the 4m+1 lattice, so the
    // Richardson half-grid estimate must still be populated (not NaN).
    EXPECT_TRUE(std::isfinite(quote->integration_error_est))
        << "quality=" << static_cast<int>(q);
  }
}

// LowT (PV-3): declared, previously never written. It now fires whenever (a)
// the resolution floor above had to raise the node count, or (b) a caller-
// pinned node count leaves the grid under-resolved and the floor could not
// raise it -- pin semantics are load-bearing for deriv_greeks' grid pinning
// (see DerivGreeks.HighVolRegimeGridPinKeepsSecondOrderSane), so a pinned
// grid is flagged rather than silently overridden.
TEST(StripResolution, LowTFlagFires) {
  const double spot = 100.0;
  const double sigma = 0.20;
  const double T_1d = 1.0 / 252.0;
  const EssviSurface surf = make_flat_surface(sigma, T_1d / 10.0, 1.00);
  const CurveSet cs = make_flat_curves(spot, T_1d / 10.0, 1.00);

  // (a) Caller pins exactly the Fast-tier default node count (97) at T = 1
  // day: the floor cannot override a pinned count, so it flags instead.
  {
    DerivConfig cfg = deriv_default_config();
    cfg.quality = DerivQuality::Fast;
    cfg.strip_nodes = 97u;
    const auto q = var_swap_fair_strike(surf, cs, T_1d, cfg);
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->strip_nodes_used, 97u) << "pin must hold exactly";
    EXPECT_TRUE(has_flag(q->flags, DerivFlags::LowT));
  }

  // (b) Unpinned default Fast grid at the same tenor: the floor DOES engage
  // (raises the node count past the tier default), which is itself flagged.
  {
    DerivConfig cfg = deriv_default_config();
    cfg.quality = DerivQuality::Fast;
    const auto q = var_swap_fair_strike(surf, cs, T_1d, cfg);
    ASSERT_TRUE(q.has_value());
    EXPECT_GT(q->strip_nodes_used, 97u) << "floor must have raised the node count";
    EXPECT_TRUE(has_flag(q->flags, DerivFlags::LowT));
  }

  // (c) A 3-month Standard-tier quote is nowhere near the floor: no LowT.
  {
    DerivConfig cfg = deriv_default_config();
    cfg.quality = DerivQuality::Standard;
    const auto q = var_swap_fair_strike(surf, cs, 0.25, cfg);
    ASSERT_TRUE(q.has_value());
    EXPECT_FALSE(has_flag(q->flags, DerivFlags::LowT));
  }
}

// C-2's resolution floor sizes the node count against dk = span/(n-1) -- the
// spacing of ONE UNIFORM lattice. C-3's kink-aligned split retires that
// lattice: integer apportionment cannot divide a span evenly, so a panel's own
// spacing runs above the nominal dk (1.6% at Standard's 256 intervals). A
// tenor whose resolved dk sits just under dk_max therefore passed the
// pre-split check while a panel actually breached the ceiling -- the floor's
// guarantee went from exact to approximate. It must be exact: the floor now
// provisions the apportionment headroom analytically (`dk_floor_nodes`'
// n_panels term).
//
// The ceiling constrains the spacing the strip ACTUALLY integrates on, so that
// is what this asserts: re-plan the split from the grid the quote reports and
// check the widest panel, not the nominal dk.
double worst_panel_dk(const atx::vol::DerivQuote& q, double band) {
  return atx::vol::strip::max_panel_spacing(atx::vol::strip::plan_strip_split(
      q.strip_k_lo_used, q.strip_k_hi_used, q.strip_nodes_used, band));
}

TEST(StripResolution, PanelSpacingRespectsCeilingAtTheFloorBoundary) {
  const double sigma = 0.20;

  // The reviewer's exact near-ceiling case, worked backwards from Standard's
  // geometry: 257 nodes over +-1.5 give a uniform dk of 3.0/256 = 0.01171875,
  // while the split's longest panel (1.0 wide, 84 intervals) spans
  // 1.0/84 = 0.011904762. Any dk_max strictly between those two clears the
  // pre-split check and is breached by a panel. dk_max = sigma*sqrt(T)/4, so
  // sigma*sqrt(T) = 0.0472 sits squarely in the gap (0.046875, 0.047619).
  {
    const double s = 0.0472;  // sigma*sqrt(T)
    const double T_test = (s / sigma) * (s / sigma);
    const EssviSurface surf = make_flat_surface(sigma, T_test / 10.0, 1.00);
    const CurveSet cs = make_flat_curves(100.0, T_test / 10.0, 1.00);
    ASSERT_LT(std::fabs(surf.iv(0.0, T_test) - sigma), 1.0e-9);

    DerivConfig cfg = deriv_default_config();
    cfg.quality = DerivQuality::Standard;
    const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
    ASSERT_TRUE(q.has_value());
    const double dk_max = atx::vol::strip::dk_ceiling(sigma, T_test);
    EXPECT_LE(worst_panel_dk(*q, 0.5), dk_max)
        << "a panel breached the ceiling the floor claimed to enforce; n="
        << q->strip_nodes_used;
  }

  // ... and the guarantee must hold for ALL inputs, not just the one crossing
  // above. Walk sigma*sqrt(T) across four decades so every tier's resolved dk
  // crosses its own dk_max somewhere in the sweep.
  //
  // Aggregate review fix (I-2 / MUST-FIX 3): the sweep's smallest `s` values
  // now push demand past `kMaxStripNodes` for every tier (most visibly
  // Audit, whose span is widest) -- the floor's exact per-panel guarantee is
  // deliberately traded for a bounded cost there, honestly flagged via
  // LowT, rather than growing the node count without bound. Below the cap
  // the exact guarantee is unchanged and still asserted; at/past it, assert
  // the cap engaged instead.
  for (const DerivQuality tier : {DerivQuality::Fast, DerivQuality::Standard,
                                  DerivQuality::High, DerivQuality::Audit}) {
    for (int j = 0; j < 160; ++j) {
      const double s = 0.002 * std::pow(10.0, 2.0 * static_cast<double>(j) / 159.0);
      const double T_test = (s / sigma) * (s / sigma);
      const EssviSurface surf = make_flat_surface(sigma, T_test / 10.0, 1.00);
      const CurveSet cs = make_flat_curves(100.0, T_test / 10.0, 1.00);
      DerivConfig cfg = deriv_default_config();
      cfg.quality = tier;
      const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
      ASSERT_TRUE(q.has_value()) << "tier=" << static_cast<int>(tier) << " s=" << s;
      const double dk_max = atx::vol::strip::dk_ceiling(sigma, T_test);
      // strip_nodes_used == kMaxStripNodes is ambiguous on its own: Audit's
      // OWN tier default already equals the cap, so a node count sitting
      // there can mean either "the cap genuinely bound" (LowT true -- a
      // truly under-resolved grid, ceiling not asserted) or "demand was
      // already comfortably satisfied by the tier default" (LowT false --
      // the pre-fix guarantee still holds). Use the real LowT flag, not the
      // node count alone, to tell the two apart.
      if (q->strip_nodes_used < atx::vol::strip::kMaxStripNodes ||
          !has_flag(q->flags, DerivFlags::LowT)) {
        EXPECT_LE(worst_panel_dk(*q, 0.5), dk_max)
            << "tier=" << static_cast<int>(tier) << " s=" << s << " T=" << T_test
            << " n=" << q->strip_nodes_used;
      } else {
        EXPECT_EQ(q->strip_nodes_used, atx::vol::strip::kMaxStripNodes)
            << "tier=" << static_cast<int>(tier) << " s=" << s;
      }
      // Provisioning the headroom must not cost the Richardson estimate.
      EXPECT_TRUE(std::isfinite(q->integration_error_est))
          << "tier=" << static_cast<int>(tier) << " s=" << s;
    }
  }

  // The headroom is provisioning, not a tax. At an ordinary tenor it is inert:
  // every tier still resolves exactly its own default budget, so no default
  // mark moves by even one ulp. (Fast is the tight one -- 96 intervals against
  // a requirement of 80 + 4*4 = 96 exactly.)
  {
    const double T_test = 0.25;
    const EssviSurface surf = make_flat_surface(sigma, T_test / 10.0, 1.00);
    const CurveSet cs = make_flat_curves(100.0, T_test / 10.0, 1.00);
    const std::pair<DerivQuality, std::uint32_t> defaults[] = {
        {DerivQuality::Fast, 97u},
        {DerivQuality::Standard, 257u},
        {DerivQuality::High, 769u},
        {DerivQuality::Audit, 2049u}};
    for (const auto& [tier, n_default] : defaults) {
      DerivConfig cfg = deriv_default_config();
      cfg.quality = tier;
      const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
      ASSERT_TRUE(q.has_value());
      EXPECT_EQ(q->strip_nodes_used, n_default) << "tier=" << static_cast<int>(tier);
      EXPECT_FALSE(has_flag(q->flags, DerivFlags::LowT))
          << "tier=" << static_cast<int>(tier);
    }
  }

  // Where the floor DOES engage, the headroom costs exactly 4*n_panels == 16
  // intervals plus the 4m+1 rounding. Pinned because C-2's CHANGELOG table
  // quotes these counts, and they must not drift silently apart from it.
  {
    const double T_1d = 1.0 / 252.0;
    const EssviSurface surf = make_flat_surface(sigma, T_1d / 10.0, 1.00);
    const CurveSet cs = make_flat_curves(100.0, T_1d / 10.0, 1.00);
    const std::pair<DerivQuality, std::uint32_t> raised[] = {
        {DerivQuality::Fast, 653u},      // C-2 alone gave 637
        {DerivQuality::Standard, 973u},  // 957
        {DerivQuality::High, 1289u},     // 1273
        {DerivQuality::Audit, 2049u}};   // floor never engaged; unchanged
    for (const auto& [tier, n_raised] : raised) {
      DerivConfig cfg = deriv_default_config();
      cfg.quality = tier;
      const auto q = var_swap_fair_strike(surf, cs, T_1d, cfg);
      ASSERT_TRUE(q.has_value());
      EXPECT_EQ(q->strip_nodes_used, n_raised) << "tier=" << static_cast<int>(tier);
    }
  }
}

// ── Aggregate review fix (C-R Important I-2 / MUST-FIX 3) ─────────────────
//
// `dk_floor_nodes`'s raw demand `span/dk_max + 4*n_panels` grows without
// bound as sigma_atm*sqrt(T) -> 0: nothing capped it, and the uncapped path
// fed `std::ceil(intervals)` into a `size_t` cast that is UB once `intervals`
// exceeds `size_t`'s range. `kMaxStripNodes` (the Audit tier's own 2049)
// caps both, checked before the cast. Unit-level pin on the pure function
// itself, isolated from any particular surface/tenor fixture.
TEST(StripResolution, DkFloorNodesCapsPastKMaxStripNodes) {
  using atx::vol::strip::dk_floor_nodes;
  using atx::vol::strip::kMaxStripNodes;

  // Pathological demand (dk_max = 1e-12, the "T ~ 1 second" corner from the
  // review): the raw `span/dk_max` term alone is ~6e12, far past size_t
  // range on the `+1` the uncapped path would have cast. Capped at exactly
  // kMaxStripNodes, still on the 4m+1 Richardson lattice.
  const std::size_t n = dk_floor_nodes(/*span=*/6.0, /*current_n=*/2049u,
                                        /*dk_max=*/1.0e-12, /*n_panels=*/4u);
  EXPECT_EQ(n, kMaxStripNodes);
  EXPECT_EQ(n % 4u, 1u) << "cap must stay on the 4m+1 Richardson lattice";

  // An ordinary demand well under the cap raises exactly as before -- the
  // cap only bites the pathological end, not everyday floor engagement.
  const std::size_t small_raise =
      dk_floor_nodes(/*span=*/2.0, /*current_n=*/97u, /*dk_max=*/0.01, /*n_panels=*/4u);
  EXPECT_GT(small_raise, 97u);
  EXPECT_LT(small_raise, kMaxStripNodes);
}

// ── C-3 / LIT-10: kink-aligned composite Simpson panels ──────────────────
//
// The strip's OTM integrand is only PIECEWISE smooth: it kinks in C1 at k = 0
// (put-call parity flips the branch and the K-derivative jumps by the discount
// factor) and at +-wing_clamp_k when the clamp binds (d(iv)/dk drops to zero
// across the band edge). Composite Simpson is O(h^4) on a smooth panel and only
// O(h^2) on one that STRADDLES a kink, and the Richardson /15 estimate -- which
// assumes the h^4 law -- then reports a number unrelated to the true error.
// Pre-C-3 the k = 0 kink sat on a panel boundary only because every DEFAULT
// grid is symmetric with 4m+1 nodes; any caller-pinned asymmetric span broke it
// silently. These three tests pin the post-fix contract.

// Shared skew fixture for the quadrature tests: ATM 20 vol with a steep but
// butterfly-legal equity put skew (d(iv)/dk(0) == -0.40 at the 3M pillar)
// whose curvature decays across the term structure. At T = 3M the default
// wing-clamp band (|k| = 0.5) reads iv ~ 0.364 on the put side against 0.20
// ATM, so the clamp really does bend the integrand there.
constexpr double kQuadAtmVol = 0.20;
constexpr double kQuadSkewSlope = -0.40;
constexpr double kQuadConvexity = 0.35;
constexpr double kQuadT = 0.25;  // the 3M fixture pillar

[[nodiscard]] EssviSurface quad_flat_surface() {
  return atx::vol::deriv_testkit::make_flat_surface(kQuadAtmVol);
}

[[nodiscard]] EssviSurface quad_skew_surface() {
  return atx::vol::deriv_testkit::make_skew_surface(kQuadAtmVol, kQuadSkewSlope,
                                                    kQuadConvexity);
}

[[nodiscard]] CurveSet quad_curves() {
  return atx::vol::deriv_testkit::make_curves(100.0, 0.02, 0.01);
}

// The split's structural contract, checked directly on the pure planner rather
// than inferred from a quote: on ANY grid the caller can ask for, every
// interior kink is a panel BOUNDARY, the panels tile the span exactly, and the
// total node budget is preserved to the node. This is what "by construction"
// has to mean -- the quadrature tests below then show it buys the accuracy.
TEST(StripQuadrature, PlanSplitKeepsKinksOnPanelBoundaries) {
  using atx::vol::strip::plan_strip_split;
  using atx::vol::strip::StripSplit;

  // Spans: symmetric, both asymmetries, clamp-inside-span, span inside the
  // clamp band (no clamp kink), one-sided (no k = 0 kink), and an endpoint
  // sitting exactly on a kink (the dedup path).
  const std::pair<double, double> spans[] = {
      {-1.5, 1.5},   {-0.714, 0.686}, {-0.31, 0.29}, {-3.0, 0.4},
      {-0.4, 0.4},   {-0.25, 2.0},    {0.1, 2.0},    {-2.0, -0.1},
      {-0.5, 1.0},   {0.0, 1.5},      {-1.0, 0.0},   {-2.5, 0.5}};
  const std::size_t counts[] = {3u,  5u,   9u,   13u,  17u,  33u,
                                97u, 101u, 257u, 769u, 2049u, 4001u};
  const double bands[] = {0.5, 0.0, 1.25};

  for (const auto& [k_lo, k_hi] : spans) {
    for (const std::size_t n : counts) {
      for (const double band : bands) {
        const StripSplit s = plan_strip_split(k_lo, k_hi, n, band);
        const std::string where = "span=[" + std::to_string(k_lo) + "," +
                                  std::to_string(k_hi) + "] n=" + std::to_string(n) +
                                  " band=" + std::to_string(band);
        ASSERT_GE(s.count, 1u) << where;
        ASSERT_LE(s.count, atx::vol::strip::kMaxStripPanels) << where;

        // Panels tile [k_lo, k_hi] exactly, left to right, none degenerate.
        EXPECT_DOUBLE_EQ(s.panels[0].k_lo, k_lo) << where;
        EXPECT_DOUBLE_EQ(s.panels[s.count - 1].k_hi, k_hi) << where;
        std::size_t total_intervals = 0;
        for (std::size_t p = 0; p < s.count; ++p) {
          EXPECT_GT(s.panels[p].k_hi, s.panels[p].k_lo) << where << " p=" << p;
          if (p > 0) {
            EXPECT_DOUBLE_EQ(s.panels[p].k_lo, s.panels[p - 1].k_hi) << where;
          }
          EXPECT_GE(s.panels[p].n_nodes, 3u) << where << " p=" << p;
          EXPECT_EQ(s.panels[p].n_nodes % 2u, 1u) << where << " p=" << p;
          total_intervals += s.panels[p].n_nodes - 1u;
        }
        // Node budget preserved: adjacent panels share their boundary node.
        EXPECT_EQ(total_intervals + 1u, n) << where;

        // Which kinks the integrand actually carries on this span. Index 1 is
        // the parity kink at k = 0, always present; the outer two are the
        // clamp edges, present only when the clamp is on. Indexing rather than
        // testing `kink == 0.0` matters: at band == 0 the outer entries are
        // -0.0/+0.0, which compare EQUAL to the parity kink.
        const double kinks[] = {-band, 0.0, band};
        const auto carried = [&](std::size_t j) {
          return (j == 1u || band > 0.0) && kinks[j] > k_lo && kinks[j] < k_hi;
        };
        std::size_t interior = 0;
        for (std::size_t j = 0; j < 3u; ++j) {
          interior += carried(j) ? 1u : 0u;
        }
        // Rung 1 of the degradation ladder, recomputed here from the budget
        // alone rather than read back off the plan: 4m intervals with a whole
        // 4-interval unit to spare per panel splits at EVERY kink.
        const bool full_split_afforded =
            ((n - 1u) % 4u) == 0u && (n - 1u) / 4u >= interior + 1u;
        for (std::size_t j = 0; j < 3u; ++j) {
          if (!carried(j)) {
            continue;
          }
          bool on_boundary = false;
          for (std::size_t p = 1; p < s.count; ++p) {
            on_boundary = on_boundary || (s.panels[p].k_lo == kinks[j]);
          }
          // A starved budget gives up the CLAMP edges first (their slope jumps
          // are orders below k = 0's) and the split itself last. k = 0 outlives
          // both: 4 intervals always buy two panels of 2, because `unit`
          // degrades from 4 to 2 before the split is abandoned.
          if (j == 1u ? n >= 5u : full_split_afforded) {
            EXPECT_TRUE(on_boundary)
                << where << " kink=" << kinks[j] << " count=" << s.count;
          }
        }

        // The estimate is claimed exactly when every panel can halve onto its
        // own kink-aligned sub-grid.
        bool all_4m1 = true;
        for (std::size_t p = 0; p < s.count; ++p) {
          all_4m1 = all_4m1 && ((s.panels[p].n_nodes % 4u) == 1u);
        }
        EXPECT_EQ(s.richardson_ok, all_4m1) << where;
      }
    }
  }
}

// Every default tier budget must reach the top rung of the degradation ladder:
// all kinks split AND the Richardson estimate populated. This is the invariant
// the tier defaults quietly relied on before C-3 and now assert.
TEST(StripQuadrature, PlanSplitPopulatesRichardsonOnDefaultBudgets) {
  using atx::vol::strip::plan_strip_split;
  // The four tier grids (97/257/769/2049 nodes over +-1/1.5/2/3).
  const std::pair<double, std::size_t> tiers[] = {
      {1.0, 97u}, {1.5, 257u}, {2.0, 769u}, {3.0, 2049u}};
  for (const auto& [half, n] : tiers) {
    const auto s = plan_strip_split(-half, half, n, 0.5);
    EXPECT_EQ(s.count, 4u) << "n=" << n;  // [-h,-0.5] [-0.5,0] [0,0.5] [0.5,h]
    EXPECT_TRUE(s.richardson_ok) << "n=" << n;
  }
}

// The pinned asymmetric span the C-3 brief calls for, widened from the brief's
// +-0.3 to +-0.7 so SPAN TRUNCATION (which at 3 sigma sqrt(T) costs ~6e-4 rel,
// two orders above the quadrature effect under test) cannot mask the quadrature
// signal: at 7 sigma sqrt(T) the truncated tail is ~1e-15 rel. The 0.014 offset
// is exactly one node spacing, so k = 0 lands on node 51 -- an ODD index, i.e.
// the MIDPOINT of a Simpson panel, which is the worst case for a straddled
// C1 kink (the panel error is J*h^2/6 there, and 0 at a panel boundary).
constexpr double kAsymKLo = -0.714;
constexpr double kAsymKHi = 0.686;
constexpr double kSymKLo = -0.700;
constexpr double kSymKHi = 0.700;
constexpr std::uint32_t kPinNodes = 101u;

// MEASURED on this exact fixture (truth == 0.04):
//                     PRE-C-3 (single panel)          POST-C-3 (split)
//   symmetric pin     0.040000001707333969  +1.7e-9   0.040000008157407653  +8.2e-9
//   asymmetric pin    0.040261330772305835  +2.6e-4   0.040000008157546028  +8.2e-9
// The pre-fix asymmetric miss is the O(h^2) straddle term J*h^2/6*(2/T) with
// J = 1 (the parity slope jump) and h = 0.014, which predicts 2.6133e-4 --
// the measured 2.61331e-4 confirms the mechanism to five digits. Post-fix the
// two pins agree to 1.4e-13 and both sit on the grid's O(h^4) floor.
TEST(StripQuadrature, AsymmetricPinMatchesSymmetricReference) {
  const EssviSurface flat = quad_flat_surface();
  const CurveSet cs = quad_curves();
  const double truth = kQuadAtmVol * kQuadAtmVol;  // flat lognormal: K_var == sigma^2

  DerivConfig sym = deriv_default_config();
  sym.k_min_log = kSymKLo;
  sym.k_max_log = kSymKHi;
  sym.strip_nodes = kPinNodes;
  DerivConfig asym = sym;
  asym.k_min_log = kAsymKLo;
  asym.k_max_log = kAsymKHi;

  const auto q_sym = var_swap_fair_strike(flat, cs, kQuadT, sym);
  const auto q_asym = var_swap_fair_strike(flat, cs, kQuadT, asym);
  ASSERT_TRUE(q_sym.has_value());
  ASSERT_TRUE(q_asym.has_value());

  // Both spans are ~7 sigma sqrt(T) wide, so the only error either quote can
  // carry is quadrature. A 101-node grid's O(h^4) floor is ~8e-9 in K_var
  // units; 1e-6 sits two orders above that floor and 260x BELOW the pre-fix
  // asymmetric miss, so this bound separates the two regimes cleanly.
  const double tol = 1.0e-6;
  EXPECT_LT(std::fabs(q_sym->fair_strike_dec - truth), tol)
      << "symmetric reference K=" << q_sym->fair_strike_dec;
  EXPECT_LT(std::fabs(q_asym->fair_strike_dec - truth), tol)
      << "asymmetric pin K=" << q_asym->fair_strike_dec
      << " (pre-fix this missed by 2.61e-4 -- k = 0 straddled a panel)";
  // Agreement is a much tighter claim than either bound above: both grids
  // carry the same total budget over near-identical panels, so post-fix they
  // land 1.4e-13 apart (measured) against 2.6e-4 pre-fix.
  EXPECT_LT(std::fabs(q_asym->fair_strike_dec - q_sym->fair_strike_dec), 1.0e-9)
      << "asymmetric pin must agree with the symmetric reference";

  // The pins are honored verbatim (total span + total node count), which is
  // what greeks pinning and archived quotes replay against.
  EXPECT_EQ(q_asym->strip_nodes_used, kPinNodes);
  EXPECT_DOUBLE_EQ(q_asym->strip_k_lo_used, kAsymKLo);
  EXPECT_DOUBLE_EQ(q_asym->strip_k_hi_used, kAsymKHi);

  // The default (symmetric, tier) grid is unaffected and still hits truth.
  const auto q_default = var_swap_fair_strike(flat, cs, kQuadT, deriv_default_config());
  ASSERT_TRUE(q_default.has_value());
  EXPECT_LT(std::fabs(q_default->fair_strike_dec - truth), tol);
}

// Node count for the same-span truth proxy a pinned case is measured against:
// 40x the pinned density, so its own O(h^4) error is ~2.6e6x smaller and the
// distance from it IS the pinned grid's quadrature error, with the shared
// span's truncation cancelling exactly.
constexpr std::uint32_t kFineNodes = 4001u;

// Assert `est` is an ESTIMATE of `err` -- same order of magnitude, both
// directions. A bound that only caught overstatement would pass on an estimate
// pinned at zero; one that only caught understatement would pass on noise.
void expect_estimates(double est, double err, const char* what) {
  ASSERT_TRUE(std::isfinite(est)) << what << ": estimate not populated";
  EXPECT_GT(est, 0.1 * err) << what << ": estimate understates -- est=" << est
                            << " true=" << err;
  EXPECT_LT(est, 10.0 * err) << what << ": estimate overstates -- est=" << est
                             << " true=" << err;
}

// The Richardson |I_h - I_2h|/15 estimate must behave like an ESTIMATE of the
// true quadrature error, not like noise.
//
// MEASURED est / true-error ratio on this fixture (1.0 is a perfect estimate):
//                            PRE-C-3   POST-C-3
//   Standard default grid    0.689     1.000   -- the defaults' symmetry had
//                                                 already rescued this one
//   symmetric  +-0.7 pin     574       1.158   -- k = 0 is a full-grid
//                                                 boundary but an ODD HALF-grid
//                                                 index, so the /15 difference
//                                                 measured the half grid's own
//                                                 O(h^2) straddle, not the error
//   asymmetric +-0.7 pin     0.133     1.161   -- k = 0 mid-panel on both grids
// The symmetric-pin case is the sharp one: the estimate was 574x the error it
// claimed to estimate (4.1e4x on the flat fixture).
TEST(StripQuadrature, RichardsonBoundsTrueErrorOnSkew) {
  const EssviSurface skew = quad_skew_surface();
  const CurveSet cs = quad_curves();

  // (a) the default Standard grid, against the Audit tier as truth proxy
  // (2049 nodes over +-3.0: h is 4x finer, so ~256x more accurate under the
  // h^4 law, and its wider span strictly contains Standard's).
  DerivConfig audit = deriv_default_config();
  audit.quality = DerivQuality::Audit;
  const auto q_audit = var_swap_fair_strike(skew, cs, kQuadT, audit);
  ASSERT_TRUE(q_audit.has_value());
  DerivConfig std_cfg = deriv_default_config();
  std_cfg.quality = DerivQuality::Standard;
  const auto q_std = var_swap_fair_strike(skew, cs, kQuadT, std_cfg);
  ASSERT_TRUE(q_std.has_value());
  expect_estimates(q_std->integration_error_est,
                   std::fabs(q_std->fair_strike_dec - q_audit->fair_strike_dec),
                   "Standard default grid");

  // (b), (c) the same claim on the two pinned spans, each against its OWN
  // same-span fine reference (a pinned span is narrower than Audit's, so its
  // distance from Audit carries a truncation term the estimate does not, and
  // must not, model).
  const std::pair<double, double> pins[] = {{kSymKLo, kSymKHi}, {kAsymKLo, kAsymKHi}};
  for (const auto& [k_lo, k_hi] : pins) {
    DerivConfig pinned = deriv_default_config();
    pinned.k_min_log = k_lo;
    pinned.k_max_log = k_hi;
    pinned.strip_nodes = kPinNodes;
    DerivConfig fine = pinned;
    fine.strip_nodes = kFineNodes;
    const auto q_pin = var_swap_fair_strike(skew, cs, kQuadT, pinned);
    const auto q_fine = var_swap_fair_strike(skew, cs, kQuadT, fine);
    ASSERT_TRUE(q_pin.has_value());
    ASSERT_TRUE(q_fine.has_value());
    expect_estimates(q_pin->integration_error_est,
                     std::fabs(q_pin->fair_strike_dec - q_fine->fair_strike_dec),
                     k_lo == kSymKLo ? "symmetric pin" : "asymmetric pin");
  }
}

// Splitting at the clamp edges must not MOVE a default-grid mark: the split
// only relocates nodes inside a span it does not change. The reference values
// are the pre-C-3 single-panel quotes, measured on this exact fixture and
// recorded here so the bound is checked against the number the release actually
// shipped, not against a value this same code recomputes.
//
// MEASURED relative moves (post-C-3 vs the recorded pre-C-3 mark):
//   Fast 3.1e-16, Standard 2.78e-7, High 0 (exact), Audit 4.2e-9.
// Fast and High do not move at all because their proportional apportionment
// happens to reproduce the un-split uniform spacing exactly (96 intervals over
// four equal panels; 768 over 1.5/0.5/0.5/1.5). Standard's 2.78e-7 move is
// toward truth, not away: |K - Audit| goes 1.14e-8 -> 1.34e-9, 8.5x better.
struct ClampEdgeCase {
  DerivQuality tier;
  double k_var_pre_c3;
};

TEST(StripQuadrature, ClampEdgeSplit) {
  const EssviSurface skew = quad_skew_surface();
  const CurveSet cs = quad_curves();

  // PRE-FIX MEASURED (single-panel composite Simpson), 17 significant digits.
  const ClampEdgeCase cases[] = {
      {DerivQuality::Fast, 0.045847674795002777},
      {DerivQuality::Standard, 0.045847649986699476},
      {DerivQuality::High, 0.045847661437682291},
      {DerivQuality::Audit, 0.045847661198757744},
  };

  for (const ClampEdgeCase& c : cases) {
    DerivConfig cfg = deriv_default_config();
    cfg.quality = c.tier;
    const auto q = var_swap_fair_strike(skew, cs, kQuadT, cfg);
    ASSERT_TRUE(q.has_value()) << static_cast<int>(c.tier);
    // Every tier's span reaches past the +-0.5 trust band, so the clamp binds
    // and the split really does add the two band-edge boundaries.
    EXPECT_TRUE(has_flag(q->flags, DerivFlags::WingClamped))
        << "tier=" << static_cast<int>(c.tier);
    const double rel = std::fabs(q->fair_strike_dec - c.k_var_pre_c3) / c.k_var_pre_c3;
    EXPECT_LT(rel, 1.0e-6) << "tier=" << static_cast<int>(c.tier)
                           << " K=" << q->fair_strike_dec
                           << " pre-C-3=" << c.k_var_pre_c3;
    // Default budgets keep every panel on the 4m+1 lattice, so the split must
    // never cost the error estimate.
    EXPECT_TRUE(std::isfinite(q->integration_error_est))
        << "tier=" << static_cast<int>(c.tier);
    EXPECT_GE(q->integration_error_est, 0.0);
  }
}

// ── Aged dispatch (test_vol_deriv_aged.c) ────────────────────────────────

// Mid-life variance swap with rv_done == K_var_future: the linear blend must
// collapse to that single number and price the fair-struck contract to PV = 0.
TEST(AgedDispatch, VarSwap_MidLifeRvEqualsFuture_BlendsToFutureStrike) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();

  const auto strip_q = var_swap_fair_strike(surf, cs, 0.10, cfg);
  ASSERT_TRUE(strip_q.has_value());
  const double k_var_future = strip_q->fair_strike_dec;
  EXPECT_GT(k_var_future, 0.0);

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.10;
  c.notional = 1.0e6;
  c.strike_dec = k_var_future;  // fair -> expect PV = 0
  c.marking = DerivMarkingConvention::Otc;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 24u;
  c.rv_spec.n_obs_done = 12u;
  c.rv_spec.rv_done_dec = k_var_future;

  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());
  EXPECT_LT(std::fabs(q->fair_strike_dec - k_var_future), 1.0e-12);
  EXPECT_LT(std::fabs(q->pv), 1.0e-9 * c.notional * k_var_future);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::Aged));
  EXPECT_FALSE(has_flag(q->flags, DerivFlags::FullyAged));
  // Each leg contributes K_var_future * 0.5.
  EXPECT_LT(std::fabs(q->accrued_component_dec - 0.5 * k_var_future), 1.0e-12);
  EXPECT_LT(std::fabs(q->future_component_dec - 0.5 * k_var_future), 1.0e-12);
}

// Mid-life with rv_done > K_var_future: the blend is strictly between the leg
// values and PV vs the fair-future strike is positive.
TEST(AgedDispatch, VarSwap_MidLifeHighRealized_PvPositive) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivConfig cfg = deriv_default_config();

  const auto strip_q = var_swap_fair_strike(surf, cs, 0.10, cfg);
  ASSERT_TRUE(strip_q.has_value());
  const double k = strip_q->fair_strike_dec;

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.10;
  c.notional = 1.0e6;
  c.strike_dec = k;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 24u;
  c.rv_spec.n_obs_done = 18u;
  c.rv_spec.rv_done_dec = k * 4.0;  // heavy realized

  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value());

  // Closed-form blend: (18/24)*4K + (6/24)*K = 3.25 K.
  const double expected = (18.0 / 24.0) * 4.0 * k + (6.0 / 24.0) * k;
  EXPECT_LT(std::fabs(q->fair_strike_dec - expected), 1.0e-12);
  EXPECT_GT(q->pv, 0.0);
}

// ── Discrete-monitoring correction (Task C-1: PV-1/PV-8/LIT-3) ───────────
//
// Broadie-Jain (2008) diffusion-drift correction for the future
// implied-variance leg: each fixing has E[r_i^2] = K_var*dt + mu^2*dt^2 (mu =
// r_bar - q_bar - K_var/2), so summing n_remaining fixings and annualizing
// gives the discrete-monitoring fair strike K_var + (T_resid/n_remaining)*
// mu^2 -- ADDITIVE, using the FUTURE leg's own remaining fixing count. Uses
// Task 0's `deriv_testkit::make_curves` (nontrivial r - q carry) because the
// correction needs a real rate/carry differential to be nonzero at all; this
// file's own `make_flat_curves` above is zero-rate by design.

TEST(DiscreteCorrection, MatchesBSExactDiscreteFairStrike) {
  using atx::vol::deriv_testkit::make_curves;
  using atx::vol::deriv_testkit::make_flat_surface;
  using atx::vol::deriv_testkit::mc_realized_variance;
  using atx::vol::deriv_testkit::McModelParams;

  const double sigma = 0.20;
  const double r = 0.06;
  const double q = 0.01;
  const double T = 1.0;  // 1Y fixture pillar -- ATM iv == sigma exactly there
  const std::uint32_t n = 252u;

  const EssviSurface surf = make_flat_surface(sigma);
  const CurveSet cs = make_curves(100.0, r, q);

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T;
  c.notional = 1.0;
  c.strike_dec = 0.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = n;
  c.rv_spec.n_obs_done = 0u;
  c.rv_spec.rv_done_dec = 0.0;

  DerivConfig cfg_off = deriv_default_config();
  cfg_off.quality = DerivQuality::High;  // tighter strip quadrature error
  cfg_off.discrete_correction_mode = DerivDiscreteCorrection::None;
  const auto q_off = deriv_price(surf, cs, c, cfg_off);
  ASSERT_TRUE(q_off.has_value());

  DerivConfig cfg_on = cfg_off;
  cfg_on.discrete_correction_mode = DerivDiscreteCorrection::Diffusion1OverN;
  const auto q_on = deriv_price(surf, cs, c, cfg_on);
  ASSERT_TRUE(q_on.has_value());
  EXPECT_TRUE(has_flag(q_on->flags, DerivFlags::DiscreteCorrApplied));

  // Oracle #1 (analytic, exact): sigma^2 + (T/n)*mu^2, mu = r - q - sigma^2/2
  // = 0.03 here -- the brief's own worked example: correction = (1/252)*9e-4
  // = 3.571e-6 = 0.0357 var pts at T=1, n=252. High-quality strip quadrature
  // error is < 1e-5 on a flat surface (see VarStrip.FlatVol_HighQuality...);
  // the OLD multiplicative code overstates the correction by ~44x (~1.6e-4),
  // well clear of that error floor.
  const double mu = r - q - 0.5 * sigma * sigma;
  const double truth = sigma * sigma + (T / static_cast<double>(n)) * mu * mu;
  EXPECT_NEAR(q_on->fair_strike_dec, truth, 5.0e-5);

  // Oracle #2 (independent): Task-0 seeded-MC harness, 3-SE band.
  const McModelParams p{100.0, r, q, sigma, T};
  const auto mc = mc_realized_variance(p, 200000, n, 7);
  ASSERT_GT(mc.stderr_rv, 0.0);
  EXPECT_NEAR(q_on->fair_strike_dec, mc.mean_rv, 3.0 * mc.stderr_rv)
      << "fair_strike=" << q_on->fair_strike_dec << " mc_mean=" << mc.mean_rv
      << " mc_stderr=" << mc.stderr_rv;
}

// Aged contract: T_resid = 0.5 (6M pillar) of an original 1Y/252-fixing
// contract half elapsed (n_done = 126). The correction divisor must be
// n_remaining = n_total - n_done = 126, NOT n_total = 252.
TEST(DiscreteCorrection, MidLifeUsesRemainingFixings) {
  using atx::vol::deriv_testkit::make_curves;
  using atx::vol::deriv_testkit::make_flat_surface;

  const double sigma = 0.20;
  const double r = 0.06;
  const double q = 0.01;
  const double T_resid = 0.5;  // 6M fixture pillar
  const std::uint32_t n_total = 252u;
  const std::uint32_t n_done = 126u;  // n_remaining == 126

  const EssviSurface surf = make_flat_surface(sigma);
  const CurveSet cs = make_curves(100.0, r, q);

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T_resid;
  c.notional = 1.0;
  c.strike_dec = 0.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = n_total;
  c.rv_spec.n_obs_done = n_done;
  c.rv_spec.rv_done_dec = 0.0;

  DerivConfig cfg_off = deriv_default_config();
  cfg_off.quality = DerivQuality::High;
  cfg_off.discrete_correction_mode = DerivDiscreteCorrection::None;
  const auto q_off = deriv_price(surf, cs, c, cfg_off);
  ASSERT_TRUE(q_off.has_value());

  DerivConfig cfg_on = cfg_off;
  cfg_on.discrete_correction_mode = DerivDiscreteCorrection::Diffusion1OverN;
  const auto q_on = deriv_price(surf, cs, c, cfg_on);
  ASSERT_TRUE(q_on.has_value());

  // future_component_dec = w_future * K_var_future (corrected when the mode
  // is on); w_future is identical for both calls (same aging), so dividing it
  // back out isolates K_var_future itself and cancels the strip's own
  // quadrature bias (both calls share the exact same strip evaluation).
  const double w_future =
      static_cast<double>(n_total - n_done) / static_cast<double>(n_total);
  const double k_var_future_uncorrected = q_off->future_component_dec / w_future;
  const double k_var_future_corrected = q_on->future_component_dec / w_future;
  const double actual_addend = k_var_future_corrected - k_var_future_uncorrected;

  // ln(F/S)/T_resid == r - q exactly by construction of `make_curves` (F =
  // S*exp((r-q)*T) at every fixture pillar, and T_resid = 0.5 IS a pillar).
  const double r_minus_q = r - q;
  const double mu = r_minus_q - 0.5 * k_var_future_uncorrected;
  const double addend_using_n_remaining = (T_resid / 126.0) * mu * mu;
  const double addend_using_n_total = (T_resid / 252.0) * mu * mu;  // wrong divisor

  EXPECT_NEAR(actual_addend, addend_using_n_remaining, 1.0e-11);
  EXPECT_GT(std::fabs(actual_addend - addend_using_n_total), 1.0e-6);
}

// With the mode on, xi must be resolved against the UNCORRECTED strip mean
// (PV-8) -- resolve_vol_of_vol's "reproduces Carr-Lee exactly" contract has
// to survive the discrete-monitoring correction mode, so vol_of_vol_used must
// be identical whether the correction is on or off.
TEST(DiscreteCorrection, XiCalibratedPreCorrection) {
  using atx::vol::deriv_testkit::make_curves;
  using atx::vol::deriv_testkit::make_flat_surface;

  const EssviSurface surf = make_flat_surface(0.20);
  const CurveSet cs = make_curves(100.0, 0.06, 0.01);

  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = 0.5;  // 6M pillar
  c.notional = 1.0;
  c.strike_dec = 0.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 252u;
  c.rv_spec.n_obs_done = 126u;  // mid-life -> always the distribution model
  c.rv_spec.rv_done_dec = 0.03;  // arbitrary accrued leg; does not affect xi

  DerivConfig cfg_off = deriv_default_config();
  cfg_off.discrete_correction_mode = DerivDiscreteCorrection::None;
  const auto q_off = deriv_price(surf, cs, c, cfg_off);
  ASSERT_TRUE(q_off.has_value());
  EXPECT_TRUE(has_flag(q_off->flags, DerivFlags::VolOfVolCalibrated));

  DerivConfig cfg_on = deriv_default_config();
  cfg_on.discrete_correction_mode = DerivDiscreteCorrection::Diffusion1OverN;
  const auto q_on = deriv_price(surf, cs, c, cfg_on);
  ASSERT_TRUE(q_on.has_value());
  EXPECT_TRUE(has_flag(q_on->flags, DerivFlags::VolOfVolCalibrated));
  EXPECT_TRUE(has_flag(q_on->flags, DerivFlags::DiscreteCorrApplied));

  EXPECT_NEAR(q_on->vol_of_vol_used, q_off->vol_of_vol_used, 1.0e-14);

  // Sanity: the correction actually moved the priced quantity, so this is not
  // vacuously passing a no-op correction.
  EXPECT_NE(q_on->fair_strike_dec, q_off->fair_strike_dec);
}

// ── Marquee PnL identity (test_vol_deriv_marquee_pnl.c) ──────────────────

// "A vol swap struck at the t0 fair vol k1 with vega v1, realizing r1 to
// expiry, pays (r1 - k1) * v1." Verifies internal consistency of the dispatch:
// t0 strike <-> expiry payoff.
TEST(Marquee, VolSwap_PnlIdentity_HoldsAtExpiry) {
  const double spot = 100.0;
  const double sigma_atm = 0.20;
  const EssviSurface surf = make_flat_surface(sigma_atm, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(spot, 0.01, 1.00);

  const std::uint32_t n_obs_total = 25u;
  const double T_total = static_cast<double>(n_obs_total) / 252.0;
  const double v1 = 100000.0;  // vol notional ("vega")

  const DerivConfig cfg = deriv_default_config();

  DerivContract contract_t0{};
  contract_t0.kind = DerivKind::VolSwap;
  contract_t0.maturity_t = T_total;
  contract_t0.notional = v1;
  contract_t0.marking = DerivMarkingConvention::Otc;
  contract_t0.rv_spec.annualization = 252.0;
  contract_t0.rv_spec.n_obs_total = n_obs_total;
  contract_t0.rv_spec.n_obs_done = 0u;
  contract_t0.strike_dec = 0.0;  // first pass: extract K_vol from the quote

  const auto q_seed = deriv_price(surf, cs, contract_t0, cfg);
  ASSERT_TRUE(q_seed.has_value());
  const double k_vol = q_seed->fair_strike_dec;
  EXPECT_GT(k_vol, 0.05);
  EXPECT_LT(k_vol, 0.50);
  // Carr-Lee on a flat-vol surface should land near sigma_atm.
  EXPECT_LT(std::fabs(k_vol - sigma_atm), 5.0e-3);

  // Re-price with strike = K_vol -> PV ~ 0 by construction.
  contract_t0.strike_dec = k_vol;
  const auto q_t0 = deriv_price(surf, cs, contract_t0, cfg);
  ASSERT_TRUE(q_t0.has_value());
  EXPECT_LT(std::fabs(q_t0->pv), 1.0e-9 * v1 * k_vol);

  // Drive a deterministic (transcendental) spot path through the tracker.
  auto built = RealizedTracker::create(252.0, n_obs_total);
  ASSERT_TRUE(built.has_value());
  RealizedTracker tracker = std::move(*built);
  ASSERT_TRUE(tracker.observe(spot).has_value());
  for (std::uint32_t i = 0; i < n_obs_total; ++i) {
    const double r = 0.015 * std::sin(0.71 * static_cast<double>(i + 1)) +
                     0.008 * std::cos(1.13 * static_cast<double>(i + 2));
    const double s_prev = (i == 0u) ? spot : tracker.prev_spot();
    const double s_next = s_prev * std::exp(r);
    ASSERT_TRUE(tracker.observe(s_next).has_value());
  }
  const RealizedVarianceSpec rv_at_expiry = tracker.snapshot();
  EXPECT_EQ(rv_at_expiry.n_obs_done, n_obs_total);
  const double r1 = std::sqrt(rv_at_expiry.rv_done_dec);
  EXPECT_GT(r1, 0.0);

  // Price at expiry (fully aged).
  DerivContract contract_exp = contract_t0;
  contract_exp.maturity_t = 0.0;
  contract_exp.rv_spec = rv_at_expiry;
  contract_exp.strike_dec = k_vol;  // strike fixed at trade date

  const auto q_exp = deriv_price(surf, cs, contract_exp, cfg);
  ASSERT_TRUE(q_exp.has_value());

  // The marquee identity, to sub-ULP tolerance.
  const double pv_expected = v1 * (r1 - k_vol);
  const double tol = 1.0e-9 * std::fabs(v1 * std::fmax(k_vol, r1));
  EXPECT_LT(std::fabs(q_exp->pv - pv_expected), tol);

  EXPECT_TRUE(has_flag(q_exp->flags, DerivFlags::FullyAged));
  EXPECT_TRUE(has_flag(q_exp->flags, DerivFlags::Aged));
  EXPECT_LT(std::fabs(q_exp->fair_strike_dec - r1), 1.0e-12);
}

// ── Carr-Lee convexity refinement (Task C-5 / LIT-4) ──────────────────────
//
// LIT-4 (P1-class model bias): the naive ATMF-straddle K_vol ~= sqrt(2 pi /
// T) * C_ATMF / (F * df) is the approximation Carr-Lee's rrvd.pdf explicitly
// declines to endorse (Remark 6.5) -- it reads only the ATMF point and never
// sees the rest of the smile, so under equity skew it is biased LOW relative
// to the true VOL0 (>40 vol bp at 6M in the paper's own Heston BCC example,
// Sec. 6.5). `DerivConfig::carr_lee_form == Refined` recovers part of that
// gap via the Remark 6.4/6.5 refinement against the strip's own K_var --
// task-C-5-report.md has the from-paper (re-)derivation, including the
// annualization fix the paper's un-annualized statement needs.

TEST(CarrLee, RefinementVanishesOnFlat) {
  // Idealized "flat vol" case: K_var == K_vol_naive^2 exactly, so there is no
  // convexity gap left to recover. The correction numerator
  // T*(K_var - K_vol_naive^2) is then identically zero, so refined must
  // equal naive to the bit -- for any tenor and any vol level, not just the
  // one this task's other tests happen to exercise.
  const double tenors[] = {1.0 / 12.0, 0.25, 0.5, 1.0, 3.0};
  const double naive_vols[] = {0.05, 0.20, 0.45, 0.90};
  for (const double T : tenors) {
    for (const double k_vol_naive : naive_vols) {
      const double k_var = k_vol_naive * k_vol_naive;
      const double refined =
          atx::vol::detail::refine_carr_lee_k_vol(k_vol_naive, k_var, T);
      EXPECT_NEAR(refined, k_vol_naive, 1e-12)
          << "T=" << T << " k_vol_naive=" << k_vol_naive;
    }
  }
}

// Skewed fixture (rho ~= -0.7, deriv_testkit::kSkewRho -- LIT-4's cited
// correlation), evaluated at LIT-4's cited 6M tenor: the refinement must
// recover PART of the naive-vs-sqrt(K_var) convexity gap -- naive < refined
// < sqrt(K_var) -- never overshooting the Jensen bound VOL0 <= VAR0 (rrvd
// Prop. 6.1(c)) in this regime. Reuses the StripQuadrature section's shared
// skew fixture above (quad_skew_surface / quad_curves) -- same surface
// family the C-3 quadrature tests already validated against Prop 6.1(c)'s
// sibling bound.
TEST(CarrLee, RefinementOrderedUnderSkew) {
  const EssviSurface surf = quad_skew_surface();
  const CurveSet cs = quad_curves();
  const double T = 0.5;  // 6M pillar, LIT-4's cited tenor

  DerivConfig naive_cfg = deriv_default_config();
  naive_cfg.carr_lee_form = atx::vol::CarrLeeForm::Naive;
  const auto naive_q = vol_swap_fair_strike(surf, cs, T, naive_cfg);
  ASSERT_TRUE(naive_q.has_value());

  DerivConfig refined_cfg = deriv_default_config();
  refined_cfg.carr_lee_form = atx::vol::CarrLeeForm::Refined;
  const auto refined_q = vol_swap_fair_strike(surf, cs, T, refined_cfg);
  ASSERT_TRUE(refined_q.has_value());

  const auto var_q = var_swap_fair_strike(surf, cs, T, deriv_default_config());
  ASSERT_TRUE(var_q.has_value());
  const double sqrt_k_var = std::sqrt(var_q->fair_strike_dec);

  EXPECT_LT(naive_q->fair_strike_dec, refined_q->fair_strike_dec);
  EXPECT_LT(refined_q->fair_strike_dec, sqrt_k_var);

  // Refined mode ran the strip internally -- unlike Naive, its own
  // diagnostics now reflect that (carry-forward of the fix documented on
  // vol_swap_fair_strike's declaration): a real Richardson estimate, not
  // NaN, and a nonzero uncapped_var_dec, not the struct default.
  EXPECT_TRUE(std::isfinite(refined_q->integration_error_est));
  EXPECT_GT(refined_q->uncapped_var_dec, 0.0);

  // Naive's own contract is untouched by this task (v1.1 default path).
  EXPECT_TRUE(std::isnan(naive_q->integration_error_est));
  EXPECT_EQ(naive_q->uncapped_var_dec, 0.0);
}

// ── Realized tracker (test_realized_tracker.c) ───────────────────────────

TEST(RealizedTracker, Create_RejectsBadInputs) {
  EXPECT_FALSE(RealizedTracker::create(0.0, 22u).has_value());
  EXPECT_FALSE(RealizedTracker::create(-1.0, 22u).has_value());
  EXPECT_FALSE(RealizedTracker::create(252.0, 0u).has_value());
  EXPECT_EQ(RealizedTracker::create(0.0, 22u).error().code(),
            ErrorCode::InvalidArgument);
}

TEST(RealizedTracker, Create_ClearsState) {
  const auto built = RealizedTracker::create(252.0, 22u);
  ASSERT_TRUE(built.has_value());
  const RealizedTracker& t = *built;
  EXPECT_FALSE(t.have_prev());
  const RealizedVarianceSpec spec = t.snapshot();
  EXPECT_EQ(spec.n_obs_done, 0u);
  EXPECT_EQ(spec.n_obs_total, 22u);
  EXPECT_LT(std::fabs(spec.sum_sq_log_returns_done), 1.0e-15);
  EXPECT_LT(std::fabs(spec.rv_done_dec), 1.0e-15);
  EXPECT_LT(std::fabs(spec.annualization - 252.0), 1.0e-15);
}

TEST(RealizedTracker, ObserveBatch_HandComputedThreeReturns) {
  // Spots [100, 101, 99, 102] -> three returns; check sum of squares and the
  // annualized decimal variance against the hand computation.
  const double spots[] = {100.0, 101.0, 99.0, 102.0};
  auto built = RealizedTracker::create(252.0, 100u);
  ASSERT_TRUE(built.has_value());
  RealizedTracker t = std::move(*built);
  ASSERT_TRUE(t.observe_batch(spots).has_value());

  const double r1 = std::log(101.0 / 100.0);
  const double r2 = std::log(99.0 / 101.0);
  const double r3 = std::log(102.0 / 99.0);
  const double sum_sq_expected = r1 * r1 + r2 * r2 + r3 * r3;
  const double rv_dec_expected = 252.0 * sum_sq_expected / 3.0;

  const RealizedVarianceSpec spec = t.snapshot();
  EXPECT_EQ(spec.n_obs_done, 3u);
  EXPECT_LT(std::fabs(spec.sum_sq_log_returns_done - sum_sq_expected), 1.0e-15);
  EXPECT_LT(std::fabs(spec.rv_done_dec - rv_dec_expected), 1.0e-13);
}

TEST(RealizedTracker, Observe_RefusesObservationsPastTotal) {
  auto built = RealizedTracker::create(252.0, 2u);
  ASSERT_TRUE(built.has_value());
  RealizedTracker t = std::move(*built);
  // Three spots -> two returns fill the contract.
  EXPECT_TRUE(t.observe(100.0).has_value());
  EXPECT_TRUE(t.observe(101.0).has_value());
  EXPECT_TRUE(t.observe(102.0).has_value());
  // Fourth spot would write a third return -> refuse.
  EXPECT_FALSE(t.observe(103.0).has_value());
  EXPECT_EQ(t.snapshot().n_obs_done, 2u);
}

TEST(RealizedTracker, Observe_RejectsNonPositiveSpot) {
  auto built = RealizedTracker::create(252.0, 10u);
  ASSERT_TRUE(built.has_value());
  RealizedTracker t = std::move(*built);
  EXPECT_FALSE(t.observe(0.0).has_value());
  EXPECT_FALSE(t.observe(-1.0).has_value());
}

TEST(RealizedTracker, ObserveDated_RefusesReplayAndBackdate) {
  auto built = RealizedTracker::create(252.0, 10u);
  ASSERT_TRUE(built.has_value());
  RealizedTracker t = std::move(*built);
  EXPECT_TRUE(t.observe_dated(1000, 100.0).has_value());
  EXPECT_TRUE(t.observe_dated(2000, 101.0).has_value());
  const auto after_two = t.snapshot();
  EXPECT_EQ(after_two.n_obs_done, 1u);
  // exact replay: refused, state untouched
  EXPECT_FALSE(t.observe_dated(2000, 101.0).has_value());
  // backdate: refused
  EXPECT_FALSE(t.observe_dated(1500, 99.0).has_value());
  const auto still = t.snapshot();
  EXPECT_EQ(still.n_obs_done, after_two.n_obs_done);
  EXPECT_EQ(t.last_fixing_ts_ns(), 2000);
  // forward continues fine
  EXPECT_TRUE(t.observe_dated(3000, 102.0).has_value());
  EXPECT_EQ(t.snapshot().n_obs_done, 2u);
}

TEST(RealizedTracker, ObserveDated_MatchesUndatedArithmetic) {
  auto a = RealizedTracker::create(252.0, 10u);
  auto b = RealizedTracker::create(252.0, 10u);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const double spots[] = {100.0, 101.0, 99.0, 102.0};
  ASSERT_TRUE(a->observe_batch(spots).has_value());
  std::int64_t ts = 1;
  for (const double s : spots) {
    ASSERT_TRUE(b->observe_dated(ts++, s).has_value());
  }
  EXPECT_EQ(a->snapshot().n_obs_done, b->snapshot().n_obs_done);
  EXPECT_DOUBLE_EQ(a->snapshot().sum_sq_log_returns_done,
                   b->snapshot().sum_sq_log_returns_done);
}

// ── Reserved-field validation (test_deriv_reserved_validation.c) ─────────

TEST(ReservedValidation, VarStrip_ZeroReserved_Succeeds) {
  const EssviSurface surf = make_flat_surface(0.20, 0.10, 0.50);
  const CurveSet cs = make_flat_curves(100.0, 0.10, 0.50);
  const DerivConfig cfg = deriv_default_config();
  const auto q = var_swap_fair_strike(surf, cs, 0.30, cfg);
  EXPECT_TRUE(q.has_value());
}

TEST(ReservedValidation, VarStrip_NonzeroAbsPriceTol_ReturnsNotImplemented) {
  const EssviSurface surf = make_flat_surface(0.20, 0.10, 0.50);
  const CurveSet cs = make_flat_curves(100.0, 0.10, 0.50);
  DerivConfig cfg = deriv_default_config();
  cfg.abs_price_tol = 1.0e-6;
  const auto q = var_swap_fair_strike(surf, cs, 0.30, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::NotImplemented);
}

TEST(ReservedValidation, VolStrip_NonzeroRelPriceTol_ReturnsNotImplemented) {
  const EssviSurface surf = make_flat_surface(0.20, 0.10, 0.50);
  const CurveSet cs = make_flat_curves(100.0, 0.10, 0.50);
  DerivConfig cfg = deriv_default_config();
  cfg.rel_price_tol = 1.0e-3;
  const auto q = vol_swap_fair_strike(surf, cs, 0.30, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::NotImplemented);
}

TEST(ReservedValidation, DerivPrice_NonzeroFlagsRequest_ReturnsNotImplemented) {
  const EssviSurface surf = make_flat_surface(0.20, 0.10, 0.50);
  const CurveSet cs = make_flat_curves(100.0, 0.10, 0.50);
  DerivConfig cfg = deriv_default_config();
  cfg.flags_request = 1u;

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.30;
  c.strike_dec = 0.04;
  c.notional = 1.0;

  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::NotImplemented);
}

TEST(ReservedValidation, DerivPrice_NonzeroCapDecOnVarSwap_ReturnsInvalidArgument) {
  const EssviSurface surf = make_flat_surface(0.20, 0.10, 0.50);
  const CurveSet cs = make_flat_curves(100.0, 0.10, 0.50);
  const DerivConfig cfg = deriv_default_config();

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.30;
  c.strike_dec = 0.04;
  c.notional = 1.0;
  c.cap_dec = 0.10;  // cap_dec is capped-kinds-only; VarSwap must reject it

  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
}

// ── Wing clamp (uncertified-wing discipline) ──────────────────────────────
//
// The fit pipeline certifies a surface's no-arbitrage properties only on
// |k| <= 0.5 (RiskSurfaceValidationConfig{}.k_min/k_max); beyond that band a
// parametric eSSVI/SVI slice serves an UNBOUNDED linear-in-|k| extrapolation
// that no quote ever disciplined. The strip's Standard span is ±1.5, so by
// default ~2/3 of the integration span read pure extrapolation — on the
// sp100-2026 XOM corpus that inflated the 3M fair strike from ~30 to ~38 vol
// and put ~98% of its day-to-day mark variance in the fictional wings.
//
// `DerivConfig::wing_clamp_k` fixes the READS, not the span: nodes beyond the
// trust band price at their true strikes under the BAND-EDGE vol (flat-vol
// tails — no truncation bias), 0 selects the certified band, < 0 restores the
// old unclamped behavior, and `DerivFlags::WingClamped` records that the strip
// span exceeded the trust band so a mark's provenance is inspectable.

// Steep-wing eSSVI surface: same phi/rho on both slices so the time-interp of
// total variance stays exactly eSSVI-shaped at every k; ATM iv is sigma but
// iv(-1.0) is ~2.1x sigma — a caricature of an undisciplined fitted wing.
EssviSurface make_steep_wing_surface(double sigma, double T_lo, double T_hi) {
  EssviSurface surf(2);
  const EssviSlice s0{sigma * sigma * T_lo, 4.0, -0.7, T_lo};
  const EssviSlice s1{sigma * sigma * T_hi, 4.0, -0.7, T_hi};
  EXPECT_TRUE(surf.set_slice(0, s0).has_value());
  EXPECT_TRUE(surf.set_slice(1, s1).has_value());
  return surf;
}

// The same steep-wing eSSVI shape as `make_steep_wing_surface` above (phi =
// 4.0, rho = -0.7, theta = sigma^2*T so ATM iv == sigma at every pillar), but
// carried through the modern CurveSurface/EssviCurve container so it reaches
// the E6 PricedSurface-native overloads (Task C-6: those, not the legacy
// EssviSurface path above, are what a surface-carried certified wing band
// resolves through). Zero rate throughout, mirroring `make_flat_curves`'
// simplicity for the legacy path.
atx::vol::PricedSurface make_steep_wing_priced_surface(std::uint32_t uid, double spot,
                                                        double sigma,
                                                        const std::vector<double>& Ts) {
  atx::vol::CurveSurface cs;
  std::vector<atx::vol::SliceContext> ctx;
  std::uint16_t i = 0;
  for (const double T : Ts) {
    atx::vol::EssviParams e{};
    e.theta = sigma * sigma * T;
    e.phi = 4.0;
    e.rho = -0.7;
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = spot;
    e.expiry_id = i;
    cs.push(std::make_unique<atx::vol::EssviCurve>(e, 1.0)); // df = 1 (zero rate)
    ctx.push_back(atx::vol::SliceContext{T, spot, 0.0, 0.0, 250, 7});
    ++i;
  }
  atx::vol::PricingContext pc;
  pc.S = spot;
  pc.r = 0.0;
  pc.now_ts_ns = atx::vol::testkit::kFixtureNow;
  pc.method = atx::vol::AmericanMethod::AndersenLake;
  pc.al_opts = atx::vol::al_fast_opts();
  pc.uid = uid;
  return atx::vol::testkit::unwrap_surface(
      atx::vol::PricedSurface::create(std::move(cs), std::move(ctx), pc));
}

// Hand Simpson replication of the strip with vol reads clamped to [-band, band]
// on the exact grid the quote reports. Same quadrature convention on purpose:
// the assertion is about WHICH vol each node reads, not about quadrature -- so
// this walks the same C-3 kink-aligned panels (`plan_strip_split`, itself
// pinned by StripQuadrature.PlanSplitKeepsKinksOnPanelBoundaries) rather than
// one uniform grid, and stays exact to the last bit.
double clamped_strip_oracle(const EssviSurface& surf, const CurveSet& cs, double T,
                            const atx::vol::DerivQuote& grid_src, double band) {
  const double F = cs.spot;          // flat curves: F == spot at every pillar
  const double df = cs.yield.disc(T); // zero rates: 1.0
  const auto split = atx::vol::strip::plan_strip_split(
      grid_src.strip_k_lo_used, grid_src.strip_k_hi_used, grid_src.strip_nodes_used,
      band);
  double integral = 0.0;
  for (std::size_t p = 0; p < split.count; ++p) {
    const auto& panel = split.panels[p];
    const std::size_t np = panel.n_nodes;
    const double dx = (panel.k_hi - panel.k_lo) / static_cast<double>(np - 1);
    double sum = 0.0;
    for (std::size_t i = 0; i < np; ++i) {
      const double x = (i == 0)        ? panel.k_lo
                       : (i + 1 == np) ? panel.k_hi
                                       : panel.k_lo + dx * static_cast<double>(i);
      const double K = F * std::exp(x);
      const double x_read = std::clamp(x, -band, band);
      const double sigma = surf.iv(x_read, T);
      const double price = atx::vol::black76_price(F, K, T, sigma, df,
                                                   x < 0.0 ? atx::vol::Side::Put
                                                           : atx::vol::Side::Call);
      sum += atx::vol::strip::simpson_weight(i, np) * price / (df * K);
    }
    integral += sum * (dx / 3.0);
  }
  return (2.0 / T) * integral;
}

TEST(WingClamp, DefaultClampReadsFlatBeyondCertifiedBand) {
  const double sigma = 0.30;
  const double T_test = 0.25;
  const EssviSurface surf = make_steep_wing_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  // The wing really is steep and really is read by the unclamped strip.
  ASSERT_GT(surf.iv(-1.0, T_test), 1.8 * sigma);

  DerivConfig off = deriv_default_config();
  off.wing_clamp_k = -1.0;  // old behavior: read the raw wing everywhere
  const auto q_off = var_swap_fair_strike(surf, cs, T_test, off);
  ASSERT_TRUE(q_off.has_value());
  EXPECT_FALSE(has_flag(q_off->flags, DerivFlags::WingClamped));

  const auto q_def = var_swap_fair_strike(surf, cs, T_test, deriv_default_config());
  ASSERT_TRUE(q_def.has_value());

  // Same span and node count — the clamp changes reads, never the grid.
  EXPECT_EQ(q_def->strip_nodes_used, q_off->strip_nodes_used);
  EXPECT_EQ(q_def->strip_k_lo_used, q_off->strip_k_lo_used);
  EXPECT_EQ(q_def->strip_k_hi_used, q_off->strip_k_hi_used);

  // Flat tails cut the fictional wing contribution, so the clamped strike is
  // strictly below the raw one, and its provenance says so.
  EXPECT_LT(q_def->fair_strike_dec, q_off->fair_strike_dec);
  EXPECT_TRUE(has_flag(q_def->flags, DerivFlags::WingClamped));

  // And it is exactly the certified-band flat-tail strip, node for node.
  const double oracle = clamped_strip_oracle(surf, cs, T_test, *q_def, 0.5);
  EXPECT_NEAR(q_def->fair_strike_dec, oracle, 1.0e-12);
}

TEST(WingClamp, FlatSmileStrikeUnchanged) {
  const double sigma = 0.20;
  const double T_test = 0.25;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivConfig off = deriv_default_config();
  off.wing_clamp_k = -1.0;
  const auto q_off = var_swap_fair_strike(surf, cs, T_test, off);
  const auto q_def = var_swap_fair_strike(surf, cs, T_test, deriv_default_config());
  ASSERT_TRUE(q_off.has_value());
  ASSERT_TRUE(q_def.has_value());

  // On a flat smile the band-edge vol IS the wing vol, so clamping moves
  // nothing; the flag still records that tail nodes were read at the edge.
  EXPECT_NEAR(q_def->fair_strike_dec, q_off->fair_strike_dec, 1.0e-9);
  EXPECT_NEAR(q_def->fair_strike_dec, sigma * sigma, 5.0e-5);
  EXPECT_TRUE(has_flag(q_def->flags, DerivFlags::WingClamped));
}

TEST(WingClamp, ExplicitBandTightensMonotonically) {
  const double T_test = 0.25;
  const EssviSurface surf = make_steep_wing_surface(0.30, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivConfig tight = deriv_default_config();
  tight.wing_clamp_k = 0.25;
  DerivConfig off = deriv_default_config();
  off.wing_clamp_k = -1.0;

  const auto q_tight = var_swap_fair_strike(surf, cs, T_test, tight);
  const auto q_def = var_swap_fair_strike(surf, cs, T_test, deriv_default_config());
  const auto q_off = var_swap_fair_strike(surf, cs, T_test, off);
  ASSERT_TRUE(q_tight.has_value());
  ASSERT_TRUE(q_def.has_value());
  ASSERT_TRUE(q_off.has_value());

  // On a monotone-steepening smile, a tighter trust band flattens more of the
  // wing and can only lower the strike.
  EXPECT_LT(q_tight->fair_strike_dec, q_def->fair_strike_dec);
  EXPECT_LT(q_def->fair_strike_dec, q_off->fair_strike_dec);
}

TEST(WingClamp, NonFiniteBandRejected) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivConfig cfg = deriv_default_config();
  cfg.wing_clamp_k = std::numeric_limits<double>::quiet_NaN();
  const auto q = var_swap_fair_strike(surf, cs, 0.25, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
}

TEST(WingClamp, FlagPropagatesThroughDerivPrice) {
  const EssviSurface surf = make_steep_wing_surface(0.30, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.25;
  c.strike_dec = 0.09;
  c.notional = 1.0;
  c.rv_spec.n_obs_total = 63;

  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value());
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::WingClamped));
}

// Review fix I-2 (C-5) + aggregate review fix I-1 (C-R, Important #1):
// price_vol_swap's unaged branch runs up to TWO separate strip evaluations --
// the center's own (inside vol_swap_fair_strike, only under Refined) and the
// best-effort diagnostic at :637 (UNCONDITIONAL: every unaged VolSwap price
// pays it, Naive or Refined). Pre-I-2, neither one's provenance flags reached
// the dispatch quote. I-2 wired the center's own flags in -- visible only
// under Refined, since Naive's center never runs a strip at all
// (carr_lee_k_vol is a closed-form ATMF-straddle read). I-1 wired the
// diagnostic's flags in too, unconditionally -- so under Naive, a dispatch
// quote can now carry WingClamped/StripTruncated*/LowT/InteriorBadNodes from
// the diagnostic strip even though the CENTER price itself never ran one
// (this was the "dead on this path for every default caller" gap I-1
// closed). Same steep-wing fixture as WingClamp.FlagPropagatesThroughDerivPrice.
TEST(CarrLee, StripFlagsPropagateThroughDerivPriceUnderBothForms) {
  const EssviSurface surf = make_steep_wing_surface(0.30, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const double T_test = 0.25;

  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T_test;
  c.strike_dec = 0.30;
  c.notional = 1.0;
  c.rv_spec.n_obs_total = 63;  // unaged (n_obs_done defaults to 0)

  // Naive: independent oracle first -- call the SAME diagnostic strip
  // directly, exactly as :637 does, and confirm it wing-clamps on this
  // surface, so the propagation assertion below traces a real signal rather
  // than a coincidence.
  DerivConfig naive_cfg = deriv_default_config();
  naive_cfg.carr_lee_form = atx::vol::CarrLeeForm::Naive;
  const auto strip_direct = var_swap_fair_strike(surf, cs, T_test, naive_cfg);
  ASSERT_TRUE(strip_direct.has_value());
  ASSERT_TRUE(has_flag(strip_direct->flags, DerivFlags::WingClamped));

  const auto naive_q = deriv_price(surf, cs, c, naive_cfg);
  ASSERT_TRUE(naive_q.has_value());
  EXPECT_TRUE(has_flag(naive_q->flags, DerivFlags::WingClamped));

  // Refined: the center's OWN strip wing-clamps too (I-2) -- still true, now
  // for two independent reasons (its own strip AND the diagnostic).
  DerivConfig refined_cfg = deriv_default_config();
  refined_cfg.carr_lee_form = atx::vol::CarrLeeForm::Refined;
  const auto refined_q = deriv_price(surf, cs, c, refined_cfg);
  ASSERT_TRUE(refined_q.has_value());
  EXPECT_TRUE(has_flag(refined_q->flags, DerivFlags::WingClamped));
}

// ── FIT-C7 / Task C-6: surface-carried certified wing band ────────────────
//
// The strip's mode-blind default band (`strip::kCertifiedWingHalfBand`, 0.5)
// is only what the FIT PIPELINE actually certifies for a BALANCED-quality
// surface (`risk_validation_config`, pricer_fitter.cpp). A Latency-quality
// surface certifies a NARROWER ±0.35, and a default-config quote against it
// used to read the uncertified [0.35, 0.5] band as trusted — precisely what
// the clamp exists to prevent. `var_swap_fair_strike` (etc.)'s PricedSurface-
// native overloads now accept the surface's own certified band explicitly
// (`atx::vol::certified_wing_half_band(quality_mode)`, surface_policy.hpp);
// `wing_clamp_k == 0` resolves through it when supplied.
//
// THIS IS THE MECHANISM, NOT THE WIRING. These tests exercise
// `resolve_wing_clamp` directly through the low-level entry points and prove
// the mechanism is correct; they do NOT by themselves prove any production
// caller actually supplies a band. That is a SEPARATE fact, proved where the
// callers live: `SwapLeg.SolveCycleSwapTrustsSurfaceCertifiedWingBandWhenSupplied`
// (swap_leg_test.cpp — `backtest.cpp`'s swap mark lane and every
// `DeclarativeStrategy` swap leg) and
// `DerivBook.WingBandResolverAppliesTheCallersCertifiedBandPerRow`
// (deriv_book_test.cpp — `price_deriv_book`'s public API). Review round 1
// (task-C-6-review.md, CRITICAL-1/2) found the first delivery had ONLY this
// half: the mechanism existed but no call site used it, and this file's
// `q_default` case below was mislabeled in a way that read as "the fix" when
// it was actually still reproducing the pre-fix number for a Latency surface.

TEST(WingClamp, SurfaceCertifiedBandOverridesModeBlindDefault) {
  const double sigma = 0.30;
  const double T_test = 0.35; // a fitted pillar of the fixture grid below
  const std::vector<double> Ts = {0.10, T_test, 1.00};
  const atx::vol::PricedSurface ps = make_steep_wing_priced_surface(9001, 100.0, sigma, Ts);

  const double latency_band =
      atx::vol::certified_wing_half_band(atx::vol::FitQualityMode::Latency);
  ASSERT_DOUBLE_EQ(latency_band, 0.35);

  // NO SURFACE-CARRIED BAND SUPPLIED: this is NOT "the Latency behaviour" --
  // it is the UNWIRED-CALLER fallback, the exact FIT-C7 gap that stays open
  // for any caller that never resolves the surface's own quality mode. It
  // remains correct and necessary (an unknown-provenance surface must not
  // invent trust it was never given), but it is not what a Latency-fit
  // surface's mark SHOULD read once its caller is wired -- see q_latency
  // below, and the production-call-site tests cross-referenced above.
  const auto q_unwired = atx::vol::var_swap_fair_strike(ps, T_test);
  ASSERT_TRUE(q_unwired.has_value()) << q_unwired.error().to_string();
  EXPECT_DOUBLE_EQ(q_unwired->resolved_wing_clamp, atx::vol::strip::kCertifiedWingHalfBand);
  EXPECT_TRUE(has_flag(q_unwired->flags, DerivFlags::WingClamped));

  // Same default config, but the caller now states the surface's own Latency
  // build quality mode (exactly what backtest.cpp/swap_leg.cpp/deriv_book.cpp
  // now do via `certified_wing_band_for`/`WingBandResolver`): the strip must
  // trust ONLY the certified 0.35 band. THIS is the fixed behaviour.
  const auto q_latency =
      atx::vol::var_swap_fair_strike(ps, T_test, deriv_default_config(), latency_band);
  ASSERT_TRUE(q_latency.has_value()) << q_latency.error().to_string();
  EXPECT_DOUBLE_EQ(q_latency->resolved_wing_clamp, latency_band);
  EXPECT_TRUE(has_flag(q_latency->flags, DerivFlags::WingClamped));

  // Same span and node count either way -- the certified band changes reads,
  // never the grid (mirrors WingClamp.DefaultClampReadsFlatBeyondCertifiedBand).
  EXPECT_EQ(q_unwired->strip_nodes_used, q_latency->strip_nodes_used);
  EXPECT_EQ(q_unwired->strip_k_lo_used, q_latency->strip_k_lo_used);
  EXPECT_EQ(q_unwired->strip_k_hi_used, q_latency->strip_k_hi_used);

  // The mark actually moves: flattening more of a steepening wing under the
  // tighter certified band can only lower the strike further (mirrors
  // WingClamp.ExplicitBandTightensMonotonically) -- this IS the fix.
  EXPECT_LT(q_latency->fair_strike_dec, q_unwired->fair_strike_dec);
}

TEST(WingClamp, BalancedSurfaceCertifiedBandBitIdenticalToModeBlindDefault) {
  const double sigma = 0.30;
  const double T_test = 0.35;
  const std::vector<double> Ts = {0.10, T_test, 1.00};
  const atx::vol::PricedSurface ps = make_steep_wing_priced_surface(9002, 100.0, sigma, Ts);

  const double balanced_band =
      atx::vol::certified_wing_half_band(atx::vol::FitQualityMode::Balanced);
  ASSERT_DOUBLE_EQ(balanced_band, atx::vol::strip::kCertifiedWingHalfBand);

  const auto q_default = atx::vol::var_swap_fair_strike(ps, T_test);
  const auto q_balanced =
      atx::vol::var_swap_fair_strike(ps, T_test, deriv_default_config(), balanced_band);
  ASSERT_TRUE(q_default.has_value());
  ASSERT_TRUE(q_balanced.has_value());

  // A Balanced-quality surface's own certified band IS the mode-blind
  // default -- stating it explicitly must move nothing, bit for bit.
  EXPECT_EQ(q_default->resolved_wing_clamp, q_balanced->resolved_wing_clamp);
  EXPECT_EQ(q_default->fair_strike_dec, q_balanced->fair_strike_dec);
  EXPECT_EQ(q_default->strip_k_lo_used, q_balanced->strip_k_lo_used);
  EXPECT_EQ(q_default->strip_k_hi_used, q_balanced->strip_k_hi_used);
}

TEST(WingClamp, SurfaceCertifiedBandHonorsExplicitOverride) {
  const double sigma = 0.30;
  const double T_test = 0.35;
  const std::vector<double> Ts = {0.10, T_test, 1.00};
  const atx::vol::PricedSurface ps = make_steep_wing_priced_surface(9003, 100.0, sigma, Ts);
  const double latency_band =
      atx::vol::certified_wing_half_band(atx::vol::FitQualityMode::Latency);

  // An explicit caller override still wins over any surface-carried band --
  // spec: "Explicit >0 and <0 semantics unchanged."
  DerivConfig tight = deriv_default_config();
  tight.wing_clamp_k = 0.15;
  const auto q_tight = atx::vol::var_swap_fair_strike(ps, T_test, tight, latency_band);
  ASSERT_TRUE(q_tight.has_value());
  EXPECT_DOUBLE_EQ(q_tight->resolved_wing_clamp, 0.15);

  DerivConfig off = deriv_default_config();
  off.wing_clamp_k = -1.0;
  const auto q_off = atx::vol::var_swap_fair_strike(ps, T_test, off, latency_band);
  ASSERT_TRUE(q_off.has_value());
  EXPECT_DOUBLE_EQ(q_off->resolved_wing_clamp, 0.0);
  EXPECT_FALSE(has_flag(q_off->flags, DerivFlags::WingClamped));
}

// Review fix I-5 (Task C-6, round 2): the ORIGINAL version of this test only
// asserted the CENTER quote's `resolved_wing_clamp` and that the greeks were
// finite -- both hold whether or not `pin_center_scheme` pins the band into
// the bumps (the center is priced through `PricedSurfaceStripView`, which
// carries the band regardless; a wrong, WIDER clamp on the bumps still
// produces a finite, plausible-looking greek, just the WRONG one). It gave no
// coverage for the actual invariant the pin exists to hold.
//
// This version compares two paths that MUST agree bit-for-bit if the pin is
// present, and provably diverge if it is not:
//
//   Path A (surface-carried): default cfg (wing_clamp_k == 0), the band
//     supplied via `certified_wing_band` on the surface adapter. Every bump
//     prices through RespotView/VolShiftView, which carries no such member,
//     so path A's bumps depend ENTIRELY on `pin_center_scheme` writing the
//     center's resolved 0.35 into `cfg_pinned.wing_clamp_k` for them to see.
//   Path B (explicit override): the SAME 0.35, but as `cfg.wing_clamp_k`
//     directly. `resolve_wing_clamp`'s `> 0` branch never consults the
//     surface, so path B resolves identically for the center AND every bump
//     regardless of adapter type -- it needs no pin, and serves as the
//     independent reference.
//
// Center and bumps alike then resolve the identical wing_band (0.35) via the
// identical resolve_wing_clamp branch (Path A's explicit-override pin makes
// its bumps take the SAME branch Path B's always took), off the SAME surface
// and contract, so the two paths are bit-identical BY CONSTRUCTION whenever
// the pin does its job. Remove `pin_center_scheme`'s wing-clamp pin and path
// A's bumps silently fall back to the mode-blind 0.5 (RespotView/VolShiftView
// carry no provenance) while path B's stay pinned at 0.35 regardless -- the
// two diverge, which is exactly what this comparison is built to catch.
TEST(WingClamp, SurfaceCertifiedBandStaysPinnedAcrossGreekBumps) {
  const double sigma = 0.30;
  const double T_test = 0.35;
  const std::vector<double> Ts = {0.10, T_test, 1.00};
  const atx::vol::PricedSurface ps = make_steep_wing_priced_surface(9004, 100.0, sigma, Ts);
  const double latency_band =
      atx::vol::certified_wing_half_band(atx::vol::FitQualityMode::Latency);

  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T_test;
  c.strike_dec = sigma * sigma;
  c.notional = 1.0e6;
  c.rv_spec.n_obs_total = 63;

  const auto g_surface = atx::vol::deriv_greeks(ps, c, deriv_default_config(),
                                                atx::vol::DerivGreekBumps{}, latency_band);
  ASSERT_TRUE(g_surface.has_value()) << g_surface.error().to_string();
  EXPECT_DOUBLE_EQ(g_surface->quote.resolved_wing_clamp, latency_band);

  DerivConfig explicit_cfg = deriv_default_config();
  explicit_cfg.wing_clamp_k = latency_band;
  const auto g_explicit =
      atx::vol::deriv_greeks(ps, c, explicit_cfg, atx::vol::DerivGreekBumps{});
  ASSERT_TRUE(g_explicit.has_value()) << g_explicit.error().to_string();
  EXPECT_DOUBLE_EQ(g_explicit->quote.resolved_wing_clamp, latency_band);

  EXPECT_TRUE(std::isfinite(g_surface->vega));
  EXPECT_EQ(g_surface->delta, g_explicit->delta);
  EXPECT_EQ(g_surface->gamma, g_explicit->gamma);
  EXPECT_EQ(g_surface->vega, g_explicit->vega);
  EXPECT_EQ(g_surface->volga, g_explicit->volga);
  EXPECT_EQ(g_surface->vanna, g_explicit->vanna);
  EXPECT_EQ(g_surface->theta, g_explicit->theta);
  EXPECT_EQ(g_surface->rho, g_explicit->rho);
  EXPECT_EQ(g_surface->charm, g_explicit->charm);
}

// ── Wing extrapolation mode (Task F-1) ────────────────────────────────────
//
// `DerivConfig::wing_mode` picks WHAT the strip serves beyond the certified
// wing trust band (`wing_clamp_k`'s resolved band): FlatClamp (v1.1 default,
// unchanged from every WingClamp.* test above), LeeSlopeExtrapolation (total
// variance continues at the fitted slice's OWN band-edge slope, clamped to
// Lee's [0, 2-eps] moment bound), or Raw (no clamp -- identical to
// `wing_clamp_k < 0`).

TEST(WingMode, DefaultConfigIsFlatClamp) {
  // Pins the v1.1 default explicitly: `DerivConfig{}` (every construction
  // site that predates this field) must keep pricing through FlatClamp --
  // the literal zero enumerator -- which is what makes the default path
  // bit-identical to every quote struck before `wing_mode` existed.
  EXPECT_EQ(DerivConfig{}.wing_mode, StripWingMode::FlatClamp);
  EXPECT_EQ(deriv_default_config().wing_mode, StripWingMode::FlatClamp);
  EXPECT_EQ(static_cast<std::uint8_t>(StripWingMode::FlatClamp), 0u);
}

// Same steep-wing eSSVI shape as `make_steep_wing_surface` (phi = 4.0, rho =
// -0.7 on both slices), evaluated at a 6M tenor -- the brief's own fixture.
// Both slices share phi/rho, so the time-interpolated total variance at any
// T between them is EXACTLY theta(T)*f(k) for the fixed shape f; theta(T) =
// sigma^2*T is itself linear in T, so interpolating between the two slices
// reproduces a single eSSVI slice at T_test exactly (no interpolation
// artifact to account for).
TEST(WingMode, OrderingUnderSkew) {
  const double sigma = 0.20;
  const double T_test = 0.5;  // 6M
  const EssviSurface surf = make_steep_wing_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivConfig flat = deriv_default_config();
  flat.wing_mode = StripWingMode::FlatClamp;
  DerivConfig lee = deriv_default_config();
  lee.wing_mode = StripWingMode::LeeSlopeExtrapolation;
  DerivConfig raw = deriv_default_config();
  raw.wing_mode = StripWingMode::Raw;

  const auto q_flat = var_swap_fair_strike(surf, cs, T_test, flat);
  const auto q_lee = var_swap_fair_strike(surf, cs, T_test, lee);
  const auto q_raw = var_swap_fair_strike(surf, cs, T_test, raw);
  ASSERT_TRUE(q_flat.has_value());
  ASSERT_TRUE(q_lee.has_value());
  ASSERT_TRUE(q_raw.has_value());

  EXPECT_LT(q_flat->fair_strike_dec, q_lee->fair_strike_dec);
  EXPECT_LE(q_lee->fair_strike_dec, q_raw->fair_strike_dec);

  EXPECT_TRUE(has_flag(q_flat->flags, DerivFlags::WingClamped));
  EXPECT_TRUE(has_flag(q_lee->flags, DerivFlags::WingExtrapolated));
  EXPECT_FALSE(has_flag(q_lee->flags, DerivFlags::WingClamped));
  EXPECT_FALSE(has_flag(q_raw->flags, DerivFlags::WingClamped));
  EXPECT_FALSE(has_flag(q_raw->flags, DerivFlags::WingExtrapolated));

  // Raw eSSVI wings are close to linear in |k| well before the certified
  // band (the fitted slice's asymptotic slope is nearly reached by k = 0.5
  // on this steep a fit -- see the task report for the measured
  // convergence), so LeeSlope's straight-line continuation from the band
  // edge tracks Raw closely: the LeeSlope-to-Raw gap should be a small
  // fraction of the FlatClamp-to-Raw gap the wing treatment spans end to end.
  const double gap_flat_to_raw = q_raw->fair_strike_dec - q_flat->fair_strike_dec;
  const double gap_lee_to_raw = q_raw->fair_strike_dec - q_lee->fair_strike_dec;
  ASSERT_GT(gap_flat_to_raw, 0.0);
  EXPECT_LT(gap_lee_to_raw / gap_flat_to_raw, 0.05);
}

TEST(WingMode, FlatSurfaceInvariant) {
  const double sigma = 0.20;
  const double T_test = 0.25;
  // EXACTLY flat: phi = 0, rho = 0 makes w(k) = theta identically (no
  // floating-point residual the way phi = 1e-6 in `make_flat_surface` would
  // leave), so LeeSlope's band-edge slope is EXACTLY 0 and every mode serves
  // the SAME constant vol at every node -- the tightest test of "the modes
  // must agree when there is no wing shape for them to disagree about".
  EssviSurface surf(2);
  const EssviSlice s0{sigma * sigma * 0.01, 0.0, 0.0, 0.01};
  const EssviSlice s1{sigma * sigma * 1.00, 0.0, 0.0, 1.00};
  ASSERT_TRUE(surf.set_slice(0, s0).has_value());
  ASSERT_TRUE(surf.set_slice(1, s1).has_value());
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivConfig flat = deriv_default_config();
  flat.wing_mode = StripWingMode::FlatClamp;
  DerivConfig lee = deriv_default_config();
  lee.wing_mode = StripWingMode::LeeSlopeExtrapolation;
  DerivConfig raw = deriv_default_config();
  raw.wing_mode = StripWingMode::Raw;

  const auto q_flat = var_swap_fair_strike(surf, cs, T_test, flat);
  const auto q_lee = var_swap_fair_strike(surf, cs, T_test, lee);
  const auto q_raw = var_swap_fair_strike(surf, cs, T_test, raw);
  ASSERT_TRUE(q_flat.has_value());
  ASSERT_TRUE(q_lee.has_value());
  ASSERT_TRUE(q_raw.has_value());

  // Review fix I-3: isolate the mechanism directly instead of arguing it in
  // prose. `FlatClamp` with the clamp turned OFF (`wing_clamp_k = -1.0`)
  // runs the exact same code path FlatClamp always has, reading the exact
  // same raw, unclamped values LeeSlope's `beta == 0` shortcut also reads
  // here (see below) -- if that is really the whole story, this must be
  // bit-exact with `q_lee`, not merely close.
  DerivConfig flat_clamp_off = deriv_default_config();
  flat_clamp_off.wing_mode = StripWingMode::FlatClamp;
  flat_clamp_off.wing_clamp_k = -1.0;
  const auto q_flat_clamp_off = var_swap_fair_strike(surf, cs, T_test, flat_clamp_off);
  ASSERT_TRUE(q_flat_clamp_off.has_value());
  EXPECT_EQ(q_lee->fair_strike_dec, q_flat_clamp_off->fair_strike_dec);

  // Measured, not the brief-literal 1e-12 (deviation, same class C-3 already
  // established this sprint: "brief's literal test params unmeasurable as
  // specified" -- task-C-3-report.md). The SERVED VOL is bit-identical
  // across all three modes on this fixture -- verified by construction:
  // phi=0/rho=0 makes w(k) EXACTLY theta for every k (0.5*x*2 round-trips x
  // exactly in IEEE754), so the central-difference slope is EXACTLY 0 and
  // `lee_slope_sigma`'s beta==0 branch (derivatives.cpp) returns the SAME
  // `surface.iv` call FlatClamp's clamped read and Raw's unclamped read both
  // make -- and the assertion just above proves it directly, not by
  // argument. The INTEGRAND is not constant (`price/(df*K)` spans ~23
  // orders of magnitude from k=-1 to k=0 on this fixture -- a genuinely
  // constant integrand would make Simpson EXACT, so if it were constant a
  // panelization difference could never move the answer at all); what is
  // constant is only the served VOL. What remains is `split_wing_band`:
  // FlatClamp splits at +-0.5 (4 panels), LeeSlope/Raw do not (2 panels,
  // since this fixture's slope is exactly 0 and so never binds the Lee
  // clamp -- see WingMode.SlopeFoldsToZeroKeepsTheEdgeSplitAndTheError
  // Honest for the case where it does) -- composite Simpson evaluates a
  // DIFFERENT set of quadrature nodes under the two panelizations of this
  // ANALYTIC (not constant) integrand, and each panelization's own
  // Richardson estimate is exactly the bound on how far its discrete sum can
  // sit from the true integral. Anchoring the gate to the SUM of both
  // modes' own reported `integration_error_est` (rather than a magic
  // constant that silently voids itself if the default grid ever changes)
  // makes that bound explicit and self-updating. This IS a loosening, and
  // said so plainly: `err_budget` below measures 1.5794e-09 on today's
  // default grid, 1.58x looser in absolute terms than the 1.0e-9 constant it
  // replaces, while the actual `gap_lee` it gates (9.6852e-11) is unchanged
  // and still passes at 6.13% of the new budget (16x margin) -- the
  // mechanism-anchored bound is the right trade for staying valid if the
  // default grid ever changes, not a tightening.
  const double gap_lee = std::fabs(q_flat->fair_strike_dec - q_lee->fair_strike_dec);
  const double gap_raw = std::fabs(q_flat->fair_strike_dec - q_raw->fair_strike_dec);
  ASSERT_TRUE(q_flat->integration_error_est == q_flat->integration_error_est);
  ASSERT_TRUE(q_lee->integration_error_est == q_lee->integration_error_est);
  ASSERT_TRUE(q_raw->integration_error_est == q_raw->integration_error_est);
  const double err_budget = q_flat->integration_error_est + q_lee->integration_error_est;
  EXPECT_LE(gap_lee, err_budget);
  EXPECT_LE(gap_raw, q_flat->integration_error_est + q_raw->integration_error_est);
  // LeeSlope and Raw share the SAME panelization (both resolve
  // split_wing_band == 0.0 -- the clamp never binds on this fixture) and,
  // via the beta==0 shortcut, the SAME reads -- bit-identical, not merely
  // quadrature-close.
  EXPECT_EQ(q_lee->fair_strike_dec, q_raw->fair_strike_dec);
  EXPECT_NEAR(q_flat->fair_strike_dec, sigma * sigma, 5.0e-5);
}

// Constructed via the C-8 HINGE_QUAD wing-residual fixture: a moderate
// backbone (same phi/rho as the skew fixture above) plus a HINGE_QUAD
// residual active just past the certified band, sized so the fitted slice's
// OWN total-variance slope at the RIGHT band edge exceeds Lee's [0, 2] moment
// bound -- exercising the clamp `clamp_lee_slope` (derivatives.cpp) exists
// for. `VolSurface` (not the legacy `EssviSurface`/`EssviSlice`, which has NO
// residual support at all -- surface.hpp's own header note) is the vehicle:
// `essvi_total_w` is the only evaluator that adds the residual atop the
// backbone (vol_surface.cpp); the legacy evaluator (surface.cpp) never does.
atx::vol::VolSurface make_hinge_quad_slope_surface(double sigma, double T) {
  atx::vol::VolSurface surf =
      atx::vol::VolSurface::create(21u, atx::vol::Parametrization::Essvi, 1).value();
  atx::vol::EssviParams p{};
  p.theta = sigma * sigma * T;
  p.phi = 4.0;
  p.rho = -0.7;
  p.T = T;
  p.expiry_id = 0;
  p.resid_scale = 0.6;
  p.resid_basis_kind = atx::vol::ResidualBasisKind::HingeQuad;
  p.resid_n_basis = 5;
  p.resid_coef[4] = 1.75;  // right-wing hinge-squared term only
  EXPECT_TRUE(surf.set_slice_essvi(0, p).has_value());
  return surf;
}

TEST(WingMode, SlopeClampBinds) {
  const double sigma = 0.20;
  const double T_test = 0.5;
  const atx::vol::VolSurface surf = make_hinge_quad_slope_surface(sigma, T_test);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  // VolSurface carries no `certified_wing_band` member (FIT-C7 legacy-
  // container carve-out), so `wing_clamp_k == 0` resolves the mode-blind
  // default -- same constant `resolve_wing_clamp` falls back to.
  const double band = atx::vol::strip::kCertifiedWingHalfBand;

  // Precondition: the fixture really does exceed Lee's bound at the band
  // edge, computed independently of any strip machinery (a bare central
  // difference of the surface's own total variance, mirroring the formula
  // `derivatives.cpp` uses but re-typed here rather than reused).
  constexpr double h = 1.0e-4;
  const auto w_at = [&](double k) {
    const double s = surf.iv(k, T_test);
    return s * s * T_test;
  };
  const double slope_raw = (w_at(band + h) - w_at(band - h)) / (2.0 * h);
  ASSERT_GT(slope_raw, 2.0) << "fixture must exceed Lee's bound to exercise the clamp";

  DerivConfig lee = deriv_default_config();
  lee.wing_mode = StripWingMode::LeeSlopeExtrapolation;
  const auto q_lee = var_swap_fair_strike(surf, cs, T_test, lee);
  ASSERT_TRUE(q_lee.has_value());
  EXPECT_TRUE(has_flag(q_lee->flags, DerivFlags::WingExtrapolated));

  // Independent oracle: re-integrate the SAME resolved grid with the
  // extrapolation formula spelled out by hand, parameterized on the
  // RIGHT-edge slope so it can be evaluated once at Lee's clamp value and
  // once at the fixture's own (excessive) raw slope.
  //
  // Review fix I-1: the split band passed to `plan_strip_split` here must
  // match what `var_swap_fair_strike`'s `split_wing_band` ACTUALLY resolves,
  // not always `0.0`. This fixture's right edge clamps (`slope_raw > 2.0`,
  // asserted above), so production now keeps the band-edge panel boundary
  // (the fix for I-1 — the served slope right at the edge is `2-eps` from
  // outside but the raw surface's own `2.534836` from inside, a genuine C0/C1
  // break that needs isolating exactly like FlatClamp's). Passing `band`
  // here reproduces that.
  constexpr double eps = 1.0e-3;
  const double w_edge_r = w_at(band);
  const double w_edge_l = w_at(-band);
  const double slope_l_raw = -(w_at(-band + h) - w_at(-band - h)) / (2.0 * h);
  const double beta_l = std::max(0.0, std::min(2.0 - eps, slope_l_raw));

  const auto oracle = [&](double beta_r) {
    const auto split = atx::vol::strip::plan_strip_split(
        q_lee->strip_k_lo_used, q_lee->strip_k_hi_used, q_lee->strip_nodes_used, band);
    const double F = cs.spot;
    const double df = cs.yield.disc(T_test);
    double integral = 0.0;
    for (std::size_t p = 0; p < split.count; ++p) {
      const auto& panel = split.panels[p];
      const std::size_t np = panel.n_nodes;
      const double dx = (panel.k_hi - panel.k_lo) / static_cast<double>(np - 1);
      double sum = 0.0;
      for (std::size_t i = 0; i < np; ++i) {
        const double x = (i == 0)        ? panel.k_lo
                         : (i + 1 == np) ? panel.k_hi
                                         : panel.k_lo + dx * static_cast<double>(i);
        double sigma;
        if (std::fabs(x) <= band) {
          sigma = surf.iv(x, T_test);
        } else if (x > 0.0) {
          sigma = std::sqrt((w_edge_r + beta_r * (x - band)) / T_test);
        } else {
          sigma = std::sqrt((w_edge_l + beta_l * (-x - band)) / T_test);
        }
        const double K = F * std::exp(x);
        const double price = atx::vol::black76_price(
            F, K, T_test, sigma, df, x < 0.0 ? atx::vol::Side::Put : atx::vol::Side::Call);
        sum += atx::vol::strip::simpson_weight(i, np) * price / (df * K);
      }
      integral += sum * (dx / 3.0);
    }
    return (2.0 / T_test) * integral;
  };

  const double k_var_clamped = oracle(2.0 - eps);
  const double k_var_unclamped = oracle(slope_raw);

  // Review fix I-1: THIS is the assertion that carries the fix for this
  // test -- the oracle's `plan_strip_split` call above now passes `band`
  // instead of unconditionally `0.0` (see the comment on that call), so a
  // bit-tight match here requires production to have made the SAME
  // clamp-bound split decision the oracle now assumes. Confirmed non-vacuous
  // by an independent reversion probe (task-F-1-fix-round-1-review.md
  // section 2): reverting only the gate back to
  // `use_lee_slope ? 0.0 : wing_band` moves `q_lee->fair_strike_dec` away
  // from this oracle by 8.247e-09 -- about 8,247x (roughly four orders of
  // magnitude) past the 1.0e-12 tolerance.
  EXPECT_NEAR(q_lee->fair_strike_dec, k_var_clamped, 1.0e-12);
  // The unclamped oracle uses a materially steeper wing (slope_raw > 2 vs the
  // clamped 2 - eps) -- if production had failed to clamp, it would match
  // THIS oracle instead, so the two oracle values must themselves be clearly
  // distinguishable for the comparison above to carry any weight.
  EXPECT_GT(std::fabs(k_var_unclamped - k_var_clamped), 1.0e-6);

  // Review fix m-2: the bound below is a SMOKE check, not I-1's evidence for
  // THIS test -- this fixture's error is dominated by resolving a served
  // wing that reaches ~200% vol (an extreme HINGE_QUAD residual, not the
  // edge kink), so its `integration_error_est` only improves modestly under
  // the fix (measured 7.6613e-08 pre-fix vs 6.0100e-08 post-fix, both three
  // decades inside a 1.0e-6 bound -- an absolute threshold this loose cannot
  // tell the two apart; confirmed by the same reversion probe, this
  // assertion PASSES unchanged under a full revert of the gate). What
  // carries I-1 for THIS test is the `EXPECT_NEAR` above, via the oracle's
  // `0.0 -> band` split change -- see WingMode.SlopeFoldsToZeroKeepsTheEdge
  // SplitAndTheErrorEstimateHonest for the fixture where the error-estimate
  // MAGNITUDE itself (not just the strike) is the load-bearing check. This
  // assertion stays only as a sanity floor (NaN-safety plus "still small in
  // an absolute sense"), not a regression pin.
  ASSERT_TRUE(q_lee->integration_error_est == q_lee->integration_error_est);
  EXPECT_LT(q_lee->integration_error_est, 1.0e-6);
}

// Review fix I-1: the REACHABLE clamp-binding case, not the exotic beta > 2
// corner `SlopeClampBinds` constructs above. Any eSSVI slice's right-wing
// total-variance slope is NEGATIVE below the smile's own minimum whenever
// rho < 0 (ordinary equity skew, not a contrived corner), so beta_right
// folds to Lee's zero floor at ordinary bands -- reproduces the review's own
// concrete fixture (sprint's steep-skew surface, phi=4.0/rho=-0.7/
// sigma_atm=0.20, T=0.5, wing_clamp_k=0.15, well inside
// certified_wing_half_band(Latency)=0.35, so a slightly steeper
// Latency-quality surface lands here by default).
TEST(WingMode, SlopeFoldsToZeroKeepsTheEdgeSplitAndTheErrorEstimateHonest) {
  const double sigma = 0.20;
  const double T_test = 0.5;
  const EssviSurface surf = make_steep_wing_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const double band = 0.15;

  // Precondition: the right edge's raw slope really is negative, so
  // `clamp_lee_slope`'s `!(beta > 0.0)` branch folds it to 0 -- the
  // reachable case, not the exotic beta > 2 one.
  constexpr double h = 1.0e-4;
  const auto w_at = [&](double k) {
    const double s = surf.iv(k, T_test);
    return s * s * T_test;
  };
  const double slope_r_raw = (w_at(band + h) - w_at(band - h)) / (2.0 * h);
  ASSERT_LT(slope_r_raw, 0.0) << "fixture must fold to zero to exercise the reachable case";

  DerivConfig flat = deriv_default_config();
  flat.wing_mode = StripWingMode::FlatClamp;
  flat.wing_clamp_k = band;
  DerivConfig lee = deriv_default_config();
  lee.wing_mode = StripWingMode::LeeSlopeExtrapolation;
  lee.wing_clamp_k = band;

  const auto q_flat = var_swap_fair_strike(surf, cs, T_test, flat);
  const auto q_lee = var_swap_fair_strike(surf, cs, T_test, lee);
  ASSERT_TRUE(q_flat.has_value());
  ASSERT_TRUE(q_lee.has_value());
  ASSERT_TRUE(has_flag(q_flat->flags, DerivFlags::WingClamped));
  ASSERT_TRUE(has_flag(q_lee->flags, DerivFlags::WingExtrapolated));

  // Same grid under both modes -- and, after this fix, the SAME reason the
  // split decision differs from the non-binding case
  // (WingMode.SplitLogicNoEdgeKinkUnderLeeSlope): the right edge's Lee clamp
  // bound here, so LeeSlope keeps the band-edge panel boundary FlatClamp
  // always has, instead of dropping it into what would otherwise be a real
  // mid-panel kink.
  ASSERT_EQ(q_flat->strip_nodes_used, q_lee->strip_nodes_used);
  ASSERT_EQ(q_flat->strip_k_lo_used, q_lee->strip_k_lo_used);
  ASSERT_EQ(q_flat->strip_k_hi_used, q_lee->strip_k_hi_used);
  // Review fix m-1: a `plan_strip_split(k_lo, k_hi, n, band)` comparison used
  // to sit here, calling that pure function on both sides with k_lo/k_hi/n
  // already asserted equal three lines up and the SAME literal `band` fed to
  // both calls -- so it compared the function to itself and could not
  // observe what `var_swap_fair_strike` actually resolved `split_wing_band`
  // to internally; it passed unchanged under a full revert of this fix.
  // What DOES observe production's actual split decision is the
  // error-estimate comparison below: a genuinely-dropped edge panel leaves
  // an unresolved C0/C1 kink mid-panel that the Richardson estimate's own
  // `/15` divisor (which assumes O(h^4) convergence, broken by the kink)
  // cannot see coming, so `integration_error_est` moves by orders of
  // magnitude exactly when the split decision is wrong and by nothing when
  // it is right -- confirmed non-vacuous by an independent reversion probe:
  // reverting only the gate back to `use_lee_slope ? 0.0 : wing_band` makes
  // the bound below fail: err_est_lee/err_est_flat measured at 43.9x under
  // reversion (2.5586e-07 vs 5.8210e-09), ~22x past the 2x bound this
  // assertion requires (task-F-1-fix-round-1-review.md section 2).
  //
  // The payoff: LeeSlope's error estimate is back to the SAME ORDER OF
  // MAGNITUDE as FlatClamp's own on the identical grid -- measured
  // 5.849e-09 vs 5.822e-09 (0.47% apart; the served function differs on the
  // LEFT wing, where the clamp did not bind, so a small residual gap is
  // expected and fine) -- a world away from the pre-fix 96.9x-17,000x
  // blowup review fix I-1 measured (the observed quadrature order collapsed
  // from 4 to ~1.9 there because a real C0/C1 break sat unsplit mid-panel).
  // 2x keeps ample margin over the measured ~1.005x while still being a
  // three-orders-of-magnitude tighter bound than the defect this pins shut.
  ASSERT_TRUE(q_flat->integration_error_est == q_flat->integration_error_est);
  ASSERT_TRUE(q_lee->integration_error_est == q_lee->integration_error_est);
  EXPECT_LT(q_lee->integration_error_est, 2.0 * q_flat->integration_error_est);
}

TEST(WingMode, RawMatchesLegacyWingClampOff) {
  const EssviSurface surf = make_steep_wing_surface(0.30, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const double T_test = 0.25;

  DerivConfig legacy_off = deriv_default_config();
  legacy_off.wing_clamp_k = -1.0;  // pre-F-1 escape hatch: clamp off
  const auto q_legacy = var_swap_fair_strike(surf, cs, T_test, legacy_off);
  ASSERT_TRUE(q_legacy.has_value());

  // wing_mode alone, with wing_clamp_k left at its OWN default (0, "resolve
  // the certified band") -- StripWingMode::Raw must still win, proving it is
  // not merely riding along on wing_clamp_k's sign.
  DerivConfig raw_mode = deriv_default_config();
  raw_mode.wing_mode = StripWingMode::Raw;
  ASSERT_EQ(raw_mode.wing_clamp_k, 0.0);
  const auto q_raw = var_swap_fair_strike(surf, cs, T_test, raw_mode);
  ASSERT_TRUE(q_raw.has_value());

  EXPECT_EQ(q_legacy->fair_strike_dec, q_raw->fair_strike_dec);
  EXPECT_EQ(q_legacy->flags, q_raw->flags);
  EXPECT_FALSE(has_flag(q_raw->flags, DerivFlags::WingClamped));
  EXPECT_FALSE(has_flag(q_raw->flags, DerivFlags::WingExtrapolated));
}

// C-3's split logic (`plan_strip_split`) must handle the new mode: the band
// edge is a genuine C0 kink under FlatClamp (d(iv)/dk drops to zero across
// it) and stays a panel boundary; LeeSlopeExtrapolation is continuous AND
// slope-matched there when the clamp does not bind, so no panel boundary is
// needed. This is the CONTROL for the non-binding regime -- this fixture's
// right-edge raw slope at this band is measured positive and inside Lee's
// [0, 2-eps] bound (see the in-test note below), so `split_wing_band`
// resolves to 0.0 for LeeSlope here both before and after review fix I-1's
// clamp-bound gate; the binding regime, where the gate actually changes the
// outcome, is WingMode.SlopeFoldsToZeroKeepsTheEdgeSplitAndTheErrorEstimate
// Honest's job, not this test's. The payoff of not needing the edge panel is
// demonstrated via the Richardson error estimate staying small for BOTH
// modes -- proving the missing edge panel costs LeeSlope nothing, not merely
// asserting it in prose.
TEST(WingMode, SplitLogicNoEdgeKinkUnderLeeSlope) {
  const double sigma = 0.20;
  const double T_test = 0.5;
  const EssviSurface surf = make_steep_wing_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivConfig flat = deriv_default_config();
  flat.wing_mode = StripWingMode::FlatClamp;
  DerivConfig lee = deriv_default_config();
  lee.wing_mode = StripWingMode::LeeSlopeExtrapolation;

  const auto q_flat = var_swap_fair_strike(surf, cs, T_test, flat);
  const auto q_lee = var_swap_fair_strike(surf, cs, T_test, lee);
  ASSERT_TRUE(q_flat.has_value());
  ASSERT_TRUE(q_lee.has_value());
  // Both must actually engage the clamp/extrapolation for this to be a
  // meaningful test of the split logic at the band edge.
  ASSERT_TRUE(has_flag(q_flat->flags, DerivFlags::WingClamped));
  ASSERT_TRUE(has_flag(q_lee->flags, DerivFlags::WingExtrapolated));
  // Same grid under both modes -- the mode changes reads, never the span/
  // node count (same invariant WingClamp.DefaultClampReadsFlatBeyondCertified
  // Band pins for wing_clamp_k).
  ASSERT_EQ(q_flat->strip_nodes_used, q_lee->strip_nodes_used);
  ASSERT_EQ(q_flat->strip_k_lo_used, q_lee->strip_k_lo_used);
  ASSERT_EQ(q_flat->strip_k_hi_used, q_lee->strip_k_hi_used);

  // Precondition, mirroring WingMode.SlopeFoldsToZeroKeepsTheEdgeSplitAndThe
  // ErrorEstimateHonest's own check: the right edge's raw slope at THIS
  // fixture's default-resolved band (0.5, certified_wing_half_band(Balanced))
  // is inside Lee's [0, 2-eps] bound, so the clamp does not bind and
  // `split_wing_band` resolves to 0.0 for LeeSlope -- the SAME value it
  // resolved to before review fix I-1 existed, which is what makes this test
  // a control rather than a regression pin for that fix.
  constexpr double h = 1.0e-4;
  const auto w_at = [&](double k) {
    const double s = surf.iv(k, T_test);
    return s * s * T_test;
  };
  const double default_band = 0.5;
  const double slope_r_raw =
      (w_at(default_band + h) - w_at(default_band - h)) / (2.0 * h);
  ASSERT_GE(slope_r_raw, 0.0) << "fixture must NOT fold to zero -- this is the control";
  ASSERT_LT(slope_r_raw, 2.0) << "fixture must NOT exceed Lee's bound -- this is the control";

  // Structural well-formedness check at the two band values `split_wing_band`
  // resolves to for each mode here (0.5 for FlatClamp, 0.0 for LeeSlope --
  // see the precondition above): `plan_strip_split` itself must produce a
  // panelization on the 4m+1 Richardson lattice for either.
  //
  // Review fix m-1: this used to also assert
  // `EXPECT_EQ(split_flat.count, 4u)` / `EXPECT_EQ(split_lee.count, 2u)` --
  // both pure restatements of `plan_strip_split`'s own arithmetic at literal
  // hardcoded bands, never reading back what `var_swap_fair_strike` actually
  // resolved `split_wing_band` to internally for either mode; removed rather
  // than left as an assertion that read as evidence while carrying none.
  // What DOES read production's own numbers back is the error-estimate
  // check below.
  const auto split_flat = atx::vol::strip::plan_strip_split(
      q_flat->strip_k_lo_used, q_flat->strip_k_hi_used, q_flat->strip_nodes_used, 0.5);
  const auto split_lee = atx::vol::strip::plan_strip_split(
      q_lee->strip_k_lo_used, q_lee->strip_k_hi_used, q_lee->strip_nodes_used, 0.0);

  // Payoff for not needing that extra split: the Richardson estimate stays
  // valid (every panel 4m+1) and small for BOTH. If LeeSlope's node reads
  // left a genuine unresolved kink at the edge (e.g. this gating were wrong
  // and a real discontinuity went unsplit), the /15 estimate would inflate
  // the way C-3's own finding measured on a misaligned kink (574x-4.1e4x) --
  // it does not, because there is no kink to misalign here.
  ASSERT_TRUE(split_flat.richardson_ok);
  ASSERT_TRUE(split_lee.richardson_ok);
  ASSERT_TRUE(q_flat->integration_error_est == q_flat->integration_error_est);
  ASSERT_TRUE(q_lee->integration_error_est == q_lee->integration_error_est);
  EXPECT_LT(q_flat->integration_error_est, 1.0e-6);
  EXPECT_LT(q_lee->integration_error_est, 1.0e-6);
}

TEST(SurfacePolicy, CertifiedWingHalfBandMatchesEachQualityMode) {
  using atx::vol::FitQualityMode;
  EXPECT_DOUBLE_EQ(atx::vol::certified_wing_half_band(FitQualityMode::Latency), 0.35);
  EXPECT_DOUBLE_EQ(atx::vol::certified_wing_half_band(FitQualityMode::Balanced), 0.50);
  EXPECT_DOUBLE_EQ(atx::vol::certified_wing_half_band(FitQualityMode::Accuracy), 0.60);
  // Balanced is the strip's own mode-blind default -- the identity this
  // whole feature is built on (also static_asserted in derivatives.cpp).
  EXPECT_DOUBLE_EQ(atx::vol::certified_wing_half_band(FitQualityMode::Balanced),
                   atx::vol::strip::kCertifiedWingHalfBand);
}

// MUST-FIX 2 (C-6 I-6): `certified_wing_half_band`'s three literals are a
// hand-kept COPY of `risk_validation_config`'s `k_max` column (the fit-time
// band the independent risk oracle actually certifies, pricer_fitter.cpp);
// only the Balanced case was cross-checked (the test above, against the
// SEPARATE `strip::kCertifiedWingHalfBand` constant -- not against
// `risk_validation_config` itself). Correct today for all three modes
// (verified below), but nothing previously caught the two literals drifting
// apart -- silently re-creating FIT-C7 elsewhere. This is also the phase's
// one live oracle gap (aggregate review §4): the WingClamp.SurfaceCertified
// BandOverridesModeBlindDefault 0.35 pin is independent of the RESOLVER, but
// was never checked against the fact it claims to mirror.
TEST(SurfacePolicy, CertifiedWingHalfBandMatchesRiskValidationConfig) {
  using atx::vol::FitQualityMode;
  for (const FitQualityMode mode :
       {FitQualityMode::Latency, FitQualityMode::Balanced, FitQualityMode::Accuracy}) {
    EXPECT_DOUBLE_EQ(atx::vol::certified_wing_half_band(mode),
                     atx::vol::risk_validation_config(mode).k_max)
        << "mode=" << atx::vol::to_string(mode);
  }
}

// ── Kind x engine dispatch matrix (PV-5) ──────────────────────────────────
//
// deriv_price now enforces the full matrix: VarSwap -> {Auto,
// StripLogContract}; VolSwap -> {Auto, VolCarrLee (unaged only, as before),
// RvDistributionProxy}; CappedVarSwap/CappedVolSwap -> {Auto,
// RvDistributionProxy} (already covered by ReservedValidation/existing
// capped-kind tests). The two combinations below used to fall through to a
// DIFFERENT engine's math with no error and no flag -- price_var_swap never
// read cfg.engine at all, and price_vol_swap's unaged branch only tested
// `cfg.engine != RvDistributionProxy`, not which engine it actually was.

TEST(Dispatch, EngineKindMatrixEnforced) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  {
    DerivConfig cfg = deriv_default_config();
    cfg.engine = DerivEngine::VolCarrLee;

    DerivContract c{};
    c.kind = DerivKind::VarSwap;
    c.maturity_t = 0.25;
    c.strike_dec = 0.04;
    c.notional = 1.0;

    const auto q = deriv_price(surf, cs, c, cfg);
    ASSERT_FALSE(q.has_value());
    EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
  }
  {
    DerivConfig cfg = deriv_default_config();
    cfg.engine = DerivEngine::StripLogContract;

    DerivContract c{};
    c.kind = DerivKind::VolSwap;
    c.maturity_t = 0.25;
    c.strike_dec = 0.20;
    c.notional = 1.0;

    const auto q = deriv_price(surf, cs, c, cfg);
    ASSERT_FALSE(q.has_value());
    EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
  }
  // Task F-2: GammaSwap joins the matrix at the exact same cells VarSwap
  // occupies (both are strip-only kinds with no VolCarrLee/RvDistribution*
  // formula of their own).
  {
    DerivConfig cfg = deriv_default_config();
    cfg.engine = DerivEngine::VolCarrLee;

    DerivContract c{};
    c.kind = DerivKind::GammaSwap;
    c.maturity_t = 0.25;
    c.strike_dec = 0.04;
    c.notional = 1.0;

    const auto q = deriv_price(surf, cs, c, cfg);
    ASSERT_FALSE(q.has_value());
    EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
  }
  {
    // RvDistributionProxy is reserved for GammaSwap: the reserved-engine
    // switch's allow-list (CappedVarSwap/CappedVolSwap/VolSwap only) was
    // deliberately NOT extended for this task -- GammaSwap has no
    // distribution-model formula, wired or otherwise.
    DerivConfig cfg = deriv_default_config();
    cfg.engine = DerivEngine::RvDistributionProxy;

    DerivContract c{};
    c.kind = DerivKind::GammaSwap;
    c.maturity_t = 0.25;
    c.strike_dec = 0.04;
    c.notional = 1.0;

    const auto q = deriv_price(surf, cs, c, cfg);
    ASSERT_FALSE(q.has_value());
    EXPECT_EQ(q.error().code(), ErrorCode::NotImplemented);
  }
  {
    // The two legal engines both reach the SAME strip (price_gamma_swap never
    // reads cfg.engine, exactly like price_var_swap) -- Auto and
    // StripLogContract must price identically.
    DerivContract c{};
    c.kind = DerivKind::GammaSwap;
    c.maturity_t = 0.25;
    c.strike_dec = 0.04;
    c.notional = 1.0;

    DerivConfig cfg_auto = deriv_default_config();
    cfg_auto.engine = DerivEngine::Auto;
    DerivConfig cfg_strip = deriv_default_config();
    cfg_strip.engine = DerivEngine::StripLogContract;

    const auto q_auto = deriv_price(surf, cs, c, cfg_auto);
    const auto q_strip = deriv_price(surf, cs, c, cfg_strip);
    ASSERT_TRUE(q_auto.has_value());
    ASSERT_TRUE(q_strip.has_value());
    EXPECT_DOUBLE_EQ(q_auto->fair_strike_dec, q_strip->fair_strike_dec);
  }
}

// ── Interior bad-node accounting (PV-4) ───────────────────────────────────
//
// Before this task only the two grid ENDPOINTS were checked for a non-
// finite/non-positive surface read (bad_first/bad_last -> StripTruncated
// Left/Right); a node strictly inside the grid silently contributed 0 to the
// integral with no trace anywhere in the returned quote.
//
// Raw SVI has no arb-free enforcement (svi_w's own doc: "no domain
// restrictions ... are enforced here"), so a slice can be built directly
// (bypassing the make_skew_surface-style butterfly-bound assert) with a
// vertex value low enough that w(k) = a + b*(rho*(k-m) + sqrt((k-m)^2 +
// sigma^2)) dips below zero in a band around k = m, while staying positive
// at k = 0 (ATM) and at the grid's far wings -- the sqrt term dominates and
// grows without bound away from m, so the dip is an isolated hole, not a
// truncation. iv() (Surface<Slice>::iv) already turns any w <= 0 into NaN.
//
// Both tests pin an EXACT, symmetric grid ([-1, 1], wing clamp off, 401
// nodes) so the split (plan_strip_split) cuts only at k = 0 into two equal
// 201-node panels, dx = 1/200 = 0.005 per panel -- placing the vertex at
// m = 0.5 exactly on a grid node and making the negative-w half-width
// precisely controllable against that spacing.
DerivConfig pinned_symmetric_strip_cfg() {
  DerivConfig cfg = deriv_default_config();
  cfg.k_min_log = -1.0;
  cfg.k_max_log = 1.0;
  cfg.strip_nodes = 401;   // 4*100 + 1: on the Richardson 4m+1 lattice
  cfg.wing_clamp_k = -1.0; // clamp off: the only kink is k = 0
  return cfg;
}

SviSurface make_interior_hole_surface(double a, double m, double sigma, double T) {
  SviSurface surf(1);
  const SviSlice slice{a, 1.0, 0.0, m, sigma, T};
  EXPECT_TRUE(surf.set_slice(0, slice).has_value());
  return surf;
}

TEST(Strip, InteriorNaNFlagged) {
  const double T_test = 0.5;
  // Half-width of the negative-w band around m = 0.5:
  // sqrt(0.0035^2 - 0.001^2) ~= 0.00335, comfortably under the 0.005 node
  // spacing -- only the node AT m itself falls inside; its neighbors one
  // node either side (offset +-0.005) stay positive.
  const SviSurface surf = make_interior_hole_surface(-0.0035, 0.5, 0.001, T_test);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  const auto q = var_swap_fair_strike(surf, cs, T_test, pinned_symmetric_strip_cfg());
  ASSERT_TRUE(q.has_value());
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::InteriorBadNodes));
}

TEST(Strip, InteriorNaNExceedsThreshold_ReturnsInternal) {
  const double T_test = 0.5;
  // Half-width sqrt(0.06^2 - 0.001^2) ~= 0.05999 -> the band covers offsets
  // out to +-0.055 at the 0.005 node spacing, 23 nodes (~5.7% of the
  // 401-node grid) -- comfortably past max(2, 401/100) == 4.
  const SviSurface surf = make_interior_hole_surface(-0.06, 0.5, 0.001, T_test);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  const auto q = var_swap_fair_strike(surf, cs, T_test, pinned_symmetric_strip_cfg());
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::Internal);
}

// ── Fix round 1: interior-bad-node accounting must not couple to the ATM
//    read (review Critical finding) ────────────────────────────────────────
//
// `sigma_atm = surface.iv(0.0, T)` reads the IDENTICAL (k_log=0.0, T) point
// the strip's own forced k = 0 panel-boundary kink node reads inside the
// loop -- k = 0 is always a distinct grid node whenever k_lo < 0 < k_hi
// (strip_grid.hpp's `strip_panel_bounds`), true of every symmetric-span
// call including `pinned_symmetric_strip_cfg` above. A gate built on
// `sigma_atm` alone therefore cannot tell "the surface is unusable
// everywhere" (the short-T extrapolation corner the earlier fix round was
// protecting) apart from "the ONE bad interior node happens to sit at ATM"
// -- exactly the case PV-4's finding names explicitly ("including the k = 0
// put-call-parity kink"). These two tests place the SVI vertex at m = 0
// instead of m = 0.5 (the earlier pair's placement, which structurally
// cannot reach k = 0) so ATM itself is the bad node while every other node,
// including both true grid endpoints, stays clean.

TEST(Strip, InteriorNaNAtAtmFlagged) {
  const double T_test = 0.5;
  // Same half-width margin as InteriorNaNFlagged (~0.00335 < the 0.005 node
  // spacing), just recentered on m = 0 so the bad node IS k = 0 itself --
  // the same point sigma_atm reads.
  const SviSurface surf = make_interior_hole_surface(-0.0035, 0.0, 0.001, T_test);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  const auto q = var_swap_fair_strike(surf, cs, T_test, pinned_symmetric_strip_cfg());
  ASSERT_TRUE(q.has_value());
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::InteriorBadNodes));
}

TEST(Strip, InteriorNaNAtAtmExceedsThreshold_ReturnsInternal) {
  const double T_test = 0.5;
  // Same half-width margin as InteriorNaNExceedsThreshold_ReturnsInternal
  // (~0.05999, 23 bad nodes), recentered on m = 0.
  const SviSurface surf = make_interior_hole_surface(-0.06, 0.0, 0.001, T_test);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  const auto q = var_swap_fair_strike(surf, cs, T_test, pinned_symmetric_strip_cfg());
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::Internal);
}

// ── MUST-FIX 7 (C-4 deferred (a)) ──────────────────────────────────────────
//
// The gate is `interior_bad_count > max(2, n/100)`; at the pinned 401-node
// grid, threshold == max(2, 401/100) == 4 exactly. Every existing fixture
// (23 bad nodes) clears the gate by a wide margin and cannot tell `>` from
// `>=` apart -- this pair pins the exact boundary, count == 4 (must stay Ok)
// vs count == 5 (must return Internal).
//
// A dip centered ON a grid node (m a multiple of dx = 0.005, as the fixtures
// above all use) only ever covers an ODD node count (symmetric +-K nodes
// around the center, 2K+1) -- there is no way to land on the EVEN count 4
// that way. Centering the dip exactly BETWEEN two adjacent grid nodes
// (m = 0.5025, halfway between i=300 at k=0.5 and i=301 at k=0.505) makes the
// coverage symmetric around the GAP instead, which only ever covers EVEN
// counts: h in (0.0025, 0.0075) bad nodes are {300,301} (2), h in
// (0.0075, 0.0125) bad nodes are {299,300,301,302} (4). h = 0.01 sits
// centered in the second interval.
TEST(Strip, InteriorNaNAtExactThreshold_StaysOk) {
  const double T_test = 0.5;
  // h = sqrt(a^2 - sigma^2) = 0.01 exactly, with sigma = 0.001:
  // a = -sqrt(0.01^2 + 0.001^2) = -sqrt(0.000101).
  const double sigma_svi = 0.001;
  const double h = 0.01;
  const double a = -std::sqrt(h * h + sigma_svi * sigma_svi);
  const SviSurface surf = make_interior_hole_surface(a, 0.5025, sigma_svi, T_test);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  const auto q = var_swap_fair_strike(surf, cs, T_test, pinned_symmetric_strip_cfg());
  ASSERT_TRUE(q.has_value()) << "count == threshold (4) must stay Ok, not Internal";
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::InteriorBadNodes));
}

// Same construction, dip centered ON a grid node (m = 0.5, the fixtures
// above's own placement) so coverage is the ODD count 2K+1: h in
// (0.01, 0.015) covers {298,...,302}, K = 2, count 5 -- one past threshold.
// h = 0.0125 sits centered in that interval.
TEST(Strip, InteriorNaNOneNodePastThreshold_ReturnsInternal) {
  const double T_test = 0.5;
  const double sigma_svi = 0.001;
  const double h = 0.0125;
  const double a = -std::sqrt(h * h + sigma_svi * sigma_svi);
  const SviSurface surf = make_interior_hole_surface(a, 0.5, sigma_svi, T_test);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  const auto q = var_swap_fair_strike(surf, cs, T_test, pinned_symmetric_strip_cfg());
  ASSERT_FALSE(q.has_value()) << "count == threshold+1 (5) must return Internal";
  EXPECT_EQ(q.error().code(), ErrorCode::Internal);
}

// ── Fix round 1: the shared panel-boundary node must count exactly once
//    (review Important finding) ─────────────────────────────────────────
//
// C-3's panel split reuses the previous panel's LAST node as the next
// panel's FIRST node (the `shared` variable in the loop) rather than
// re-reading `iv()` -- one read per DISTINCT node. A bad value at that
// shared boundary must be counted once, not twice (double-count: a clean
// surface could spuriously cross the Internal threshold).
//
// NOTE on the wing clamp's read semantics: `integrand_at` clamps the READ
// position, not just a validity check -- `x_read = clamp(x, -band, band)`.
// So EVERY node beyond `+wing_band` reads `iv()` AT `wing_band` itself, the
// SAME call the shared boundary node makes. Placing the bad vertex exactly
// at `+wing_band` therefore does not isolate a single bad read: it also
// poisons every node in the panel beyond the band (they clamp to the same
// bad point), including the grid's own true right endpoint -- collateral,
// not a counting bug, and accounted for below rather than avoided.
//
// Grid: symmetric [-1, 1], wing_clamp_k = 0.9 (band close to k_max = 1, so
// the panel beyond it is as narrow as the apportionment allows), 15 nodes.
// intervals = 14, 14 % 4 == 2 -> unit = 2 immediately (skips the 4-unit
// path), 4 panels at {-1,-0.9}, {-0.9,0}, {0,0.9}, {0.9,1}
// (`strip_panel_bounds`/`plan_strip_split`). intervals/unit = 7, spare =
// 7 - 4 = 3; apportion_units gives share [1, 3, 2, 1] (len-proportional,
// remainder to the longer/lower-index panel), i.e. panel_intervals
// [2, 6, 4, 2] -- the two length-0.1 end panels get the apportionment
// floor, 2 intervals (3 nodes) each. threshold = max(2, 15/100) == 2.
//
// The SVI vertex sits at m = +0.9, the shared node between panel [0,0.9]'s
// last node (read once, at the end of that panel's loop) and panel
// [0.9,1]'s first node (reused via `shared`, not reread). Half-width
// h = sqrt(0.01^2 - 0.001^2) ~= 0.00995, comfortably under panel [0,0.9]'s
// own spacing (0.9/4 = 0.225), so from the UNCLAMPED left side only the
// vertex node itself is bad -- its neighbor at k = 0.675 stays clean.
// From the clamped right side, panel [0.9,1] has exactly one more node
// beyond the shared one at its OWN spacing (0.1/2 = 0.05): the midpoint
// k = 0.95, which clamps to `wing_band` = 0.9 same as the shared node does,
// so it reads the SAME bad value -- one more genuine, distinct bad read
// (collateral of the clamp, not of the split). The panel's last node,
// k = 1.0, is the grid's own true endpoint (`is_grid_last`) and is excluded
// from interior accounting by construction, contributing to `bad_last`
// instead (expected, not asserted against here).
//
// Correct interior count = 2 (shared node + the k = 0.95 collateral read),
// exactly the threshold: Ok, flagged. If the shared node were counted
// TWICE (the bug this test targets), the observed count would be 3 > 2 and
// this would return Internal instead, catching it.
TEST(Strip, InteriorNaNAtSharedWingBandNodeCountsOnce) {
  const double T_test = 0.5;
  const double wing_band = 0.9;
  const double sigma = 0.001;
  const double a = -0.01; // h = sqrt(0.01^2 - 0.001^2) ~= 0.00995

  const SviSurface surf = make_interior_hole_surface(a, wing_band, sigma, T_test);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivConfig cfg = deriv_default_config();
  cfg.k_min_log = -1.0;
  cfg.k_max_log = 1.0;
  cfg.strip_nodes = 15;
  cfg.wing_clamp_k = wing_band;

  const auto q = var_swap_fair_strike(surf, cs, T_test, cfg);
  ASSERT_TRUE(q.has_value());
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::InteriorBadNodes));
}

// ── Aggregate review fix (C-R Critical #1) ─────────────────────────────────
//
// C-10's `carry_theta` (default true) resolves a fresh `var_swap_fair_strike`
// inside `eval_bump_table` to price the injected fixing at K_var_future. C-4
// gave that SAME strip routine a hard `Internal` failure past its
// interior-bad-node threshold (see `InteriorNaNExceedsThreshold_ReturnsInternal`
// above), and `price_vol_swap`'s own unaged branch treats this exact call as
// best-effort (":635-637", never fails the price over it). Pre-fix,
// `eval_bump_table` wrapped the call in a plain `ATX_TRY` and lost the WHOLE
// `Result<DerivGreeks>` on a fixture like this one, under the DEFAULT config
// (`carry_theta` true, `engine` Auto) -- a regression from "complete greek
// block, diagnostic simply absent" to "no greeks at all" on an unaged VolSwap.
//
// Reuses the IDENTICAL holey-surface fixture as
// `InteriorNaNExceedsThreshold_ReturnsInternal` (23 bad nodes, past
// max(2, 401/100) == 4) so the strip failure exercised here is the real C-4
// gate, not a simulated one. Naive Carr-Lee (the default `CarrLeeForm`) never
// touches the strip for the CENTER price (`carr_lee_k_vol` is a closed-form
// ATMF straddle read, see `vol_swap_fair_strike`'s Naive branch) -- only the
// carry-theta diagnostic call and the best-effort convexity diagnostic in
// `price_vol_swap` see the failure, so this fixture isolates the fix from any
// change to the center's own fail-loud contract (C-4's stays untouched).
TEST(CarryTheta, UnagedVolSwapHoleySurfaceKeepsBlockAliveWithCarryFieldsNaN) {
  const double T_test = 0.5;
  const SviSurface surf = make_interior_hole_surface(-0.06, 0.5, 0.001, T_test);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T_test;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;  // unaged: n_obs_done left at 0

  // pinned_symmetric_strip_cfg(): the same exact grid the C-4 fixtures pin,
  // so the carry-theta strip call hits the identical 23-bad-node failure the
  // center's diagnostic strip (best-effort, already tolerant) also hits.
  const auto g = atx::vol::deriv_greeks(surf, cs, c, pinned_symmetric_strip_cfg());
  // default bumps: carry_theta == true
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  EXPECT_TRUE(std::isnan(g->theta_carry));
  EXPECT_TRUE(std::isnan(g->theta_zero_fixing));
  EXPECT_TRUE(std::isfinite(g->pv));
  EXPECT_TRUE(std::isfinite(g->delta));
  EXPECT_TRUE(std::isfinite(g->gamma));
  EXPECT_TRUE(std::isfinite(g->vega));
  EXPECT_TRUE(std::isfinite(g->volga));
  EXPECT_TRUE(std::isfinite(g->vanna));
  EXPECT_TRUE(std::isfinite(g->theta));
  EXPECT_TRUE(std::isfinite(g->rho));
  EXPECT_TRUE(std::isfinite(g->charm));
}

// ── MUST-FIX 6 (C-5 M-5) ────────────────────────────────────────────────────
//
// `vol_swap_fair_strike`'s header doc (above its declaration) and the
// CHANGELOG both promise that CarrLeeForm::Refined "propagates that strip's
// error contract too (Internal on an unusably holey surface, etc.)" -- but no
// test exercised that promise before this fix, even though it is exactly the
// propagation the aggregate review's Critical finding (C-1, above) turns on:
// Refined's center price DEPENDS on the strip succeeding, unlike Naive or the
// best-effort diagnostic. Reuses the same C-4 interior-bad-node fixture as
// `InteriorNaNExceedsThreshold_ReturnsInternal` (23 bad nodes, past
// max(2, 401/100) == 4) so the failure pinned here is the real gate.
TEST(CarrLee, RefinedPropagatesStripFailure) {
  const double T_test = 0.5;
  const SviSurface surf = make_interior_hole_surface(-0.06, 0.5, 0.001, T_test);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivConfig cfg = pinned_symmetric_strip_cfg();
  cfg.carr_lee_form = atx::vol::CarrLeeForm::Refined;

  const auto q = vol_swap_fair_strike(surf, cs, T_test, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::Internal);
}

// ── Gamma swap (Task F-2, PV-F1 / LIT-7) ────────────────────────────────────
//
// Lee's weighted-variance strip (w(y) = y/Y0): DerivKind::GammaSwap shares the
// SAME grid/span/wing-clamp/kink resolution as VarSwap's own strip
// (`strip_fair_value_core`, derivatives.cpp) but its per-node integrand is the
// raw undiscounted OTM price (the 1/K weight cancels against the log-strike
// Jacobian) and its outer scale is 2/(T*S0) rather than 2/T.
//
// Three mandated oracles, in the brief's own order:
//   FlatZeroCarryExact — flat sigma, r=q=0: K_gamma == sigma^2 exactly
//     (E[S_t/S0] == 1 under zero carry, so Y0 cancels the whole weight).
//   SkewOrdering — negative skew: K_gamma < K_var (Lee's price-weighting
//     downweights the rich put wing relative to VarSwap's own 1/K weight).
//   MCOracle — Task-0 MC harness extended with the S_i/S0 weight, BS with
//     drift, 3-SE agreement.
// Plus supporting tests. `CarryApproximationClosedForm` re-derives, from
// scratch (Carr-Madan spanning, h(K) = K*ln(K) - K so h''(K) = 1/K matches
// the strip's own price/df integrand exactly), the closed form the flat-
// surface strip evaluates to under NONZERO carry, and uses it to MEASURE the
// O((r-q)*T) approximation the header doc promises a number for.

TEST(GammaSwap, FlatZeroCarryExact) {
  const double spot = 100.0;  // != 1: see this test's own note below
  const double sigma = 0.20;
  const double T_test = 0.10;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(spot, 0.01, 1.00);  // zero rate: r == q == 0

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::High;  // tighter quadrature; see the tolerance note below

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = T_test;
  c.notional = 1.0;
  c.strike_dec = 0.0;
  // rv_spec left at n_obs_total == 0: unaged, pure future leg.

  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q.has_value()) << q.error().to_string();

  // THE assertion that carries this oracle. An UNIMPLEMENTED GammaSwap
  // dispatch fails ASSERT_TRUE above outright (the pre-F-2 `deriv_price`
  // switch has no GammaSwap case and returns InvalidArgument). A wrong OUTER
  // SCALE (a missing /S0, or a leftover VarSwap 2/T) misses by a factor of
  // `spot` == 100 here, not by quadrature noise -- `spot` is deliberately
  // != 1 so that whole bug class cannot hide behind a coincidental S0 == 1.
  // What this test does NOT, by itself, discriminate: a strip that computed
  // K_var instead of K_gamma (i.e. reused VarSwap's own 1/K-weighted
  // integrand AND its 2/T scale) would ALSO land on sigma^2 here, because
  // K_var is itself carry-independent and equals sigma^2 under this exact
  // fixture -- the two formulas coincide at zero carry on a flat surface by
  // construction (see the file derivation this test's own numbers were
  // checked against). `SkewOrdering` below is what specifically rules out
  // that failure mode.
  const double k_gamma_expected = sigma * sigma;
  EXPECT_NEAR(q->fair_strike_dec, k_gamma_expected, 1.0e-6 * k_gamma_expected);
  EXPECT_GT(q->fair_strike_dec, 0.0);
}

TEST(GammaSwap, SkewOrdering) {
  using atx::vol::deriv_testkit::kSkewRefT;
  using atx::vol::deriv_testkit::make_curves;
  using atx::vol::deriv_testkit::make_skew_surface;

  const double atm_vol = 0.20;
  const double skew_slope = -0.60;  // steep negative skew (rho fixed at -0.7)
  const double convexity = 0.5;
  const EssviSurface surf = make_skew_surface(atm_vol, skew_slope, convexity);
  const CurveSet cs = make_curves(100.0, 0.0, 0.0);  // zero carry: isolate the skew mechanism

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::High;

  const auto q_var = var_swap_fair_strike(surf, cs, kSkewRefT, cfg);
  ASSERT_TRUE(q_var.has_value());

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = kSkewRefT;
  c.notional = 1.0;
  c.strike_dec = 0.0;
  const auto q_gamma = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q_gamma.has_value());

  // THE assertion that carries this oracle: a STRICT inequality against the
  // production VarSwap value, same skewed surface, same T, same cfg. A
  // GammaSwap dispatch that -- by a kind-routing bug, a reverted price_node,
  // or an accidental alias -- actually computed K_var would make the two
  // EQUAL, failing this EXPECT_LT outright (not a tolerance question, a
  // strict-ordering one). This is the discriminator FlatZeroCarryExact's own
  // comment above says it cannot be: at zero carry on a FLAT surface the two
  // formulas coincide by construction, but under skew they provably diverge
  // (VarSwap's extra 1/K weight amplifies the rich, low-strike put wing under
  // negative skew more than GammaSwap's flat-in-K weighting does).
  EXPECT_LT(q_gamma->fair_strike_dec, q_var->fair_strike_dec);
  EXPECT_GT(q_gamma->fair_strike_dec, 0.0);
}

// I-2 (Task F-2 fix round 1, review .../task-F-2-review.md): this oracle was
// ORIGINALLY presented as a discriminator against VarSwap. It is not one --
// under reversion (GammaSwap silently computing K_var instead of K_gamma) it
// PASSES with a BETTER margin than production does (review's 8-point drift
// scan: 14.626x vs 1.840x here), because at GBM/flat-vol precision the
// gamma-vs-variance discrimination signal and the single-expiry
// approximation bias this oracle must tolerate are the SAME order,
// O((r-q)*T)*sigma^2 -- "passes" and "discriminates" cannot both hold at any
// feasible path count (this is the same shape as this sprint's earlier
// `>=2x`-and-`<=1e-11` ruling). The real discriminators are `SkewOrdering`
// above (fails at exactly K_gamma - K_var == 0 under reversion) and
// `CarryApproximationClosedForm` below (an independent closed-form
// re-derivation). This test is kept as a CALIBRATION check only: it still
// catches a wrong outer scale or broken quadrature against an independent
// seeded-MC path simulation, which is worth having, but its passing is not
// evidence GammaSwap and VarSwap were computed differently -- the strict
// EXPECT_LT below (a same-fixture VarSwap comparison) is what actually
// checks that, in this test.
TEST(GammaSwap, MCOracle) {
  using atx::vol::deriv_testkit::make_curves;
  using atx::vol::deriv_testkit::make_flat_surface;
  using atx::vol::deriv_testkit::mc_gamma_realized_variance;
  using atx::vol::deriv_testkit::McModelParams;

  const double sigma = 0.20;
  const double r = 0.021;
  const double q = 0.020;  // r-q = 0.001: measured (not the originally
                           // reported, non-reproducing digits) at
                           // |diff| = 1.7770e-05 against a 3*mc.stderr_rv
                           // band of 3.26931e-05 (~1.840x margin) -- see this
                           // test's own header comment above for why that
                           // margin is a calibration bound, not evidence of
                           // discrimination. A real dispatch/weighting bug
                           // (O(sigma^2) ~ 0.04 scale) still misses by several
                           // hundred multiples of that same band.
                           //
                           // An earlier r-q=0.005 attempt at this fixture was
                           // REJECTED: reviewer independently measured
                           // 5.7352e-05 against a 3.2748e-05 band at that
                           // config (this file's prior comment's
                           // 6.4225710546005066e-05 / 2.790575118117747e-05
                           // did not reproduce -- same verdict, i.e. still a
                           // fail, just different digits; corrected here
                           // rather than left standing).
  const double T = 0.5;   // 6M fixture pillar
  const double spot = 100.0;

  const EssviSurface surf = make_flat_surface(sigma);
  const CurveSet cs = make_curves(spot, r, q);

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::High;

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = T;
  c.notional = 1.0;
  c.strike_dec = 0.0;
  const auto q_gamma = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q_gamma.has_value()) << q_gamma.error().to_string();

  // I-2: a same-fixture VarSwap comparison, strict inequality -- THIS is what
  // actually rules out "GammaSwap silently computed K_var", the failure mode
  // the review found this oracle's 3-SE band cannot discriminate. Under
  // reversion (aliased to K_var) this collapses to K_gamma == K_var, failing
  // outright. Direction here is GT, not SkewOrdering's LT: on a FLAT surface
  // (this fixture, no skew) the ordering is set by carry sign, not skew --
  // K_var is carry-independent (sigma^2 exactly, the log-contract's "delta
  // term vanishes" identity) while K_gamma ~= sigma^2*e^{(r-q)T}
  // (CarryApproximationClosedForm below), and this fixture's r=0.021 >
  // q=0.020 makes that strictly > sigma^2 = K_var.
  const auto q_var = var_swap_fair_strike(surf, cs, T, cfg);
  ASSERT_TRUE(q_var.has_value());
  EXPECT_GT(q_gamma->fair_strike_dec, q_var->fair_strike_dec);

  const McModelParams p{spot, r, q, sigma, T};
  const auto mc = mc_gamma_realized_variance(p, 200000, 252u, 11);
  ASSERT_GT(mc.stderr_rv, 0.0);

  // Calibration assertion (NOT a discriminator -- see this test's header
  // comment): production K_gamma (the real dispatch, `deriv_price` ->
  // `price_gamma_swap` -> `strip_fair_value_core`, not a re-implementation)
  // against an INDEPENDENT seeded-MC simulation extended with the S_i/S0
  // weight (deriv_fixtures.hpp, never calls into derivatives.cpp). A wrong
  // outer scale or broken quadrature still misses by O(sigma^2) ~ 0.04,
  // dwarfing 3*mc.stderr_rv.
  EXPECT_NEAR(q_gamma->fair_strike_dec, mc.mean_rv, 3.0 * mc.stderr_rv)
      << "fair_strike=" << q_gamma->fair_strike_dec << " mc_mean=" << mc.mean_rv
      << " mc_stderr=" << mc.stderr_rv;
}

// Supporting test: re-derives, from scratch, the closed form
// `strip_fair_value_core` evaluates to for a FLAT surface under nonzero
// carry (Carr-Madan spanning against h(K) = K*ln(K) - K, h''(K) = 1/K, the
// SAME weight the strip's price/df integrand carries for GammaSwap) --
// K_gamma_shipped = sigma^2 * F/S0 = sigma^2 * e^{(r-q)*T} -- and measures
// the gap against the TRUE continuous-monitoring expectation of the
// gamma-weighted realized variance under the same flat-BS model,
// E[RV_gamma] = sigma^2 * (e^{(r-q)*T} - 1) / ((r-q)*T) (a SEPARATE
// derivation: (sigma^2/T) * integral_0^T E[S_t/S0] dt with E[S_t/S0] =
// e^{(r-q)*t}).
TEST(GammaSwap, CarryApproximationClosedForm) {
  const double spot = 100.0;
  const double sigma = 0.20;
  const double r = 0.05;
  const double q = 0.00;
  const double T_test = 0.5;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = atx::vol::deriv_testkit::make_curves(spot, r, q);

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::High;

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = T_test;
  c.notional = 1.0;
  c.strike_dec = 0.0;
  const auto q_gamma = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q_gamma.has_value());

  const double carry = (r - q) * T_test;
  const double k_gamma_shipped = sigma * sigma * std::exp(carry);
  // THE assertion that carries the closed-form half of this test: production
  // matches an INDEPENDENTLY re-derived formula (not copied from
  // derivatives.cpp) to near machine precision -- the strip's own quadrature
  // error at High quality on a perfectly smooth flat-vol integrand.
  EXPECT_NEAR(q_gamma->fair_strike_dec, k_gamma_shipped, 1.0e-6 * k_gamma_shipped);

  // The exactness-caveat measurement (report deliverable): the shipped
  // single-expiry strike vs. the TRUE continuous-monitoring expectation.
  // expm1/carry is the numerically stable form of (e^carry - 1)/carry.
  const double k_gamma_true = sigma * sigma * std::expm1(carry) / carry;
  const double approx_gap = k_gamma_shipped - k_gamma_true;
  const double leading_order = sigma * sigma * carry / 2.0;
  // Within 15% of the leading-order term: loose enough to allow the genuine
  // O(carry^2) next term (measured ~1.8% away at this fixture's carry, well
  // inside this bound), tight enough that a wrong-order or sign-flipped
  // approximation would fail it.
  EXPECT_NEAR(approx_gap, leading_order, 0.15 * std::fabs(leading_order));
  EXPECT_GT(approx_gap, 0.0);  // shipped strike OVERSTATES the true expectation here
}

// P-4 audit: `DerivGreekMethod::AnalyticStrip` on a GammaSwap contract must
// fall back to FiniteDifference SILENTLY (bit-identical output), never
// differentiate VarSwap's closed form against a GammaSwap center. Carries the
// audit by BIT-IDENTITY, not a tolerance: `deriv_greeks`'s own
// `analytic_in_scope` is `bumps.method == AnalyticStrip && contract.kind ==
// DerivKind::VarSwap && analytic_scope_from_cfg(cfg)` -- a WHITELIST, so
// GammaSwap can never satisfy it regardless of any other field. A regression
// that widened the whitelist (e.g. testing `kind != DerivKind::VolSwap`
// instead of `kind == DerivKind::VarSwap`) would route GammaSwap through the
// VarSwap closed form and break this identity.
TEST(GammaSwap, AnalyticStripMethodFallsBackToFiniteDifference) {
  const double sigma = 0.20;
  const double T_test = 0.25;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = T_test;
  c.notional = 1.0;
  c.strike_dec = sigma * sigma;

  const DerivConfig cfg = deriv_default_config();

  atx::vol::DerivGreekBumps bumps_fd{};
  bumps_fd.method = atx::vol::DerivGreekMethod::FiniteDifference;
  const auto g_fd = atx::vol::deriv_greeks(surf, cs, c, cfg, bumps_fd);
  ASSERT_TRUE(g_fd.has_value());

  atx::vol::DerivGreekBumps bumps_an{};
  bumps_an.method = atx::vol::DerivGreekMethod::AnalyticStrip;
  const auto g_an = atx::vol::deriv_greeks(surf, cs, c, cfg, bumps_an);
  ASSERT_TRUE(g_an.has_value());

  EXPECT_DOUBLE_EQ(g_fd->delta, g_an->delta);
  EXPECT_DOUBLE_EQ(g_fd->gamma, g_an->gamma);
  EXPECT_DOUBLE_EQ(g_fd->vega, g_an->vega);
  EXPECT_DOUBLE_EQ(g_fd->volga, g_an->volga);
  EXPECT_DOUBLE_EQ(g_fd->vanna, g_an->vanna);
  EXPECT_DOUBLE_EQ(g_fd->pv, g_an->pv);
}

// Scope decision (see price_gamma_swap's own comment, derivatives.cpp):
// Diffusion1OverN has no derivation for the gamma-weighted estimator, so it
// is REJECTED, not silently ignored or silently misapplied.
TEST(GammaSwap, DiffusionOneOverNRejected) {
  const double sigma = 0.20;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = atx::vol::deriv_testkit::make_curves(100.0, 0.06, 0.01);

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = 1.0;
  c.notional = 1.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 252u;

  DerivConfig cfg = deriv_default_config();
  cfg.discrete_correction_mode = DerivDiscreteCorrection::Diffusion1OverN;

  const auto q = deriv_price(surf, cs, c, cfg);
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::NotImplemented);
}

// price_gamma_swap must blend `rv.rv_gamma_done_dec` (the S_i/S0-weighted
// accrual), never `rv.rv_done_dec` (the plain one) -- the two coexist on the
// SAME RealizedVarianceSpec since Task F-2 appended the gamma field beside
// the pre-existing plain one, so a field mix-up is a real, reachable defect
// class, not a hypothetical one.
TEST(GammaSwap, AgedBlendReadsGammaWeightedAccrualNotPlain) {
  const double sigma = 0.20;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0;
  c.strike_dec = 0.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 100u;
  c.rv_spec.n_obs_done = 40u;
  c.rv_spec.rv_gamma_done_dec = 0.09;  // heavy accrued leg
  c.rv_spec.rv_done_dec = 0.01;        // deliberately different; must NOT be read
  // C-1 fix round 1: the mid-life blend now requires a seed-spot anchor to
  // rescale the future leg onto (see AgedBlendRescalesFutureLegOntoAccrual
  // Anchor below for the rescale itself). Anchored at the SAME spot `cs`
  // uses (100.0) so the rescale factor is exactly 1.0 and this test's
  // original point -- which FIELD gets read, not the anchor mechanics --
  // stays isolated and its pre-existing assertions stay valid unchanged.
  c.rv_spec.gamma_seed_spot = 100.0;

  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value());

  const double w_done = 40.0 / 100.0;
  const double w_future = 60.0 / 100.0;
  const double k_gamma_future = q->future_component_dec / w_future;
  const double expected_total = w_done * 0.09 + w_future * k_gamma_future;
  // THE assertion that carries this test: reading rv_done_dec (0.01) instead
  // of rv_gamma_done_dec (0.09) would move fair_strike_dec by
  // w_done*(0.09-0.01) == 0.032 -- far past this tolerance.
  EXPECT_NEAR(q->fair_strike_dec, expected_total, 1.0e-10);
  EXPECT_NEAR(q->accrued_component_dec, w_done * 0.09, 1.0e-12);
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::Aged));
}

// C-1 Critical (Task F-2 fix round 1, review .../task-F-2-review.md): the
// aged blend above combines `rv.rv_gamma_done_dec` -- anchored at the
// tracker's SEED spot -- with a future leg the strip anchors at TODAY's spot
// (`curves.spot`). A hand-built mid-life spec (0 < n_obs_done < n_obs_total)
// that never populates `gamma_seed_spot` has no way to supply that anchor,
// so `price_gamma_swap` must FAIL LOUD rather than silently blend two
// mismatched anchors (the exact defect this fix round closes -- see
// AgedBlendRescalesFutureLegOntoAccrualAnchor below for the correctly-
// anchored case, and its measured before/after gap).
TEST(GammaSwap, AgedBlendFailsLoudWithoutSeedSpotAnchor) {
  const double sigma = 0.20;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(120.0, 0.01, 1.00);  // spot moved since inception

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0e6;
  c.strike_dec = 0.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 100u;
  c.rv_spec.n_obs_done = 40u;
  c.rv_spec.rv_gamma_done_dec = 0.09;
  // c.rv_spec.gamma_seed_spot left at its 0.0 default: no anchor recorded.

  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  // THE assertion that carries this test: before the fix this silently
  // priced a wrong number (fair_strike_dec still populated, no flag, no
  // error) -- the exact "silent" outcome the review called the one thing
  // this file otherwise refuses to ship. After the fix it is a loud,
  // explicit InvalidArgument.
  ASSERT_FALSE(q.has_value());
  EXPECT_EQ(q.error().code(), ErrorCode::InvalidArgument);
}

// C-1 Critical (Task F-2 fix round 1): reproduces the reviewer's fixture
// shape -- a REAL RealizedTracker seeded at spot 100, walked to spot 120
// over 40 of 100 scheduled daily fixings, sigma=20%, ZERO carry (r == q),
// T=0.5, notional=1e6 -- through the actual accrual path (RealizedTracker::
// observe), not a hand-set rv_gamma_done_dec, so the anchor this bug needs is
// populated exactly the way production code populates it. The review's own
// walk was not specified beyond its endpoints/count (only "walked to 120
// over 40/100 fixings"); this test uses an explicit linear-in-price walk and
// checks the result against an INDEPENDENTLY reconstructed expected value --
// not a golden number copied from the review -- the same "closed-form, not
// copied" standard `CarryApproximationClosedForm` above already holds this
// file to. See this test's own two assertions below for what pins it, and
// this file's own disclosure in the fix-round report for how closely the
// measured gap tracks the review's reported 15.43% / $4,800 figure despite
// the differing path.
TEST(GammaSwap, AgedBlendRescalesFutureLegOntoAccrualAnchor) {
  const double sigma = 0.20;
  const double T = 0.5;
  const double notional = 1.0e6;
  const std::uint32_t n_total = 100u;
  const std::uint32_t n_done = 40u;
  const double seed_spot = 100.0;
  const double end_spot = 120.0;

  auto tracker = RealizedTracker::create(252.0, n_total);
  ASSERT_TRUE(tracker.has_value());
  ASSERT_TRUE(tracker->observe(seed_spot).has_value());  // seeds gamma_seed_spot
  for (std::uint32_t i = 1; i <= n_done; ++i) {
    const double s = seed_spot + (end_spot - seed_spot) * (static_cast<double>(i) / n_done);
    ASSERT_TRUE(tracker->observe(s).has_value());
  }
  const RealizedVarianceSpec accrued = tracker->snapshot();
  ASSERT_EQ(accrued.n_obs_done, n_done);
  ASSERT_DOUBLE_EQ(accrued.gamma_seed_spot, seed_spot);

  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  // m-7 (fix round 2): `make_flat_curves`'s 2nd/3rd args are yield-curve
  // TENOR PILLARS (T_lo, T_hi -- see this file's own local helper at
  // :106-119), not a rate; it hard-codes r == 0.0 always. Zero carry here
  // because that hard-coding, not because these args encode a rate.
  const CurveSet cs = make_flat_curves(end_spot, 0.01, 1.00);  // zero carry (r == q == 0, always)

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = T;
  c.notional = notional;
  c.strike_dec = 0.0;
  c.rv_spec = accrued;

  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value()) << q.error().to_string();

  // Independently reconstruct the future leg by calling the production strip
  // directly (not re-implementing it), then build BOTH the buggy (unrescaled)
  // and correct (rescaled) blends from first principles here in the test.
  const auto strip = var_swap_fair_strike(surf, cs, T, deriv_default_config());
  ASSERT_TRUE(strip.has_value());
  const double k_gamma_future = strip->fair_strike_dec;  // == K_gamma at zero carry, flat
                                                          // surface (FlatZeroCarryExact).

  const double w_done = static_cast<double>(n_done) / n_total;
  const double w_future = 1.0 - w_done;
  const double shipped_buggy = w_done * accrued.rv_gamma_done_dec + w_future * k_gamma_future;
  const double correct = w_done * accrued.rv_gamma_done_dec +
                         w_future * (end_spot / seed_spot) * k_gamma_future;

  // THE assertion that carries this test: production matches the correctly
  // ANCHOR-RESCALED blend, not the unrescaled one a reversion of the fix
  // would ship. `shipped_buggy` is the exact quantity price_gamma_swap
  // computed before this fix round; asserting q is FAR from it (not just
  // close to `correct`) is what makes this test fail under reversion, not
  // merely under some other unrelated regression.
  EXPECT_NEAR(q->fair_strike_dec, correct, 1.0e-9);
  const double gap = correct - shipped_buggy;
  EXPECT_GT(std::fabs(q->fair_strike_dec - shipped_buggy), 0.5 * std::fabs(gap));

  // Round-2 fix (V-8, task-F-2-fix-round-1-review.md): the two magnitude
  // assertions this comment used to make (`EXPECT_NEAR(gap, 0.0048, ...)`,
  // `EXPECT_GT(gap / correct, 0.10)`) were VACUOUS -- `gap` and `correct` are
  // both computed here from test-local quantities, never from `q`, so they
  // passed unchanged under every reversion. Rewritten to read PRODUCTION's
  // own deviation from the buggy value instead of the test's own local
  // arithmetic; both now fail if `q->fair_strike_dec` collapses toward
  // `shipped_buggy`. The gap itself is (S_t/S_seed - 1) * w_future *
  // K_gamma_future -- independent of the accrued leg's value, so it depends
  // only on the fixture's endpoints/weights, not on this test's own
  // (undisclosed-in-the-review) path shape. Pins the review's reported
  // magnitude: 0.6 * 0.2 * ~0.04 ~= 0.0048 decimal on a ~1e6 notional ~=
  // $4,800, matching the review's own "$4,800 PV, 15.43% of the strike" to
  // the precision that magnitude claim needs.
  EXPECT_NEAR(q->fair_strike_dec - shipped_buggy, 0.0048, 2.0e-4);
  EXPECT_GT((q->fair_strike_dec - shipped_buggy) / q->fair_strike_dec, 0.10);
  // Sanity: the local `gap`/`correct` computation the test uses to derive
  // `correct` above agrees with what the two now-live assertions just
  // measured off production -- i.e. this test's own arithmetic and
  // production's answer are the SAME number, not coincidentally close.
  EXPECT_NEAR(gap, q->fair_strike_dec - shipped_buggy, 1.0e-9);
}

// C-3 Critical (Task F-2 fix round 2, review .../task-F-2-fix-round-1-review.md):
// round 1's `AgedBlendRescalesFutureLegOntoAccrualAnchor` above walks a
// tracker PAST n_obs_done == 0 before ever pricing it, so it -- like every
// other GammaSwap fixture in the tree at round 1 -- never exercised a live
// `gamma_seed_spot` at `n_obs_done == 0` (I-3's exact finding: the suite
// could not distinguish 24d0342 from a corrected build). This test prices
// EXACTLY that state: a real `RealizedTracker` seeded once and nothing more
// (`gamma_seed_spot` recorded, `n_obs_done` still 0 -- the state a
// tracker-driven contract occupies between inception and its first return
// fixing), at a `curves.spot` that has moved since the seed.
TEST(GammaSwap, AgedBlendRescalesUnaccruedAnchorAtZeroObservationsDone) {
  const double sigma = 0.20;
  const double T = 0.5;
  const double notional = 1.0e6;
  const std::uint32_t n_total = 100u;
  const double seed_spot = 100.0;
  const double today_spot = 120.0;  // spot has moved since inception; nothing accrued yet

  auto tracker = RealizedTracker::create(252.0, n_total);
  ASSERT_TRUE(tracker.has_value());
  ASSERT_TRUE(tracker->observe(seed_spot).has_value());  // seed only -- n_obs_done stays 0
  const RealizedVarianceSpec seeded = tracker->snapshot();
  ASSERT_EQ(seeded.n_obs_done, 0u);
  ASSERT_DOUBLE_EQ(seeded.gamma_seed_spot, seed_spot);  // C-3's exact "known and ignored" state

  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(today_spot, 0.01, 1.00);  // zero carry (r == q == 0, always)

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = T;
  c.notional = notional;
  c.strike_dec = 0.0;
  c.rv_spec = seeded;

  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value()) << q.error().to_string();

  // Independently reconstruct the future leg via the production strip
  // directly (zero carry, flat surface: K_gamma == K_var, FlatZeroCarryExact).
  const auto strip = var_swap_fair_strike(surf, cs, T, deriv_default_config());
  ASSERT_TRUE(strip.has_value());
  const double k_gamma_future = strip->fair_strike_dec;

  const double correct = (today_spot / seed_spot) * k_gamma_future;
  const double shipped_buggy = k_gamma_future;  // 24d0342's unrescaled value at n_obs_done == 0

  // THE assertion that carries this test: production matches the RESCALED
  // future leg. 24d0342 (round 1) returns `k_gamma_future_dec` UNRESCALED
  // whenever n_obs_done == 0, even with a live anchor recorded -- C-3.
  EXPECT_NEAR(q->fair_strike_dec, correct, 1.0e-9);
  EXPECT_GT(std::fabs(q->fair_strike_dec - shipped_buggy), 0.5 * std::fabs(correct - shipped_buggy));
  // Non-vacuity: the gap between correct and buggy is a real 20% of the
  // strike at this fixture (today_spot/seed_spot - 1 == 0.2), not a
  // rounding-order effect the tolerance above could satisfy by accident.
  EXPECT_GT(std::fabs(correct - shipped_buggy) / correct, 0.15);
}

// C-4 Critical (Task F-2 fix round 2): reproduces the review's own C-4
// fixture -- mid-life 40/100, rv_gamma_done_dec = 0.09, curves.spot = 100,
// zero rates (theta_carry's own documented meaning, "isolates the pure
// discounting drift", puts the truth near 0 here) -- with gamma_seed_spot =
// 80, spot having risen 25% since inception, an ORDINARY mid-life state, not
// an edge case. Round 1's `CarryThetaDiffersFromZeroFixingThetaMidLife`
// deliberately pinned `gamma_seed_spot == curves.spot` to isolate C-2 from
// C-1; that isolation choice is exactly what hid C-4 (I-3's finding).
TEST(GammaSwap, CarryThetaRescalesInjectedFixingOntoSeedAnchor) {
  const double sigma = 0.20;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);  // zero carry (r == q == 0, always)

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0e6;
  c.strike_dec = 0.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 100u;
  c.rv_spec.n_obs_done = 40u;
  c.rv_spec.rv_gamma_done_dec = 0.09;
  c.rv_spec.gamma_seed_spot = 80.0;  // spot has risen 25% since inception -- C-4's exact fixture

  const atx::vol::DerivGreekBumps bumps{};
  const auto g = atx::vol::deriv_greeks(surf, cs, c, deriv_default_config(), bumps);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  // THE assertion that carries this test: at zero rates, theta_carry's own
  // documented meaning puts the truth near 0. 24d0342 (rounds 1) added the
  // injected fixing (anchored at curves.spot == 100) directly into
  // rv_gamma_done_dec (anchored at gamma_seed_spot == 80) with no rescale,
  // reporting -36524.999922309376 (wrong sign, ~8.4e7x) instead of a number
  // near 0. Bound generously (1.0) since the true value is a small residual
  // (T-vs-T-dt strip/roll noise), not exactly 0.
  EXPECT_LT(std::fabs(g->theta_carry), 1.0);
  // The "must differ" contract still holds with a real (not no-op) anchor.
  EXPECT_NE(g->theta_carry, g->theta_zero_fixing);
}

// RealizedTracker's gamma accumulator (Task F-2, appended field + observe
// path), tested directly against an independent hand-recomputation of
// Sigma (S_i/S0)*r_i^2.
TEST(RealizedTrackerGamma, AccumulatesWeightedVariance) {
  auto t = RealizedTracker::create(252.0, 3);
  ASSERT_TRUE(t.has_value());

  const double s0 = 100.0;
  ASSERT_TRUE(t->observe(s0).has_value());  // seed: anchors S0 too

  const double s1 = 101.0;
  const double s2 = 99.0;
  const double s3 = 102.0;
  ASSERT_TRUE(t->observe(s1).has_value());
  ASSERT_TRUE(t->observe(s2).has_value());
  ASSERT_TRUE(t->observe(s3).has_value());

  const double r1 = std::log(s1 / s0);
  const double r2 = std::log(s2 / s1);
  const double r3 = std::log(s3 / s2);
  const double sum_weighted = (s1 / s0) * r1 * r1 + (s2 / s0) * r2 * r2 + (s3 / s0) * r3 * r3;
  const double expected_rv_gamma = 252.0 * sum_weighted / 3.0;
  const double sum_plain = r1 * r1 + r2 * r2 + r3 * r3;

  const RealizedVarianceSpec spec = t->snapshot();
  // THE assertion that carries this test: an independent, from-scratch
  // recomputation of Sigma (S_i/S0)*r_i^2 against the tracker's own
  // accumulator -- a tracker that never populated rv_gamma_done_dec (a
  // reverted/unimplemented gamma accumulator) would leave both new fields at
  // their struct default 0.0, failing this outright.
  EXPECT_NEAR(spec.sum_weighted_sq_log_returns_done, sum_weighted, 1.0e-15);
  EXPECT_NEAR(spec.rv_gamma_done_dec, expected_rv_gamma, 1.0e-13);
  // Non-regression: the pre-existing plain accumulator is untouched by this
  // task, same tracker, same call sequence.
  EXPECT_NEAR(spec.sum_sq_log_returns_done, sum_plain, 1.0e-15);
  EXPECT_NEAR(spec.rv_done_dec, 252.0 * sum_plain / 3.0, 1.0e-13);
}

// C-2 Critical (Task F-2 fix round 1, review .../task-F-2-review.md):
// `inject_carry_fixing` (derivatives.cpp) wrote only `rv_done_dec` /
// `sum_sq_log_returns_done` -- true up through Task F-2's own commit, since
// nothing read the gamma leg back yet -- but `price_gamma_swap` reads
// `rv_gamma_done_dec` for its accrued leg instead. Left unfixed, the carry
// and zero-fixing repricings below both silently fall back to the SAME
// (never-updated) `rv_gamma_done_dec`, making `theta_carry` and
// `theta_zero_fixing` bitwise IDENTICAL -- this file documents that pair as
// "must differ" -- and wrong by 305x/364x with a sign flip against a
// same-shape VarSwap control (review's own measurement). Mid-life regime
// (0 < n_obs_done < n_obs_total); see the next test for n_obs_done == 0.
TEST(GammaSwap, CarryThetaDiffersFromZeroFixingThetaMidLife) {
  const double sigma = 0.20;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  // m-7 (fix round 2): 0.01/1.00 are yield-curve tenor pillars (T_lo, T_hi),
  // not a rate -- see make_flat_curves's own local helper, :106-119, which
  // takes no rate argument and hard-codes r == 0.0 always.
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);  // zero carry (r == q == 0, always)

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0e6;
  c.strike_dec = 0.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 100u;
  c.rv_spec.n_obs_done = 40u;
  c.rv_spec.rv_gamma_done_dec = 0.09;
  c.rv_spec.gamma_seed_spot = 100.0;  // anchored at curves.spot: C-1 rescale is a no-op here,
                                       // isolating this test's point to C-2 alone.

  const atx::vol::DerivGreekBumps bumps{};  // defaults: carry_theta = true
  const auto g = atx::vol::deriv_greeks(surf, cs, c, deriv_default_config(), bumps);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  ASSERT_TRUE(std::isfinite(g->theta_carry));
  ASSERT_TRUE(std::isfinite(g->theta_zero_fixing));

  // Control: the IDENTICAL contract shape priced as a VarSwap (rv_done_dec in
  // place of rv_gamma_done_dec, same numeric value). Zero carry + flat
  // surface makes GammaSwap's and VarSwap's future legs coincide
  // (FlatZeroCarryExact's own point), so a correctly-anchored gamma carry-
  // theta pair should land close to this control, not off by two orders of
  // magnitude the way the review's own bug measurement was.
  DerivContract c_var = c;
  c_var.kind = DerivKind::VarSwap;
  c_var.rv_spec.rv_done_dec = c.rv_spec.rv_gamma_done_dec;
  const auto g_var = atx::vol::deriv_greeks(surf, cs, c_var, deriv_default_config(), bumps);
  ASSERT_TRUE(g_var.has_value()) << g_var.error().to_string();

  // THE assertion that carries this test: the "must differ" contract itself
  // -- a bitwise-equal pair is exactly what exposed C-2 (inject_carry_fixing
  // updating only the plain accumulator, never the gamma one price_gamma_swap
  // actually reads, so both injected repricings silently collapsed to the
  // same wrong number).
  EXPECT_NE(g->theta_carry, g->theta_zero_fixing);
  // Magnitude check: within an order of magnitude of the VarSwap control --
  // a bound the review's own 305x/364x-off bug would have failed outright.
  ASSERT_NE(g_var->theta_carry, 0.0);
  EXPECT_LT(std::fabs(g->theta_carry - g_var->theta_carry), 10.0 * std::fabs(g_var->theta_carry));
  ASSERT_NE(g_var->theta_zero_fixing, 0.0);
  EXPECT_LT(std::fabs(g->theta_zero_fixing - g_var->theta_zero_fixing),
            10.0 * std::fabs(g_var->theta_zero_fixing));
}

// C-2 Critical (Task F-2 fix round 1): the n_obs_done == 0 regime -- exactly
// the shape `solve_cycle_swap` produces (a freshly-struck, scheduled
// contract, never routed through RealizedTracker, so `rv_gamma_done_dec` and
// `gamma_seed_spot` sit at their struct defaults). Review measured
// theta_carry ~= -144,977 against a correct ~= +398 on this shape. C-1's
// auto-anchor (inject_carry_fixing establishes gamma_seed_spot at
// curves.spot exactly when this injection IS the contract's first-ever
// fixing -- see that function's own comment) is what keeps this FINITE
// rather than erroring: a never-accrued contract's inception and "now"
// genuinely coincide.
TEST(GammaSwap, CarryThetaFiniteAndDiffersAtZeroObservationsDone) {
  const double sigma = 0.20;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0e6;
  c.strike_dec = 0.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 100u;
  // n_obs_done / rv_gamma_done_dec / gamma_seed_spot left at their struct
  // defaults (0u / 0.0 / 0.0) -- the solve_cycle_swap shape described above.

  const atx::vol::DerivGreekBumps bumps{};
  const auto g = atx::vol::deriv_greeks(surf, cs, c, deriv_default_config(), bumps);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  // THE assertion that carries this test: finiteness. Isolating C-2's own
  // defect (C-1's guard already in place) turns this from "wrong number"
  // into "NaN" -- a mid-life reprice with no anchor -- since the un-fixed
  // inject_carry_fixing never sets gamma_seed_spot either; still a
  // regression this test catches, just via a different failure mode than the
  // review's combined-bug measurement (see this fix round's report for the
  // full before/after chain). Reverting BOTH C-1 and C-2 together reproduces
  // the review's actual wrong-finite-number shape instead.
  EXPECT_TRUE(std::isfinite(g->theta_carry));
  EXPECT_TRUE(std::isfinite(g->theta_zero_fixing));
  // The "must differ" contract, pinned directly: a bitwise-equal pair is
  // exactly what exposed this bug (fixing_dec = k_var_future for one branch,
  // 0.0 for the other, must produce different repricings).
  EXPECT_NE(g->theta_carry, g->theta_zero_fixing);
}

// ── Corridor variance swap (Task F-3, PV-F3 / LIT-7) ────────────────────────
//
// `DerivKind::CorridorVarSwap` is VarSwap with the replicating weight
// 1{K in C}/K^2: same integrand, same 2/T outer scale, and ONE difference --
// the integration window is [ln(corridor_lo/F), ln(corridor_hi/F)] intersected
// with the resolved span, which makes the corridor edges Simpson panel
// boundaries (C-3's machinery, run on the restricted interval).
//
// Three mandated oracles, in the brief's own order:
//   FullCorridorIdentity  -- a corridor spanning the whole grid reproduces
//     K_var on the SAME nodes with the SAME weights.
//   SubCorridorOrdering   -- on a negative-skew fixture, a down-corridor is
//     worth strictly more than an up-corridor of the same log-width.
//   EdgeSplitAccuracy     -- a corridor edge landing mid-grid still converges
//     at the composite-Simpson rate (Standard vs Audit agree to < 1e-6 rel).
// Plus the realized-leg, degenerate-input, dispatch-matrix and split-collision
// coverage; and, per this sprint's standing lesson, at least one fixture in
// which EACH appended field genuinely differs from its trivial value.

// Corridor bound at log-moneyness `k` off forward `F`. Written as a helper so
// every fixture below states its corridor in the coordinate the strip actually
// reasons in, and the absolute-strike conversion happens in exactly one place.
[[nodiscard]] double corridor_strike_at_k(double F, double k) {
  return F * std::exp(k);
}

TEST(Corridor, FullCorridorIdentity) {
  const double spot = 100.0;
  const double sigma = 0.20;
  const double T_test = 0.5;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(spot, 0.01, 1.00);

  DerivConfig cfg = deriv_default_config();

  const auto q_var = var_swap_fair_strike(surf, cs, T_test, cfg);
  ASSERT_TRUE(q_var.has_value()) << q_var.error().to_string();

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = T_test;
  c.notional = 1.0;
  c.strike_dec = 0.0;
  // rv_spec left unaged: this compares the FUTURE leg, which is the strip.

  // (a) The UNBOUNDED encoding (0 on both sides).
  //
  // VACUITY GUARD, and the reason this oracle needs one more than the other
  // two. "A corridor spanning the whole grid reproduces K_var" is satisfied
  // just as well by a build in which the corridor path NEVER RAN -- a routing
  // mistake that sent this contract to `price_var_swap` would pass every value
  // assertion below for the wrong reason. So the DISPATCH itself is witnessed:
  // `CorridorVarSwapStripEvals` is bumped only inside `strip_fair_value_core`
  // under `DerivKind::CorridorVarSwap`, so a non-zero count is direct evidence
  // the corridor body executed, and a zero `VarSwapStripEvals` is evidence it
  // did not silently take the var-swap route instead.
  //
  // SCOPE OF THE WITNESS, stated so it is not over-read: it witnesses ROUTING
  // -- that this contract entered the corridor body -- and NOT that the window
  // was applied. It survives a reversion in which the corridor never restricts
  // anything, which is correct and expected; this oracle is an identity, so no
  // assertion in it can discriminate that reversion. `SubCorridorOrdering` and
  // `EdgeSplitAccuracy` are where the window itself is measured.
  namespace ledger = atx::vol::counters::ledger;
  ledger::reset();
  const auto q_unbounded = deriv_price(surf, cs, c, cfg);
  ASSERT_TRUE(q_unbounded.has_value()) << q_unbounded.error().to_string();
  EXPECT_EQ(ledger::snapshot().get(ledger::Solve::CorridorVarSwapStripEvals), 1u);
  EXPECT_EQ(ledger::snapshot().get(ledger::Solve::VarSwapStripEvals), 0u);

  // (b) The brief's literal reading: FINITE bounds that strictly contain the
  // whole resolved grid, so the corridor code path runs with real numbers in
  // it and the fmax/fmin intersection still has to select the span's own
  // endpoints. `strip_k_lo_used`/`strip_k_hi_used` report the resolved span,
  // so a bound one full unit outside each is unambiguously outside.
  ASSERT_TRUE(std::isfinite(q_var->strip_k_lo_used));
  ASSERT_TRUE(std::isfinite(q_var->strip_k_hi_used));
  DerivContract c_wide = c;
  c_wide.corridor_lo = corridor_strike_at_k(spot, q_var->strip_k_lo_used - 1.0);
  c_wide.corridor_hi = corridor_strike_at_k(spot, q_var->strip_k_hi_used + 1.0);
  ledger::reset();
  const auto q_wide = deriv_price(surf, cs, c_wide, cfg);
  ASSERT_TRUE(q_wide.has_value()) << q_wide.error().to_string();
  EXPECT_EQ(ledger::snapshot().get(ledger::Solve::CorridorVarSwapStripEvals), 1u);
  EXPECT_EQ(ledger::snapshot().get(ledger::Solve::VarSwapStripEvals), 0u);

  // THE assertions that carry this oracle. The brief asks for 1e-12; both are
  // pinned BIT-EXACT instead, which is the honest strength of the claim: the
  // corridor intersection is fmax(span_lo, -inf) / fmin(span_hi, +inf) in case
  // (a) and fmax(span_lo, something strictly smaller) in case (b), so the
  // window IS the span in both -- same nodes, same panel plan, same weights,
  // same summation order. Anything weaker than equality here would mean the
  // corridor path had perturbed the quadrature it is supposed to leave alone.
  EXPECT_EQ(q_unbounded->fair_strike_dec, q_var->fair_strike_dec);
  EXPECT_EQ(q_wide->fair_strike_dec, q_var->fair_strike_dec);
  // PROVENANCE CONSISTENCY, and NOT a node-level witness -- said plainly
  // because an earlier version of this comment claimed the opposite and the
  // review measured it false. These four fields CANNOT discriminate a corridor
  // that bound: `strip_k_lo_used`/`strip_k_hi_used` are the PRE-corridor span
  // by explicit design (see `strip_fair_value_core`'s note on why reproduction
  // requires that), and `strip_nodes_used` is the node BUDGET, which a
  // narrower window only makes the `dk_floor_nodes` floor LESS likely to
  // raise. Measured: a corridor 1/150th the span's width reports the identical
  // -1.5 / 1.5 / 257 while its value is 9.3x smaller.
  //
  // So these assert only that the corridor path left the reported grid alone,
  // which is worth pinning but is not evidence about the window. As this quote
  // is currently specified there is NO node-level witness available to this
  // oracle at all. What carries it is `fair_strike_dec` bit-equality above and
  // the ledger-counter ROUTING witness below.
  EXPECT_EQ(q_unbounded->strip_nodes_used, q_var->strip_nodes_used);
  EXPECT_EQ(q_wide->strip_nodes_used, q_var->strip_nodes_used);
  EXPECT_EQ(q_wide->strip_k_lo_used, q_var->strip_k_lo_used);
  EXPECT_EQ(q_wide->strip_k_hi_used, q_var->strip_k_hi_used);
  EXPECT_EQ(q_wide->integration_error_est, q_var->integration_error_est);
  // Flat 20-vol surface, zero carry: the strip's own closed-form answer.
  EXPECT_NEAR(q_unbounded->fair_strike_dec, sigma * sigma, 1.0e-6 * sigma * sigma);
}

TEST(Corridor, SubCorridorOrdering) {
  using atx::vol::deriv_testkit::kSkewRefT;
  using atx::vol::deriv_testkit::make_curves;
  using atx::vol::deriv_testkit::make_skew_surface;

  const double spot = 100.0;
  const double atm_vol = 0.20;
  const double skew_slope = -0.60;  // steep negative skew (rho fixed at -0.7)
  const double convexity = 0.5;
  const EssviSurface surf = make_skew_surface(atm_vol, skew_slope, convexity);
  const CurveSet cs = make_curves(spot, 0.0, 0.0);  // zero carry: isolate skew

  DerivConfig cfg = deriv_default_config();
  cfg.quality = DerivQuality::High;

  // EQUAL WIDTH IN LOG-MONEYNESS, which is the coordinate the strip integrates
  // in, so the two corridors get identical node budgets over identical-length
  // intervals and the only thing that differs is WHICH side of the smile they
  // sit on. F == spot here (make_curves is zero-carry), so k = 0 is exactly
  // the shared edge.
  constexpr double kHalfWidth = 0.30;

  DerivContract down{};
  down.kind = DerivKind::CorridorVarSwap;
  down.maturity_t = kSkewRefT;
  down.notional = 1.0;
  down.corridor_lo = corridor_strike_at_k(spot, -kHalfWidth);
  down.corridor_hi = corridor_strike_at_k(spot, 0.0);

  DerivContract up = down;
  up.corridor_lo = corridor_strike_at_k(spot, 0.0);
  up.corridor_hi = corridor_strike_at_k(spot, kHalfWidth);

  const auto q_down = deriv_price(surf, cs, down, cfg);
  ASSERT_TRUE(q_down.has_value()) << q_down.error().to_string();
  const auto q_up = deriv_price(surf, cs, up, cfg);
  ASSERT_TRUE(q_up.has_value()) << q_up.error().to_string();

  // THE assertion that carries this oracle: a STRICT inequality between two
  // corridors of identical width on the same surface, same T, same cfg. It is
  // the corridor analogue of GammaSwap.SkewOrdering, and it is what rules out
  // the whole "the corridor was ignored" family: a dispatch that integrated
  // the full span for both would make these EQUAL (both would be K_var), and
  // one that got the window's SIGN or direction wrong would order them the
  // other way. Under negative skew the put wing carries richer implied vol, so
  // the OTM prices the down-corridor integrates are strictly larger.
  EXPECT_GT(q_down->fair_strike_dec, q_up->fair_strike_dec);
  EXPECT_GT(q_up->fair_strike_dec, 0.0);

  // Both are STRICT sub-corridors -- each is worth strictly less than the
  // whole strip, which is the other half of "the corridor did something". A
  // corridor covering only part of the span cannot reach the full K_var: the
  // integrand is strictly positive on the excluded region.
  const auto q_var = var_swap_fair_strike(surf, cs, kSkewRefT, cfg);
  ASSERT_TRUE(q_var.has_value());
  EXPECT_LT(q_down->fair_strike_dec, q_var->fair_strike_dec);
  EXPECT_LT(q_up->fair_strike_dec, q_var->fair_strike_dec);
  // ... and the two halves plus the excluded wings must not exceed it either
  // (additivity of the integral over disjoint sub-intervals).
  EXPECT_LT(q_down->fair_strike_dec + q_up->fair_strike_dec, q_var->fair_strike_dec);
}

TEST(Corridor, EdgeSplitAccuracy) {
  using atx::vol::deriv_testkit::kSkewRefT;
  using atx::vol::deriv_testkit::make_curves;
  using atx::vol::deriv_testkit::make_skew_surface;

  const double spot = 100.0;
  const EssviSurface surf = make_skew_surface(0.20, -0.60, 0.5);
  const CurveSet cs = make_curves(spot, 0.0, 0.0);

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = kSkewRefT;
  c.notional = 1.0;
  // Edges deliberately MID-GRID and off any round fraction of the tier
  // spacing, so neither lands on a node of either tier by luck: Standard's
  // 257-node span here is +-1.5 (dk ~= 0.0117) and Audit's is 2049 nodes
  // (dk ~= 0.00146). Both edges also straddle k = 0, so the split has to keep
  // the put-call parity kink AND both corridor edges as panel boundaries at
  // once.
  c.corridor_lo = corridor_strike_at_k(spot, -0.3717);
  c.corridor_hi = corridor_strike_at_k(spot, 0.2341);

  DerivConfig std_cfg = deriv_default_config();
  std_cfg.quality = DerivQuality::Standard;
  DerivConfig audit_cfg = deriv_default_config();
  audit_cfg.quality = DerivQuality::Audit;

  const auto q_std = deriv_price(surf, cs, c, std_cfg);
  ASSERT_TRUE(q_std.has_value()) << q_std.error().to_string();
  const auto q_audit = deriv_price(surf, cs, c, audit_cfg);
  ASSERT_TRUE(q_audit.has_value()) << q_audit.error().to_string();

  ASSERT_GT(q_audit->fair_strike_dec, 0.0);
  const double rel =
      std::fabs(q_std->fair_strike_dec - q_audit->fair_strike_dec) / q_audit->fair_strike_dec;

  // THE assertion that carries this oracle, and what it actually proves. If
  // either corridor edge were left INSIDE a Simpson panel -- which is what the
  // rejected "keep the full span, multiply by an indicator" design would do --
  // the integrand would have a JUMP there, the panel straddling it would
  // contribute an O(h) error rather than O(h^4), and Standard (dk ~ 0.0117)
  // versus Audit (dk ~ 0.00146) would disagree at the 1e-3 level, not 1e-6.
  // The edges being the restricted interval's own ENDPOINTS is what buys this.
  EXPECT_LT(rel, 1.0e-6);

  // Mechanism, not just outcome: the Richardson estimate is only populated
  // when EVERY panel of the split landed on the 4m+1 lattice, so a finite
  // value here is direct evidence the corridor-restricted split preserved the
  // even-panel-count invariant composite Simpson needs (and that the F-1 /
  // C-3 error estimate is still meaningful on this grid).
  EXPECT_TRUE(std::isfinite(q_std->integration_error_est));
  EXPECT_TRUE(std::isfinite(q_audit->integration_error_est));
  // The observed gap must also sit inside the two grids' own reported error
  // budget -- an anchored bound (F-1's I-3 precedent) rather than a magic
  // constant, so it tightens automatically when the quadrature improves.
  EXPECT_LE(std::fabs(q_std->fair_strike_dec - q_audit->fair_strike_dec),
            q_std->integration_error_est + q_audit->integration_error_est);
}

// The corridor is stated in ABSOLUTE STRIKES but resolved through F, and the
// three oracles above run on zero-carry fixtures where F == spot, so none of
// them can tell an F-conversion from a spot-conversion. This one can: under
// r - q = 6% at a 3M tenor the two differ by ln(F/S) = 0.015, and the slab of
// integrand that width sits right at the ATM peak.
//
// The oracle is EXACT rather than approximate. A corridor whose lower bound is
// placed at the forward itself, on a span pinned to [-1.2, +1.2], integrates
// exactly [0, 1.2]; the plain variance strip with its SPAN pinned to [0, 1.2]
// and the same node count integrates the identical interval. `plan_strip_split`
// is a pure function of (k_lo, k_hi, n, wing_band) and all four agree, so the
// two runs share panels, nodes, weights and summation order. A spot-conversion
// would instead integrate [0.015, 1.2] and lose ~10% of the value.
TEST(Corridor, BoundsResolveThroughTheForwardNotTheSpot) {
  using atx::vol::deriv_testkit::kSkewRefT;
  using atx::vol::deriv_testkit::make_curves;
  using atx::vol::deriv_testkit::make_skew_surface;

  const double spot = 100.0;
  const double r = 0.06;
  const double q_div = 0.0;
  const EssviSurface surf = make_skew_surface(0.20, -0.60, 0.5);
  const CurveSet cs = make_curves(spot, r, q_div);
  // `make_curves` builds its forward pillars as spot*exp((r-q)*T_i) and
  // `kSkewRefT` IS one of those pillars, so this is the forward the pricer
  // resolves, not an approximation of it.
  const double forward = spot * std::exp((r - q_div) * kSkewRefT);
  ASSERT_GT(forward, spot * 1.01);  // the conversions are far apart here

  constexpr double kHalfSpan = 1.2;
  constexpr std::uint32_t kNodes = 1025u;

  DerivConfig ref_cfg = deriv_default_config();
  ref_cfg.k_min_log = 0.0;  // pinned: k_max_log below is non-zero
  ref_cfg.k_max_log = kHalfSpan;
  ref_cfg.strip_nodes = kNodes;
  const auto ref = var_swap_fair_strike(surf, cs, kSkewRefT, ref_cfg);
  ASSERT_TRUE(ref.has_value()) << ref.error().to_string();

  DerivConfig cor_cfg = deriv_default_config();
  cor_cfg.k_min_log = -kHalfSpan;
  cor_cfg.k_max_log = kHalfSpan;
  cor_cfg.strip_nodes = kNodes;

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = kSkewRefT;
  c.notional = 1.0;
  c.corridor_lo = forward;  // k_lo == ln(F/F) == 0 under an F-conversion
  c.corridor_hi = 0.0;      // unbounded above
  const auto cor = deriv_price(surf, cs, c, cor_cfg);
  ASSERT_TRUE(cor.has_value()) << cor.error().to_string();

  // THE assertion, with its tolerance EXPLAINED rather than tuned. The two
  // runs are not bit-identical, and the reason is measurable: the corridor run
  // opens its window at ln(F_resolved/F_fixture), which `resolve_forward`'s
  // log-blend leaves at ~1e-16 rather than exactly 0. That sub-ulp endpoint
  // offset is enough to move `apportion_units`' largest-remainder split by one
  // Simpson unit between the two panels, which is an O(h^4) requantization,
  // and the observed gap is 1.1437378821810285e-13 absolute
  // (8.1e-12 relative). It is NOT a difference in what was integrated.
  //
  // Two bounds, so this is not a magic constant: an ANCHORED one against the
  // grids' own Richardson estimates (F-1's I-3 precedent -- it tightens by
  // itself when the quadrature improves), and a 1e-9 relative backstop that
  // still sits seven decades below the ~2%+ effect the control below measures
  // for the spot-conversion this test exists to exclude.
  const double gap = std::fabs(cor->fair_strike_dec - ref->fair_strike_dec);
  ASSERT_TRUE(std::isfinite(ref->integration_error_est));
  ASSERT_TRUE(std::isfinite(cor->integration_error_est));
  EXPECT_LE(gap, ref->integration_error_est + cor->integration_error_est);
  EXPECT_LT(gap, 1.0e-9 * ref->fair_strike_dec);

  // The control that MEASURES how far apart the two conversions are, so the
  // 1e-12 tolerance above is not merely a tight number on an untested claim.
  // A spot-conversion of `corridor_lo == forward` would open the window at
  // k = ln(F/S) = 0.015 instead of 0; this contract reproduces exactly that
  // window through the (correct) F-conversion, by placing its bound at
  // F*exp(ln(F/S)).
  DerivContract shifted_window = c;
  shifted_window.corridor_lo = forward * (forward / spot);
  const auto shifted = deriv_price(surf, cs, shifted_window, cor_cfg);
  ASSERT_TRUE(shifted.has_value()) << shifted.error().to_string();
  EXPECT_LT(shifted->fair_strike_dec, 0.98 * ref->fair_strike_dec);
}

// ── Realized leg: the previous-close convention, and the non-trivial fixtures
//
// THE SPRINT'S STANDING LESSON (F-2's I-3, proven by construction there): a
// green suite proves nothing if every fixture pins the new field at its
// no-op value. F-2's reviewer built a fully-corrected TU and got 21/21
// passing BITWISE IDENTICAL to the broken one, because every gamma fixture
// had gamma_seed_spot == curves.spot. The three oracles above leave `rv_spec`
// unaged and are blind to this whole half of the task by construction, so the
// fixtures below deliberately put each appended field at a value where a wrong
// implementation changes an assertion:
//   * `n_obs_in_corridor` STRICTLY LESS than `n_obs_done`, never equal;
//   * `sum_sq_log_returns_in_corridor` pinned to the PREVIOUS-CLOSE return's
//     square and asserted DIFFERENT from the current-close alternative's;
//   * `rv_corridor_done_dec` STRICTLY DIFFERENT from `rv_done_dec`;
//   * `corridor_lo`/`corridor_hi` genuinely narrower than the resolved span.

TEST(Corridor, TrackerCountsOnlyPreviousCloseInsideTheCorridor) {
  // Corridor [95, 105] around a 100 spot. The walk is chosen so the
  // PREVIOUS-CLOSE convention and the (wrong) current-close convention pick
  // DIFFERENT return sets -- the only construction that distinguishes the
  // documented convention from its alternative.
  //
  //   seed 100                       -- no return
  //   100 -> 110   prev 100 IN   cur 110 OUT   -> counts under prev-close only
  //   110 -> 108   prev 110 OUT  cur 108 OUT   -> counts under neither
  //   108 -> 100   prev 108 OUT  cur 100 IN    -> counts under cur-close only
  //
  // Both conventions give n_obs_in_corridor == 1, so a COUNT assertion alone
  // cannot separate them; the accumulated Sigma r^2 is what discriminates, and
  // the two candidate returns differ by ~19%.
  auto tracker = RealizedTracker::create_corridor(252.0, 10u, 95.0, 105.0);
  ASSERT_TRUE(tracker.has_value()) << tracker.error().to_string();

  const double path[] = {100.0, 110.0, 108.0, 100.0};
  for (const double s : path) {
    const auto st = tracker->observe(s);
    ASSERT_TRUE(st.has_value()) << st.error().to_string();
  }
  const RealizedVarianceSpec rv = tracker->snapshot();

  ASSERT_EQ(rv.n_obs_done, 3u);
  // NON-TRIVIAL BY CONSTRUCTION: 1 != 3. A corridor admitting everything (the
  // field at its no-op value) would read 3 and collapse every assertion below
  // onto the plain leg.
  EXPECT_EQ(rv.n_obs_in_corridor, 1u);

  const double r1 = std::log(110.0 / 100.0);
  const double r3 = std::log(100.0 / 108.0);
  // THE assertion that carries the convention.
  EXPECT_DOUBLE_EQ(rv.sum_sq_log_returns_in_corridor, r1 * r1);
  EXPECT_NE(rv.sum_sq_log_returns_in_corridor, r3 * r3);
  // Normalized by n_obs_done (3), NOT by n_obs_in_corridor (1) -- the field's
  // documented contract, and what makes the n_done/n_total aged blend land on
  // annualization * Sigma_{in C} r^2 / n_total.
  EXPECT_DOUBLE_EQ(rv.rv_corridor_done_dec, 252.0 * (r1 * r1) / 3.0);
  EXPECT_NE(rv.rv_corridor_done_dec, rv.rv_done_dec);
  EXPECT_LT(rv.rv_corridor_done_dec, rv.rv_done_dec);
}

TEST(Corridor, TrackerUnboundedCorridorTracksThePlainLegExactly) {
  // The realized-leg half of FullCorridorIdentity: an unbounded corridor
  // admits every fixing, so the corridor accumulators must shadow the plain
  // ones BIT-EXACTLY. That is what lets a caller read the corridor fields off
  // ANY tracker without first asking whether one was configured.
  auto plain = RealizedTracker::create(252.0, 10u);
  ASSERT_TRUE(plain.has_value());
  auto wide = RealizedTracker::create_corridor(252.0, 10u, 0.0, 0.0);
  ASSERT_TRUE(wide.has_value());

  const double path[] = {100.0, 110.0, 108.0, 100.0, 91.0};
  for (const double s : path) {
    ASSERT_TRUE(plain->observe(s).has_value());
    ASSERT_TRUE(wide->observe(s).has_value());
  }
  const RealizedVarianceSpec a = plain->snapshot();
  const RealizedVarianceSpec b = wide->snapshot();

  EXPECT_EQ(a.n_obs_in_corridor, a.n_obs_done);
  EXPECT_EQ(a.sum_sq_log_returns_in_corridor, a.sum_sq_log_returns_done);
  EXPECT_EQ(a.rv_corridor_done_dec, a.rv_done_dec);
  EXPECT_EQ(b.n_obs_in_corridor, a.n_obs_in_corridor);
  EXPECT_EQ(b.sum_sq_log_returns_in_corridor, a.sum_sq_log_returns_in_corridor);
  EXPECT_EQ(b.rv_corridor_done_dec, a.rv_corridor_done_dec);
}

TEST(Corridor, AgedBlendReadsTheCorridorAccrualNotThePlainOne) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0;
  c.strike_dec = 0.0;
  c.corridor_lo = 90.0;
  c.corridor_hi = 115.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 100u;
  c.rv_spec.n_obs_done = 40u;
  // The two accrued legs are DELIBERATELY different, and the corridor count is
  // DELIBERATELY below the observed count: reading `rv_done_dec` (VarSwap's
  // field) instead of `rv_corridor_done_dec` moves the fair strike by
  // w_done * (0.09 - 0.03) = 0.024, some 40x any quadrature noise here.
  c.rv_spec.rv_done_dec = 0.09;
  c.rv_spec.rv_corridor_done_dec = 0.03;
  c.rv_spec.n_obs_in_corridor = 25u;

  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value()) << q.error().to_string();

  const double w_done = 0.40;
  const double w_future = 0.60;
  EXPECT_DOUBLE_EQ(q->accrued_component_dec, w_done * 0.03);
  EXPECT_NE(q->accrued_component_dec, w_done * 0.09);
  EXPECT_DOUBLE_EQ(q->fair_strike_dec, q->accrued_component_dec + q->future_component_dec);
  EXPECT_NEAR(q->future_component_dec, w_future * q->uncapped_var_dec,
              1.0e-12 * std::fabs(q->uncapped_var_dec));
  EXPECT_TRUE(has_flag(q->flags, DerivFlags::Aged));
}

TEST(Corridor, ConditionalVariantComesFromTheSameAccrualAsTheUnconditionalOne) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0;
  c.corridor_lo = 90.0;
  c.corridor_hi = 115.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 100u;
  c.rv_spec.n_obs_done = 40u;
  c.rv_spec.rv_corridor_done_dec = 0.03;
  // 25 != 40: the conditional field IS the ratio between these two, so a
  // fixture with them equal would be blind to it.
  c.rv_spec.n_obs_in_corridor = 25u;

  const auto q = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(q.has_value()) << q.error().to_string();

  // "Both numbers from ONE accrual", pinned as an identity between the two
  // published fields rather than as two independent expected constants: the
  // unconditional accrued component and the conditional reading are the same
  // accrual scaled by n_done/n_in_corridor, which is only true if one pass
  // produced both.
  EXPECT_DOUBLE_EQ(q->conditional_corridor_var_dec, 0.03 * 40.0 / 25.0);
  EXPECT_DOUBLE_EQ(q->conditional_corridor_var_dec * 25.0 / 100.0,
                   q->accrued_component_dec);
  EXPECT_NE(q->conditional_corridor_var_dec, c.rv_spec.rv_corridor_done_dec);

  // Nothing in the corridor yet -> NaN ("no conditional average of an empty
  // set"), never 0.0, which would read as "flat in there".
  DerivContract empty = c;
  empty.rv_spec.n_obs_in_corridor = 0u;
  empty.rv_spec.rv_corridor_done_dec = 0.0;  // forced by the same invariant
  const auto q_empty = deriv_price(surf, cs, empty, deriv_default_config());
  ASSERT_TRUE(q_empty.has_value()) << q_empty.error().to_string();
  EXPECT_TRUE(std::isnan(q_empty->conditional_corridor_var_dec));

  // ... and never populated on another kind: a VarSwap quote must not carry a
  // number a caller could mistake for a corridor reading.
  DerivContract var = c;
  var.kind = DerivKind::VarSwap;
  var.corridor_lo = 0.0;
  var.corridor_hi = 0.0;
  const auto q_var = deriv_price(surf, cs, var, deriv_default_config());
  ASSERT_TRUE(q_var.has_value()) << q_var.error().to_string();
  EXPECT_TRUE(std::isnan(q_var->conditional_corridor_var_dec));
}

// C-2/C-4 class regression guard (F-2's own Criticals were exactly this shape
// on the gamma leg): if `inject_carry_fixing` had no corridor arm, the corridor
// accrual would be IDENTICAL in both injected repricings and theta_carry would
// equal theta_zero_fixing bitwise for every corridor swap. Spot sits INSIDE the
// corridor here, which is the regime where the two genuinely must differ.
TEST(Corridor, CarryThetaDiffersFromZeroFixingWhenSpotIsInsideTheCorridor) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0e6;
  c.strike_dec = 0.0;
  c.corridor_lo = 90.0;  // spot 100 is INSIDE
  c.corridor_hi = 115.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 100u;
  c.rv_spec.n_obs_done = 40u;
  c.rv_spec.rv_done_dec = 0.05;
  c.rv_spec.rv_corridor_done_dec = 0.03;
  c.rv_spec.n_obs_in_corridor = 25u;

  const atx::vol::DerivGreekBumps bumps{};
  const auto g = atx::vol::deriv_greeks(surf, cs, c, deriv_default_config(), bumps);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  EXPECT_TRUE(std::isfinite(g->theta_carry));
  EXPECT_TRUE(std::isfinite(g->theta_zero_fixing));
  EXPECT_NE(g->theta_carry, g->theta_zero_fixing);
}

// The other half of the same contract, and a TEST rather than a footnote: on a
// corridor swap whose spot is OUTSIDE the corridor, theta_carry ==
// theta_zero_fixing BITWISE is CORRECT. A fixing that cannot count contributes
// nothing whatever the market does, so both injections add 0.0 to the corridor
// leg. Pinning it stops a later reader from "fixing" the equality away.
TEST(Corridor, CarryThetaEqualsZeroFixingWhenSpotIsOutsideTheCorridor) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0e6;
  c.strike_dec = 0.0;
  c.corridor_lo = 120.0;  // spot 100 is OUTSIDE (below the corridor)
  c.corridor_hi = 140.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 100u;
  c.rv_spec.n_obs_done = 40u;
  c.rv_spec.rv_done_dec = 0.05;
  c.rv_spec.rv_corridor_done_dec = 0.01;
  c.rv_spec.n_obs_in_corridor = 8u;

  const atx::vol::DerivGreekBumps bumps{};
  const auto g = atx::vol::deriv_greeks(surf, cs, c, deriv_default_config(), bumps);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  EXPECT_TRUE(std::isfinite(g->theta_carry));
  EXPECT_EQ(g->theta_carry, g->theta_zero_fixing);
}

// ── Degenerate corridor inputs: the contract, stated as tests ───────────────

TEST(Corridor, RejectsMalformedCorridorBounds) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  DerivContract base{};
  base.kind = DerivKind::CorridorVarSwap;
  base.maturity_t = 0.5;
  base.notional = 1.0;

  struct Case {
    double lo;
    double hi;
    const char *why;
  };
  // `0` is OVERLOADED as "unbounded on this side", so it cannot ALSO mean
  // "invalid" -- which forces every other unusable value to be named. +Inf is
  // here deliberately: it passes a bare `x > 0.0` (the exact check F-2 shipped
  // in the gamma anchor guard and had to correct in a cleanup round) and would
  // otherwise read as "unbounded" spelled a second, undocumented way.
  const Case bad[] = {
      {-1.0, 0.0, "negative lower bound"},
      {0.0, -1.0, "negative upper bound"},
      {inf, 0.0, "+Inf lower bound is not a second spelling of unbounded"},
      {0.0, inf, "+Inf upper bound is not a second spelling of unbounded"},
      {-inf, 0.0, "-Inf lower bound"},
      {nan, 0.0, "NaN lower bound"},
      {0.0, nan, "NaN upper bound"},
      {120.0, 80.0, "inverted corridor"},
      {100.0, 100.0, "zero-width corridor accrues and replicates nothing"},
  };
  for (const Case &k : bad) {
    DerivContract c = base;
    c.corridor_lo = k.lo;
    c.corridor_hi = k.hi;
    const auto quote = deriv_price(surf, cs, c, deriv_default_config());
    EXPECT_FALSE(quote.has_value()) << k.why;
    if (!quote.has_value()) {
      EXPECT_EQ(quote.error().code(), ErrorCode::InvalidArgument) << k.why;
    }
  }

  // The LEGAL degenerate encodings, for contrast: fully unbounded, and each
  // one-sided half. None is an error -- 0 means unbounded, and a half-corridor
  // is an ordinary product.
  const Case good[] = {
      {0.0, 0.0, "both sides unbounded"},
      {90.0, 0.0, "lower half only"},
      {0.0, 115.0, "upper half only"},
  };
  for (const Case &k : good) {
    DerivContract c = base;
    c.corridor_lo = k.lo;
    c.corridor_hi = k.hi;
    const auto quote = deriv_price(surf, cs, c, deriv_default_config());
    EXPECT_TRUE(quote.has_value()) << k.why;
  }
}

TEST(Corridor, RejectsCorridorBoundsOnNonCorridorKinds) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  // Same rule `cap_dec` already follows: a knob that names nothing on this
  // kind is a caller error, not a silent no-op. Without it, a corridor set on
  // a VarSwap prices as a plain var swap and the caller never learns the
  // corridor was dropped -- the silent-scope class P-4's C-1 and F-2's C-2
  // both were.
  for (const DerivKind kind : {DerivKind::VarSwap, DerivKind::VolSwap, DerivKind::GammaSwap}) {
    DerivContract c{};
    c.kind = kind;
    c.maturity_t = 0.5;
    c.notional = 1.0;
    c.corridor_lo = 90.0;
    c.corridor_hi = 115.0;
    const auto quote = deriv_price(surf, cs, c, deriv_default_config());
    ASSERT_FALSE(quote.has_value()) << static_cast<int>(kind);
    EXPECT_EQ(quote.error().code(), ErrorCode::InvalidArgument);
  }
}

TEST(Corridor, EmptyIntersectionWithTheResolvedSpanFailsLoud) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0;
  // Far below anything the resolved span reaches. Returning 0.0 would be a
  // plausible-looking wrong number -- the true corridor variance out there is
  // small but not zero, and the caller would get no signal at all.
  c.corridor_lo = corridor_strike_at_k(100.0, -20.0);
  c.corridor_hi = corridor_strike_at_k(100.0, -19.0);

  const auto quote = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_FALSE(quote.has_value());
  EXPECT_EQ(quote.error().code(), ErrorCode::OutOfRange);
}

TEST(Corridor, RejectsInternallyInconsistentCorridorAccrual) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract base{};
  base.kind = DerivKind::CorridorVarSwap;
  base.maturity_t = 0.5;
  base.notional = 1.0;
  base.corridor_lo = 90.0;
  base.corridor_hi = 115.0;
  base.rv_spec.annualization = 252.0;
  base.rv_spec.n_obs_total = 100u;
  base.rv_spec.n_obs_done = 40u;

  // A corridor spec has NO witness field whose absence proves "never
  // populated" -- unlike F-2's gamma_seed_spot, all-zeros is ALSO the entirely
  // legitimate "spot never entered the corridor". What CAN be checked is the
  // arithmetic the tracker maintains; these are the three relations a
  // hand-built or half-migrated spec breaks.
  {
    DerivContract c = base;
    c.rv_spec.n_obs_in_corridor = 41u;  // > n_obs_done
    const auto quote = deriv_price(surf, cs, c, deriv_default_config());
    ASSERT_FALSE(quote.has_value());
    EXPECT_EQ(quote.error().code(), ErrorCode::InvalidArgument);
  }
  {
    DerivContract c = base;
    c.rv_spec.n_obs_in_corridor = 10u;
    c.rv_spec.rv_corridor_done_dec = -0.01;  // a sum of squares cannot be < 0
    const auto quote = deriv_price(surf, cs, c, deriv_default_config());
    ASSERT_FALSE(quote.has_value());
    EXPECT_EQ(quote.error().code(), ErrorCode::InvalidArgument);
  }
  {
    // Catches "the caller set the corridor leg by hand and forgot the count":
    // nothing counted, yet a non-zero corridor variance. The sum is over an
    // empty set, so nothing but 0.0 is representable.
    DerivContract c = base;
    c.rv_spec.n_obs_in_corridor = 0u;
    c.rv_spec.rv_corridor_done_dec = 0.03;
    const auto quote = deriv_price(surf, cs, c, deriv_default_config());
    ASSERT_FALSE(quote.has_value());
    EXPECT_EQ(quote.error().code(), ErrorCode::InvalidArgument);
  }
  {
    DerivContract c = base;
    c.rv_spec.n_obs_in_corridor = 0u;
    c.rv_spec.rv_corridor_done_dec = 0.0;
    const auto quote = deriv_price(surf, cs, c, deriv_default_config());
    EXPECT_TRUE(quote.has_value());
  }
}

TEST(Corridor, TrackerCreateCorridorValidatesThroughTheSamePredicate) {
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(RealizedTracker::create_corridor(252.0, 10u, -1.0, 0.0).has_value());
  EXPECT_FALSE(RealizedTracker::create_corridor(252.0, 10u, 0.0, inf).has_value());
  EXPECT_FALSE(RealizedTracker::create_corridor(252.0, 10u, inf, 0.0).has_value());
  EXPECT_FALSE(RealizedTracker::create_corridor(252.0, 10u, 120.0, 80.0).has_value());
  EXPECT_FALSE(RealizedTracker::create_corridor(252.0, 10u, 100.0, 100.0).has_value());
  // ... and `create`'s own validation still applies through it.
  EXPECT_FALSE(RealizedTracker::create_corridor(0.0, 10u, 90.0, 110.0).has_value());
  EXPECT_FALSE(RealizedTracker::create_corridor(252.0, 0u, 90.0, 110.0).has_value());
  EXPECT_TRUE(RealizedTracker::create_corridor(252.0, 10u, 90.0, 110.0).has_value());
}

// ── Dispatch matrix + the `kind ==` whitelists -Wswitch cannot reach ────────

TEST(Corridor, DispatchMatrix) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0;
  c.corridor_lo = 90.0;
  c.corridor_hi = 115.0;

  for (const DerivEngine e : {DerivEngine::Auto, DerivEngine::StripLogContract}) {
    DerivConfig cfg = deriv_default_config();
    cfg.engine = e;
    const auto quote = deriv_price(surf, cs, c, cfg);
    EXPECT_TRUE(quote.has_value()) << static_cast<int>(e);
  }
  {
    DerivConfig cfg = deriv_default_config();
    cfg.engine = DerivEngine::VolCarrLee;  // names no corridor-variance formula
    const auto quote = deriv_price(surf, cs, c, cfg);
    ASSERT_FALSE(quote.has_value());
    EXPECT_EQ(quote.error().code(), ErrorCode::InvalidArgument);
  }
  // The reserved engines stay reserved: CorridorVarSwap is deliberately NOT
  // added to the reserved-engine switch's allow-list.
  for (const DerivEngine e : {DerivEngine::RvDistributionProxy, DerivEngine::RvDistributionAffine,
                              DerivEngine::McQe}) {
    DerivConfig cfg = deriv_default_config();
    cfg.engine = e;
    const auto quote = deriv_price(surf, cs, c, cfg);
    ASSERT_FALSE(quote.has_value()) << static_cast<int>(e);
    EXPECT_EQ(quote.error().code(), ErrorCode::NotImplemented);
  }
  {
    DerivContract capped = c;
    capped.cap_dec = 0.25;  // uncapped kind, unchanged by the corridor
    const auto quote = deriv_price(surf, cs, capped, deriv_default_config());
    ASSERT_FALSE(quote.has_value());
    EXPECT_EQ(quote.error().code(), ErrorCode::InvalidArgument);
  }
}

TEST(Corridor, Diffusion1OverNRejected) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0;
  c.corridor_lo = 90.0;
  c.corridor_hi = 115.0;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 100u;

  // Broadie-Jain's addend is derived for the plain, ALWAYS-COUNTING estimator.
  // Applying it to an indicator-gated one would be a wrong number; ignoring the
  // caller's request silently is the P-4 C-1 defect. Loud NotImplemented, the
  // same remedy GammaSwap chose.
  DerivConfig cfg = deriv_default_config();
  cfg.discrete_correction_mode = DerivDiscreteCorrection::Diffusion1OverN;
  const auto quote = deriv_price(surf, cs, c, cfg);
  ASSERT_FALSE(quote.has_value());
  EXPECT_EQ(quote.error().code(), ErrorCode::NotImplemented);
}

// P-4 scope audit, corridor edition: `analytic_scope_from_cfg` /
// `analytic_in_scope` are `kind ==` WHITELISTS, which -Wswitch cannot police --
// the recurring silent-miss class of this sprint. A corridor contract must fall
// back to finite differences: the closed form differentiates the FULL-span
// strip and has no term for a moving integration boundary. Verified BY TEST,
// not by reading the predicate.
TEST(Corridor, AnalyticStripMethodFallsBackToFiniteDifference) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0e6;
  c.corridor_lo = 90.0;
  c.corridor_hi = 115.0;

  atx::vol::DerivGreekBumps fd{};
  fd.method = atx::vol::DerivGreekMethod::FiniteDifference;
  atx::vol::DerivGreekBumps analytic{};
  analytic.method = atx::vol::DerivGreekMethod::AnalyticStrip;

  const auto g_fd = atx::vol::deriv_greeks(surf, cs, c, deriv_default_config(), fd);
  ASSERT_TRUE(g_fd.has_value()) << g_fd.error().to_string();
  const auto g_an = atx::vol::deriv_greeks(surf, cs, c, deriv_default_config(), analytic);
  ASSERT_TRUE(g_an.has_value()) << g_an.error().to_string();

  // Bit-identical: the fallback picks the same NUMERICAL METHOD, so every
  // greek must match exactly. A predicate admitting CorridorVarSwap into the
  // analytic path would differentiate VarSwap's full-span closed form against
  // a corridor center and diverge here.
  EXPECT_DOUBLE_EQ(g_an->delta, g_fd->delta);
  EXPECT_DOUBLE_EQ(g_an->gamma, g_fd->gamma);
  EXPECT_DOUBLE_EQ(g_an->vega, g_fd->vega);
  EXPECT_DOUBLE_EQ(g_an->volga, g_fd->volga);
  EXPECT_DOUBLE_EQ(g_an->vanna, g_fd->vanna);
}

// ── Split-point collisions, and the reported-grid decision ──────────────────

// F-1's clamp-gated wing-band boundaries and C-3's k = 0 parity kink now share
// one panelization with the corridor edges. `strip_panel_bounds`' strict
// comparisons are what keep every collision safe, but "safe" has to be
// MEASURED: each case must price, must stay on the Richardson lattice (a
// finite `integration_error_est` means every panel is 4m+1, which is the
// even-interval-count invariant composite Simpson needs), and must agree with
// a much finer grid.
TEST(Corridor, EdgeCollisionsWithTheKinkAndTheWingBand) {
  using atx::vol::deriv_testkit::kSkewRefT;
  using atx::vol::deriv_testkit::make_curves;
  using atx::vol::deriv_testkit::make_skew_surface;

  const double spot = 100.0;
  const EssviSurface surf = make_skew_surface(0.20, -0.60, 0.5);
  const CurveSet cs = make_curves(spot, 0.0, 0.0);  // zero carry: F == spot

  DerivConfig cfg = deriv_default_config();
  cfg.wing_clamp_k = 0.5;  // explicit band, so the collisions below are exact
  DerivConfig fine = cfg;
  fine.quality = DerivQuality::Audit;

  // Standard's span here is +-1.5 over 257 nodes, so one node spacing is
  // ~0.0117; `kNodeStep` places an edge deliberately within one node of
  // another split point.
  constexpr double kNodeStep = 0.0117;

  struct Case {
    double k_lo;
    double k_hi;
    const char *why;
  };
  const Case cases[] = {
      {-0.5, 0.5, "both edges EXACTLY on the wing band"},
      {-0.5, 0.0, "lower edge on the band, upper EXACTLY on the k = 0 kink"},
      {0.0, 0.5, "lower edge exactly on k = 0, upper on the band"},
      {-0.5 - 1.0e-9, 0.4, "one nanounit OUTSIDE the band: a near-zero-width panel"},
      {-0.5 + 1.0e-9, 0.4, "one nanounit INSIDE the band: the band kink is dropped"},
      {-1.0e-9, 0.4, "lower edge one nanounit below the k = 0 kink"},
      {-0.4, 1.0e-9, "upper edge one nanounit above the k = 0 kink"},
      {-0.5 - kNodeStep, 0.4, "lower edge exactly one node outside the band"},
      {-kNodeStep, 0.4, "lower edge exactly one node below the k = 0 kink"},
      {-0.37, 0.23, "both edges strictly interior, no collision (control)"},
  };

  for (const Case &k : cases) {
    DerivContract c{};
    c.kind = DerivKind::CorridorVarSwap;
    c.maturity_t = kSkewRefT;
    c.notional = 1.0;
    c.corridor_lo = corridor_strike_at_k(spot, k.k_lo);
    c.corridor_hi = corridor_strike_at_k(spot, k.k_hi);

    const auto quote = deriv_price(surf, cs, c, cfg);
    ASSERT_TRUE(quote.has_value()) << k.why << ": " << quote.error().to_string();
    ASSERT_TRUE(std::isfinite(quote->fair_strike_dec)) << k.why;
    EXPECT_GT(quote->fair_strike_dec, 0.0) << k.why;
    // Even panel counts survived every collision: a degenerate or odd-interval
    // panel would drop the Richardson estimate to NaN.
    EXPECT_TRUE(std::isfinite(quote->integration_error_est)) << k.why;

    const auto q_fine = deriv_price(surf, cs, c, fine);
    ASSERT_TRUE(q_fine.has_value()) << k.why << ": " << q_fine.error().to_string();
    // Converged, INCLUDING the near-zero-width-panel cases: a panel of width
    // 1e-9 contributes ~1e-9 * integrand and steals only its 4 mandated
    // intervals from the others.
    EXPECT_LT(std::fabs(quote->fair_strike_dec - q_fine->fair_strike_dec),
              1.0e-6 * q_fine->fair_strike_dec)
        << k.why;
  }
}

// The reported-grid decision, pinned so it cannot be "tidied" into reporting
// the integration window. `strip_k_lo_used`/`strip_k_hi_used` must stay the
// PRE-corridor span, because their contract is REPRODUCTION and a replaying
// caller re-derives the corridor from the contract against its own forward.
// Reporting the window instead would let `deriv_greeks`' pinned grid clip the
// corridor on ONE side only under a spot bump, halving the corridor edge's
// contribution to delta.
TEST(Corridor, ReportedGridIsThePreCorridorSpanAndReproducesTheQuote) {
  using atx::vol::deriv_testkit::kSkewRefT;
  using atx::vol::deriv_testkit::make_curves;
  using atx::vol::deriv_testkit::make_skew_surface;

  const double spot = 100.0;
  const EssviSurface surf = make_skew_surface(0.20, -0.60, 0.5);
  const CurveSet cs = make_curves(spot, 0.0, 0.0);  // zero carry: F == spot

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = kSkewRefT;
  c.notional = 1.0;
  c.corridor_lo = corridor_strike_at_k(spot, -0.37);
  c.corridor_hi = corridor_strike_at_k(spot, 0.23);

  const auto quote = deriv_price(surf, cs, c, deriv_default_config());
  ASSERT_TRUE(quote.has_value()) << quote.error().to_string();

  // THE assertion pinning the decision: the reported span STRICTLY CONTAINS
  // the corridor window, i.e. it is the pre-corridor span, not the window.
  EXPECT_LT(quote->strip_k_lo_used, -0.37);
  EXPECT_GT(quote->strip_k_hi_used, 0.23);

  // ... and the reproduction contract those fields exist for still holds:
  // feeding them back with the SAME contract reproduces the quote bit-exactly.
  DerivConfig pinned = deriv_default_config();
  pinned.k_min_log = quote->strip_k_lo_used;
  pinned.k_max_log = quote->strip_k_hi_used;
  pinned.strip_nodes = quote->strip_nodes_used;
  const auto replay = deriv_price(surf, cs, c, pinned);
  ASSERT_TRUE(replay.has_value()) << replay.error().to_string();
  EXPECT_EQ(replay->fair_strike_dec, quote->fair_strike_dec);
  EXPECT_EQ(replay->strip_nodes_used, quote->strip_nodes_used);
}

// Truncation is a COVERAGE verdict, and for a corridor swap the thing that
// must be covered is the CORRIDOR, not 6*sigma*sqrt(T). Both directions are
// pinned because the naive reading gets each wrong in the opposite way: a
// narrow interior corridor would be reported truncated forever, and a corridor
// reaching past the span would be reported complete.
TEST(Corridor, TruncationFollowsTheCorridorNotTheVolScale) {
  const double spot = 100.0;
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(spot, 0.01, 1.00);  // zero carry: F == spot

  DerivConfig cfg = deriv_default_config();

  DerivContract inside{};
  inside.kind = DerivKind::CorridorVarSwap;
  inside.maturity_t = 0.5;
  inside.notional = 1.0;
  inside.corridor_lo = corridor_strike_at_k(spot, -0.30);
  inside.corridor_hi = corridor_strike_at_k(spot, 0.30);
  const auto q_inside = deriv_price(surf, cs, inside, cfg);
  ASSERT_TRUE(q_inside.has_value()) << q_inside.error().to_string();
  EXPECT_FALSE(has_flag(q_inside->flags, DerivFlags::StripTruncatedLeft));
  EXPECT_FALSE(has_flag(q_inside->flags, DerivFlags::StripTruncatedRight));

  // The same tenor as a plain VarSwap: the vol-scaled requirement is
  // 6*0.20*sqrt(0.5) ~= 0.85, comfortably inside the Standard span, so the
  // plain quote is untruncated too -- which is what makes the corridor result
  // above a statement about the corridor rule rather than a coincidence.
  const auto q_var = var_swap_fair_strike(surf, cs, 0.5, cfg);
  ASSERT_TRUE(q_var.has_value());
  ASSERT_TRUE(std::isfinite(q_var->strip_k_lo_used));

  DerivContract beyond = inside;
  beyond.corridor_lo = corridor_strike_at_k(spot, q_var->strip_k_lo_used - 0.5);
  beyond.corridor_hi = corridor_strike_at_k(spot, q_var->strip_k_hi_used + 0.5);
  const auto q_beyond = deriv_price(surf, cs, beyond, cfg);
  ASSERT_TRUE(q_beyond.has_value()) << q_beyond.error().to_string();
  // In-corridor variance the grid never integrated: genuinely truncated on
  // both sides, even though the vol-scaled test says the span was ample.
  EXPECT_TRUE(has_flag(q_beyond->flags, DerivFlags::StripTruncatedLeft));
  EXPECT_TRUE(has_flag(q_beyond->flags, DerivFlags::StripTruncatedRight));
}

// The corridor is RE-RESOLVED per pricing against that pricing's own forward,
// not frozen at inception -- the design decision the brief leaves open. A
// corridor is a fixed barrier in PRICE space, so a forward move genuinely
// changes how much of the contract's variance falls inside it. This test is
// that decision in executable form.
TEST(Corridor, WindowIsReResolvedAgainstEachPricingsOwnForward) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = 0.5;
  c.notional = 1.0;
  c.corridor_lo = 100.0;  // an UP-corridor: everything at or above 100
  c.corridor_hi = 0.0;

  const CurveSet cs_low = make_flat_curves(80.0, 0.01, 1.00);
  const CurveSet cs_high = make_flat_curves(125.0, 0.01, 1.00);

  DerivConfig cfg = deriv_default_config();
  const auto q_low = deriv_price(surf, cs_low, c, cfg);
  ASSERT_TRUE(q_low.has_value()) << q_low.error().to_string();
  const auto q_high = deriv_price(surf, cs_high, c, cfg);
  ASSERT_TRUE(q_high.has_value()) << q_high.error().to_string();

  // THE assertion: the SAME contract on the SAME flat surface prices
  // differently at two forwards, because the corridor's log-moneyness image
  // ln(100/F) moved. A corridor frozen at inception -- or one specified in
  // moneyness rather than absolute strikes -- would make these EQUAL: the
  // surface is flat in k and the tenor identical, so nothing else here can
  // separate them.
  EXPECT_NE(q_low->fair_strike_dec, q_high->fair_strike_dec);
  // Direction: at F = 125 the barrier sits at k = ln(100/125) < 0, so the
  // corridor covers the ATM region and beyond and captures MORE variance than
  // at F = 80, where it starts at k = ln(100/80) > 0 and catches only the
  // upper wing.
  EXPECT_GT(q_high->fair_strike_dec, q_low->fair_strike_dec);
  const auto full_high = var_swap_fair_strike(surf, cs_high, 0.5, cfg);
  ASSERT_TRUE(full_high.has_value());
  EXPECT_LT(q_high->fair_strike_dec, full_high->fair_strike_dec);
}

// ── Task F-4: forward-start variance (PV-F4 / FIT-F2 / LIT-7) ────────────
//
// Coverage in this block: the three brief oracles (FlatSurfaceExact,
// TermStructureExact, NegativeForwardFailsLoud); the R4 detector threshold
// tested on BOTH sides of the fit accuracy floor; the R1 shared-policy proof
// (one config + one band reaching both legs, discriminating against a leg that
// ignored either); the R3 cancellation boundary; the R2 out-parameter contract
// on every return path; degenerate tenors including the +Inf case; and a
// fixture in which BOTH appended quote fields differ from every trivial value
// and from each other.
//
// FIXTURE NOTE (the F-3 make_flat_curves lesson): the builder below takes an
// explicit `rate` and puts it in the discount factor, the pricing context AND
// the pillar forwards (F = S*e^{rT}, so q_eff == 0 stays consistent). Every
// fixture here except the FlatSurfaceExact sweep runs at r = 4.3%, so nothing
// in this block is structurally blind to a rate the way a hard-coded r = 0
// helper would make it.

// eSSVI PricedSurface with an EXPLICIT per-pillar total variance. phi = 0 and
// rho = 0 make the eSSVI backbone w(k) = (theta/2)(1 + sqrt(1)) = theta for
// every k -- a perfectly flat smile -- so K_var(T_i) == theta_i/T_i up to the
// strip's own quadrature and every oracle below has a closed form. `phi`/`rho`
// are exposed for the one fixture that needs a genuine smile (the wing-band
// discrimination test, where a flat smile would make the clamp a no-op).
atx::vol::PricedSurface make_term_variance_priced_surface(
    std::uint32_t uid, double spot, double rate,
    const std::vector<std::pair<double, double>>& pillars, double phi = 0.0,
    double rho = 0.0) {
  atx::vol::CurveSurface cs;
  std::vector<atx::vol::SliceContext> ctx;
  std::uint16_t i = 0;
  for (const auto& tp : pillars) {
    const double T = tp.first;
    atx::vol::EssviParams e{};
    e.theta = tp.second;
    e.phi = phi;
    e.rho = rho;
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = spot * std::exp(rate * T);
    e.expiry_id = i;
    cs.push(std::make_unique<atx::vol::EssviCurve>(e, std::exp(-rate * T)));
    ctx.push_back(atx::vol::SliceContext{T, e.F, 0.0, 0.0, 250, 7});
    ++i;
  }
  atx::vol::PricingContext pc;
  pc.S = spot;
  pc.r = rate;
  pc.now_ts_ns = atx::vol::testkit::kFixtureNow;
  pc.method = atx::vol::AmericanMethod::AndersenLake;
  pc.al_opts = atx::vol::al_fast_opts();
  pc.uid = uid;
  return atx::vol::testkit::unwrap_surface(
      atx::vol::PricedSurface::create(std::move(cs), std::move(ctx), pc));
}

// Flat sigma, six pillars, every ordered tenor pair: K_fwd == sigma^2.
//
// TIER, AND THE BRIEF'S 1e-10. The tolerance is met at High and Audit but NOT
// at the Standard default: the strip's Simpson error in TOTAL variance is
// almost -- not exactly -- tenor-independent, so most of it cancels in
// (w2 - w1) and what survives is the drift. Measured on this fixture: the
// per-leg w error is 8.176e-12 at High and 7.93e-13 at Audit, constant across
// all six tenors to the digits printed, leaving a max |K_fwd - sigma^2| of
// 4.2e-17 (High) and 1.2e-13 (Audit) over the fifteen pairs; at Standard the
// w error drifts from 1.853e-10 (T=0.10) to 2.683e-10 (T=1.00) and the max
// residual is 1.51e-10, i.e. 1.5x past the brief's bar. Both tiers are
// asserted, each at what it actually achieves plus headroom.
TEST(ForwardVar, FlatSurfaceExact) {
  const double sigma = 0.20;
  const double sigma2 = sigma * sigma;
  const std::vector<double> Ts{0.10, 0.25, 0.35, 0.50, 0.75, 1.00};
  std::vector<std::pair<double, double>> pillars;
  for (const double T : Ts) {
    pillars.emplace_back(T, sigma2 * T);
  }
  const atx::vol::PricedSurface ps =
      make_term_variance_priced_surface(7401, 100.0, 0.043, pillars);

  struct Case {
    atx::vol::DerivQuality quality;
    double tol;
  };
  for (const Case c : {Case{atx::vol::DerivQuality::High, 1.0e-10},
                       Case{atx::vol::DerivQuality::Audit, 1.0e-10},
                       Case{atx::vol::DerivQuality::Standard, 5.0e-10}}) {
    DerivConfig cfg = deriv_default_config();
    cfg.quality = c.quality;
    for (std::size_t a = 0; a + 1 < Ts.size(); ++a) {
      for (std::size_t b = a + 1; b < Ts.size(); ++b) {
        const auto f = atx::vol::forward_var_fair_strike(ps, Ts[a], Ts[b], cfg);
        ASSERT_TRUE(f.has_value()) << f.error().to_string();
        EXPECT_NEAR(f->fair_strike_dec, sigma2, c.tol)
            << "quality=" << static_cast<int>(c.quality) << " T1=" << Ts[a] << " T2=" << Ts[b];
        // The forward TOTAL variance over the window, and the legs, come along.
        EXPECT_NEAR(f->uncapped_var_dec, sigma2 * (Ts[b] - Ts[a]), c.tol);
        EXPECT_NEAR(f->leg_T1_var_dec, sigma2, 1.0e-8);
        EXPECT_NEAR(f->leg_T2_var_dec, sigma2, 1.0e-8);
        EXPECT_FALSE(has_flag(f->flags, DerivFlags::CalendarInconsistent));
      }
    }
  }
}

// NOT AN EXACTNESS ORACLE ON ITS OWN -- read the block above. On a flat
// surface K_var(T1) == K_var(T2) == K_fwd == sigma^2, so FlatSurfaceExact
// CANNOT distinguish the two appended leg fields from each other, from
// `fair_strike_dec`, or from a build that simply copied one into the others.
// That is the F-2 blindness shape, and this is the fixture that closes it: a
// term structure on which all three numbers are DIFFERENT.
//
// w(T) is constructed linear in T between the outer pillars (theta = 0.010 /
// 0.0275 / 0.045 at T = 0.25 / 0.50 / 0.75), so the analytic forward variance
// is (0.045 - 0.010)/(0.75 - 0.25) = 0.07 over the whole window AND over every
// sub-window, including one whose endpoints are interpolated rather than
// pillars. K_var(0.25) = 0.04 and K_var(0.75) = 0.06 -- so the two legs and
// K_fwd are three distinct numbers, none of them 0, none NaN, and no two
// interchangeable.
TEST(ForwardVar, TermStructureExact) {
  DerivConfig cfg = deriv_default_config();
  cfg.quality = atx::vol::DerivQuality::High;
  const atx::vol::PricedSurface ps = make_term_variance_priced_surface(
      7410, 100.0, 0.043, {{0.25, 0.010}, {0.50, 0.0275}, {0.75, 0.045}});

  const std::vector<std::pair<double, double>> windows{
      {0.25, 0.75}, {0.25, 0.50}, {0.50, 0.75}, {0.30, 0.70}};
  for (const auto& w : windows) {
    const auto f = atx::vol::forward_var_fair_strike(ps, w.first, w.second, cfg);
    ASSERT_TRUE(f.has_value()) << f.error().to_string();
    EXPECT_NEAR(f->fair_strike_dec, 0.07, 1.0e-12)
        << "T1=" << w.first << " T2=" << w.second;
    // Total-variance additivity (LIT-7) as an identity on the served numbers.
    EXPECT_NEAR(f->leg_T1_var_dec * w.first + f->fair_strike_dec * (w.second - w.first),
                f->leg_T2_var_dec * w.second, 1.0e-13);
  }

  // The three-distinct-numbers assertion, on the full window.
  const auto f = atx::vol::forward_var_fair_strike(ps, 0.25, 0.75, cfg);
  ASSERT_TRUE(f.has_value());
  EXPECT_NEAR(f->leg_T1_var_dec, 0.04, 1.0e-9);
  EXPECT_NEAR(f->leg_T2_var_dec, 0.06, 1.0e-9);
  EXPECT_NE(f->leg_T1_var_dec, f->leg_T2_var_dec);
  EXPECT_NE(f->leg_T1_var_dec, f->fair_strike_dec);
  EXPECT_NE(f->leg_T2_var_dec, f->fair_strike_dec);
  EXPECT_NE(f->leg_T1_var_dec, 0.0);
  EXPECT_NE(f->leg_T2_var_dec, 0.0);
  EXPECT_FALSE(std::isnan(f->leg_T1_var_dec));
  EXPECT_FALSE(std::isnan(f->leg_T2_var_dec));

  // Each leg is BIT-IDENTICAL to a standalone strip at the same tenor under
  // the same config -- the legs are the strips, not a re-derivation of them.
  const auto l1 = var_swap_fair_strike(ps, 0.25, cfg);
  const auto l2 = var_swap_fair_strike(ps, 0.75, cfg);
  ASSERT_TRUE(l1.has_value());
  ASSERT_TRUE(l2.has_value());
  EXPECT_EQ(f->leg_T1_var_dec, l1->fair_strike_dec);
  EXPECT_EQ(f->leg_T2_var_dec, l2->fair_strike_dec);

  // Fields no forward-start entry populates keep their "not computed" NaN, and
  // a quote from any OTHER entry keeps NaN in the two appended leg fields.
  EXPECT_TRUE(std::isnan(f->conditional_corridor_var_dec));
  EXPECT_EQ(f->pv, 0.0);
  EXPECT_TRUE(std::isnan(l1->leg_T1_var_dec));
  EXPECT_TRUE(std::isnan(l1->leg_T2_var_dec));
}

// A theta-inverted surface -- the C-8 shape: total variance DECREASING in T,
// so the forward variance the caller asked for does not exist. Must fail loud,
// and the flag must reach the caller through the out-parameter (there is no
// quote on an Err to read it off).
TEST(ForwardVar, NegativeForwardFailsLoud) {
  DerivConfig cfg = deriv_default_config();
  cfg.quality = atx::vol::DerivQuality::High;
  // theta halves between the two pillars: w(0.75) = 0.007 < w(0.35) = 0.014.
  const atx::vol::PricedSurface inverted = make_term_variance_priced_surface(
      7420, 100.0, 0.043, {{0.35, 0.014}, {0.75, 0.007}});

  atx::vol::DerivQuote diag{};
  const auto f = atx::vol::forward_var_fair_strike(inverted, 0.35, 0.75, cfg, std::nullopt, &diag);
  ASSERT_FALSE(f.has_value());
  EXPECT_EQ(f.error().code(), ErrorCode::Internal);
  EXPECT_TRUE(has_flag(diag.flags, DerivFlags::CalendarInconsistent));
  // The error-path diagnostic carries the EVIDENCE, not a zeroed placeholder:
  // both legs, and the raw (negative) quotient rather than a clamped one.
  EXPECT_NEAR(diag.leg_T1_var_dec, 0.04, 1.0e-9);
  EXPECT_NEAR(diag.leg_T2_var_dec, 0.007 / 0.75, 1.0e-9);
  EXPECT_NEAR(diag.fair_strike_dec, (0.007 - 0.014) / 0.40, 1.0e-9);
  EXPECT_LT(diag.fair_strike_dec, 0.0);

  // The same surface read the other way round is perfectly fine, so the
  // failure is about the term structure and not about the fixture.
  const auto ok = var_swap_fair_strike(inverted, 0.75, cfg);
  ASSERT_TRUE(ok.has_value());
  EXPECT_FALSE(has_flag(ok->flags, DerivFlags::CalendarInconsistent));
}

// R4: the detector's threshold against the fit accuracy floor, BOTH SIDES.
//
// `kCalendarTotalVarianceTol` (arb.hpp, 1e-7 in total variance) is the bar the
// library's own fit-side calendar checks use, so a surface can PASS those and
// still carry a w-decrease that size. The detector's dead band is 2x that
// floor plus both legs' measured Richardson estimates -- 2.00016e-07 on this
// fixture at High tier, the quadrature terms contributing 1.6e-12 of it.
// Measured outcomes at drops of 1e-7 / 2e-7 / 4e-7 / 1e-6 / 1e-3: served as
// 0.0 / served as 0.0 / Internal / Internal / Internal.
TEST(ForwardVar, CalendarDetectorRespectsTheFitAccuracyFloor) {
  DerivConfig cfg = deriv_default_config();
  cfg.quality = atx::vol::DerivQuality::High;

  // (a) AT the fit floor: fit noise, not arbitrage. Must NOT fire, and must
  // serve exactly 0.0 rather than a small negative variance.
  const atx::vol::PricedSurface noisy = make_term_variance_priced_surface(
      7430, 100.0, 0.043,
      {{0.35, 0.014}, {0.75, 0.014 - atx::vol::kCalendarTotalVarianceTol}});
  atx::vol::DerivQuote noisy_diag{};
  const auto q_noisy =
      atx::vol::forward_var_fair_strike(noisy, 0.35, 0.75, cfg, std::nullopt, &noisy_diag);
  ASSERT_TRUE(q_noisy.has_value()) << q_noisy.error().to_string();
  EXPECT_EQ(q_noisy->fair_strike_dec, 0.0);
  EXPECT_EQ(q_noisy->uncapped_var_dec, 0.0);
  EXPECT_FALSE(has_flag(q_noisy->flags, DerivFlags::CalendarInconsistent));
  // The raw numerator IS negative -- so this is a real inversion being
  // absorbed, not a fixture that failed to invert.
  EXPECT_LT(q_noisy->leg_T2_var_dec * 0.75 - q_noisy->leg_T1_var_dec * 0.35, 0.0);
  // ... and it is the DEAD BAND absorbing it, at the size the floor predicts.
  EXPECT_NEAR(q_noisy->integration_error_est * 0.40, 2.0e-7, 1.0e-9);

  // (b) 4x the fit floor: past anything either leg's accuracy explains.
  const atx::vol::PricedSurface arbed = make_term_variance_priced_surface(
      7431, 100.0, 0.043,
      {{0.35, 0.014}, {0.75, 0.014 - 4.0 * atx::vol::kCalendarTotalVarianceTol}});
  atx::vol::DerivQuote arb_diag{};
  const auto q_arb =
      atx::vol::forward_var_fair_strike(arbed, 0.35, 0.75, cfg, std::nullopt, &arb_diag);
  ASSERT_FALSE(q_arb.has_value());
  EXPECT_EQ(q_arb.error().code(), ErrorCode::Internal);
  EXPECT_TRUE(has_flag(arb_diag.flags, DerivFlags::CalendarInconsistent));
  EXPECT_NEAR(arb_diag.fair_strike_dec, -4.0e-7 / 0.40, 1.0e-11);
}

// R1: ONE config resolution and ONE certified band reach BOTH legs, while the
// GRID is resolved per tenor. Needs a genuine smile (phi > 0, rho < 0), since
// on a flat smile the wing clamp moves nothing and the test would pass for a
// build that dropped the band entirely.
//
// Measured on this fixture at High tier: the T1 = 0.10 leg resolves 769 nodes
// over +-2.0 and the T2 = 1.00 leg 1269 nodes over +-3.3 (6*sigma*sqrt(T)
// widening bites only on the long leg), so per-tenor budgets are real here and
// not an untested claim.
TEST(ForwardVar, SharedPolicyReachesBothLegsWhileGridsResolvePerTenor) {
  DerivConfig cfg = deriv_default_config();
  cfg.quality = atx::vol::DerivQuality::High;
  const double sigma = 0.55;
  const atx::vol::PricedSurface ps = make_term_variance_priced_surface(
      7440, 100.0, 0.043, {{0.10, sigma * sigma * 0.10}, {1.00, sigma * sigma * 1.00}},
      /*phi=*/1.5, /*rho=*/-0.4);

  const auto l1 = var_swap_fair_strike(ps, 0.10, cfg);
  const auto l2 = var_swap_fair_strike(ps, 1.00, cfg);
  ASSERT_TRUE(l1.has_value());
  ASSERT_TRUE(l2.has_value());
  EXPECT_NE(l1->strip_nodes_used, l2->strip_nodes_used);
  EXPECT_NE(l1->strip_k_hi_used, l2->strip_k_hi_used);

  const auto wide = atx::vol::forward_var_fair_strike(ps, 0.10, 1.00, cfg);
  ASSERT_TRUE(wide.has_value()) << wide.error().to_string();
  EXPECT_EQ(wide->leg_T1_var_dec, l1->fair_strike_dec);
  EXPECT_EQ(wide->leg_T2_var_dec, l2->fair_strike_dec);
  // Grid provenance is documented as the T2 leg's.
  EXPECT_EQ(wide->strip_nodes_used, l2->strip_nodes_used);
  EXPECT_EQ(wide->strip_k_hi_used, l2->strip_k_hi_used);

  // A NARROWER certified band must move BOTH legs. A build that resolved the
  // band once and applied it to one leg only would leave the other equal to
  // its wide-band value, so each of these two EXPECT_NEs is a separate,
  // discriminating witness that this leg saw the shared band.
  const auto narrow = atx::vol::forward_var_fair_strike(ps, 0.10, 1.00, cfg,
                                                        std::optional<double>{0.35});
  ASSERT_TRUE(narrow.has_value()) << narrow.error().to_string();
  EXPECT_NE(narrow->leg_T1_var_dec, wide->leg_T1_var_dec);
  EXPECT_NE(narrow->leg_T2_var_dec, wide->leg_T2_var_dec);
  EXPECT_NE(narrow->fair_strike_dec, wide->fair_strike_dec);
  // ... and each narrow-band leg equals a standalone strip at that same band,
  // which pins WHICH band reached it rather than merely that something moved.
  const auto n1 = var_swap_fair_strike(ps, 0.10, cfg, std::optional<double>{0.35});
  const auto n2 = var_swap_fair_strike(ps, 1.00, cfg, std::optional<double>{0.35});
  ASSERT_TRUE(n1.has_value());
  ASSERT_TRUE(n2.has_value());
  EXPECT_EQ(narrow->leg_T1_var_dec, n1->fair_strike_dec);
  EXPECT_EQ(narrow->leg_T2_var_dec, n2->fair_strike_dec);

  // The same argument for the OTHER shared knobs: a config change has to move
  // both legs, never one. `width_sigmas < 0` turns vol scaling off, which
  // shrinks the long leg's span back to the tier default.
  DerivConfig no_scale = cfg;
  no_scale.width_sigmas = -1.0;
  const auto unscaled = atx::vol::forward_var_fair_strike(ps, 0.10, 1.00, no_scale);
  ASSERT_TRUE(unscaled.has_value()) << unscaled.error().to_string();
  EXPECT_NE(unscaled->leg_T2_var_dec, wide->leg_T2_var_dec);
}

// The documented grid-provenance rule, on its own: a forward-start quote
// reports the T2 LEG's grid, because the two legs genuinely resolve different
// ones and there is no single grid to report.
//
// This assertion is ALSO the override witness for the out-of-tree reversion
// probes (Task F-4 report, non-vacuity section). It is deliberately isolated
// in its own test and depends on NONE of the behaviours those probes revert,
// so "the substituted translation unit is the one linked" and "the reverted
// behaviour was detected" are two independent observations rather than one
// circular one. It is a value-level witness, not an error string: this sprint
// has twice had a string sentinel go identical or unreachable between rounds.
TEST(ForwardVar, ProbeWitnessGridProvenanceIsTheT2Leg) {
  DerivConfig cfg = deriv_default_config();
  cfg.quality = atx::vol::DerivQuality::High;
  const double sigma = 0.55;
  const atx::vol::PricedSurface ps = make_term_variance_priced_surface(
      7480, 100.0, 0.043, {{0.10, sigma * sigma * 0.10}, {1.00, sigma * sigma * 1.00}},
      /*phi=*/1.5, /*rho=*/-0.4);
  const auto l1 = var_swap_fair_strike(ps, 0.10, cfg);
  const auto l2 = var_swap_fair_strike(ps, 1.00, cfg);
  ASSERT_TRUE(l1.has_value());
  ASSERT_TRUE(l2.has_value());
  // The two legs' node counts must actually DIFFER, or this witness could not
  // tell them apart and would silently stop witnessing anything.
  ASSERT_NE(l1->strip_nodes_used, l2->strip_nodes_used);

  const auto f = atx::vol::forward_var_fair_strike(ps, 0.10, 1.00, cfg);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  EXPECT_EQ(f->strip_nodes_used, l2->strip_nodes_used);
  EXPECT_NE(f->strip_nodes_used, l1->strip_nodes_used);
}

// R3: the cancellation guard. The numerator differences two nearly-equal total
// variances and the denominator vanishes, so the noise floor is amplified by
// 1/(T2 - T1). Measured at Audit tier on a flat sigma = 0.20 surface: the
// floor is 2.0000e-07 (the quadrature terms contribute ~1e-12), so the gate at
// `kFwdVarNoiseCeilingVar` = 1e-3 bites at dT = 2.0e-4 years -- about 1.75
// hours. dT = 1e-4 refuses (noise 2.0e-3), dT = 1e-3 serves (noise 2.0e-4, and
// the served K_fwd is 0.040000000000095, i.e. 9.5e-14 from truth).
//
// The refusal is DELIBERATELY conservative: 2e-7 of that floor is the
// library's stated calendar accuracy, which this synthetic fixture beats by
// five orders of magnitude. The gate cannot know that, and must not assume it.
TEST(ForwardVar, TenorSeparationBelowResolutionRefuses) {
  DerivConfig cfg = deriv_default_config();
  cfg.quality = atx::vol::DerivQuality::Audit;
  const double sigma2 = 0.04;
  const atx::vol::PricedSurface ps = make_term_variance_priced_surface(
      7450, 100.0, 0.043,
      {{0.50, sigma2 * 0.50}, {0.5001, sigma2 * 0.5001}, {0.501, sigma2 * 0.501},
       {0.51, sigma2 * 0.51}, {0.75, sigma2 * 0.75}});

  atx::vol::DerivQuote tight_diag{};
  const auto tight =
      atx::vol::forward_var_fair_strike(ps, 0.50, 0.5001, cfg, std::nullopt, &tight_diag);
  ASSERT_FALSE(tight.has_value());
  EXPECT_EQ(tight.error().code(), ErrorCode::OutOfRange);
  // The diagnostic still carries both legs on this path -- they were priced.
  EXPECT_FALSE(std::isnan(tight_diag.leg_T1_var_dec));
  EXPECT_FALSE(std::isnan(tight_diag.leg_T2_var_dec));
  EXPECT_GT(tight_diag.integration_error_est, atx::vol::kFwdVarNoiseCeilingVar);

  const auto ok = atx::vol::forward_var_fair_strike(ps, 0.50, 0.501, cfg);
  ASSERT_TRUE(ok.has_value()) << ok.error().to_string();
  EXPECT_NEAR(ok->fair_strike_dec, sigma2, 1.0e-11);
  EXPECT_LE(ok->integration_error_est, atx::vol::kFwdVarNoiseCeilingVar);
}

// Degenerate tenors. Note +Inf specifically: `T > 0.0` ADMITS it, so the
// finiteness test has to run first, and the code must be InvalidArgument
// rather than the OutOfRange a fitted-range gate would have produced.
TEST(ForwardVar, DegenerateTenorsRejected) {
  DerivConfig cfg = deriv_default_config();
  const atx::vol::PricedSurface ps = make_term_variance_priced_surface(
      7460, 100.0, 0.043, {{0.25, 0.010}, {0.75, 0.045}});
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();

  const std::vector<std::pair<double, double>> bad{
      {0.0, 0.75},    // T1 == 0
      {-0.25, 0.75},  // T1 < 0
      {0.25, 0.25},   // T2 == T1
      {0.75, 0.25},   // T2 < T1
      {nan, 0.75},   {0.25, nan}, {inf, 0.75}, {0.25, inf}, {-inf, 0.75}, {0.25, -inf},
  };
  for (const auto& p : bad) {
    atx::vol::DerivQuote diag{};
    diag.fair_strike_dec = 12345.0;  // pre-dirtied: must be overwritten
    const auto f =
        atx::vol::forward_var_fair_strike(ps, p.first, p.second, cfg, std::nullopt, &diag);
    ASSERT_FALSE(f.has_value()) << "T1=" << p.first << " T2=" << p.second;
    EXPECT_EQ(f.error().code(), ErrorCode::InvalidArgument)
        << "T1=" << p.first << " T2=" << p.second;
    // Out-parameter contract on a path that never priced a leg: reset, not
    // left holding the caller's stale bytes.
    EXPECT_EQ(diag.fair_strike_dec, 0.0);
    EXPECT_TRUE(std::isnan(diag.leg_T1_var_dec));
    EXPECT_EQ(diag.flags, DerivFlags::None);
  }

  // Both tenors are gated against the fitted pillar range, by ONE resolution.
  EXPECT_EQ(atx::vol::forward_var_fair_strike(ps, 0.10, 0.75, cfg).error().code(),
            ErrorCode::OutOfRange);  // T1 below the front pillar
  EXPECT_EQ(atx::vol::forward_var_fair_strike(ps, 0.25, 1.50, cfg).error().code(),
            ErrorCode::OutOfRange);  // T2 past the back pillar
}

// R2: the out-parameter is the ONLY channel for an error-path flag, so its
// contract ("assigned on every return path") is itself load-bearing. nullptr
// must also be safe -- it is the default.
TEST(ForwardVar, DiagnosticOutIsWrittenOnEveryReturnPath) {
  DerivConfig cfg = deriv_default_config();
  cfg.quality = atx::vol::DerivQuality::High;
  const atx::vol::PricedSurface good = make_term_variance_priced_surface(
      7470, 100.0, 0.043, {{0.25, 0.010}, {0.75, 0.045}});
  const atx::vol::PricedSurface bad = make_term_variance_priced_surface(
      7471, 100.0, 0.043, {{0.25, 0.045}, {0.75, 0.010}});

  // Success path: the diagnostic equals the returned quote field for field.
  atx::vol::DerivQuote diag{};
  const auto ok = atx::vol::forward_var_fair_strike(good, 0.25, 0.75, cfg, std::nullopt, &diag);
  ASSERT_TRUE(ok.has_value()) << ok.error().to_string();
  EXPECT_EQ(diag.fair_strike_dec, ok->fair_strike_dec);
  EXPECT_EQ(diag.leg_T1_var_dec, ok->leg_T1_var_dec);
  EXPECT_EQ(diag.leg_T2_var_dec, ok->leg_T2_var_dec);
  EXPECT_EQ(diag.flags, ok->flags);

  // Calendar-failure path: this is the case the flag exists for, and the only
  // way a caller can see it.
  atx::vol::DerivQuote fail_diag{};
  const auto err =
      atx::vol::forward_var_fair_strike(bad, 0.25, 0.75, cfg, std::nullopt, &fail_diag);
  ASSERT_FALSE(err.has_value());
  EXPECT_TRUE(has_flag(fail_diag.flags, DerivFlags::CalendarInconsistent));

  // nullptr: same status, no crash, and the flag is simply not delivered.
  const auto err_null = atx::vol::forward_var_fair_strike(bad, 0.25, 0.75, cfg);
  ASSERT_FALSE(err_null.has_value());
  EXPECT_EQ(err_null.error().code(), err.error().code());
  const auto ok_null = atx::vol::forward_var_fair_strike(good, 0.25, 0.75, cfg);
  ASSERT_TRUE(ok_null.has_value());
  EXPECT_EQ(ok_null->fair_strike_dec, ok->fair_strike_dec);
}

// The templated sibling over the Tier-A container plus an explicit CurveSet.
// One CurveSet serves both tenors there, exactly as one carry does on the
// PricedSurface path.
TEST(ForwardVar, TemplatedSiblingSharesTheSameRule) {
  const double sigma = 0.20;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const atx::vol::VolSurface tier_a = make_flat_vol_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivConfig cfg = deriv_default_config();
  cfg.quality = atx::vol::DerivQuality::High;

  atx::vol::DerivQuote diag{};
  const auto f =
      atx::vol::forward_var_fair_strike(surf, cs, 0.25, 0.75, cfg, &diag);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  const auto l1 = var_swap_fair_strike(surf, cs, 0.25, cfg);
  const auto l2 = var_swap_fair_strike(surf, cs, 0.75, cfg);
  ASSERT_TRUE(l1.has_value());
  ASSERT_TRUE(l2.has_value());
  EXPECT_EQ(f->leg_T1_var_dec, l1->fair_strike_dec);
  EXPECT_EQ(f->leg_T2_var_dec, l2->fair_strike_dec);
  EXPECT_DOUBLE_EQ(f->fair_strike_dec,
                   (l2->fair_strike_dec * 0.75 - l1->fair_strike_dec * 0.25) / 0.50);
  EXPECT_NEAR(f->fair_strike_dec, sigma * sigma, 1.0e-3);
  EXPECT_EQ(diag.fair_strike_dec, f->fair_strike_dec);

  // The Tier-A instantiation links and agrees with the demoted container.
  const auto f_tier_a = atx::vol::forward_var_fair_strike(tier_a, cs, 0.25, 0.75, cfg);
  ASSERT_TRUE(f_tier_a.has_value()) << f_tier_a.error().to_string();
  EXPECT_NEAR(f_tier_a->fair_strike_dec, f->fair_strike_dec, 1.0e-12);

  // Argument validation is the SAME rule on this path (one helper, two
  // callers), not a second copy that could drift.
  EXPECT_EQ(
      atx::vol::forward_var_fair_strike(surf, cs, 0.75, 0.25, cfg).error().code(),
      ErrorCode::InvalidArgument);
  EXPECT_EQ(atx::vol::forward_var_fair_strike(surf, cs, std::numeric_limits<double>::infinity(),
                                              0.75, cfg)
                .error()
                .code(),
            ErrorCode::InvalidArgument);
}

}  // namespace
