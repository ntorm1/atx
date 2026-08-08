#include <gtest/gtest.h>

#include <cmath>

#include "atx/vol/rates_curve.hpp"
#include "atx/vol/derivatives.hpp"
#include "atx/vol/detail/legacy_surface.hpp"  // EssviSurface (demoted, S4-T21)
#include "atx/vol/priced_surface.hpp"     // PricedSurface-native greeks overload
#include "atx/vol/surface.hpp"
#include "support/analytics_fixture.hpp"  // testkit::make_flat_surface (PricedSurface)
#include "support/deriv_test_fixture.hpp" // testsupport::make_flat_surface / make_flat_curves

// Task 7: finite-difference greeks for every vol-derivative kind. Every bump
// reprices through `deriv_price`, so a product/age/cap regime gets its greeks
// from exactly the path that produced its mark.

namespace {

using atx::vol::CurveSet;
using atx::vol::deriv_default_config;
using atx::vol::deriv_greeks;
using atx::vol::deriv_price;
using atx::vol::DerivConfig;
using atx::vol::DerivContract;
using atx::vol::DerivEngine;
using atx::vol::DerivGreekBumps;
using atx::vol::DerivKind;
using atx::vol::ErrorCode;
using atx::vol::EssviSlice;
using atx::vol::EssviSurface;
using atx::vol::testsupport::make_flat_curves;
using atx::vol::testsupport::make_flat_surface;

// Skewed eSSVI: rho < 0 (downside skew), phi > 0 (curvature). theta is
// proportional to T, so the ATM vol is a flat 20 across the term structure and
// only the SMILE shape drives the spot greeks.
EssviSurface make_skewed_surface() {
  EssviSurface surf(2);
  const EssviSlice s0{0.04 * 0.01, 1.5, -0.6, 0.01};
  const EssviSlice s1{0.04 * 1.00, 1.5, -0.6, 1.00};
  EXPECT_TRUE(surf.set_slice(0, s0).has_value());
  EXPECT_TRUE(surf.set_slice(1, s1).has_value());
  return surf;
}

// An unaged 1e6-notional var swap maturing at `T`.
DerivContract var_swap_at(double T) {
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  return c;
}

// Var swap on a FLAT surface: analytic truths.
//   vega  = dK_var/dsigma * w_future * df * N = 2*sigma * 1 * df * N
//   delta = 0 (no skew, sticky-strike)   gamma ~ 0
//   rho: PV(K != fair) discounts, d(df)/dr = -T*df
TEST(DerivGreeks, VarSwapFlatSurfaceAnalyticTruths) {
  const double sigma = 0.20, T = 0.25, N = 1e6;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T;
  c.notional = N;
  c.strike_dec = 0.02;  // off-fair so rho has something to discount
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  const double df = cs.yield.disc(T);
  EXPECT_NEAR(g->vega, 2.0 * sigma * df * N, 2e-2 * 2.0 * sigma * df * N);
  EXPECT_NEAR(g->delta * 100.0 / (N), 0.0, 1e-3);  // per-spot units, flat => ~0
  EXPECT_NEAR(g->rho, -T * g->pv, 5e-3 * std::fabs(-T * g->pv) + 1e-6);
  EXPECT_TRUE(std::isfinite(g->gamma));
  EXPECT_TRUE(std::isfinite(g->theta));
  // theta of an off-fair var swap on a flat surface: future K_var is
  // T-independent, so d/dt only hits the discount: theta ~ r*pv = 0 here (r=0).
  EXPECT_NEAR(g->theta, 0.0, 1e-2 * std::fabs(g->pv) + 1.0);
}

// Skewed surface: delta must be nonzero and negative for a long var swap under
// sticky-strike with a negative skew (down-moves ride up the smile).
TEST(DerivGreeks, VarSwapSkewGivesNonzeroDelta) {
  EssviSurface surf(2);
  // rho < 0 skew, phi > 0 curvature
  const EssviSlice s0{0.04 * 0.01, 1.5, -0.6, 0.01};
  const EssviSlice s1{0.04 * 1.00, 1.5, -0.6, 1.00};
  ASSERT_TRUE(surf.set_slice(0, s0).has_value());
  ASSERT_TRUE(surf.set_slice(1, s1).has_value());
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.25;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value());
  EXPECT_LT(g->delta, 0.0);
  EXPECT_GT(std::fabs(g->delta) * 100.0, 1.0);  // economically visible
}

