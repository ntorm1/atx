#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "atx/vol/american.hpp" // al_fast_opts, AmericanMethod
#include "atx/vol/deriv_book.hpp"
#include "atx/vol/derivatives.hpp" // deriv_greeks (PricedSurface overload) — the reference
#include "atx/vol/detail/counters.hpp"       // Task P-6: ledger::Solve::VarSwapStripEvals
#include "atx/vol/detail/deriv_ref_bridge.hpp" // Task P-6: unmemoized detail::*_on_ref reference
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
using atx::vol::DerivConfig;
using atx::vol::DerivContract;
using atx::vol::DerivDiscreteCorrection;
using atx::vol::DerivGreekBumps;
using atx::vol::DerivGreekMethod;
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
using atx::vol::SurfaceRef;
using atx::vol::SurfaceSet;
namespace ledger = atx::vol::counters::ledger;

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

// Task F-2: the gamma-swap sibling of `var_swap_at` above.
[[nodiscard]] DerivContract gamma_swap_at(double T) {
  DerivContract c{};
  c.kind = DerivKind::GammaSwap;
  c.maturity_t = T;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 88u;
  return c;
}

// Task F-3: the corridor sibling. The corridor is deliberately NARROWER than
// the resolved span (the fixture's spot is 100.0), never the unbounded 0/0 --
// a corridor at its no-op value would make this contract price identically to
// `var_swap_at` and hide any routing mistake.
[[nodiscard]] DerivContract corridor_var_swap_at(double T) {
  DerivContract c{};
  c.kind = DerivKind::CorridorVarSwap;
  c.maturity_t = T;
  c.notional = 1e6;
  c.rv_spec.annualization = 252.0;
  c.rv_spec.n_obs_total = 88u;
  c.corridor_lo = 85.0;
  c.corridor_hi = 120.0;
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

// Task F-2 (deriv_book plumbing / "kind passthrough"): a GammaSwap row prices
// correctly through the SAME public entry point VarSwap/VolSwap already do --
// `var_swap_memo_eligible` (deriv_book.cpp) whitelists `kind ==
// DerivKind::VarSwap` only, so this row falls straight through to the generic
// unmemoized `deriv_price_on_ref`/`deriv_greeks_on_ref` path, which dispatches
// on `contract.kind` exactly as the templated `deriv_price` does. No
// deriv_book.cpp code change was needed for this kind (see the task report's
// exhaustiveness audit); this is the positive proof.
TEST(DerivBook, PricesGammaSwapAgainstSurfaceSet) {
  const atx::vol::PricedSurface ps = atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, 0.30);
  const atx::vol::PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  DerivPosition p0{};
  p0.id = 21;
  p0.uid = 7;
  p0.qty = 2.0;
  p0.contract = gamma_swap_at(0.35);

  const DerivPosition book[] = {p0};
  const auto f = price_deriv_book(*ss, book);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows.size(), 1u);
  EXPECT_EQ(f->rows[0].status, PriceStatus::Ok);
  // sigma == 0.30 flat, ZERO carry on this fixture (testkit::make_flat_surface
  // has no q/r differential -- see FixtureActuallyRollsItsForward's own note
  // on the OTHER fixture builder above), so K_gamma == K_var == sigma^2 here
  // exactly, same closed-form identity `GammaSwap.FlatZeroCarryExact`
  // (derivatives_test.cpp) pins.
  EXPECT_NEAR(f->rows[0].fair_strike_dec, 0.09, 5e-4);  // sigma^2
  EXPECT_GT(f->rows[0].pv, 0.0);  // struck at 0, qty > 0
  EXPECT_GT(f->rows[0].greeks.vega, 0.0);
}

