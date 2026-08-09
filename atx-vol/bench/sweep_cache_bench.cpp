// sweep_cache_bench.cpp — the sprint's headline cache number: `run_sweep`
// (Task C3, atx/vol/sweep_driver.hpp) over a 32-variant grid, COLD (empty
// lakehouse => every variant is a miss, a real backtest runs) vs WARM (the
// SAME grid re-run against an already-populated lakehouse => every variant
// is a catalog probe hit, no backtest runs at all — the "cached rerun" path
// this file's name refers to).
//
// Only built when ATX_VOL_LAKEHOUSE is ON (bench/CMakeLists.txt): `run_sweep`
// needs Catalog/TrackStore compiled into atx-vol, exactly like
// tests/sweep_driver_test.cpp (tests/CMakeLists.txt gates that file the same
// way).
//
// WHY THIS IS THE HEADLINE NUMBER (sprint plan, Task E3 Step 1): the whole
// point of the D1-D3 lakehouse (content-addressed TrackKey + Parquet store +
// SQLite catalog) is that a strategy-development iteration loop re-runs a
// sweep grid over and over as it tweaks ONE variant at a time — every
// variant that has not changed should cost a cheap catalog probe, not a full
// backtest recompute. `sweep/cached_rerun` measures exactly that ratio.
//
// GRID: 32 variants — 16 entry-cadences (entry_every_n in {100,110,...,250})
// x 2 position signs (+1/-1) — chosen so every variant hashes to a DISTINCT
// TrackKey (no accidental dedupe collapsing the grid; see
// SweepDriverTest.DuplicateVariantsCollapseAndRerunIsAllCacheHits for the
// dedupe behavior this deliberately avoids exercising here). Corpus: a
// small (kDates-day, single-underlier) synthetic archive corpus — reused
// from support/synth_corpus.hpp with n_names=1 — kept small so a COLD
// 32-variant sweep (32 real backtests) stays a fraction of a second; the
// bench's interesting number is the cold/warm RATIO, not either absolute
// time.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/vol/backtest.hpp"          // Clock
#include "atx/vol/backtest_template.hpp" // BacktestStrategyTemplate, make_40_delta_3_calendar_month_strangle_template
#include "atx/vol/sweep_driver.hpp"      // SweepSpec, SweepConfig, SweepResult, run_sweep
#include "atx/vol/track_store.hpp"       // TrackMeta

#include "bench_util.hpp"
#include "support/synth_corpus.hpp" // build_synth_corpus