// FD self-consistency: greeks must reproduce a direct large-bump repricing.
//
// The reference is a CENTRAL large-bump difference, not a one-sided one. On
// this surface PV(sigma) = N*sigma^2 exactly, so a one-sided +1-vol reference
// evaluates to N*(2*sigma + 0.01) -- it carries its OWN O(h) bias of N*h =
// 10,000, i.e. 2.5% of the 400,000 truth (measured, not assumed). That would
// make the test assert the reference's discretization error rather than the
// greek's. Centering the reference cancels the O(h) term and leaves O(h^2),
// which is what makes "the greek reproduces a direct repricing" a real claim.
TEST(DerivGreeks, VegaMatchesDirectReprice) {
  const double sigma = 0.20, T = 0.25;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const EssviSurface surf_up = make_flat_surface(sigma + 0.01, 0.01, 1.00);
  const EssviSurface surf_dn = make_flat_surface(sigma - 0.01, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  const auto g = deriv_greeks(surf, cs, c);
  const auto p1 = deriv_price(surf_up, cs, c, deriv_default_config());
  const auto pm1 = deriv_price(surf_dn, cs, c, deriv_default_config());
  ASSERT_TRUE(g.has_value());
  ASSERT_TRUE(p1.has_value());
  ASSERT_TRUE(pm1.has_value());
  const double fd = (p1->pv - pm1->pv) / 0.02;
  EXPECT_NEAR(g->vega, fd, 2e-2 * std::fabs(fd));
}

// Fully aged: pure discounting, all market greeks exactly zero.
TEST(DerivGreeks, FullyAgedHasOnlyRho) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = 0.0;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value());
  EXPECT_EQ(g->delta, 0.0);
  EXPECT_EQ(g->gamma, 0.0);
  EXPECT_EQ(g->vega, 0.0);
  EXPECT_EQ(g->volga, 0.0);
  EXPECT_EQ(g->vanna, 0.0);
  EXPECT_EQ(g->theta, 0.0);
}

// Every product kind produces finite greeks mid-life (the full matrix).
TEST(DerivGreeks, AllKindsMidLifeFinite) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  for (const DerivKind kind : {DerivKind::VarSwap, DerivKind::VolSwap,
                               DerivKind::CappedVarSwap, DerivKind::CappedVolSwap}) {
    DerivContract c{};
    c.kind = kind;
    c.maturity_t = 0.10;
    c.notional = 1e5;
    c.strike_dec = 0.03;
    c.cap_dec = (kind == DerivKind::CappedVarSwap)   ? 0.25
                : (kind == DerivKind::CappedVolSwap) ? 0.50
                                                     : 0.0;
    c.rv_spec.annualization = 252.0;
    c.rv_spec.n_obs_total = 63u;
    c.rv_spec.n_obs_done = 21u;
    c.rv_spec.rv_done_dec = 0.05;
    const auto g = deriv_greeks(surf, cs, c);
    ASSERT_TRUE(g.has_value()) << static_cast<int>(kind);
    for (const double v : {g->pv, g->delta, g->gamma, g->vega, g->volga,
                           g->vanna, g->theta, g->rho, g->charm}) {
      EXPECT_TRUE(std::isfinite(v)) << static_cast<int>(kind);
    }
  }
}

// PricedSurface-native overload works end to end.
TEST(DerivGreeks, PricedSurfaceOverload) {
  const atx::vol::PricedSurface ps =
      atx::vol::testkit::make_flat_surface(9, 100.0, 100.0, 0.30);
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = 0.35;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 88u;
  const auto g = atx::vol::deriv_greeks(ps, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  // The fixture surface is flat at 30 vol and carries kFixtureRate, so the
  // analytic var-swap vega is 2*sigma*df*N with df read off that same rate.
  const double df = std::exp(-atx::vol::testkit::kFixtureRate * 0.35);
  const double vega_expected = 2.0 * 0.30 * 1e6 * df;
  EXPECT_NEAR(g->vega, vega_expected, 0.05 * vega_expected);
}

// Fully aged with time still to run: PV(t) = e^{-r(T-t)}*X is a pure discount,
// so the two time greeks are analytic AND mutually consistent -- rho = -T*PV
// and theta = r*PV are one identity differentiated two ways. A fast path that
// returned theta = 0 alongside rho = -T*PV would be self-contradictory: a PV
// that discounts must also accrete.
TEST(DerivGreeks, FullyAgedWithRateAccretesAtTheCarry) {
  const double r = 0.043, T = 0.25;
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, r);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;  // sqrt = 0.21, so the swap is 3 vols in the money
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  ASSERT_GT(g->pv, 0.0);  // an in-the-money leg, so the identities have signal
  EXPECT_NEAR(g->theta, r * g->pv, 1e-9 * std::fabs(r * g->pv));
  EXPECT_DOUBLE_EQ(g->rho, -T * g->pv);
  EXPECT_EQ(g->delta, 0.0);
  EXPECT_EQ(g->gamma, 0.0);
  EXPECT_EQ(g->vega, 0.0);
  EXPECT_EQ(g->volga, 0.0);
  EXPECT_EQ(g->vanna, 0.0);
  EXPECT_EQ(g->charm, 0.0);
}

