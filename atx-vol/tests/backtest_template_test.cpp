#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/vol/american.hpp"
#include "atx/vol/backtest_template.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

namespace {

using namespace atx::vol;
namespace fs = std::filesystem;
namespace time = atx::core::time;

constexpr std::uint32_t kUid = 17u;
constexpr double kRate = 0.043;

class ScopedTempDirectory {
public:
  explicit ScopedTempDirectory(std::string tag)
      : path_{fs::temp_directory_path() / ("atx-backtest-template-" + std::move(tag))} {
    std::error_code error;
    fs::remove_all(path_, error);
    error.clear();
    fs::create_directories(path_, error);
  }

  ~ScopedTempDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  fs::path path_;
};

[[nodiscard]] std::int64_t timestamp(std::int32_t year, std::uint32_t month, std::uint32_t day,
                                     std::uint32_t hour = 20u, std::uint32_t minute = 0u) {
  return time::timestamp_from_utc(year, month, day, hour, minute, 0u, 0u).unix_nanos();
}

[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, std::int64_t now_ts) {
  CurveSurface curves;
  std::vector<SliceContext> context;
  std::uint16_t expiry_id = 0u;
  for (const double term : {0.03, 0.08, 0.15, 0.25, 0.50, 1.00}) {
    EssviParams parameters{};
    parameters.theta = 0.035 + 0.012 * term;
    parameters.phi = 1.35;
    parameters.rho = -0.32;
    parameters.psi = 0.5;
    parameters.p = 0.5;
    parameters.lambda = 0.5;
    parameters.T = term;
    parameters.F = 100.0;
    parameters.expiry_id = expiry_id++;
    curves.push(std::make_unique<EssviCurve>(parameters, std::exp(-kRate * term)));
    context.push_back(SliceContext{term, 100.0, 0.0, 0.02, 120, 7});
  }
  PricingContext pricing;
  pricing.S = 100.0;
  pricing.r = kRate;
  pricing.now_ts_ns = now_ts;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = uid;
  auto surface = PricedSurface::create(std::move(curves), std::move(context), pricing);
  EXPECT_TRUE(surface) << (surface ? std::string{} : surface.error().to_string());
  return std::move(*surface);
}

[[nodiscard]] Result<MarketSnapshot> write_snapshot(const fs::path &directory,
                                                    const PricedSurface &surface) {
  const std::string path = (directory / "snapshot.atxvsa").string();
  const SurfaceArchiveItem item{"TEST", &surface};
  const Status written =
      write_surface_archive_v2_file(path, std::span<const SurfaceArchiveItem>{&item, 1u});
  if (!written) {
    return atx::core::Err(written.error());
  }
  return MarketSnapshot::load(path);
}

TEST(BacktestTemplate, ValidationAndFingerprintCoverCatalogAndEconomicsButNotDisplayName) {
  auto made = make_40_delta_3_calendar_month_strangle_template(-1.0, 5u);
  ASSERT_TRUE(made) << (made ? std::string{} : made.error().to_string());
  EXPECT_TRUE(validate_backtest_template(*made));

  const std::uint64_t base = fingerprint_backtest_template(*made);
  EXPECT_NE(base, 0u);

  BacktestStrategyTemplate renamed = *made;
  renamed.name = "Same economics, different display label";
  EXPECT_EQ(fingerprint_backtest_template(renamed), base);

  BacktestStrategyTemplate different_identity = *made;
  different_identity.id = "another-catalog-entry";
  EXPECT_EQ(fingerprint_backtest_template(different_identity), base);

  BacktestStrategyTemplate different_quantity = *made;
  different_quantity.legs.front().quantity = -2.0;
  EXPECT_NE(fingerprint_backtest_template(different_quantity), base);

  BacktestStrategyTemplate invalid = *made;
  invalid.entry_every_n = 0u;
  EXPECT_FALSE(validate_backtest_template(invalid));
  EXPECT_EQ(fingerprint_backtest_template(invalid), 0u);
}

TEST(BacktestTemplate, FortyDeltaThreeCalendarMonthFactoryIsExact) {
  auto made = make_40_delta_3_calendar_month_strangle_template(-1.0, 21u);
  ASSERT_TRUE(made) << (made ? std::string{} : made.error().to_string());

  EXPECT_EQ(made->id, "strangle-40d-3cm-hold-expiry-daily-delta-v1");
  EXPECT_EQ(made->name, "40 Delta 3 Calendar Month Strangle");
  EXPECT_EQ(made->entry_every_n, 21u);
  EXPECT_EQ(made->holding, BacktestHoldingRule::HoldToExpiry);
  EXPECT_EQ(made->settlement, TheoreticalSettlementRule::FollowingNyseSessionSnapshot);
  EXPECT_EQ(made->hedge.kind, HedgeSpec::Kind::DeltaToZero);
  EXPECT_EQ(made->hedge.cadence, HedgeSpec::Cadence::Daily);
  EXPECT_DOUBLE_EQ(made->hedge.band, 0.0);
  ASSERT_EQ(made->legs.size(), 2u);
  EXPECT_EQ(made->legs[0].side, Side::Call);
  EXPECT_EQ(made->legs[1].side, Side::Put);
  for (const BacktestTemplateLeg &leg : made->legs) {
    EXPECT_EQ(leg.maturity.kind, ProjectedMaturityKind::CalendarMonths);
    EXPECT_EQ(leg.maturity.calendar_count, 3);
    EXPECT_EQ(leg.strike.kind, ProjectedStrikeKind::Delta);
    EXPECT_DOUBLE_EQ(leg.strike.value, 0.40);
    EXPECT_DOUBLE_EQ(leg.quantity, -1.0);
    EXPECT_DOUBLE_EQ(leg.multiplier, 100.0);
  }
}

TEST(BacktestTemplate, ProjectionCreatesConcreteLotsWithOneFollowingSessionExpiry) {
  const ScopedTempDirectory temp{"projection"};
  const PricedSurface surface = make_surface(kUid, timestamp(2026, 7, 31, 19u, 55u));
  auto snapshot = write_snapshot(temp.path(), surface);
  ASSERT_TRUE(snapshot) << (snapshot ? std::string{} : snapshot.error().to_string());

  auto spec = make_40_delta_3_calendar_month_strangle_template(1.0, 1u);
  ASSERT_TRUE(spec);
  auto strategy = ProjectedTemplateStrategy::create(*spec, kUid);
  ASSERT_TRUE(strategy) << (strategy ? std::string{} : strategy.error().to_string());

  PortfolioState book;
  std::uint64_t next_lot_id = 50u;
  const Status entered = strategy->on_step(*snapshot, 0u, book, next_lot_id);
  ASSERT_TRUE(entered) << (entered ? std::string{} : entered.error().to_string());

  const std::int64_t expected_expiry = timestamp(2026, 11u, 2u, 19u, 55u);
  ASSERT_EQ(book.lots.size(), 2u);
  EXPECT_EQ(next_lot_id, 52u);
  EXPECT_EQ(book.lots[0].expiry_ts_ns, expected_expiry);
  EXPECT_EQ(book.lots[1].expiry_ts_ns, expected_expiry);
  EXPECT_EQ(book.lots[0].contract.T, book.lots[1].contract.T);
  EXPECT_EQ(book.lots[0].contract.side, Side::Call);
  EXPECT_EQ(book.lots[1].contract.side, Side::Put);
  EXPECT_GT(book.lots[0].contract.K, 0.0);
  EXPECT_GT(book.lots[1].contract.K, 0.0);
  EXPECT_NE(book.lots[0].contract.K, book.lots[1].contract.K);
  EXPECT_TRUE(std::isfinite(book.lots[0].entry_price));
  EXPECT_TRUE(std::isfinite(book.lots[1].entry_price));
  EXPECT_EQ(strategy->next_cohort_counter(), 1u);
}

TEST(BacktestTemplate, FailedLegLeavesBookIdsAndCohortUnchanged) {
  const ScopedTempDirectory temp{"atomic"};
  const PricedSurface surface = make_surface(kUid, timestamp(2026, 7, 31));
  auto snapshot = write_snapshot(temp.path(), surface);
  ASSERT_TRUE(snapshot);

  auto spec = make_40_delta_3_calendar_month_strangle_template(1.0, 1u);
  ASSERT_TRUE(spec);
  spec->legs[1].maturity = ProjectedMaturitySpec::absolute(1);
  ASSERT_TRUE(validate_backtest_template(*spec));
  auto strategy = ProjectedTemplateStrategy::create(*spec, kUid, 9u);
  ASSERT_TRUE(strategy);

  PortfolioState book;
  Lot existing;
  existing.id = 3u;
  book.lots.push_back(existing);
  std::uint64_t next_lot_id = 100u;
  const Status entered = strategy->on_step(*snapshot, 0u, book, next_lot_id);

  EXPECT_FALSE(entered);
  ASSERT_EQ(book.lots.size(), 1u);
  EXPECT_EQ(book.lots.front().id, 3u);
  EXPECT_EQ(next_lot_id, 100u);
  EXPECT_EQ(strategy->next_cohort_counter(), 9u);
}

TEST(BacktestTemplate, EntryCadenceUsesGlobalStepIndicesNotCohortCount) {
  const ScopedTempDirectory temp{"cadence"};
  const PricedSurface surface = make_surface(kUid, timestamp(2026, 7, 31));
  auto snapshot = write_snapshot(temp.path(), surface);
  ASSERT_TRUE(snapshot);

  auto spec = make_40_delta_3_calendar_month_strangle_template(1.0, 3u);
  ASSERT_TRUE(spec);
  auto strategy = ProjectedTemplateStrategy::create(*spec, kUid, 7u);
  ASSERT_TRUE(strategy);

  PortfolioState book;
  std::uint64_t next_lot_id = 10u;
  EXPECT_TRUE(strategy->on_step(*snapshot, 1u, book, next_lot_id));
  EXPECT_TRUE(strategy->on_step(*snapshot, 2u, book, next_lot_id));
  EXPECT_TRUE(book.lots.empty());
  EXPECT_EQ(strategy->next_cohort_counter(), 7u);

  EXPECT_TRUE(strategy->on_step(*snapshot, 3u, book, next_lot_id));
  ASSERT_EQ(book.lots.size(), 2u);
  EXPECT_EQ(book.lots[0].cohort, 7u);
  EXPECT_EQ(book.lots[1].cohort, 7u);
  EXPECT_EQ(strategy->next_cohort_counter(), 8u);

  EXPECT_TRUE(strategy->on_step(*snapshot, 4u, book, next_lot_id));
  EXPECT_EQ(book.lots.size(), 2u);
  EXPECT_TRUE(strategy->on_step(*snapshot, 6u, book, next_lot_id));
  ASSERT_EQ(book.lots.size(), 4u);
  EXPECT_EQ(book.lots[2].cohort, 8u);
  EXPECT_EQ(book.lots[3].cohort, 8u);
  EXPECT_EQ(strategy->next_cohort_counter(), 9u);
  EXPECT_EQ(next_lot_id, 14u);
}

} // namespace
