// backtest_throughput_bench.cpp — Google Benchmark throughput for the atx-vol
// backtest engine's `run_backtest` over a non-trivial multi-underlier American book.
//
// Relocated from tests/backtest_bench_test.cpp (which was a throughput smoke gated
// by ATX_VOL_SKIP_UNLESS_BENCH, asserting only a generous wall-clock ceiling — not a
// correctness gate). Here the same measurement becomes a proper benchmark: the
// engine's steps/s (backtest steps priced per wall-second) is what items/s reports.
//
// SYNTHETIC surfaces only (the backtest_test / tearsheet_test make_surface pattern —
// analytic eSSVI, no fitting): a real corpus fit is slow and would swamp the timing.
// We write one synthetic archive per date (U surfaces, distinct uids / symbols) over
// D dates, then run a multi-leg straddle strategy (one straddle clip per underlier,
// EveryStep / HoldToExpiry) so the book grows to D*U*2 overlapping lots — a genuinely
// large multi-underlier American book. The corpus/archives + Clock are built ONCE
// (static fixture, the bench-suite convention) OUTSIDE the timed loop; only
// `run_backtest` is timed. `run_backtest` fans out over all cores by default, so the
// case runs `->UseRealTime()` — the reported time (and hence items/s = steps/s) is
// wall-clock, matching the test's original steady_clock measurement.

// The surface/archive/curve headers this file used to name went out with the corpus
// fixture — support/synth_corpus.hpp owns them now. What remains here is the
// strategy + engine surface plus what bench_fatal needs.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include <benchmark/benchmark.h>

#include "atx/vol/api/backtest/backtest.hpp" // Clock, MarketSnapshot, run_backtest, BacktestResult
#include "atx/vol/api/backtest/strategy.hpp" // DeclarativeStrategy, StrategySpec

#include "bench_util.hpp"
#include "support/synth_corpus.hpp" // build_synth_corpus — the per-date archive Clock

