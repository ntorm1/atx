// margin.hpp — Reg-T short-option margin + scenario-grid portfolio margin
// (Task B2, backtest-production-lakehouse sprint).
//
// Two groups of gates:
//
//   1. RegTHandComputed*  — regt_short_option_margin against hand-derived
//      dollar figures (shown in each test's comment), covering every formula
//      branch: OTM/ITM x call/put, and the deep-OTM floor on both sides.
//   2. ScenarioMargin*    — scenario_margin against the KNOWN worst cell of a
//      pinned 2-lot short straddle on the testkit flat-surface fixture,
//      computed by calling scenario_grid directly with the SAME grid and
//      taking min(pnl) — proving scenario_margin reuses scenario_grid's
//      revaluation rather than re-deriving one of its own.
//
// Engine-wiring tests (BacktestResult::margin_required, RunConfig::
// margin_breach) are appended separately below the "── Engine wiring ──"
// banner (Task B2 commit 2) and need backtest.hpp; the two banners are kept
// physically separate so the pure-function gates above compile and pass
// standalone against margin.hpp alone, per the sprint's "pure functions
// first" discipline.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <vector>

#include "atx/vol/margin.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/scenario_grid.hpp"
#include "support/analytics_fixture.hpp" // testkit::make_flat_surface

using namespace atx::vol;

namespace {

[[nodiscard]] SurfaceSet set_of(const std::vector<const PricedSurface *> &v) {
  auto ss = SurfaceSet::create(v);
  EXPECT_TRUE(ss.has_value());
  return std::move(*ss);
}

} // namespace

// ── 1. regt_short_option_margin: hand-computed formula branches ────────────
//
// Formula (margin.hpp): OTM = max(K-S,0) call / max(S-K,0) put;
// floor = 0.10*S call / 0.10*K put; margin = max(0.20*S - OTM, floor)*mult +
// premium. `premium` is the CONTRACT dollar premium (per-share mark * mult).

// OTM put: S=100, K=90 (put strike below spot => OTM), mult=100,
// premium=$150 (contract dollars).
//   OTM   = max(100-90, 0)        = 10
//   base  = 0.20*100 - 10         = 10
//   floor = 0.10*90 (put => K)    = 9
//   max(10, 9) = 10  =>  10*100 + 150 = 1150
TEST(RegTShortOptionMargin, OtmPut) {
  const double m = regt_short_option_margin(/*spot=*/100.0, /*strike=*/90.0, /*premium=*/150.0,
                                            /*mult=*/100.0, Side::Put);
  EXPECT_DOUBLE_EQ(m, 1150.0);
}

// ITM call: S=100, K=90 (call strike below spot => ITM), mult=100,
// premium=$1200.
//   OTM   = max(90-100, 0)        = 0
//   base  = 0.20*100 - 0          = 20
//   floor = 0.10*100 (call => S)  = 10
//   max(20, 10) = 20  =>  20*100 + 1200 = 3200
TEST(RegTShortOptionMargin, ItmCall) {
  const double m = regt_short_option_margin(/*spot=*/100.0, /*strike=*/90.0, /*premium=*/1200.0,
                                            /*mult=*/100.0, Side::Call);
  EXPECT_DOUBLE_EQ(m, 3200.0);
}

// Deep-OTM put (floor governs): S=100, K=50, mult=100, premium=$5.
//   OTM   = max(100-50, 0)        = 50
//   base  = 0.20*100 - 50         = -30   (negative -- the floor must win)
//   floor = 0.10*50 (put => K)    = 5
//   max(-30, 5) = 5  =>  5*100 + 5 = 505
TEST(RegTShortOptionMargin, DeepOtmPutFloor) {
  const double m = regt_short_option_margin(/*spot=*/100.0, /*strike=*/50.0, /*premium=*/5.0,
                                            /*mult=*/100.0, Side::Put);
  EXPECT_DOUBLE_EQ(m, 505.0);
}

// Deep-OTM call (floor governs): S=100, K=150, mult=100, premium=$5.
//   OTM   = max(150-100, 0)       = 50
//   base  = 0.20*100 - 50         = -30
//   floor = 0.10*100 (call => S)  = 10
//   max(-30, 10) = 10  =>  10*100 + 5 = 1005
TEST(RegTShortOptionMargin, DeepOtmCallFloor) {
  const double m = regt_short_option_margin(/*spot=*/100.0, /*strike=*/150.0, /*premium=*/5.0,
                                            /*mult=*/100.0, Side::Call);
  EXPECT_DOUBLE_EQ(m, 1005.0);
}

// OTM call (mirrors OtmPut, other side): S=100, K=110, mult=100, premium=$80.
//   OTM   = max(110-100, 0)       = 10
//   base  = 0.20*100 - 10         = 10
//   floor = 0.10*100 (call => S)  = 10
//   max(10, 10) = 10  =>  10*100 + 80 = 1080
TEST(RegTShortOptionMargin, OtmCall) {
  const double m = regt_short_option_margin(/*spot=*/100.0, /*strike=*/110.0, /*premium=*/80.0,
                                            /*mult=*/100.0, Side::Call);
  EXPECT_DOUBLE_EQ(m, 1080.0);
}

