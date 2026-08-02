// dispersion_ladder_bench.cpp — Google Benchmark for the OVERLAPPING-CLIP
// ("ladder") dispersion shape: a fresh dispersion clip entered EVERY step on a
// 3-month tenor and HELD TO ITS OWN EXPIRY, so the priced book ladders up by one
// cohort per step instead of staying at one clip.
//
// WHAT THIS MEASURES THAT THE EXISTING BACKTEST BENCHES DO NOT.
// `backtest_throughput_bench.cpp` holds the book SIZE roughly fixed and asks how
// fast steps go. This asks the opposite question: how the per-step cost grows as the
// open book grows, because that is the shape a production dispersion ladder actually
// runs. Under RollAtHorizon the book is ~2*(n_names+1) lots no matter how long the
// run is; here it is that clip size times the number of live cohorts, so by the last
// step the engine is repricing an order of magnitude more contracts than the rolling
// shape ever does, and the delta hedge reprices all of them every day.
//
// IT DRIVES THE LIBRARY PATH, DELIBERATELY. The strategy comes from
// `make_dispersion_backtest_strategy` (via `make_dispersion_ladder_config`), so the
// clip is sized by the authoritative `build_dispersion_book` — vega-neutral, short
// index / long names, weights renormalized over survivors. A hand-rolled
// `StrategySpec` of N straddles (what backtest_throughput_bench's universe case
// uses) would price a similar NUMBER of contracts but would not exercise the
// dispersion sizing at all, so it could not catch a regression in the thing this
// benchmark is named after. Before the lifecycle became a `DispersionBacktestConfig`
// field this composition was not expressible: the library hardcoded
// EveryNDays/RollAtHorizon.
//
// TENOR vs THE SYNTHETIC SLICE GRID (why the sweep stops where it does).
// synth_corpus's surfaces carry slices T in [0.05, 1.00]. A clip entered at T=0.25
// ages by one CALENDAR day per step (kCorpusDayNs), so after n steps the oldest live
// cohort sits at 0.25 - n/365.25. Past n≈73 that leaves the grid and the benchmark
// would be timing extrapolation off the short end rather than the intended path, so
// the deepest ladder measured here is kLadderDates=60 (oldest cohort T≈0.089, still
// inside the grid). Measuring roll-off at expiry needs a corpus whose grid reaches
// T≈0 — a different fixture, not a deeper sweep of this one.

#include <cstdint>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/backtest.hpp"            // Clock, run_backtest, BacktestResult
#include "atx/vol/dispersion.hpp"          // DispersionUniverse, DispersionMember
#include "atx/vol/research/dispersion_backtest.hpp" // make_dispersion_ladder_config, make_..._strategy
#include "atx/vol/types.hpp"

#include "bench_util.hpp"
#include "support/synth_corpus.hpp"

