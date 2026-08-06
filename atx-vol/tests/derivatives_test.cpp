#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "atx/vol/black76.hpp"              // WingClamp oracle repricing
#include "atx/vol/derivatives.hpp"
#include "atx/vol/detail/strip_grid.hpp"    // strip::simpson_weight (WingClamp oracle)
#include "atx/vol/detail/legacy_surface.hpp"  // EssviSurface (demoted, S4-T21)
#include "atx/vol/priced_surface.hpp"  // E6: PricedSurface-native overloads
#include "atx/vol/rates_curve.hpp"
#include "atx/vol/surface.hpp"
#include "atx/vol/vol_surface.hpp" // Tier-A instantiation set (closeout 1.2)
#include "deriv_fixtures.hpp" // Task 0: deriv_testkit::make_curves / MC oracle
#include "support/analytics_fixture.hpp" // E6: testkit::make_flat_surface

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

// Hand Simpson replication of the strip with vol reads clamped to [-band, band]
// on the exact grid the quote reports. Same quadrature convention on purpose:
// the assertion is about WHICH vol each node reads, not about quadrature.
double clamped_strip_oracle(const EssviSurface& surf, const CurveSet& cs, double T,
                            const atx::vol::DerivQuote& grid_src, double band) {
  const double F = cs.spot;          // flat curves: F == spot at every pillar
  const double df = cs.yield.disc(T); // zero rates: 1.0
  const std::size_t n = grid_src.strip_nodes_used;
  const double k_lo = grid_src.strip_k_lo_used;
  const double k_hi = grid_src.strip_k_hi_used;
  const double dx = (k_hi - k_lo) / static_cast<double>(n - 1);
  double integral = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double x = k_lo + dx * static_cast<double>(i);
    const double K = F * std::exp(x);
    const double x_read = std::clamp(x, -band, band);
    const double sigma = surf.iv(x_read, T);
    const double price = atx::vol::black76_price(F, K, T, sigma, df,
                                                 x < 0.0 ? atx::vol::Side::Put
                                                         : atx::vol::Side::Call);
    integral += atx::vol::strip::simpson_weight(i, n) * price / (df * K);
  }
  integral *= dx / 3.0;
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

}  // namespace
