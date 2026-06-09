// research_bench.cpp — the S4b capstone throughput + scale-lever benchmark.
// MEASURED-ONLY: no correctness assertions, no production code. Mirrors
// factory_bench.cpp's Google Benchmark form.
//
// Two headline measurements:
//
//   (A) ENGINE THROUGHPUT — BM_ResearchEngine. One ResearchDriver::run over a
//       fixed budget on a planted-edge panel: reports alphas mined/sec (the run's
//       total_mined as items_per_second) plus admitted / dedup% / library-growth
//       counters. The whole fixture (DSL Library / Panel / sim / policy / gate) is
//       built ONCE; each iteration runs the engine into a FRESH temp-dir library
//       (admission from empty — the real per-engine cost).
//
//   (B) THE SCALE LEVER — BM_LibraryPoolWorstCorr vs BM_AlphaStorePoolWorstCorr.
//       The ONE operation pool-aware fitness needs from the pool: worst_corr (MAX
//       |corr| of a candidate vs the pool). LibraryPool serves it in O(neighbors)
//       via the SimHash corr index; AlphaStorePool serves it in O(N) via an
//       exhaustive scan. Both are swept over GROWING pool sizes (64/256/1024/4096)
//       so the measured-time gap is readable at scale. NO ideal-speedup claim —
//       the numbers speak. Each pool is built ONCE per size outside the timed loop;
//       only the per-candidate worst_corr probe is timed.
//
// Build is Debug / clang-cl (the project default), so every figure here is an
// UPPER BOUND on cost, not the optimised number. A short min-time / iteration cap
// keeps the Debug bench well under a second per case.

#include <cmath>        // std::sin (synthetic low-corr pnl streams)
#include <cstdint>      // std::uint64_t (the deterministic LCG state)
#include <filesystem>   // per-iteration temp dir for the engine library
#include <span>
#include <string>       // std::string (seed exprs / field names / tmp dirs)
#include <system_error> // std::error_code (bench_tmpdir's remove_all/create_directories)
#include <utility>      // std::move
#include <vector>       // std::vector (synthetic panel / pnl storage)

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp" // f64, u32, u64, usize

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp" // alpha::Library (its definition site)
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp"    // combine::AlphaGate, GateConfig
#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics
#include "atx/engine/combine/store.hpp"   // combine::AlphaStore, AlphaId

#include "atx/engine/factory/factory.hpp"   // factory::FactoryConfig
#include "atx/engine/factory/pool_view.hpp" // factory::AlphaStorePool, LibraryPool, PoolView
#include "atx/engine/factory/research_driver.hpp" // factory::ResearchDriver, ResearchConfig, ResearchReport

#include "atx/engine/library/library.hpp" // library::Library, AlphaCandidate, AdmitKind
#include "atx/engine/library/record.hpp"  // library::Provenance

namespace {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaId;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::AlphaStore;
using atx::engine::combine::GateConfig;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::AlphaStorePool;
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::LibraryPool;
using atx::engine::factory::ResearchConfig;
using atx::engine::factory::ResearchDriver;
using atx::engine::factory::ResearchReport;

namespace lib = atx::engine::library;

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

// Deterministic LCG -> uniform(-1, 1) (no RNG dep; the S3-4/S3-5 fixture idiom).
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

// Planted-edge momentum close [dates*insts]: per-instrument persistent drift +
// bounded i.i.d. noise -> rank/ts_mean(close) carry a genuine finite-Sharpe edge.
[[nodiscard]] std::vector<f64> momentum_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.010 - 0.0040 * static_cast<f64>(j);
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.008 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

// A two-field (close + 1-day reversal) planted-edge panel — the seed-grammar field
// surface (matches the integration-test fixture).
[[nodiscard]] Panel planted_panel(usize dates, usize insts) {
  std::vector<f64> close = momentum_close(dates, insts, 0xA11Cu);
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  auto r = Panel::create(dates, insts, {"close", "rev"}, {close, rev}, {});
  return std::move(r.value());
}

[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)", "rank(rev)",         "ts_mean(close, 5)", "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))", "delta(close, 2)"};
}

[[nodiscard]] FactoryConfig per_run_cfg(usize pop, usize gens) {
  FactoryConfig cfg;
  cfg.search.master_seed = 0; // overwritten per run by the engine
  cfg.search.population = pop;
  cfg.search.generations = gens;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.search.novelty_w = 0.1;
  cfg.search.fitness.trial_count = 4;
  cfg.seed_exprs = seed_exprs();
  cfg.panel_fields = {"close", "rev"};
  cfg.min_dsr = 0.5;
  return cfg;
}

// A per-iteration temp directory the engine library is rooted at (fresh per run so
// each engine admits from empty — the real per-engine cost).
[[nodiscard]] std::string bench_tmpdir(const std::string &tag) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s4b_engine_bench" / tag;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  (void)ec;
  std::filesystem::create_directories(dir, ec);
  (void)ec;
  return dir.string();
}

