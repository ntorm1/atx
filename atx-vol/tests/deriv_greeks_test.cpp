#include <gtest/gtest.h>

#include <cmath>

#include "atx/vol/curve.hpp"
#include "atx/vol/derivatives.hpp"
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
using atx::vol::DerivKind;
using atx::vol::EssviSlice;
using atx::vol::EssviSurface;
using atx::vol::testsupport::make_flat_curves;
using atx::vol::testsupport::make_flat_surface;

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
  EXPECT_EQ(g->pv, g->quote.pv);
}

}  // namespace
