#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/backtest_db.hpp"
#include "atx/vol/backtest_db_build.hpp"
#include "atx/vol/backtest_template.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_db.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 0.043;
constexpr std::int64_t kBaseNow = 1'767'643'200'000'000'000LL;
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;
constexpr std::uint32_t kUid = 17u;

[[nodiscard]] fs::path clean_root(std::string_view suffix) {
  const fs::path root =
      fs::temp_directory_path() / ("atx-backtest-db-build-" + std::string{suffix});
  std::error_code ec;
  fs::remove_all(root, ec);
  return root;
}

[[nodiscard]] PricedSurface make_surface(double spot, std::int64_t now_ts, double vol_bump) {
  CurveSurface curves;
  std::vector<SliceContext> contexts;
  constexpr double tenors[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  std::uint16_t expiry_id = 0u;
  for (const double tenor : tenors) {
    const double forward = spot * std::exp((kRate - 0.02) * tenor);
    EssviParams params{};
    params.theta = 0.04 + 0.005 * static_cast<double>(expiry_id) + vol_bump;
    params.phi = 1.5 - 0.05 * static_cast<double>(expiry_id);
    params.rho = -0.4 + 0.02 * static_cast<double>(expiry_id);
    params.psi = 0.5;
    params.p = 0.5;
    params.lambda = 0.5;
    params.T = tenor;
    params.F = forward;
    params.expiry_id = expiry_id;
    curves.push(std::make_unique<EssviCurve>(params, std::exp(-kRate * tenor)));
    contexts.push_back(SliceContext{tenor, forward, 0.0, 0.02, 250, 7});
    ++expiry_id;
  }

  PricingContext pricing;
  pricing.S = spot;
  pricing.r = kRate;
  pricing.now_ts_ns = now_ts;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = kUid;
  auto surface = PricedSurface::create(std::move(curves), std::move(contexts), pricing);
  EXPECT_TRUE(surface.has_value())
      << (surface.has_value() ? std::string{} : surface.error().to_string());
  return std::move(*surface);
}

void write_symbol_date(SurfaceDb &db, std::size_t index, std::string_view symbol,
                       double rewritten_bump = 0.0) {
  char date[11];
  std::snprintf(date, sizeof date, "2026-01-%02u", static_cast<unsigned>(5u + index));
  const PricedSurface surface = make_surface(100.0 + 0.6 * static_cast<double>(index),
                                             kBaseNow + static_cast<std::int64_t>(index) * kDayNs,
                                             0.001 * static_cast<double>(index) + rewritten_bump);
  const SurfaceArchiveItem item{symbol, &surface};
  const Status status = db.write_partition(date, std::span<const SurfaceArchiveItem>{&item, 1u});
  ASSERT_TRUE(status.has_value()) << (status.has_value() ? std::string{}
                                                         : status.error().to_string());
}

void write_dates(SurfaceDb &db, std::size_t first, std::size_t count, double rewritten_bump = 0.0) {
  for (std::size_t offset = 0; offset < count; ++offset) {
    write_symbol_date(db, first + offset, "SPY", rewritten_bump);
  }
}

[[nodiscard]] BacktestDbBuildSpec make_spec(const fs::path &surface_root,
                                            const fs::path &backtest_root) {
  auto strategy = make_40_delta_3_calendar_month_strangle_template(-1.0, 1u);
  EXPECT_TRUE(strategy.has_value())
      << (strategy.has_value() ? std::string{} : strategy.error().to_string());
  strategy->id = "short-40d-3m-daily";
  strategy->name = "Short 40 Delta 3M Strangle, Daily Entry, Daily Delta Hedge";

  BacktestDbBuildSpec spec;
  spec.surface_db_root = surface_root.string();
  spec.backtest_db_root = backtest_root.string();
  spec.templates.push_back(std::move(*strategy));
  spec.symbols.push_back("SPY");
  spec.price_threads = 1u;
  return spec;
}

[[nodiscard]] BacktestDbBuildSpec make_settlement_spec(const fs::path &surface_root,
                                                       const fs::path &backtest_root) {
  constexpr unsigned kEntryCadence = 100u;
  constexpr std::int32_t kMaturityDays = 21;
  auto strategy = make_40_delta_3_calendar_month_strangle_template(-1.0, kEntryCadence);
  EXPECT_TRUE(strategy.has_value())
      << (strategy.has_value() ? std::string{} : strategy.error().to_string());
  strategy->id = "short-40d-21d-settlement";
  strategy->name = "Short 40 Delta 21 Day Strangle Settlement Boundary";
  for (BacktestTemplateLeg &leg : strategy->legs) {
    leg.maturity = ProjectedMaturitySpec::days(kMaturityDays);
  }
  // Keep a live hedge into expiry while retaining a two-leg projected cohort.
  strategy->legs[1].quantity = -0.5;

  BacktestDbBuildSpec spec;
  spec.surface_db_root = surface_root.string();
  spec.backtest_db_root = backtest_root.string();
  spec.templates.push_back(std::move(*strategy));
  spec.symbols.push_back("SPY");
  spec.price_threads = 1u;
  return spec;
}

template <typename T>
void expect_vectors_equal(const std::vector<T> &actual, const std::vector<T> &expected) {
  EXPECT_EQ(actual, expected);
}

void expect_same_result(const BacktestResult &actual, const BacktestResult &expected) {
  expect_vectors_equal(actual.date, expected.date);
  expect_vectors_equal(actual.ts_ns, expected.ts_ns);
  expect_vectors_equal(actual.pnl_total, expected.pnl_total);
  expect_vectors_equal(actual.pnl_delta, expected.pnl_delta);
  expect_vectors_equal(actual.pnl_gamma, expected.pnl_gamma);
  expect_vectors_equal(actual.pnl_vega, expected.pnl_vega);
  expect_vectors_equal(actual.pnl_vanna, expected.pnl_vanna);
  expect_vectors_equal(actual.pnl_volga, expected.pnl_volga);
  expect_vectors_equal(actual.pnl_theta, expected.pnl_theta);
  expect_vectors_equal(actual.pnl_rho, expected.pnl_rho);
  expect_vectors_equal(actual.pnl_charm, expected.pnl_charm);
  expect_vectors_equal(actual.pnl_unexplained, expected.pnl_unexplained);
  expect_vectors_equal(actual.pnl_settlement, expected.pnl_settlement);
  expect_vectors_equal(actual.pnl_shares, expected.pnl_shares);
  expect_vectors_equal(actual.financing, expected.financing);
  expect_vectors_equal(actual.cost, expected.cost);
  expect_vectors_equal(actual.nav, expected.nav);
  expect_vectors_equal(actual.cash, expected.cash);
  expect_vectors_equal(actual.gross_delta, expected.gross_delta);
  expect_vectors_equal(actual.gross_gamma, expected.gross_gamma);
  expect_vectors_equal(actual.gross_vega, expected.gross_vega);
  expect_vectors_equal(actual.gross_vega_abs, expected.gross_vega_abs);
  expect_vectors_equal(actual.gross_theta, expected.gross_theta);
  expect_vectors_equal(actual.turnover_notional, expected.turnover_notional);
  expect_vectors_equal(actual.turnover_vega, expected.turnover_vega);
  expect_vectors_equal(actual.n_open_lots, expected.n_open_lots);
  expect_vectors_equal(actual.n_unpriced_lots, expected.n_unpriced_lots);
  expect_vectors_equal(actual.n_unpriced_greeks, expected.n_unpriced_greeks);
  expect_vectors_equal(actual.step_pnl_total, expected.step_pnl_total);
  expect_vectors_equal(actual.nav_liquidation, expected.nav_liquidation);
  EXPECT_EQ(actual.signals, expected.signals);
}

} // namespace