namespace atx::vol::bench {
namespace {

// Bench dimensions. D dates x U underliers. A straddle clip (2 legs per underlier)
// is opened EVERY step and held to expiry, so the book grows to D*U*2 lots — a
// genuinely non-trivial multi-underlier American book. Sized so the whole run
// stays a few seconds (American greeks over a growing book are ~ms/lot). The
// straddle tenor stays inside the synthetic grid's [0.05, 1.0] T span across the
// whole run (front cohort ages to ~0.10, never below 0.05).
constexpr int kD = 20;                  // dates (=> D-1 priced steps)
constexpr int kU = 4;                   // underliers per date
constexpr std::uint32_t kUidBase = 100;
constexpr double kTargetT = 0.15;       // straddle tenor (in-grid all run)

// ── Universe-shaped case (M2) dimensions ─────────────────────────────────────
// N names × daily entry × held-to-expiry × daily delta-hedge — the WS-D dispersion
// strategy shape (40Δ strangles, DeltaToZero daily). Sized so the growing book
// (~kUnivN×(kUnivD-1)×2 option lots ≈ 180) lands near the scoreboard's 160-lot
// reference so the reshaped steps/s is directly comparable to the ~64 steps/s
// baseline, while the SHAPE (per-entry 40Δ delta-solves + a daily full-book hedge)
// is the real hot loop the ≥5–10× gate targets. The 3M tenor stays inside the
// synthetic grid's [0.05, 1.0] span across the whole (~9 trading-day) run.
constexpr int kUnivD = 10;              // universe dates (=> 9 priced steps)
constexpr int kUnivN = 10;              // universe names
constexpr std::uint32_t kUnivUidBase = 200;
constexpr double kUnivTargetT = 0.25;   // 3M strangle tenor (in-grid all run)

// ── L3 entry-resolve wall dimensions ─────────────────────────────────────────
// The 51-name delta-strangle entry day (finding-3 #1 wall): one resolve_spec pass
// resolves 51 names × 2 sides of 40Δ strikes (Illinois iterations) + a full_greek
// seed per resolved strike. L3 fans this across the shared pool.
constexpr int kEntryN = 51;             // 51-name entry-resolve config (L3 gate)
constexpr std::uint32_t kEntryUidBase = 400;

// Setup failures happen once, outside the timed region (static fixture init), so an
// abort is the honest response — a benchmark of an error return measures nothing.
[[noreturn]] void bench_fatal(const std::string& msg) {
  std::fprintf(stderr, "FATAL(backtest-throughput-bench): %s\n", msg.c_str());
  std::abort();
}

// Process-lifetime Clock fixtures (built once on first use).
[[nodiscard]] const Clock& corpus_clock() {
  static const Clock clock =
      build_synth_corpus(kD, kU, kUidBase, "atx-backtest-throughput-bench");
  return clock;
}

[[nodiscard]] const Clock& universe_clock() {
  static const Clock clock =
      build_synth_corpus(kUnivD, kUnivN, kUnivUidBase, "atx-backtest-universe-bench");
  return clock;
}

// One ATM straddle clip per underlier, a fresh cohort every step, held to expiry.
[[nodiscard]] StrategySpec make_spec() {
  StrategySpec spec;
  spec.name = "bench-multi-underlier-straddle";
  for (int u = 0; u < kU; ++u) {
    LegSpec leg;
    leg.uid = kUidBase + static_cast<std::uint32_t>(u);
    leg.tenor.target_T = kTargetT;
    leg.structure.kind = StructureSpec::Kind::Straddle;  // ATM call + put
    leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
    spec.legs.push_back(leg);
  }
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  return spec;
}

// The WS-D universe strategy shape: for each of kUnivN names, a 40Δ Strangle (OTM
// call + OTM put, strikes resolved by delta off the synthetic surface), a fresh
// cohort EVERY step, HELD TO EXPIRY (overlapping cohorts), with a DeltaToZero hedge
// rebalanced DAILY. FixedContracts sizing keeps the book deterministic (net-vega
// cross-leg sizing is a WS-D/D4 concern, not this throughput baseline). The engine
// executes the hedge overlay (validate_hedge_spec accepts DeltaToZero+Daily; the
// step re-prices the full book once to supply both entry Greeks and the per-uid
// hedge delta), so this steps/s number is the real deserialize+price+delta-hedge
// hot loop the ≥5–10× gate targets.
[[nodiscard]] StrategySpec make_universe_spec() {
  StrategySpec spec;
  spec.name = "universe-40d-strangle-htx-dhedge";
  for (int u = 0; u < kUnivN; ++u) {
    LegSpec leg;
    leg.uid = kUnivUidBase + static_cast<std::uint32_t>(u);
    leg.tenor.target_T = kUnivTargetT;
    leg.tenor.snap_to_listed = false;
    leg.structure.kind = StructureSpec::Kind::Strangle;  // OTM call + OTM put
    leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
    spec.legs.push_back(std::move(leg));
  }
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.hedge = HedgeSpec{HedgeSpec::Kind::DeltaToZero, HedgeSpec::Cadence::Daily, 0.0};
  return spec;
}

// Manual warm-up — replaces apply_common's GB-managed MinWarmUpTime(0.5), which
// Google Benchmark 1.9.0 FORBIDS alongside the review-mandated explicit
// Iterations() (Benchmark::Iterations asserts IsZero(min_warmup_time_), so the
// combination aborts the whole binary at static-init). One untimed backtest
// primes turbo / I-cache / the run_backtest thread-pool / the snapshot cache
// before the timed reps — the same "brief turbo prime" intent, moved in-body so
// the review's Iterations(kBtItersPerRep) x Repetitions(kBtReps) sampling scheme
// (and its CV/p95 stats) is preserved verbatim. Runs once per repetition (each BM
// body is invoked once per rep), matching MinWarmUpTime's per-rep warm-up cadence.
void warm_up_backtest(const Clock& clock, const StrategySpec& spec) {
  DeclarativeStrategy warm{spec};
  auto warm_res = run_backtest(clock, warm);
  benchmark::DoNotOptimize(warm_res);
}

// ── Throughput: D dates x U underliers, straddle clips held to expiry ──────────
void BM_MultiUnderlierStraddle(benchmark::State& state) {
  const Clock& clock = corpus_clock();
  const StrategySpec spec = make_spec();
  warm_up_backtest(clock, spec);  // in-body warm-up (see warm_up_backtest / registration)

  double final_open_lots = 0.0;
  for (auto _ : state) {
    // Fresh strategy each iteration (cheap spec copy) so every timed run opens the
    // same deterministic cohort sequence; the ~µs construction is negligible against
    // the multi-second backtest and keeps runs independent.
    DeclarativeStrategy strat{spec};
    auto res = run_backtest(clock, strat);
    if (!res.has_value()) {
      state.SkipWithError(res.error().to_string().c_str());
      break;
    }
    if (res->size() != static_cast<std::size_t>(kD)) {
      state.SkipWithError("run_backtest produced unexpected row count");
      break;
    }
    if (!res->n_open_lots.empty()) {
      final_open_lots = res->n_open_lots.back();
    }
    benchmark::DoNotOptimize(res->nav.data());
    benchmark::ClobberMemory();
  }

  // Headline unit: priced steps (D-1). items/s == steps/s (the metric the test
  // measured). leg_steps = straddle (2 legs) x U underliers x priced_steps.
  const int priced_steps = kD - 1;
  const long long leg_steps = static_cast<long long>(priced_steps) * kU * 2;
  const double iters = static_cast<double>(state.iterations());
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(priced_steps));
  state.counters["leg_steps_per_s"] =
      benchmark::Counter(iters * static_cast<double>(leg_steps), benchmark::Counter::kIsRate);
  state.counters["final_open_lots"] = final_open_lots;
}

// ── Throughput (M2): universe of 40Δ strangles, held to expiry, daily delta-hedge ──
// The reshaped real-universe case. kUnivN names each enter a 40Δ strangle EVERY step
// (strikes resolved by delta off the synthetic surface — the per-entry solve WS-P P4
// batches), held to expiry (overlapping cohorts), with a DAILY DeltaToZero hedge. The
// book grows to ~kUnivN×(kUnivD-1)×2 option lots; each step re-prices the whole open
// book once for marks + Greeks + the per-uid hedge delta. Headline: steps/s (== the
// scoreboard's backtest ★ratify metric); final_open_lots reports the book size.
void BM_UniverseStrangleHedged(benchmark::State& state) {
  const Clock& clock = universe_clock();
  const StrategySpec spec = make_universe_spec();
  warm_up_backtest(clock, spec);  // in-body warm-up (see warm_up_backtest / registration)

  double final_open_lots = 0.0;
  for (auto _ : state) {
    DeclarativeStrategy strat{spec};
    auto res = run_backtest(clock, strat);
    if (!res.has_value()) {
      state.SkipWithError(res.error().to_string().c_str());
      break;
    }
    if (res->size() != static_cast<std::size_t>(kUnivD)) {
      state.SkipWithError("run_backtest produced unexpected row count");
      break;
    }
    if (!res->n_open_lots.empty()) {
      final_open_lots = res->n_open_lots.back();
    }
    benchmark::DoNotOptimize(res->nav.data());
    benchmark::ClobberMemory();
  }

  const int priced_steps = kUnivD - 1;
  // leg_steps = strangle (2 legs) × N names × priced_steps (entry legs opened; the
  // held book that each step reprices is far larger — final_open_lots captures it).
  const long long leg_steps = static_cast<long long>(priced_steps) * kUnivN * 2;
  const double iters = static_cast<double>(state.iterations());
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(priced_steps));
  state.counters["leg_steps_per_s"] =
      benchmark::Counter(iters * static_cast<double>(leg_steps), benchmark::Counter::kIsRate);
  state.counters["names"] = static_cast<double>(kUnivN);
  state.counters["final_open_lots"] = final_open_lots;
}

