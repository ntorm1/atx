// alpha_batch_bench.cpp — P3c-1 batch / cross-alpha evaluation throughput + CSE
// lever (mined-style battery).
//
// A ~24-alpha mined battery with HEAVY subexpression overlap (rank(close),
// ts_mean(close,5), correlation(close,volume,10), ts_std(close,10), …) is
// compiled ONCE into a single hash-consed Program (cross-alpha CSE) and evaluated
// warm over a fixed 512×256 panel. Reported:
//   * unique/total AST-node ratio (the CSE-lever — how much the cons table folds);
//   * intern cache-hit % (Program::cache_hit_pct — the dual telemetry);
//   * alphas/s and ns/cell (warm Engine::evaluate throughput).
//
// The compile-side CSE metrics are constant per battery; they are echoed once to
// stdout AND attached as benchmark counters so they ride in the JSON/CSV. The
// timed region is the warm `Engine::evaluate` (the batch path — it returns one
// SignalSet::Alpha per root). Build is Debug / clang-cl (the project default), so
// the throughput figures are UPPER-BOUND latencies, NOT the optimised number;
// host/build context is recorded by Google Benchmark's own header.

#include <cstdint>
#include <iostream>
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

namespace {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;

constexpr atx::usize kDates = 512;
constexpr atx::usize kInstruments = 256;

// The mined high-overlap battery: 24 named alphas reusing a handful of shared
// subexpressions. Verbatim from the P3-9 proof bench so the CSE lever is
// comparable across sprints.
[[nodiscard]] std::string_view mined_battery() {
  return "m01 = rank(close)\n"
         "m02 = rank(close) - rank(open)\n"
         "m03 = rank(close) * ts_mean(close, 5)\n"
         "m04 = ts_mean(close, 5) - close\n"
         "m05 = correlation(close, volume, 10)\n"
         "m06 = correlation(close, volume, 10) * rank(close)\n"
         "m07 = ts_mean(close, 5) / (ts_mean(volume, 5) + 1)\n"
         "m08 = rank(close) + correlation(close, volume, 10)\n"
         "m09 = ts_std(close, 10) * rank(close)\n"
         "m10 = ts_mean(close, 5) + ts_mean(close, 5)\n"
         "m11 = delta(close, 2) * rank(close)\n"
         "m12 = correlation(close, volume, 10) - ts_mean(close, 5)\n"
         "m13 = rank(volume) * rank(close)\n"
         "m14 = scale(ts_mean(close, 5) - close, 1)\n"
         "m15 = ts_std(close, 10) + ts_std(close, 10)\n"
         "m16 = rank(close) * rank(close)\n"
         "m17 = correlation(close, volume, 10) / (ts_std(close, 10) + 1)\n"
         "m18 = ts_mean(close, 5) * ts_mean(volume, 5)\n"
         "m19 = rank(close - open) + ts_mean(close, 5)\n"
         "m20 = delta(close, 2) + delta(close, 2)\n"
         "m21 = rank(close) - ts_std(close, 10)\n"
         "m22 = correlation(close, volume, 10) * ts_mean(volume, 5)\n"
         "m23 = ts_mean(close, 5) - ts_mean(volume, 5)\n"
         "m24 = rank(close) * correlation(close, volume, 10)\n";
}

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Compile the battery to one Program (parse_program -> analyze -> compile). On
// any stage failure returns a default Program (roots empty) — the bench guards.
[[nodiscard]] Program compile_battery() {
  auto ast = parse_program(mined_battery(), shared_lib());
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

// Warm batch evaluation of the whole mined battery (one Engine::evaluate per
// iteration over the fixed panel — the Engine is warm after the first call, so
// the dispatch loop allocates nothing). Counters carry the CSE lever + throughput.
void BM_BatchEvaluate_MinedBattery(benchmark::State &state) {
  const Program prog = compile_battery();
  if (prog.roots.empty()) {
    state.SkipWithError("mined battery failed to compile");
    return;
  }
  const Panel panel = make_panel(kDates, kInstruments, 0xBE0C0DEULL);
  const atx::usize cells = kDates * kInstruments;
  const auto num_alphas = static_cast<atx::usize>(prog.roots.size());

  Engine engine{panel};
  auto warm = engine.evaluate(prog); // size the SlotPool + scratch once
  if (!warm) {
    state.SkipWithError("warm evaluate failed");
    return;
  }

  for (auto _ : state) {
    auto out = engine.evaluate(prog);
    benchmark::DoNotOptimize(out);
    benchmark::ClobberMemory();
  }

  // Throughput: one "item" == one alpha-cell (alphas * cells per evaluate).
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(num_alphas * cells));
  // CSE-lever telemetry as counters (constant per battery; echoed for the JSON).
  state.counters["unique_nodes"] = static_cast<double>(prog.unique_nodes);
  state.counters["total_ast_nodes"] = static_cast<double>(prog.total_ast_nodes);
  state.counters["unique_over_total"] =
      prog.total_ast_nodes == 0
          ? 0.0
          : static_cast<double>(prog.unique_nodes) / static_cast<double>(prog.total_ast_nodes);
  state.counters["cache_hit_pct"] = prog.cache_hit_pct();
  state.counters["num_slots"] = static_cast<double>(prog.num_slots);
  state.counters["alphas"] = static_cast<double>(num_alphas);
}
BENCHMARK(BM_BatchEvaluate_MinedBattery)->Unit(benchmark::kMillisecond);

// One-time stdout echo of the compile-side CSE lever (Google Benchmark counters
// are per-row; this prints the human-readable summary the ledger captures).
struct CseReport {
  CseReport() {
    const Program prog = compile_battery();
    const double ratio = prog.total_ast_nodes == 0 ? 0.0
                                                   : static_cast<double>(prog.unique_nodes) /
                                                         static_cast<double>(prog.total_ast_nodes);
    std::cout << "[ bench    ] AlphaBatch mined-battery CSE lever (Debug build):\n"
              << "[ bench    ]   panel            = " << kDates << " x " << kInstruments << " ("
              << (kDates * kInstruments) << " cells)\n"
              << "[ bench    ]   alphas           = " << prog.roots.size() << "\n"
              << "[ bench    ]   unique_nodes     = " << prog.unique_nodes << "\n"
              << "[ bench    ]   total_ast_nodes  = " << prog.total_ast_nodes << "\n"
              << "[ bench    ]   unique/total     = " << ratio << "\n"
              << "[ bench    ]   cache_hits       = " << prog.cache_hits << "\n"
              << "[ bench    ]   intern_attempts  = " << prog.intern_attempts << "\n"
              << "[ bench    ]   cache_hit_pct    = " << prog.cache_hit_pct() << "\n"
              << "[ bench    ]   num_slots        = " << prog.num_slots << "\n";
  }
};
const CseReport g_cse_report{};

} // namespace
