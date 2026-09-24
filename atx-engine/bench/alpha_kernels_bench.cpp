// alpha_kernels_bench.cpp — Lane 1 rolling / cross-sectional kernel throughput.
//
// Panel: 512 dates x 3000 instruments (fixed-seed LCG, never clocked). Every
// benchmark reports `ns_per_cell` (wall ns per output cell of ONE op).
//
//   * BM_Engine/<expr>/<mode>: a warm Engine::evaluate of a single-op program
//     (LoadField copies included), AuditExact (0) vs ResearchFast (1). ts_rank /
//     rank route through the new order-statistic / radix kernels in BOTH modes;
//     decay_linear routes through the sliding lane under ResearchFast only.
//   * BM_Kernel*: the raw Lane 1 kernels on the panel buffer — sweep_comoment /
//     sweep_unary (date-outer, all instruments) and the order-stat column sweep —
//     vs the batch per-cell kernels they replace. corr/cov are measured here
//     because the VM's pair dispatch (vm.hpp) is not yet wired to them.
//   * BM_StreamingStep/<lookback>/<mode>: one StreamingEngine::step over a
//     5-alpha battery after warm(lookback); time_per_alpha_day.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/cs_ops.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streaming_engine.hpp"
#include "atx/engine/alpha/ts_ops.hpp"
#include "atx/engine/alpha/ts_order_stat.hpp"
#include "atx/engine/alpha/ts_sliding.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atx_bench_l1_kernels {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::CrossSection;
using atx::engine::alpha::Engine;
using atx::engine::alpha::EvalMode;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::alpha::StreamingEngine;
namespace det = atx::engine::alpha::detail;
namespace sliding = atx::engine::alpha::sliding;
namespace ordstat = atx::engine::alpha::ordstat;

constexpr atx::usize kDates = 512;
constexpr atx::usize kInst = 3000;
constexpr atx::usize kCells = kDates * kInst;

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
  return compile(ast.value(), ana.value()).value_or(Program{});
}

struct Cols {
  std::vector<atx::f64> close;
  std::vector<atx::f64> volume;
};

// Random-walk prices ~1e2 and volume ~1e6 (the cancellation-prone regime).
[[nodiscard]] const Cols &shared_cols() {
  static const Cols c = [] {
    Cols out;
    out.close.resize(kCells);
    out.volume.resize(kCells);
    std::uint64_t s = 0x5EED1234ULL;
    auto next = [&s]() noexcept {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      return static_cast<atx::f64>(s >> 11) / static_cast<atx::f64>(1ULL << 53);
    };
    std::vector<atx::f64> px(kInst, 100.0);
    for (atx::usize t = 0; t < kDates; ++t) {
      for (atx::usize j = 0; j < kInst; ++j) {
        px[j] += next() - 0.5;
        out.close[t * kInst + j] = px[j];
        out.volume[t * kInst + j] = 1.0e6 * (0.5 + next());
      }
    }
    return out;
  }();
  return c;
}

[[nodiscard]] const Panel &shared_panel() {
  static const Panel p = [] {
    const Cols &c = shared_cols();
    auto r = Panel::create(kDates, kInst, {"close", "volume"}, {c.close, c.volume}, {});
    return r.value();
  }();
  return p;
}

void set_ns_per_cell(benchmark::State &state) {
  state.counters["ns_per_cell"] = benchmark::Counter(
      static_cast<double>(kCells), benchmark::Counter::kIsIterationInvariantRate |
                                       benchmark::Counter::kInvert);
}

void engine_bench(benchmark::State &state, const std::string &expr, EvalMode mode) {
  const Program prog = compile_one(expr);
  if (prog.roots.empty()) {
    state.SkipWithError("compile failed");
    return;
  }
  Engine eng{shared_panel()};
  eng.set_eval_mode(mode);
  if (!eng.evaluate(prog)) {
    state.SkipWithError("warm evaluate failed");
    return;
  }
  for (auto _ : state) {
    auto out = eng.evaluate(prog);
    benchmark::DoNotOptimize(out);
  }
  set_ns_per_cell(state);
}

// ---- raw kernels -----------------------------------------------------------

void BM_KernelComomentSliding(benchmark::State &state) {
  const auto op = static_cast<OpCode>(state.range(0));
  const auto d = static_cast<atx::usize>(state.range(1));
  const Cols &c = shared_cols();
  std::vector<atx::f64> out(kCells);
  for (auto _ : state) {
    sliding::sweep_comoment(op, c.close, c.volume, out, kDates, kInst, d, 0, kInst);
    benchmark::DoNotOptimize(out.data());
  }
  set_ns_per_cell(state);
}