// A kEntryN-name 40Δ Strangle spec — one entry day's resolution workload.
[[nodiscard]] StrategySpec make_entry_spec() {
  StrategySpec spec;
  spec.name = "l3-entry-resolve-51name";
  for (int u = 0; u < kEntryN; ++u) {
    LegSpec leg;
    leg.uid = kEntryUidBase + static_cast<std::uint32_t>(u);
    leg.tenor.target_T = kUnivTargetT;
    leg.tenor.snap_to_listed = false;
    leg.structure.kind = StructureSpec::Kind::Strangle;
    leg.structure.call_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.structure.put_leg = StrikeSelector{StrikeSelector::Kind::Delta, 0.40};
    leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
    spec.legs.push_back(std::move(leg));
  }
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryStep;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  return spec;
}

// One loaded kEntryN-name snapshot (built once; a 2-date corpus, date 0 loaded).
[[nodiscard]] const MarketSnapshot& entry_snapshot() {
  static const MarketSnapshot snap = [] {
    const Clock clock = build_synth_corpus(2, kEntryN, kEntryUidBase, "atx-l3-entry-resolve-bench");
    auto s = MarketSnapshot::load(clock.refs()[0].archive_path);
    if (!s.has_value()) {
      bench_fatal(s.error().to_string());
    }
    return std::move(*s);
  }();
  return snap;
}

