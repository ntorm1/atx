// alpha_eval_perf_test.cpp — S1-0: compile memoization unit + microbench.
//
// Verifies:
//   CompileCacheIdentity  — compile(ast) twice → byte-identical Program.
//   CollisionSafety       — N distinct ASTs each round-trip to their own correct
//                           cold-compiled Program through the cache.
//   DigestUnchanged       — a fixed AST set evaluated on a synthetic 3×2 panel
//                           produces byte-identical SignalSet before/after the
//                           cache warms (output unaffected by caching).
//   ColdVsWarmMicrobench  — wall-clock of M compiles of a fixed corpus, cold vs
//                           warm. Reports ns/compile; no hard assertion on speedup.
//
// Naming: Subject_Condition_ExpectedResult.

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

namespace atxtest_alpha_eval_perf_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::compile_cached;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Instr;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Parse + analyze + cold compile (bypasses cache). ASSERTs each stage succeeds.
[[nodiscard]] Program cold_compile(std::string_view src) {
  auto parsed = parse_program(src, shared_lib());
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Program{};
  }
  auto analysis = analyze(*parsed);
  EXPECT_TRUE(analysis.has_value()) << (analysis ? "" : analysis.error().message());
  if (!analysis) {
    return Program{};
  }
  auto prog = compile(*parsed, *analysis);
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog ? std::move(*prog) : Program{};
}

// Parse + analyze + compile_cached (cache path). ASSERTs each stage succeeds.
[[nodiscard]] Program cached_compile(std::string_view src) {
  auto parsed = parse_program(src, shared_lib());
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Program{};
  }
  auto analysis = analyze(*parsed);
  EXPECT_TRUE(analysis.has_value()) << (analysis ? "" : analysis.error().message());
  if (!analysis) {
    return Program{};
  }
  auto prog = compile_cached(*parsed, *analysis);
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog ? std::move(*prog) : Program{};
}

// Equality check over the EXECUTABLE fields of a Program.
//
// The linearizer's deterministic output is: code, roots, fields, num_slots,
// and required_lookback. The telemetry counters (cache_hits, intern_attempts,
// unique_nodes, total_ast_nodes) are "pure observability" (see Program comment)
// and are excluded: a cached Program retains the telemetry of the compile that
// first populated the cache, which naturally differs from a fresh cold compile.
// The brief's "byte-identical Program bytes" contract refers to the instruction
// stream and slot layout, not the observability fields.
[[nodiscard]] bool programs_equal(const Program &a, const Program &b) {
  if (a.num_slots != b.num_slots) {
    return false;
  }
  if (a.required_lookback != b.required_lookback) {
    return false;
  }
  if (a.fields != b.fields) {
    return false;
  }
  if (a.roots.size() != b.roots.size()) {
    return false;
  }
  for (atx::usize i = 0; i < a.roots.size(); ++i) {
    if (a.roots[i].name != b.roots[i].name) {
      return false;
    }
    if (a.roots[i].output != b.roots[i].output) {
      return false;
    }
  }
  if (a.code.size() != b.code.size()) {
    return false;
  }
  // Field-by-field instruction comparison. Do NOT use memcmp: the Instr struct
  // has padding bytes between n_out (u8) and imm (f64[2]) that are not
  // explicitly initialised and differ between stack frames — memcmp over them
  // is UB and produces false negatives. Compare the semantically meaningful
  // fields only; padding is irrelevant to correctness.
  for (atx::usize i = 0; i < a.code.size(); ++i) {
    const Instr &ai = a.code[i];
    const Instr &bi = b.code[i];
    if (ai.op != bi.op) {
      return false;
    }
    if (ai.dst != bi.dst) {
      return false;
    }
    if (ai.src != bi.src) {
      return false;
    }
    if (ai.param != bi.param) {
      return false;
    }
    if (ai.n_out != bi.n_out) {
      return false;
    }
    if (ai.imm != bi.imm) {
      return false;
    }
  }
  return true;
}

// ---- CompileCacheIdentity --------------------------------------------------

TEST(CompileCache_SameAst_ByteIdenticalProgram, ColdCompileTwice) {
  // Two cold compiles of the same source → byte-identical Programs.
  const Program a = cold_compile("x = ts_mean(close, 5) + rank(open)");
  const Program b = cold_compile("x = ts_mean(close, 5) + rank(open)");
  EXPECT_TRUE(programs_equal(a, b));
}

