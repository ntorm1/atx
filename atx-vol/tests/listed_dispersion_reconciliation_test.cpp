#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp"
#include "atx/vol/corpus.hpp"
#include "atx/vol/listed_dispersion_reconciliation.hpp"
#include "atx/vol/listed_dispersion_strategy.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 0.04;
constexpr std::int64_t kDayNs = 86'400'000'000'000LL;
constexpr std::int64_t kNow0 = 1'700'000'000'000'000'000LL;
constexpr std::int64_t kNow1 = kNow0 + kDayNs;
constexpr std::int64_t kNow2 = kNow1 + kDayNs;
constexpr std::int64_t kExpiry0 = kNow0 + static_cast<std::int64_t>(0.10 * kNsPerYear);
constexpr std::int64_t kExpiry1 = kNow1 + static_cast<std::int64_t>(0.10 * kNsPerYear);

PricedSurface make_surface(std::uint32_t uid, double spot, std::int64_t now) {
  CurveSurface curves;
  std::vector<SliceContext> context;
  std::uint16_t expiry_id = 0;
  for (const double term : {0.05, 0.10, 0.20, 0.50}) {
    const double forward = spot * std::exp((kRate - 0.02) * term);
    EssviParams parameters{};
    parameters.theta = 0.04 + 0.01 * term;
    parameters.phi = 1.4;
    parameters.rho = -0.35;
    parameters.psi = 0.5;
    parameters.p = 0.5;
    parameters.lambda = 0.5;
    parameters.T = term;
    parameters.F = forward;
    parameters.expiry_id = expiry_id++;
    curves.push(std::make_unique<EssviCurve>(parameters, std::exp(-kRate * term)));
    context.push_back(SliceContext{term, forward, 0.0, 0.02, 100, 7});
  }
  PricingContext pricing;
  pricing.S = spot;
  pricing.r = kRate;
  pricing.now_ts_ns = now;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = uid;
  auto result = PricedSurface::create(std::move(curves), std::move(context), pricing);
  EXPECT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  return std::move(*result);
}

std::vector<PricedSurface> surfaces(std::int64_t now, double shift) {
  std::vector<PricedSurface> result;
  result.push_back(make_surface(1u, 500.0 + shift, now));
  result.push_back(make_surface(2u, 100.0 + 0.3 * shift, now));
  result.push_back(make_surface(3u, 200.0 - 0.2 * shift, now));
  return result;
}

std::vector<const PricedSurface *> pointers(const std::vector<PricedSurface> &surfaces) {
  std::vector<const PricedSurface *> result;
  for (const PricedSurface &surface : surfaces) {
    result.push_back(&surface);
  }
  return result;
}

ListedOptionQuote option(std::string symbol, std::uint32_t id, double strike, Side side,
                         std::string date, std::int64_t now, std::int64_t expiry) {
  ListedOptionQuote quote;
  quote.trade_date = std::move(date);
  quote.symbol = std::move(symbol);
  quote.instrument_id = id;
  quote.raw_symbol = quote.symbol + std::to_string(id);
  quote.expiry_ts_ns = expiry;
  quote.strike = strike;
  quote.side = side;
  quote.bid = 1.0;
  quote.ask = 1.2;
  quote.quote_ts_ns = now;
  quote.multiplier = 100.0;
  quote.standard_monthly = true;
  quote.standard_deliverable = true;
  quote.source_fingerprint = 1000u + id;
  return quote;
}

ListedStraddle straddle(std::string symbol, std::uint32_t uid, std::uint32_t id, double strike,
                        double weight, const std::string &date, std::int64_t now,
                        std::int64_t expiry) {
  ListedStraddle result;
  result.symbol = std::move(symbol);
  result.uid = uid;
  result.expiry_ts_ns = expiry;
  result.strike = strike;
  result.call = option(result.symbol, id, strike, Side::Call, date, now, expiry);
  result.put = option(result.symbol, id + 1u, strike, Side::Put, date, now, expiry);
  result.raw_weight = weight;
  result.normalized_weight = weight;
  return result;
}

