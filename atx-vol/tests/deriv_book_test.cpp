#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "atx/vol/american.hpp" // al_fast_opts, AmericanMethod
#include "atx/vol/deriv_book.hpp"
#include "atx/vol/derivatives.hpp" // deriv_greeks (PricedSurface overload) — the reference
#include "atx/vol/detail/strip_grid.hpp" // strip::kCertifiedWingHalfBand (Task C-6)
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"    // PricedSurface, PricingContext
#include "atx/vol/surface_parity.hpp"    // SliceContext
#include "atx/vol/surface_policy.hpp"    // FitQualityMode, certified_wing_half_band (Task C-6)
#include "atx/vol/vol_curve.hpp"         // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"       // EssviParams
#include "support/analytics_fixture.hpp" // testkit::make_flat_surface (PricedSurface)

// Task 9: DerivBook — portfolio-layer pricing of vol-derivative (swap) books
// against a SurfaceSet. The additive companion to PortfolioPricer: a missing
// surface or a pricing failure marks a ROW, never the call, and the totals
// block follows the established PriceTotals conventions (NaN = not computed).

namespace {

using atx::vol::combine_totals;
using atx::vol::CurveSurface;
using atx::vol::DerivContract;
using atx::vol::DerivDiscreteCorrection;
using atx::vol::DerivGreekBumps;
using atx::vol::DerivKind;
using atx::vol::DerivPosition;
using atx::vol::EssviCurve;
using atx::vol::EssviParams;
using atx::vol::price_deriv_book;
using atx::vol::PricedSurface;
using atx::vol::PriceStatus;
using atx::vol::PriceTotals;
using atx::vol::PricingContext;
using atx::vol::SliceContext;
using atx::vol::SurfaceSet;

// A surface carrying BOTH a genuine downside skew AND a genuine term structure
// of forwards: every slice gets its own F(T) = S*e^{(r-q)T}, so
// `PricedSurface::forward_at` actually MOVES with T.
//
// Neither testkit builder does this. `make_flat_surface` has no skew at all, and
// both testkit builders pin every slice's `SliceContext::forward` to one
// constant `fwd` — so `interp_forward`'s log-blend of two equal pillars is that
// same constant and `forward_at(T) == forward_at(T - dt)` identically, no matter
// what `fwd`/S imply about carry. With no forward roll and no skew there is
// nothing for a forward-roll theta term to be, so a test built on them would
// pass vacuously. (`FixtureActuallyRollsItsForward` below asserts the property
// rather than trusting this comment.)
[[nodiscard]] PricedSurface make_carry_skew_surface(std::uint32_t uid, double S, double sigma,
                                                    double q) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  std::uint16_t i = 0;
  for (const double T : atx::vol::testkit::fixture_tenors()) {
    const double F = S * std::exp((atx::vol::testkit::kFixtureRate - q) * T);
    EssviParams e{};
    e.theta = sigma * sigma * T; // w proportional to T => the k-smile is T-invariant
    e.phi = 1.5;                 // curvature
    e.rho = -0.6;                // downside skew: dsigma/dk < 0 at the money
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = F;
    e.expiry_id = i;
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-atx::vol::testkit::kFixtureRate * T)));
    ctx.push_back(SliceContext{T, F, 0.0, q, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = atx::vol::testkit::kFixtureRate;
  pc.now_ts_ns = atx::vol::testkit::kFixtureNow;
  pc.method = atx::vol::AmericanMethod::AndersenLake;
  pc.al_opts = atx::vol::al_fast_opts();
  pc.uid = uid;
  return atx::vol::testkit::unwrap_surface(
      PricedSurface::create(std::move(cs), std::move(ctx), pc));
}

// Steep-wing eSSVI surface (Task C-6): same phi/rho shape as derivatives_
// test.cpp's `make_steep_wing_priced_surface` -- a caricature of an
// undisciplined fitted wing, so a 0.35 vs 0.5 wing-trust band resolves
// materially different strikes rather than a rounding-noise difference.
[[nodiscard]] PricedSurface make_steep_wing_surface(std::uint32_t uid, double S, double sigma) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  std::uint16_t i = 0;
  for (const double T : atx::vol::testkit::fixture_tenors()) {
    EssviParams e{};
    e.theta = sigma * sigma * T;
    e.phi = 4.0;
    e.rho = -0.7;
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = S;
    e.expiry_id = i;
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-atx::vol::testkit::kFixtureRate * T)));
    ctx.push_back(SliceContext{T, S, 0.0, 0.0, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = atx::vol::testkit::kFixtureRate;
  pc.now_ts_ns = atx::vol::testkit::kFixtureNow;
  pc.method = atx::vol::AmericanMethod::AndersenLake;
  pc.al_opts = atx::vol::al_fast_opts();
  pc.uid = uid;
  return atx::vol::testkit::unwrap_surface(
      PricedSurface::create(std::move(cs), std::move(ctx), pc));
}

// An unaged 1e6-notional var swap struck at 0.
[[nodiscard]] DerivContract var_swap_at(double T) {
  DerivContract c{};
  c.kind = DerivKind::VarSwap;
  c.maturity_t = T;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 88u;
  return c;
}

TEST(DerivBook, PricesVarAndVolSwapAgainstSurfaceSet) {
  const atx::vol::PricedSurface ps = atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, 0.30);
  const atx::vol::PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  DerivPosition p0{};
  p0.id = 11;
  p0.uid = 7;
  p0.qty = 2.0;
  p0.contract.kind = DerivKind::VarSwap;
  p0.contract.maturity_t = 0.35;
  p0.contract.notional = 1e6;
  p0.contract.rv_spec.annualization = 252.0;
  p0.contract.rv_spec.n_obs_total = 88u;
  DerivPosition p1 = p0;
  p1.id = 12;
  p1.qty = -1.0;
  p1.contract.kind = DerivKind::VolSwap;

  const DerivPosition book[] = {p0, p1};
  const auto f = price_deriv_book(*ss, book);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows.size(), 2u);
  EXPECT_EQ(f->rows[0].id, 11u);
  EXPECT_EQ(f->rows[0].status, PriceStatus::Ok);
  EXPECT_EQ(f->rows[1].status, PriceStatus::Ok);
  EXPECT_NEAR(f->rows[0].fair_strike_dec, 0.09, 5e-4); // sigma^2
  EXPECT_NEAR(f->rows[1].fair_strike_dec, 0.30, 5e-3); // ~sigma
  // struck at 0 => pv = df*qty*N*K; sign follows qty
  EXPECT_GT(f->rows[0].pv, 0.0);
  EXPECT_LT(f->rows[1].pv, 0.0);
  // totals = serial sum of Ok rows
  EXPECT_NEAR(f->totals.pv, f->rows[0].pv + f->rows[1].pv, 1e-9);
  EXPECT_EQ(f->n_ok(), 2u);
  // qty scaling: row0 vega is 2x the single-contract vega, roughly 2*2*sigma*N*df
  EXPECT_GT(f->rows[0].greeks.vega, 0.0);
}

