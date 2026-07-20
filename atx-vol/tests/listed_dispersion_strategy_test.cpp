#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/backtest.hpp"
#include "atx/vol/corpus.hpp"
#include "atx/vol/listed_dispersion.hpp"
#include "atx/vol/listed_dispersion_schedule.hpp"
#include "atx/vol/listed_dispersion_strategy.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/types.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kNow = 1'700'000'000'000'000'000LL;
constexpr std::int64_t kExpiry = kNow + static_cast<std::int64_t>(0.10 * kNsPerYear);

[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double spot) {
  CurveSurface curves;
  std::vector<SliceContext> context;
  const double terms[] = {0.05, 0.10, 0.20, 0.50};
  std::uint16_t expiry_id = 0;
  for (const double T : terms) {
    EssviParams params{};
    params.theta = 0.04 + 0.01 * T;
    params.phi = 1.4;
    params.rho = -0.35;
    params.psi = 0.5;
    params.p = 0.5;
    params.lambda = 0.5;
    params.T = T;
    params.F = spot;
    params.expiry_id = expiry_id++;
    curves.push(std::make_unique<EssviCurve>(params, std::exp(-kR * T)));
    context.push_back(SliceContext{T, spot, 0.0, 0.02, 100, 7});
  }
  PricingContext pricing;
  pricing.S = spot;
  pricing.r = kR;
  pricing.now_ts_ns = kNow;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = uid;
  auto result = PricedSurface::create(std::move(curves), std::move(context), pricing);
  EXPECT_TRUE(result.has_value()) << result.error().to_string();
  return std::move(*result);
}

[[nodiscard]] ListedOptionQuote option(std::string symbol, std::uint32_t id, double strike,
                                       Side side) {
  ListedOptionQuote q;
  q.trade_date = "2026-07-10";
  q.symbol = std::move(symbol);
  q.instrument_id = id;
  q.raw_symbol = q.symbol + std::to_string(id);
  q.expiry_ts_ns = kExpiry;
  q.strike = strike;
  q.side = side;
  q.bid = 1.0;
  q.ask = 1.2;
  q.quote_ts_ns = kNow;
  q.standard_monthly = true;
  q.standard_deliverable = true;
  q.source_fingerprint = 100u + id;
  return q;
}

[[nodiscard]] ListedStraddle straddle(std::string symbol, std::uint32_t uid, std::uint32_t id,
                                      double strike, double weight) {
  ListedStraddle result;
  result.symbol = std::move(symbol);
  result.uid = uid;
  result.expiry_ts_ns = kExpiry;
  result.strike = strike;
  result.call = option(result.symbol, id, strike, Side::Call);
  result.put = option(result.symbol, id + 1u, strike, Side::Put);
  result.raw_weight = weight;
  result.normalized_weight = weight;
  return result;
}

[[nodiscard]] ListedDispersionSelection selection() {
  ListedDispersionSelection result;
  result.trade_date = "2026-07-10";
  result.valuation_ts_ns = kNow;
  result.expiry_ts_ns = kExpiry;
  result.dte_days = static_cast<double>(kExpiry - kNow) / kListedNsPerDay;
  result.index = straddle("SPY", 1u, 1u, 500.0, 0.0);
  result.names.push_back(straddle("N0", 2u, 3u, 100.0, 0.4));
  result.names.push_back(straddle("N1", 3u, 5u, 200.0, 0.6));
  return result;
}

[[nodiscard]] std::vector<const PricedSurface *>
pointers(const std::vector<PricedSurface> &surfaces) {
  std::vector<const PricedSurface *> result;
  for (const PricedSurface &surface : surfaces)
    result.push_back(&surface);
  return result;
}

[[nodiscard]] ListedDispersionSchedule schedule_from(const std::vector<PricedSurface> &surfaces) {
  auto set = SurfaceSet::create(pointers(surfaces));
  EXPECT_TRUE(set.has_value()) << set.error().to_string();
  ListedScheduleBuildConfig cfg;
  cfg.gross_index_vega_target_per_vol_point = 1000.0;
  cfg.cohort = 4u;
  cfg.surface_fingerprint = 12345u;
  auto roll = build_listed_dispersion_roll(selection(), *set, cfg);
  EXPECT_TRUE(roll.has_value()) << roll.error().to_string();
  ListedDispersionSchedule result;
  result.rolls.push_back(std::move(*roll));
  return result;
}

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path path =
      fs::temp_directory_path() / (std::string{"atx-listed-dispersion-strategy-"} + tag);
  std::error_code ec;
  fs::remove_all(path, ec);
  fs::create_directories(path, ec);
  return path;
}