TEST(BacktestDbBuild, InitialBuildUnchangedRerunAndDailyExtensionMatchOneShot) {
  const fs::path surface_root = clean_root("append-surface");
  const fs::path incremental_root = clean_root("append-incremental");
  const fs::path one_shot_root = clean_root("append-one-shot");
  auto surface_db = SurfaceDb::create(surface_root.string());
  ASSERT_TRUE(surface_db.has_value()) << surface_db.error().to_string();
  write_dates(*surface_db, 0u, 3u);

  BacktestDbBuildSpec spec = make_spec(surface_root, incremental_root);
  auto first = build_backtest_db(spec);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  EXPECT_EQ(first->n_full, 1u);
  EXPECT_EQ(first->n_extended, 0u);
  EXPECT_EQ(first->rows_computed, 3u);

  auto unchanged = build_backtest_db(spec);
  ASSERT_TRUE(unchanged.has_value()) << unchanged.error().to_string();
  EXPECT_EQ(unchanged->n_unchanged, 1u);
  EXPECT_EQ(unchanged->rows_computed, 0u);

  write_dates(*surface_db, 3u, 3u);
  auto extended = build_backtest_db(spec);
  ASSERT_TRUE(extended.has_value()) << extended.error().to_string();
  EXPECT_EQ(extended->n_extended, 1u);
  EXPECT_EQ(extended->rows_computed, 3u);
  EXPECT_EQ(extended->rows_added, 3u);

  auto incremental_db = BacktestDb::open(incremental_root.string());
  ASSERT_TRUE(incremental_db.has_value()) << incremental_db.error().to_string();
  auto incremental = incremental_db->load_series("short-40d-3m-daily", "spy");
  ASSERT_TRUE(incremental.has_value()) << incremental.error().to_string();
  EXPECT_EQ(incremental->backtest.size(), 6u);
  EXPECT_EQ(incremental->sources.size(), 6u);
  EXPECT_EQ(incremental->checkpoint.completed_step_index, 5u);
  EXPECT_EQ(incremental->next_cohort, 6u);

  BacktestDbBuildSpec one_shot_spec = make_spec(surface_root, one_shot_root);
  auto one_shot_report = build_backtest_db(one_shot_spec);
  ASSERT_TRUE(one_shot_report.has_value()) << one_shot_report.error().to_string();
  EXPECT_EQ(one_shot_report->n_full, 1u);
  EXPECT_EQ(one_shot_report->rows_computed, 6u);
  auto one_shot_db = BacktestDb::open(one_shot_root.string());
  ASSERT_TRUE(one_shot_db.has_value()) << one_shot_db.error().to_string();
  auto one_shot = one_shot_db->load_series("short-40d-3m-daily", "SPY");
  ASSERT_TRUE(one_shot.has_value()) << one_shot.error().to_string();

  expect_same_result(incremental->backtest, one_shot->backtest);
  EXPECT_EQ(incremental->checkpoint, one_shot->checkpoint);

  std::error_code ec;
  fs::remove_all(surface_root, ec);
  fs::remove_all(incremental_root, ec);
  fs::remove_all(one_shot_root, ec);
}