// FIT-C7 / Task C-6, review round 1 CRITICAL-1: `price_deriv_book` is the
// umbrella-exported public entry point (deriv_book.hpp); before this fix
// landed no caller of it could supply a certified band at all, so every row
// against a non-Balanced-quality surface silently trusted the mode-blind
// default. `WingBandResolver` closes that: a caller supplies a uid ->
// std::optional<double> function and every row's own uid is looked up
// through it. An unset resolver ({}) is unchanged prior behaviour.
TEST(DerivBook, WingBandResolverAppliesTheCallersCertifiedBandPerRow) {
  const atx::vol::PricedSurface ps = make_steep_wing_surface(21, 100.0, 0.30);
  const atx::vol::PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  DerivPosition p{};
  p.id = 1;
  p.uid = 21;
  p.qty = 1.0;
  p.contract = var_swap_at(0.35);
  const DerivPosition book[] = {p};

  // No resolver: the mode-blind default, unchanged prior behaviour.
  const auto f_default = price_deriv_book(*ss, book);
  ASSERT_TRUE(f_default.has_value()) << f_default.error().to_string();
  ASSERT_EQ(f_default->rows[0].status, PriceStatus::Ok);
  EXPECT_DOUBLE_EQ(f_default->rows[0].greeks.quote.resolved_wing_clamp,
                   atx::vol::strip::kCertifiedWingHalfBand);

  // A resolver that reports this uid's surface as Latency-quality.
  const double latency_band =
      atx::vol::certified_wing_half_band(atx::vol::FitQualityMode::Latency);
  const atx::vol::WingBandResolver resolver =
      [&](std::uint32_t uid) -> std::optional<double> {
    return uid == 21u ? std::optional<double>{latency_band} : std::nullopt;
  };
  const auto f_latency =
      price_deriv_book(*ss, book, atx::vol::DerivConfig{}, /*greeks=*/true,
                       atx::vol::DerivGreekBumps{}, resolver);
  ASSERT_TRUE(f_latency.has_value()) << f_latency.error().to_string();
  ASSERT_EQ(f_latency->rows[0].status, PriceStatus::Ok);
  EXPECT_DOUBLE_EQ(f_latency->rows[0].greeks.quote.resolved_wing_clamp, latency_band);

  // Not a no-op, and not merely different: flattening more of the steepening
  // wing under the tighter certified band can only lower the strike.
  EXPECT_LT(f_latency->rows[0].fair_strike_dec, f_default->rows[0].fair_strike_dec);
}