// Task F-2 audit point (deriv_book.cpp's key-field audit,
// `var_swap_memo_eligible`): GammaSwap rows must NEVER share the VarSwap
// book-level memo, and must bump the SEPARATE GammaSwapStripEvals counter
// (never VarSwapStripEvals -- folding the two would silently corrupt what
// VarSwapMemo.EvalCountIsPerDistinctTenorNotPerRow measures, see
// counters.hpp's own doc on that enumerator). Proven by BOTH directions at
// once: L=5 GammaSwap rows sharing one (uid,T) bump GammaSwapStripEvals
// exactly 5 times (O(L), no memo collapse to O(1)) and bump
// VarSwapStripEvals exactly 0 times.
//
// MARKS-ONLY (greeks=false), same reason `VarSwapMemo.MarksOnlyEvalCount
// IsAlsoPerDistinctTenor` above uses it: `deriv_greeks`'s OWN FD bump table
// (unrelated to this task, pre-existing) issues MANY strip evaluations per
// contract (center, spot/vol bumps, second-order cross terms, and -- the one
// that actually explains an early draft of this test measuring 75, not 5 --
// the carry-theta diagnostic's `var_swap_fair_strike` call for the injected-
// fixing rate, which `eval_bump_table` runs for EVERY DerivKind, not just
// VarSwap; see the header comment on `DerivGreeks::theta_carry`). That
// multiplicity is real and orthogonal to what this test checks (memo
// eligibility, not per-contract FD cost), so marks-only pricing (one strip
// call per row, exactly `price_gamma_swap`'s own contract) isolates it.
TEST(DerivBook, GammaSwapNeverUsesTheVarSwapMemo) {
  const PricedSurface ps = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  std::vector<DerivPosition> book;
  for (std::uint32_t i = 0; i < 5u; ++i) {
    DerivPosition p{};
    p.id = i;
    p.uid = 7;
    p.qty = 1.0;
    p.contract = gamma_swap_at(0.35);
    p.contract.strike_dec = 0.01 * static_cast<double>(i);
    p.contract.rv_spec.n_obs_done = i;  // mid-life: not fully aged, needs the strip
    // C-1 fix round 1: mid-life (0 < n_obs_done < n_obs_total) now requires a
    // seed-spot anchor to blend against (see GammaSwap.AgedBlendFailsLoud
    // WithoutSeedSpotAnchor, derivatives_test.cpp). Anchored at the surface's
    // own spot (100.0) so the rescale is a no-op and this test's point --
    // memo bypass / strip-eval counting -- stays isolated from C-1.
    p.contract.rv_spec.gamma_seed_spot = 100.0;
    book.push_back(p);
  }

  ledger::reset();
  const auto f = price_deriv_book(*ss, book, DerivConfig{}, /*greeks=*/false);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows.size(), 5u);
  for (const auto &row : f->rows) {
    EXPECT_EQ(row.status, PriceStatus::Ok);
  }
  // THE assertion that carries this test: O(L), not O(1) -- if GammaSwap ever
  // accidentally became memo-eligible (e.g. a future edit widening `kind ==
  // DerivKind::VarSwap` to something looser), this count would collapse to 1
  // instead of 5.
  EXPECT_EQ(ledger::snapshot().get(ledger::Solve::GammaSwapStripEvals), 5u);
  // And the two counters never cross-contaminate: this book has NO VarSwap
  // rows at all.
  EXPECT_EQ(ledger::snapshot().get(ledger::Solve::VarSwapStripEvals), 0u);
}

// Task F-3, the exact sibling of the test above and for the same reason.
// `var_swap_memo_eligible` (deriv_book.cpp) is a `kind ==` WHITELIST, and
// `-Wswitch -WX` does NOT police those -- a silently-widened whitelist is the
// recurring defect of this whole sprint (P-4 C-1, F-2 C-1..C-4), and it is
// precisely the class a compiler cannot catch. So it is checked by TEST.
//
// A corridor row sharing the VarSwap memo would be a WRONG NUMBER, not merely
// a lost optimization: the memo caches the FULL-SPAN strip keyed on
// (uid, T-bits, wing band), and `corridor_lo`/`corridor_hi` are NOT in that
// key, so five corridor rows would all be served one full-span K_var with
// their corridors silently discarded -- and two rows with DIFFERENT corridors
// would be served the same number.
//
// Marks-only for the same isolation reason the GammaSwap sibling documents.
TEST(DerivBook, CorridorVarSwapNeverUsesTheVarSwapMemo) {
  const PricedSurface ps = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  std::vector<DerivPosition> book;
  for (std::uint32_t i = 0; i < 5u; ++i) {
    DerivPosition p{};
    p.id = i;
    p.uid = 7;
    p.qty = 1.0;
    p.contract = corridor_var_swap_at(0.35);
    p.contract.strike_dec = 0.01 * static_cast<double>(i);
    p.contract.rv_spec.n_obs_done = i;  // mid-life: not fully aged, needs the strip
    // NON-TRIVIAL corridor accrual, per this sprint's standing lesson: the
    // in-corridor count is strictly below n_obs_done for every row past the
    // first, so a blend reading the plain leg would move these rows' totals.
    p.contract.rv_spec.n_obs_in_corridor = i / 2u;
    p.contract.rv_spec.rv_corridor_done_dec = (i / 2u) > 0u ? 0.02 : 0.0;
    p.contract.rv_spec.rv_done_dec = 0.05;
    book.push_back(p);
  }

  ledger::reset();
  const auto f = price_deriv_book(*ss, book, DerivConfig{}, /*greeks=*/false);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows.size(), 5u);
  for (const auto &row : f->rows) {
    EXPECT_EQ(row.status, PriceStatus::Ok);
  }
  // THE assertion that carries this test: O(L), not O(1). A widened memo
  // whitelist collapses this to 1.
  EXPECT_EQ(ledger::snapshot().get(ledger::Solve::CorridorVarSwapStripEvals), 5u);
  // No cross-contamination in either direction: the P-6 book-memo gate reads
  // VarSwapStripEvals, and a corridor eval folded into it would corrupt what
  // that gate measures.
  EXPECT_EQ(ledger::snapshot().get(ledger::Solve::VarSwapStripEvals), 0u);
  EXPECT_EQ(ledger::snapshot().get(ledger::Solve::GammaSwapStripEvals), 0u);

  // The corridor genuinely bound: two rows differing ONLY in corridor would be
  // served the same cached number if the memo had swallowed them. Priced
  // against the same (uid, T), a strictly narrower corridor is worth strictly
  // less -- which a shared full-span block could not reproduce.
  std::vector<DerivPosition> pair;
  DerivPosition wide{};
  wide.id = 100;
  wide.uid = 7;
  wide.qty = 1.0;
  wide.contract = corridor_var_swap_at(0.35);
  DerivPosition narrow = wide;
  narrow.id = 101;
  narrow.contract.corridor_lo = 95.0;
  narrow.contract.corridor_hi = 106.0;
  pair.push_back(wide);
  pair.push_back(narrow);

  const auto g = price_deriv_book(*ss, pair, DerivConfig{}, /*greeks=*/false);
  ASSERT_TRUE(g.has_value()) << g.error().to_string();
  ASSERT_EQ(g->rows.size(), 2u);
  EXPECT_EQ(g->rows[0].status, PriceStatus::Ok);
  EXPECT_EQ(g->rows[1].status, PriceStatus::Ok);
  EXPECT_LT(g->rows[1].fair_strike_dec, g->rows[0].fair_strike_dec);
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