ListedDispersionSelection selection(const std::string &date, std::int64_t now, std::int64_t expiry,
                                    std::uint32_t id_base) {
  ListedDispersionSelection result;
  result.trade_date = date;
  result.valuation_ts_ns = now;
  result.expiry_ts_ns = expiry;
  result.dte_days = static_cast<double>(expiry - now) / kListedNsPerDay;
  result.index = straddle("SPY", 1u, id_base, 500.0, 0.0, date, now, expiry);
  result.names.push_back(straddle("N0", 2u, id_base + 2u, 100.0, 0.4, date, now, expiry));
  result.names.push_back(straddle("N1", 3u, id_base + 4u, 200.0, 0.6, date, now, expiry));
  return result;
}

ListedScheduleRoll roll(const ListedDispersionSelection &selected,
                        const std::vector<PricedSurface> &source, std::uint32_t cohort) {
  auto set = SurfaceSet::create(pointers(source));
  EXPECT_TRUE(set) << (set ? std::string{} : set.error().to_string());
  ListedScheduleBuildConfig config;
  config.gross_index_vega_target_per_vol_point = 1000.0;
  config.cohort = cohort;
  config.surface_fingerprint = 9000u + cohort;
  auto result = build_listed_dispersion_roll(selected, *set, config);
  EXPECT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  return std::move(*result);
}

std::vector<ListedOptionQuote> quotes_for(const ListedScheduleRoll &roll,
                                          const SurfaceSet &surfaces, const std::string &date,
                                          std::int64_t now, std::uint32_t id_offset) {
  std::vector<ListedOptionQuote> quotes;
  for (const ListedScheduleLeg &leg : roll.legs) {
    const PricedSurface *surface = surfaces.find(leg.uid);
    EXPECT_NE(surface, nullptr);
    const double term = static_cast<double>(leg.expiry_ts_ns - now) / kNsPerYear;
    auto mark = surface->fair_value(leg.strike, term, leg.side);
    EXPECT_TRUE(mark) << (mark ? std::string{} : mark.error().to_string());
    ListedOptionQuote quote;
    quote.trade_date = date;
    quote.symbol = leg.symbol;
    quote.instrument_id = leg.instrument_id + id_offset;
    quote.raw_symbol = leg.raw_symbol;
    quote.expiry_ts_ns = leg.expiry_ts_ns;
    quote.strike = leg.strike;
    quote.side = leg.side;
    quote.bid = std::max(0.0, *mark - 0.1);
    quote.ask = *mark + 0.1;
    quote.quote_ts_ns = now;
    quote.multiplier = leg.multiplier;
    quote.standard_monthly = true;
    quote.standard_deliverable = true;
    quote.source_fingerprint = 7000u + quote.instrument_id;
    quotes.push_back(std::move(quote));
  }
  return quotes;
}

fs::path fresh_dir() {
  const fs::path path = fs::temp_directory_path() / "atx-listed-reconciliation";
  std::error_code error;
  fs::remove_all(path, error);
  fs::create_directories(path, error);
  return path;
}

std::string write_archive(const fs::path &dir, const std::string &date,
                          const std::vector<PricedSurface> &surfaces) {
  const std::vector<SurfaceArchiveItem> items = {
      {"SPY", &surfaces[0]}, {"N0", &surfaces[1]}, {"N1", &surfaces[2]}};
  const std::string path = (dir / (date + ".atxvsa")).string();
  const Status status = write_surface_archive_v2_file(path, items);
  EXPECT_TRUE(status) << (status ? std::string{} : status.error().to_string());
  return path;
}

} // namespace