[[nodiscard]] std::string write_archive(const fs::path &dir,
                                        const std::vector<PricedSurface> &surfaces) {
  const std::string path = (dir / "2026-07-10.atxvsa").string();
  const std::vector<SurfaceArchiveItem> items = {
      {"SPY", &surfaces[0]}, {"N0", &surfaces[1]}, {"N1", &surfaces[2]}};
  const Status status = write_surface_archive_v2_file(path, items);
  EXPECT_TRUE(status.has_value()) << status.error().to_string();
  return path;
}

[[nodiscard]] std::vector<PricedSurface> surfaces() {
  std::vector<PricedSurface> result;
  result.push_back(make_surface(1u, 500.0));
  result.push_back(make_surface(2u, 100.0));
  result.push_back(make_surface(3u, 200.0));
  return result;
}

} // namespace

TEST(ListedDispersionStrategy, MaterializesExactScheduleLots) {
  const std::vector<PricedSurface> source = surfaces();
  const ListedDispersionSchedule schedule = schedule_from(source);
  auto lots = materialize_listed_dispersion_roll(schedule.rolls.front(), kNow, 100u);
  ASSERT_TRUE(lots.has_value()) << lots.error().to_string();
  ASSERT_EQ(lots->size(), schedule.rolls.front().legs.size());
  for (std::size_t i = 0; i < lots->size(); ++i) {
    const Lot &lot = (*lots)[i];
    const ListedScheduleLeg &leg = schedule.rolls.front().legs[i];
    EXPECT_EQ(lot.id, 100u + i);
    EXPECT_EQ(lot.contract.uid, leg.uid);
    EXPECT_DOUBLE_EQ(lot.contract.K, leg.strike);
    EXPECT_EQ(lot.contract.side, leg.side);
    EXPECT_DOUBLE_EQ(lot.qty, leg.quantity);
    EXPECT_EQ(lot.expiry_ts_ns, kExpiry);
    EXPECT_EQ(lot.cohort, 4u);
    EXPECT_DOUBLE_EQ(lot.entry_price, leg.model_mark);
  }
}

