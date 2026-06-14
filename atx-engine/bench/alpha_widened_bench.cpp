// alpha_widened_bench.cpp — S3.6 widened-op evaluation throughput + CSE lever.
//
// A ~16-alpha battery exercising the S3.1–S3.4 widened op surface (cs_residualize,
// the four BRAIN ts_*, quantile/reverse/vec_*) over the S3.3 datafields (vwap /
// dollar_volume / adv20), with deliberate subexpression overlap (quantile(close,5),
// cs_residualize(close, IndClass.sector), ts_regression(close, vwap, 10), …) so the
// hash-consed Program folds them once (cross-alpha CSE). Compiled ONCE and
// evaluated warm over a fixed 256×128 datafields panel. Reported:
//   * unique/total AST-node ratio (the CSE lever);
//   * intern cache-hit % (Program::cache_hit_pct);
//   * alphas/s and ns/cell (warm Engine::evaluate throughput).
//
// Build is Debug / clang-cl (project default), so the throughput figures are
// UPPER-BOUND latencies, NOT the optimised number; host/build context is recorded
// by Google Benchmark's own header.

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/datafields.hpp"
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
namespace df = atx::engine::alpha::datafields;

constexpr atx::usize kDates = 256;
constexpr atx::usize kInstruments = 128;

// The widened-op battery: 16 alphas reusing a handful of shared subexpressions so
// the cross-alpha CSE lever is non-trivial.
[[nodiscard]] std::string_view widened_battery() {
  return "w01 = rank(quantile(close, 5))\n"
         "w02 = vec_avg(close)\n"
         "w03 = cs_residualize(close, IndClass.sector)\n"
         "w04 = cs_residualize(close, IndClass.sector, vwap)\n"
         "w05 = ts_regression(close, vwap, 10)\n"
         "w06 = ts_decay_exp(close, 10, 2.0)\n"
         "w07 = ts_moment(close, 10, 2)\n"
         "w08 = ts_entropy(close, 10, 8)\n"
         "w09 = reverse(quantile(close, 5))\n"
         "w10 = vec_sum(volume) - vec_avg(volume)\n"
         "w11 = rank(close / adv20)\n"
         "w12 = ts_regression(close, vwap, 10) * rank(quantile(close, 5))\n"
         "w13 = cs_residualize(close, IndClass.sector) + vec_avg(close)\n"
         "w14 = scale(ts_decay_exp(close, 10, 2.0) - close, 1)\n"
         "w15 = quantile(close, 5) / (vec_avg(volume) + 1)\n"
         "w16 = ts_moment(close, 10, 2) - ts_entropy(close, 10, 8)\n";
}

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] Program compile_battery() {
  auto ast = parse_program(widened_battery(), shared_lib());
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

// A fixed-seed datafields panel (OHLCV + IndClass.sector then vwap/dollar_volume/
// adv20 derived). No <random>, never clocked — fully deterministic.
[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments, std::uint64_t seed) {
  const atx::usize cells = dates * instruments;
  std::vector<std::vector<atx::f64>> cols(6, std::vector<atx::f64>(cells));
  std::uint64_t state = seed | 1ULL;
  auto next = [&state]() noexcept -> atx::f64 {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<atx::f64>(state >> 11) / static_cast<atx::f64>(1ULL << 53);
  };
  for (atx::usize i = 0; i < cells; ++i) {
    const atx::f64 base = 20.0 + next() * 80.0;
    const atx::f64 spread = 1.0 + next() * 4.0;
    cols[0][i] = base;                         // close
    cols[1][i] = base + (next() - 0.5) * 2.0;  // open
    cols[2][i] = base + spread;                // high
    cols[3][i] = base - spread;                // low
    cols[4][i] = 1.0e4 + next() * 9.0e5;       // volume
    cols[5][i] = static_cast<atx::f64>(i % 4); // IndClass.sector
  }
  std::vector<std::string> names = {"close", "open", "high", "low", "volume", "IndClass.sector"};
  std::vector<atx::u16> adv = {20};
  auto p = df::with_datafields(dates, instruments, std::move(names), std::move(cols), {}, adv);
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

void BM_WidenedEvaluate_Battery(benchmark::State &state) {
  const Program prog = compile_battery();
  if (prog.roots.empty()) {
    state.SkipWithError("widened battery failed to compile");
    return;
  }
  const Panel panel = make_panel(kDates, kInstruments, 0x5311C0DEULL);
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
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(num_alphas * cells));
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
BENCHMARK(BM_WidenedEvaluate_Battery)->Unit(benchmark::kMillisecond);

// One-time stdout echo of the compile-side CSE lever for the ledger.
struct WidenedCseReport {
  WidenedCseReport() {
    const Program prog = compile_battery();
    const double ratio = prog.total_ast_nodes == 0 ? 0.0
                                                   : static_cast<double>(prog.unique_nodes) /
                                                         static_cast<double>(prog.total_ast_nodes);
    std::cout << "[ bench    ] AlphaWidened battery CSE lever (Debug build):\n"
              << "[ bench    ]   panel            = " << kDates << " x " << kInstruments << " ("
              << (kDates * kInstruments) << " cells)\n"
              << "[ bench    ]   alphas           = " << prog.roots.size() << "\n"
              << "[ bench    ]   unique_nodes     = " << prog.unique_nodes << "\n"
              << "[ bench    ]   total_ast_nodes  = " << prog.total_ast_nodes << "\n"
              << "[ bench    ]   unique/total     = " << ratio << "\n"
              << "[ bench    ]   cache_hit_pct    = " << prog.cache_hit_pct() << "\n"
              << "[ bench    ]   num_slots        = " << prog.num_slots << "\n";
  }
};
const WidenedCseReport g_widened_cse_report{};

} // namespace
