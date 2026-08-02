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
using atx::vol::DerivContract;
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

}  // namespace
