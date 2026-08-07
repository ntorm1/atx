#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
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
      EXPECT_LE(worst_panel_dk(*q, 0.5), dk_max)
          << "tier=" << static_cast<int>(tier) << " s=" << s << " T=" << T_test
          << " n=" << q->strip_nodes_used;
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

// Review fix I-2 (C-5): price_vol_swap's unaged branch used to hardcode
// `flags = DerivFlags::VolCarrLee` and never OR in the strip's own
// provenance. Harmless before C-5 (that strip was a pure best-effort
// diagnostic the price never depended on); under CarrLeeForm::Refined the
// strip now FEEDS the price, so a caller gating on WingClamped/StripTruncated*
// /LowT (the pattern this file establishes everywhere else a strip runs)
// must see it here too. Same steep-wing fixture as WingClamp.
// FlagPropagatesThroughDerivPrice, but VolSwap/unaged/Refined instead of
// VarSwap: Naive never runs a strip on this path at all (no signal to
// propagate, flag correctly absent), Refined must carry it through.
TEST(CarrLee, RefinedPropagatesStripFlagsThroughDerivPrice) {
  const EssviSurface surf = make_steep_wing_surface(0.30, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = 0.25;
  c.strike_dec = 0.30;
  c.notional = 1.0;
  c.rv_spec.n_obs_total = 63;  // unaged (n_obs_done defaults to 0)

  DerivConfig naive_cfg = deriv_default_config();
  naive_cfg.carr_lee_form = atx::vol::CarrLeeForm::Naive;
  const auto naive_q = deriv_price(surf, cs, c, naive_cfg);
  ASSERT_TRUE(naive_q.has_value());
  EXPECT_FALSE(has_flag(naive_q->flags, DerivFlags::WingClamped));

  DerivConfig refined_cfg = deriv_default_config();
  refined_cfg.carr_lee_form = atx::vol::CarrLeeForm::Refined;
  const auto refined_q = deriv_price(surf, cs, c, refined_cfg);
  ASSERT_TRUE(refined_q.has_value());
  EXPECT_TRUE(has_flag(refined_q->flags, DerivFlags::WingClamped));
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

}  // namespace
