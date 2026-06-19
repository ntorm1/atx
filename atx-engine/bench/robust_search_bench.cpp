// robust_search_bench.cpp — the S4.5 robust-search throughput + front-growth
// benchmark. MEASURED-ONLY: no production code; the ONLY asserts are non-vacuity
// (so the bench can never silently no-op). Mirrors research_bench.cpp /
// factory_bench.cpp's Google Benchmark form (BENCHMARK macros; the bench/CMakeLists
// globs *_bench.cpp into atx-engine-bench — no CMake edit).
//
// Three headline measurements:
//
//   (A) SORT THROUGHPUT — BM_SearchScalar vs BM_SearchMultiObjective. One
//       SearchDriver::run over a fixed (population x generations) budget on a
//       planted-edge panel, in ScalarRaw (the p1 baseline: total order by raw
//       fitness) vs MultiObjective (NSGA-II non-dominated sort + crowding, the
//       O(N^2) per-generation overhead). Swept over population {16, 32}. Reports
//       candidates scored / sec (the headline) + the run trial_count. The two
//       benchmarks are directly comparable — same fixture, same budget, only the
//       objective_mode differs, so the gap IS the multi-objective sort cost.
//
//   (B) ADMITTED-ROBUST/HOUR — BM_AdmittedRobustRate. One RobustResearchDriver::run
//       over a planted-edge panel into a fresh temp-dir library, reporting the
//       robust survivors admitted per second (extrapolable to per-hour). The robust
//       pipeline gate is ON, so this is the genuine admitted-robust rate.
//
//   (C) PARETO-FRONT SIZE OVER GENERATIONS — BM_ParetoFrontGrowth. fast_nondominated_
//       sort over a synthetic two-objective population that GROWS its trade-off
//       frontier as the generation index rises, reporting front-0 size as a counter.
//       Swept over a "generation" axis so the front-size trajectory (grow then
//       stabilize as the frontier saturates the population) is readable in the
//       counters column.
//
// Build is Debug / clang-cl (the project default), so every figure here is an UPPER
// BOUND on cost, never an optimised / release number. A short min-time / iteration
// cap keeps each Debug case well under a second.

#include <algorithm>  // std::min (front-size saturation cap)
#include <cmath>      // std::sin (synthetic frontier objectives)
#include <cstdint>    // std::uint64_t (the deterministic LCG state)
#include <filesystem> // per-iteration temp dir for the robust pipeline library
#include <span>
#include <string>
#include <system_error> // std::error_code (bench_tmpdir's remove_all/create_directories)
#include <utility>      // std::move
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp" // f64, u16, u64, usize

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp" // alpha::Library (its definition site)
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp"  // combine::AlphaGate, GateConfig
#include "atx/engine/combine/store.hpp" // combine::AlphaStore

#include "atx/engine/factory/pareto.hpp"          // factory::ObjMatrix, fast_nondominated_sort
#include "atx/engine/factory/robust_pipeline.hpp" // factory::RobustResearchDriver, RobustPipelineConfig
#include "atx/engine/factory/search_driver.hpp" // factory::SearchDriver, SearchConfig, ObjectiveMode

#include "atx/engine/library/library.hpp" // library::Library

namespace {

using atx::f64;
using atx::u16;
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
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::fast_nondominated_sort;
using atx::engine::factory::ObjectiveMode;
using atx::engine::factory::ObjMatrix;
using atx::engine::factory::RobustPipelineConfig;
using atx::engine::factory::RobustReport;
using atx::engine::factory::RobustResearchDriver;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;

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

// Planted-edge momentum close [dates*insts] (matches the integration fixtures).
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
  return {"rank(close)",
          "rank(rev)",
          "ts_mean(close, 5)",
          "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))",
          "delta(close, 2)"};
}

[[nodiscard]] SearchConfig search_cfg(usize pop, usize gens, ObjectiveMode mode) {
  SearchConfig cfg;
  cfg.master_seed = 0x5EEDu;
  cfg.population = pop;
  cfg.generations = gens;
  cfg.elites = 2;
  cfg.k_tournament = 3;
  cfg.p_cross = 0.5;
  cfg.enable_behavioral_novelty = (mode == ObjectiveMode::MultiObjective);
  cfg.objective_mode = mode;
  cfg.fitness.trial_count = 4;
  return cfg;
}

