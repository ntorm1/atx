// atx-vol SurfaceDb-driven dispersion backtest gate tests.
//
// Task 1 of the surface-db dispersion sprint adds `Clock::between(lo, hi)`, the
// date-window subset every later task in this sprint uses to carve a run window
// out of a db-backed clock. A SurfaceDb partition key IS the ISO date, and the
// canonical keys sort lexicographically == chronologically, so the window is a
// plain string-range filter over `Clock::refs()`.
//
//   1. BetweenSelectsInclusiveWindow      — [lo, hi] is inclusive on BOTH ends
//                                           and keeps the refs' archive paths.
//   2. BetweenClampsToAvailableRange      — bounds outside the corpus clamp to
//                                           the available refs, they do not error.
//   3. BetweenEmptyWindowIsInvalidArgument— lo > hi, and a window containing no
//                                           partition, are both InvalidArgument
//                                           whose message names the available range.
//
// Fixtures are synthetic eSSVI surfaces written into a fresh SurfaceDb under
// %TEMP% (make_test_db below); nothing here reads the real data lake.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"        // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"        // Clock, MarketSnapshot
#include "atx/vol/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/surface_archive.hpp" // SurfaceArchiveItem
#include "atx/vol/surface_db.hpp"      // SurfaceDb
#include "atx/vol/surface_parity.hpp"  // SliceContext
#include "atx/vol/types.hpp"           // Result, ErrorCode
#include "atx/vol/vol_curve.hpp"       // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"     // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;

// A synthetic eSSVI PricedSurface (flat forward == spot, genuine American
// premium via q_eff=0.02), 7 slices T in [0.05, 1.0]. Copied from
// surface_db_backtest_test.cpp's make_surface (the sprint's fixture pattern).
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

// Fresh per-test temp dir under the system temp root, self-cleaning at start so
// a prior crashed run does not leak stale manifest/partition files into this
// run. Copied from surface_db_test.cpp:150-153 (via surface_db_backtest_test.cpp).
[[nodiscard]] fs::path test_root(std::string_view name) {
  auto p = fs::temp_directory_path() / ("atx_surface_db_disp_" + std::string(name));
  fs::remove_all(p);
  return p;
}

// Build a SurfaceDb at `root` with one partition per entry of `dates`, each
// holding every entry of `symbols` (uid = 1-based index, distinct spot and vol
// bump per symbol, gentle per-date spot drift so nothing is degenerate). The
// partition's `now_ts_ns` advances one day per date in `dates` ORDER, so the
// caller may hand dates out of chronological order to exercise sorting.
//
// Shared fixture builder for this file — later tasks in this sprint extend it.
[[nodiscard]] Result<SurfaceDb> make_test_db(const fs::path &root,
                                             const std::vector<std::string_view> &dates,
                                             const std::vector<std::string_view> &symbols) {
  auto db = SurfaceDb::create(root.string());
  if (!db.has_value()) {
    return atx::core::Err(db.error());
  }
  constexpr std::int64_t kBaseTs = 1'700'000'000'000'000'000LL;
  for (std::size_t d = 0; d < dates.size(); ++d) {
    const std::int64_t ts = kBaseTs + static_cast<std::int64_t>(d) * kDayNs;
    std::vector<PricedSurface> surfaces;
    surfaces.reserve(symbols.size());
    for (std::size_t s = 0; s < symbols.size(); ++s) {
      const double spot =
          100.0 * static_cast<double>(s + 1) * (1.0 + 0.002 * static_cast<double>(d));
      surfaces.push_back(
          make_surface(spot, ts, 0.01 * static_cast<double>(s), static_cast<std::uint32_t>(s + 1)));
    }
    // NB: SurfaceArchiveItem::symbol is a std::string_view — it must alias
    // `symbols`, which outlives this call, never a temporary std::string.
    std::vector<SurfaceArchiveItem> items;
    items.reserve(symbols.size());
    for (std::size_t s = 0; s < symbols.size(); ++s) {
      items.push_back(SurfaceArchiveItem{symbols[s], &surfaces[s]});
    }
    auto st = db->write_partition(dates[d], items);
    if (!st.has_value()) {
      return atx::core::Err(st.error());
    }
  }
  return db;
}

