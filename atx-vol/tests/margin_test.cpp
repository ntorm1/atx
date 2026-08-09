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
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/backtest.hpp" // Task B2 commit 2: RunConfig::margin_breach, BacktestResult::margin_required
#include "atx/vol/corpus.hpp"
#include "atx/vol/margin.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/scenario_grid.hpp"
#include "atx/vol/strategy.hpp"
#include "atx/vol/surface_archive.hpp"
#include "support/analytics_fixture.hpp" // testkit::make_flat_surface

using namespace atx::vol;
namespace fs = std::filesystem;

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

// ── Engine wiring (Task B2 commit 2) ────────────────────────────────────────
//
// Two gates:
//   1. MarginRequiredRecordedPerStepAndMatchesRegTFormula -- runs the FIXED-
//      BOOK (B0) overload over a flat-vol corpus with one short put lot and
//      cross-checks EVERY recorded row's `margin_required` against an
//      independently computed regt_short_option_margin call (same public
//      function commit 1 already pins) fed the SAME surface's fair_value at
//      that row's date -- so this proves the engine WIRES the pure function
//      correctly (right spot/strike/T/premium/qty each row), not that the
//      formula itself is right (commit 1 already covers that).
//   2. HaltBreachAbortsWithNamedRow -- RunConfig::margin_breach == Halt
//      refuses the run (Err, not a recorded row) the first time
//      margin_required exceeds available capital, naming the offending row's
//      date in the error message.
//   3. StrategyOverloadHaltUsesCashNotAFixedZero -- the strategy overload's
//      "available capital" is the ENGINE'S OWN cash ledger (opening a short
//      put credits it with the premium received, which structurally undershoots
//      Reg-T's collateral requirement by exactly the max(0.20*S-OTM,floor)*mult
//      term -- see margin.hpp), not a hardcoded constant: a large
//      `initial_cash` absorbs the same book that a small one refuses.
//
// Fixture: a flat (skew-free) testkit surface reused verbatim at successive
// `now_ts` values (S/sigma fixed), so an option's fair_value at any row is
// EXACTLY reproducible in the test body via the same PricedSurface::fair_value
// call book_margin_required makes internally -- no hand-solved American price
// needed, only the (already-tested) formula wiring.