// ===========================================================================
//  (A) BM_SearchScalar / BM_SearchMultiObjective — the sort-overhead sweep.
//
//  state.range(0) == population. A fixed 4-generation budget on a 96x6 planted
//  panel. items_per_second == candidates scored / sec. The two share the fixture +
//  budget — only objective_mode differs — so the throughput gap is the NSGA-II cost.
// ===========================================================================
void run_search_bench(benchmark::State &state, ObjectiveMode mode) {
  const usize pop = static_cast<usize>(state.range(0));
  constexpr usize kGens = 4;

  const Library dsl{};
  const Panel panel = planted_panel(96, 6);
  const WeightPolicy policy{};
  const ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver{dsl, panel, policy, sim, seed_exprs(), {"close", "rev"}};
  const AlphaStore pool{};
  const SearchConfig cfg = search_cfg(pop, kGens, mode);

  std::int64_t total_scored = 0;
  usize last_trials = 0;
  for (auto _ : state) {
    const SearchResult r = driver.run(cfg, pool);
    usize sink = r.trial_count;
    benchmark::DoNotOptimize(sink);
    benchmark::ClobberMemory();
    total_scored += static_cast<std::int64_t>(r.trial_count);
    last_trials = r.trial_count;
  }
  // Non-vacuity: a real-signal search MUST score candidates (never a silent no-op).
  if (last_trials == 0U) {
    state.SkipWithError("robust_search_bench: search scored zero candidates (vacuous)");
    return;
  }
  state.SetItemsProcessed(total_scored);
  state.counters["trials"] = static_cast<double>(last_trials);
}

void BM_SearchScalar(benchmark::State &state) { run_search_bench(state, ObjectiveMode::ScalarRaw); }
BENCHMARK(BM_SearchScalar)->Arg(16)->Arg(32)->Unit(benchmark::kMillisecond);

void BM_SearchMultiObjective(benchmark::State &state) {
  run_search_bench(state, ObjectiveMode::MultiObjective);
}
BENCHMARK(BM_SearchMultiObjective)->Arg(16)->Arg(32)->Unit(benchmark::kMillisecond);

// ===========================================================================
//  (B) BM_AdmittedRobustRate — admitted-robust survivors / sec (-> per hour).
//
//  One RobustResearchDriver::run per iteration into a FRESH temp-dir library on a
//  planted-edge panel, the robust pipeline gate ON. items_per_second == robust
//  survivors / sec; "admitted_per_sec" the persistent-library admits / sec.
// ===========================================================================
[[nodiscard]] std::string bench_tmpdir(const std::string &tag) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s4_5_robust_bench" / tag;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  (void)ec;
  std::filesystem::create_directories(dir, ec);
  (void)ec;
  return dir.string();
}

[[nodiscard]] RobustPipelineConfig robust_pipeline_cfg() {
  RobustPipelineConfig cfg;
  FactoryConfig per_run;
  per_run.search.master_seed = 0;
  per_run.search.population = 16;
  per_run.search.generations = 4;
  per_run.search.elites = 2;
  per_run.search.k_tournament = 3;
  per_run.search.p_cross = 0.5;
  per_run.search.enable_behavioral_novelty = true;
  per_run.search.objective_mode = ObjectiveMode::MultiObjective;
  per_run.search.fitness.trial_count = 4;
  per_run.seed_exprs = seed_exprs();
  per_run.panel_fields = {"close", "rev"};
  per_run.min_dsr = 0.5;
  cfg.research.per_run = per_run;
  cfg.research.max_runs = 3;
  cfg.research.patience = 0;
  cfg.research.master_seed = 0x5EEDu;
  cfg.research.robustness_gate = true;
  cfg.research.robustness_cfg.vol_window = 10;
  cfg.research.robustness_cfg.min_regime_sharpe = -1.0; // permit a real (mildly fragile) edge
  cfg.lockbox_frac = 0.20;
  cfg.book.single.gross_leverage = 1.0;
  cfg.book.single.max_iters = 16;
  cfg.book.trade_rate = 1.0;
  return cfg;
}