TEST(BacktestDbBuild, ExtensionAcrossExactProjectedExpiryMatchesOneShotSettlement) {
  constexpr std::size_t kExpiryIndex = 21u;
  constexpr std::size_t kInitialRows = kExpiryIndex;
  constexpr std::size_t kAppendedRows = 2u;
  constexpr std::size_t kTotalRows = kInitialRows + kAppendedRows;
  constexpr std::int64_t kExpectedExpiry =
      kBaseNow + static_cast<std::int64_t>(kExpiryIndex) * kDayNs;
  const fs::path surface_root = clean_root("settlement-surface");
  const fs::path incremental_root = clean_root("settlement-incremental");
  const fs::path one_shot_root = clean_root("settlement-one-shot");
  auto surface_db = SurfaceDb::create(surface_root.string());
  ASSERT_TRUE(surface_db.has_value()) << surface_db.error().to_string();
  write_dates(*surface_db, 0u, kInitialRows);

  BacktestDbBuildSpec incremental_spec = make_settlement_spec(surface_root, incremental_root);
  auto initial_report = build_backtest_db(incremental_spec);
  ASSERT_TRUE(initial_report.has_value()) << initial_report.error().to_string();
  ASSERT_EQ(initial_report->n_full, 1u);
  auto initial_db = BacktestDb::open(incremental_root.string());
  ASSERT_TRUE(initial_db.has_value()) << initial_db.error().to_string();
  auto before_expiry = initial_db->load_series("short-40d-21d-settlement", "SPY");
  ASSERT_TRUE(before_expiry.has_value()) << before_expiry.error().to_string();
  ASSERT_EQ(before_expiry->backtest.size(), kInitialRows);
  ASSERT_EQ(before_expiry->checkpoint.portfolio.lots.size(), 2u);
  EXPECT_EQ(before_expiry->checkpoint.portfolio.lots[0].expiry_ts_ns, kExpectedExpiry);
  EXPECT_EQ(before_expiry->checkpoint.portfolio.lots[1].expiry_ts_ns, kExpectedExpiry);
  ASSERT_EQ(before_expiry->checkpoint.hedge_shares.size(), 1u);
  EXPECT_NE(before_expiry->checkpoint.hedge_shares.front().shares, 0.0);
  EXPECT_EQ(before_expiry->next_cohort, 1u);

  write_dates(*surface_db, kInitialRows, kAppendedRows);
  auto extension_report = build_backtest_db(incremental_spec);
  ASSERT_TRUE(extension_report.has_value()) << extension_report.error().to_string();
  EXPECT_EQ(extension_report->n_extended, 1u);
  EXPECT_EQ(extension_report->rows_computed, kAppendedRows);
  EXPECT_EQ(extension_report->rows_added, kAppendedRows);
  auto incremental_db = BacktestDb::open(incremental_root.string());
  ASSERT_TRUE(incremental_db.has_value()) << incremental_db.error().to_string();
  auto incremental = incremental_db->load_series("short-40d-21d-settlement", "SPY");
  ASSERT_TRUE(incremental.has_value()) << incremental.error().to_string();

  BacktestDbBuildSpec one_shot_spec = make_settlement_spec(surface_root, one_shot_root);
  auto one_shot_report = build_backtest_db(one_shot_spec);
  ASSERT_TRUE(one_shot_report.has_value()) << one_shot_report.error().to_string();
  ASSERT_EQ(one_shot_report->n_full, 1u);
  auto one_shot_db = BacktestDb::open(one_shot_root.string());
  ASSERT_TRUE(one_shot_db.has_value()) << one_shot_db.error().to_string();
  auto one_shot = one_shot_db->load_series("short-40d-21d-settlement", "SPY");
  ASSERT_TRUE(one_shot.has_value()) << one_shot.error().to_string();

  ASSERT_EQ(incremental->backtest.size(), kTotalRows);
  ASSERT_EQ(incremental->backtest.ts_ns[kExpiryIndex], kExpectedExpiry);
  EXPECT_EQ(incremental->backtest.n_open_lots[kExpiryIndex - 1u], 2.0);
  EXPECT_EQ(incremental->backtest.n_open_lots[kExpiryIndex], 0.0);
  EXPECT_EQ(incremental->backtest.n_open_lots.back(), 0.0);
  EXPECT_NE(incremental->backtest.pnl_settlement[kExpiryIndex], 0.0);
  EXPECT_TRUE(incremental->checkpoint.portfolio.lots.empty());
  ASSERT_EQ(incremental->checkpoint.hedge_shares.size(), 1u);
  EXPECT_DOUBLE_EQ(incremental->checkpoint.hedge_shares.front().shares, 0.0);
  EXPECT_DOUBLE_EQ(incremental->checkpoint.cash, incremental->backtest.cash.back());
  EXPECT_DOUBLE_EQ(incremental->checkpoint.nav, incremental->backtest.nav.back());
  EXPECT_EQ(incremental->checkpoint.completed_step_index, kTotalRows - 1u);
  EXPECT_EQ(incremental->next_cohort, 1u);

  expect_same_result(incremental->backtest, one_shot->backtest);
  EXPECT_EQ(incremental->checkpoint, one_shot->checkpoint);
  EXPECT_EQ(incremental->next_cohort, one_shot->next_cohort);

  std::error_code ec;
  fs::remove_all(surface_root, ec);
  fs::remove_all(incremental_root, ec);
  fs::remove_all(one_shot_root, ec);
}