// ===========================================================================
//  (A) BM_ResearchEngine — engine throughput over a fixed budget.
//
//  state.range(0) == per-run population, state.range(1) == generations,
//  state.range(2) == max_runs. One ResearchDriver::run per iteration into a FRESH
//  temp-dir library. Counters:
//    items_per_second   -> alphas MINED (total_mined) per second (the headline).
//    "admitted_per_sec" -> survivors admitted into the persistent library / sec.
//    "library_size"     -> mean final library.n_alphas() over iterations.
//    "dedup_pct"        -> mean ResearchReport.dedup_pct over iterations.
// ===========================================================================
void BM_ResearchEngine(benchmark::State &state) {
  const usize pop = static_cast<usize>(state.range(0));
  const usize gens = static_cast<usize>(state.range(1));
  const usize max_runs = static_cast<usize>(state.range(2));

  const Library dsl{};
  const Panel panel = planted_panel(120, 8);
  const ExecutionSimulator sim = frictionless_sim();
  const WeightPolicy policy{};
  const AlphaGate gate{GateConfig{}};

  ResearchConfig cfg;
  cfg.per_run = per_run_cfg(pop, gens);
  cfg.max_runs = max_runs;
  cfg.patience = 0; // run the full budget every iteration
  cfg.master_seed = 0x5EEDu;

  std::int64_t total_mined = 0;
  std::int64_t total_admitted = 0;
  std::int64_t total_lib = 0;
  double sum_dedup = 0.0;
  int run_idx = 0;

  for (auto _ : state) {
    state.PauseTiming();
    lib::Library library =
        lib::Library::open(bench_tmpdir("engine_" + std::to_string(run_idx++)), GateConfig{},
                           {0xC0FFEEu});
    state.ResumeTiming();

    ResearchDriver engine{library, dsl, panel, sim, policy, gate};
    const ResearchReport rep = engine.run(cfg);

    // Sink to mutable locals so the non-deprecated DoNotOptimize(T&) overload is
    // selected (the const-ref overload is deprecated under -Werror).
    usize sink_mined = rep.total_mined;
    usize sink_admitted = rep.total_admitted;
    benchmark::DoNotOptimize(sink_mined);
    benchmark::DoNotOptimize(sink_admitted);
    benchmark::ClobberMemory();

    total_mined += static_cast<std::int64_t>(rep.total_mined);
    total_admitted += static_cast<std::int64_t>(rep.total_admitted);
    total_lib += static_cast<std::int64_t>(rep.library_size);
    sum_dedup += rep.dedup_pct;
  }

  const double iters = static_cast<double>(state.iterations());
  // alphas mined/sec: total distinct candidates scored across all iterations / time
  // (Google Benchmark reports SetItemsProcessed as items_per_second).
  state.SetItemsProcessed(total_mined);
  state.counters["admitted_per_sec"] =
      benchmark::Counter(static_cast<double>(total_admitted), benchmark::Counter::kIsRate);
  state.counters["library_size"] = (iters > 0.0) ? static_cast<double>(total_lib) / iters : 0.0;
  state.counters["dedup_pct"] = (iters > 0.0) ? sum_dedup / iters : 0.0;
}
BENCHMARK(BM_ResearchEngine)
    ->Args({16, 4, 3})
    ->Args({24, 6, 4})
    ->Unit(benchmark::kMillisecond);

// ===========================================================================
//  Scale-lever pool fixtures.
//
//  Synthetic, low-mutual-correlation pnl streams (each a distinct-phase sinusoid +
//  small per-alpha noise) of length T, with metrics that clear the gate floors so
//  every candidate is admitted and the pool (and its corr index) actually grows to
//  the target size. NaN-free, finite-variance — a clean corr substrate.
// ===========================================================================
constexpr usize kPnlT = 256; // pnl periods (the corr vector length)

// One alpha's pnl stream: a phase-shifted small-amplitude oscillation + light noise,
// distinct per `idx` so streams are mutually low-correlated (the corr index has work
// to do but no member saturates the max_pool_corr gate).
[[nodiscard]] std::vector<f64> synth_pnl(u64 idx) {
  std::vector<f64> pnl(kPnlT, 0.0);
  Lcg rng{0xA11CE5u + idx * 0x9E3779B97F4A7C15ULL};
  const f64 phase = static_cast<f64>(idx) * 0.61803398875; // golden-ratio phase spread
  for (usize t = 1; t < kPnlT; ++t) {
    const f64 ang = 0.10 * static_cast<f64>(t) + phase;
    // A small positive-mean oscillation: mean>0 (clears Sharpe), low amplitude.
    pnl[t] = 0.0009 + 0.0006 * std::sin(ang) + 0.00015 * rng.next();
  }
  return pnl;
}

// Position cross-section over `insts` instruments for one alpha (period-major then
// instrument-minor), modest stable weights so turnover stays under the gate cap.
[[nodiscard]] std::vector<f64> synth_pos(u64 idx, usize insts) {
  std::vector<f64> pos(kPnlT * insts, 0.0);
  for (usize t = 0; t < kPnlT; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 w = 0.1 * std::sin(0.05 * static_cast<f64>(t) +
                                   static_cast<f64>(idx + j) * 0.3);
      pos[t * insts + j] = w;
    }
  }
  return pos;
}

