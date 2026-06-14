// atx::engine::parallel — parallel_evaluate (S2-2) determinism proof.
//
// parallel_evaluate fans N compiled single-root Programs across DetPool workers,
// each on its OWN stateful Engine (Engine is NOT thread-safe), and assembles the
// roots into ONE SignalSet in program order. The whole point of this suite is the
// DETERMINISM contract:
//
//   1. MatchesSingleThreadDigest — the parallel digest is BYTE-IDENTICAL to the
//      single-thread Engine::evaluate of the equivalent BATCH program. The
//      pre-existing alpha_batch_test proves batch==singly cell-for-cell (CSE
//      changes SHARING, not the math/bits), so digest(parallel over per-root
//      progs) == digest(Engine::evaluate(batch prog)). This is the load-bearing
//      assertion.
//   2. ThreadCountInvariant — the digest is invariant across worker counts
//      {1,2,4,8}: scheduling order can never perturb the bits (each k writes only
//      its own disjoint output slots; no cross-worker FP accumulation).
//   3. EmptyProgramListYieldsEmptySignalSet — degenerate empty input still yields
//      a correctly-shaped (dates,instruments) empty SignalSet.
//   4. SingleRootMatchesEngine — a one-program fan equals a direct Engine eval.
//   5. ErrorPropagatesLowestIndex — an evaluate error (a Cs*/Ts* NotImplemented
//      op) surfaces as Err; with multiple failing programs the LOWEST-index error
//      wins (deterministic failure).
//
// Naming: Subject_Condition_ExpectedResult is not used here (gtest test names are
// descriptive verbs) — the suite is `ParallelBatchEval`.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/parallel/batch_eval.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/digest.hpp"

namespace atxtest_parallel_batch_eval_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::compile_batch;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
using atx::engine::parallel::DetPool;
using atx::engine::parallel::parallel_evaluate;
using atx::engine::parallel::signal_set_digest;

// Process-lifetime Library: the Ast borrows OpSig pointers from it, so it must
// outlive every parse result.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// ---- synthetic panel (a trimmed mirror of alpha_batch_test.cpp's generator) -

[[nodiscard]] Panel make_panel(atx::usize dates, atx::usize instruments, std::uint64_t seed) {
  const atx::usize cells = dates * instruments;
  std::vector<std::string> names = {"open", "high", "low", "close", "volume", "vwap", "returns"};
  std::vector<std::vector<atx::f64>> cols(names.size(), std::vector<atx::f64>(cells));

  // A cheap, fully-deterministic LCG (no <random>) — fixed seed, never clocked.
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
    cols[0][i] = base;                         // open
    cols[1][i] = hi;                           // high
    cols[2][i] = lo;                           // low
    cols[3][i] = lo + (hi - lo) * 0.5;         // close
    cols[4][i] = 1.0e4 + next() * 9.9e5;       // volume
    cols[5][i] = (hi + lo + cols[3][i]) / 3.0; // vwap
    cols[6][i] = next() * 0.1 - 0.05;          // returns
  }

  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  // Delist a couple of instruments mid-sample (no survivorship; reads as NaN).
  if (instruments >= 3 && dates >= 4) {
    const atx::usize delist_date = dates * 2 / 3;
    for (atx::usize d = delist_date; d < dates; ++d) {
      universe[d * instruments + 1] = 0;
      universe[d * instruments + (instruments - 1)] = 0;
    }
  }
  // Scatter a few NaN source cells (data gaps) so NaN==NaN paths are exercised.
  const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
  if (cells >= 8) {
    cols[3][cells / 5] = nan;
    cols[4][cells / 3] = nan;
    cols[3][cells * 4 / 5] = nan;
  }

  auto p = Panel::create(dates, instruments, names, cols, universe);
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

// ---- compile helpers --------------------------------------------------------
//
// compile_batch(srcs) auto-names each entry `aN` (its 0-based index). To make a
// per-program FAN whose root names match that batch (so the digest — which folds
// names — agrees), each fan program is a single NAMED assignment `aK = <expr>`
// compiled through the parser pipeline, giving a single root named `aK`.