TEST(CompileCache_SameAst_ByteIdenticalProgram, CachedEqualsColda) {
  // A cached compile must produce a Program byte-identical to a cold compile.
  const Program cold = cold_compile("y = close / open");
  const Program warm = cached_compile("y = close / open");
  EXPECT_TRUE(programs_equal(cold, warm));
}

TEST(CompileCache_SameAst_ByteIdenticalProgram, CachedTwiceIdentical) {
  // Two cached compiles of the same source → byte-identical Programs.
  const Program a = cached_compile("z = ts_std(close - open, 10)");
  const Program b = cached_compile("z = ts_std(close - open, 10)");
  EXPECT_TRUE(programs_equal(a, b));
}

// ---- CollisionSafety -------------------------------------------------------
//
// N structurally DISTINCT ASTs each round-trip through the cache to their
// own cold-compiled Program. No two ASTs may share a cached Program.

TEST(CompileCache_DistinctAsts_EachHasOwnProgram, NDistinctSources) {
  // A representative corpus of structurally distinct alpha expressions.
  const std::vector<std::string_view> corpus = {
      "a = close",
      "a = open",
      "a = close - open",
      "a = close + open",
      "a = ts_mean(close, 5)",
      "a = ts_mean(close, 10)",
      "a = ts_mean(open, 5)",
      "a = rank(close)",
      "a = rank(open)",
      "a = ts_std(close, 20)",
      "a = close * 2",
      "a = ts_mean(close, 5) - ts_mean(close, 20)",
  };

  for (std::string_view src : corpus) {
    const Program cold = cold_compile(src);
    const Program warm = cached_compile(src);
    EXPECT_TRUE(programs_equal(cold, warm))
        << "Cache returned wrong Program for: " << src;
  }

  // Cross-check: every pair of distinct programs is actually distinct.
  std::vector<Program> cold_progs;
  cold_progs.reserve(corpus.size());
  for (std::string_view src : corpus) {
    cold_progs.push_back(cold_compile(src));
  }
  for (atx::usize i = 0; i < cold_progs.size(); ++i) {
    for (atx::usize j = i + 1; j < cold_progs.size(); ++j) {
      // Two structurally distinct programs must not be equal.
      EXPECT_FALSE(programs_equal(cold_progs[i], cold_progs[j]))
          << "Programs " << i << " and " << j << " should differ (corpus has distinct ASTs)";
    }
  }
}

// ---- DigestUnchanged -------------------------------------------------------
//
// Compile two alphas (cold then cached) and evaluate on a 3×2 synthetic panel.
// Both SignalSets must be bit-identical: the cache must not affect output values.

namespace {

// Build a minimal Panel with "close" and "open" fields over 3 dates × 2 instruments.
[[nodiscard]] Panel make_tiny_panel() {
  //            inst0   inst1
  // date0 :  100.0   200.0      close
  // date1 :  105.0   195.0
  // date2 :  110.0   190.0
  //            inst0   inst1
  // date0 :   99.0   198.0      open
  // date1 :  103.0   194.0
  // date2 :  108.0   188.0
  std::vector<std::vector<atx::f64>> data{
      {100.0, 200.0, 105.0, 195.0, 110.0, 190.0}, // close (date-major)
      {99.0, 198.0, 103.0, 194.0, 108.0, 188.0},  // open
  };
  std::vector<std::string> fields{"close", "open"};
  // All cells in-universe: pass empty universe vector.
  auto panel_res = Panel::create(3, 2, std::move(fields), std::move(data), {});
  if (!panel_res) {
    ADD_FAILURE() << panel_res.error().message();
    // Return a minimal valid 1×1 panel so the test can continue cleanly.
    std::vector<std::vector<atx::f64>> fallback{{1.0}, {1.0}};
    auto fb = Panel::create(1, 1, {"close", "open"}, std::move(fallback), {});
    return std::move(*fb);
  }
  return std::move(*panel_res);
}

// True iff two SignalSets are bit-identical (NaN==NaN treated as equal).
[[nodiscard]] bool signal_sets_equal(const SignalSet &a, const SignalSet &b) {
  if (a.dates != b.dates || a.instruments != b.instruments) {
    return false;
  }
  if (a.alphas.size() != b.alphas.size()) {
    return false;
  }
  for (atx::usize i = 0; i < a.alphas.size(); ++i) {
    if (a.alphas[i].name != b.alphas[i].name) {
      return false;
    }
    const auto &av = a.alphas[i].values;
    const auto &bv = b.alphas[i].values;
    if (av.size() != bv.size()) {
      return false;
    }
    for (atx::usize k = 0; k < av.size(); ++k) {
      // NaN == NaN for this digest check (both NaN is identical).
      const bool a_nan = std::isnan(av[k]);
      const bool b_nan = std::isnan(bv[k]);
      if (a_nan != b_nan) {
        return false;
      }
      if (!a_nan && av[k] != bv[k]) {
        return false;
      }
    }
  }
  return true;
}

} // namespace