// Fully aged AND past its own maturity marker (an expired lot not yet rolled
// off the book). rho = -T*PV must not sign-flip on negative T: clamp T to 0
// so an already-settled lot reports zero rate sensitivity instead of
// -(-0.01)*PV, which would fabricate a small positive rho out of a lot that
// has nothing left to discount. (PV-9)
TEST(DerivGreeks, FullyAgedNegativeMaturityClampsRhoToZero) {
  const double r = 0.043;
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, r);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = -0.01;  // expired lot
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;  // sqrt = 0.21, so pv != 0 -- a real claim
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  ASSERT_NE(g->pv, 0.0);
  EXPECT_EQ(g->rho, 0.0);
}

// High-vol regime: sigma*sqrt(T) = 0.35 > 0.25, so the E2 adaptive-wing rescale
// is ACTIVE and its node count is a ceil() of a vol-dependent quantity. Without
// pinning the center's grid, a bumped evaluation can land on a different node
// count than the center and the second-order stencils then difference a step in
// the quadrature rather than a change in the price -- which shows up as a
// wildly inflated gamma on a surface whose PV does not depend on spot at all.
TEST(DerivGreeks, HighVolRegimeGridPinKeepsSecondOrderSane) {
  const double sigma = 0.35, T = 1.0, N = 1e5, S = 100.0;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(S, 0.01, 1.00);
  for (const DerivKind kind : {DerivKind::VarSwap, DerivKind::VolSwap,
                               DerivKind::CappedVarSwap, DerivKind::CappedVolSwap}) {
    DerivContract c{};
    c.kind = kind;
    c.maturity_t = T;
    c.notional = N;
    c.strike_dec = 0.03;
    c.cap_dec = (kind == DerivKind::CappedVarSwap)   ? 0.25
                : (kind == DerivKind::CappedVolSwap) ? 0.50
                                                     : 0.0;
    c.rv_spec.annualization = 252.0;
    c.rv_spec.n_obs_total = 63u;
    c.rv_spec.n_obs_done = 21u;
    c.rv_spec.rv_done_dec = 0.05;
    const auto g = deriv_greeks(surf, cs, c);
    ASSERT_TRUE(g.has_value()) << static_cast<int>(kind);
    for (const double v : {g->pv, g->delta, g->gamma, g->vega, g->volga,
                           g->vanna, g->theta, g->rho, g->charm}) {
      EXPECT_TRUE(std::isfinite(v)) << static_cast<int>(kind);
    }
    // The strip must report the grid it used, or there is nothing to pin. The
    // measured grid here is 361 nodes over [-2.1, 2.1] -- NOT the Standard tier
    // default of 257 over [-1.5, 1.5] -- which is the proof that this contract
    // really does exercise the adaptive rescale this test exists to guard.
    EXPECT_GT(g->quote.strip_nodes_used, 0u) << static_cast<int>(kind);
    EXPECT_TRUE(std::isfinite(g->quote.strip_k_lo_used)) << static_cast<int>(kind);
    EXPECT_TRUE(std::isfinite(g->quote.strip_k_hi_used)) << static_cast<int>(kind);
    EXPECT_GT(g->quote.strip_nodes_used, 257u) << static_cast<int>(kind);

    // CONTAMINATION TRIPWIRE. On a flat surface the strip is scale-invariant
    // and Carr-Lee's K_vol is forward-independent, so PV does not depend on
    // spot at all for ANY of the four kinds: the true gamma and vanna are
    // EXACTLY zero and every nonzero digit is numerical.
    //
    // Bound derivation: measured artifacts are |gamma| <= 2.2e-07 and |vanna|
    // <= 6.9e-07, which is the expected cancellation floor (three PVs of ~1.2e4
    // differenced, ~2.7e-12 of ULP noise, divided by ds^2 = 1e-4). A node-count
    // flip between bumped evaluations moves K_var by roughly the strip's own
    // Richardson error and lands gamma near 1e-1 -- five orders ABOVE this
    // bound. 1e-3 therefore sits ~4 orders above the noise (so FP evaluation
    // order, incl. the scalar-vs-AVX2 CI legs, cannot trip it) and ~2 orders
    // below the artifact it is built to catch.
    EXPECT_LT(std::fabs(g->gamma), 1.0e-3) << static_cast<int>(kind);
    EXPECT_LT(std::fabs(g->vanna), 1.0e-3) << static_cast<int>(kind);
  }
}