TEST(DerivBook, MissingSurfaceMarksRowNotCall) {
  const atx::vol::PricedSurface ps = atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, 0.30);
  const atx::vol::PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  DerivPosition good{};
  good.id = 1;
  good.uid = 7;
  good.contract.kind = DerivKind::VarSwap;
  good.contract.maturity_t = 0.35;
  good.contract.notional = 1e6;
  good.contract.rv_spec.annualization = 252.0;
  good.contract.rv_spec.n_obs_total = 88u;
  DerivPosition orphan = good;
  orphan.id = 2;
  orphan.uid = 999; // no such surface
  const DerivPosition book[] = {good, orphan};
  const auto f = price_deriv_book(*ss, book);
  ASSERT_TRUE(f.has_value());
  ASSERT_EQ(f->rows.size(), 2u);
  EXPECT_EQ(f->rows[0].status, PriceStatus::Ok);
  EXPECT_EQ(f->rows[1].status, PriceStatus::ModelUnavailable);
  EXPECT_TRUE(std::isnan(f->rows[1].pv));
  EXPECT_TRUE(std::isnan(f->rows[1].greeks.vega));
  EXPECT_EQ(f->n_ok(), 1u);
  EXPECT_NEAR(f->totals.pv, f->rows[0].pv, 1e-9); // orphan excluded, not zeroed
}

TEST(DerivBook, MarksOnlySkipsGreeks) {
  const atx::vol::PricedSurface ps = atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, 0.30);
  const atx::vol::PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  DerivPosition p{};
  p.id = 1;
  p.uid = 7;
  p.contract.kind = DerivKind::VarSwap;
  p.contract.maturity_t = 0.35;
  p.contract.notional = 1e6;
  p.contract.rv_spec.annualization = 252.0;
  p.contract.rv_spec.n_obs_total = 88u;
  const DerivPosition book[] = {p};
  const auto f = price_deriv_book(*ss, book, {}, /*greeks=*/false);
  ASSERT_TRUE(f.has_value());
  ASSERT_EQ(f->rows.size(), 1u);
  EXPECT_TRUE(std::isfinite(f->rows[0].pv));
  EXPECT_TRUE(std::isnan(f->rows[0].greeks.vega));
  EXPECT_TRUE(std::isfinite(f->totals.pv));
  EXPECT_EQ(f->n_ok(), 1u);
  // Marks-only: every greek TOTAL is "not computed", including the gross-vega
  // companion — a 0.0 there would read as a genuinely vega-flat book.
  EXPECT_TRUE(std::isnan(f->totals.delta));
  EXPECT_TRUE(std::isnan(f->totals.gamma));
  EXPECT_TRUE(std::isnan(f->totals.vega));
  EXPECT_TRUE(std::isnan(f->totals.abs_vega));
  EXPECT_TRUE(std::isnan(f->totals.theta));
  EXPECT_TRUE(std::isnan(f->totals.rho));
  EXPECT_TRUE(std::isnan(f->totals.vanna));
  EXPECT_TRUE(std::isnan(f->totals.volga));
  EXPECT_TRUE(std::isnan(f->totals.charm));
  // No per-contract IV lane exists for a swap, so the carry axis is never
  // computed on this route.
  EXPECT_TRUE(std::isnan(f->totals.dP_dq));
}

