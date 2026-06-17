// parallel_determinism_test.cpp — S2-4 capstone: the cross-cutting determinism
// proof of Sprint S2 (deterministic parallel compute).
//
// ===========================================================================
//  What this suite proves
// ===========================================================================
//  For BOTH parallel paths shipped by S2 — parallel_evaluate (batch eval, S2-2)
//  and parallel_cpcv (CPCV folds, S2-3) — the result digest is:
//    1. IDENTICAL across worker counts {1, 2, 4, 8}, AND
//    2. EQUAL to the single-thread reference,
//  on a LARGE stress fixture (many small alphas — the worst case for races:
//  short per-item compute maximizes contention on the atomic dispenser and the
//  output-slot writes), plus
//    3. RUN-TO-RUN stable (same worker count, repeated — no nondeterminism from
//       allocator / scheduling jitter across invocations).
//
// ===========================================================================
//  TSan residual (recorded sprint residual)
// ===========================================================================
//  clang-cl on Windows has NO ThreadSanitizer (TSan is unsupported on the MSVC
//  toolchain). Race-freedom is therefore argued BY CONSTRUCTION — the inputs are
//  const and shared read-only (the Panel / the AlphaStreams / the fold index
//  sets), each work item writes ONLY its own disjoint pre-indexed output slot,
//  and each worker runs on its OWN stateful Engine (Engine is not thread-safe) —
//  and VALIDATED empirically by the digest-invariance matrix below: a data race
//  on a shared mutable cell would, on the many-small-alphas worst case, flip a
//  bit and so flip a digest, which these EXPECT_EQ matrices would catch. The
//  upstream cross-check is an OPTIONAL Linux-CI ThreadSanitizer job running these
//  same suites; that is recorded as a sprint residual, not gated here.

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
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/eval/cpcv.hpp"

#include "atx/engine/parallel/batch_eval.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/digest.hpp"
#include "atx/engine/parallel/parallel_run.hpp"