// second_order off: vanna and charm are the only greeks with evaluations of
// their own, so they go NaN; gamma and volga ride the first-order stencils and
// must survive.
TEST(DerivGreeks, SecondOrderOffLeavesOnlyVannaAndCharmNaN) {
  const EssviSurface surf = make_skewed_surface();
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivContract c = var_swap_at(0.25);
  DerivGreekBumps bumps{};
  bumps.second_order = false;
  const auto g = deriv_greeks(surf, cs, c, deriv_default_config(), bumps);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(std::isnan(g->vanna));
  EXPECT_TRUE(std::isnan(g->charm));
  EXPECT_TRUE(std::isfinite(g->delta));
  EXPECT_TRUE(std::isfinite(g->gamma));
  EXPECT_TRUE(std::isfinite(g->vega));
  EXPECT_TRUE(std::isfinite(g->volga));
  EXPECT_TRUE(std::isfinite(g->theta));
  EXPECT_TRUE(std::isfinite(g->rho));
}

// A contract too close to expiry to roll: the theta stencil would land at or
// past T = 0, where an unaged var swap has no future leg to price. Report those
// two as not-computed rather than failing the whole block.
TEST(DerivGreeks, RollPastExpiryLeavesThetaAndCharmNaN) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivGreekBumps bumps{};
  const DerivContract c = var_swap_at(0.5 * bumps.time_years);  // half a day out
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(std::isnan(g->theta));
  EXPECT_TRUE(std::isnan(g->charm));
  EXPECT_TRUE(std::isfinite(g->delta));
  EXPECT_TRUE(std::isfinite(g->gamma));
  EXPECT_TRUE(std::isfinite(g->vega));
  EXPECT_TRUE(std::isfinite(g->volga));
  EXPECT_TRUE(std::isfinite(g->vanna));
  EXPECT_TRUE(std::isfinite(g->rho));
}

// Bump sizes and the spot divisor are caller inputs, validated at the boundary.
TEST(DerivGreeks, RejectsUnusableBumpsAndSpot) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivContract c = var_swap_at(0.25);

  DerivGreekBumps zero_spot{};
  zero_spot.spot_rel = 0.0;
  const auto g0 = deriv_greeks(surf, cs, c, deriv_default_config(), zero_spot);
  ASSERT_FALSE(g0.has_value());
  EXPECT_EQ(g0.error().code(), ErrorCode::InvalidArgument);

  DerivGreekBumps whole_spot{};
  whole_spot.spot_rel = 1.0;  // would take the down-bumped spot to exactly 0
  EXPECT_FALSE(deriv_greeks(surf, cs, c, deriv_default_config(), whole_spot).has_value());

  DerivGreekBumps neg_time{};
  neg_time.time_years = -1.0;
  EXPECT_FALSE(deriv_greeks(surf, cs, c, deriv_default_config(), neg_time).has_value());

  CurveSet no_spot = make_flat_curves(100.0, 0.01, 1.00);
  no_spot.spot = 0.0;  // delta's divisor
  const auto gs = deriv_greeks(surf, no_spot, c);
  ASSERT_FALSE(gs.has_value());
  EXPECT_EQ(gs.error().code(), ErrorCode::InvalidArgument);
}

