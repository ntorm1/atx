// ts_column_parallel_bench.cpp — p7 S3-3: intra-eval cross-instrument column
// parallelism for the batch Ts path (eval_time_series).
//
// Measures the WALL-CLOCK throughput of ONE Engine::evaluate of a Ts-heavy program
// as the column DetPool worker count sweeps {1,2,4}. The panel is 256 dates x 512
// instruments (instruments >> dates — the regime where column parallelism wins:
// many independent columns, each O(dates*d) of work). The ROW ratios across the
// worker counts ARE the speedup curve (no ideal-speedup claim). The DetPool here
// is the engine's intra-eval column pool (set_ts_pool), SEPARATE from any
// search-level pool — exactly the AuditExact, digest-invariant path S3-3 proves.
//
// UseRealTime: column parallelism is a wall-clock win, not summed CPU-time. Build
// is Release / clang-cl when run from the build-bench dir. The Engine is warmed
// once per worker count so the timed region is steady-state (scratch pre-sized).

#include <cstdint>
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

constexpr atx::usize kDates = 256;
constexpr atx::usize kInstruments = 512;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] Program compile_prog(std::string_view src) {
  auto ast = parse_program(src, shared_lib());
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

[[nodiscard]] const Panel &shared_panel() {
  static const Panel p = make_panel(kDates, kInstruments, 0xC0117ULL);
  return p;
}

// A Ts-heavy multi-root program (the batch column-extract path): a unary scan,
// a sort-based rank, and a binary correlation, all at d=20.
[[nodiscard]] const Program &shared_prog() {
  static const Program prog = compile_prog("s = ts_std(close, 20)\n"
                                           "r = ts_rank(close, 20)\n"
                                           "c = correlation(close, volume, 20)\n");
  return prog;
}

// One Engine::evaluate with a column pool of `w` workers. Arg(0) = worker count.
void BM_TsColumnEval(benchmark::State &state) {
  const Program &prog = shared_prog();
  if (prog.roots.empty()) {
    state.SkipWithError("Ts-heavy program failed to compile");
    return;
  }
  const Panel &panel = shared_panel();
  const std::size_t w = static_cast<std::size_t>(state.range(0));
  DetPool pool{w};

  Engine engine{panel};
  engine.set_ts_pool(&pool);
  auto warm = engine.evaluate(prog); // size pool scratch + per-worker buffers once
  if (!warm) {
    state.SkipWithError("warm evaluate failed");
    return;
  }
  for (auto _ : state) {
    auto out = engine.evaluate(prog);
    benchmark::DoNotOptimize(out);
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<std::int64_t>(kDates * kInstruments * prog.roots.size()));
  state.counters["workers"] = static_cast<double>(w);
}
BENCHMARK(BM_TsColumnEval)->Arg(1)->Arg(2)->Arg(4)->Unit(benchmark::kMillisecond)->UseRealTime();

} // namespace