void BM_KernelComomentBatch(benchmark::State &state) {
  const auto op = static_cast<OpCode>(state.range(0));
  const auto d = static_cast<atx::usize>(state.range(1));
  const Cols &c = shared_cols();
  std::vector<atx::f64> out(kCells);
  std::vector<atx::f64> colx(kDates);
  std::vector<atx::f64> coly(kDates);
  std::vector<atx::f64> sa(d);
  std::vector<atx::f64> sb(d);
  for (auto _ : state) {
    // The VM's AuditExact shape: column extract + per-cell ts_pair_at.
    for (atx::usize j = 0; j < kInst; ++j) {
      for (atx::usize t = 0; t < kDates; ++t) {
        colx[t] = c.close[t * kInst + j];
        coly[t] = c.volume[t * kInst + j];
      }
      for (atx::usize t = 0; t < kDates; ++t) {
        out[t * kInst + j] = det::ts_pair_at(op, colx, coly, t, 0, d, 1, sa, sb);
      }
    }
    benchmark::DoNotOptimize(out.data());
  }
  set_ns_per_cell(state);
}

void BM_KernelUnarySliding(benchmark::State &state) {
  const auto op = static_cast<OpCode>(state.range(0));
  const auto d = static_cast<atx::usize>(state.range(1));
  const Cols &c = shared_cols();
  std::vector<atx::f64> out(kCells);
  for (auto _ : state) {
    sliding::sweep_unary(op, c.close, out, kDates, kInst, d, 0, kInst);
    benchmark::DoNotOptimize(out.data());
  }
  set_ns_per_cell(state);
}

void BM_KernelOrderStat(benchmark::State &state) {
  const auto op = static_cast<OpCode>(state.range(0));
  const auto d = static_cast<atx::usize>(state.range(1));
  const Cols &c = shared_cols();
  std::vector<atx::f64> out(kCells);
  for (auto _ : state) {
    for (atx::usize j = 0; j < kInst; ++j) {
      ordstat::sweep_strided(op, c.close, out, kDates, j, d, kInst);
    }
    benchmark::DoNotOptimize(out.data());
  }
  set_ns_per_cell(state);
}

// The pre-Lane-1 VM shape for ts_rank / med: column extract + per-cell batch.
void BM_KernelOrderStatBatch(benchmark::State &state) {
  const auto op = static_cast<OpCode>(state.range(0));
  const auto d = static_cast<atx::usize>(state.range(1));
  const Cols &c = shared_cols();
  std::vector<atx::f64> out(kCells);
  std::vector<atx::f64> col(kDates);
  std::vector<atx::f64> sa(d);
  for (auto _ : state) {
    for (atx::usize j = 0; j < kInst; ++j) {
      for (atx::usize t = 0; t < kDates; ++t) {
        col[t] = c.close[t * kInst + j];
      }
      for (atx::usize t = 0; t < kDates; ++t) {
        out[t * kInst + j] = det::ts_value_at(op, col, t, 0, d, 1, sa, 0.0);
      }
    }
    benchmark::DoNotOptimize(out.data());
  }
  set_ns_per_cell(state);
}

void BM_KernelCsRank(benchmark::State &state) {
  const Cols &c = shared_cols();
  std::vector<atx::f64> out(kCells);
  std::vector<atx::usize> valid(kInst);
  for (atx::usize j = 0; j < kInst; ++j) {
    valid[j] = j;
  }
  det::CsScratch scratch;
  for (auto _ : state) {
    for (atx::usize t = 0; t < kDates; ++t) {
      const std::span<const atx::f64> row{c.close.data() + t * kInst, kInst};
      det::cs_rank_row(row, valid, std::span<atx::f64>{out.data() + t * kInst, kInst}, scratch);
    }
    benchmark::DoNotOptimize(out.data());
  }
  set_ns_per_cell(state);
}

// The pre-Lane-1 cs_rank_row body (stable_sort by value) for comparison.
void BM_KernelCsRankStableSort(benchmark::State &state) {
  const Cols &c = shared_cols();
  std::vector<atx::f64> out(kCells);
  std::vector<atx::usize> order(kInst);
  for (auto _ : state) {
    for (atx::usize t = 0; t < kDates; ++t) {
      const atx::f64 *row = c.close.data() + t * kInst;
      for (atx::usize j = 0; j < kInst; ++j) {
        order[j] = j;
      }
      std::stable_sort(order.begin(), order.end(),
                       [row](atx::usize a, atx::usize b) { return row[a] < row[b]; });
      for (atx::usize r = 0; r < kInst; ++r) {
        out[t * kInst + order[r]] = static_cast<atx::f64>(r) / static_cast<atx::f64>(kInst - 1);
      }
    }
    benchmark::DoNotOptimize(out.data());
  }
  set_ns_per_cell(state);
}
constexpr auto kCorr = static_cast<std::int64_t>(OpCode::TsCorr);
constexpr auto kCov = static_cast<std::int64_t>(OpCode::TsCov);
constexpr auto kDecay = static_cast<std::int64_t>(OpCode::TsDecayLinear);
constexpr auto kSlope = static_cast<std::int64_t>(OpCode::TsSlope);
constexpr auto kRank = static_cast<std::int64_t>(OpCode::TsRank);
constexpr auto kMed = static_cast<std::int64_t>(OpCode::TsMed);