// A vol_abs bump >= the surface's own ATM vol pushes v_dn's down-shifted iv
// to <= 0 -- a silently-corrupted node the downstream strip resolves rather
// than errors on, hollowing out vega/volga/vanna with no visible signal.
// Reject up front off a cheap single sigma_atm read at k=0. (GK-C7)
TEST(DerivGreeks, RejectsVolAbsBumpAtOrAboveAtmVol) {
  const EssviSurface surf = make_flat_surface(0.10, 0.01, 1.00);  // 0.10-vol surface
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivContract c = var_swap_at(0.25);

  DerivGreekBumps too_big{};
  too_big.vol_abs = 0.15;  // >= sigma_atm = 0.10
  const auto g = deriv_greeks(surf, cs, c, deriv_default_config(), too_big);
  ASSERT_FALSE(g.has_value());
  EXPECT_EQ(g.error().code(), ErrorCode::InvalidArgument);
}

// Charm's sign on a negative-skew surface, cross-checked against an INDEPENDENT
// maturity difference.
//
// Why the sign is not readable off the skew alone: delta = N*df*(dK_var/dk)/S,
// and to leading order dK_var/dk = w'(0)/T = theta*rho*phi/T. eSSVI's theta is
// proportional to T on this surface, so that leading term is T-INDEPENDENT and
// contributes nothing to d(delta)/dT. Charm is therefore set entirely by the
// O(T) smile-curvature corrections (w'' and w''' averaged over an integration
// width that grows like sigma*sqrt(T)), whose net sign is not something to
// assert from a hand expansion. So: pin the sign the implementation produces,
// and independently corroborate it with a coarse d(delta)/dT taken across a
// 0.10-year maturity span -- a completely different difference from charm's own
// one-day roll, which a stencil sign error could not survive.
TEST(DerivGreeks, CharmSignOnSkewedSurfaceMatchesMaturitySlope) {
  const EssviSurface surf = make_skewed_surface();
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);

  const auto delta_at = [&surf, &cs](double T) -> double {
    const auto g = deriv_greeks(surf, cs, var_swap_at(T));
    if (!g.has_value()) {
      ADD_FAILURE() << "deriv_greeks failed at T=" << T << ": " << g.error().to_string();
      return 0.0;
    }
    return g->delta;
  };

  const auto g = deriv_greeks(surf, cs, var_swap_at(0.25));
  ASSERT_TRUE(g.has_value()) << g.error().to_string();

  // Measured: delta = -363.08, which confirms the leading-order account above
  // to ~1% (N*sigma^2*rho*phi/S = 1e6*0.04*(-0.6)*1.5/100 = -360).
  EXPECT_LT(g->delta, 0.0);

  // Measured: charm = +12.3766. POSITIVE -- the skew-driven short spot exposure
  // DECAYS toward zero as expiry approaches (delta rises from -363 toward 0),
  // so a hedger of a long negative-skew var swap buys back stock as the
  // contract runs off. Equivalently d(delta)/dT < 0: delta gets more negative
  // the longer the contract, the O(T) curvature correction adding to the
  // T-independent leading term.
  EXPECT_GT(g->charm, 0.0);

  // charm = d(delta)/dt and t runs opposite to T, so charm == -d(delta)/dT.
  // Measured agreement is 6.5e-5 relative between a one-DAY roll and this
  // 0.10-YEAR difference; 2e-2 leaves ~300x margin for the coarse stencil's own
  // O(h^2) error while still catching any sign or scale error.
  const double d_delta_dT = (delta_at(0.30) - delta_at(0.20)) / 0.10;
  EXPECT_LT(d_delta_dT, 0.0);
  EXPECT_NEAR(g->charm, -d_delta_dT, 2e-2 * std::fabs(d_delta_dT));
}

// Vega's bump size must not matter on a surface where PV is NOT quadratic in
// the vol shift.
//
// The flat-surface test above is exact but structurally lucky: there
// PV = N*sigma^2, so a central difference is exact at ANY bump size and the
// test cannot see a badly-sized stencil. On the skewed surface PV picks up
// genuine higher-order dependence on a parallel shift, so agreement between a
// 1e-4 and a 1e-2 bump is a real statement that the stencil is converged.
//
// (An independently-constructed shifted reference -- the flat test's two
// separate surfaces -- is not available here: a parallel ADDITIVE shift of
// iv(k,T) is not representable in the eSSVI parametrization, and deriv_price's
// template body lives in the .cpp, so a test-local shifted view cannot
// instantiate it.)
TEST(DerivGreeks, VegaIsBumpSizeIndependentOnSkewedSurface) {
  const EssviSurface surf = make_skewed_surface();
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivContract c = var_swap_at(0.25);

  const auto g_small = deriv_greeks(surf, cs, c);  // default vol_abs = 1e-4
  DerivGreekBumps big{};
  big.vol_abs = 1.0e-2;  // 100x
  const auto g_big = deriv_greeks(surf, cs, c, deriv_default_config(), big);
  ASSERT_TRUE(g_small.has_value());
  ASSERT_TRUE(g_big.has_value());
  EXPECT_GT(std::fabs(g_small->vega), 1.0);  // there is something to compare
  EXPECT_NEAR(g_small->vega, g_big->vega, 3e-2 * std::fabs(g_small->vega));
}

