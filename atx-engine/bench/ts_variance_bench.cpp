// ts_variance_bench.cpp — p7 S3-1/S3-2: variance-family kernel throughput,
// batch (AuditExact) vs Welford/Neumaier online (ResearchFast).
//
// Each BM_*Batch evaluates a single variance-family op under EvalMode::AuditExact
// (the O(T*W) per-window batch recompute, the oracle-bit-exact path); each
// BM_*Welford evaluates the SAME op under EvalMode::ResearchFast (the O(T) Welford
// online column sweep). The pair's ratio is the kernel speedup the sprint claims;
// the ledger records it with host/build context. Panel is a fixed-seed 512x256
// (the same shape as alpha_batch_bench) so the d=20 window is well inside the
// 512 dates. Build is Release / clang-cl when run via the build-bench dir.
//
// Timed region: the WARM Engine::evaluate (the Engine is warmed once so the slot
// pool + scratch are already sized — the dispatch loop allocates nothing). The
// op + window are fixed per benchmark; only the EvalMode differs across the pair.

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

namespace {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::EvalMode;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;

constexpr atx::usize kDates = 512;
constexpr atx::usize kInstruments = 256;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] Program compile_one(std::string_view src) {
  auto ast = parse_expr(src, shared_lib());
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
// Volume is at 1e4..1e6 magnitude so the variance kernel is exercised on the same
// high-magnitude regime the catastrophic-cancellation revert reacted to.
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
  static const Panel p = make_panel(kDates, kInstruments, 0xBE0C0DEULL);
  return p;
}

// Time a single variance-family expression under one EvalMode. The Engine is
// warmed once before the timed loop so the steady-state kernel cost is measured.
void run_variance_bench(benchmark::State &state, std::string_view expr, EvalMode mode) {
  const Program prog = compile_one(expr);
  if (prog.roots.empty()) {
    state.SkipWithError("variance expr failed to compile");
    return;
  }
  const Panel &panel = shared_panel();
  Engine engine{panel};
  engine.set_eval_mode(mode);
  auto warm = engine.evaluate(prog);
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
                          static_cast<std::int64_t>(kDates * kInstruments));
  state.counters["mode"] = (mode == EvalMode::ResearchFast) ? 1.0 : 0.0;
}

void BM_TsVarBatch(benchmark::State &state) {
  run_variance_bench(state, "ts_std(close, 20)", EvalMode::AuditExact);
}
BENCHMARK(BM_TsVarBatch)->Unit(benchmark::kMillisecond);

void BM_TsVarWelford(benchmark::State &state) {
  run_variance_bench(state, "ts_std(close, 20)", EvalMode::ResearchFast);
}
BENCHMARK(BM_TsVarWelford)->Unit(benchmark::kMillisecond);

void BM_TsZscoreBatch(benchmark::State &state) {
  run_variance_bench(state, "ts_zscore(close, 20)", EvalMode::AuditExact);
}
BENCHMARK(BM_TsZscoreBatch)->Unit(benchmark::kMillisecond);

void BM_TsZscoreWelford(benchmark::State &state) {
  run_variance_bench(state, "ts_zscore(close, 20)", EvalMode::ResearchFast);
}
BENCHMARK(BM_TsZscoreWelford)->Unit(benchmark::kMillisecond);

void BM_TsAvDiffBatch(benchmark::State &state) {
  run_variance_bench(state, "ts_av_diff(close, 20)", EvalMode::AuditExact);
}
BENCHMARK(BM_TsAvDiffBatch)->Unit(benchmark::kMillisecond);

void BM_TsAvDiffWelford(benchmark::State &state) {
  run_variance_bench(state, "ts_av_diff(close, 20)", EvalMode::ResearchFast);
}
BENCHMARK(BM_TsAvDiffWelford)->Unit(benchmark::kMillisecond);

} // namespace
