#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"
#include "atx/vol/backtest.hpp"
#include "atx/vol/corpus.hpp"
#include "atx/vol/listed_dispersion.hpp"
#include "atx/vol/listed_dispersion_reconciliation.hpp" // ListedReconciliationConfig (shared tol)
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

TEST(ListedDispersionStrategy, RequiredEconomicExecutionIsPolicyAware) {
  const std::vector<PricedSurface> source = surfaces();
  const ListedDispersionSchedule schedule = schedule_from(source);

  // Default and ExactArchive replay the cold archive marks exactly, so their
  // economics must stay ColdReference (the engine gate keeps them off any fast
  // tier). Record deliberately reprices the frozen definitions through the
  // interpolated Configured route, so it requires Configured economics.
  auto def = ListedDispersionStrategy::create(schedule);
  ASSERT_TRUE(def.has_value()) << def.error().to_string();
  EXPECT_EQ(def->required_economic_execution(), QueryExecution::ColdReference);

  auto exact = ListedDispersionStrategy::create(schedule, 0.0, ScheduleMarkPolicy::ExactArchive);
  ASSERT_TRUE(exact.has_value()) << exact.error().to_string();
  EXPECT_EQ(exact->required_economic_execution(), QueryExecution::ColdReference);

  auto record = ListedDispersionStrategy::create(schedule, 0.0, ScheduleMarkPolicy::Record);
  ASSERT_TRUE(record.has_value()) << record.error().to_string();
  EXPECT_EQ(record->required_economic_execution(), QueryExecution::Configured);
}