TEST(ListedDispersionReconciliation, ReconcilesHeldMarksAcrossAtomicRoll) {
  const std::vector<PricedSurface> day0 = surfaces(kNow0, 0.0);
  const std::vector<PricedSurface> day1 = surfaces(kNow1, 2.0);
  const std::vector<PricedSurface> day2 = surfaces(kNow2, -1.0);

  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(roll(selection("2026-07-10", kNow0, kExpiry0, 1u), day0, 4u));
  schedule.rolls.push_back(roll(selection("2026-07-11", kNow1, kExpiry1, 101u), day1, 5u));

  auto set0 = SurfaceSet::create(pointers(day0));
  auto set1 = SurfaceSet::create(pointers(day1));
  auto set2 = SurfaceSet::create(pointers(day2));
  ASSERT_TRUE(set0 && set1 && set2);
  std::vector<ListedOptionQuote> quotes0 =
      quotes_for(schedule.rolls[0], *set0, "2026-07-10", kNow0, 0u);
  std::vector<ListedOptionQuote> quotes1 =
      quotes_for(schedule.rolls[0], *set1, "2026-07-11", kNow1, 1000u);
  std::vector<ListedOptionQuote> entry1 =
      quotes_for(schedule.rolls[1], *set1, "2026-07-11", kNow1, 2000u);
  quotes1.insert(quotes1.end(), entry1.begin(), entry1.end());
  std::vector<ListedOptionQuote> quotes2 =
      quotes_for(schedule.rolls[1], *set2, "2026-07-12", kNow2, 3000u);

  const std::vector<ListedReconciliationSnapshot> snapshots = {
      {"2026-07-10", kNow0, &*set0, quotes0},
      {"2026-07-11", kNow1, &*set1, quotes1},
      {"2026-07-12", kNow2, &*set2, quotes2},
  };
  auto result = reconcile_listed_dispersion(schedule, snapshots);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  ASSERT_EQ(result->rows.size(), 3u);
  EXPECT_EQ(result->rows[1].held_cohort, 4u);
  EXPECT_EQ(result->rows[2].held_cohort, 5u);
  EXPECT_DOUBLE_EQ(result->rows[1].quote_mid_coverage, 1.0);
  EXPECT_DOUBLE_EQ(result->rows[2].quote_mid_coverage, 1.0);
  EXPECT_NE(result->rows[1].model_option_pnl, 0.0);
  EXPECT_DOUBLE_EQ(result->rows[1].model_minus_quote_pnl,
                   result->rows[1].model_option_pnl - result->rows[1].quote_mid_pnl);

  const std::size_t legs = schedule.rolls[0].legs.size();
  ASSERT_EQ(result->marks.size(), legs * 4u);
  EXPECT_EQ(result->marks[0].role, ListedMarkRole::Entry);
  EXPECT_EQ(result->marks[legs].role, ListedMarkRole::Held);
  EXPECT_EQ(result->marks[legs * 2u].role, ListedMarkRole::Entry);
  EXPECT_EQ(result->marks[legs * 3u].role, ListedMarkRole::Held);

  const std::string marks = serialize_listed_contract_marks(*result);
  const std::string reconciliation = serialize_listed_reconciliation(*result);
  EXPECT_NE(marks.find("\tHeld\t4\tSPY\t"), std::string::npos);
  EXPECT_NE(reconciliation.find("model_minus_quote_pnl"), std::string::npos);
}

TEST(ListedDispersionReconciliation, ExactModelPnlClosesToCanonicalBacktest) {
  const std::vector<PricedSurface> day0 = surfaces(kNow0, 0.0);
  const std::vector<PricedSurface> day1 = surfaces(kNow1, 2.0);
  const std::vector<PricedSurface> day2 = surfaces(kNow2, -1.0);
  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(roll(selection("2026-07-10", kNow0, kExpiry0, 1u), day0, 4u));
  schedule.rolls.push_back(roll(selection("2026-07-11", kNow1, kExpiry1, 101u), day1, 5u));

  auto set0 = SurfaceSet::create(pointers(day0));
  auto set1 = SurfaceSet::create(pointers(day1));
  auto set2 = SurfaceSet::create(pointers(day2));
  ASSERT_TRUE(set0 && set1 && set2);
  std::vector<ListedOptionQuote> quotes0 =
      quotes_for(schedule.rolls[0], *set0, "2026-07-10", kNow0, 0u);
  std::vector<ListedOptionQuote> quotes1 =
      quotes_for(schedule.rolls[0], *set1, "2026-07-11", kNow1, 1000u);
  std::vector<ListedOptionQuote> entry1 =
      quotes_for(schedule.rolls[1], *set1, "2026-07-11", kNow1, 2000u);
  quotes1.insert(quotes1.end(), entry1.begin(), entry1.end());
  std::vector<ListedOptionQuote> quotes2 =
      quotes_for(schedule.rolls[1], *set2, "2026-07-12", kNow2, 3000u);
  const std::vector<ListedReconciliationSnapshot> snapshots = {
      {"2026-07-10", kNow0, &*set0, quotes0},
      {"2026-07-11", kNow1, &*set1, quotes1},
      {"2026-07-12", kNow2, &*set2, quotes2},
  };
  auto reconciliation = reconcile_listed_dispersion(schedule, snapshots);
  ASSERT_TRUE(reconciliation) << (reconciliation ? std::string{}
                                                 : reconciliation.error().to_string());

  const fs::path dir = fresh_dir();
  CorpusManifest manifest;
  manifest.dates = {"2026-07-10", "2026-07-11", "2026-07-12"};
  manifest.entries = {
      {"2026-07-10", "SPY", CorpusFitStatus::Ok, VolCurveKind::Essvi, 4u, 0.0, ErrorCode::Unknown,
       write_archive(dir, "2026-07-10", day0)},
      {"2026-07-11", "SPY", CorpusFitStatus::Ok, VolCurveKind::Essvi, 4u, 0.0, ErrorCode::Unknown,
       write_archive(dir, "2026-07-11", day1)},
      {"2026-07-12", "SPY", CorpusFitStatus::Ok, VolCurveKind::Essvi, 4u, 0.0, ErrorCode::Unknown,
       write_archive(dir, "2026-07-12", day2)},
  };
  manifest.n_boards = 3u;
  manifest.n_ok = 3u;
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock) << (clock ? std::string{} : clock.error().to_string());
  auto strategy = ListedDispersionStrategy::create(schedule, 0.0);
  ASSERT_TRUE(strategy) << (strategy ? std::string{} : strategy.error().to_string());
  RunConfig config;
  config.unpriced = UnpricedLotPolicy::Error;
  auto backtest = run_backtest(*clock, *strategy, config);
  ASSERT_TRUE(backtest) << (backtest ? std::string{} : backtest.error().to_string());
  const Status validation =
      validate_listed_reconciliation_backtest(*reconciliation, *backtest, 1.0e-7);
  EXPECT_TRUE(validation) << (validation ? std::string{} : validation.error().to_string());

  std::error_code error;
  fs::remove_all(dir, error);
}

