// eval_multiple_testing_bench.cpp — Lane 4 (l4-mtest) throughput gates.
//
//  * BM_HansenSpa / BM_RomanoWolf: 1000 candidates × 2520 days × 1000
//    stationary-bootstrap replicates (mean block 10), arg = worker threads.
//    The block-prefix-sum kernel makes one replicate O(K · T / L).
//  * BM_TrialRegistryAppend: 10^6 in-memory records (T = 252, count-sketch
//    d = 16 / 64; exact d = T at 10^5); per-record cost O(T + d²/2).
//  * BM_TrialRegistrySummary: summary() alone on a 10^6-trial registry.
//  * BM_TrialRegistryAppendDurable: 10^4 flushed appends to a temp file.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"
#include "atx/engine/eval/multiple_testing.hpp"
#include "atx/engine/eval/trial_registry.hpp"

namespace atx_bench_l4_mtest {

using atx::f64;
using atx::u64;
using atx::usize;
using namespace atx::engine::eval;

constexpr usize kCandidates = 1000U;
constexpr usize kDays = 2520U;

u64 mix(u64 &s) {
  u64 z = (s += 0x9e3779b97f4a7c15ULL);
  z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31U);
}

f64 gauss(u64 &s) {
  const f64 u1 = (static_cast<f64>(mix(s) >> 11U) + 0.5) * 0x1.0p-53;
  const f64 u2 = (static_cast<f64>(mix(s) >> 11U) + 0.5) * 0x1.0p-53;
  return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
}

const std::vector<f64> &panel() {
  static const std::vector<f64> x = [] {
    std::vector<f64> v(kCandidates * kDays);
    u64 s = 12345U;
    for (f64 &e : v) {
      e = 0.01 * gauss(s);
    }
    return v;
  }();
  return x;
}

void BM_HansenSpa(benchmark::State &state) {
  const std::vector<f64> &x = panel();
  const std::vector<f64> bench(kDays, 0.0);
  BootstrapCfg cfg;
  cfg.n_boot = 1000U;
  cfg.threads = static_cast<usize>(state.range(0));
  for (auto _ : state) {
    auto r = hansen_spa(PnlMatrix{x, kCandidates, kDays}, bench, cfg);
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_HansenSpa)->Arg(1)->Arg(4)->Unit(benchmark::kMillisecond)->Iterations(2);

void BM_RomanoWolf(benchmark::State &state) {
  const std::vector<f64> &x = panel();
  BootstrapCfg cfg;
  cfg.n_boot = 1000U;
  cfg.threads = static_cast<usize>(state.range(0));
  for (auto _ : state) {
    auto r = romano_wolf(PnlMatrix{x, kCandidates, kDays}, cfg, 0.05);
    benchmark::DoNotOptimize(r);
  }
}
BENCHMARK(BM_RomanoWolf)->Arg(1)->Arg(4)->Unit(benchmark::kMillisecond)->Iterations(2);

constexpr usize kRegT = 252U;
constexpr usize kPool = 1024U;

const std::vector<f64> &pnl_pool() {
  static const std::vector<f64> v = [] {
    std::vector<f64> p(kPool * kRegT);
    u64 s = 777U;
    for (f64 &e : p) {
      e = 0.01 * gauss(s);
    }
    return p;
  }();
  return v;
}

TrialRegistry fill(usize n, usize sketch_dim = 64U) {
  TrialRegistryConfig cfg;
  cfg.pnl_len = kRegT;
  cfg.sketch_dim = sketch_dim;
  TrialRegistry reg = std::move(*TrialRegistry::in_memory(cfg));
  const std::vector<f64> &pool = pnl_pool();
  for (usize i = 0; i < n; ++i) {
    const std::span<const f64> x{pool.data() + (i % kPool) * kRegT, kRegT};
    auto r = reg.record(TrialKind::MinerExpr, i, x, 0.001 * static_cast<f64>(i % 97U));
    benchmark::DoNotOptimize(r);
  }
  return reg;
}

void BM_TrialRegistryAppend(benchmark::State &state) {
  const usize n = static_cast<usize>(state.range(0));
  for (auto _ : state) {
    TrialRegistry reg = fill(n, static_cast<usize>(state.range(1)));
    TrialSummary s = reg.summary();
    benchmark::DoNotOptimize(s);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n));
}
BENCHMARK(BM_TrialRegistryAppend)
    ->Args({1000000, 16})
    ->Args({1000000, 64})
    ->Args({100000, 256})
    ->Unit(benchmark::kMillisecond)
    ->Iterations(1);

void BM_TrialRegistrySummary(benchmark::State &state) {
  const TrialRegistry reg = fill(1000000U);
  for (auto _ : state) {
    TrialSummary s = reg.summary();
    benchmark::DoNotOptimize(s);
  }
}
BENCHMARK(BM_TrialRegistrySummary)->Unit(benchmark::kMicrosecond);

void BM_TrialRegistryAppendDurable(benchmark::State &state) {
  const usize n = static_cast<usize>(state.range(0));
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "atx_l4_registry_bench.bin";
  TrialRegistryConfig cfg;
  cfg.pnl_len = kRegT;
  cfg.sketch_dim = 64U;
  const std::vector<f64> &pool = pnl_pool();
  for (auto _ : state) {
    state.PauseTiming();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    state.ResumeTiming();
    TrialRegistry reg = std::move(*TrialRegistry::open(path, cfg));
    for (usize i = 0; i < n; ++i) {
      const std::span<const f64> x{pool.data() + (i % kPool) * kRegT, kRegT};
      auto r = reg.record(TrialKind::MinerExpr, i, x, 0.0);
      benchmark::DoNotOptimize(r);
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(n));
  std::error_code ec;
  std::filesystem::remove(path, ec);
}
BENCHMARK(BM_TrialRegistryAppendDurable)
    ->Arg(10000)
    ->Unit(benchmark::kMillisecond)
    ->Iterations(3);

} // namespace atx_bench_l4_mtest
