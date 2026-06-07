// parallel_bench.cpp — S2-4: parallel_evaluate throughput vs worker count.
//
// Measures the WALL-CLOCK throughput of parallel_evaluate (the batch-eval fan,
// S2-2) over a fixed panel + a fixed per-program fan of ~128 cheap alphas, as the
// DetPool worker count sweeps {1, 2, 4, 8} plus the pool default (0 -> cores-2).
// Reported as alphas/s (SetItemsProcessed = iterations * #programs) with a
// `workers` counter on each row, so the speedup curve is the ratio of the items/s
// columns across the worker-count rows. No ideal-speedup claim is made — the ROW
// numbers ARE the actual speedup curve (strategy-A recompute + Debug overhead +
// pool/dispenser contention all priced in).
//
// UseRealTime: the parallel metric is wall-clock speedup, not summed CPU-time
// (which barely moves with worker count). Build is Debug / clang-cl (the project
// default — same as alpha_batch_bench.cpp), so the absolute figures are
// UPPER-BOUND latencies, NOT the optimised number; host/build context is recorded
// by Google Benchmark's own header. The fixture (panel + compiled fan) is built
// ONCE in a function-local static, so the timed region is purely the parallel
// evaluate (no compile / no panel-gen cost leaks in).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/parallel/batch_eval.hpp"
#include "atx/engine/parallel/det_pool.hpp"

namespace {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::parallel::DetPool;
using atx::engine::parallel::parallel_evaluate;

constexpr atx::usize kDates = 256;
constexpr atx::usize kInstruments = 128;
constexpr std::size_t kAlphas = 128;

[[nodiscard]] const Library& shared_lib() {
  static const Library lib;
  return lib;
}

// A fixed-seed synthetic panel (no <random>, never clocked — fully deterministic).
[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments, std::uint64_t seed) {
  const atx::usize cells = dates * instruments;
  std::vector<std::string> names = {"open", "high", "low", "close", "volume", "vwap", "returns"};
  std::vector<std::vector<atx::f64>> cols(names.size(), std::vector<atx::f64>(cells));

  std::uint64_t state = seed | 1ULL;
  auto next = [&state]() noexcept -> atx::f64 {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<atx::f64>(state >> 11) / static_cast<atx::f64>(1ULL << 53);
  };
  for (atx::usize i = 0; i < cells; ++i) {
    const atx::f64 base = 10.0 + next() * 190.0;
    const atx::f64 spread = next() * 5.0;
    const atx::f64 hi = base + spread;
    const atx::f64 lo = base - spread;
    cols[0][i] = base;
    cols[1][i] = hi;
    cols[2][i] = lo;
    cols[3][i] = lo + (hi - lo) * 0.5;
    cols[4][i] = 1.0e4 + next() * 9.9e5;
    cols[5][i] = (hi + lo + cols[3][i]) / 3.0;
    cols[6][i] = next() * 0.1 - 0.05;
  }
  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});

  auto p = Panel::create(dates, instruments, names, cols, universe);
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// >= 128 distinct cheap element-wise alphas, with a few shared subexpressions —
// the same worst-case shape the determinism suite stresses (kept cheap so the
// bench runs fast). Each is a single NAMED root `a{k} = <expr>` so the fan's
// roots are a0..aN. The expression strings are owned by the caller's vector.
[[nodiscard]] std::vector<std::string> big_battery(std::size_t n) {
  std::vector<std::string> v;
  v.reserve(n);
  for (std::size_t k = 0; k < n; ++k) {
    const int c = static_cast<int>(k % 17) + 1;
    switch (k % 5) {
      case 0:
        v.push_back("close - open + " + std::to_string(c));
        break;
      case 1:
        v.push_back("(high - low) * " + std::to_string(c));
        break;
      case 2:
        v.push_back("close / open - " + std::to_string(c));
        break;
      case 3:
        v.push_back("volume - close * " + std::to_string(c));
        break;
      default:
        v.push_back("(high + low) / 2 - vwap + " + std::to_string(c));
        break;
    }
  }
  return v;
}

[[nodiscard]] Program compile_named(std::size_t k, std::string_view src) {
  const std::string text = "a" + std::to_string(k) + " = " + std::string(src);
  auto ast = parse_program(text, shared_lib());
  if (!ast) {
    return Program{};
  }
  auto ana = analyze(ast.value());
  if (!ana) {
    return Program{};
  }
  auto prog = compile(ast.value(), ana.value());
  return prog.value_or(Program{});
}

// The shared fixture: a fixed panel + the compiled per-program fan, built ONCE.
struct Fixture {
  Panel panel;
  std::vector<Program> progs;
};

[[nodiscard]] const Fixture& fixture() {
  static const Fixture f = [] {
    const std::vector<std::string> strs = big_battery(kAlphas);
    std::vector<Program> progs;
    progs.reserve(strs.size());
    for (std::size_t k = 0; k < strs.size(); ++k) {
      progs.push_back(compile_named(k, strs[k]));
    }
    return Fixture{make_panel(kDates, kInstruments, 0xBE0C0DEULL), std::move(progs)};
  }();
  return f;
}

// parallel_evaluate throughput vs worker count. Arg(0) -> pool default (cores-2).
void BM_ParallelEval(benchmark::State& state) {
  const Fixture& f = fixture();
  if (f.progs.empty() || f.progs.front().roots.empty()) {
    state.SkipWithError("battery failed to compile");
    return;
  }
  const std::size_t w = static_cast<std::size_t>(state.range(0));
  DetPool pool{w};

  // Warm once so the per-worker Engine slot pools have already grown — the timed
  // region is the steady-state parallel evaluate, not first-touch allocation.
  {
    auto warm = parallel_evaluate(f.progs, f.panel, pool);
    benchmark::DoNotOptimize(warm);
  }

  for (auto _ : state) {
    auto ss = parallel_evaluate(f.progs, f.panel, pool);
    benchmark::DoNotOptimize(ss);
    benchmark::ClobberMemory();
  }

  // Throughput: one "item" == one alpha (program) evaluated.
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(f.progs.size()));
  state.counters["workers"] = static_cast<double>(w);
  state.counters["alphas"] = static_cast<double>(f.progs.size());
}
BENCHMARK(BM_ParallelEval)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->Arg(0) // 0 -> pool default = max(1, cores - 2)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();

} // namespace