// GK-C9b. A NaN total used to be a dead end -- no way to tell "1 excluded
// lane" from "every lane excluded". A book of 3: two ordinary lanes plus one
// too-short-to-roll lane (T < bumps.time_years, theta/charm NaN on that ONE
// otherwise-Ok row, mirrors DerivGreeks.RollPastExpiryLeavesThetaAndCharmNaN)
// must still poison totals.theta/charm exactly as before (semantics
// UNCHANGED) while now also naming the count: 1 lane excluded from each of
// theta/charm, 0 from vanna (second_order stays on, so vanna's own NaN
// condition never fires here).
TEST(DerivBook, ExcludedLaneCountsNameTheNaNColumn) {
  const PricedSurface ps = atx::vol::testkit::make_flat_surface(7, 100.0, 100.0, 0.30);
  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  const DerivGreekBumps bumps{};
  DerivPosition normal_a{};
  normal_a.id = 1;
  normal_a.uid = 7;
  normal_a.contract = var_swap_at(0.35);
  DerivPosition normal_b = normal_a;
  normal_b.id = 2;
  DerivPosition too_short{};
  too_short.id = 3;
  too_short.uid = 7;
  too_short.contract = var_swap_at(0.5 * bumps.time_years);  // half a day out: can't roll
  const DerivPosition book[] = {normal_a, normal_b, too_short};

  const auto f = price_deriv_book(*ss, book);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows.size(), 3u);
  EXPECT_EQ(f->rows[0].status, PriceStatus::Ok);
  EXPECT_EQ(f->rows[1].status, PriceStatus::Ok);
  EXPECT_EQ(f->rows[2].status, PriceStatus::Ok);
  EXPECT_TRUE(std::isfinite(f->rows[0].greeks.theta));
  EXPECT_TRUE(std::isfinite(f->rows[1].greeks.theta));
  EXPECT_TRUE(std::isnan(f->rows[2].greeks.theta));
  EXPECT_TRUE(std::isnan(f->rows[2].greeks.charm));

  // Semantics unchanged: the one NaN lane still poisons the whole column.
  EXPECT_TRUE(std::isnan(f->totals.theta));
  EXPECT_TRUE(std::isnan(f->totals.charm));
  EXPECT_TRUE(std::isfinite(f->totals.vega));  // an untouched column stays sane

  // The count now names why.
  EXPECT_EQ(f->n_theta_excluded, 1u);
  EXPECT_EQ(f->n_charm_excluded, 1u);
  EXPECT_EQ(f->n_vanna_excluded, 0u);
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

// ── Task P-6 (GK-P book memo) ───────────────────────────────────────────────

[[nodiscard]] std::uint64_t raw_bits(double x) noexcept {
  std::uint64_t b = 0;
  std::memcpy(&b, &x, sizeof b);
  return b;
}