// Task C-10 (GK-C2): `theta` rolls ONLY the calendar (T -> T - dt) with
// `rv_spec` held fixed, so it silently omits the implied->realized fixing
// rollover -- the largest deterministic daily P&L term on an unaged/mid-life
// swap (theta reports ~0 on a fair-struck swap even though the fixing roll
// itself is a real, large daily mark move). theta_carry / theta_zero_fixing
// price that roll too, by injecting one extra fixing into a COPY of rv_spec
// before the same T - dt roll `theta` already takes.
TEST(CarryTheta, FairSwapCarryIsDiscountingOnly) {
  const double sigma = 0.20, T = 0.25, N = 1e6, r = 0.03;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, r);

  // Resolve the strip's own fair strike first (a Simpson quadrature, not
  // exactly sigma^2), then re-price fair-struck so the center's PV is 0 to
  // floating precision and any residual theta_carry is real signal, not
  // off-fair drift.
  DerivContract probe{};
  probe.kind = DerivKind::VarSwap;
  probe.maturity_t = T;
  probe.notional = N;
  probe.rv_spec.annualization = 252.0;
  probe.rv_spec.n_obs_total = 63u;
  const auto probe_q = deriv_price(surf, cs, probe, deriv_default_config());
  ASSERT_TRUE(probe_q.has_value()) << probe_q.error().to_string();
  const double k_var = probe_q->fair_strike_dec;  // unaged: == raw K_var_future
  ASSERT_GT(k_var, 0.0);

  DerivContract c = probe;
  c.strike_dec = k_var;  // fair-struck: center PV == 0
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  ASSERT_NEAR(g->pv, 0.0, 1.0);  // sanity: really fair-struck

  const double df = cs.yield.disc(T);
  // Scale of ONE fixing's mark move: w_future ~= 1/n_total of the future leg
  // (valued at k_var) reweighted onto the accrued leg -- the natural,
  // non-annualized epsilon a "theta_carry is small" claim gets quantified
  // against (see theta_zero_fixing below, which IS this same scale,
  // annualized).
  const double one_fixing_pv = df * N * k_var / c.rv_spec.n_obs_total;

  // theta_carry: the injected fixing lands exactly at today's implied
  // variance rate, so the blend does not move (fair stays fair) and only the
  // discount roll is left -- r*PV, here 0 because a fair-struck swap has
  // nothing to discount. Brief's bound: < 5% of df*N*K_var/n_total.
  EXPECT_NEAR(g->theta_carry, r * g->pv, 0.05 * one_fixing_pv);

  // theta_zero_fixing: the deterministic "nothing happened overnight" mark --
  // one fixing's worth of future-leg weight moves from K_var to a realized
  // zero, annualized by the SAME dt the stencil rolls T by
  // (DerivGreekBumps::time_years -- NOT rv_spec.annualization; the two need
  // not agree, and here time_years = 1/365.25 while annualization = 252, see
  // task-C-10-report.md for the derivation of why the code (and this test)
  // divide by time_years, not a hardcoded 252).
  const DerivGreekBumps bumps{};
  EXPECT_NEAR(g->theta_zero_fixing, -one_fixing_pv / bumps.time_years,
              0.05 * (one_fixing_pv / bumps.time_years));
}