// ITM put (mirrors ItmCall, other side): S=100, K=110, mult=100, premium=$1150.
//   OTM   = max(100-110, 0)       = 0
//   base  = 0.20*100 - 0          = 20
//   floor = 0.10*110 (put => K)   = 11
//   max(20, 11) = 20  =>  20*100 + 1150 = 3150
TEST(RegTShortOptionMargin, ItmPut) {
  const double m = regt_short_option_margin(/*spot=*/100.0, /*strike=*/110.0, /*premium=*/1150.0,
                                            /*mult=*/100.0, Side::Put);
  EXPECT_DOUBLE_EQ(m, 3150.0);
}

// Non-standard multiplier is honored as given (no defaulting): mult=10 instead
// of the usual 100, everything else as OtmPut.
//   max(10, 9) = 10  =>  10*10 + 15 = 115
TEST(RegTShortOptionMargin, NonStandardMultiplierIsUsedAsGiven) {
  const double m = regt_short_option_margin(/*spot=*/100.0, /*strike=*/90.0, /*premium=*/15.0,
                                            /*mult=*/10.0, Side::Put);
  EXPECT_DOUBLE_EQ(m, 115.0);
}

// ── 2. scenario_margin: worst cell of a pinned 2-lot book == scenario_grid's own worst cell ──

namespace {

// A short straddle on the testkit flat-vol fixture (sigma=0.20 flat, S=fwd=100):
// -1 call + -1 put, both struck ATM, same tenor. A short straddle's worst loss
// is at the largest |spot move| combined with the largest vol increase (both
// inflate the option values the position is short), so the pinned 3x3 grid's
// worst cell is expected at a spot extreme x +vol_shock -- this test does not
// ASSUME that placement, it independently recomputes the whole grid via
// scenario_grid itself and compares scenario_margin against ITS min, so the
// two can never silently diverge.
[[nodiscard]] std::vector<Position> short_straddle() {
  return {
      Position{0, OptionContract{1, 100.0, 0.25, Side::Call}, -1.0, 100.0},
      Position{1, OptionContract{1, 100.0, 0.25, Side::Put}, -1.0, 100.0},
  };
}

} // namespace

TEST(ScenarioMargin, EqualsWorstCellOfPinned2LotBookOnFlatSurface) {
  const PricedSurface flat = testkit::make_flat_surface(/*uid=*/1, /*S=*/100.0, /*fwd=*/100.0,
                                                        /*sigma=*/0.20);
  const SurfaceSet base = set_of({&flat});
  const std::vector<Position> book = short_straddle();

  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value()) << pf.error().to_string();
  const PortfolioPricer pricer(std::move(*pf));

  MarginScenarioSpec spec; // defaults: +/-15% spot, +/-10 vol pts (see margin.hpp)
  const Result<double> margin = scenario_margin(pricer, base, spec);
  ASSERT_TRUE(margin.has_value()) << margin.error().to_string();

  // Independent oracle: the SAME grid shape scenario_margin builds internally,
  // fed straight into scenario_grid (no margin.hpp code involved), so this is
  // a genuine cross-check rather than a restatement of the implementation.
  ScenarioGridSpec grid;
  grid.spot_pct = {-0.15, 0.0, 0.15};
  grid.vol_bump = {-0.10, 0.0, 0.10};
  const Result<ScenarioGridResult> oracle = scenario_grid(book, base, grid);
  ASSERT_TRUE(oracle.has_value()) << oracle.error().to_string();
  ASSERT_EQ(oracle->n_cells(), 9u);

  double worst = 0.0;
  for (const double pnl : oracle->pnl) {
    if (pnl < worst) {
      worst = pnl;
    }
  }
  // A short straddle must actually lose somewhere on a +/-15%/+/-10vol grid,
  // or this test is not exercising the worst-cell reduction at all.
  ASSERT_LT(worst, 0.0);
  EXPECT_DOUBLE_EQ(*margin, -worst);
}

// An empty book prices to an all-zero scenario grid (scenario_grid.hpp: "An
// empty book yields an all-zero grid"), so its worst cell is 0.0 and
// scenario_margin reports exactly 0.0 -- the floor branch, exercised without
// having to reason about the sign of any real option's P&L under a large
// shock (a LONG position is not a safe stand-in for "never loses": a long
// straddle, for instance, loses real value on a vol-DOWN cell, since it is
// long vega -- see the worst-cell test above, which is exactly that book).
TEST(ScenarioMargin, EmptyBookFloorsAtZero) {
  const PricedSurface flat = testkit::make_flat_surface(/*uid=*/1, /*S=*/100.0, /*fwd=*/100.0,
                                                        /*sigma=*/0.20);
  const SurfaceSet base = set_of({&flat});
  const std::vector<Position> book; // empty

  auto pf = Portfolio::create(book);
  ASSERT_TRUE(pf.has_value()) << pf.error().to_string();
  const PortfolioPricer pricer(std::move(*pf));

  MarginScenarioSpec spec;
  const Result<double> margin = scenario_margin(pricer, base, spec);
  ASSERT_TRUE(margin.has_value()) << margin.error().to_string();
  EXPECT_DOUBLE_EQ(*margin, 0.0);
}