// A book of L VarSwap rows sharing (uid, T) must cost O(distinct (uid,T)
// groups) strip evaluations, not O(L) rows -- the core performance claim.
// `counters::ledger::Solve::VarSwapStripEvals` bumps once per actual
// `var_swap_fair_strike` quadrature (see that enumerator's own doc,
// counters.hpp); comparing a 10-row/2-tenor book's delta against the SUM of
// two single-row probes (one per tenor) proves the shared cost is paid
// exactly once per tenor, independent of how many rows share it.
TEST(VarSwapMemo, EvalCountIsPerDistinctTenorNotPerRow) {
  const PricedSurface ps = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  DerivPosition probe1{};
  probe1.id = 1;
  probe1.uid = 7;
  probe1.qty = 1.0;
  probe1.contract = var_swap_at(0.35);
  probe1.contract.rv_spec.n_obs_done = 10u; // mid-life: not fully aged, needs the strip
  const DerivPosition probe1_book[] = {probe1};

  ledger::reset();
  const auto f_probe1 = price_deriv_book(*ss, probe1_book);
  ASSERT_TRUE(f_probe1.has_value()) << f_probe1.error().to_string();
  ASSERT_EQ(f_probe1->rows[0].status, PriceStatus::Ok);
  const std::uint64_t per_group_t1 = ledger::snapshot().get(ledger::Solve::VarSwapStripEvals);
  ASSERT_GT(per_group_t1, 0u);

  DerivPosition probe2 = probe1;
  probe2.contract.maturity_t = 0.5;
  const DerivPosition probe2_book[] = {probe2};
  ledger::reset();
  const auto f_probe2 = price_deriv_book(*ss, probe2_book);
  ASSERT_TRUE(f_probe2.has_value()) << f_probe2.error().to_string();
  ASSERT_EQ(f_probe2->rows[0].status, PriceStatus::Ok);
  const std::uint64_t per_group_t2 = ledger::snapshot().get(ledger::Solve::VarSwapStripEvals);
  ASSERT_GT(per_group_t2, 0u);

  // 10 rows: 5 at T=0.35, 5 at T=0.5, each with its OWN strike/notional/age --
  // exactly the "L rows sharing (uid,T,cfg)" scenario the book memo targets.
  std::vector<DerivPosition> book10;
  for (std::uint32_t i = 0; i < 5u; ++i) {
    DerivPosition p = probe1;
    p.id = i;
    p.contract.strike_dec = 0.01 * static_cast<double>(i);
    p.contract.notional = 1.0e6 + 1.0e5 * static_cast<double>(i);
    p.contract.rv_spec.n_obs_done = i;
    book10.push_back(p);
  }
  for (std::uint32_t i = 0; i < 5u; ++i) {
    DerivPosition p = probe2;
    p.id = 100u + i;
    p.contract.strike_dec = 0.02 * static_cast<double>(i);
    p.contract.notional = 2.0e6 + 1.0e5 * static_cast<double>(i);
    p.contract.rv_spec.n_obs_done = i + 1u;
    book10.push_back(p);
  }
  ASSERT_EQ(book10.size(), 10u);

  ledger::reset();
  const auto f10 = price_deriv_book(*ss, book10);
  ASSERT_TRUE(f10.has_value()) << f10.error().to_string();
  ASSERT_EQ(f10->rows.size(), 10u);
  for (const auto &row : f10->rows) {
    EXPECT_EQ(row.status, PriceStatus::Ok);
  }
  const std::uint64_t evals_10rows = ledger::snapshot().get(ledger::Solve::VarSwapStripEvals);

  // EXACT, not approximate: 2 groups' worth, independent of how many rows
  // share each one -- not just "less than 10x", but precisely the sum of the
  // two single-row probes.
  EXPECT_EQ(evals_10rows, per_group_t1 + per_group_t2);
  EXPECT_LT(evals_10rows, 10u * per_group_t1); // well under the naive 10x
}

// Marks-only (greeks=false) pays for the strip alone (no bump table), and the
// memo still collapses it to one build per (uid,T) group.
TEST(VarSwapMemo, MarksOnlyEvalCountIsAlsoPerDistinctTenor) {
  const PricedSurface ps = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  DerivPosition probe{};
  probe.id = 1;
  probe.uid = 7;
  probe.qty = 1.0;
  probe.contract = var_swap_at(0.35);
  probe.contract.rv_spec.n_obs_done = 10u;
  const DerivPosition probe_book[] = {probe};

  ledger::reset();
  const auto f_probe =
      price_deriv_book(*ss, probe_book, DerivConfig{}, /*greeks=*/false);
  ASSERT_TRUE(f_probe.has_value()) << f_probe.error().to_string();
  const std::uint64_t per_group = ledger::snapshot().get(ledger::Solve::VarSwapStripEvals);
  ASSERT_GT(per_group, 0u);

  std::vector<DerivPosition> book;
  for (std::uint32_t i = 0; i < 6u; ++i) {
    DerivPosition p = probe;
    p.id = i;
    p.contract.strike_dec = 0.01 * static_cast<double>(i);
    p.contract.rv_spec.n_obs_done = i;
    book.push_back(p);
  }

  ledger::reset();
  const auto f = price_deriv_book(*ss, book, DerivConfig{}, /*greeks=*/false);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  for (const auto &row : f->rows) {
    EXPECT_EQ(row.status, PriceStatus::Ok);
  }
  const std::uint64_t evals = ledger::snapshot().get(ledger::Solve::VarSwapStripEvals);
  EXPECT_EQ(evals, per_group); // one tenor -> one build, regardless of row count
}