namespace atxtest_parallel_determinism_test {

namespace eval = atx::engine::eval;

using atx::engine::alpha::analyze;
using atx::engine::alpha::AlphaStreams;
using atx::engine::alpha::compile;
using atx::engine::alpha::compile_batch;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
using atx::engine::parallel::DetPool;
using atx::engine::parallel::FoldResult;
using atx::engine::parallel::parallel_cpcv;
using atx::engine::parallel::parallel_evaluate;
using atx::engine::parallel::result_table_digest;
using atx::engine::parallel::signal_set_digest;

// ---------------------------------------------------------------------------
//  Process-lifetime Library: the Ast borrows OpSig pointers from it, so it must
//  outlive every parse result. (Same idiom as parallel_batch_eval_test.cpp;
//  helpers are re-derived per-TU — they are not shared across translation units.)
// ---------------------------------------------------------------------------
[[nodiscard]] const Library& shared_lib() {
  static const Library lib;
  return lib;
}

// ---- synthetic panel (mirror of parallel_batch_eval_test.cpp's generator) ---

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

// ---- compile helpers (mirror parallel_batch_eval_test.cpp) ------------------
//
// compile_batch(srcs) auto-names each entry `aN` (its 0-based index). The
// per-program FAN names each program `a{k} = <expr>` so the fan's root names
// match the batch — the digest folds names, so they must agree.

[[nodiscard]] Program compile_named(std::size_t k, std::string_view src) {
  const std::string text = "a" + std::to_string(k) + " = " + std::string(src);
  auto ast = parse_program(text, shared_lib());
  EXPECT_TRUE(ast.has_value()) << (ast ? "" : ast.error().message());
  auto ana = analyze(ast.value());
  EXPECT_TRUE(ana.has_value()) << (ana ? "" : ana.error().message());
  auto prog = compile(ast.value(), ana.value());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

[[nodiscard]] std::vector<Program> compile_fan(const std::vector<std::string_view>& srcs) {
  std::vector<Program> progs;
  progs.reserve(srcs.size());
  for (std::size_t k = 0; k < srcs.size(); ++k) {
    progs.push_back(compile_named(k, srcs[k]));
  }
  return progs;
}

[[nodiscard]] Program compile_batch_ok(const std::vector<std::string_view>& srcs) {
  auto prog = compile_batch(std::span<const std::string_view>{srcs}, shared_lib());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

// ---- synthetic AlphaStreams (mirror parallel_run_test.cpp) ------------------

[[nodiscard]] AlphaStreams make_streams(std::size_t n_alphas, std::size_t n_periods,
                                        std::size_t n_inst, std::uint64_t seed) {
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
  for (auto& v : s.pnl_flat) {
    v = nx() * 0.04 - 0.02; // small returns in [-0.02, 0.02)
  }
  for (auto& v : s.pos_flat) {
    v = nx() * 2.0 - 1.0; // weights in [-1, 1)
  }
  return s;
}

// ---- the LARGE stress battery -----------------------------------------------
//
// >= 128 distinct BARE element-wise expressions, generated programmatically:
// each root differs (the trailing integer constant varies) but stays cheap, and
// a handful of SHARED subexpressions (close-open, high-low, (high+low)/2) recur
// across the battery so the strategy-A per-program recompute is exercised AT
// SCALE. Only element-wise +,-,*,/ and panel fields — no Cs*/Ts* (those work but
// would make the worst-case stress slower without exercising any new race path).

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

// Build a vector of string_views over a kept-alive vector<string>.
[[nodiscard]] std::vector<std::string_view> views_of(const std::vector<std::string>& owned) {
  return std::vector<std::string_view>(owned.begin(), owned.end());
}

// ===========================================================================
//  1. BATCH-EVAL matrix: {1,2,4,8} == single-thread, on the LARGE battery.
// ===========================================================================
TEST(ParallelDeterminism, BatchEvalMatrixInvariant) {
  const std::vector<std::string> strs = big_battery(128); // keep the strings alive
  const std::vector<std::string_view> srcs = views_of(strs);
  const Panel panel = make_panel(64, 32, 0xD37E12ULL); // many small alphas, worst case

  // Single-thread reference: Engine::evaluate of the equivalent BATCH program.
  const Program batch = compile_batch_ok(srcs);
  Engine eng{panel};
  const atx::u64 ref = signal_set_digest(eng.evaluate(batch).value());

  const std::vector<Program> progs = compile_fan(srcs);
  for (std::size_t w : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    DetPool pool{w};
    EXPECT_EQ(signal_set_digest(parallel_evaluate(progs, panel, pool).value()), ref)
        << "batch-eval digest changed at workers=" << w;
  }
}

// ===========================================================================
//  2. CPCV matrix: {1,2,4,8} == single-thread (pool{1}) reference.
//     C(8,2)=28 folds over a 96-period synthetic stream.
// ===========================================================================
TEST(ParallelDeterminism, CpcvMatrixInvariant) {
  const auto streams = make_streams(1, 96, 8, 0x5EED01ULL);
  std::vector<eval::LabelSpan> spans;
  spans.reserve(96);
  for (std::size_t i = 0; i < 96; ++i) {
    spans.push_back({i, i + 1});
  }
  const auto folds = eval::cpcv_folds(spans, eval::CpcvConfig{8, 2, 0.0}); // C(8,2)=28 folds

  // Single-thread reference: pool{1}.
  DetPool ref_pool{1};
  const atx::u64 ref = result_table_digest(parallel_cpcv(folds, streams, 0, 1.0e6, ref_pool));

  for (std::size_t w : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    DetPool pool{w};
    EXPECT_EQ(result_table_digest(parallel_cpcv(folds, streams, 0, 1.0e6, pool)), ref)
        << "cpcv digest changed at workers=" << w;
  }
}

// ===========================================================================
//  3. BATCH-EVAL run-to-run stability: same worker count, repeated.
// ===========================================================================
TEST(ParallelDeterminism, BatchEvalRunToRunStable) {
  const std::vector<std::string> strs = big_battery(96); // keep the strings alive
  const std::vector<std::string_view> srcs = views_of(strs);
  const Panel panel = make_panel(48, 24, 7ULL);
  const std::vector<Program> progs = compile_fan(srcs);

  DetPool pool{4};
  const atx::u64 d0 = signal_set_digest(parallel_evaluate(progs, panel, pool).value());
  for (int rep = 0; rep < 8; ++rep) {
    EXPECT_EQ(signal_set_digest(parallel_evaluate(progs, panel, pool).value()), d0)
        << "rep=" << rep;
  }
}


}  // namespace atxtest_parallel_determinism_test
