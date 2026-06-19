// factory_bench.cpp — throughput micro-benchmark for the S3-6 Factory mine ->
// gate -> admit loop. MEASURED-ONLY: no correctness assertions, no production code.
//
// The factory is a COLD research path (one full evolutionary search + admission per
// mine() call), so this measures the END-TO-END per-RUN cost and reports the four
// throughput counters the sprint plan calls out:
//
//   * evaluated/sec  — distinct candidates SCORED per second (state items == the
//                      run's trial_count; Google Benchmark's items_per_second is
//                      then evaluated/sec).
//   * admitted/hour  — survivors admitted per wall-hour (a derived counter on the
//                      report: admitted / run_seconds * 3600).
//   * dedup%         — 1 - trial_count/candidates_generated (the F6 dedup lever).
//   * cse%           — mean Program::cache_hit_pct() over the run's scored genomes.
//
// The whole fixture (Library / Panel / sim / policy) is built ONCE outside the
// measured loop; only Factory::mine() over a FRESH pool is timed (a fresh pool each
// iteration so the admission path runs from empty, the real per-run cost). The
// panel is the planted-edge momentum random-walk (the S3-4/S3-5 idiom), so the run
// admits a realistic survivor set — the admitted/hour counter is non-trivial.
//
// Build is Debug / clang-cl (the project default), so every figure here is an UPPER
// BOUND on cost, not the optimised number. A short min-time keeps the Debug bench
// well under a second per case.

#include <cstdint> // std::uint64_t (the deterministic LCG state)
#include <string>  // std::string (seed exprs / field names)
#include <utility> // std::move
#include <vector>  // std::vector (synthetic panel storage)

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp" // f64, usize, u64

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp" // alpha::Library (its definition site)
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp"  // combine::AlphaGate, GateConfig
#include "atx/engine/combine/store.hpp" // combine::AlphaStore

#include "atx/engine/factory/factory.hpp" // factory::Factory, FactoryConfig, FactoryReport

namespace {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaGate;
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
using atx::engine::factory::Factory;
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::FactoryReport;

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

[[nodiscard]] FactoryConfig mine_cfg(u64 seed, usize pop, usize gens) {
  FactoryConfig cfg;
  cfg.search.master_seed = seed;
  cfg.search.population = pop;
  cfg.search.generations = gens;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.search.enable_behavioral_novelty = true;
  cfg.search.fitness.trial_count = 4;
  cfg.seed_exprs = seed_exprs();
  cfg.panel_fields = {"close", "rev"};
  cfg.min_dsr = 0.5;
  return cfg;
}

// ===========================================================================
//  BM_FactoryMine — the full mine -> gate -> admit run over a planted-edge panel.
//
//  state.range(0) == population, state.range(1) == generations (the search budget).
//  The Library/Panel/sim/policy are built ONCE; each iteration mines a FRESH pool
//  (admission from empty — the real per-run cost). Counters:
//    items_per_second   -> evaluated (trials) per second.
//    "admitted_per_hour"-> admitted survivors per wall-hour.
//    "dedup_pct"        -> the F6 dedup lever (mean over iterations).
//    "cse_pct"          -> mean Program::cache_hit_pct() over scored genomes.
// ===========================================================================
void BM_FactoryMine(benchmark::State &state) {
  const usize pop = static_cast<usize>(state.range(0));
  const usize gens = static_cast<usize>(state.range(1));

  const Library lib{};
  const Panel panel = planted_panel(120, 8);
  const ExecutionSimulator sim = frictionless_sim();
  const WeightPolicy policy{};
  const AlphaGate gate{GateConfig{}};
  const FactoryConfig cfg = mine_cfg(/*seed*/ 1, pop, gens);

  Factory factory{lib, panel, sim, policy};

  std::int64_t total_trials = 0;
  std::int64_t total_admitted = 0;
  double sum_dedup = 0.0;
  double sum_cse = 0.0;

  for (auto _ : state) {
    AlphaStore pool; // fresh pool per run (admission from empty)
    const FactoryReport rep = factory.mine(cfg, pool, gate);
    // Sink to mutable locals so the non-deprecated DoNotOptimize(T&) overload is
    // selected (the const-ref overload is deprecated under -Werror).
    usize sink_admitted = rep.admitted;
    usize sink_pool = pool.n_alphas();
    benchmark::DoNotOptimize(sink_admitted);
    benchmark::DoNotOptimize(sink_pool);
    benchmark::ClobberMemory();
    total_trials += static_cast<std::int64_t>(rep.trials);
    total_admitted += static_cast<std::int64_t>(rep.admitted);
    sum_dedup += rep.dedup_pct;
    sum_cse += rep.cse_pct;
  }

  const double iters = static_cast<double>(state.iterations());
  // evaluated/sec: total distinct candidates scored across all iterations / time
  // (Google Benchmark reports this as items_per_second).
  state.SetItemsProcessed(total_trials);
  // admitted/sec: Benchmark divides the total by the measured wall-time (kIsRate).
  // The per-HOUR figure the plan asks for is simply this * 3600 in the report.
  state.counters["admitted_per_sec"] =
      benchmark::Counter(static_cast<double>(total_admitted), benchmark::Counter::kIsRate);
  // admitted/hour: pre-scale by 3600 so the headline counter reads directly.
  state.counters["admitted_per_hour"] = benchmark::Counter(
      static_cast<double>(total_admitted) * 3600.0, benchmark::Counter::kIsRate);
  state.counters["dedup_pct"] = (iters > 0.0) ? sum_dedup / iters : 0.0;
  state.counters["cse_pct"] = (iters > 0.0) ? sum_cse / iters : 0.0;
}
BENCHMARK(BM_FactoryMine)->Args({16, 4})->Args({24, 6})->Args({32, 8})->Unit(benchmark::kMillisecond);

} // namespace