TEST(DerivBook, EmptyBookIsAnEmptyFrameNotAnError) {
  const atx::vol::PricedSurface ps = atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, 0.30);
  const atx::vol::PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  const auto f = price_deriv_book(*ss, {});
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  EXPECT_TRUE(f->rows.empty());
  EXPECT_EQ(f->n_ok(), 0u);
  EXPECT_EQ(f->totals.pv, 0.0);
  EXPECT_EQ(f->totals.n_ok, 0u);
  // greeks were REQUESTED, so the empty sums are genuinely 0, not "not computed".
  EXPECT_EQ(f->totals.vega, 0.0);
  EXPECT_EQ(f->totals.abs_vega, 0.0);
}

TEST(DerivBook, InvalidContractMarksRowInvalidNotNumeric) {
  const atx::vol::PricedSurface ps = atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, 0.30);
  const atx::vol::PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  DerivPosition bad{};
  bad.id = 5;
  bad.uid = 7;
  bad.contract.kind = DerivKind::VarSwap;
  bad.contract.maturity_t = 0.0; // unaged swap at expiry: nothing left to price
  bad.contract.notional = 1e6;
  bad.contract.rv_spec.annualization = 252.0;
  bad.contract.rv_spec.n_obs_total = 88u;
  const DerivPosition book[] = {bad};
  const auto f = price_deriv_book(*ss, book);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows.size(), 1u);
  EXPECT_EQ(f->rows[0].status, PriceStatus::InvalidContract);
  EXPECT_TRUE(std::isnan(f->rows[0].pv));
  EXPECT_TRUE(std::isnan(f->rows[0].fair_strike_dec));
  EXPECT_EQ(f->n_ok(), 0u);
  EXPECT_EQ(f->totals.pv, 0.0);
}

// The per-position CurveSet is a SNAPSHOT of the surface's own carry at the
// contract tenor. This pins what that snapshot must reproduce.
//
// On a flat surface K_var(T) == sigma^2 for every T, so a var swap struck at 0
// is PURE DISCOUNTING: pv = e^{-rT}*N*sigma^2, and both time greeks follow from
// differentiating that one statement two ways — rho = -T*pv and theta = +r*pv.
// theta in particular is the gate on the snapshot's yield curve being flat in
// RATE: a curve built from a single pillar extrapolates log(df) FLAT, freezing
// the discount across the theta roll and collapsing theta to ~0.
TEST(DerivBook, CarrySnapshotReproducesDiscountAndItsRoll) {
  const double sigma = 0.30;
  const double T = 0.35;
  const double N = 1e6;
  const atx::vol::PricedSurface ps = atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, sigma);
  const atx::vol::PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  DerivPosition p{};
  p.id = 1;
  p.uid = 7;
  p.contract.kind = DerivKind::VarSwap;
  p.contract.maturity_t = T;
  p.contract.notional = N;
  p.contract.rv_spec.annualization = 252.0;
  p.contract.rv_spec.n_obs_total = 88u;
  const DerivPosition book[] = {p};
  const auto f = price_deriv_book(*ss, book);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows.size(), 1u);
  ASSERT_EQ(f->rows[0].status, PriceStatus::Ok);

  const double r = atx::vol::testkit::kFixtureRate;
  const double df = std::exp(-r * T);
  EXPECT_NEAR(f->rows[0].pv, df * N * sigma * sigma, 5e-4 * N);
  const double pv = f->rows[0].pv;
  EXPECT_NEAR(f->rows[0].greeks.rho, -T * pv, 5e-3 * std::fabs(T * pv));
  EXPECT_NEAR(f->rows[0].greeks.theta, r * pv, 2e-2 * std::fabs(r * pv));
  // vega of a var swap = dK_var/dsigma * df * N = 2*sigma*df*N.
  EXPECT_NEAR(f->rows[0].greeks.vega, 2.0 * sigma * df * N, 2e-2 * 2.0 * sigma * df * N);
}

// Guards the premise of the theta test below: if this fixture ever loses its
// forward term structure or its skew, the reference comparison would still pass
// while measuring nothing.
TEST(DerivBook, FixtureActuallyRollsItsForward) {
  const PricedSurface ps = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const double T = 0.35;
  const double dt = DerivGreekBumps{}.time_years;
  const double f_now = ps.forward_at(T);
  const double f_rolled = ps.forward_at(T - dt);
  ASSERT_GT(f_now, 0.0);
  ASSERT_GT(f_rolled, 0.0);
  EXPECT_NE(f_now, f_rolled);
  // And the smile is genuinely skewed, so mis-centering the strip on that
  // forward gap actually moves K_var.
  EXPECT_LT(ps.iv(f_now * std::exp(0.1), T), ps.iv(f_now * std::exp(-0.1), T));
  // For contrast: testkit's builder pins one constant forward across slices.
  const PricedSurface flat = atx::vol::testkit::make_flat_surface(8, 100.0, 100.0, 0.30);
  EXPECT_DOUBLE_EQ(flat.forward_at(T), flat.forward_at(T - dt));
}

