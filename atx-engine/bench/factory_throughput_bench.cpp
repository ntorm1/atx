// factory_throughput_bench.cpp — L3 search-factory throughput (BM_SearchThroughput).
//
// Measures SearchDriver::run end to end on a planted-edge random-walk panel and
// sweeps the L3 levers against the unchanged baseline:
//   mode 0  baseline            (all L3 flags off — today's driver)
//   mode 1  + semantic canon    (bit-exact rewrite before hashing)
//   mode 2  + output dedup      (rank-quantized fingerprint reuses prior scores)
//   mode 3  + fidelity race, conservative ladder {4x2, 2x1, full}
//   mode 4  + fidelity race, default ladder {4x4, 2x2, full}
// state.range(1) is the DetPool worker count.
//
// Counters:
//   genomes_per_sec   candidates generated (children produced + scored or
//                     deduped) per wall second — the search's raw throughput;
//   trials_per_sec    distinct candidates LOOKED AT per second (trial_count);
//   fitness_calls_per_sec candidates that paid the full fitness pass;
//   dedup_pct         1 - trial_count / candidates_generated (structural+semantic);
//   fp_hit_pct        fingerprint-reused representatives / trial_count;
//   fid_reject_pct    rung-rejected / trial_count;
//   best_raw          final generation's best raw fitness (the quality guard).
//
// Panel size: ATX_L3_BENCH_DATES x ATX_L3_BENCH_INSTS (env), default 756 x 500
// (the plan's 2500 x 3000 is reachable via env; it needs several GB and minutes
// per run, so it is not the default on the shared 16 GB lane box).

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atx_bench_l3_factory_throughput {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaStore;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;

[[nodiscard]] usize env_or(const char *name, usize fallback) {
  std::string v;
#if defined(_MSC_VER) || defined(_WIN32)
  char *e = nullptr;
  std::size_t len = 0;
  if (_dupenv_s(&e, &len, name) == 0 && e != nullptr) {
    v = e;
  }
  free(e); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s heap copy
#else
  if (const char *e = std::getenv(name); e != nullptr) {
    v = e;
  }
#endif
  if (v.empty()) {
    return fallback;
  }
  const long long n = std::strtoll(v.c_str(), nullptr, 10);
  return n > 0 ? static_cast<usize>(n) : fallback;
}

[[nodiscard]] ExecutionSimulator zero_cost() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

// Planted-edge walk: bounded per-instrument drift (+-20 bps) and 1% noise.
[[nodiscard]] Panel planted(usize dates, usize insts) {
  std::vector<f64> close(dates * insts);
  std::vector<f64> rev(dates * insts, 0.0);
  std::vector<f64> vol(dates * insts);
  std::vector<f64> px(insts, 100.0);
  std::uint64_t s = 0x7A11ULL;
  auto uni = [&]() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return 2.0 * (static_cast<f64>(s >> 11U) / static_cast<f64>(1ULL << 53U)) - 1.0;
  };
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 drift = 0.002 * static_cast<f64>(static_cast<long long>(j % 21U) - 10) / 10.0;
      px[j] *= 1.0 + drift + 0.01 * uni();
      close[t * insts + j] = px[j];
      vol[t * insts + j] = 1.0e6 * (1.5 + uni());
      if (t > 0) {
        rev[t * insts + j] = -(px[j] / close[(t - 1) * insts + j] - 1.0);
      }
    }
  }
  auto r = Panel::create(dates, insts, {"close", "rev", "volume"}, {close, rev, vol}, {});
  return std::move(r.value());
}

[[nodiscard]] SearchConfig cfg_for(usize mode, usize workers) {
  SearchConfig cfg;
  cfg.master_seed = 20260922;
  cfg.population = 32;
  cfg.generations = 6;
  cfg.n_workers = workers;
  cfg.stagnation_patience = 0;
  cfg.canon.semantic = mode >= 1;
  cfg.output_dedup = mode >= 2;
  cfg.fidelity.enabled = mode >= 3;
  if (mode == 3) { // conservative ladder: 1/8 of the cells, then 1/2, then full
    cfg.fidelity.rungs = {{atx::engine::factory::Rung{4, 2, 0},
                           atx::engine::factory::Rung{2, 1, 0},
                           atx::engine::factory::Rung{1, 1, 0}}};
  }
  return cfg;
}

void BM_SearchThroughput(benchmark::State &state) {
  const usize mode = static_cast<usize>(state.range(0));
  const usize workers = static_cast<usize>(state.range(1));
  static const Panel panel =
      planted(env_or("ATX_L3_BENCH_DATES", 756), env_or("ATX_L3_BENCH_INSTS", 500));
  const Library lib{};
  const WeightPolicy policy{};
  const ExecutionSimulator sim = zero_cost();
  const SearchConfig cfg = cfg_for(mode, workers);
  const std::vector<std::string> seeds{"rank(close)",       "rank(rev)",
                                       "ts_mean(close, 5)", "ts_mean(rev, 3)",
                                       "rank(ts_mean(close, 10))", "delta(close, 2)",
                                       "rank(volume)",      "ts_rank(close, 10)"};

  f64 generated = 0.0;
  f64 trials = 0.0;
  f64 fp_hits = 0.0;
  f64 rejected = 0.0;
  f64 dedup = 0.0;
  f64 best = 0.0;
  for (auto _ : state) {
    SearchDriver driver{lib, panel, policy, sim, seeds, {"close", "rev", "volume"}};
    const AlphaStore pool{};
    SearchResult r = driver.run(cfg, pool);
    benchmark::DoNotOptimize(r.digest);
    generated += static_cast<f64>(r.candidates_generated);
    trials += static_cast<f64>(r.trial_count);
    fp_hits += static_cast<f64>(r.fingerprint_hits);
    rejected += static_cast<f64>(r.fidelity_rejected);
    dedup += r.dedup_pct;
    best = r.best_fitness_per_gen.empty() ? 0.0 : r.best_fitness_per_gen.back();
  }
  const f64 iters = static_cast<f64>(state.iterations());
  using C = benchmark::Counter;
  state.counters["genomes_per_sec"] = C(generated, C::kIsRate);
  state.counters["trials_per_sec"] = C(trials, C::kIsRate);
  state.counters["fitness_calls_per_sec"] = C(trials - fp_hits - rejected, C::kIsRate);
  state.counters["dedup_pct"] = iters > 0.0 ? dedup / iters : 0.0;
  state.counters["fp_hit_pct"] = trials > 0.0 ? fp_hits / trials : 0.0;
  state.counters["fid_reject_pct"] = trials > 0.0 ? rejected / trials : 0.0;
  state.counters["best_raw"] = best;
}
BENCHMARK(BM_SearchThroughput)
    ->ArgsProduct({{0, 1, 2, 3, 4}, {1, 4, 8}})
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1)
    ->UseRealTime();

} // namespace atx_bench_l3_factory_throughput