// Fix round 1, I-2 regression guard. An ALL-fully-aged book must cost the
// memo NOTHING: the unmemoized path (`price_var_swap`) skips the strip
// entirely once `n_obs_done == n_obs_total`, and the group-build helper
// (`ensure_var_swap_center_strip`, derivatives.cpp) must honour that SAME
// gate rather than resolving the (expensive, K-quadrature) strip just
// because a group happens to exist. Before the fix, `ensure_var_swap_center`
// ran the strip unconditionally on first touch regardless of aging --
// inverting the point of the memo (a real, cheap workload made slower).
// Covers both greeks=true and greeks=false, two tenors, two uids: zero
// strip evaluations in every case, not merely "fewer than 10x".
TEST(VarSwapMemo, AllFullyAgedBookCostsTheMemoZeroStripEvals) {
  const PricedSurface ps7 = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const PricedSurface ps9 = make_carry_skew_surface(9, 100.0, 0.25, 0.01);
  const PricedSurface *arr[] = {&ps7, &ps9};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  std::vector<DerivPosition> book;
  std::uint32_t next_id = 0;
  for (const std::uint32_t uid : {7u, 9u}) {
    for (const double T : {0.35, 0.5}) {
      for (std::uint32_t i = 0; i < 3u; ++i) {
        DerivPosition p{};
        p.id = next_id++;
        p.uid = uid;
        p.qty = 1.0;
        p.contract = var_swap_at(T);
        p.contract.strike_dec = 0.01 * static_cast<double>(i);
        p.contract.notional = 1.0e6 + 1.0e5 * static_cast<double>(i);
        // Fully aged: n_obs_done == n_obs_total, so the unmemoized path never
        // touches the strip for this row either.
        p.contract.rv_spec.n_obs_done = p.contract.rv_spec.n_obs_total;
        book.push_back(p);
      }
    }
  }
  ASSERT_EQ(book.size(), 12u);

  ledger::reset();
  const auto f_greeks = price_deriv_book(*ss, book, DerivConfig{}, /*greeks=*/true);
  ASSERT_TRUE(f_greeks.has_value()) << f_greeks.error().to_string();
  ASSERT_EQ(f_greeks->rows.size(), 12u);
  for (const auto &row : f_greeks->rows) {
    EXPECT_EQ(row.status, PriceStatus::Ok);
  }
  EXPECT_EQ(ledger::snapshot().get(ledger::Solve::VarSwapStripEvals), 0u);

  ledger::reset();
  const auto f_marks = price_deriv_book(*ss, book, DerivConfig{}, /*greeks=*/false);
  ASSERT_TRUE(f_marks.has_value()) << f_marks.error().to_string();
  for (const auto &row : f_marks->rows) {
    EXPECT_EQ(row.status, PriceStatus::Ok);
  }
  EXPECT_EQ(ledger::snapshot().get(ledger::Solve::VarSwapStripEvals), 0u);
}