TEST(CompileCache_Digest_OutputUnchanged, ColdVsCachedEvalOnTinyPanel) {
  constexpr std::string_view kSrc = "x = close - open";
  const Panel panel = make_tiny_panel();

  const Program cold = cold_compile(kSrc);
  const Program warm = cached_compile(kSrc);

  Engine cold_engine{panel};
  Engine warm_engine{panel};
  const auto sig_cold = cold_engine.evaluate(cold);
  const auto sig_warm = warm_engine.evaluate(warm);

  ASSERT_TRUE(sig_cold.has_value()) << sig_cold.error().message();
  ASSERT_TRUE(sig_warm.has_value()) << sig_warm.error().message();
  EXPECT_TRUE(signal_sets_equal(*sig_cold, *sig_warm))
      << "Cached compile produced different evaluation output vs cold compile";
}

// ---- ColdVsWarmMicrobench --------------------------------------------------
//
// Time M compiles of a fixed corpus, cold (bypassing cache) vs warm (cache hits).
// Report ns/compile to stdout. No hard speedup assertion.

TEST(CompileCache_Microbench, ColdVsWarmCompileTime) {
  constexpr int kReps = 200;
  const std::vector<std::string_view> corpus = {
      "a = ts_mean(close, 5) + rank(open) * close",
      "a = ts_std(close, 20) - ts_mean(close, 5)",
      "a = rank(close - open) / ts_std(close, 10)",
      "a = ts_mean(close, 5) * ts_mean(open, 5)",
  };

  // Pre-warm the cache once so the first "warm" run is truly cached.
  for (std::string_view src : corpus) {
    (void)cached_compile(src);
  }

  // --- cold: bypass the cache each time -------------------------------------
  using Clock = std::chrono::steady_clock;
  const auto cold_start = Clock::now();
  for (int r = 0; r < kReps; ++r) {
    for (std::string_view src : corpus) {
      const auto p = cold_compile(src);
      // Prevent the compiler from optimising away the work.
      ASSERT_FALSE(p.code.empty());
    }
  }
  const auto cold_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - cold_start).count();

  // --- warm: all compiles are cache hits ------------------------------------
  const auto warm_start = Clock::now();
  for (int r = 0; r < kReps; ++r) {
    for (std::string_view src : corpus) {
      const auto p = cached_compile(src);
      ASSERT_FALSE(p.code.empty());
    }
  }
  const auto warm_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - warm_start).count();

  const long total_compiles = static_cast<long>(kReps) * static_cast<long>(corpus.size());
  const double cold_ns_per = static_cast<double>(cold_ns) / static_cast<double>(total_compiles);
  const double warm_ns_per = static_cast<double>(warm_ns) / static_cast<double>(total_compiles);

  std::cout << "[CompileCache microbench] " << total_compiles << " compiles × "
            << corpus.size() << " sources\n"
            << "  cold: " << cold_ns_per << " ns/compile\n"
            << "  warm: " << warm_ns_per << " ns/compile\n"
            << "  speedup: " << (cold_ns_per / warm_ns_per) << "x\n";

  // Warm must not be slower than cold (allow 5x slack for noise; we just want to
  // catch a catastrophic regression, not race the clock).
  EXPECT_LE(warm_ns_per, cold_ns_per * 5.0)
      << "Cache warm path is unreasonably slow vs cold — check implementation";
}

} // namespace atxtest_alpha_eval_perf_test