TEST(BacktestDbBuild, HistoricalSurfaceRewriteForcesFullCellRebuild) {
  const fs::path surface_root = clean_root("rewrite-surface");
  const fs::path backtest_root = clean_root("rewrite-backtest");
  const fs::path fresh_root = clean_root("rewrite-fresh");
  auto surface_db = SurfaceDb::create(surface_root.string());
  ASSERT_TRUE(surface_db.has_value()) << surface_db.error().to_string();
  write_dates(*surface_db, 0u, 4u);

  BacktestDbBuildSpec spec = make_spec(surface_root, backtest_root);
  auto first = build_backtest_db(spec);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  EXPECT_EQ(first->n_full, 1u);

  write_dates(*surface_db, 1u, 1u, 0.03);
  auto rebuilt = build_backtest_db(spec);
  ASSERT_TRUE(rebuilt.has_value()) << rebuilt.error().to_string();
  EXPECT_EQ(rebuilt->n_rebuilt, 1u);
  EXPECT_EQ(rebuilt->rows_computed, 4u);

  BacktestDbBuildSpec fresh_spec = make_spec(surface_root, fresh_root);
  auto fresh_report = build_backtest_db(fresh_spec);
  ASSERT_TRUE(fresh_report.has_value()) << fresh_report.error().to_string();
  auto rebuilt_db = BacktestDb::open(backtest_root.string());
  auto fresh_db = BacktestDb::open(fresh_root.string());
  ASSERT_TRUE(rebuilt_db.has_value()) << rebuilt_db.error().to_string();
  ASSERT_TRUE(fresh_db.has_value()) << fresh_db.error().to_string();
  auto actual = rebuilt_db->load_series("short-40d-3m-daily", "SPY");
  auto expected = fresh_db->load_series("short-40d-3m-daily", "SPY");
  ASSERT_TRUE(actual.has_value()) << actual.error().to_string();
  ASSERT_TRUE(expected.has_value()) << expected.error().to_string();
  expect_same_result(actual->backtest, expected->backtest);

  std::error_code ec;
  fs::remove_all(surface_root, ec);
  fs::remove_all(backtest_root, ec);
  fs::remove_all(fresh_root, ec);
}

