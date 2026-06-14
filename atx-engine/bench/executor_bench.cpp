// executor_bench.cpp — S7.6 (C): IExecutor wall-clock throughput / speedup across
// worker counts {1,2,4,8} on BOTH substrates (ThreadExecutor + ProcessExecutor),
// over a representative real workload (parallel_backtests).
//
// WHAT IT MEASURES: the wall-clock time to run one parallel_backtests of a fixed
// AlphaStreams fan across each (substrate, worker-count) cell, reported two ways:
//
//   * Google Benchmark rows (BM_Executor/<substrate>/<workers>) for the standard
//     JSON/console table — UseRealTime (the metric is wall-clock speedup, not
//     summed CPU time), Debug/clang-cl so the absolute figures are UPPER BOUNDS.
//   * a one-shot human-readable SPEEDUP / KNEE table printed at static-init time
//     (g_speedup_report): for each substrate, the per-worker wall time, the speedup
//     vs. its own 1-worker baseline, and the KNEE (the worker count past which
//     speedup gains <5%). The thread baseline doubles as the cross-substrate
//     reference. This is the "small speedup/knee table" the done-gate asks for.
//
// IT IS NOT A CORRECTNESS GATE: it does not ASSERT (it is a benchmark). But it is
// DETERMINISTIC IN WHAT IT COMPUTES — the fixed-seed streams + fixed fan make every
// cell's result_table_digest identical, and the report PRINTS whether all digests
// agreed (a one-line self-check) so it doubles as a smoke test. Only the TIMING is
// clock-derived; no clock/RNG/thread-id ever enters a digest (the bench reads the
// chrono clock ONLY for its timing print — a benchmark-timing path, never a
// determinism-contract path).
//
// The ProcessExecutor cells spawn atx-shm-worker (it lands in the same build/bin/),
// so this bench, like the process tests, depends on that exe being built.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/streams.hpp"

#include "atx/engine/parallel/executor.hpp"
#include "atx/engine/parallel/parallel_run.hpp"
#include "atx/engine/parallel/process_executor.hpp"
#include "atx/engine/parallel/thread_executor.hpp"