TEST(ListedDispersionReconciliation, MissingRawQuoteReducesCoverageWithoutPatchingModel) {
  const std::vector<PricedSurface> day0 = surfaces(kNow0, 0.0);
  const std::vector<PricedSurface> day1 = surfaces(kNow1, 2.0);
  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(roll(selection("2026-07-10", kNow0, kExpiry0, 1u), day0, 4u));
  auto set0 = SurfaceSet::create(pointers(day0));
  auto set1 = SurfaceSet::create(pointers(day1));
  ASSERT_TRUE(set0 && set1);
  std::vector<ListedOptionQuote> quotes0 =
      quotes_for(schedule.rolls[0], *set0, "2026-07-10", kNow0, 0u);
  std::vector<ListedOptionQuote> quotes1 =
      quotes_for(schedule.rolls[0], *set1, "2026-07-11", kNow1, 1000u);
  quotes1.pop_back();
  const std::vector<ListedReconciliationSnapshot> snapshots = {
      {"2026-07-10", kNow0, &*set0, quotes0},
      {"2026-07-11", kNow1, &*set1, quotes1},
  };
  auto result = reconcile_listed_dispersion(schedule, snapshots);
  ASSERT_TRUE(result) << (result ? std::string{} : result.error().to_string());
  EXPECT_LT(result->rows.back().quote_mid_coverage, 1.0);
  EXPECT_NE(result->rows.back().model_option_pnl, 0.0);
  EXPECT_EQ(result->marks.back().status, ListedMarkStatus::NoRawQuote);
}

TEST(ListedDispersionReconciliation, RejectsMissingSurfaceAndEntryMarkMismatch) {
  const std::vector<PricedSurface> day0 = surfaces(kNow0, 0.0);
  ListedDispersionSchedule schedule;
  schedule.rolls.push_back(roll(selection("2026-07-10", kNow0, kExpiry0, 1u), day0, 4u));
  std::vector<const PricedSurface *> incomplete = {&day0[0], &day0[1]};
  auto set = SurfaceSet::create(incomplete);
  ASSERT_TRUE(set);
  std::vector<ListedOptionQuote> quotes =
      quotes_for(schedule.rolls[0], *SurfaceSet::create(pointers(day0)), "2026-07-10", kNow0, 0u);
  const std::vector<ListedReconciliationSnapshot> missing = {
      {"2026-07-10", kNow0, &*set, quotes},
  };
  EXPECT_FALSE(reconcile_listed_dispersion(schedule, missing));

  auto complete = SurfaceSet::create(pointers(day0));
  ASSERT_TRUE(complete);
  schedule.rolls.front().legs.front().model_mark += 0.01;
  const std::vector<ListedReconciliationSnapshot> mismatch = {
      {"2026-07-10", kNow0, &*complete, quotes},
  };
  EXPECT_FALSE(reconcile_listed_dispersion(schedule, mismatch));
}