namespace atx::vol::bench {
namespace {

// Ladder dimensions. kLadderNames basket members + 1 index leg; a clip is a straddle
// per member, so one cohort is (kLadderNames + 1) * 2 option lots and the book at
// step n is roughly n times that.
constexpr int kLadderNames = 10;
constexpr std::uint32_t kLadderUidBase = 600;  // distinct from every other fixture
constexpr double kLadderTenorDays = 91.0;      // 3 months (T≈0.249 at entry)
constexpr double kLadderGrossIndexVega = 10'000.0;

// The swept ladder depths (dates). Each is its OWN corpus fixture, because the Clock
// length IS the independent variable: a deeper ladder is a longer run, and the point
// of the sweep is that per-step cost rises with the book the earlier steps built.
constexpr int kLadderDepths[] = {10, 30, 60};

[[nodiscard]] const Clock& ladder_clock(int n_dates) {
  // One process-lifetime fixture per depth. Static locals rather than a map so each
  // corpus is written exactly once, on first use of that depth.
  static const Clock c10 =
      build_synth_corpus(10, kLadderNames + 1, kLadderUidBase, "atx-dispersion-ladder-bench-10");
  static const Clock c30 =
      build_synth_corpus(30, kLadderNames + 1, kLadderUidBase, "atx-dispersion-ladder-bench-30");
  static const Clock c60 =
      build_synth_corpus(60, kLadderNames + 1, kLadderUidBase, "atx-dispersion-ladder-bench-60");
  if (n_dates == 10) {
    return c10;
  }
  if (n_dates == 30) {
    return c30;
  }
  if (n_dates == 60) {
    return c60;
  }
  corpus_fatal("ladder_clock: unswept depth " + std::to_string(n_dates));
}

// Index leg = the first synthetic underlier ("U0"), basket = the rest, equally
// weighted. Equal weights keep the clip deterministic and make the per-name lot
// count trivially predictable; cap-weighting is an economics question this
// throughput benchmark has no view on.
[[nodiscard]] DispersionUniverse make_ladder_universe() {
  DispersionUniverse universe;
  universe.index = DispersionMember{"U0", kLadderUidBase, 1.0};
  universe.names.reserve(static_cast<std::size_t>(kLadderNames));
  const double w = 1.0 / static_cast<double>(kLadderNames);
  for (int i = 1; i <= kLadderNames; ++i) {
    universe.names.push_back(DispersionMember{
        "U" + std::to_string(i), kLadderUidBase + static_cast<std::uint32_t>(i), w});
  }
  return universe;
}

[[nodiscard]] DispersionBacktestConfig make_ladder_config() {
  DispersionBacktestConfig config = make_dispersion_ladder_config(
      kLadderTenorDays, kLadderGrossIndexVega, static_cast<std::size_t>(kLadderNames));
  // Daily delta hedge to zero: the whole open book is repriced every step to supply
  // the per-uid hedge delta, which is what makes the growing book the dominant cost
  // rather than just the entry.
  config.delta_band = 0.0;
  return config;
}

// Untimed prime of turbo / I-cache / the run_backtest thread pool / the snapshot
// cache, mirroring backtest_throughput_bench's in-body warm-up (apply_common's
// GB-managed MinWarmUpTime is illegal alongside the explicit Iterations() below).
void warm_up_ladder(const Clock& clock, const DispersionBacktestConfig& config) {
  auto warm = run_dispersion_backtest(clock, make_ladder_universe(), config);
  benchmark::DoNotOptimize(warm);
}

// ── Ladder: daily 3M dispersion clips, held to expiry, daily delta hedge ────────
void BM_DispersionLadder(benchmark::State& state) {
  const int n_dates = static_cast<int>(state.range(0));
  const Clock& clock = ladder_clock(n_dates);
  const DispersionBacktestConfig config = make_ladder_config();
  warm_up_ladder(clock, config);

  double final_open_lots = 0.0;
  for (auto _ : state) {
    auto res = run_dispersion_backtest(clock, make_ladder_universe(), config);
    if (!res.has_value()) {
      state.SkipWithError(res.error().to_string().c_str());
      break;
    }
    if (res->size() != static_cast<std::size_t>(n_dates)) {
      state.SkipWithError("run_dispersion_backtest produced unexpected row count");
      break;
    }
    if (!res->n_open_lots.empty()) {
      final_open_lots = res->n_open_lots.back();
    }
    benchmark::DoNotOptimize(res->nav.data());
    benchmark::ClobberMemory();
  }

  // Headline unit is steps/s so this row is directly comparable to the other
  // backtest rows. The ladder-specific numbers are the counters:
  //   final_open_lots  — the book the last step priced (the thing being scaled)
  //   lot_steps_per_s  — Σ over steps of the open lots priced at that step, per
  //                      second. THIS is the throughput that should stay roughly
  //                      flat across depths if per-lot cost is constant; steps/s
  //                      necessarily falls as the ladder deepens, and reading that
  //                      fall as a regression would be a mistake.
  // Σ_{i=1..n-1} i*clip = clip * n(n-1)/2 counts the lots priced across the run,
  // using the ladder's exact fill schedule (one cohort per step) rather than an
  // average, so it is a count and not an estimate.
  const int priced_steps = n_dates - 1;
  const long long clip_lots = static_cast<long long>(kLadderNames + 1) * 2;
  const long long lot_steps =
      clip_lots * static_cast<long long>(priced_steps) * static_cast<long long>(n_dates) / 2;
  const double iters = static_cast<double>(state.iterations());
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(priced_steps));
  state.counters["lot_steps_per_s"] =
      benchmark::Counter(iters * static_cast<double>(lot_steps), benchmark::Counter::kIsRate);
  state.counters["final_open_lots"] = final_open_lots;
  state.counters["clip_lots"] = static_cast<double>(clip_lots);
}

// Same sampling scheme as the other heavy backtest rows (see the long note in
// backtest_throughput_bench.cpp): fixed work per rep so each rep-mean averages
// exactly kLadderItersPerRep full backtests, which is what makes the CV meaningful,
// with GB's warm-up cleared because Iterations() forbids it.
constexpr int kLadderReps = 15;
constexpr int kLadderItersPerRep = 2;

const int kLadderRegistered = [] {
  auto* b = apply_common(benchmark::RegisterBenchmark("backtest/dispersion_ladder_3m/steps",
                                                      BM_DispersionLadder))
                ->MinWarmUpTime(0.0)
                ->Unit(benchmark::kMillisecond)
                ->Iterations(kLadderItersPerRep)
                ->Repetitions(kLadderReps)
                ->UseRealTime();
  for (const int d : kLadderDepths) {
    b->Arg(d);
  }
  return 0;
}();

}  // namespace
}  // namespace atx::vol::bench