// ── L3: entry-day strike-resolve + full_greek-seed wall (finding-3 #1) ──────────
// One resolve_spec over kEntryN 40Δ-strangle legs = the per-name Illinois strike
// solves + the full_greek seed per resolved strike, the 0.75-4.5 s single-threaded
// entry-day wall. L3 fans this across the shared pool. Arg(0) = price.n_threads:
// 1 = serial-equivalent (single pool worker == the pre-L3 wall), 0 = full pool (the
// L3 fanned path). Headline: ms per resolve == one entry day's resolve+seed wall
// (gate: <= 50 ms fanned). PROVISIONAL — cite only under the quiet-window
// (rel-avx2, P-core lease); the deterministic bit-identity of entries is the hard
// gate (backtest/strategy tests), this row is the timing follow-up.
void BM_EntryResolve(benchmark::State& state) {
  const MarketSnapshot& snap = entry_snapshot();
  const StrategySpec spec = make_entry_spec();
  PriceOptions po;
  po.n_threads = static_cast<unsigned>(state.range(0));
  {
    auto warm = resolve_spec(snap, spec, po);  // prime the pool + surfaces
    if (!warm.has_value()) {
      state.SkipWithError(warm.error().to_string().c_str());
      return;
    }
    benchmark::DoNotOptimize(warm->data());
  }
  std::size_t n_sized = 0;
  for (auto _ : state) {
    auto sized = resolve_spec(snap, spec, po);
    if (!sized.has_value()) {
      state.SkipWithError(sized.error().to_string().c_str());
      break;
    }
    n_sized = sized->size();
    benchmark::DoNotOptimize(sized->data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kEntryN));
  state.counters["names"] = static_cast<double>(kEntryN);
  state.counters["legs_resolved"] = static_cast<double>(n_sized);
}

// Sample-count override (review fix, finding #2). These are heavy sub-second
// backtests: apply_common's default (5 reps, and with a ~0.3 s op only ~1-2 auto
// iterations/rep) gives only ~5-10 timed executions, so two outlier reps swing the
// CV. Fix each rep to Iterations(kBtItersPerRep) work (each reported rep-mean
// averages that many full backtests) AND raise to kBtReps repetitions — enough that
// no pair of noisy reps dominates the CV before M4 freezes the steps/s as a gate.
constexpr int kBtReps = 15;
constexpr int kBtItersPerRep = 4;

// Warm-up reconciliation (SHAPE (a), chosen deliberately). apply_common chains
// MinWarmUpTime(0.5), but Google Benchmark 1.9.0's Benchmark::Iterations() asserts
// IsZero(min_warmup_time_) — so apply_common(...)->Iterations(N) aborts the binary
// at static-init (benchmark_register.cc:369). We clear the GB warm-up on JUST these
// two rows with ->MinWarmUpTime(0.0) (legal here: iterations_ is still 0 at that
// point in the chain) and re-supply the warm-up manually in-body via
// warm_up_backtest() (one untimed op per rep). We deliberately KEEP explicit
// Iterations(kBtItersPerRep) rather than switch to MinTime auto-iteration (shape
// (b)): the finding-#2 fix relies on DETERMINISTIC, fixed work per rep so each
// rep-mean averages exactly kBtItersPerRep full backtests — the property that makes
// the CV meaningful. So the sampling scheme (kBtItersPerRep x kBtReps, CV/p95) is
// preserved verbatim; only the warm-up delivery mechanism moved (GB -> in-body).
// apply_common is shared by ~20 other bench files, so this is fixed at the two
// conflicting rows here, never in apply_common.
const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("backtest/multiunderlier_straddle/steps",
                                            BM_MultiUnderlierStraddle))
      ->MinWarmUpTime(0.0)  // clear apply_common's GB warm-up (illegal with Iterations); warm-up is in-body
      ->Unit(benchmark::kMillisecond)
      ->Iterations(kBtItersPerRep)
      ->Repetitions(kBtReps)
      ->UseRealTime();
  apply_common(benchmark::RegisterBenchmark("backtest/universe_strangle_hedged/steps",
                                            BM_UniverseStrangleHedged))
      ->MinWarmUpTime(0.0)  // clear apply_common's GB warm-up (illegal with Iterations); warm-up is in-body
      ->Unit(benchmark::kMillisecond)
      ->Iterations(kBtItersPerRep)
      ->Repetitions(kBtReps)
      ->UseRealTime();
  // L3 entry-resolve wall: Arg 1 = serial-equivalent (single pool worker), Arg 0 =
  // full pool (fanned). Compare the two rows for the L3 speedup on the 51-name day.
  apply_common(benchmark::RegisterBenchmark("strategy/entry_resolve/51name", BM_EntryResolve))
      ->MinWarmUpTime(0.0)  // clear apply_common's GB warm-up (illegal with Iterations); warm-up is in-body
      ->Unit(benchmark::kMillisecond)
      ->Arg(1)  // serial-equivalent (single pool worker == pre-L3 wall)
      ->Arg(0)  // full pool (L3 fanned)
      ->Iterations(kBtItersPerRep)
      ->Repetitions(kBtReps)
      ->UseRealTime();
  return 0;
}();

}  // namespace
}  // namespace atx::vol::bench