// Pins theta_carry against theta_zero_fixing (and the aged-blend arithmetic
// both ride) via a closed-form difference that is exact by construction,
// independent of n_done, strike_dec, and even the rolled future leg's own
// value -- see task-C-10-report.md for the full derivation. Deliberately
// AGED and OFF-FAIR (unlike the unaged/fair fixture above) so this exercises
// the general blend, not the n_done == 0 degenerate case.
TEST(CarryTheta, SumIdentity) {
  const double sigma = 0.20, T = 0.25, N = 1e6;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T;
  c.notional = N;
  c.strike_dec = 0.03;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 21u;
  c.rv_spec.rv_done_dec = 0.05;

  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  ASSERT_TRUE(std::isfinite(g->theta_carry));
  ASSERT_TRUE(std::isfinite(g->theta_zero_fixing));

  // theta_carry - theta_zero_fixing == df(T-dt)*N*K_var_future /
  //                                    (n_obs_total * bumps.time_years)
  // Both variants inject exactly one fixing (n_done -> n_done+1) into the
  // SAME blend at the SAME rolled T, differing only in the injected fixing's
  // own value (K_var_future vs 0); every other term -- the rolled future leg
  // K_var_future(T-dt), strike_dec, the n_done-weighted pre-fixing accrual --
  // is common to both reprices and cancels in the difference.
  const double k_var = g->quote.uncapped_var_dec;  // raw K_var_future at T
  ASSERT_GT(k_var, 0.0);
  const DerivGreekBumps bumps{};
  const double df_rolled = cs.yield.disc(T - bumps.time_years);
  const double expected =
      df_rolled * N * k_var / (c.rv_spec.n_obs_total * bumps.time_years);
  EXPECT_NEAR(g->theta_carry - g->theta_zero_fixing, expected, 0.05 * expected);
}

// bumps.carry_theta = false must leave both fields at their NaN default with
// no extra evaluation paid -- the opt-out this knob exists for.
TEST(CarryTheta, OptOutLeavesBothNaN) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  const DerivContract c = var_swap_at(0.25);
  DerivGreekBumps bumps{};
  bumps.carry_theta = false;
  const auto g = deriv_greeks(surf, cs, c, deriv_default_config(), bumps);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(std::isnan(g->theta_carry));
  EXPECT_TRUE(std::isnan(g->theta_zero_fixing));
  EXPECT_TRUE(std::isfinite(g->theta));  // plain theta is unaffected
}

// Fully aged: both carry variants equal theta exactly -- nothing left to
// realize, so there is no fixing roll left to price either (mirrors
// FullyAgedWithRateAccretesAtTheCarry's theta/rho identity above).
TEST(CarryTheta, FullyAgedEqualsTheta) {
  const double r = 0.043, T = 0.25;
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, r);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;
  const auto g = deriv_greeks(surf, cs, c);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_DOUBLE_EQ(g->theta_carry, g->theta);
  EXPECT_DOUBLE_EQ(g->theta_zero_fixing, g->theta);
}

// --- Fix round 1 (review findings) -----------------------------------------