// THE forward-carry gate on the bridge's carry snapshot.
//
// The PricedSurface-native `deriv_greeks` is the REFERENCE: its carry CurveSet
// holds every fitted pillar, so its theta roll reads the surface's own forward
// at the rolled tenor. The bridge must agree. It only does so because the
// snapshot carries a second forward pillar at (T - dt); with a lone pillar at T,
// `resolve_forward` clamps and the rolled repricing reads F(T) while the smile
// is anchored at F(T - dt) — the strip's k = 0 lands at k = (r - q)*dt on a
// SKEWED smile and K_var picks up a bias that theta then divides by dt.
TEST(DerivBook, BridgeThetaMatchesPricedSurfaceReference) {
  const double T = 0.35;
  const PricedSurface ps = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const DerivContract c = var_swap_at(T);

  const auto reference = atx::vol::deriv_greeks(ps, c);
  ASSERT_TRUE(reference.has_value()) << reference.error().to_string();
  ASSERT_TRUE(std::isfinite(reference->theta));

  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  DerivPosition p{};
  p.id = 1;
  p.uid = 7;
  p.contract = c;
  const DerivPosition book[] = {p};
  const auto f = price_deriv_book(*ss, book);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows[0].status, PriceStatus::Ok);

  // The mark is the same pricer on the same carry: agree tightly.
  EXPECT_NEAR(f->rows[0].pv, reference->pv, 1e-6 * std::fabs(reference->pv));
  // Theta is what the second forward pillar buys. Both paths resolve F through a
  // CurveSet built from the SAME surface with the SAME log-linear blend, so the
  // agreement is structural, not coincidental.
  EXPECT_NEAR(f->rows[0].greeks.theta, reference->theta, 2e-2 * std::fabs(reference->theta) + 1e-6);
  // The other market greeks ride the same carry and must match too.
  EXPECT_NEAR(f->rows[0].greeks.vega, reference->vega, 1e-6 * std::fabs(reference->vega));
  EXPECT_NEAR(f->rows[0].greeks.rho, reference->rho, 1e-6 * std::fabs(reference->rho));
  EXPECT_NEAR(f->rows[0].greeks.delta, reference->delta, 1e-6 * std::fabs(reference->delta) + 1e-9);
}

// Mirror of the above at the surface's FRONT fitted pillar (GK-C8). Before the
// fix, `carry_from`'s single carry snapshot held every fitted pillar but only
// ONE at T itself; a theta roll to T - dt landed BELOW every pillar, and both
// `resolve_forward` (forward) and `YieldCurve` (discount) clamped flat at T
// instead of reading the surface's own extrapolated forward/rate at T - dt --
// mis-centering the strip on a SKEWED smile and dropping theta's discount-roll
// term, both of which theta's own /dt then amplifies (see the now-deleted
// header caveat). The bridge path (`carry_from_ref`) never had this bug: it
// always carries a second forward + rate pillar at the rolled tenor, front or
// not, so it remains a valid independent reference here too.
TEST(DerivBook, BridgeThetaMatchesPricedSurfaceReferenceAtFrontPillar) {
  const double T = atx::vol::testkit::fixture_tenors().front();  // 0.05, the front pillar
  const PricedSurface ps = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const DerivContract c = var_swap_at(T);

  const auto reference = atx::vol::deriv_greeks(ps, c);
  ASSERT_TRUE(reference.has_value()) << reference.error().to_string();
  ASSERT_TRUE(std::isfinite(reference->theta));

  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  DerivPosition p{};
  p.id = 1;
  p.uid = 7;
  p.contract = c;
  const DerivPosition book[] = {p};
  const auto f = price_deriv_book(*ss, book);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows[0].status, PriceStatus::Ok);

  // The mark is the same pricer on the same carry: agree tightly.
  EXPECT_NEAR(f->rows[0].pv, reference->pv, 1e-6 * std::fabs(reference->pv));
  // Theta is what the second forward pillar buys, front pillar included now.
  EXPECT_NEAR(f->rows[0].greeks.theta, reference->theta, 2e-2 * std::fabs(reference->theta) + 1e-6);
  // The other market greeks ride the same carry and must match too.
  EXPECT_NEAR(f->rows[0].greeks.vega, reference->vega, 1e-6 * std::fabs(reference->vega));
  EXPECT_NEAR(f->rows[0].greeks.rho, reference->rho, 1e-6 * std::fabs(reference->rho));
  EXPECT_NEAR(f->rows[0].greeks.delta, reference->delta, 1e-6 * std::fabs(reference->delta) + 1e-9);
}