// A single-root program named exactly like batch entry `k` (`a{k} = <src>`).
[[nodiscard]] Program compile_named(std::size_t k, std::string_view src) {
  std::string text = "a" + std::to_string(k) + " = " + std::string(src);
  auto ast = parse_program(text, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

// A single-root program named `a0` (the canonical single-program case).
[[nodiscard]] Program compile_one(std::string_view src) { return compile_named(0, src); }

// Build the per-program fan for a battery of bare expressions: program k is
// `a{k} = srcs[k]`, so the fan's root names are a0..aN — matching compile_batch.
[[nodiscard]] std::vector<Program> compile_fan(const std::vector<std::string_view> &srcs) {
  std::vector<Program> progs;
  progs.reserve(srcs.size());
  for (std::size_t k = 0; k < srcs.size(); ++k) {
    progs.push_back(compile_named(k, srcs[k]));
  }
  return progs;
}

[[nodiscard]] Program compile_batch_ok(const std::vector<std::string_view> &srcs) {
  auto prog = compile_batch(std::span<const std::string_view>{srcs}, shared_lib());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

// ---- the alpha battery ------------------------------------------------------
//
// >=32 distinct BARE element-wise/arith expressions (compile_batch auto-names
// them a0..aN; compile_fan names each a{k} to match). Several deliberately SHARE
// subexpressions (e.g. `high - low`, `(high + low) / 2`, `close - open`) so the
// strategy-A recompute (CSE lost across the per-program shard boundary) is really
// exercised — the batch program dedups them, the fan recomputes them, and the
// digests must STILL agree. Only ops the VM implements element-wise (+,-,*,/ and
// the panel fields); no Cs*/Ts* (those are NotImplemented).
[[nodiscard]] std::vector<std::string_view> battery() {
  return {
      "close - open",
      "(high + low) / 2 - vwap",
      "close / open - 1",
      "high - low",
      "(high - low) / close",
      "(high + low) / 2",
      "close - (high + low) / 2",
      "volume * close",
      "volume / (close + 1)",
      "open * high - low * close",
      "(close - open) / (high - low)",
      "vwap - close",
      "vwap / close",
      "returns * volume",
      "returns + close - open",
      "high * 2 - low",
      "low * 2 - high",
      "(open + close) / 2 - (high + low) / 2",
      "close * close - open * open",
      "(high - low) * (close - open)",
      "volume * (close - open)",
      "vwap * volume",
      "(close - open) + (high - low)",
      "open - close + high - low",
      "close / vwap - 1",
      "(vwap - open) / (high - low)",
      "high / low - 1",
      "(close + high + low) / 3",
      "returns * returns",
      "volume - volume / 2",
      "open + high + low + close",
      "(open + high + low + close) / 4 - vwap",
      "close * returns - open * returns",
      "(high - vwap) - (vwap - low)",
  };
}

// ===========================================================================
//  1. parallel digest == single-thread BATCH digest (the load-bearing test).
// ===========================================================================

TEST(ParallelBatchEval, MatchesSingleThreadDigest) {
  const auto srcs = battery(); // >=32 named assignments
  const Panel panel = make_panel(40, 16, 0xABCDEF01ULL);

  const Program batch = compile_batch_ok(srcs);
  Engine eng{panel};
  const SignalSet seq = eng.evaluate(batch).value();

  std::vector<Program> progs = compile_fan(srcs);
  DetPool pool{4};
  const SignalSet par = parallel_evaluate(progs, panel, pool).value();

  EXPECT_EQ(signal_set_digest(par), signal_set_digest(seq))
      << "parallel fan digest must be byte-identical to single-thread batch";
}

// ===========================================================================
//  2. digest invariant across worker counts {1,2,4,8}.
// ===========================================================================

TEST(ParallelBatchEval, ThreadCountInvariant) {
  const auto srcs = battery();
  const Panel panel = make_panel(40, 16, 0xABCDEF01ULL);

  std::vector<Program> progs = compile_fan(srcs);

  atx::u64 ref = 0;
  for (std::size_t w : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    DetPool pool{w};
    const atx::u64 d = signal_set_digest(parallel_evaluate(progs, panel, pool).value());
    if (w == 1) {
      ref = d;
    } else {
      EXPECT_EQ(d, ref) << "worker count " << w << " changed the digest";
    }
  }
}

// ===========================================================================
//  3. empty program list -> correctly-shaped empty SignalSet.
// ===========================================================================

TEST(ParallelBatchEval, EmptyProgramListYieldsEmptySignalSet) {
  const Panel panel = make_panel(8, 4, 1ULL);
  DetPool pool{4};
  const SignalSet out = parallel_evaluate(std::span<const Program>{}, panel, pool).value();
  EXPECT_EQ(out.alphas.size(), 0U);
  EXPECT_EQ(out.dates, 8U);
  EXPECT_EQ(out.instruments, 4U);
}

// ===========================================================================
//  4. a one-program fan equals a direct Engine eval.
// ===========================================================================

TEST(ParallelBatchEval, SingleRootMatchesEngine) {
  const Panel panel = make_panel(12, 5, 7ULL);
  const Program p = compile_one("close - open");
  Engine eng{panel};
  const SignalSet ref = eng.evaluate(p).value();
  DetPool pool{4};
  const SignalSet got = parallel_evaluate(std::span<const Program>{&p, 1}, panel, pool).value();
  EXPECT_EQ(signal_set_digest(got), signal_set_digest(ref));
}

// ===========================================================================
//  5. an evaluate error surfaces as Err; lowest-index error wins.
//
//  A panel built WITHOUT the `vwap` field makes any program that references vwap
//  fail at EVALUATE with Err(NotFound) (resolve_fields -> panel_.field_id). Two
//  such programs at indices 1 and 3 (amid good programs) must surface as Err,
//  deterministically regardless of worker count. (We use a field-missing panel
//  because the formerly-NotImplemented Cs*/Ts* ops now evaluate successfully, so
//  they no longer provide a cheap evaluate-error case.)
// ===========================================================================

// A panel WITHOUT `vwap` (so a program referencing vwap fails at evaluate).
[[nodiscard]] Panel make_panel_no_vwap(atx::usize dates, atx::usize instruments) {
  const atx::usize cells = dates * instruments;
  std::vector<std::string> names = {"open", "high", "low", "close", "volume", "returns"};
  std::vector<std::vector<atx::f64>> cols(names.size(), std::vector<atx::f64>(cells, 1.0));
  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  auto p = Panel::create(dates, instruments, names, cols, universe);
  EXPECT_TRUE(p.has_value()) << (p ? "" : p.error().message());
  return p.value_or(Panel::create(0, 0, {}, {}, {}).value());
}

TEST(ParallelBatchEval, ErrorPropagatesLowestIndex) {
  const Panel panel = make_panel_no_vwap(12, 5);
  std::vector<Program> progs;
  progs.push_back(compile_named(0, "close - open"));   // ok
  progs.push_back(compile_named(1, "vwap - close"));   // Err(NotFound) at eval (no vwap)
  progs.push_back(compile_named(2, "high - low"));     // ok
  progs.push_back(compile_named(3, "vwap / close"));   // Err(NotFound) at eval (no vwap)

  DetPool pool{4};
  auto r = parallel_evaluate(progs, panel, pool);
  ASSERT_FALSE(r.has_value()) << "a failing program must surface as Err";
  // Deterministic: a failure surfaces regardless of worker count.
  for (std::size_t w : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    DetPool p{w};
    auto rr = parallel_evaluate(progs, panel, p);
    EXPECT_FALSE(rr.has_value()) << "worker count " << w;
  }
}


}  // namespace atxtest_parallel_batch_eval_test