namespace {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::engine::alpha::AlphaStreams;
using atx::engine::parallel::ExecutorConfig;
using atx::engine::parallel::IExecutor;
using atx::engine::parallel::parallel_backtests;
using atx::engine::parallel::ProcessExecutor;
using atx::engine::parallel::result_table_digest;
using atx::engine::parallel::ThreadExecutor;

constexpr f64 kBook = 1.0e6;
constexpr usize kAlphas = 256;     // a fan wide enough to amortise spawn over shards
constexpr usize kPeriods = 96;     // per-alpha stream length
constexpr usize kInstruments = 8;  // position cross-section width

// The worker-count sweep the done-gate names. 0 (substrate default) is NOT included
// — the speedup curve is against an explicit 1-worker baseline.
constexpr std::array<usize, 4> kWorkerCounts = {1, 2, 4, 8};

// A fixed-seed synthetic AlphaStreams (no <random>, never clocked — fully
// deterministic, so every cell's result digest is identical by construction).
[[nodiscard]] AlphaStreams make_streams(usize n_alphas, usize n_periods, usize n_inst,
                                        std::uint64_t seed) {
  AlphaStreams s;
  s.n_alphas_ = n_alphas;
  s.n_periods_ = n_periods;
  s.n_instruments_ = n_inst;
  s.pnl_flat.resize(n_alphas * n_periods);
  s.pos_flat.resize(n_alphas * n_periods * n_inst);
  std::uint64_t st = seed | 1ULL;
  auto nx = [&st] {
    st = st * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<double>(st >> 11) / static_cast<double>(1ULL << 53);
  };
  for (auto &v : s.pnl_flat) {
    v = nx() * 0.04 - 0.02;
  }
  for (auto &v : s.pos_flat) {
    v = nx() * 2.0 - 1.0;
  }
  return s;
}

// The shared fixture: one fixed AlphaStreams, built ONCE.
[[nodiscard]] const AlphaStreams &fixture() {
  static const AlphaStreams s = make_streams(kAlphas, kPeriods, kInstruments, 0xB17E5ULL);
  return s;
}

// ---------------------------------------------------------------------------
//  Google Benchmark rows: one cell per (substrate, worker-count). Arg(0) selects
//  the substrate (0 = thread, 1 = process); Arg(1) is the worker count.
// ---------------------------------------------------------------------------
void BM_Executor(benchmark::State &state) {
  const AlphaStreams &streams = fixture();
  const bool process = state.range(0) != 0;
  const auto w = static_cast<usize>(state.range(1));

  for (auto _ : state) {
    if (process) {
      ProcessExecutor pe{ExecutorConfig{w, false}};
      auto rows = parallel_backtests(streams, kBook, pe);
      benchmark::DoNotOptimize(rows);
    } else {
      ThreadExecutor te{ExecutorConfig{w, false}};
      auto rows = parallel_backtests(streams, kBook, te);
      benchmark::DoNotOptimize(rows);
    }
    benchmark::ClobberMemory();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(streams.n_alphas()));
  state.counters["workers"] = static_cast<double>(w);
  state.counters["substrate"] = process ? 1.0 : 0.0; // 0 = thread, 1 = process
}
BENCHMARK(BM_Executor)
    ->Args({0, 1})
    ->Args({0, 2})
    ->Args({0, 4})
    ->Args({0, 8})
    ->Args({1, 1})
    ->Args({1, 2})
    ->Args({1, 4})
    ->Args({1, 8})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

// ---------------------------------------------------------------------------
//  One-shot speedup / knee table (printed once at static init). Times each
//  (substrate, worker) cell directly with steady_clock and prints the speedup vs
//  the substrate's own 1-worker baseline plus the knee. Also self-checks that every
//  cell produced the SAME result digest (the determinism smoke test).
// ---------------------------------------------------------------------------

// Median wall time (ms) of `reps` parallel_backtests on a fresh `make` executor,
// and the result digest (identical across cells by construction). Median over a few
// reps damps the Debug-build / scheduler jitter without needing a warmup harness.
template <typename MakeExec>
[[nodiscard]] double time_ms(MakeExec make, const AlphaStreams &streams, int reps, u64 &digest_out) {
  std::vector<double> samples;
  samples.reserve(static_cast<std::size_t>(reps));
  u64 digest = 0;
  for (int r = 0; r < reps; ++r) {
    auto exec = make();
    const auto t0 = std::chrono::steady_clock::now();
    auto rows = parallel_backtests(streams, kBook, *exec);
    const auto t1 = std::chrono::steady_clock::now();
    digest = result_table_digest(rows); // same every rep/cell (determinism)
    samples.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
  }
  std::sort(samples.begin(), samples.end());
  digest_out = digest;
  return samples[samples.size() / 2];
}

// The knee: the smallest worker count past which the marginal speedup vs the
// previous count is < 5% (i.e. extra workers stop paying). Returns the last count
// if the curve never flattens within the sweep.
[[nodiscard]] usize knee_of(const std::array<double, 4> &speedup) {
  for (std::size_t i = 1; i < kWorkerCounts.size(); ++i) {
    const double prev = speedup[i - 1];
    const double gain = prev > 0.0 ? (speedup[i] - prev) / prev : 0.0;
    if (gain < 0.05) {
      return kWorkerCounts[i - 1];
    }
  }
  return kWorkerCounts.back();
}

void print_substrate_row(const char *name, const std::array<double, 4> &ms,
                         const std::array<u64, 4> &digests, bool &seen, u64 &first_digest,
                         bool &digests_agree) {
  std::array<double, 4> speedup{};
  for (std::size_t i = 0; i < kWorkerCounts.size(); ++i) {
    speedup[i] = ms[0] > 0.0 ? ms[0] / ms[i] : 0.0;
    // A dedicated `seen` flag (NOT first_digest==0) is the correct sentinel: a
    // legitimate digest CAN be 0, so testing against 0 would mis-seed the baseline.
    if (!seen) {
      seen = true;
      first_digest = digests[i];
    } else if (digests[i] != first_digest) {
      digests_agree = false;
    }
  }
  std::cout << "[ bench    ] " << std::left << std::setw(16) << name;
  for (std::size_t i = 0; i < kWorkerCounts.size(); ++i) {
    std::cout << "  w" << kWorkerCounts[i] << "=" << std::fixed << std::setprecision(2) << ms[i]
              << "ms(" << std::setprecision(2) << speedup[i] << "x)";
  }
  std::cout << "  knee=w" << knee_of(speedup) << "\n";
}

struct SpeedupReport {
  SpeedupReport() {
    const AlphaStreams &streams = fixture();
    constexpr int kReps = 5;

    std::array<double, 4> thread_ms{};
    std::array<double, 4> proc_ms{};
    std::array<u64, 4> thread_dg{};
    std::array<u64, 4> proc_dg{};
    for (std::size_t i = 0; i < kWorkerCounts.size(); ++i) {
      const usize w = kWorkerCounts[i];
      thread_ms[i] = time_ms([w] { return std::make_unique<ThreadExecutor>(ExecutorConfig{w, false}); },
                             streams, kReps, thread_dg[i]);
      proc_ms[i] = time_ms([w] { return std::make_unique<ProcessExecutor>(ExecutorConfig{w, false}); },
                           streams, kReps, proc_dg[i]);
    }

    bool seen = false;
    u64 first_digest = 0;
    bool digests_agree = true;
    std::cout << "[ bench    ] Executor speedup/knee (parallel_backtests, " << kAlphas
              << " alphas x " << kPeriods << "p x " << kInstruments
              << "i, Debug build; ms = median of " << kReps << " reps):\n";
    print_substrate_row("ThreadExecutor", thread_ms, thread_dg, seen, first_digest, digests_agree);
    print_substrate_row("ProcessExecutor", proc_ms, proc_dg, seen, first_digest, digests_agree);
    // Non-vacuity note: a 0 digest over 256 alphas would itself be suspicious, so
    // flag it — an all-zero "AGREE" must not read as a healthy smoke-test pass.
    std::cout << "[ bench    ]   determinism self-check: all 8 cells "
              << (digests_agree ? "AGREE" : "DISAGREE") << " on result digest "
              << "(0x" << std::hex << first_digest << std::dec << ")"
              << (first_digest == 0 ? " [WARN: zero digest — check the fixture]" : "") << "\n";
  }
};
const SpeedupReport g_speedup_report{};

} // namespace