// THE hard gate: memoized book rows are bit-identical to calling the
// unmemoized `detail::deriv_greeks_on_ref` directly, once per row, with no
// shared block at all. Exercises the FD path (the bumps default) with
// second_order and carry_theta on, across TWO surfaces (uid 7 and 9), two
// tenors each, and three rows per (uid,T) group that differ in EVERY field
// the memo's key deliberately omits: strike_dec, notional, qty, and rv_spec's
// own aging (n_obs_done) -- exactly "L rows sharing (uid,T,cfg)". One row is
// deliberately too short to roll (theta/charm/carry-theta all NaN on that
// row), so this ALSO pins that the memo's NaN payload -- not just its
// finite-value bits -- matches the unmemoized path (see
// `deriv_greeks_var_swap_shared`'s own comment, derivatives.cpp, on why NaN
// is produced by UNCONDITIONAL arithmetic rather than an explicit literal).
TEST(VarSwapMemo, RowsAreBitIdenticalToTheUnmemoizedPerRowPath) {
  const PricedSurface psA = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const PricedSurface psB = make_carry_skew_surface(9, 120.0, 0.22, -0.01);
  const PricedSurface *arr[] = {&psA, &psB};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  const DerivGreekBumps bumps{}; // FD, second_order + carry_theta on (defaults)
  std::vector<DerivPosition> book;
  const double tenors[] = {0.35, 0.5};
  const std::uint32_t uids[] = {7u, 9u};
  std::uint64_t next_id = 1;
  for (std::uint32_t uid : uids) {
    for (double T : tenors) {
      for (std::uint32_t i = 0; i < 3u; ++i) {
        DerivPosition p{};
        p.id = next_id++;
        p.uid = uid;
        p.qty = 1.0 + 0.5 * static_cast<double>(i);
        p.contract = var_swap_at(T);
        p.contract.strike_dec = 0.005 * static_cast<double>(i);
        p.contract.notional = 1.0e6 * (1.0 + 0.25 * static_cast<double>(i));
        p.contract.rv_spec.n_obs_done = i * 7u; // varies aging, all still mid-life
        book.push_back(p);
      }
    }
  }
  // One row too short to roll: theta/charm/theta_carry/theta_zero_fixing NaN.
  DerivPosition too_short{};
  too_short.id = next_id++;
  too_short.uid = 7u;
  too_short.qty = 1.0;
  too_short.contract = var_swap_at(0.5 * bumps.time_years);
  book.push_back(too_short);
  // Shares (uid=7, T=0.35) with the FIRST group above but is FULLY AGED:
  // must bypass the shared block's strip entirely and still agree bit for
  // bit with the unmemoized fully-aged closed form.
  DerivPosition fully_aged{};
  fully_aged.id = next_id++;
  fully_aged.uid = 7u;
  fully_aged.qty = -2.0;
  fully_aged.contract = var_swap_at(0.35);
  fully_aged.contract.rv_spec.n_obs_done = fully_aged.contract.rv_spec.n_obs_total;
  fully_aged.contract.rv_spec.rv_done_dec = 0.05;
  book.push_back(fully_aged);

  const auto f_memo = price_deriv_book(*ss, book, DerivConfig{}, /*greeks=*/true, bumps);
  ASSERT_TRUE(f_memo.has_value()) << f_memo.error().to_string();
  ASSERT_EQ(f_memo->rows.size(), book.size());

  double ref_pv_sum = 0.0;
  for (std::size_t i = 0; i < book.size(); ++i) {
    const DerivPosition &p = book[i];
    const SurfaceRef ref = ss->find(p.uid);
    ASSERT_NE(ref, nullptr);
    const auto g = atx::vol::detail::deriv_greeks_on_ref(ref, p.contract, DerivConfig{}, bumps,
                                                          std::nullopt);
    ASSERT_TRUE(g.has_value()) << "row " << i << ": " << g.error().to_string();

    const auto &row = f_memo->rows[i];
    ASSERT_EQ(row.status, PriceStatus::Ok) << "row " << i;
    EXPECT_EQ(raw_bits(row.pv), raw_bits(p.qty * g->pv)) << "row " << i << " pv";
    EXPECT_EQ(raw_bits(row.fair_strike_dec), raw_bits(g->quote.fair_strike_dec))
        << "row " << i << " fair_strike_dec";
    EXPECT_EQ(raw_bits(row.greeks.delta), raw_bits(p.qty * g->delta)) << "row " << i << " delta";
    EXPECT_EQ(raw_bits(row.greeks.gamma), raw_bits(p.qty * g->gamma)) << "row " << i << " gamma";
    EXPECT_EQ(raw_bits(row.greeks.vega), raw_bits(p.qty * g->vega)) << "row " << i << " vega";
    EXPECT_EQ(raw_bits(row.greeks.volga), raw_bits(p.qty * g->volga)) << "row " << i << " volga";
    EXPECT_EQ(raw_bits(row.greeks.vanna), raw_bits(p.qty * g->vanna)) << "row " << i << " vanna";
    EXPECT_EQ(raw_bits(row.greeks.theta), raw_bits(p.qty * g->theta)) << "row " << i << " theta";
    EXPECT_EQ(raw_bits(row.greeks.rho), raw_bits(p.qty * g->rho)) << "row " << i << " rho";
    EXPECT_EQ(raw_bits(row.greeks.charm), raw_bits(p.qty * g->charm)) << "row " << i << " charm";
    EXPECT_EQ(raw_bits(row.greeks.theta_carry), raw_bits(p.qty * g->theta_carry))
        << "row " << i << " theta_carry";
    EXPECT_EQ(raw_bits(row.greeks.theta_zero_fixing), raw_bits(p.qty * g->theta_zero_fixing))
        << "row " << i << " theta_zero_fixing";
    ref_pv_sum += p.qty * g->pv;
  }
  // Totals: the SAME serial sum in book order the frame's own accumulate()
  // performs, so this is bit-identical too, not just "close".
  EXPECT_EQ(raw_bits(f_memo->totals.pv), raw_bits(ref_pv_sum));
}