// CRITICAL-1: injecting the carry fixing turns an unaged VolSwap mid-life
// (n_obs_done 0 -> 1), and `price_vol_swap` rejects an EXPLICIT VolCarrLee
// engine mid-life (Carr-Lee cannot blend an accrued leg). Pre-fix, that
// InvalidArgument propagated out of `eval_bump_table` via ATX_TRY and dropped
// the ENTIRE greek block -- delta through charm, not just the two carry
// fields -- for a documented, previously-working configuration (this file's
// own vol-swap dispatch doc: unaged + explicit VolCarrLee is Marquee's own
// inception convention). No existing `deriv_greeks` test set that engine,
// which is why the suite stayed green pre-fix. Pins: the call still succeeds,
// the two carry fields report NaN (honest "not computed" for a diagnostic
// that cannot be priced under the caller's own explicit engine choice), and
// every other greek is unaffected.
TEST(CarryTheta, UnagedVolSwapExplicitCarrLeeKeepsBlockAliveWithCarryFieldsNaN) {
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = 0.25;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;  // unaged: n_obs_done left at 0

  DerivConfig cfg = deriv_default_config();
  cfg.engine = DerivEngine::VolCarrLee;
  const auto g = deriv_greeks(surf, cs, c, cfg);  // default bumps: carry_theta = true
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

// IMPORTANT-1: the fully-aged branch returns theta_carry == theta_zero_fixing
// == theta UNCONDITIONALLY -- nothing is realized there, so there is no
// fixing roll left for the knob to gate (the brief itself states "Fully-aged:
// both = theta" with no carve-out). Pins that `carry_theta = false` does NOT
// turn these NaN on that branch, matching the corrected doc on
// `DerivGreeks::theta_carry`. Otherwise identical to `FullyAgedEqualsTheta`.
TEST(CarryTheta, FullyAgedIgnoresCarryThetaOptOut) {
  const double r = 0.043, T = 0.25;
  const EssviSurface surf = make_flat_surface(0.20, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, r);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T;
  c.notional = 1e5;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;
  c.rv_spec.n_obs_done = 63u;
  c.rv_spec.rv_done_dec = 0.0441;
  DerivGreekBumps bumps{};
  bumps.carry_theta = false;
  const auto g = deriv_greeks(surf, cs, c, deriv_default_config(), bumps);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  EXPECT_TRUE(std::isfinite(g->theta_carry));
  EXPECT_DOUBLE_EQ(g->theta_carry, g->theta);
  EXPECT_DOUBLE_EQ(g->theta_zero_fixing, g->theta);
}

// IMPORTANT-2: on an unaged VolSwap the center prices via Carr-Lee, but the
// injected +1-fixing copy is mid-life and therefore prices via the lognormal
// RV distribution model (`price_vol_swap_distribution`) -- the two carry
// variants momentarily difference PVs from two DIFFERENT pricers, unlike
// `theta` (which never touches `rv_spec` and so never leaves Carr-Lee).
//
// Reference derivation (also recorded on `DerivGreeks::theta_carry` and in
// task-C-10-report.md's "Fix round 1" section): both variants inject the SAME
// b = w_future and share the SAME lognormal W (same rolled T, same
// auto-calibrated xi -- `resolve_vol_of_vol` depends only on the surface/T,
// not on `a`), differing only in `a_carry = K_var_future/n_total` vs
// `a_zero = 0`. Since a_carry is a SMALL perturbation (1/n_total of the
// blend), a first-order Taylor expansion of sqrt(a+bW) around a = 0 gives
//   theta_carry - theta_zero_fixing
//       ~= df(T-dt)*N*K_var_future / (2*K_vol*n_total*bumps.time_years)
// (the extra 1/(2*K_vol) next to VarSwap's exact SumIdentity is d/da[sqrt] at
// a = 0). This reproduces the reviewer's own worked example almost exactly
// (their "~5.8e4 carry signal" on a 1e5-notional, sigma=0.20, T=0.25 fixture
// is precisely this formula), and the ~0.16% they separately quantify as the
// Jensen-gap residual (E[sqrt] vs sqrt-of-mean, from replacing 1/n_total of
// the lognormal leg with a deterministic one) is what the 1% tolerance below
// leaves room for -- so this is a real tripwire on the model-switch artifact
// staying second-order, not a restatement of the implementation.
TEST(CarryTheta, UnagedVolSwapCarryTracksLinearBlendWithinModelSwitchTolerance) {
  const double sigma = 0.20, T = 0.25, N = 1e6;
  const EssviSurface surf = make_flat_surface(sigma, 0.01, 1.00);
  const CurveSet cs = make_flat_curves(100.0, 0.01, 1.00, 0.03);
  DerivContract c{};
  c.kind = DerivKind::VolSwap;
  c.maturity_t = T;
  c.notional = N;
  c.strike_dec = 0.18;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 63u;  // unaged: n_obs_done left at 0

  const auto g = deriv_greeks(surf, cs, c);  // default config: Auto engine, auto-xi
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  ASSERT_TRUE(std::isfinite(g->theta_carry));
  ASSERT_TRUE(std::isfinite(g->theta_zero_fixing));

  // Best-effort strip diagnostic on the unaged Carr-Lee dispatch quote --
  // populated whenever the strip succeeds (it does, on this flat surface);
  // see price_vol_swap's own comment on `uncapped_var_dec`.
  const double k_var = g->quote.uncapped_var_dec;
  const double k_vol = g->quote.fair_strike_dec;  // Carr-Lee K_vol at the center
  ASSERT_GT(k_var, 0.0);
  ASSERT_GT(k_vol, 0.0);

  const DerivGreekBumps bumps{};
  const double df_rolled = cs.yield.disc(T - bumps.time_years);
  const double expected = df_rolled * N * k_var /
                          (2.0 * k_vol * c.rv_spec.n_obs_total * bumps.time_years);
  EXPECT_GT(expected, 0.0);  // there is something to compare
  EXPECT_NEAR(g->theta_carry - g->theta_zero_fixing, expected, 0.01 * expected);
}

}  // namespace