void BM_AdmittedRobustRate(benchmark::State &state) {
  const Library dsl{};
  const Panel panel = planted_panel(120, 8);
  const WeightPolicy policy{};
  const ExecutionSimulator sim = frictionless_sim();
  const AlphaGate gate{GateConfig{}};
  const RobustPipelineConfig cfg = robust_pipeline_cfg();

  std::int64_t total_robust = 0;
  std::int64_t total_admitted = 0;
  usize last_robust = 0;
  int run_idx = 0;
  for (auto _ : state) {
    state.PauseTiming();
    lib::Library library = lib::Library::open(bench_tmpdir("rp_" + std::to_string(run_idx++)),
                                              GateConfig{}, {0xC0FFEEu});
    state.ResumeTiming();

    RobustResearchDriver pipe{library, dsl, panel, sim, policy, gate};
    auto rep = pipe.run(cfg);
    const usize robust = rep.has_value() ? rep->robust_size : 0U;
    const usize admitted = rep.has_value() ? rep->research.total_admitted : 0U;
    usize sink = robust;
    benchmark::DoNotOptimize(sink);
    benchmark::ClobberMemory();
    total_robust += static_cast<std::int64_t>(robust);
    total_admitted += static_cast<std::int64_t>(admitted);
    last_robust = robust;
  }
  // Non-vacuity: the planted-edge pipeline MUST admit at least one robust survivor.
  if (last_robust == 0U) {
    state.SkipWithError(
        "robust_search_bench: planted-edge pipeline admitted zero robust (vacuous)");
    return;
  }
  state.SetItemsProcessed(total_robust);
  state.counters["admitted_per_sec"] =
      benchmark::Counter(static_cast<double>(total_admitted), benchmark::Counter::kIsRate);
}
BENCHMARK(BM_AdmittedRobustRate)->Unit(benchmark::kMillisecond);

// ===========================================================================
//  (C) BM_ParetoFrontGrowth — Pareto front-0 size over a growing frontier.
//
//  A synthetic two-objective population whose trade-off frontier WIDENS as the
//  "generation" index (state.range(0)) rises: gen g seeds g+1 non-dominated trade-off
//  points (a concave (wq, diversify) frontier) plus a block of dominated interior
//  points. fast_nondominated_sort recovers the front; the "front0_size" counter rises
//  with g then SATURATES once the frontier fills the fixed population — the grow-then-
//  stabilize trajectory the search explores. items_per_second == sorts / sec.
// ===========================================================================
void BM_ParetoFrontGrowth(benchmark::State &state) {
  const usize gen = static_cast<usize>(state.range(0));
  constexpr usize kPop = 24; // the fixed population the frontier saturates into

  // Build the objective matrix ONCE (outside the timed loop). The first `n_front`
  // points lie on a concave frontier (mutually non-dominated); the rest are dominated.
  const usize n_front = std::min(gen + 1U, kPop);
  std::vector<f64> data(kPop * 2U, 0.0);
  for (usize i = 0; i < kPop; ++i) {
    if (i < n_front) {
      // A concave (wq, diversify) trade-off: as wq falls, diversify rises — no point
      // dominates another, so all n_front co-occupy front 0.
      const f64 theta =
          (n_front == 1U) ? 0.0 : static_cast<f64>(i) / static_cast<f64>(n_front - 1U);
      data[i * 2U + 0U] = std::sin(1.5707963267948966 * (1.0 - theta)); // wq
      data[i * 2U + 1U] = std::sin(1.5707963267948966 * theta);         // diversify
    } else {
      // A dominated interior point (strictly inside the frontier box).
      data[i * 2U + 0U] = 0.10;
      data[i * 2U + 1U] = 0.10;
    }
  }
  const ObjMatrix obj{std::span<const f64>{data}, kPop, 2U};
  std::vector<usize> canon(kPop);
  for (usize i = 0; i < kPop; ++i) {
    canon[i] = i;
  }

  usize front0 = 0;
  for (auto _ : state) {
    const std::vector<u16> fronts = fast_nondominated_sort(obj, std::span<const usize>{canon});
    usize cnt = 0;
    for (const u16 f : fronts) {
      if (f == 0U) {
        ++cnt;
      }
    }
    benchmark::DoNotOptimize(cnt);
    benchmark::ClobberMemory();
    front0 = cnt;
  }
  // Non-vacuity: front 0 must hold the planted frontier (never an empty / no-op sort).
  if (front0 == 0U) {
    state.SkipWithError("robust_search_bench: nondominated sort produced an empty front 0");
    return;
  }
  state.SetItemsProcessed(state.iterations());
  state.counters["front0_size"] = static_cast<double>(front0);
  state.counters["generation"] = static_cast<double>(gen);
}
// The sweep starts at gen 2 (n_front >= 3): a pure concave frontier with < 3 points
// has no member that dominates the interior block on BOTH axes, so front 0 would
// (correctly, but uninterestingly) hold the whole population. From gen 2 on the
// frontier mid-points dominate the interior and the front-0 size traces the intended
// grow-then-saturate trajectory (3 -> 5 -> 9 -> 17 -> 24, capped at the population).
BENCHMARK(BM_ParetoFrontGrowth)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->Arg(16)
    ->Arg(32)
    ->Unit(benchmark::kNanosecond);

} // namespace