// P-4 interaction, analytic branch ON: `bumps.method = AnalyticStrip` selects
// the shared block's raw analytic sub-block (`have_analytic`) instead of the
// FD bump grid -- still resolved ONCE per (uid,T) group, still bit-identical
// to the unmemoized path, and cheaper (no market-bump grid at all).
TEST(VarSwapMemo, AnalyticStripPathIsAlsoMemoizedAndBitIdentical) {
  const PricedSurface ps = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  DerivGreekBumps bumps{};
  bumps.method = DerivGreekMethod::AnalyticStrip;

  DerivPosition probe{};
  probe.id = 1;
  probe.uid = 7;
  probe.qty = 1.0;
  probe.contract = var_swap_at(0.35);
  probe.contract.rv_spec.n_obs_done = 5u;
  const DerivPosition probe_book[] = {probe};
  ledger::reset();
  const auto f_probe = price_deriv_book(*ss, probe_book, DerivConfig{}, /*greeks=*/true, bumps);
  ASSERT_TRUE(f_probe.has_value()) << f_probe.error().to_string();
  const std::uint64_t per_group = ledger::snapshot().get(ledger::Solve::VarSwapStripEvals);
  ASSERT_GT(per_group, 0u);

  std::vector<DerivPosition> book;
  for (std::uint32_t i = 0; i < 4u; ++i) {
    DerivPosition p = probe;
    p.id = i;
    p.contract.strike_dec = 0.01 * static_cast<double>(i);
    p.contract.notional = 1.0e6 + 5.0e4 * static_cast<double>(i);
    p.contract.rv_spec.n_obs_done = i;
    book.push_back(p);
  }

  ledger::reset();
  const auto f = price_deriv_book(*ss, book, DerivConfig{}, /*greeks=*/true, bumps);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  const std::uint64_t evals = ledger::snapshot().get(ledger::Solve::VarSwapStripEvals);
  EXPECT_EQ(evals, per_group); // 4 rows, 1 tenor -> exactly one group's cost

  for (std::size_t i = 0; i < book.size(); ++i) {
    const DerivPosition &p = book[i];
    const SurfaceRef ref = ss->find(p.uid);
    const auto g = atx::vol::detail::deriv_greeks_on_ref(ref, p.contract, DerivConfig{}, bumps,
                                                          std::nullopt);
    ASSERT_TRUE(g.has_value()) << "row " << i;
    ASSERT_EQ(f->rows[i].status, PriceStatus::Ok) << "row " << i;
    EXPECT_EQ(raw_bits(f->rows[i].greeks.delta), raw_bits(p.qty * g->delta)) << "row " << i;
    EXPECT_EQ(raw_bits(f->rows[i].greeks.gamma), raw_bits(p.qty * g->gamma)) << "row " << i;
    EXPECT_EQ(raw_bits(f->rows[i].greeks.vega), raw_bits(p.qty * g->vega)) << "row " << i;
    EXPECT_EQ(raw_bits(f->rows[i].greeks.vanna), raw_bits(p.qty * g->vanna)) << "row " << i;
    EXPECT_EQ(raw_bits(f->rows[i].greeks.volga), raw_bits(p.qty * g->volga)) << "row " << i;
    EXPECT_EQ(raw_bits(f->rows[i].pv), raw_bits(p.qty * g->pv)) << "row " << i;
  }
}

// P-3/P-4-class key-omission guard: two rows share (uid, T) but the caller's
// `WingBandResolver` is (deliberately, adversarially) NOT a pure function of
// uid -- it alternates on every call. A real resolver is documented as
// "uid -> band" (pure per uid), so this never happens in practice, but the
// memo keys on the RESOLVED band anyway (see deriv_book.cpp's key-field
// audit) rather than assuming purity; this proves that choice actually
// prevents row 2 from silently reading row 1's cached band.
TEST(VarSwapMemo, WingBandDifferenceIsInTheKeyNotJustUid) {
  const PricedSurface ps = make_steep_wing_surface(21, 100.0, 0.30);
  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  DerivPosition p0{};
  p0.id = 1;
  p0.uid = 21;
  p0.qty = 1.0;
  p0.contract = var_swap_at(0.35);
  DerivPosition p1 = p0;
  p1.id = 2;
  const DerivPosition book[] = {p0, p1};

  const double band_latency = atx::vol::certified_wing_half_band(atx::vol::FitQualityMode::Latency);
  const double band_accuracy =
      atx::vol::certified_wing_half_band(atx::vol::FitQualityMode::Accuracy);
  ASSERT_NE(band_latency, band_accuracy);
  int call = 0;
  const atx::vol::WingBandResolver resolver = [&](std::uint32_t) -> std::optional<double> {
    return (call++ % 2 == 0) ? std::optional<double>{band_latency}
                            : std::optional<double>{band_accuracy};
  };
  const auto f =
      price_deriv_book(*ss, book, DerivConfig{}, /*greeks=*/true, DerivGreekBumps{}, resolver);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  ASSERT_EQ(f->rows[0].status, PriceStatus::Ok);
  ASSERT_EQ(f->rows[1].status, PriceStatus::Ok);
  EXPECT_DOUBLE_EQ(f->rows[0].greeks.quote.resolved_wing_clamp, band_latency);
  EXPECT_DOUBLE_EQ(f->rows[1].greeks.quote.resolved_wing_clamp, band_accuracy);
  EXPECT_NE(f->rows[0].fair_strike_dec, f->rows[1].fair_strike_dec);
}

