#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "atx/vol/deriv_book.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "support/analytics_fixture.hpp" // testkit::make_flat_surface (PricedSurface)

// Task 9: DerivBook — portfolio-layer pricing of vol-derivative (swap) books
// against a SurfaceSet. The additive companion to PortfolioPricer: a missing
// surface or a pricing failure marks a ROW, never the call, and the totals
// block follows the established PriceTotals conventions (NaN = not computed).

namespace {

using atx::vol::combine_totals;
using atx::vol::DerivContract;
using atx::vol::DerivKind;
using atx::vol::DerivPosition;
using atx::vol::price_deriv_book;
using atx::vol::PriceStatus;
using atx::vol::PriceTotals;
using atx::vol::SurfaceSet;

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
