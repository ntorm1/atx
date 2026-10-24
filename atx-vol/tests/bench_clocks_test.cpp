// Lane B0 benchmark clock contract (tools/bench_clocks.hpp, docs/benchmark-clocks.md).
//
// Pins: (1) the clock summary is the linear order-statistic rule; (2) PhaseClock laps
// charge the span between marks, a split lap charges only its measured parts (the rest
// is unclaimed, never a stage) and never charges an over-claim (it is reported as a
// conflict instead); (3) fit_parts carves the family build and the de-Am endpoint /
// exception time out of the deam bucket without going negative; (4) the stage table
// reconciles against the cold clock only when the stages measured it: an untimed region
// shows as a nonzero unattributed_s and fails the 1% check, and the family stage is
// not_built, never zero, when no family was built; (5) the de-Am stage clock times band
// edges without changing the fit, and its scope guard disarms on an exception; (6) the
// in-process cold reset empties the thread memo so a second fit repeats the first one's
// work exactly, and is not faster than a stated bound.

#include "bench_clocks.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include "fitting/counters.hpp"
#include "fitting/deam_iv_cache.hpp"

namespace atx::vol {
namespace {

using bench::BenchClock;
using bench::Stage;
using std::chrono::microseconds;
using std::chrono::milliseconds;

constexpr double kTight = 1.0e-12;

// A cold rep must take at least this share of the first rep's wall: a warm memo hit
// skips every inversion and runs far faster (documented in docs/benchmark-clocks.md).
constexpr double kColdRepMinRatio = 0.5;

TEST(BenchClocks, SummaryUsesTheLinearOrderStatisticRule) {
  const std::array<double, 5> values = {5.0, 1.0, 3.0, 2.0, 4.0};
  const bench::ClockSummary s = bench::summarize(values);
  EXPECT_EQ(s.n, 5u);
  EXPECT_DOUBLE_EQ(s.min, 1.0);
  EXPECT_DOUBLE_EQ(s.max, 5.0);
  EXPECT_DOUBLE_EQ(s.median, 3.0);
  EXPECT_NEAR(s.p95, 4.8, kTight); // position 0.95 * 4 = 3.8 between 4 and 5
  EXPECT_NEAR(s.p99, 4.96, kTight);
  const std::array<double, 1> one = {2.5};
  const bench::ClockSummary single = bench::summarize(one);
  EXPECT_EQ(single.n, 1u);
  EXPECT_DOUBLE_EQ(single.p99, 2.5);
  const bench::ClockSummary empty = bench::summarize(std::span<const double>{});
  EXPECT_EQ(empty.n, 0u);
  EXPECT_TRUE(std::isnan(empty.median));
  EXPECT_TRUE(bench::summary_json(empty)["median"].is_null());
}

TEST(BenchPhaseClock, UnarmedClockRecordsNothing) {
  bench::PhaseClock clock;
  const BenchClock::time_point t0{};
  clock.start(t0);
  clock.lap(Stage::Normalization, t0 + milliseconds(4));
  EXPECT_FALSE(clock.started());
  EXPECT_EQ(clock.total(), 0.0);
}

TEST(BenchPhaseClock, LapsPartitionTheSpanExactly) {
  bench::PhaseClock clock;
  clock.arm();
  const BenchClock::time_point t0{};
  clock.start(t0);
  clock.lap(Stage::Normalization, t0 + milliseconds(3));
  clock.lap(Stage::Publication, t0 + milliseconds(5));
  clock.lap(Stage::Normalization, t0 + milliseconds(6));
  EXPECT_NEAR(clock.seconds(Stage::Normalization), 0.004, kTight);
  EXPECT_NEAR(clock.seconds(Stage::Publication), 0.002, kTight);
  EXPECT_NEAR(clock.total(), 0.006, kTight);
  EXPECT_EQ(clock.conflict_seconds(), 0.0);
  // A mark that runs backwards charges nothing (steady_clock never does; the guard is
  // the defined behaviour for a misuse).
  clock.lap(Stage::Publication, t0 + milliseconds(1));
  EXPECT_NEAR(clock.total(), 0.006, kTight);
}

TEST(BenchPhaseClock, SplitLapChargesOnlyMeasuredPartsAndReportsOverclaims) {
  bench::PhaseClock clock;
  clock.arm();
  const BenchClock::time_point t0{};
  clock.start(t0);
  const std::array<bench::StagePart, 2> parts = {
      bench::StagePart{Stage::Inverses, 0.004}, bench::StagePart{Stage::CurveRepair, 0.003}};
  clock.lap_split(t0 + milliseconds(10), parts);
  EXPECT_NEAR(clock.seconds(Stage::Inverses), 0.004, kTight);
  EXPECT_NEAR(clock.seconds(Stage::CurveRepair), 0.003, kTight);
  // The 3 ms no part measured is charged to no stage.
  EXPECT_NEAR(clock.total(), 0.007, kTight);
  EXPECT_NEAR(clock.unclaimed_seconds(), 0.003, kTight);
  EXPECT_EQ(clock.conflict_seconds(), 0.0);
  // Parts claiming more than the lap held (overlapping fit workers): the lap is charged
  // at most once and the excess is a conflict, never time.
  const std::array<bench::StagePart, 2> over = {
      bench::StagePart{Stage::Inverses, 0.008}, bench::StagePart{Stage::FinalGrid, 0.005}};
  clock.lap_split(t0 + milliseconds(20), over);
  EXPECT_NEAR(clock.seconds(Stage::Inverses), 0.012, kTight);
  EXPECT_NEAR(clock.seconds(Stage::FinalGrid), 0.002, kTight);
  EXPECT_NEAR(clock.total(), 0.017, kTight);
  EXPECT_NEAR(clock.unclaimed_seconds(), 0.003, kTight);
  EXPECT_NEAR(clock.conflict_seconds(), 0.003, kTight);
  clock.add_conflict(0.001);
  EXPECT_NEAR(clock.conflict_seconds(), 0.004, kTight);
  // A skipped region moves the mark and is unclaimed time.
  clock.skip(t0 + milliseconds(25));
  clock.lap(Stage::Publication, t0 + milliseconds(26));
  EXPECT_NEAR(clock.unclaimed_seconds(), 0.008, kTight);
  EXPECT_NEAR(clock.seconds(Stage::Publication), 0.001, kTight);
}

bench::FitWork buckets(std::initializer_list<std::uint64_t> us) {
  bench::FitWork work;
  std::copy(us.begin(), us.end(), work.bucket_us.begin());
  return work;
}

TEST(BenchFitParts, CarvesFamilyEndpointsAndExceptionsOutOfTheDeamBucket) {
  // fit_timing order: other, deam, solve, project, price, certify, calendar (us).
  bench::FitWork work = buckets({100, 1000, 200, 50, 300, 20, 30});
  double excess = -1.0;
  const auto parts = bench::fit_parts(work, 250'000, 100'000, excess);
  EXPECT_EQ(excess, 0.0);
  const auto seconds_of = [&parts](Stage stage) {
    double sum = 0.0;
    for (const bench::StagePart &p : parts) sum += p.stage == stage ? p.seconds : 0.0;
    return sum;
  };
  EXPECT_NEAR(seconds_of(Stage::Endpoints), 250e-6, kTight);
  EXPECT_NEAR(seconds_of(Stage::ReferenceExceptions), 100e-6, kTight);
  EXPECT_NEAR(seconds_of(Stage::Inverses), 650e-6, kTight);
  EXPECT_NEAR(seconds_of(Stage::CurveRepair), 250e-6, kTight);
  EXPECT_NEAR(seconds_of(Stage::FinalGrid), 350e-6, kTight);
  EXPECT_NEAR(seconds_of(Stage::FitOther), 100e-6, kTight);
  double total = 0.0;
  for (const bench::StagePart &p : parts) total += p.seconds;
  EXPECT_NEAR(total, 1700e-6, kTight); // the seven buckets, nothing added or lost
  EXPECT_EQ(seconds_of(Stage::FamilyBuildValidation), 0.0);
  // Sub-spans above the bucket (rounding, or a wrong mapping) are clamped, not charged.
  const auto clamped = bench::fit_parts(work, 900'000, 300'000, excess);
  EXPECT_NEAR(excess, 200e-6, kTight);
  EXPECT_NEAR(clamped[1].seconds + clamped[2].seconds + clamped[3].seconds, 1000e-6, kTight);
  EXPECT_EQ(clamped[3].stage, Stage::Inverses);
  EXPECT_EQ(clamped[3].seconds, 0.0);
  // A family build (inside the deam bucket) is carved out first.
  work.deam_family_build_us = 400;
  const auto family = bench::fit_parts(work, 250'000, 100'000, excess);
  EXPECT_EQ(excess, 0.0);
  EXPECT_EQ(family[0].stage, Stage::FamilyBuildValidation);
  EXPECT_NEAR(family[0].seconds, 400e-6, kTight);
  EXPECT_NEAR(family[3].seconds, 250e-6, kTight); // inverses: 1000 - 400 - 250 - 100
}

TEST(BenchFitWork, AddAndDeltaAreComponentWise) {
  counters::fit_timing::FitTimingCounters a;
  a.wall_us = 10;
  a.deam_us = 7;
  a.other_us = 3;
  a.al_price_calls = 5;
  a.al_by_bucket[1] = 5;
  a.deam_inversions = 4;
  a.deam_premium_fallback_by_reason[2] = 1;
  counters::fit_timing::FitTimingCounters b = a;
  b.wall_us = 20;
  b.deam_us = 15;
  b.other_us = 5;
  bench::FitWork work;
  bench::add_fit_work(work, a);
  const bench::FitWork first = work;
  bench::add_fit_work(work, b);
  EXPECT_EQ(work.chains, 2u);
  EXPECT_EQ(work.wall_us, 30u);
  EXPECT_EQ(work.bucket_us[1], 22u);
  EXPECT_EQ(work.al_by_bucket[1], 10u);
  EXPECT_EQ(work.deam_inversions, 8u);
  EXPECT_EQ(work.deam_fallback_by_reason[2], 2u);
  const bench::FitWork second = bench::fit_work_delta(work, first);
  EXPECT_EQ(second.chains, 1u);
  EXPECT_EQ(second.wall_us, 20u);
  EXPECT_EQ(second.bucket_us[0], 5u);
  EXPECT_EQ(bench::fit_work_delta(first, work).wall_us, 0u); // saturating, never wraps
}

TEST(BenchStages, TableReconcilesWithTheColdClockAndMarksTheFamilyNotBuilt) {
  bench::PhaseClock clock;
  clock.arm();
  const BenchClock::time_point t0{};
  clock.start(t0);
  clock.lap(Stage::Normalization, t0 + milliseconds(2));
  clock.lap(Stage::Inverses, t0 + milliseconds(10));
  const bench::Json ok = bench::stages_json(clock, 0.0101, false);
  EXPECT_TRUE(ok["stages"]["family_build_validation"]["seconds"].is_null());
  EXPECT_EQ(ok["stages"]["family_build_validation"]["status"], "not_built");
  EXPECT_EQ(ok["stages"]["inverses"]["status"], "timed");
  EXPECT_NEAR(ok["sum_s"].get<double>(), 0.010, kTight);
  EXPECT_NEAR(ok["unattributed_s"].get<double>(), 0.0001, kTight);
  EXPECT_TRUE(ok["reconciles_within_1pct"].get<bool>());
  const bench::Json off = bench::stages_json(clock, 0.0102, true);
  EXPECT_FALSE(off["reconciles_within_1pct"].get<bool>()); // a 2% residual is reported
  EXPECT_EQ(off["stages"]["family_build_validation"]["status"], "timed");
  EXPECT_EQ(off["stages"].size(), bench::kStageCount);
}

// Lane H1: the serve_build stage (serving records built after the fit clock closes) is
// not_built, never zero, unless records were built; built, it is one more exclusive stage
// of the same table, so the fit clock is the stage sum less it.
TEST(BenchStages, TheServeBuildStageIsNotBuiltUnlessRecordsWereBuilt) {
  bench::PhaseClock clock;
  clock.arm();
  const BenchClock::time_point t0{};
  clock.start(t0);
  clock.lap(Stage::Inverses, t0 + milliseconds(10));
  clock.lap(Stage::ServeBuild, t0 + milliseconds(14));
  const bench::Json off = bench::stages_json(clock, 0.014, true);
  EXPECT_TRUE(off["stages"]["serve_build"]["seconds"].is_null());
  EXPECT_EQ(off["stages"]["serve_build"]["status"], "not_built");
  const bench::Json on = bench::stages_json(clock, 0.014, true, true);
  EXPECT_EQ(on["stages"]["serve_build"]["status"], "timed");
  EXPECT_NEAR(on["stages"]["serve_build"]["seconds"].get<double>(), 0.004, kTight);
  EXPECT_NEAR(on["fit_clock_s"].get<double>(), 0.010, kTight);
  EXPECT_NEAR(on["sum_s"].get<double>(), 0.014, kTight);
  EXPECT_EQ(on["stages"].size(), bench::kStageCount);
}

// The reconciliation is a test, not an identity: a fit lap whose chains measured only
// part of it (a deliberately untimed region inside the lap, e.g. an uninstrumented
// family build or surface scheduling) leaves the cold clock - stage sum nonzero, and a
// region over 1% of the cold clock fails reconciles_within_1pct.
TEST(BenchStages, UntimedRegionIsUnattributedAndFailsTheOnePercentCheck) {
  const BenchClock::time_point t0{};
  const auto run = [&t0](microseconds untimed) {
    bench::PhaseClock clock;
    clock.arm();
    clock.start(t0);
    clock.lap(Stage::Normalization, t0 + milliseconds(10));
    // A 100 ms fit lap plus the untimed region; the chains measured 100 ms of it.
    const std::array<bench::StagePart, 2> measured = {
        bench::StagePart{Stage::Inverses, 0.060}, bench::StagePart{Stage::FinalGrid, 0.040}};
    clock.lap_split(t0 + milliseconds(110) + untimed, measured);
    clock.lap(Stage::Publication, t0 + milliseconds(120) + untimed);
    const double cold = bench::seconds_between(t0, t0 + milliseconds(120) + untimed);
    return bench::stages_json(clock, cold, false);
  };
  const bench::Json big = run(microseconds(5'000)); // 5 ms of 125 ms = 4%
  EXPECT_NEAR(big["sum_s"].get<double>(), 0.120, kTight);
  EXPECT_NEAR(big["unattributed_s"].get<double>(), 0.005, kTight);
  EXPECT_NEAR(big["unclaimed_lap_s"].get<double>(), 0.005, kTight);
  EXPECT_NEAR(big["span_check_s"].get<double>(), 0.0, kTight);
  EXPECT_FALSE(big["reconciles_within_1pct"].get<bool>());
  const bench::Json small = run(microseconds(600)); // 0.6 ms of 120.6 ms = 0.5%
  EXPECT_NEAR(small["unattributed_s"].get<double>(), 0.0006, kTight);
  EXPECT_GT(small["unattributed_s"].get<double>(), 0.0); // nonzero, yet within 1%
  EXPECT_TRUE(small["reconciles_within_1pct"].get<bool>());
  const bench::Json none = run(microseconds(0)); // every interval measured
  EXPECT_NEAR(none["unattributed_s"].get<double>(), 0.0, kTight);
  EXPECT_TRUE(none["reconciles_within_1pct"].get<bool>());
}

// The IV comparator's population rule (bench_clocks.hpp classify_iv_row): resolved rows
// pass or fail at the threshold; an unresolved row whose error clears the threshold by
// more than 3 uncertainties is a decidable fail; the rest stay unresolved.
TEST(BenchIvPopulation, RuleSeparatesResolvedDecidableAndUnresolvedRows) {
  using bench::IvPopulation;
  constexpr double kThreshold = 1.0e-7;
  const auto at = [](double error, double uncertainty) {
    return bench::classify_iv_row(error, uncertainty, kThreshold);
  };
  EXPECT_EQ(at(7.1e-13, 2.0e-9), IvPopulation::ResolvedPass);
  EXPECT_EQ(at(2.0e-7, 5.0e-8), IvPopulation::ResolvedFail);
  EXPECT_EQ(at(1.0e-7, 1.0e-7), IvPopulation::ResolvedPass); // both bounds inclusive
  // cohort5 AAPL call K=10: error 6.839e-4 against an uncertainty of 2.612e-5.
  EXPECT_EQ(at(6.839e-4, 2.612e-5), IvPopulation::DecidableFail);
  // cohort5 RIVN put K=37: error 7.617e-5 inside 3 x its uncertainty 7.928e-5.
  EXPECT_EQ(at(7.617e-5, 7.928e-5), IvPopulation::ReferenceUnresolved);
  EXPECT_EQ(at(5.0e-8, 1.0e-6), IvPopulation::ReferenceUnresolved); // cannot be a pass
  // The margin is strict: error == threshold + 3 u stays unresolved.
  EXPECT_EQ(at(kThreshold + 3.0 * 1.0e-6, 1.0e-6), IvPopulation::ReferenceUnresolved);
  EXPECT_EQ(at(kThreshold + 3.0 * 1.0e-6 + 1.0e-9, 1.0e-6), IvPopulation::DecidableFail);
  // No reference vega (undefined uncertainty) is never decided.
  EXPECT_EQ(at(3.0e-2, std::numeric_limits<double>::quiet_NaN()),
            IvPopulation::ReferenceUnresolved);
  EXPECT_STREQ(bench::iv_population_name(IvPopulation::DecidableFail), "decidable_fail");
  EXPECT_EQ(bench::kDecidableFailMargin, 3.0);
}

struct Chain {
  QuoteSliceMarket market;
  std::vector<QuoteSliceQuote> quotes;
};

// An SPY-shaped chain (the quote_slice_vol_space_test fixture): skewed smile priced by
// the Andersen-Lake engine the fitter inverts, bidless far wings.
Chain spy_chain(double years) {
  constexpr double kSpot = 670.0;
  constexpr double kRate = 0.04;
  constexpr double kYield = 0.012;
  Chain out;
  out.market = QuoteSliceMarket{kSpot, years, years, kRate, kYield, 0.0};
  for (int step = -10; step <= 10; ++step) {
    const double strike = std::round(kSpot * (1.0 + 0.025 * step));
    const double k = std::log(strike / kSpot);
    const double sigma = std::max(0.08, 0.16 - 0.30 * k + 0.80 * k * k);
    for (Side side : {Side::Call, Side::Put}) {
      const auto price = andersen_lake(kSpot, strike, years, sigma, kRate, kYield, side);
      if (!price) {
        ADD_FAILURE() << price.error().to_string();
        return out;
      }
      const double half = std::max(0.02, 0.03 * *price);
      const bool otm = (side == Side::Call) == (k > 0.0);
      const double bid = otm && std::abs(k) > 0.2 ? 0.0 : std::max(*price - half, 0.0);
      out.quotes.push_back(QuoteSliceQuote{strike, side, bid, *price + half});
    }
  }
  return out;
}

QuoteSliceConfig cached_vol_config() {
  QuoteSliceConfig config;
  config.dividend_model = QuoteSliceDividendModel::ContinuousYield;
  config.channel.repair_half_spreads = 0.5;
  config.channel.fit_space = QuoteSliceFitSpace::VolChannel;
  config.channel.deam_profile = QuoteSliceDeamProfile::Accurate;
  config.channel.deam_cache = true;
  return config;
}

struct Rep {
  QuoteSliceFit fit;
  std::uint64_t al_solves{0};
  double seconds{0.0};
};

Rep fit_once(const Chain &chain, const QuoteSliceConfig &config) {
  namespace ledger = counters::ledger;
  const std::uint64_t al0 = ledger::this_thread(ledger::Solve::AlBoundarySolves);
  const auto start = BenchClock::now();
  auto fit = fit_quote_slice_channel(chain.market, chain.quotes, config, nullptr);
  const double seconds = bench::seconds_between(start, BenchClock::now());
  EXPECT_TRUE(fit) << (fit ? "" : fit.error().to_string());
  Rep rep{fit ? std::move(*fit) : QuoteSliceFit{}, 0, seconds};
  rep.al_solves = ledger::this_thread(ledger::Solve::AlBoundarySolves) - al0;
  return rep;
}

bool same_bits(double a, double b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

void expect_same_rows(const QuoteSliceFit &a, const QuoteSliceFit &b) {
  EXPECT_EQ(a.fit_status, b.fit_status);
  ASSERT_EQ(a.rows.size(), b.rows.size());
  for (std::size_t i = 0; i < a.rows.size(); ++i) {
    EXPECT_EQ(a.rows[i].evaluation_status, b.rows[i].evaluation_status) << i;
    EXPECT_TRUE(same_bits(a.rows[i].fitted_price, b.rows[i].fitted_price)) << i;
    EXPECT_TRUE(same_bits(a.rows[i].fitted_iv, b.rows[i].fitted_iv)) << i;
  }
}

TEST(BenchDeamStageClock, ArmedClockTimesBandEdgesWithoutChangingTheFit) {
  const Chain chain = spy_chain(0.15);
  const QuoteSliceConfig config = cached_vol_config();
  deam_iv_memo_clear();
  const Rep plain = fit_once(chain, config);
  deam_iv_memo_clear();
  DeAmStageClock clock;
  ASSERT_EQ(deam_stage_clock(), nullptr);
  const Rep timed = [&] {
    const bench::StageClockArm armed(&clock);
    EXPECT_EQ(deam_stage_clock(), &clock);
    return fit_once(chain, config);
  }();
  EXPECT_EQ(deam_stage_clock(), nullptr); // the guard restored the previous (none)
  // An exception leaves no clock armed: the guard restores on unwind.
  try {
    const bench::StageClockArm armed(&clock);
    throw std::runtime_error("unwind");
  } catch (const std::runtime_error &) {
  }
  EXPECT_EQ(deam_stage_clock(), nullptr);
  expect_same_rows(plain.fit, timed.fit);
  EXPECT_EQ(plain.al_solves, timed.al_solves);
  // Every valid quote's band is filled once: its edges were priced inside the clock.
  EXPECT_GT(clock.endpoint_bands, 0u);
  EXPECT_LE(clock.endpoint_bands, chain.quotes.size());
  EXPECT_GT(clock.endpoint_ns, 0u);
  EXPECT_LT(1.0e-9 * static_cast<double>(clock.endpoint_ns), timed.seconds);
  EXPECT_GE(timed.fit.diagnostics.deam.model_prices, 2u * clock.endpoint_bands);
  deam_iv_memo_clear();
}

TEST(BenchColdReset, ResetEmptiesTheThreadMemoSoTheSecondRepRepeatsTheFirst) {
  const QuoteSliceConfig config = cached_vol_config();
  // First-use work (static tables, the thread's ALO slot) is paid on another chain, so
  // rep 1 below is not inflated by one-time process effects.
  deam_iv_memo_clear();
  static_cast<void>(fit_once(spy_chain(0.40), config));
  const Chain chain = spy_chain(0.15);
  static_cast<void>(bench::cold_reset());

  // Warm control: without a reset the second fit hits the thread memo.
  const Rep warm1 = fit_once(chain, config);
  EXPECT_EQ(bench::deam_memo_entries(), 1u);
  const Rep warm2 = fit_once(chain, config);
  EXPECT_FALSE(warm1.fit.diagnostics.deam.reused);
  EXPECT_TRUE(warm2.fit.diagnostics.deam.reused);
  EXPECT_EQ(warm2.fit.diagnostics.deam.inversions, 0u);
  EXPECT_LT(warm2.al_solves, warm1.al_solves);
  expect_same_rows(warm1.fit, warm2.fit);

  // Cold: reset before each rep. The memo is empty at the start of rep 2 (the receipt
  // and the probe agree) and rep 2 repeats rep 1's work exactly.
  const bench::Json receipt1 = bench::cold_reset();
  EXPECT_EQ(receipt1["cleared"][0]["entries_before"], 1u);
  EXPECT_EQ(receipt1["cleared"][0]["entries_after"], 0u);
  const Rep cold1 = fit_once(chain, config);
  const bench::Json receipt2 = bench::cold_reset();
  EXPECT_EQ(receipt2["cleared"][0]["entries_before"], 1u);
  EXPECT_EQ(bench::deam_memo_entries(), 0u);
  const Rep cold2 = fit_once(chain, config);
  EXPECT_FALSE(cold1.fit.diagnostics.deam.reused);
  EXPECT_FALSE(cold2.fit.diagnostics.deam.reused);
  EXPECT_GT(cold1.fit.diagnostics.deam.inversions, 0u);
  EXPECT_EQ(cold2.fit.diagnostics.deam.inversions, cold1.fit.diagnostics.deam.inversions);
  EXPECT_EQ(cold2.fit.diagnostics.deam.model_prices, cold1.fit.diagnostics.deam.model_prices);
  EXPECT_EQ(cold2.al_solves, cold1.al_solves);
  EXPECT_EQ(cold1.al_solves, warm1.al_solves);
  expect_same_rows(cold1.fit, cold2.fit);
  expect_same_rows(warm1.fit, cold2.fit);
  // Timing (secondary to the exact work equality above): a cold rep 2 is not faster
  // than kColdRepMinRatio of rep 1.
  ASSERT_GT(cold1.seconds, 0.0);
  EXPECT_GE(cold2.seconds / cold1.seconds, kColdRepMinRatio)
      << "rep1 " << cold1.seconds << " s rep2 " << cold2.seconds << " s";
  std::printf("[BenchColdReset] rep1 %.6f s rep2 %.6f s ratio %.3f; warm rep2 %.6f s; "
              "AL solves cold %llu warm2 %llu\n",
              cold1.seconds, cold2.seconds, cold2.seconds / cold1.seconds, warm2.seconds,
              static_cast<unsigned long long>(cold2.al_solves),
              static_cast<unsigned long long>(warm2.al_solves));
  deam_iv_memo_clear();
}

} // namespace
} // namespace atx::vol