// The NumericError branch of the status mapping: a reserved discrete-correction
// mode is NotImplemented — a well-formed contract the engine will not price —
// and must NOT be conflated with InvalidContract. Run as an A/B against the same
// book under the default config so the surface and contract are proven fine.
TEST(DerivBook, ReservedCorrectionModeMarksRowNumericError) {
  const PricedSurface ps = atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, 0.30);
  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  DerivPosition good{};
  good.id = 1;
  good.uid = 7;
  good.contract = var_swap_at(0.35);
  DerivPosition orphan = good;
  orphan.id = 2;
  orphan.uid = 999;
  const DerivPosition book[] = {good, orphan};

  // A: default config — the good lane prices.
  const auto ok = price_deriv_book(*ss, book);
  ASSERT_TRUE(ok.has_value()) << ok.error().to_string();
  ASSERT_EQ(ok->rows.size(), 2u);
  EXPECT_EQ(ok->rows[0].status, PriceStatus::Ok);
  EXPECT_EQ(ok->rows[1].status, PriceStatus::ModelUnavailable);
  EXPECT_EQ(ok->n_ok(), 1u);

  // B: same book, reserved correction mode — the SAME lane is now NumericError.
  atx::vol::DerivConfig cfg{};
  cfg.discrete_correction_mode = DerivDiscreteCorrection::FullMc;
  const auto f = price_deriv_book(*ss, book, cfg);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows.size(), 2u);
  EXPECT_EQ(f->rows[0].status, PriceStatus::NumericError);
  EXPECT_TRUE(std::isnan(f->rows[0].pv));
  EXPECT_TRUE(std::isnan(f->rows[0].fair_strike_dec));
  EXPECT_TRUE(std::isnan(f->rows[0].greeks.vega));
  EXPECT_TRUE(std::isnan(f->rows[0].greeks.quote.pv));
  // The other lane keeps its own independent verdict, and nothing reaches totals.
  EXPECT_EQ(f->rows[1].status, PriceStatus::ModelUnavailable);
  EXPECT_EQ(f->n_ok(), 0u);
  EXPECT_EQ(f->totals.pv, 0.0);
  EXPECT_EQ(f->totals.n_ok, 0u);
  // Marks-only takes the same mapping (deriv_price rejects it identically).
  const auto m = price_deriv_book(*ss, book, cfg, /*greeks=*/false);
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->rows[0].status, PriceStatus::NumericError);
}

TEST(DerivBook, CombineTotalsAddsFieldsAndCounts) {
  PriceTotals a{};
  a.pv = 10.0;
  a.vega = 2.0;
  a.abs_vega = 2.0;
  a.n_ok = 3u;
  PriceTotals b{};
  b.pv = -4.0;
  b.vega = -5.0;
  b.abs_vega = 5.0;
  b.n_ok = 2u;
  const PriceTotals c = combine_totals(a, b);
  EXPECT_DOUBLE_EQ(c.pv, 6.0);
  EXPECT_DOUBLE_EQ(c.vega, -3.0);
  EXPECT_DOUBLE_EQ(c.abs_vega, 7.0);
  EXPECT_EQ(c.n_ok, 5u);
  // A "not computed" field on either side poisons the combined field: a marks-only
  // block must never silently contribute a 0.0 greek to an option book's risk.
  PriceTotals marks{};
  marks.vega = std::numeric_limits<double>::quiet_NaN();
  const PriceTotals d = combine_totals(a, marks);
  EXPECT_TRUE(std::isnan(d.vega));
  EXPECT_DOUBLE_EQ(d.pv, 10.0);
  EXPECT_EQ(d.n_ok, 3u);
}

} // namespace