TEST(ListedDispersionStrategy, AtomicallyOpensArchiveVerifiedRollAndRequestsDailyHedge) {
  const std::vector<PricedSurface> source = surfaces();
  const ListedDispersionSchedule schedule = schedule_from(source);
  const fs::path dir = fresh_dir("open");
  auto snapshot = MarketSnapshot::load(write_archive(dir, source));
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  auto strategy = ListedDispersionStrategy::create(schedule, 5.0);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();

  PortfolioState book;
  Lot old;
  old.id = 77u;
  book.lots.push_back(old);
  std::uint64_t next_id = 100u;
  ASSERT_TRUE(strategy->on_step(*snapshot, 0u, book, next_id).has_value());
  EXPECT_TRUE(strategy->all_rolls_consumed());
  EXPECT_EQ(next_id, 100u + schedule.rolls.front().legs.size());
  ASSERT_EQ(book.lots.size(), schedule.rolls.front().legs.size());
  EXPECT_EQ(book.lots.front().id, 100u);
  EXPECT_EQ(strategy->hedge_spec().kind, HedgeSpec::Kind::DeltaToZero);
  EXPECT_EQ(strategy->hedge_spec().cadence, HedgeSpec::Cadence::Daily);
  EXPECT_DOUBLE_EQ(strategy->hedge_spec().band, 5.0);

  const std::vector<Lot> opened = book.lots;
  ASSERT_TRUE(strategy->on_step(*snapshot, 1u, book, next_id).has_value());
  EXPECT_EQ(book.lots.size(), opened.size());
  EXPECT_EQ(next_id, 100u + schedule.rolls.front().legs.size());
  EXPECT_TRUE(strategy->entry_risk_seeds().empty());
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(ListedDispersionStrategy, ForcedColdValidationPublishesExactEntrySeedsOnFastSnapshot) {
  const std::vector<PricedSurface> source = surfaces();
  const ListedDispersionSchedule schedule = schedule_from(source);
  const fs::path dir = fresh_dir("cold-seeds-fast-snapshot");
  auto snapshot =
      MarketSnapshot::load(write_archive(dir, source), QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  auto strategy = ListedDispersionStrategy::create(schedule);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();

  PriceOptions options;
  options.analytic_greeks = true;
  options.query_execution = QueryExecution::ColdReference;
  PortfolioState book;
  std::uint64_t next_id = 100u;
  ASSERT_TRUE(strategy->on_step(*snapshot, 0u, book, next_id, options).has_value());
  ASSERT_EQ(book.lots.size(), schedule.rolls.front().legs.size());
  ASSERT_EQ(strategy->entry_risk_seeds().size(), book.lots.size());
  for (std::size_t i = 0; i < book.lots.size(); ++i) {
    const Lot &lot = book.lots[i];
    const FullGreekSeed &seed = strategy->entry_risk_seeds()[i];
    EXPECT_EQ(seed.uid(), lot.contract.uid) << i;
    EXPECT_EQ(seed.K(), lot.contract.K) << i;
    EXPECT_EQ(seed.T(), lot.contract.T) << i;
    EXPECT_EQ(seed.side(), lot.contract.side) << i;
    EXPECT_TRUE(seed.analytic_greeks()) << i;
    EXPECT_EQ(seed.query_execution(), QueryExecution::ColdReference) << i;
    EXPECT_EQ(seed.greeks().price, lot.entry_price) << i;
  }

  ASSERT_TRUE(strategy->on_step(*snapshot, 1u, book, next_id, options).has_value());
  EXPECT_TRUE(strategy->entry_risk_seeds().empty());
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(ListedDispersionStrategy, ColdAuthoredScheduleRejectsFastConfiguredBeforeArchiveLoad) {
  const std::vector<PricedSurface> source = surfaces();
  const ListedDispersionSchedule schedule = schedule_from(source);
  const fs::path dir = fresh_dir("cold-required-engine-gate");
  const std::string archive_path = write_archive(dir, source);
  CorpusManifest manifest;
  manifest.dates.push_back("2026-07-10");
  CorpusEntry entry;
  entry.date = "2026-07-10";
  entry.symbol = "SPY";
  entry.status = CorpusFitStatus::Ok;
  entry.archive_path = archive_path;
  manifest.entries.push_back(std::move(entry));
  auto clock = Clock::from_manifest(manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  auto strategy = ListedDispersionStrategy::create(schedule);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();
  EXPECT_EQ(strategy->required_economic_execution(), QueryExecution::ColdReference);

  RunConfig fast_configured;
  fast_configured.query_pricing_tier = QueryPricingTier::RepresentativeFast;
  fast_configured.price.query_execution = QueryExecution::Configured;
  fast_configured.prefetch_snapshots = false;
  MarketSnapshot::reset_open_count();
  const auto rejected = run_backtest(*clock, *strategy, fast_configured);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(rejected.error().message().find("requires ColdReference"), std::string::npos);
  EXPECT_EQ(MarketSnapshot::open_count(), 0u);
  EXPECT_EQ(strategy->next_roll_index(), 0u);

  RunConfig forced_cold = fast_configured;
  forced_cold.price.query_execution = QueryExecution::ColdReference;
  const auto accepted = run_backtest(*clock, *strategy, forced_cold);
  ASSERT_TRUE(accepted.has_value()) << accepted.error().to_string();
  EXPECT_EQ(MarketSnapshot::open_count(), 1u);
  EXPECT_TRUE(strategy->all_rolls_consumed());
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(ListedDispersionStrategy, MarkMismatchLeavesBookCounterAndCursorUntouched) {
  const std::vector<PricedSurface> source = surfaces();
  ListedDispersionSchedule schedule = schedule_from(source);
  schedule.rolls.front().legs.front().model_mark += 0.01;
  const fs::path dir = fresh_dir("mismatch");
  auto snapshot = MarketSnapshot::load(write_archive(dir, source));
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  auto strategy = ListedDispersionStrategy::create(schedule);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();

  PortfolioState book;
  Lot old;
  old.id = 77u;
  book.lots.push_back(old);
  std::uint64_t next_id = 100u;
  const Status status = strategy->on_step(*snapshot, 0u, book, next_id);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::Unavailable);
  ASSERT_EQ(book.lots.size(), 1u);
  EXPECT_EQ(book.lots.front().id, 77u);
  EXPECT_EQ(next_id, 100u);
  EXPECT_EQ(strategy->next_roll_index(), 0u);
  EXPECT_TRUE(strategy->entry_risk_seeds().empty());
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(ListedDispersionStrategy, ExactArchivePolicyStillRejectsPerturbedMark) {
  const std::vector<PricedSurface> source = surfaces();
  ListedDispersionSchedule schedule = schedule_from(source);
  schedule.rolls.front().legs.front().model_mark += 0.01;
  const fs::path dir = fresh_dir("exact-archive-reject");
  auto snapshot = MarketSnapshot::load(write_archive(dir, source));
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  auto strategy =
      ListedDispersionStrategy::create(schedule, 0.0, ScheduleMarkPolicy::ExactArchive);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();

  PortfolioState book;
  std::uint64_t next_id = 100u;
  const Status status = strategy->on_step(*snapshot, 0u, book, next_id);
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::Unavailable);
  EXPECT_NE(status.error().message().find("archive mark differs from schedule"), std::string::npos);
  EXPECT_TRUE(strategy->last_mark_divergences().empty());
  EXPECT_EQ(strategy->next_roll_index(), 0u);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(ListedDispersionStrategy, RecordPolicyAcceptsPerturbedMarkAndRecordsDivergence) {
  const std::vector<PricedSurface> source = surfaces();
  ListedDispersionSchedule schedule = schedule_from(source);
  // Live mark from the archive equals the un-perturbed schedule mark bit-for-bit
  // (see AtomicallyOpens*). Perturb one leg so exactly that leg diverges.
  const double live_mark = schedule.rolls.front().legs.front().model_mark;
  schedule.rolls.front().legs.front().model_mark += 0.01;
  const ListedScheduleLeg perturbed = schedule.rolls.front().legs.front();

  const fs::path dir = fresh_dir("record-divergence");
  auto snapshot = MarketSnapshot::load(write_archive(dir, source));
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
  auto strategy = ListedDispersionStrategy::create(schedule, 0.0, ScheduleMarkPolicy::Record);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();

  PortfolioState book;
  std::uint64_t next_id = 100u;
  const Status status = strategy->on_step(*snapshot, 0u, book, next_id);
  ASSERT_TRUE(status.has_value()) << status.error().to_string();
  EXPECT_TRUE(strategy->all_rolls_consumed());
  ASSERT_EQ(book.lots.size(), schedule.rolls.front().legs.size());

  // Exactly the perturbed leg is recorded, with correct schedule/live values.
  const std::vector<MarkDivergence> &divs = strategy->last_mark_divergences();
  ASSERT_EQ(divs.size(), 1u);
  EXPECT_EQ(divs.front().uid, perturbed.uid);
  EXPECT_DOUBLE_EQ(divs.front().strike, perturbed.strike);
  EXPECT_EQ(divs.front().expiry_ts_ns, perturbed.expiry_ts_ns);
  EXPECT_EQ(divs.front().side, perturbed.side);
  EXPECT_DOUBLE_EQ(divs.front().schedule_mark, perturbed.model_mark);
  EXPECT_DOUBLE_EQ(divs.front().live_mark, live_mark);

  // entry_price is the live seed price under Record: the perturbed lot uses the
  // live mark (not the perturbed schedule mark); untouched legs are self-equal.
  EXPECT_DOUBLE_EQ(book.lots.front().entry_price, live_mark);
  EXPECT_NE(book.lots.front().entry_price, perturbed.model_mark);
  for (std::size_t i = 1; i < book.lots.size(); ++i) {
    EXPECT_DOUBLE_EQ(book.lots[i].entry_price,
                     schedule.rolls.front().legs[i].model_mark)
        << i;
  }

  // Cleared per step: the second (no-op) step leaves no divergences behind.
  ASSERT_TRUE(strategy->on_step(*snapshot, 1u, book, next_id).has_value());
  EXPECT_TRUE(strategy->last_mark_divergences().empty());
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(ListedDispersionStrategy, RejectsInvalidScheduleOrDeltaBand) {
  ListedDispersionSchedule empty;
  auto bad_schedule = ListedDispersionStrategy::create(std::move(empty));
  ASSERT_FALSE(bad_schedule.has_value());
  EXPECT_EQ(bad_schedule.error().code(), ErrorCode::InvalidArgument);

  const std::vector<PricedSurface> source = surfaces();
  auto bad_band = ListedDispersionStrategy::create(schedule_from(source), -1.0);
  ASSERT_FALSE(bad_band.has_value());
  EXPECT_EQ(bad_band.error().code(), ErrorCode::InvalidArgument);
}
