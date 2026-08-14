// atx-vol Clock::from_surface_db gate tests.
//
// A SurfaceDb's partitions are bare ATXVSA archives at
// <root>/partitions/<KEY>.atxvsa — the exact format MarketSnapshot::load
// already opens. Clock::from_surface_db is the one missing bridge: it turns
// the db's partition index into an ordered Clock of SnapshotRefs so a
// backtest can run entirely off a SurfaceDb root, with no CorpusManifest and
// no loose archive paths.
//
//   1. ClockFromDb_OrderedRefsAndPathsLoad — partitions written out of date
//      order come back sorted ascending by key, and every ref path loads as
//      a MarketSnapshot with both symbols resolvable.
//   2. ClockFromDb_EmptyDbRejected           — an empty db is InvalidArgument.
//   3. DbDrivesRunBacktestEndToEnd           — acceptance gate: a db-backed
//      Clock drives run_backtest end-to-end with a real DeclarativeStrategy.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"        // al_fast_opts, AmericanMethod
#include "atx/vol/api/backtest/backtest.hpp"        // Clock, MarketSnapshot, run_backtest, RunConfig
#include "atx/vol/api/backtest/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/api/backtest/strategy.hpp"        // DeclarativeStrategy, StrategySpec, LegSpec
#include "atx/vol/api/storage/surface_archive.hpp" // SurfaceArchiveItem
#include "atx/vol/api/storage/surface_db.hpp"      // SurfaceDb
#include "atx/vol/api/fitting/surface_parity.hpp"  // SliceContext
#include "atx/vol/api/core/types.hpp"           // Result, ErrorCode
#include "atx/vol/api/fitting/vol_curve.hpp"       // CurveSurface, EssviCurve
#include "atx/vol/api/fitting/vol_surface.hpp"     // EssviParams

using namespace atx::vol;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;

// A synthetic eSSVI PricedSurface (flat forward == spot, genuine American
// premium via q_eff=0.02), 7 slices T in [0.05, 1.0]. Mirrors the
// strategy_test.cpp / spy_strangle_backtest_test.cpp make_surface pattern,
// parameterized by (S, now_ts, vol_bump, uid) so distinct symbols within one
// date partition can carry distinct uids while sharing that date's now_ts.
[[nodiscard]] PricedSurface make_surface(double S, std::int64_t now_ts, double vol_bump,
                                         std::uint32_t uid) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i) + vol_bump;
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = S;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, S, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

// Fresh per-test temp dir under the system temp root, self-cleaning at start
// so a prior crashed run doesn't leak stale manifest/partition files into
// this run. Copied from surface_db_test.cpp:150-153.
[[nodiscard]] std::filesystem::path test_root(std::string_view name) {
  auto p = std::filesystem::temp_directory_path() / ("atx_surface_db_" + std::string(name));
  std::filesystem::remove_all(p);
  return p;
}

} // namespace

TEST(SurfaceDbBacktest, ClockFromDb_OrderedRefsAndPathsLoad) {
  const auto root = test_root("clock_from_db"); // temp-dir helper, copy from surface_db_test.cpp:150-153
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  // 3 dates, deliberately written OUT of order; 2 symbols per date.
  const char *dates[] = {"2026-01-06", "2026-01-02", "2026-01-05"};
  std::int64_t base_ts = 1'700'000'000'000'000'000;
  const std::int64_t day_ts[] = {base_ts + 4 * kDayNs, base_ts, base_ts + 3 * kDayNs};
  for (int d = 0; d < 3; ++d) {
    const auto spy = make_surface(500.0, day_ts[d], 0.0, /*uid=*/1);
    const auto aapl = make_surface(200.0, day_ts[d], 0.05, /*uid=*/2);
    std::vector<SurfaceArchiveItem> items{{"SPY", &spy}, {"AAPL", &aapl}};
    ASSERT_TRUE(db->write_partition(dates[d], items).has_value());
  }
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value());
  ASSERT_EQ(clock->size(), 3u);
  EXPECT_EQ(clock->refs()[0].date, "2026-01-02");
  EXPECT_EQ(clock->refs()[1].date, "2026-01-05");
  EXPECT_EQ(clock->refs()[2].date, "2026-01-06");
  // Every ref path loads as a MarketSnapshot with both symbols resolvable.
  for (const auto &ref : clock->refs()) {
    auto snap = MarketSnapshot::load(ref.archive_path);
    ASSERT_TRUE(snap.has_value()) << ref.archive_path;
    EXPECT_TRUE(snap->uid_of("SPY").has_value());
    EXPECT_TRUE(snap->uid_of("aapl").has_value()); // symbol lookup canonicalizes
  }
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbBacktest, ClockFromDb_EmptyDbRejected) {
  const auto root = test_root("clock_from_db_empty");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  auto clock = Clock::from_surface_db(*db);
  ASSERT_FALSE(clock.has_value());
  EXPECT_EQ(clock.error().code(), ErrorCode::InvalidArgument);
  std::filesystem::remove_all(root);
}

TEST(SurfaceDbBacktest, DbDrivesRunBacktestEndToEnd) {
  // Acceptance gate: a db populated with a small synthetic multi-date corpus
  // drives run_backtest end-to-end — no loose archive paths, no CorpusManifest.
  const auto root = test_root("db_backtest_e2e");
  auto db = SurfaceDb::create(root.string());
  ASSERT_TRUE(db.has_value());
  std::int64_t base_ts = 1'700'000'000'000'000'000;
  for (int d = 0; d < 6; ++d) {
    char date[11];
    std::snprintf(date, sizeof date, "2026-02-%02d", 2 + d);
    const double spot = 500.0 * (1.0 + 0.002 * d); // gentle drift so pnl is nonzero
    const auto spy = make_surface(spot, base_ts + d * kDayNs, 0.0, /*uid=*/1);
    std::vector<SurfaceArchiveItem> items{{"SPY", &spy}};
    ASSERT_TRUE(db->write_partition(date, items).has_value());
  }
  auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value());
  StrategySpec spec; // simple daily-restrike short strangle, mirrors spy_strangle_backtest_test::make_spec
  spec.name = "db-e2e";
  LegSpec leg;
  leg.symbol = "SPY";
  leg.tenor.target_T = 0.5;
  leg.structure.kind = StructureSpec::Kind::Strangle;
  leg.structure.call_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.structure.put_leg = {StrikeSelector::Kind::Delta, 0.40};
  leg.size = {SizeSpec::Kind::FixedContracts, 10.0, -1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::RollAtHorizon;
  spec.lifecycle.roll_at_T = 1.0; // > tenor: daily restrike, single cohort
  DeclarativeStrategy strat(spec);
  auto r = run_backtest(*clock, strat, RunConfig{});
  ASSERT_TRUE(r.has_value());
  ASSERT_EQ(r->size(), 6u);
  EXPECT_EQ(r->date.front(), "2026-02-02");
  EXPECT_EQ(r->date.back(), "2026-02-07");
  for (std::size_t i = 0; i < r->size(); ++i) EXPECT_EQ(r->n_open_lots[i], 2u) << i;
  // Non-degenerate: something priced and moved.
  EXPECT_NE(r->nav.back(), 0.0);
  std::filesystem::remove_all(root);
}