// Metrics that clear the default gate floors (sharpe/fitness >= 1, turnover <= 0.7).
[[nodiscard]] AlphaMetrics passing_metrics() {
  AlphaMetrics m{};
  m.sharpe = 2.0;
  m.turnover = 0.20;
  m.returns = 0.15;
  m.drawdown = 0.05;
  m.margin = 0.75;
  m.fitness = 1.5;
  m.holding_days = 5.0;
  return m;
}

// Fill `store` AND `library` with `n` admitted alphas with the SAME pnl streams, so
// the AlphaStorePool (O(N)) and LibraryPool (O(neighbors)) screen against identical
// pools. A relaxed gate (max_pool_corr = 1.0) guarantees every candidate is accepted
// regardless of mutual corr, so both pools reach exactly `n` members.
void fill_pools(usize n, usize insts, AlphaStore &store, lib::Library &library,
                const AlphaGate &gate) {
  const AlphaMetrics met = passing_metrics();
  for (u64 i = 0; i < n; ++i) {
    const std::vector<f64> pnl = synth_pnl(i);
    const std::vector<f64> pos = synth_pos(i, insts);

    // O(N) exact pool.
    const auto sid = store.insert(nullptr, pnl, pos, met);
    // Sink to a mutable local so the non-deprecated DoNotOptimize(T&) overload is
    // selected (the const-ref overload is deprecated under -Werror).
    bool sink_ok = sid.has_value();
    benchmark::DoNotOptimize(sink_ok);

    // O(neighbors) library pool: admit through the facade (the corr index grows).
    lib::Provenance prov;
    prov.expr_source = "synthetic_" + std::to_string(i);
    prov.seed = i;
    lib::AlphaCandidate cand{};
    cand.canon_hash = 0xD15EA5Eu + i; // distinct keys (never dedup)
    cand.pnl = std::span<const f64>{pnl};
    cand.pos_flat = std::span<const f64>{pos};
    cand.metrics = met;
    cand.prov = prov;
    cand.as_of = 0;
    cand.source = nullptr;
    const lib::AdmitVerdict v = library.admit(cand, gate);
    auto sink_kind = v.kind;
    benchmark::DoNotOptimize(sink_kind);
  }
}

// ===========================================================================
//  (B1) BM_AlphaStorePoolWorstCorr — the O(N) exact MAX |corr| scan.
//
//  state.range(0) == pool size. The pool is built ONCE (paused); only the
//  per-candidate worst_corr probe is timed. items_per_second == probes/sec.
// ===========================================================================
void BM_AlphaStorePoolWorstCorr(benchmark::State &state) {
  const usize n = static_cast<usize>(state.range(0));
  constexpr usize kInsts = 8;

  AlphaStore store;
  lib::Library library = lib::Library::open(bench_tmpdir("storepool_" + std::to_string(n)),
                                            GateConfig{}, {0xC0FFEEu});
  // Relaxed corr gate so every synthetic candidate is admitted to reach n members.
  GateConfig relaxed;
  relaxed.max_pool_corr = 1.0;
  const AlphaGate fill_gate{relaxed};
  fill_pools(n, kInsts, store, library, fill_gate);

  const AlphaStorePool view{store};
  const std::vector<f64> probe = synth_pnl(0xBEEFu); // a fixed off-pool candidate
  const std::span<const f64> probe_span{probe};

  for (auto _ : state) {
    f64 worst = view.worst_corr(probe_span);
    benchmark::DoNotOptimize(worst);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["pool_size"] = static_cast<double>(n);
}
BENCHMARK(BM_AlphaStorePoolWorstCorr)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024)
    ->Arg(4096)
    ->Unit(benchmark::kMicrosecond);

// ===========================================================================
//  (B2) BM_LibraryPoolWorstCorr — the O(neighbors) SimHash MAX |corr| scan.
//
//  The same pool sizes + the same probe candidate as (B1), so the two benchmarks'
//  measured times are directly comparable. items_per_second == probes/sec.
// ===========================================================================
void BM_LibraryPoolWorstCorr(benchmark::State &state) {
  const usize n = static_cast<usize>(state.range(0));
  constexpr usize kInsts = 8;

  AlphaStore store;
  lib::Library library = lib::Library::open(bench_tmpdir("libpool_" + std::to_string(n)),
                                            GateConfig{}, {0xC0FFEEu});
  GateConfig relaxed;
  relaxed.max_pool_corr = 1.0;
  const AlphaGate fill_gate{relaxed};
  fill_pools(n, kInsts, store, library, fill_gate);

  const LibraryPool view{library};
  const std::vector<f64> probe = synth_pnl(0xBEEFu); // the SAME off-pool candidate as (B1)
  const std::span<const f64> probe_span{probe};

  for (auto _ : state) {
    f64 worst = view.worst_corr(probe_span);
    benchmark::DoNotOptimize(worst);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["pool_size"] = static_cast<double>(n);
}
BENCHMARK(BM_LibraryPoolWorstCorr)
    ->Arg(64)
    ->Arg(256)
    ->Arg(1024)
    ->Arg(4096)
    ->Unit(benchmark::kMicrosecond);

} // namespace