TEST(BacktestDbBuild, DateRangeRegressionIsRefusedAndStoredCoverageSurvives) {
  const fs::path surface_root = clean_root("regression-surface");
  const fs::path backtest_root = clean_root("regression-backtest");
  auto surface_db = SurfaceDb::create(surface_root.string());
  ASSERT_TRUE(surface_db.has_value()) << surface_db.error().to_string();
  write_dates(*surface_db, 0u, 4u);

  BacktestDbBuildSpec spec = make_spec(surface_root, backtest_root);
  auto first = build_backtest_db(spec);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  spec.date_hi = "2026-01-07";
  auto refused = build_backtest_db(spec);
  ASSERT_TRUE(refused.has_value()) << refused.error().to_string();
  EXPECT_EQ(refused->n_failed, 1u);
  ASSERT_EQ(refused->cells.size(), 1u);
  EXPECT_EQ(refused->cells.front().mode, BacktestDbCellBuildMode::Failed);

  auto db = BacktestDb::open(backtest_root.string());
  ASSERT_TRUE(db.has_value()) << db.error().to_string();
  auto stored = db->load_series("short-40d-3m-daily", "SPY");
  ASSERT_TRUE(stored.has_value()) << stored.error().to_string();
  EXPECT_EQ(stored->backtest.size(), 4u);
  EXPECT_EQ(stored->sources.size(), 4u);

  std::error_code ec;
  fs::remove_all(surface_root, ec);
  fs::remove_all(backtest_root, ec);
}

TEST(BacktestDbBuild, LeadingPreListingDatesAreSkippedButInteriorGapsFailClosed) {
  const fs::path leading_surface_root = clean_root("leading-surface");
  const fs::path leading_backtest_root = clean_root("leading-backtest");
  auto leading_db = SurfaceDb::create(leading_surface_root.string());
  ASSERT_TRUE(leading_db.has_value()) << leading_db.error().to_string();
  write_symbol_date(*leading_db, 0u, "QQQ");
  write_symbol_date(*leading_db, 1u, "QQQ");
  write_symbol_date(*leading_db, 2u, "SPY");
  write_symbol_date(*leading_db, 3u, "SPY");

  BacktestDbBuildSpec leading_spec = make_spec(leading_surface_root, leading_backtest_root);
  auto leading = build_backtest_db(leading_spec);
  ASSERT_TRUE(leading.has_value()) << leading.error().to_string();
  EXPECT_EQ(leading->n_full, 1u);
  ASSERT_EQ(leading->cells.size(), 1u);
  EXPECT_EQ(leading->cells.front().source_dates, 2u);
  auto stored_db = BacktestDb::open(leading_backtest_root.string());
  ASSERT_TRUE(stored_db.has_value()) << stored_db.error().to_string();
  auto stored = stored_db->load_series("short-40d-3m-daily", "SPY");
  ASSERT_TRUE(stored.has_value()) << stored.error().to_string();
  ASSERT_EQ(stored->sources.size(), 2u);
  EXPECT_EQ(stored->sources.front().date, "2026-01-07");

  const fs::path gap_surface_root = clean_root("gap-surface");
  const fs::path gap_backtest_root = clean_root("gap-backtest");
  auto gap_db = SurfaceDb::create(gap_surface_root.string());
  ASSERT_TRUE(gap_db.has_value()) << gap_db.error().to_string();
  write_symbol_date(*gap_db, 0u, "SPY");
  write_symbol_date(*gap_db, 1u, "QQQ");
  write_symbol_date(*gap_db, 2u, "SPY");
  BacktestDbBuildSpec gap_spec = make_spec(gap_surface_root, gap_backtest_root);
  auto gap = build_backtest_db(gap_spec);
  ASSERT_TRUE(gap.has_value()) << gap.error().to_string();
  EXPECT_EQ(gap->n_failed, 1u);
  auto gap_store = BacktestDb::open(gap_backtest_root.string());
  ASSERT_TRUE(gap_store.has_value()) << gap_store.error().to_string();
  EXPECT_EQ(gap_store->series().size(), 0u);

  std::error_code ec;
  fs::remove_all(leading_surface_root, ec);
  fs::remove_all(leading_backtest_root, ec);
  fs::remove_all(gap_surface_root, ec);
  fs::remove_all(gap_backtest_root, ec);
}