namespace {

constexpr std::uint32_t kEngineUid = 91;
constexpr std::int64_t kEngineBaseNow = 1700000000000000000LL;
constexpr std::int64_t kEngineDayNs = 86400LL * 1000000000LL;
constexpr double kEngineSpot = 100.0;
constexpr double kEngineSigma = 0.20;
constexpr double kEngineStrike = 100.0;

[[nodiscard]] fs::path margin_engine_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-margin-engine-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

// n_dates daily archives on kEngineUid, one flat (phi=0, rho=0) surface per
// date at S=kEngineSpot/sigma=kEngineSigma advanced by one day each -- surface
// SHAPE never changes, only the calendar date it is anchored to (needed so
// `validate_step_ordering` sees strictly increasing snapshot timestamps).
[[nodiscard]] Result<Clock> make_engine_clock(const fs::path &dir, int n_dates) {
  CorpusManifest manifest;
  for (int d = 0; d < n_dates; ++d) {
    const std::int64_t now = kEngineBaseNow + static_cast<std::int64_t>(d) * kEngineDayNs;
    const PricedSurface s =
        testkit::make_flat_surface(kEngineUid, kEngineSpot, kEngineSpot, kEngineSigma, now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-09-%02d", d + 1);
    const std::string date = buf;
    const std::string path = (dir / (date + ".atxvsa")).string();
    const std::vector<SurfaceArchiveItem> items{{"AAA", &s}};
    const Status st = write_surface_archive_v2_file(path, items);
    EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
    manifest.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = "AAA";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    manifest.entries.push_back(std::move(e));
  }
  return Clock::from_manifest(manifest);
}

// Independent reference: the same public fair_value/regt_short_option_margin
// calls book_margin_required (backtest.cpp) makes, evaluated at `row_ts_ns`
// against a fresh flat surface (any `now_ts` reproduces the identical
// (K,T)->iv function, since the surface's SHAPE is what's fixed -- see the
// testkit make_flat_surface comment: "iv(K,T) == sigma for all K,T").
[[nodiscard]] double expected_short_put_margin(std::int64_t expiry_ts, std::int64_t row_ts_ns,
                                               double qty_short) {
  const PricedSurface ref =
      testkit::make_flat_surface(kEngineUid, kEngineSpot, kEngineSpot, kEngineSigma);
  const double T =
      static_cast<double>(expiry_ts - row_ts_ns) / static_cast<double>(kNsPerYear);
  const Result<double> mark = ref.fair_value(kEngineStrike, T, Side::Put);
  EXPECT_TRUE(mark.has_value()) << (mark.has_value() ? std::string{} : mark.error().to_string());
  const double premium = *mark * 100.0; // contract dollars (mult=100)
  const double per_contract =
      regt_short_option_margin(kEngineSpot, kEngineStrike, premium, /*mult=*/100.0, Side::Put);
  return per_contract * qty_short;
}

[[nodiscard]] PortfolioState short_put_book(std::int64_t expiry, double qty) {
  PortfolioState st;
  st.lots.push_back(Lot{1, OptionContract{kEngineUid, kEngineStrike, 0.0, Side::Put}, qty, 100.0,
                        expiry, 0u, 0.0});
  return st;
}

} // namespace

TEST(MarginEngineWiring, MarginRequiredRecordedPerStepAndMatchesRegTFormula) {
  constexpr int kNDates = 3;
  const fs::path dir = margin_engine_dir("required");
  const Result<Clock> clock = make_engine_clock(dir, kNDates);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // T0 = 0.5y, comfortably inside the flat fixture's [0.05, 1.00] pillar range
  // for every one of the kNDates-1 one-day steps this test takes.
  const std::int64_t expiry =
      kEngineBaseNow + static_cast<std::int64_t>(0.5 * static_cast<double>(kNsPerYear));
  constexpr double kQty = -2.0; // 2 short contracts -- also exercises the |qty| scale
  PortfolioState book = short_put_book(expiry, kQty);

  RunConfig cfg{}; // default: MarginBreachPolicy::Ignore
  const Result<BacktestResult> res = run_backtest(*clock, book, cfg);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  ASSERT_EQ(res->size(), static_cast<std::size_t>(kNDates));
  ASSERT_EQ(res->margin_required.size(), res->size())
      << "margin_required must be empty-or-row-parallel and IS produced by run_backtest";

  for (int d = 0; d < kNDates; ++d) {
    const std::int64_t row_ts = kEngineBaseNow + static_cast<std::int64_t>(d) * kEngineDayNs;
    const double expected = expected_short_put_margin(expiry, row_ts, -kQty);
    EXPECT_GT(expected, 0.0) << "fixture invariant: a short ATM-ish put must carry a real "
                                "requirement, or this test proves nothing";
    EXPECT_NEAR(res->margin_required[static_cast<std::size_t>(d)], expected, 1e-6)
        << "row " << d;
  }

  // A long-only book (flip the sign) carries no margin requirement on any row.
  PortfolioState long_book = short_put_book(expiry, /*qty=*/+2.0);
  const Result<BacktestResult> long_res = run_backtest(*clock, long_book, cfg);
  ASSERT_TRUE(long_res.has_value()) << long_res.error().to_string();
  for (const double m : long_res->margin_required) {
    EXPECT_EQ(m, 0.0);
  }
}

TEST(MarginEngineWiring, HaltBreachAbortsWithNamedRow) {
  const fs::path dir = margin_engine_dir("halt");
  const Result<Clock> clock = make_engine_clock(dir, /*n_dates=*/2);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const std::int64_t expiry =
      kEngineBaseNow + static_cast<std::int64_t>(0.5 * static_cast<double>(kNsPerYear));
  PortfolioState book = short_put_book(expiry, /*qty=*/-1.0);

  // Ignore (default): the same book runs to completion despite a real margin
  // requirement against zero available capital (the B0 overload has no cash
  // ledger at all -- see MarginBreachPolicy's doc comment).
  RunConfig ignore_cfg{};
  const Result<BacktestResult> ignored = run_backtest(*clock, book, ignore_cfg);
  ASSERT_TRUE(ignored.has_value()) << ignored.error().to_string();
  ASSERT_FALSE(ignored->margin_required.empty());
  EXPECT_GT(ignored->margin_required.front(), 0.0);

  // Halt: the SAME book, same first row, refuses instead of recording it.
  RunConfig halt_cfg{};
  halt_cfg.margin_breach = MarginBreachPolicy::Halt;
  const Result<BacktestResult> halted = run_backtest(*clock, book, halt_cfg);
  ASSERT_FALSE(halted.has_value()) << "Halt must refuse a book whose margin requirement is "
                                      "positive against zero fixed-book capital";
  EXPECT_EQ(halted.error().code(), atx::core::ErrorCode::InvalidArgument);
  // Named row: the message identifies WHICH date breached, not just that one did.
  EXPECT_NE(halted.error().message().find("2026-09-01"), std::string::npos)
      << "error should name the breaching row's date: " << halted.error().message();
  EXPECT_NE(halted.error().message().find("margin"), std::string::npos)
      << halted.error().message();
}

// Minimal IStrategy that opens ONE short put at inception and never trades
// again -- enough to exercise the strategy overload's cash-based margin_breach
// comparison (distinct from the fixed-book overload's fixed 0.0; see
// MarginBreachPolicy's doc comment on backtest.hpp).
class OpenShortPutStrategy final : public IStrategy {
public:
  OpenShortPutStrategy(std::uint32_t uid, double strike, std::int64_t expiry) noexcept
      : uid_{uid}, strike_{strike}, expiry_{expiry} {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    if (step_index != 0u) {
      return atx::core::Ok();
    }
    const SurfaceRef s = base.find(uid_);
    if (s == nullptr) {
      return atx::core::Err(atx::core::ErrorCode::NotFound,
                            "OpenShortPutStrategy: no entry surface");
    }
    const double T =
        static_cast<double>(expiry_ - base.ts_ns()) / static_cast<double>(kNsPerYear);
    const Result<double> mark = s->fair_value(strike_, T, Side::Put);
    if (!mark) {
      return atx::core::Err(mark.error());
    }
    book.lots.push_back(Lot{next_lot_id++, OptionContract{uid_, strike_, T, Side::Put}, -1.0,
                            100.0, expiry_, 0u, *mark});
    return atx::core::Ok();
  }

private:
  std::uint32_t uid_;
  double strike_;
  std::int64_t expiry_;
};

TEST(MarginEngineWiring, StrategyOverloadHaltUsesCashNotAFixedZero) {
  const fs::path dir = margin_engine_dir("strategy-cash");
  const Result<Clock> clock = make_engine_clock(dir, /*n_dates=*/2);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const std::int64_t expiry =
      kEngineBaseNow + static_cast<std::int64_t>(0.5 * static_cast<double>(kNsPerYear));

  // Ample cash: opening the short put credits cash with its premium, and Reg-T
  // margin is only ~20% of notional above that premium (see
  // expected_short_put_margin's comment), so $1,000,000 comfortably covers it.
  {
    OpenShortPutStrategy strat(kEngineUid, kEngineStrike, expiry);
    RunConfig cfg{};
    cfg.margin_breach = MarginBreachPolicy::Halt;
    cfg.financing.initial_cash = 1'000'000.0;
    const Result<BacktestResult> res = run_backtest(*clock, strat, cfg);
    ASSERT_TRUE(res.has_value()) << res.error().to_string();
    ASSERT_FALSE(res->margin_required.empty());
    EXPECT_GT(res->margin_required.front(), 0.0);
    EXPECT_GT(res->cash.front(), res->margin_required.front())
        << "fixture invariant: ample initial_cash must actually cover the requirement, or "
           "this test proves nothing";
  }
  // Zero cash: the SAME strategy/book, but cash after opening is exactly the
  // premium received, which margin.hpp's own formula guarantees undershoots
  // the Reg-T requirement (the max(0.20*S-OTM,floor)*mult term is strictly
  // positive) -- so Halt must refuse here.
  {
    OpenShortPutStrategy strat(kEngineUid, kEngineStrike, expiry);
    RunConfig cfg{};
    cfg.margin_breach = MarginBreachPolicy::Halt;
    cfg.financing.initial_cash = 0.0;
    const Result<BacktestResult> res = run_backtest(*clock, strat, cfg);
    ASSERT_FALSE(res.has_value())
        << "Halt must refuse when the engine's OWN cash ledger (not a fixed 0.0) is "
           "insufficient";
    EXPECT_EQ(res.error().code(), atx::core::ErrorCode::InvalidArgument);
    EXPECT_NE(res.error().message().find("2026-09-01"), std::string::npos) << res.error().message();
  }
}