// The four-date corpus every test in this file windows over.
const std::vector<std::string_view> kDates = {"2026-01-05", "2026-01-06", "2026-01-07",
                                              "2026-01-08"};
const std::vector<std::string_view> kSymbols = {"SPY", "AAPL"};

} // namespace

TEST(SurfaceDbDispersionBacktest, BetweenSelectsInclusiveWindow) {
  const auto root = test_root("between_window");
  auto db = make_test_db(root, kDates, kSymbols);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());
  const auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value());
  ASSERT_EQ(clock->size(), 4u);

  const auto sub = clock->between("2026-01-06", "2026-01-07");
  ASSERT_TRUE(sub.has_value()) << (sub.has_value() ? std::string{} : sub.error().to_string());
  ASSERT_EQ(sub->refs().size(), 2u);
  EXPECT_EQ(sub->refs().front().date, "2026-01-06");
  EXPECT_EQ(sub->refs().back().date, "2026-01-07");
  // Both endpoints are INCLUSIVE: a single-date window keeps exactly that date.
  const auto one = clock->between("2026-01-05", "2026-01-05");
  ASSERT_TRUE(one.has_value());
  ASSERT_EQ(one->size(), 1u);
  EXPECT_EQ(one->refs().front().date, "2026-01-05");
  // The subset carries the source refs whole (path included) and the refs still
  // load, so a windowed clock is directly runnable.
  for (const auto &ref : sub->refs()) {
    auto snap = MarketSnapshot::load(ref.archive_path);
    ASSERT_TRUE(snap.has_value()) << ref.archive_path;
    EXPECT_TRUE(snap->uid_of("SPY").has_value());
  }
  // Subsetting is non-mutating: the source clock is untouched.
  EXPECT_EQ(clock->size(), 4u);
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, BetweenClampsToAvailableRange) {
  const auto root = test_root("between_clamp");
  auto db = make_test_db(root, kDates, kSymbols);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());
  const auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value());

  // Bounds far outside the corpus clamp to what exists — not an error.
  const auto sub = clock->between("2020-01-01", "2030-01-01");
  ASSERT_TRUE(sub.has_value()) << (sub.has_value() ? std::string{} : sub.error().to_string());
  EXPECT_EQ(sub->refs().size(), 4u);
  EXPECT_EQ(sub->refs().front().date, "2026-01-05");
  EXPECT_EQ(sub->refs().back().date, "2026-01-08");
  // One-sided overhang clamps on that side alone.
  const auto lo_open = clock->between("2020-01-01", "2026-01-06");
  ASSERT_TRUE(lo_open.has_value());
  EXPECT_EQ(lo_open->size(), 2u);
  const auto hi_open = clock->between("2026-01-07", "2030-01-01");
  ASSERT_TRUE(hi_open.has_value());
  EXPECT_EQ(hi_open->size(), 2u);
  fs::remove_all(root);
}

TEST(SurfaceDbDispersionBacktest, BetweenEmptyWindowIsInvalidArgument) {
  const auto root = test_root("between_empty");
  auto db = make_test_db(root, kDates, kSymbols);
  ASSERT_TRUE(db.has_value()) << (db.has_value() ? std::string{} : db.error().to_string());
  const auto clock = Clock::from_surface_db(*db);
  ASSERT_TRUE(clock.has_value());

  const auto sub = clock->between("2026-01-06T", "2026-01-06A"); // lo > hi lexicographically
  ASSERT_FALSE(sub.has_value());
  EXPECT_EQ(sub.error().code(), ErrorCode::InvalidArgument);

  const auto gap = clock->between("2026-02-01", "2026-02-28"); // no partitions in window
  ASSERT_FALSE(gap.has_value());
  EXPECT_EQ(gap.error().code(), ErrorCode::InvalidArgument);
  // The message must name the available range so the operator can self-serve.
  EXPECT_NE(gap.error().message().find("2026-01-05"), std::string::npos) << gap.error().message();
  EXPECT_NE(gap.error().message().find("2026-01-08"), std::string::npos) << gap.error().message();
  EXPECT_NE(sub.error().message().find("2026-01-05"), std::string::npos) << sub.error().message();
  EXPECT_NE(sub.error().message().find("2026-01-08"), std::string::npos) << sub.error().message();
  fs::remove_all(root);
}