namespace atx::vol::bench {
namespace {

constexpr int kDates = 12;              // small single-underlier corpus (cheap cold backtests)
constexpr std::uint32_t kUidBase = 900; // distinct from every other bench's uid range
constexpr int kNumEntryCadences = 16;   // entry_every_n in {100,110,...,250}
constexpr int kNumVariants = kNumEntryCadences * 2; // x{+1,-1} => 32

[[noreturn]] void sweep_bench_fatal(const std::string &msg) {
  std::fprintf(stderr, "FATAL(sweep-cache-bench): %s\n", msg.c_str());
  std::abort();
}

[[nodiscard]] const Clock &sweep_corpus_clock() {
  static const Clock clock = build_synth_corpus(kDates, /*n_names=*/1, kUidBase, "atx-sweep-cache-bench");
  return clock;
}

// 32 distinct-economics variants: every (sign, entry_every_n) pair is a
// different TrackKey (SweepSpec::variants is the enumeration/dedupe order —
// see sweep_driver.hpp's file doc comment), so a cold sweep runs exactly 32
// backtests and a warm rerun hits exactly 32 cached rows.
[[nodiscard]] std::vector<BacktestStrategyTemplate> make_variant_grid() {
  std::vector<BacktestStrategyTemplate> variants;
  variants.reserve(static_cast<std::size_t>(kNumVariants));
  for (int i = 0; i < kNumEntryCadences; ++i) {
    const unsigned entry_every_n = 100u + 10u * static_cast<unsigned>(i);
    for (const double sign : {1.0, -1.0}) {
      auto made = make_40_delta_3_calendar_month_strangle_template(sign, entry_every_n);
      if (!made.has_value()) {
        sweep_bench_fatal(made.error().to_string());
      }
      variants.push_back(std::move(*made));
    }
  }
  return variants;
}

[[nodiscard]] std::array<std::uint8_t, 32> fixed_snapshot_id() {
  std::array<std::uint8_t, 32> id{};
  id.fill(0xCB); // "cache bench" — arbitrary fixed byte, never compared cross-run
  return id;
}

[[nodiscard]] SweepSpec make_spec() {
  SweepSpec spec;
  spec.variants = make_variant_grid();
  spec.clock = sweep_corpus_clock();
  spec.uid = kUidBase;
  spec.meta = TrackMeta{"SWPB", "sweep_cache_bench"};
  spec.data_snapshot_id = fixed_snapshot_id();
  return spec;
}

// Every fresh temp dir this bench creates (cold-run lakes), so it can clean
// up AFTER the timed loop rather than paying filesystem deletion cost inside
// the measured region.
std::vector<std::filesystem::path> g_cold_dirs_to_clean;
std::atomic<int> g_cold_lake_counter{0};

[[nodiscard]] std::filesystem::path fresh_cold_lake_dir() {
  const int n = g_cold_lake_counter.fetch_add(1);
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
                               ("atx-sweep-cache-bench-cold-" + std::to_string(n));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  return dir;
}

// ── COLD: an empty lakehouse every iteration => every variant is a miss ────
// Each iteration gets its OWN fresh empty lake_root/sweep_id (via
// fresh_cold_lake_dir/iteration counter) so it genuinely measures 32 fresh
// backtests, never a warm hit. SweepConfig::n_threads=0 (auto fan-out) --
// the realistic topology a sweep-heavy workload would actually use, not an
// artificially serialized one.
void BM_SweepCold(benchmark::State &state) {
  const SweepSpec spec = make_spec();
  std::vector<std::filesystem::path> my_dirs;

  for (auto _ : state) {
    state.PauseTiming();
    const std::filesystem::path dir = fresh_cold_lake_dir();
    my_dirs.push_back(dir);
    SweepConfig config;
    config.lake_root = dir.string();
    config.sweep_id = "cold-" + dir.filename().string();
    config.n_threads = 0;
    state.ResumeTiming();

    auto result = run_sweep(spec, config);
    if (!result.has_value()) {
      state.SkipWithError(result.error().to_string().c_str());
      break;
    }
    if (result->engine_runs != static_cast<std::uint64_t>(kNumVariants) || result->cache_hits != 0u) {
      state.SkipWithError("cold sweep did not run every variant fresh -- corpus/lake fixture is not actually cold");
      break;
    }
    benchmark::DoNotOptimize(result->variants.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kNumVariants));
  state.counters["variants"] = static_cast<double>(kNumVariants);
  state.counters["engine_runs_per_iter"] = static_cast<double>(kNumVariants);

  for (auto &d : my_dirs) {
    g_cold_dirs_to_clean.push_back(d);
  }
}

// ── WARM: one lake populated ONCE (untimed), then re-run the SAME sweep
// against it repeatedly -- every variant is a cache hit, so this is purely
// the catalog probe path (D3 SQLite) plus dedupe/enumeration overhead, never
// a backtest. `run_sweep` on an all-hit sweep only APPENDS `trials` rows
// (see SweepDriverTest.DuplicateVariantsCollapseAndRerunIsAllCacheHits) --
// no tracks/staging mutation -- so re-running it thousands of times in a
// timed loop is safe and idempotent w.r.t. what this bench measures.
struct WarmFixture {
  std::filesystem::path dir;
  SweepSpec spec;
  SweepConfig config;
};

[[nodiscard]] const WarmFixture &warm_fixture() {
  static const WarmFixture fixture = [] {
    WarmFixture f;
    f.dir = std::filesystem::temp_directory_path() / "atx-sweep-cache-bench-warm";
    std::error_code ec;
    std::filesystem::remove_all(f.dir, ec);
    f.spec = make_spec();
    f.config.lake_root = f.dir.string();
    f.config.sweep_id = "warm-fixture-populate";
    f.config.n_threads = 0;
    // Untimed cold pass: populates the lake so every subsequent call in the
    // timed benchmark below is a pure cache hit.
    auto populate = run_sweep(f.spec, f.config);
    if (!populate.has_value()) {
      sweep_bench_fatal(populate.error().to_string());
    }
    if (populate->engine_runs != static_cast<std::uint64_t>(kNumVariants)) {
      sweep_bench_fatal("warm fixture population did not run every variant");
    }
    return f;
  }();
  return fixture;
}

void BM_SweepWarmRerun(benchmark::State &state) {
  const WarmFixture &fixture = warm_fixture();
  // One untimed rerun to prove hot-cache steady state before the timed loop
  // (mirrors backtest_throughput_bench.cpp's warm_up_backtest convention).
  {
    auto warm = run_sweep(fixture.spec, fixture.config);
    if (!warm.has_value() || warm->cache_hits != static_cast<std::uint64_t>(kNumVariants)) {
      sweep_bench_fatal("warm fixture is not actually warm before the timed loop");
    }
  }

  for (auto _ : state) {
    SweepConfig config = fixture.config;
    // Fresh sweep_id per call: trials accumulate per sweep_id (attempts, not
    // unique configs — sweep_driver.hpp's file doc comment), and reusing one
    // id across thousands of timed iterations would grow trials.n_trials
    // without bound and eventually bias the catalog's own read cost. A
    // sequential counter keeps every call's write (record_trial) real and
    // bounded, same as production reruns each getting their own sweep_id.
    static std::atomic<int> counter{0};
    config.sweep_id = "warm-" + std::to_string(counter.fetch_add(1));

    auto result = run_sweep(fixture.spec, config);
    if (!result.has_value()) {
      state.SkipWithError(result.error().to_string().c_str());
      break;
    }
    if (result->engine_runs != 0u || result->cache_hits != static_cast<std::uint64_t>(kNumVariants)) {
      state.SkipWithError("warm rerun was not all cache hits -- fixture went cold mid-benchmark");
      break;
    }
    benchmark::DoNotOptimize(result->variants.data());
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kNumVariants));
  state.counters["variants"] = static_cast<double>(kNumVariants);
  state.counters["cache_hits_per_iter"] = static_cast<double>(kNumVariants);
}

// Iteration/repetition budget mirrors backtest_throughput_bench.cpp's
// kBtItersPerRep/kBtReps convention (fixed work per rep so the CV is
// meaningful) — cold is 32x a single small backtest, so a smaller
// Iterations() count keeps total wall time bounded; warm is pure catalog
// probes, cheap enough to afford more.
constexpr int kColdItersPerRep = 2;
constexpr int kColdReps = 10;
constexpr int kWarmItersPerRep = 5;
constexpr int kWarmReps = 10;

const int kRegistered = [] {
  apply_common(benchmark::RegisterBenchmark("sweep/cached_rerun/cold_32variant", BM_SweepCold))
      ->MinWarmUpTime(0.0) // in-body cold pass already IS the warm-up per iteration; GB warm-up would just add more cold sweeps
      ->Unit(benchmark::kMillisecond)
      ->Iterations(kColdItersPerRep)
      ->Repetitions(kColdReps)
      ->UseRealTime();
  apply_common(benchmark::RegisterBenchmark("sweep/cached_rerun/warm_32variant_rerun", BM_SweepWarmRerun))
      ->MinWarmUpTime(0.0) // explicit untimed warm() call inside BM_SweepWarmRerun already primes the cache
      ->Unit(benchmark::kMillisecond)
      ->Iterations(kWarmItersPerRep)
      ->Repetitions(kWarmReps)
      ->UseRealTime();
  return 0;
}();

// Process-exit cleanup of every cold-run temp lake (kept out of the timed
// region the whole run, see fresh_cold_lake_dir/g_cold_dirs_to_clean above).
struct ColdDirCleanup {
  ~ColdDirCleanup() {
    for (const auto &d : g_cold_dirs_to_clean) {
      std::error_code ec;
      std::filesystem::remove_all(d, ec);
    }
  }
};
const ColdDirCleanup kColdDirCleanup;

} // namespace
} // namespace atx::vol::bench