// `cfg.discrete_correction_mode != None` gates a VarSwap row OUT of the memo
// entirely (see deriv_book.cpp's key-field audit -- reproducing the
// QUADRATIC-in-K_var, per-row-n_remaining correction is out of this task's
// scope, exactly like P-4's own AnalyticStrip scope exclusion). Rows must
// still price CORRECTLY (matching the unmemoized reference), and the eval
// count must NOT show the O(distinct tenors) reduction -- confirming the
// gate actually routes these rows around the shared block rather than
// (incorrectly) serving them a raw-strip block that never saw the
// correction.
TEST(VarSwapMemo, DiscreteCorrectionModeBypassesTheMemoButStaysCorrect) {
  const PricedSurface ps = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());

  DerivConfig cfg{};
  cfg.discrete_correction_mode = DerivDiscreteCorrection::Diffusion1OverN;

  std::vector<DerivPosition> book;
  for (std::uint32_t i = 0; i < 4u; ++i) {
    DerivPosition p{};
    p.id = i;
    p.uid = 7;
    p.qty = 1.0;
    p.contract = var_swap_at(0.35);
    p.contract.strike_dec = 0.01 * static_cast<double>(i);
    p.contract.rv_spec.n_obs_done = i;
    book.push_back(p);
  }

  ledger::reset();
  const auto f = price_deriv_book(*ss, book, cfg);
  ASSERT_TRUE(f.has_value()) << f.error().to_string();
  const std::uint64_t evals = ledger::snapshot().get(ledger::Solve::VarSwapStripEvals);

  for (std::size_t i = 0; i < book.size(); ++i) {
    const DerivPosition &p = book[i];
    const SurfaceRef ref = ss->find(p.uid);
    const auto g = atx::vol::detail::deriv_greeks_on_ref(ref, p.contract, cfg, DerivGreekBumps{},
                                                          std::nullopt);
    ASSERT_TRUE(g.has_value()) << "row " << i;
    ASSERT_EQ(f->rows[i].status, PriceStatus::Ok) << "row " << i;
    EXPECT_EQ(raw_bits(f->rows[i].pv), raw_bits(p.qty * g->pv)) << "row " << i;
    EXPECT_EQ(raw_bits(f->rows[i].greeks.delta), raw_bits(p.qty * g->delta)) << "row " << i;
  }
  // NOT the memoized O(1 group) count: correction mode routes every row
  // around the shared block, so this scales with row count, same as before
  // this task. A generous upper bound (not an exact multiple, since a single
  // row's own eval count already varies with second_order/carry_theta) --
  // the point is "not collapsed to ~1 group's worth".
  EXPECT_GT(evals, 4u);
}

// Fix round 2, I-3 direct coverage. `deriv_price_var_swap_on_ref_shared` /
// `deriv_greeks_var_swap_on_ref_shared` (deriv_ref_bridge.hpp) require
// `contract.kind == VarSwap` and `cfg.discrete_correction_mode == None` --
// `price_deriv_book`'s own `var_swap_memo_eligible` gate never lets a
// violating call reach them, so that path alone can never exercise the
// rejection. Calling both entry points directly, out-of-band from
// `price_deriv_book`, with each precondition violated in turn.
TEST(VarSwapMemo, SharedEntryPointsRejectOutOfScopeContractsDirectly) {
  const PricedSurface ps = make_carry_skew_surface(7, 100.0, 0.30, 0.02);
  const PricedSurface *arr[] = {&ps};
  auto ss = SurfaceSet::create(arr);
  ASSERT_TRUE(ss.has_value());
  const SurfaceRef ref = ss->find(7);
  ASSERT_TRUE(ref.valid());

  DerivContract not_var_swap = var_swap_at(0.35);
  not_var_swap.kind = DerivKind::VolSwap;
  DerivContract discretely_corrected = var_swap_at(0.35);
  DerivConfig discrete_cfg{};
  discrete_cfg.discrete_correction_mode = DerivDiscreteCorrection::Diffusion1OverN;

  atx::vol::detail::VarSwapSharedBlock block{};

  const auto price_wrong_kind = atx::vol::detail::deriv_price_var_swap_on_ref_shared(
      ref, not_var_swap, DerivConfig{}, block, std::nullopt);
  ASSERT_FALSE(price_wrong_kind.has_value());
  EXPECT_EQ(price_wrong_kind.error().code(), atx::vol::ErrorCode::InvalidArgument);

  const auto price_wrong_correction = atx::vol::detail::deriv_price_var_swap_on_ref_shared(
      ref, discretely_corrected, discrete_cfg, block, std::nullopt);
  ASSERT_FALSE(price_wrong_correction.has_value());
  EXPECT_EQ(price_wrong_correction.error().code(), atx::vol::ErrorCode::InvalidArgument);

  const auto greeks_wrong_kind = atx::vol::detail::deriv_greeks_var_swap_on_ref_shared(
      ref, not_var_swap, DerivConfig{}, DerivGreekBumps{}, block, std::nullopt);
  ASSERT_FALSE(greeks_wrong_kind.has_value());
  EXPECT_EQ(greeks_wrong_kind.error().code(), atx::vol::ErrorCode::InvalidArgument);

  const auto greeks_wrong_correction = atx::vol::detail::deriv_greeks_var_swap_on_ref_shared(
      ref, discretely_corrected, discrete_cfg, DerivGreekBumps{}, block, std::nullopt);
  ASSERT_FALSE(greeks_wrong_correction.has_value());
  EXPECT_EQ(greeks_wrong_correction.error().code(), atx::vol::ErrorCode::InvalidArgument);

  // Rejection must not mutate `block`: every field still reads as freshly
  // default-constructed (matches I-3's own doc, "before `carry_from_ref` so a
  // rejected call cannot mutate `block`").
  EXPECT_FALSE(block.df_resolved);
  EXPECT_FALSE(block.strip_resolved);
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
