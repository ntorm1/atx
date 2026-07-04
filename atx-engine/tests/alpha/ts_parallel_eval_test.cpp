// atx::engine::alpha — cross-instrument column-parallel Ts eval tests (p7 S3-3).
//
// S3-3 wraps the batch instrument loop in eval_time_series in a DetPool dispatch
// over instrument-column bands, so a Ts-heavy genome on a wide panel uses multiple
// cores WITHIN one Engine::evaluate. The contract is AuditExact: each output cell
// out[t*I+j] depends only on input column j, no two columns share mutable state
// (each worker owns its own column-extract + window scratch), so parallel dispatch
// is bit-identical to serial. This file is the seq==parallel digest GATE — not a
// proof-by-argument: it evaluates the same Ts-heavy program with the pool null
// (serial), a 2-worker pool, and a 4-worker pool, and asserts every f64 cell is
// byte-identical (NaN==NaN).
//
// The DetPool used here is a SEPARATE instance from any search-level pool (nesting
// two DetPool dispatches risks deadlock — see Engine::set_ts_pool). The Engine is
// AuditExact throughout (column parallelism does NOT need ResearchFast).
//
// Naming: Subject_Condition_ExpectedResult.

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/parallel/det_pool.hpp"

namespace atxtest_ts_parallel_eval {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
using atx::engine::parallel::DetPool;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

[[nodiscard]] bool same_cell(atx::f64 a, atx::f64 b) noexcept {
  return (std::isnan(a) && std::isnan(b)) || a == b;
}

// Compile a multi-root program (parse_program -> analyze -> compile).
[[nodiscard]] Program compile_ok(std::string_view src) {
  auto ast = parse_program(src, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments, std::uint64_t seed) {
  const atx::usize cells = dates * instruments;
  std::vector<std::string> names = {"close", "open", "high", "low", "volume", "IndClass.sector"};
  std::mt19937_64 rng{seed};
  std::uniform_real_distribution<atx::f64> price{1.0, 100.0};
  std::uniform_real_distribution<atx::f64> vol{1.0, 1.0e6};
  std::uniform_int_distribution<int> sector{0, 2};
  std::vector<std::vector<atx::f64>> cols(6, std::vector<atx::f64>(cells));
  for (atx::usize i = 0; i < cells; ++i) {
    cols[0][i] = price(rng);
    cols[1][i] = price(rng);
    cols[2][i] = price(rng);
    cols[3][i] = price(rng);
    cols[4][i] = vol(rng);
    cols[5][i] = static_cast<atx::f64>(sector(rng));
  }
  auto p = Panel::create(dates, instruments, std::move(names), std::move(cols), {});
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// Evaluate `prog` on `panel`; if `pool` is non-null, set it as the Ts column pool
// (intra-eval parallelism). Returns the full SignalSet.
[[nodiscard]] SignalSet eval_with_pool(const Program &prog, const Panel &panel, DetPool *pool) {
  Engine engine{panel};
  engine.set_ts_pool(pool);
  auto out = engine.evaluate(prog);
  EXPECT_TRUE(out.has_value()) << "VM: " << (out ? "" : out.error().message());
  return out.value_or(SignalSet{});
}

// Assert two SignalSets are byte-identical (every f64 cell, NaN==NaN).
void expect_signal_sets_identical(const SignalSet &a, const SignalSet &b, std::string_view label) {
  ASSERT_EQ(a.alphas.size(), b.alphas.size()) << label;
  ASSERT_EQ(a.dates, b.dates) << label;
  ASSERT_EQ(a.instruments, b.instruments) << label;
  for (atx::usize r = 0; r < a.alphas.size(); ++r) {
    ASSERT_EQ(a.alphas[r].values.size(), b.alphas[r].values.size()) << label << " root " << r;
    for (atx::usize i = 0; i < a.alphas[r].values.size(); ++i) {
      ASSERT_TRUE(same_cell(a.alphas[r].values[i], b.alphas[r].values[i]))
          << label << " root " << r << " cell " << i << ": a=" << a.alphas[r].values[i]
          << " b=" << b.alphas[r].values[i];
    }
  }
}

// A Ts-heavy multi-root program: a batch unary (TsStd), a batch sort/scan op
// (TsRank), and a batch binary op (TsCorr). All three exercise the column-extract
// batch path that S3-3 parallelizes.
[[nodiscard]] std::string ts_heavy_program() {
  return "s = ts_std(close, 20)\n"
         "r = ts_rank(close, 20)\n"
         "c = correlation(close, volume, 20)\n";
}

// ===========================================================================
//  Seq == parallel digest identity at {null, 2-worker, 4-worker}.
// ===========================================================================

TEST(TsColumnParallel, SeqEqualsParallel_DigestIdentical) {
  const atx::usize dates = 600;
  const atx::usize instruments = 501;
  const Panel panel = make_panel(dates, instruments, 0xC01D5EEDULL);
  const Program prog = compile_ok(ts_heavy_program());
  ASSERT_FALSE(prog.roots.empty());

  const SignalSet serial = eval_with_pool(prog, panel, nullptr);
  {
    DetPool pool{2};
    const SignalSet par2 = eval_with_pool(prog, panel, &pool);
    expect_signal_sets_identical(serial, par2, "serial vs 2-worker");
  }
  {
    DetPool pool{4};
    const SignalSet par4 = eval_with_pool(prog, panel, &pool);
    expect_signal_sets_identical(serial, par4, "serial vs 4-worker");
  }
}

// Null-pool path is unchanged: an Engine with ts_pool_==nullptr (the default)
// produces the same output as one explicitly set to nullptr — and matches the
// 1-worker pool (a single band is the serial loop).
TEST(TsColumnParallel, NullPoolMatchesOneWorker) {
  const atx::usize dates = 120;
  const atx::usize instruments = 64;
  const Panel panel = make_panel(dates, instruments, 0xFEED5EEDULL);
  const Program prog = compile_ok(ts_heavy_program());
  ASSERT_FALSE(prog.roots.empty());

  const SignalSet null_pool = eval_with_pool(prog, panel, nullptr);
  DetPool pool{1};
  const SignalSet one_worker = eval_with_pool(prog, panel, &pool);
  expect_signal_sets_identical(null_pool, one_worker, "null vs 1-worker");
}

// Default-constructed Engine has a null Ts pool (single-threaded inert default).
TEST(TsColumnParallel, DefaultEngineHasNullTsPool) {
  const Panel panel = make_panel(8, 4, 0x1ULL);
  Engine engine{panel};
  EXPECT_EQ(engine.ts_pool(), nullptr);
}

// A single-instrument panel takes the serial path even with a pool (instruments>1
// guard); output is still correct (matches the null-pool eval).
TEST(TsColumnParallel, SingleInstrument_PoolNoOp) {
  const atx::usize dates = 40;
  const atx::usize instruments = 1;
  const Panel panel = make_panel(dates, instruments, 0xA11CEULL);
  const Program prog = compile_ok("s = ts_std(close, 10)\n");
  ASSERT_FALSE(prog.roots.empty());

  const SignalSet serial = eval_with_pool(prog, panel, nullptr);
  DetPool pool{4};
  const SignalSet par = eval_with_pool(prog, panel, &pool);
  expect_signal_sets_identical(serial, par, "single-instrument serial vs pool");
}

} // namespace atxtest_ts_parallel_eval