BENCHMARK(BM_KernelComomentSliding)
    ->ArgsProduct({{kCorr, kCov}, {10, 20, 60}})
    ->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KernelComomentBatch)->ArgsProduct({{kCorr}, {10, 20, 60}})->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KernelUnarySliding)
    ->ArgsProduct({{kDecay, kSlope}, {10, 20, 60}})
    ->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KernelOrderStat)
    ->ArgsProduct({{kRank, kMed}, {10, 20, 60, 250}})
    ->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KernelOrderStatBatch)
    ->ArgsProduct({{kRank, kMed}, {10, 20, 60, 250}})
    ->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KernelCsRank)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KernelCsRankStableSort)->Unit(benchmark::kMillisecond);

// ---- engine-level ----------------------------------------------------------

[[maybe_unused]] const bool kEngineRegistered = [] {
  const char *exprs[] = {
      "correlation(close, volume, 20)", "covariance(close, volume, 20)",
      "decay_linear(close, 10)",        "decay_linear(close, 20)",
      "decay_linear(close, 60)",        "ts_rank(close, 10)",
      "ts_rank(close, 20)",             "ts_rank(close, 60)",
      "med(close, 20)",                 "slope(close, 20)",
      "rank(close)",
  };
  for (const char *e : exprs) {
    for (const EvalMode m : {EvalMode::AuditExact, EvalMode::ResearchFast}) {
      const std::string name = std::string("BM_Engine/") + e +
                               (m == EvalMode::AuditExact ? "/audit" : "/fast");
      benchmark::RegisterBenchmark(name.c_str(),
                                   [expr = std::string(e), m](benchmark::State &st) {
                                     engine_bench(st, expr, m);
                                   })
          ->Unit(benchmark::kMillisecond);
    }
  }
  return true;
}();

// ---- streaming -------------------------------------------------------------

// A WQ101-flavoured battery over close/volume covering every streaming routing
// class (lookback, running sum, deque extreme, order stat, generic pair window,
// cross-sectional rank).
constexpr std::string_view kStreamBattery =
    "a1 = -1 * correlation(rank(close), rank(volume), 10)\n"
    "a2 = -1 * ts_rank(rank(close), 9)\n"
    "a3 = rank(close - ts_sum(close, 10) / 10)\n"
    "a4 = (0 < ts_min(delta(close, 1), 5)) ? delta(close, 1) : -1 * delta(close, 1)\n"
    "a5 = decay_linear(rank(volume), 20) * stddev(close, 20)\n";

[[nodiscard]] Program compile_program(std::string_view src) {
  auto ast = atx::engine::alpha::parse_program(src, shared_lib());
  if (!ast) {
    return Program{};
  }
  auto ana = analyze(ast.value());
  if (!ana) {
    return Program{};
  }
  return compile(ast.value(), ana.value()).value_or(Program{});
}

// BM_StreamingStep/<lookback>/<mode>: warm over `lookback` dates, then time one
// step() per iteration (dates cycle through the remaining panel rows; the state
// keeps advancing, so every step is a genuine live-day update). Reports
// time_per_alpha_day. Acceptance: lookback 250 vs 60 within 10%.
void BM_StreamingStep(benchmark::State &state) {
  const auto lookback = static_cast<atx::usize>(state.range(0));
  const auto mode = state.range(1) == 0 ? EvalMode::AuditExact : EvalMode::ResearchFast;
  const Program prog = compile_program(kStreamBattery);
  if (prog.roots.empty()) {
    state.SkipWithError("compile failed");
    return;
  }
  const Cols &c = shared_cols();
  std::vector<std::vector<atx::f64>> cols;
  for (const std::string &f : prog.fields) {
    const std::vector<atx::f64> &src = f == "close" ? c.close : c.volume;
    cols.emplace_back(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(lookback * kInst));
  }
  auto panel = Panel::create(lookback, kInst, prog.fields, std::move(cols), {});
  auto se = StreamingEngine::create(prog, static_cast<atx::u32>(kInst), mode);
  if (!panel || !se || !se->warm(panel.value())) {
    state.SkipWithError("streaming setup failed");
    return;
  }
  CrossSection cs;
  cs.fields.resize(prog.fields.size());
  atx::usize t = lookback;
  for (auto _ : state) {
    for (atx::usize i = 0; i < prog.fields.size(); ++i) {
      const std::vector<atx::f64> &src = prog.fields[i] == "close" ? c.close : c.volume;
      cs.fields[i] = std::span<const atx::f64>{src.data() + t * kInst, kInst};
    }
    auto r = se->step(cs);
    benchmark::DoNotOptimize(r);
    t = t + 1 < kDates ? t + 1 : lookback;
  }
  state.counters["time_per_alpha_day"] = benchmark::Counter(
      static_cast<double>(prog.roots.size()),
      benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}
BENCHMARK(BM_StreamingStep)
    ->ArgsProduct({{60, 250}, {0, 1}})
    ->Unit(benchmark::kMicrosecond);

} // namespace atx_bench_l1_kernels