TEST(ListedDispersionStrategy, RecordPolicyAcceptsFastConfiguredRun) {
  const std::vector<PricedSurface> source = surfaces();
  const ListedDispersionSchedule schedule = schedule_from(source);
  const fs::path dir = fresh_dir("record-fast-configured-accept");
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
  auto strategy = ListedDispersionStrategy::create(schedule, 0.0, ScheduleMarkPolicy::Record);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();
  EXPECT_EQ(strategy->required_economic_execution(), QueryExecution::Configured);

  // The cold-authored ExactArchive strategy is gated off [fast tier + Configured]
  // (see ColdAuthoredScheduleRejectsFastConfiguredBeforeArchiveLoad); the Record
  // strategy relaxes that requirement and the same run is accepted and consumed.
  RunConfig fast_configured;
  fast_configured.query_pricing_tier = QueryPricingTier::RepresentativeFast;
  fast_configured.price.query_execution = QueryExecution::Configured;
  fast_configured.prefetch_snapshots = false;
  const auto accepted = run_backtest(*clock, *strategy, fast_configured);
  ASSERT_TRUE(accepted.has_value()) << accepted.error().to_string();
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

// WS-FIX regression. The entry-mark guard was a bit-exact `!=`, so the ~1e-10
// relative divergence between the schedule's BUILD-route mark and the seed route's
// re-price (laned analytic greeks, WS-P1a) made the SHIPPING strategy fail CLOSED —
// Unavailable on every roll of a perfectly valid run. The guard is now the same
// relative-tolerance compare the reconciliation guard uses. This pins BOTH ends:
// a sub-tolerance divergence must be ACCEPTED, and a genuine economic mismatch must
// still be REJECTED — a tolerance that swallows real breaks is not a fix.
TEST(ListedDispersionStrategy, SubToleranceMarkDriftIsAcceptedButRealMismatchStillRejected) {
  const std::vector<PricedSurface> source = surfaces();
  const fs::path dir = fresh_dir("entry-mark-tol");
  auto snapshot = MarketSnapshot::load(write_archive(dir, source));
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().to_string();

  const auto run = [&](double rel_perturb) {
    ListedDispersionSchedule schedule = schedule_from(source);
    double &mark = schedule.rolls.front().legs.front().model_mark;
    // Perturb RELATIVE to the mark's own scale, matching the guard's scale formula.
    mark += rel_perturb * std::max(std::fabs(mark), 1.0);
    auto strategy = ListedDispersionStrategy::create(schedule);
    EXPECT_TRUE(strategy.has_value());
    PortfolioState book;
    std::uint64_t next_id = 100u;
    return strategy->on_step(*snapshot, 0u, book, next_id);
  };

  // The default knob is the SHARED constant — the two guards cannot drift apart.
  {
    auto s = ListedDispersionStrategy::create(schedule_from(source));
    ASSERT_TRUE(s.has_value());
    EXPECT_DOUBLE_EQ(s->entry_mark_tolerance(), kListedEntryMarkTolerance);
    EXPECT_DOUBLE_EQ(ListedReconciliationConfig{}.entry_mark_tolerance,
                     s->entry_mark_tolerance());
  }

  // Accepted: a divergence an order of magnitude INSIDE the tolerance. This is the
  // case that was failing closed before the fix.
  EXPECT_TRUE(run(0.1 * kListedEntryMarkTolerance).has_value())
      << "sub-tolerance route divergence must not abort the roll";

  // Rejected: a genuine economic mismatch (1 cent on a ~1e2 mark), and also a
  // divergence just past the band — the guard is still a guard.
  const Status economic = run(1.0e-2);
  ASSERT_FALSE(economic.has_value());
  EXPECT_EQ(economic.error().code(), ErrorCode::Unavailable);

  const Status past_band = run(1.0e3 * kListedEntryMarkTolerance);
  ASSERT_FALSE(past_band.has_value()) << "a divergence past the band must still reject";
  EXPECT_EQ(past_band.error().code(), ErrorCode::Unavailable);

  // tol = 0.0 restores the historical bit-exact compare.
  {
    ListedDispersionSchedule schedule = schedule_from(source);
    // Exactly ONE ULP up — the smallest representable divergence. (A fixed
    // absolute epsilon like 1e-15 is below one ULP at this mark's ~1e2 scale and
    // would be absorbed, silently testing nothing.)
    double &mark = schedule.rolls.front().legs.front().model_mark;
    mark = std::nextafter(mark, std::numeric_limits<double>::infinity());
    auto strategy = ListedDispersionStrategy::create(schedule);
    ASSERT_TRUE(strategy.has_value());
    strategy->set_entry_mark_tolerance(0.0);
    PortfolioState book;
    std::uint64_t next_id = 100u;
    EXPECT_FALSE(strategy->on_step(*snapshot, 0u, book, next_id).has_value())
        << "tol=0 must reject any nonzero divergence";
  }

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

// â”€â”€ WS-F F2 (BT-P1-1): quote-side fills â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// The listed route selects on real NBBO and RECORDS it (`raw_bid`/`raw_ask`/
// `raw_mid`) but has always filled at `model_mark` â€” its own fitted value. Any
// signal correlated with the fit's deviation from tradeable quotes (dispersion
// implied correlation is exactly that) therefore booked the fit error as day-0
// PnL that no market participant could capture.
//
// The fixture below is that scenario, made explicit: every leg is re-quoted so
// the NBBO brackets the model mark and sits on the UNFAVOURABLE side of it (the
// model says a leg we BUY is cheaper than the market will sell it, and a leg we
// SELL is richer than the market will pay). With that placement the ordering
//
//     NAV(CrossSpread) < NAV(QuoteMid) < NAV(ModelMark)
//
// holds by construction on a round trip. If it does not order, the fill
// accounting is wrong.

namespace {

constexpr std::int64_t kDayNs = 86'400LL * 1'000'000'000LL;
constexpr std::int64_t kF2Expiry = kNow + 60 * kDayNs; // survives every replay date

[[nodiscard]] PricedSurface make_surface_at(std::uint32_t uid, double spot, std::int64_t now_ts) {
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
  pricing.now_ts_ns = now_ts;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = uid;
  auto result = PricedSurface::create(std::move(curves), std::move(context), pricing);
  EXPECT_TRUE(result.has_value()) << result.error().to_string();
  return std::move(*result);
}

[[nodiscard]] ListedOptionQuote f2_option(const std::string &symbol, std::uint32_t id, double strike,
                                          Side side, const std::string &trade_date,
                                          std::int64_t now_ts) {
  ListedOptionQuote q;
  q.trade_date = trade_date;
  q.symbol = symbol;
  q.instrument_id = id;
  q.raw_symbol = symbol + std::to_string(id);
  q.expiry_ts_ns = kF2Expiry;
  q.strike = strike;
  q.side = side;
  q.bid = 1.0; // placeholder: requote_unfavourably() rewrites every leg's NBBO
  q.ask = 1.2;
  q.quote_ts_ns = now_ts;
  q.standard_monthly = true;
  q.standard_deliverable = true;
  q.source_fingerprint = 100u + id;
  return q;
}

[[nodiscard]] ListedStraddle f2_straddle(const std::string &symbol, std::uint32_t uid,
                                         std::uint32_t id, double strike, double weight,
                                         const std::string &trade_date, std::int64_t now_ts) {
  ListedStraddle result;
  result.symbol = symbol;
  result.uid = uid;
  result.expiry_ts_ns = kF2Expiry;
  result.strike = strike;
  result.call = f2_option(symbol, id, strike, Side::Call, trade_date, now_ts);
  result.put = f2_option(symbol, id + 1u, strike, Side::Put, trade_date, now_ts);
  result.raw_weight = weight;
  result.normalized_weight = weight;
  return result;
}

[[nodiscard]] ListedDispersionSelection f2_selection(const std::string &trade_date,
                                                     std::int64_t now_ts) {
  ListedDispersionSelection result;
  result.trade_date = trade_date;
  result.valuation_ts_ns = now_ts;
  result.expiry_ts_ns = kF2Expiry;
  result.dte_days = static_cast<double>(kF2Expiry - now_ts) / kListedNsPerDay;
  result.index = f2_straddle("SPY", 1u, 1u, 500.0, 0.0, trade_date, now_ts);
  result.names.push_back(f2_straddle("N0", 2u, 3u, 100.0, 0.4, trade_date, now_ts));
  result.names.push_back(f2_straddle("N1", 3u, 5u, 200.0, 0.6, trade_date, now_ts));
  return result;
}

// Place each leg's NBBO so the model mark is on the FAVOURABLE side of the mid
// for the direction we trade it: buying (quantity > 0) faces a mid above the
// mark, selling (quantity < 0) faces a mid below it. `half_frac` then opens a
// two-sided market around that mid, so crossing costs strictly more than the
// mid. `raw_mid` is recomputed as the exact half-sum validate_roll demands.
void requote_unfavourably(ListedScheduleRoll &roll, double edge_frac, double half_frac) {
  for (ListedScheduleLeg &leg : roll.legs) {
    const double m = leg.model_mark;
    EXPECT_GT(m, 0.0) << "fixture needs a positive mark on every leg";
    const double sign = (leg.quantity >= 0.0) ? 1.0 : -1.0;
    const double mid = m + sign * edge_frac * m;
    const double half = half_frac * m;
    leg.raw_bid = mid - half;
    leg.raw_ask = mid + half;
    leg.raw_mid = 0.5 * (leg.raw_bid + leg.raw_ask);
    EXPECT_GT(leg.raw_bid, 0.0);
  }
}

struct F2Fixture {
  fs::path dir;
  CorpusManifest manifest;
  ListedDispersionSchedule schedule;
};

[[nodiscard]] F2Fixture make_f2_fixture(const char *tag) {
  F2Fixture out;
  out.dir = fresh_dir(tag);
  const std::string dates[] = {"2026-07-10", "2026-07-11", "2026-07-12"};
  const std::int64_t stamps[] = {kNow, kNow + kDayNs, kNow + 2 * kDayNs};

  for (std::size_t d = 0; d < 3; ++d) {
    const double drift = 1.0 + 0.004 * static_cast<double>(d);
    std::vector<PricedSurface> day;
    day.push_back(make_surface_at(1u, 500.0 * drift, stamps[d]));
    day.push_back(make_surface_at(2u, 100.0 * drift, stamps[d]));
    day.push_back(make_surface_at(3u, 200.0 * drift, stamps[d]));
    const std::string path = (out.dir / (dates[d] + ".atxvsa")).string();
    const std::vector<SurfaceArchiveItem> items = {
        {"SPY", &day[0]}, {"N0", &day[1]}, {"N1", &day[2]}};
    EXPECT_TRUE(write_surface_archive_v2_file(path, items).has_value());
    out.manifest.dates.push_back(dates[d]);
    CorpusEntry e;
    e.date = dates[d];
    e.symbol = "SPY";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    out.manifest.entries.push_back(std::move(e));

    if (d == 2) {
      continue; // two rolls: open on date 0, roll (round trip) on date 1
    }
    auto snapshot = MarketSnapshot::load(path);
    EXPECT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
    ListedScheduleBuildConfig cfg;
    cfg.gross_index_vega_target_per_vol_point = 1000.0;
    cfg.cohort = static_cast<std::uint32_t>(4u + d);
    cfg.surface_fingerprint = 12345u;
    auto roll =
        build_listed_dispersion_roll(f2_selection(dates[d], stamps[d]), snapshot->set(), cfg);
    EXPECT_TRUE(roll.has_value()) << roll.error().to_string();
    requote_unfavourably(*roll, /*edge_frac=*/0.01, /*half_frac=*/0.02);
    out.schedule.rolls.push_back(std::move(*roll));
  }
  return out;
}

} // namespace

TEST(ListedDispersionStrategy, QuoteSideFillsOrderNavCrossSpreadBelowMidBelowModelMark) {
  const F2Fixture fx = make_f2_fixture("f2-fill-policy");
  auto clock = Clock::from_manifest(fx.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const auto run_with = [&](ScheduleFillPolicy fp) -> Result<BacktestResult> {
    auto strategy =
        ListedDispersionStrategy::create(fx.schedule, 0.0, ScheduleMarkPolicy::ExactArchive, fp);
    EXPECT_TRUE(strategy.has_value());
    if (!strategy) {
      return atx::core::Err(strategy.error());
    }
    RunConfig cfg;
    cfg.price.n_threads = 1u;
    cfg.prefetch_snapshots = false;
    // F2 needs the engine to CHARGE the fill/mark gap, and F1(d) proves the
    // charge closes the books: NAV must still equal liquidation on every row.
    cfg.book_entry_fill_slippage = true;
    cfg.reconcile_nav = true;
    return run_backtest(*clock, *strategy, cfg);
  };

  const auto model = run_with(ScheduleFillPolicy::ModelMark);
  ASSERT_TRUE(model.has_value()) << model.error().to_string();
  const auto mid = run_with(ScheduleFillPolicy::QuoteMid);
  ASSERT_TRUE(mid.has_value()) << mid.error().to_string();
  const auto cross = run_with(ScheduleFillPolicy::CrossSpread);
  ASSERT_TRUE(cross.has_value()) << cross.error().to_string();

  ASSERT_EQ(model->size(), 3u);
  ASSERT_EQ(mid->size(), 3u);
  ASSERT_EQ(cross->size(), 3u);

  // THE gate.
  EXPECT_LT(cross->nav.back(), mid->nav.back());
  EXPECT_LT(mid->nav.back(), model->nav.back());
  std::printf("[F2] final NAV  cross=%.6f  mid=%.6f  model=%.6f\n", cross->nav.back(),
              mid->nav.back(), model->nav.back());

  // The ordering is the SLIPPAGE, not noise: every quote-side run pays strictly
  // more realized cost on both trading rows, and the crossing run pays most.
  for (std::size_t i = 0; i < 2u; ++i) {
    EXPECT_GT(mid->cost[i], model->cost[i]) << "row " << i;
    EXPECT_GT(cross->cost[i], mid->cost[i]) << "row " << i;
  }

  // Accounting closure: each run reconciles NAV against an independently
  // recomputed liquidation value on every row (F1(d)). Without the engine's
  // fill-slippage booking these tracks would diverge by exactly the slippage.
  for (const BacktestResult *r : {&*model, &*mid, &*cross}) {
    ASSERT_EQ(r->nav_liquidation.size(), r->nav.size());
    for (std::size_t i = 0; i < r->nav.size(); ++i) {
      EXPECT_NEAR(r->nav_liquidation[i], r->nav[i], 1.0e-9) << "row " << i;
    }
  }

  // ModelMark is the compatibility default.
  auto legacy = ListedDispersionStrategy::create(fx.schedule);
  ASSERT_TRUE(legacy.has_value());
  EXPECT_EQ(legacy->fill_policy(), ScheduleFillPolicy::ModelMark);

  std::error_code ec;
  fs::remove_all(fx.dir, ec);
}

TEST(ListedDispersionStrategy, QuoteSideFillsFailClosedOnALegWithNoUsableTwoSidedQuote) {
  const F2Fixture fx = make_f2_fixture("f2-missing-quote");

  // A zero bid: the mid becomes ask/2, a price nobody trades at, and there is no
  // bid to hit at all. ModelMark is unaffected; both quote-side policies refuse.
  ListedDispersionSchedule broken = fx.schedule;
  ListedScheduleLeg &leg = broken.rolls.front().legs.front();
  leg.raw_bid = 0.0;
  leg.raw_mid = 0.5 * (leg.raw_bid + leg.raw_ask);

  EXPECT_TRUE(ListedDispersionStrategy::create(broken).has_value())
      << "the zero bid must not break the model-mark route";

  for (const ScheduleFillPolicy fp :
       {ScheduleFillPolicy::QuoteMid, ScheduleFillPolicy::CrossSpread}) {
    auto strategy =
        ListedDispersionStrategy::create(broken, 0.0, ScheduleMarkPolicy::ExactArchive, fp);
    ASSERT_FALSE(strategy.has_value()) << "quote-side fill accepted an unquotable leg";
    EXPECT_EQ(strategy.error().code(), ErrorCode::NotFound);
    EXPECT_NE(strategy.error().message().find("two-sided quote"), std::string::npos)
        << strategy.error().message();
  }

  // A crossed book (ask < bid) never reaches the fill layer through the strategy
  // — schedule validation rejects the artifact outright — so assert BOTH: the
  // strategy refuses it, and the fill primitive refuses it on its own terms.
  ListedDispersionSchedule crossed = fx.schedule;
  ListedScheduleLeg &cl = crossed.rolls.front().legs.front();
  const double b = cl.raw_bid;
  cl.raw_bid = cl.raw_ask;
  cl.raw_ask = b;
  cl.raw_mid = 0.5 * (cl.raw_bid + cl.raw_ask);
  EXPECT_FALSE(ListedDispersionStrategy::create(crossed, 0.0, ScheduleMarkPolicy::ExactArchive,
                                                ScheduleFillPolicy::CrossSpread)
                   .has_value());
  const auto crossed_px = listed_leg_fill_price(cl, ScheduleFillPolicy::CrossSpread);
  ASSERT_FALSE(crossed_px.has_value());
  EXPECT_EQ(crossed_px.error().code(), ErrorCode::NotFound);

  // And the direction is right: a long leg lifts the offer, a short hits the bid.
  const ListedScheduleRoll &roll = fx.schedule.rolls.front();
  for (const ListedScheduleLeg &l : roll.legs) {
    auto crossed_px = listed_leg_fill_price(l, ScheduleFillPolicy::CrossSpread);
    ASSERT_TRUE(crossed_px.has_value()) << crossed_px.error().to_string();
    EXPECT_DOUBLE_EQ(*crossed_px, l.quantity >= 0.0 ? l.raw_ask : l.raw_bid);
    auto mid_px = listed_leg_fill_price(l, ScheduleFillPolicy::QuoteMid);
    ASSERT_TRUE(mid_px.has_value());
    EXPECT_DOUBLE_EQ(*mid_px, 0.5 * (l.raw_bid + l.raw_ask));
    // Crossing is never better than the mid, for either direction.
    const double cross_cost = l.quantity * l.multiplier * (*crossed_px - *mid_px);
    EXPECT_GT(cross_cost, 0.0);
  }

  std::error_code ec;
  fs::remove_all(fx.dir, ec);
}

// â”€â”€ WS-F F5 (BT-T2): subset-deserialize for schedule-driven strategies â”€â”€â”€â”€â”€â”€
//
// Subset-deserialize was wired only for the FIXED-BOOK overload's private
// cache; the strategy overload constructed its private cache with no referenced
// set "because on_step names are not known up front". For a schedule-driven
// strategy that is simply false â€” the schedule enumerates every uid it will ever
// touch â€” so a replay against a wide archive reconstructed the WHOLE BOARD on
// every date to price a handful of names.
//
// The fixture below archives the 3 traded names alongside 6 untraded ones, so
// the subset is a real subset, and gates on BOTH halves of the claim:
//   1. the NAV track is BIT-IDENTICAL to the whole-board load (a perf change
//      that moves a number is a correctness bug), and
//   2. the load-bytes counter drops â€” a DETERMINISTIC count of record bytes
//      materialized, not a timing number.

namespace {

// The F2 corpus plus `n_filler` surfaces per date that the schedule never
// references. Same schedule, same rolls: only the archive is wider.
[[nodiscard]] F2Fixture make_wide_fixture(const char *tag, int n_filler) {
  F2Fixture out;
  out.dir = fresh_dir(tag);
  const std::string dates[] = {"2026-07-10", "2026-07-11", "2026-07-12"};
  const std::int64_t stamps[] = {kNow, kNow + kDayNs, kNow + 2 * kDayNs};

  for (std::size_t d = 0; d < 3; ++d) {
    const double drift = 1.0 + 0.004 * static_cast<double>(d);
    std::vector<PricedSurface> day;
    day.push_back(make_surface_at(1u, 500.0 * drift, stamps[d]));
    day.push_back(make_surface_at(2u, 100.0 * drift, stamps[d]));
    day.push_back(make_surface_at(3u, 200.0 * drift, stamps[d]));
    for (int f = 0; f < n_filler; ++f) {
      day.push_back(make_surface_at(static_cast<std::uint32_t>(100 + f),
                                    (60.0 + 7.0 * static_cast<double>(f)) * drift, stamps[d]));
    }
    std::vector<SurfaceArchiveItem> items;
    items.push_back(SurfaceArchiveItem{"SPY", &day[0]});
    items.push_back(SurfaceArchiveItem{"N0", &day[1]});
    items.push_back(SurfaceArchiveItem{"N1", &day[2]});
    std::vector<std::string> filler_names;
    filler_names.reserve(static_cast<std::size_t>(n_filler));
    for (int f = 0; f < n_filler; ++f) {
      filler_names.push_back("FILL" + std::to_string(f));
    }
    for (int f = 0; f < n_filler; ++f) {
      items.push_back(SurfaceArchiveItem{filler_names[static_cast<std::size_t>(f)],
                                         &day[3 + static_cast<std::size_t>(f)]});
    }
    const std::string path = (out.dir / (dates[d] + ".atxvsa")).string();
    EXPECT_TRUE(write_surface_archive_v2_file(path, items).has_value());
    out.manifest.dates.push_back(dates[d]);
    CorpusEntry e;
    e.date = dates[d];
    e.symbol = "SPY";
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    out.manifest.entries.push_back(std::move(e));

    if (d == 2) {
      continue;
    }
    auto snapshot = MarketSnapshot::load(path);
    EXPECT_TRUE(snapshot.has_value()) << snapshot.error().to_string();
    ListedScheduleBuildConfig cfg;
    cfg.gross_index_vega_target_per_vol_point = 1000.0;
    cfg.cohort = static_cast<std::uint32_t>(4u + d);
    cfg.surface_fingerprint = 12345u;
    auto roll =
        build_listed_dispersion_roll(f2_selection(dates[d], stamps[d]), snapshot->set(), cfg);
    EXPECT_TRUE(roll.has_value()) << roll.error().to_string();
    out.schedule.rolls.push_back(std::move(*roll));
  }
  return out;
}

void expect_track_bit_identical(const BacktestResult &a, const BacktestResult &b) {
  ASSERT_EQ(a.size(), b.size());
  const auto same = [](const std::vector<double> &x, const std::vector<double> &y,
                       const char *name) {
    ASSERT_EQ(x.size(), y.size()) << name;
    for (std::size_t i = 0; i < x.size(); ++i) {
      EXPECT_EQ(x[i], y[i]) << name << " row " << i;
    }
  };
  same(a.nav, b.nav, "nav");
  same(a.pnl_total, b.pnl_total, "pnl_total");
  same(a.pnl_delta, b.pnl_delta, "pnl_delta");
  same(a.pnl_vega, b.pnl_vega, "pnl_vega");
  same(a.pnl_settlement, b.pnl_settlement, "pnl_settlement");
  same(a.pnl_shares, b.pnl_shares, "pnl_shares");
  same(a.financing, b.financing, "financing");
  same(a.cost, b.cost, "cost");
  same(a.cash, b.cash, "cash");
  same(a.gross_delta, b.gross_delta, "gross_delta");
  same(a.gross_vega, b.gross_vega, "gross_vega");
  same(a.step_pnl_total, b.step_pnl_total, "step_pnl_total");
}

} // namespace

TEST(ListedDispersionStrategy, ScheduleEnumeratesEveryReferencedUidForSubsetDeserialize) {
  const F2Fixture fx = make_wide_fixture("f5-referenced-uids", /*n_filler=*/6);
  auto strategy = ListedDispersionStrategy::create(fx.schedule);
  ASSERT_TRUE(strategy.has_value()) << strategy.error().to_string();

  const std::span<const std::uint32_t> uids = strategy->referenced_uids();
  ASSERT_EQ(uids.size(), 3u) << "SPY + 2 names, deduped across both rolls";
  EXPECT_EQ(uids[0], 1u);
  EXPECT_EQ(uids[1], 2u);
  EXPECT_EQ(uids[2], 3u);
  EXPECT_TRUE(std::is_sorted(uids.begin(), uids.end()));

  // The set covers every leg of every roll â€” an omission is a silent wrong
  // number (the name would be absent from every snapshot), not a slow run.
  for (const ListedScheduleRoll &roll : fx.schedule.rolls) {
    for (const ListedScheduleLeg &leg : roll.legs) {
      EXPECT_NE(std::find(uids.begin(), uids.end(), leg.uid), uids.end()) << leg.symbol;
    }
  }
  std::error_code ec;
  fs::remove_all(fx.dir, ec);
}

TEST(ListedDispersionStrategy, SubsetDeserializeIsNavIdenticalAndLoadsStrictlyFewerBytes) {
  const F2Fixture fx = make_wide_fixture("f5-subset-bytes", /*n_filler=*/6);
  auto clock = Clock::from_manifest(fx.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // `whole_board` forces the pre-F5 path: a CALLER-SUPPLIED cache is never
  // subsetted (it may be reused across books with different referenced sets),
  // which is exactly the load the strategy overload used to perform.
  const auto run = [&](bool whole_board) -> std::pair<Result<BacktestResult>, std::uint64_t> {
    auto strategy = ListedDispersionStrategy::create(fx.schedule);
    EXPECT_TRUE(strategy.has_value());
    RunConfig cfg;
    cfg.price.n_threads = 1u;
    cfg.prefetch_snapshots = false; // keep the counter attributable to this run
    if (whole_board) {
      cfg.snapshot_cache = std::make_shared<SnapshotCache>();
    }
    MarketSnapshot::reset_deserialized_bytes();
    auto result = run_backtest(*clock, *strategy, cfg);
    return {std::move(result), MarketSnapshot::deserialized_bytes()};
  };

  auto [subset, subset_bytes] = run(false);
  ASSERT_TRUE(subset.has_value()) << subset.error().to_string();
  auto [board, board_bytes] = run(true);
  ASSERT_TRUE(board.has_value()) << board.error().to_string();

  // Gate 1: the perf change moves NO number.
  expect_track_bit_identical(*subset, *board);

  // Gate 2: a deterministic, non-timing drop. 3 of 9 surfaces are referenced, so
  // the subset must load well under half the record bytes.
  EXPECT_GT(board_bytes, 0u);
  EXPECT_LT(subset_bytes, board_bytes);
  EXPECT_LT(static_cast<double>(subset_bytes), 0.5 * static_cast<double>(board_bytes));
  std::printf("[F5] record bytes  subset=%llu  whole-board=%llu  (%.1f%%)\n",
              static_cast<unsigned long long>(subset_bytes),
              static_cast<unsigned long long>(board_bytes),
              100.0 * static_cast<double>(subset_bytes) / static_cast<double>(board_bytes));

  std::error_code ec;
  fs::remove_all(fx.dir, ec);
}

TEST(ListedDispersionStrategy, AStrategyThatCannotEnumerateItsNamesStillLoadsTheWholeBoard) {
  // The default `referenced_uids()` is empty and MUST keep the whole-board load:
  // a strategy that discovers names inside on_step (a point-in-time universe, a
  // signal-driven basket) would otherwise silently lose them.
  struct NoHook final : IStrategy {
    Status on_step(const MarketSnapshot &, std::size_t, PortfolioState &,
                   std::uint64_t &) override {
      return atx::core::Ok();
    }
  } strategy;
  EXPECT_TRUE(strategy.referenced_uids().empty());

  const F2Fixture fx = make_wide_fixture("f5-no-hook", /*n_filler=*/6);
  auto clock = Clock::from_manifest(fx.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  RunConfig cfg;
  cfg.price.n_threads = 1u;
  cfg.prefetch_snapshots = false;
  MarketSnapshot::reset_deserialized_bytes();
  const auto result = run_backtest(*clock, strategy, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const std::uint64_t bytes = MarketSnapshot::deserialized_bytes();

  auto listed = ListedDispersionStrategy::create(fx.schedule);
  ASSERT_TRUE(listed.has_value());
  RunConfig subset_cfg;
  subset_cfg.price.n_threads = 1u;
  subset_cfg.prefetch_snapshots = false;
  MarketSnapshot::reset_deserialized_bytes();
  const auto listed_result = run_backtest(*clock, *listed, subset_cfg);
  ASSERT_TRUE(listed_result.has_value()) << listed_result.error().to_string();
  EXPECT_LT(MarketSnapshot::deserialized_bytes(), bytes)
      << "the hookless strategy must still pay for the whole board";

  std::error_code ec;
  fs::remove_all(fx.dir, ec);
}

// F5 review follow-up. The engine never subsets a caller-SUPPLIED cache — it
// cannot know what else the caller will serve from it — so any driver that
// supplies its own cache silently opted out of F5. `dispersion_run_backtest`
// does exactly that (it shares one cache between the replay and the
// reconciliation pass), which left F5 inert on the listed `run-backtest`: the
// very path whose premise motivated the task.
//
// The fix is at the CALL SITE, not in the engine: a caller that knows its
// referenced set constructs the cache with it. This pins that mechanism — the
// same construction `dispersion_run_backtest` now performs — end to end.
TEST(ListedDispersionStrategy, ASuppliedCacheSubsetsOnlyWhenTheCallerNamesTheUids) {
  const F2Fixture fx = make_wide_fixture("f5-supplied-cache", /*n_filler=*/6);
  auto clock = Clock::from_manifest(fx.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  enum class Cache { WholeBoardSupplied, SubsetSupplied };
  const auto run = [&](Cache mode) -> std::pair<Result<BacktestResult>, std::uint64_t> {
    auto strategy = ListedDispersionStrategy::create(fx.schedule);
    EXPECT_TRUE(strategy.has_value());
    RunConfig cfg;
    cfg.price.n_threads = 1u;
    cfg.prefetch_snapshots = false;
    if (mode == Cache::WholeBoardSupplied) {
      cfg.snapshot_cache = std::make_shared<SnapshotCache>();
    } else {
      // Verbatim the construction dispersion_run_backtest performs.
      const std::span<const std::uint32_t> uids = strategy->referenced_uids();
      cfg.snapshot_cache = std::make_shared<SnapshotCache>(
          clock->size() > 0u ? clock->size() : 1u,
          std::vector<std::uint32_t>(uids.begin(), uids.end()));
    }
    MarketSnapshot::reset_deserialized_bytes();
    auto result = run_backtest(*clock, *strategy, cfg);
    return {std::move(result), MarketSnapshot::deserialized_bytes()};
  };

  auto [board, board_bytes] = run(Cache::WholeBoardSupplied);
  ASSERT_TRUE(board.has_value()) << board.error().to_string();
  auto [subset, subset_bytes] = run(Cache::SubsetSupplied);
  ASSERT_TRUE(subset.has_value()) << subset.error().to_string();

  expect_track_bit_identical(*subset, *board);
  EXPECT_LT(subset_bytes, board_bytes);
  EXPECT_LT(static_cast<double>(subset_bytes), 0.5 * static_cast<double>(board_bytes));
  std::printf("[F5 supplied cache] record bytes  subset=%llu  whole-board=%llu  (%.1f%%)\n",
              static_cast<unsigned long long>(subset_bytes),
              static_cast<unsigned long long>(board_bytes),
              100.0 * static_cast<double>(subset_bytes) / static_cast<double>(board_bytes));

  // A supplied cache with a LARGE capacity must not evict across the run: the
  // reconciliation pass that shares this cache holds every date alive at once,
  // and an evicting cache would silently re-load (and re-count) each date.
  // One archive open per date is the invariant.
  MarketSnapshot::reset_open_count();
  auto strategy = ListedDispersionStrategy::create(fx.schedule);
  ASSERT_TRUE(strategy.has_value());
  const std::span<const std::uint32_t> uids = strategy->referenced_uids();
  RunConfig cfg;
  cfg.price.n_threads = 1u;
  cfg.prefetch_snapshots = false;
  cfg.snapshot_cache = std::make_shared<SnapshotCache>(
      clock->size(), std::vector<std::uint32_t>(uids.begin(), uids.end()));
  const auto once = run_backtest(*clock, *strategy, cfg);
  ASSERT_TRUE(once.has_value()) << once.error().to_string();
  EXPECT_EQ(MarketSnapshot::open_count(), static_cast<std::uint64_t>(clock->size()));

  std::error_code ec;
  fs::remove_all(fx.dir, ec);
}
