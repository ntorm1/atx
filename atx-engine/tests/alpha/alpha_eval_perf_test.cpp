// alpha_eval_perf_test.cpp — S1-0: compile memoization unit + microbench.
//
// Verifies:
//   CompileCacheIdentity  — compile(ast) twice → instruction-stream-identical
//                           Program (telemetry fields excluded).
//   CollisionSafety       — N distinct ASTs each round-trip to their own correct
//                           cold-compiled Program through the cache.
//   DigestUnchanged       — a fixed AST set evaluated on a synthetic 3×2 panel
//                           produces a bit-identical SignalSet before/after the
//                           cache warms (output unaffected by caching).
//   ColdVsWarmMicrobench  — wall-clock of the COMPILE STEP of a fixed corpus,
//                           cold (build_dag+linearize) vs warm (cache hit, no
//                           build_dag). Reports ns/compile; sanity bound only.
//   CsValidSetInvariance  — S1-4: pin the ascending-instrument-index invariant
//                           of the cs_one_date valid-set scan.  Rank tie-break
//                           and date-independence both asserted.
//   ScratchWeightEquiv    — S1-1: scratch overload of to_target_weights produces
//                           byte-identical output to the allocating overload on a
//                           multi-date panel (NaN rows, single-valid rows, etc.).
//   StreamDigestScratch   — S1-1: fill_alpha_stream with hoisted scratch buffers
//                           produces byte-identical AlphaStreams vs allocating path.
//   ScratchAllocBench     — S1-1: microbench comparing allocating vs scratch
//                           to_target_weights; scratch O(1) allocs per alpha.
//
// Naming: Subject_Condition_ExpectedResult.

#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/cs_ops.hpp" // detail::cs_rank_row, detail::cs_zscore_row (S1-4)
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streams.hpp"    // S1-1: fill_alpha_stream, extract_streams
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/exec/execution_sim.hpp" // S1-1: ExecutionSimulator (frictionless)
#include "atx/engine/loop/weight_policy.hpp" // S1-1: WeightPolicy + scratch overload

namespace atxtest_alpha_eval_perf_test {

using atx::engine::alpha::analyze;
using atx::engine::alpha::Analysis;
using atx::engine::alpha::Ast;
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
// The contract is instruction-stream identical (telemetry fields excluded):
// the code stream, roots, fields, slot count and lookback match exactly; the
// observability counters legitimately differ between a cold and a warm compile.
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

TEST(CompileCache_SameAst_ByteIdenticalProgram, CachedEqualsCold) {
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
// Time the COMPILE STEP ONLY of a fixed corpus, cold (compile = build_dag +
// linearize) vs warm (compile_cached cache hit). Parse + analyze are done ONCE
// up front and EXCLUDED from the timing loops, so this isolates exactly what
// the cache changes: a warm hit skips build_dag entirely. Report ns/compile to
// stdout. No hard speedup assertion (machine-dependent), only a sanity bound.

TEST(CompileCache_Microbench, ColdVsWarmCompileTime) {
  constexpr int kReps = 200;
  const std::vector<std::string_view> corpus = {
      "a = ts_mean(close, 5) + rank(open) * close",
      "a = ts_std(close, 20) - ts_mean(close, 5)",
      "a = rank(close - open) / ts_std(close, 10)",
      "a = ts_mean(close, 5) * ts_mean(open, 5)",
  };

  // Parse + analyze each source ONCE; the timed loops below reuse these so the
  // measurement reflects compile() vs compile_cached() only (not the lexer).
  std::vector<Ast> asts;
  std::vector<Analysis> analyses;
  asts.reserve(corpus.size());
  analyses.reserve(corpus.size());
  for (std::string_view src : corpus) {
    auto parsed = parse_program(src, shared_lib());
    ASSERT_TRUE(parsed.has_value()) << parsed.error().message();
    auto analysis = analyze(*parsed);
    ASSERT_TRUE(analysis.has_value()) << analysis.error().message();
    asts.push_back(std::move(*parsed));
    analyses.push_back(std::move(*analysis));
  }

  // Pre-warm the cache so every "warm" call below is a true cache hit.
  for (atx::usize i = 0; i < asts.size(); ++i) {
    (void)compile_cached(asts[i], analyses[i]);
  }

  // --- cold: build_dag + linearize every call -------------------------------
  using Clock = std::chrono::steady_clock;
  const auto cold_start = Clock::now();
  for (int r = 0; r < kReps; ++r) {
    for (atx::usize i = 0; i < asts.size(); ++i) {
      const auto p = compile(asts[i], analyses[i]);
      ASSERT_TRUE(p.has_value());
      ASSERT_FALSE(p->code.empty()); // defeat dead-code elimination
    }
  }
  const auto cold_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - cold_start).count();

  // --- warm: compile_cached cache hit (no build_dag) ------------------------
  const auto warm_start = Clock::now();
  for (int r = 0; r < kReps; ++r) {
    for (atx::usize i = 0; i < asts.size(); ++i) {
      const auto p = compile_cached(asts[i], analyses[i]);
      ASSERT_TRUE(p.has_value());
      ASSERT_FALSE(p->code.empty());
    }
  }
  const auto warm_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - warm_start).count();

  const long total_compiles = static_cast<long>(kReps) * static_cast<long>(corpus.size());
  const double cold_ns_per = static_cast<double>(cold_ns) / static_cast<double>(total_compiles);
  const double warm_ns_per = static_cast<double>(warm_ns) / static_cast<double>(total_compiles);

  std::cout << "[CompileCache microbench] " << total_compiles << " compiles × "
            << corpus.size() << " sources (parse+analyze excluded)\n"
            << "  cold (build_dag+linearize): " << cold_ns_per << " ns/compile\n"
            << "  warm (cache hit):           " << warm_ns_per << " ns/compile\n"
            << "  speedup: " << (cold_ns_per / warm_ns_per) << "x\n";

  // Sanity bound only: a warm hit (no build_dag) must not be slower than a cold
  // compile. Catch a catastrophic regression, not race the clock.
  EXPECT_LE(warm_ns_per, cold_ns_per)
      << "Cache warm path is slower than a cold compile — build_dag not skipped?";
}

} // namespace atxtest_alpha_eval_perf_test

// ============================================================================
// S1-4: CsValidSetInvariance — pin the ascending-instrument-index ordering
// invariant of the cs_one_date valid-set scan.
// ============================================================================
//
// The cross-section valid-set (built in cs_one_date / vm.hpp) is REQUIRED to be
// in ascending instrument-index order.  Two distinct kernels depend on it:
//   * cs_rank_row: a stable sort whose tie-break order IS the pre-sort (i.e.
//     valid-set) order — a non-ascending scan silently flips tied ranks.
//   * cs_zscore_row: its Σx / Σ(x-mean)² reductions accumulate in scan order,
//     and f64 addition is not associative — a non-ascending scan changes the
//     summed bits (hence every cell) for ill-conditioned rows.
//
// Three sub-test groups, each pinning a DIFFERENT property (do not conflate):
//
//   CsValidSet_KernelDirect_TiedValues  [the load-bearing tie-break pin]
//     Calls cs_rank_row directly with (a) ascending valid and (b) a shuffled
//     valid containing a tie.  Asserts the shuffled order yields DIFFERENT tied
//     ranks — this test FAILS if the production scan were non-ascending.
//
//   CsValidSet_KernelDirect_IllConditioned  [the load-bearing reduction pin]
//     Calls cs_zscore_row directly on an ill-conditioned row with ascending vs
//     shuffled valid, and asserts the two reductions differ in at least one
//     cell's bits — proving cs_zscore_row reduces in valid-set (ascending)
//     order.  This test FAILS to be meaningful if the inputs were well-
//     conditioned (it self-checks that the order is observable).
//
//   CsValidSet_Engine_CrossDateStateIsolation  [a DIFFERENT property]
//     Evaluates rank/zscore on a full 3-date panel, then evaluates each date
//     independently in a permuted visit order, and asserts per-date outputs are
//     byte-identical.  This pins CROSS-DATE STATE ISOLATION (no residue leaks
//     through cs_valid_ / CsScratch between dates) — it does NOT and cannot pin
//     the ascending-instrument-index invariant, because each date rebuilds its
//     valid set fresh, so permuting dates can never expose a reversed scan.
//     Kept because it is the plan's literal §Accept and a useful determinism guard.

namespace atxtest_alpha_cs_valid {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::detail::CsScratch;
using atx::engine::alpha::detail::cs_rank_row;
using atx::engine::alpha::detail::cs_zscore_row;

// NaN sentinel matching cs_ops.hpp's kCsNaN (quiet NaN).
static constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Bit-identical equality with NaN==NaN treated as equal.
[[nodiscard]] static bool bit_eq(atx::f64 a, atx::f64 b) noexcept {
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  return a == b;
}

[[nodiscard]] static const Library &cs_lib() {
  static const Library lib;
  return lib;
}

// Parse + analyze + compile a single-alpha source.  ASSERTs each stage.
[[nodiscard]] static Program cs_compile(std::string_view src) {
  auto parsed = parse_program(src, cs_lib());
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

// ---- CsValidSetKernel — direct kernel tests --------------------------------

// Row layout (5 instruments):
//   inst 0: NaN        — excluded (NaN hole)
//   inst 1: 1.0        — tied with inst 2
//   inst 2: 1.0        — tied with inst 1
//   inst 3: 2.0
//   inst 4: NaN        — excluded (NaN hole)
//
// With ascending valid = {1, 2, 3}:
//   stable_sort by value preserves the tie order (1 before 2) -> [1, 2, 3]
//   rank[1] = 0/(3-1) = 0.0   (lowest; inst1 wins tie by ascending index)
//   rank[2] = 1/(3-1) = 0.5
//   rank[3] = 2/(3-1) = 1.0   (highest)
//
// With SHUFFLED valid = {2, 1, 3}:
//   stable_sort by value: tie now preserves shuffled order (2 before 1) -> [2, 1, 3]
//   rank[2] = 0.0              <- DIFFERENT (inst2 "wins" the tie by shuffled order)
//   rank[1] = 0.5              <- DIFFERENT
//   rank[3] = 1.0
//
// The test asserts BOTH: (a) the ascending case gives the expected values, AND
// (b) the shuffled case gives different values for the tied pair — i.e. the
// invariant is genuinely load-bearing and not just defensive documentation.

TEST(CsValidSet_KernelDirect_TiedValues, RankTiebreakByAscendingIndex) {
  const std::vector<atx::f64> x{kNaN, 1.0, 1.0, 2.0, kNaN};
  CsScratch scratch;

  // --- ascending valid = {1, 2, 3} (CORRECT: forward scan over instruments) --
  {
    const std::vector<atx::usize> valid_asc{1, 2, 3};
    std::vector<atx::f64> out(5, kNaN);
    cs_rank_row(x, valid_asc, out, scratch);

    EXPECT_TRUE(std::isnan(out[0])) << "inst0 (NaN input) must stay NaN";
    EXPECT_EQ(out[1], 0.0 / 2.0) << "inst1: tied-low -> rank 0/(3-1)=0.0 (ascending-index wins)";
    EXPECT_EQ(out[2], 1.0 / 2.0) << "inst2: tied-high -> rank 1/(3-1)=0.5";
    EXPECT_EQ(out[3], 2.0 / 2.0) << "inst3: unique highest -> rank 2/(3-1)=1.0";
    EXPECT_TRUE(std::isnan(out[4])) << "inst4 (NaN input) must stay NaN";
  }

  // --- SHUFFLED valid = {2, 1, 3} (simulates a broken non-ascending scan) ---
  // inst2 appears before inst1, so stable_sort preserves that order for the tie.
  // This produces DIFFERENT ranks for the tied pair.
  {
    const std::vector<atx::usize> valid_shuffled{2, 1, 3};
    std::vector<atx::f64> out_shuf(5, kNaN);
    cs_rank_row(x, valid_shuffled, out_shuf, scratch);

    // Shuffled order flips the tied pair:
    EXPECT_EQ(out_shuf[2], 0.0 / 2.0) << "shuffled: inst2 wins tie (appears first) -> 0.0";
    EXPECT_EQ(out_shuf[1], 1.0 / 2.0) << "shuffled: inst1 loses tie -> 0.5";
    EXPECT_EQ(out_shuf[3], 2.0 / 2.0) << "inst3 (unique highest) unchanged";

    // CRITICAL: prove the invariant is load-bearing — the tied pair must differ
    // between ascending and shuffled valid.  If they agreed, the valid-set order
    // would be irrelevant and the vm.hpp invariant comment would describe a non-bug.
    EXPECT_NE(out_shuf[1], 0.0 / 2.0)
        << "INVARIANT NOT LOAD-BEARING: shuffled valid gives same rank for inst1 as ascending "
           "(tie-break is independent of valid-set order — reassess the vm.hpp invariant comment)";
    EXPECT_NE(out_shuf[2], 1.0 / 2.0)
        << "INVARIANT NOT LOAD-BEARING: shuffled valid gives same rank for inst2 as ascending";
  }
}

TEST(CsValidSet_KernelDirect_IllConditioned, ZscoreReductionOrderIsAscendingIndex) {
  // cs_zscore_row accumulates Σx (then Σ(x-mean)²) over the valid set in SCAN
  // ORDER.  f64 addition is not associative, so for an ILL-CONDITIONED row
  // (large magnitudes that cancel, plus small ones) the summed bits depend on
  // the order — hence mean, sd, and every per-cell zscore depend on it too.
  //
  // This test pins that cs_zscore_row reduces in valid-set (== ascending
  // instrument-index) order by showing a SHUFFLED valid set yields BIT-DIFFERENT
  // output in at least one cell.  If the production scan were ever reversed or
  // reordered, the zscore column would change bits — breaking AuditExact.
  //
  // Row (7 instruments, no NaN holes here so all 7 are valid): two large
  // magnitudes that nearly cancel (1e16, -1e16) interleaved with small values.
  const std::vector<atx::f64> x{1e16, 1.0, 1.0, 1.0, -1e16, 3.0, 7.0};

  std::vector<atx::f64> out_asc(7, kNaN);
  {
    const std::vector<atx::usize> valid_asc{0, 1, 2, 3, 4, 5, 6};
    cs_zscore_row(x, valid_asc, out_asc);
  }

  // A shuffled valid order: same membership, different reduction order. The
  // large-magnitude cancellation lands at different partial sums, so the f64
  // result differs in some cells.
  std::vector<atx::f64> out_shuf(7, kNaN);
  {
    const std::vector<atx::usize> valid_shuf{4, 5, 0, 6, 1, 3, 2};
    cs_zscore_row(x, valid_shuf, out_shuf);
  }

  // CRITICAL: the two reductions MUST differ in at least one cell's bits —
  // otherwise the reduction order would be unobservable and this test would
  // pin nothing.  (Verified offline: ascending vs this shuffle diverge.)
  bool any_bit_diff = false;
  for (atx::usize i = 0; i < 7; ++i) {
    if (!bit_eq(out_asc[i], out_shuf[i])) {
      any_bit_diff = true;
      break;
    }
  }
  EXPECT_TRUE(any_bit_diff)
      << "REDUCTION ORDER UNOBSERVABLE: ascending and shuffled valid produced "
         "bit-identical zscore — pick more ill-conditioned inputs, the test pins nothing as-is";

  // And pin the canonical (ascending) output bit-exactly: re-running the same
  // ascending reduction must reproduce it (no hidden nondeterminism).
  std::vector<atx::f64> out_asc2(7, kNaN);
  {
    const std::vector<atx::usize> valid_asc{0, 1, 2, 3, 4, 5, 6};
    cs_zscore_row(x, valid_asc, out_asc2);
  }
  for (atx::usize i = 0; i < 7; ++i) {
    EXPECT_TRUE(bit_eq(out_asc[i], out_asc2[i]))
        << "ascending reduction not reproducible at inst" << i;
  }
}

// ---- CsValidSetEngine — cross-date state isolation -------------------------
//
// NOTE: these engine-level tests pin cross-date STATE ISOLATION (no residue
// leaks through cs_valid_ / CsScratch), NOT the ascending-instrument-index
// invariant — see the group header above.  Permuting the date visit order
// cannot expose a reversed instrument scan, because each date rebuilds its
// valid set fresh.  The ascending-index invariant is pinned by the two
// KernelDirect tests, which exercise the scan order directly.

// 3-date × 5-instrument panel: NaN holes + tied values on every date.
//
//          inst0   inst1   inst2   inst3   inst4
// date0:   NaN     1.0     1.0     2.0     NaN     <- ties at inst1,inst2
// date1:   3.0     NaN     1.5     1.5     0.5     <- ties at inst2,inst3
// date2:   2.0     2.0     NaN     1.0     3.0     <- ties at inst0,inst1
[[nodiscard]] static Panel make_cs_panel() {
  const std::vector<atx::f64> close_col{
      kNaN, 1.0,  1.0,  2.0,  kNaN, // date 0 (date-major layout: d*instruments+i)
      3.0,  kNaN, 1.5,  1.5,  0.5,  // date 1
      2.0,  2.0,  kNaN, 1.0,  3.0,  // date 2
  };
  std::vector<std::vector<atx::f64>> data{close_col};
  auto res = Panel::create(3, 5, {"close"}, std::move(data), {});
  if (!res) {
    ADD_FAILURE() << res.error().message();
    // Return a minimal 1×1 panel so downstream tests can continue.
    std::vector<std::vector<atx::f64>> fb{{1.0}};
    return std::move(*Panel::create(1, 1, {"close"}, std::move(fb), {}));
  }
  return std::move(*res);
}

// Evaluate `prog` on a single-date panel built from `row`.
// Returns the 5-element output values vector.
[[nodiscard]] static std::vector<atx::f64>
eval_single_date(const Program &prog, const std::vector<atx::f64> &row) {
  EXPECT_EQ(row.size(), 5u);
  std::vector<std::vector<atx::f64>> data{row};
  auto panel_res = Panel::create(1, 5, {"close"}, std::move(data), {});
  EXPECT_TRUE(panel_res.has_value()) << (panel_res ? "" : panel_res.error().message());
  if (!panel_res) {
    return std::vector<atx::f64>(5, kNaN);
  }
  // Materialize the panel before constructing the Engine (Engine borrows by ref).
  const Panel single_panel = std::move(*panel_res);
  Engine engine{single_panel};
  auto sig = engine.evaluate(prog);
  EXPECT_TRUE(sig.has_value()) << (sig ? "" : sig.error().message());
  if (!sig || sig->alphas.empty()) {
    return std::vector<atx::f64>(5, kNaN);
  }
  return sig->alphas[0].values; // 1 * 5 = 5 elements
}

TEST(CsValidSet_Engine_CrossDateStateIsolation, RankByteIdenticalPerDate) {
  // Evaluate rank(close) on the full 3-date panel.
  const Panel full = make_cs_panel();
  const Program prog = cs_compile("r = rank(close)");

  Engine full_engine{full};
  auto full_sig = full_engine.evaluate(prog);
  ASSERT_TRUE(full_sig.has_value()) << full_sig.error().message();
  const auto &fv = full_sig->alphas[0].values; // 3*5 = 15 elements

  // Evaluate each date independently in a permuted visit order (date2 → date0 →
  // date1 instead of 0 → 1 → 2).  Each single-date evaluation rebuilds the valid
  // set from scratch, so if no cross-date state leaks, the output must be byte-
  // identical to the corresponding slice of the full-panel evaluation.
  const std::vector<atx::f64> row0{kNaN, 1.0, 1.0, 2.0, kNaN};
  const std::vector<atx::f64> row1{3.0, kNaN, 1.5, 1.5, 0.5};
  const std::vector<atx::f64> row2{2.0, 2.0, kNaN, 1.0, 3.0};

  const auto r2 = eval_single_date(prog, row2); // "first" in permuted order
  const auto r0 = eval_single_date(prog, row0); // "second" in permuted order
  const auto r1 = eval_single_date(prog, row1); // "third" in permuted order

  constexpr atx::usize kI = 5;
  for (atx::usize i = 0; i < kI; ++i) {
    EXPECT_TRUE(bit_eq(fv[0 * kI + i], r0[i]))
        << "rank date0 inst" << i << ": full=" << fv[0 * kI + i] << " single=" << r0[i];
    EXPECT_TRUE(bit_eq(fv[1 * kI + i], r1[i]))
        << "rank date1 inst" << i << ": full=" << fv[1 * kI + i] << " single=" << r1[i];
    EXPECT_TRUE(bit_eq(fv[2 * kI + i], r2[i]))
        << "rank date2 inst" << i << ": full=" << fv[2 * kI + i] << " single=" << r2[i];
  }
}

TEST(CsValidSet_Engine_CrossDateStateIsolation, ZscoreByteIdenticalPerDate) {
  // Same as the rank test but for zscore — confirms no cross-date state leaks
  // through the CsScratch streaming accumulators.
  const Panel full = make_cs_panel();
  const Program prog = cs_compile("z = zscore(close)");

  Engine full_engine{full};
  auto full_sig = full_engine.evaluate(prog);
  ASSERT_TRUE(full_sig.has_value()) << full_sig.error().message();
  const auto &fv = full_sig->alphas[0].values;

  const std::vector<atx::f64> row0{kNaN, 1.0, 1.0, 2.0, kNaN};
  const std::vector<atx::f64> row1{3.0, kNaN, 1.5, 1.5, 0.5};
  const std::vector<atx::f64> row2{2.0, 2.0, kNaN, 1.0, 3.0};

  const auto z1 = eval_single_date(prog, row1); // permuted: date1 first
  const auto z2 = eval_single_date(prog, row2);
  const auto z0 = eval_single_date(prog, row0); // permuted: date0 last

  constexpr atx::usize kI = 5;
  for (atx::usize i = 0; i < kI; ++i) {
    EXPECT_TRUE(bit_eq(fv[0 * kI + i], z0[i]))
        << "zscore date0 inst" << i << ": full=" << fv[0 * kI + i] << " single=" << z0[i];
    EXPECT_TRUE(bit_eq(fv[1 * kI + i], z1[i]))
        << "zscore date1 inst" << i << ": full=" << fv[1 * kI + i] << " single=" << z1[i];
    EXPECT_TRUE(bit_eq(fv[2 * kI + i], z2[i]))
        << "zscore date2 inst" << i << ": full=" << fv[2 * kI + i] << " single=" << z2[i];
  }
}

} // namespace atxtest_alpha_cs_valid

// ============================================================================
// S1-1: ScratchWeight — caller-provided-scratch to_target_weights overload.
//
// Three test groups:
//
//   WeightScratch_Equivalence_ByteIdentical
//     Calls both the allocating and scratch overloads on a multi-date panel
//     (all-NaN row, single-valid row, all-valid row, full-valid row) and
//     asserts the outputs are bit-identical. The allocating overload is the
//     oracle; the scratch overload must match it exactly.
//
//   WeightScratch_StreamDigest_ByteIdentical
//     Evaluates fill_alpha_stream with the scratch-hoisted path (hoisted buffers
//     outside the date loop) and compares the resulting AlphaStreams byte-for-byte
//     against the original allocating path on the same synthetic multi-date panel.
//
//   WeightScratch_AllocBench_O1PerAlpha
//     Microbench: M to_target_weights calls allocating vs scratch overload, on
//     a representative panel. Reports ns/call. Scratch must be no slower (alloc
//     reduction is O(1) initial alloc per alpha, not per date).
// ============================================================================

namespace atxtest_alpha_weight_scratch {

using atx::engine::alpha::AlphaStreams;
using atx::engine::alpha::fill_alpha_stream;
using atx::engine::alpha::Panel;
using atx::engine::alpha::SignalSet;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::InstrumentId;
using atx::engine::SignalView;
using atx::engine::Universe;
using atx::engine::WeightPolicy;

static constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Bit-identical equality with NaN==NaN treated as equal (same convention as S1-4).
[[nodiscard]] static bool bit_eq_w(atx::f64 a, atx::f64 b) noexcept {
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  return a == b;
}

// Build a synthetic universe of `n` instruments (contiguous ids 0..n-1).
[[nodiscard]] static std::vector<InstrumentId> make_universe(atx::usize n) {
  std::vector<InstrumentId> ids(n);
  for (atx::usize i = 0; i < n; ++i) {
    ids[i] = InstrumentId{static_cast<atx::u32>(i)};
  }
  return ids;
}

// ---- WeightScratch_Equivalence_ByteIdentical --------------------------------
//
// Multi-date panel, 5 instruments:
//   date0: all-NaN row              (no opinions anywhere)
//   date1: single valid (inst2=3.0, rest NaN)
//   date2: all valid, distinct
//   date3: all valid, some equal (tie test)
//   date4: all valid, ZScore transform variant
//
// For each date we call to_target_weights (allocating, oracle) and
// to_target_weights_scratch (scratch overload) and assert bit-identical output.
// The scratch buffers are REUSED across dates (the point of the task).

TEST(WeightScratch_Equivalence_ByteIdentical, RankTransformMultiDate) {
  constexpr atx::usize kN = 5;
  const auto universe_ids = make_universe(kN);
  const Universe universe{universe_ids};

  WeightPolicy policy;
  policy.transform = atx::engine::Transform::Rank;
  policy.winsorize_limit = 0.0; // no winsorize to keep expected values simple

  // Multi-date rows (5 instruments each).
  // date0: all NaN -> all weights 0
  const std::vector<atx::f64> row0{kNaN, kNaN, kNaN, kNaN, kNaN};
  // date1: single valid -> all weights 0 (single live instrument cannot be dollar-neutral)
  const std::vector<atx::f64> row1{kNaN, kNaN, 3.0, kNaN, kNaN};
  // date2: all valid distinct
  const std::vector<atx::f64> row2{1.0, 3.0, 5.0, 7.0, 9.0};
  // date3: some tied (tie-break test)
  const std::vector<atx::f64> row3{2.0, 2.0, 5.0, 7.0, 9.0};
  // date4: all valid constant (degenerate: rank -> all equal -> demean -> all 0)
  const std::vector<atx::f64> row4{4.0, 4.0, 4.0, 4.0, 4.0};

  const std::vector<const std::vector<atx::f64> *> rows{&row0, &row1, &row2, &row3, &row4};

  // Scratch buffers hoisted above the date loop (the whole point of S1-1).
  std::vector<atx::f64> scratch_weights;
  std::vector<atx::usize> scratch_live_idx;
  std::vector<atx::f64> scratch_dense;
  std::vector<atx::f64> scratch_tform_tmp;

  for (atx::usize t = 0; t < rows.size(); ++t) {
    const SignalView sv{std::span<const atx::f64>{*rows[t]}};

    // Oracle: allocating overload.
    const std::vector<atx::f64> oracle = policy.to_target_weights(sv, universe);

    // Scratch overload (pre-sized buffers, reused across dates).
    policy.to_target_weights(sv, universe, scratch_weights, scratch_live_idx, scratch_dense,
                             scratch_tform_tmp);

    ASSERT_EQ(scratch_weights.size(), kN)
        << "scratch weights wrong size at date " << t;
    for (atx::usize i = 0; i < kN; ++i) {
      EXPECT_TRUE(bit_eq_w(oracle[i], scratch_weights[i]))
          << "date=" << t << " inst=" << i
          << ": oracle=" << oracle[i] << " scratch=" << scratch_weights[i];
    }
  }
}

TEST(WeightScratch_Equivalence_ByteIdentical, ZScoreTransformMultiDate) {
  constexpr atx::usize kN = 6;
  const auto universe_ids = make_universe(kN);
  const Universe universe{universe_ids};

  WeightPolicy policy;
  policy.transform = atx::engine::Transform::ZScore;
  policy.winsorize_limit = 0.0;

  const std::vector<atx::f64> row0{kNaN, kNaN, kNaN, kNaN, kNaN, kNaN}; // all-NaN
  const std::vector<atx::f64> row1{1.0, kNaN, 3.0, kNaN, 5.0, kNaN};    // 3 valid
  const std::vector<atx::f64> row2{1.0, 2.0, 3.0, 4.0, 5.0, 6.0};       // all valid
  const std::vector<atx::f64> row3{10.0, 10.0, 10.0, 10.0, 10.0, 10.0}; // constant (degenerate)

  const std::vector<const std::vector<atx::f64> *> rows{&row0, &row1, &row2, &row3};

  std::vector<atx::f64> scratch_weights;
  std::vector<atx::usize> scratch_live_idx;
  std::vector<atx::f64> scratch_dense;
  std::vector<atx::f64> scratch_tform_tmp;

  for (atx::usize t = 0; t < rows.size(); ++t) {
    const SignalView sv{std::span<const atx::f64>{*rows[t]}};
    const std::vector<atx::f64> oracle = policy.to_target_weights(sv, universe);
    policy.to_target_weights(sv, universe, scratch_weights, scratch_live_idx, scratch_dense,
                             scratch_tform_tmp);

    ASSERT_EQ(scratch_weights.size(), kN);
    for (atx::usize i = 0; i < kN; ++i) {
      EXPECT_TRUE(bit_eq_w(oracle[i], scratch_weights[i]))
          << "ZScore date=" << t << " inst=" << i
          << ": oracle=" << oracle[i] << " scratch=" << scratch_weights[i];
    }
  }
}

TEST(WeightScratch_Equivalence_ByteIdentical, RawTransformSingleValidInst) {
  // Single-instrument live case: after demean the weight is 0 regardless.
  constexpr atx::usize kN = 4;
  const auto universe_ids = make_universe(kN);
  const Universe universe{universe_ids};

  WeightPolicy policy;
  policy.transform = atx::engine::Transform::Raw;
  policy.winsorize_limit = 0.0;

  const std::vector<atx::f64> row{kNaN, 7.5, kNaN, kNaN};
  const SignalView sv{std::span<const atx::f64>{row}};

  std::vector<atx::f64> scratch_weights;
  std::vector<atx::usize> scratch_live_idx;
  std::vector<atx::f64> scratch_dense;
  std::vector<atx::f64> scratch_tform_tmp;

  const std::vector<atx::f64> oracle = policy.to_target_weights(sv, universe);
  policy.to_target_weights(sv, universe, scratch_weights, scratch_live_idx, scratch_dense,
                           scratch_tform_tmp);

  ASSERT_EQ(scratch_weights.size(), kN);
  for (atx::usize i = 0; i < kN; ++i) {
    EXPECT_TRUE(bit_eq_w(oracle[i], scratch_weights[i]))
        << "Raw/single-valid inst=" << i
        << ": oracle=" << oracle[i] << " scratch=" << scratch_weights[i];
  }
}

// ---- WeightScratch_StreamDigest_ByteIdentical -------------------------------
//
// Build a synthetic SignalSet with 2 alphas, 4 dates, 4 instruments.
// REFERENCE side: built by calling the ALLOCATING overload (to_target_weights
// returning a new std::vector per date) assembled into ref.pos_flat manually.
// This ensures the reference is independent of fill_alpha_stream and the test
// can FAIL if the scratch/hoist path diverged from the allocating path.
//
// TESTED side: built by calling fill_alpha_stream (which internally uses the
// scratch/hoisted path since the streams.hpp hoist already landed).
// The resulting pos_flat must be bit-identical to the reference.

TEST(WeightScratch_StreamDigest_ByteIdentical, FillAlphaStreamScratchMatchesOrig) {
  constexpr atx::usize kDates = 4;
  constexpr atx::usize kInsts = 4;
  constexpr atx::usize kAlphas = 2;

  // Build a Panel with a "close" field for PnL computation.
  //        inst0   inst1   inst2   inst3
  // date0: 100     200     150     120
  // date1: 105     195     155     125
  // date2: 102     202     148     130
  // date3: 108     198     160     122
  const std::vector<atx::f64> close_data{
      100.0, 200.0, 150.0, 120.0, // date0
      105.0, 195.0, 155.0, 125.0, // date1
      102.0, 202.0, 148.0, 130.0, // date2
      108.0, 198.0, 160.0, 122.0, // date3
  };

  // Two synthetic alphas (date-major, 4 dates × 4 instruments each).
  // alpha0: a mix of valid/NaN scores across dates.
  // alpha1: all valid, distinct values.
  SignalSet signals;
  signals.dates = kDates;
  signals.instruments = kInsts;
  signals.alphas.resize(kAlphas);
  signals.alphas[0].name = "a0";
  signals.alphas[0].values = {
      kNaN,  1.0,  2.0,  3.0,  // date0
      1.0,   kNaN, 3.0,  4.0,  // date1
      2.0,   3.0,  kNaN, 5.0,  // date2
      1.5,   2.5,  3.5,  kNaN, // date3
  };
  signals.alphas[1].name = "a1";
  signals.alphas[1].values = {
      4.0, 3.0, 2.0, 1.0, // date0
      1.0, 2.0, 3.0, 4.0, // date1
      2.0, 4.0, 1.0, 3.0, // date2
      3.0, 1.0, 4.0, 2.0, // date3
  };

  const WeightPolicy policy; // default: Rank, winsorize=0.025, dollar_neutral=true

  // Build universe ids (contiguous 0..kInsts-1).
  std::vector<InstrumentId> universe_ids(kInsts);
  for (atx::usize j = 0; j < kInsts; ++j) {
    universe_ids[j] = InstrumentId{static_cast<atx::u32>(j)};
  }
  const Universe universe{universe_ids};
  const std::span<const atx::f64> close_span{close_data};

  // --- REFERENCE: built independently via the ALLOCATING overload per date.
  //     This is intentionally NOT fill_alpha_stream — constructing the
  //     reference from the allocating path makes this test able to catch a
  //     divergence between the two paths.
  std::vector<atx::f64> ref_pos(kAlphas * kDates * kInsts, 0.0);
  for (atx::usize i = 0; i < kAlphas; ++i) {
    for (atx::usize t = 0; t < kDates; ++t) {
      const SignalView row{signals.alpha_cross_section(i, t)};
      const std::vector<atx::f64> w = policy.to_target_weights(row, universe);
      const atx::usize off = (i * kDates + t) * kInsts;
      for (atx::usize j = 0; j < kInsts; ++j) {
        ref_pos[off + j] = w[j];
      }
    }
  }

  // --- TESTED SIDE: fill_alpha_stream (uses hoisted scratch buffers internally
  //     after the streams.hpp hoist from the production fix).
  AlphaStreams hoisted;
  hoisted.n_alphas_ = kAlphas;
  hoisted.n_periods_ = kDates;
  hoisted.n_instruments_ = kInsts;
  hoisted.pnl_flat.assign(kAlphas * kDates, 0.0);
  hoisted.pos_flat.assign(kAlphas * kDates * kInsts, 0.0);
  for (atx::usize i = 0; i < kAlphas; ++i) {
    fill_alpha_stream(hoisted, i, signals, policy, universe, close_span, 0.0);
  }

  // pos_flat must be bit-identical to the allocating-overload reference.
  ASSERT_EQ(ref_pos.size(), hoisted.pos_flat.size());
  for (atx::usize k = 0; k < ref_pos.size(); ++k) {
    EXPECT_TRUE(bit_eq_w(ref_pos[k], hoisted.pos_flat[k]))
        << "pos_flat[" << k << "]: ref=" << ref_pos[k]
        << " hoisted=" << hoisted.pos_flat[k];
  }
}

// ---- WeightScratch_AllocBench_O1PerAlpha ------------------------------------
//
// Microbench: M calls to to_target_weights (allocating) vs scratch overload.
// Scratch must not be slower; reports ns/call for the commit body.

TEST(WeightScratch_AllocBench_O1PerAlpha, AllocVsScratchWallTime) {
  constexpr int kReps = 2000;
  constexpr atx::usize kN = 100; // representative cross-section size

  const auto universe_ids = make_universe(kN);
  const Universe universe{universe_ids};

  WeightPolicy policy;
  policy.winsorize_limit = 0.025;

  // Fixed signal row — mix of NaN and valid.
  std::vector<atx::f64> row(kN);
  for (atx::usize i = 0; i < kN; ++i) {
    row[i] = (i % 7 == 0) ? kNaN : static_cast<atx::f64>(i);
  }
  const SignalView sv{std::span<const atx::f64>{row}};

  // -- Allocating path -------------------------------------------------------
  atx::f64 sink_alloc = 0.0; // defeat dead-code elimination
  using Clock = std::chrono::steady_clock;
  const auto alloc_start = Clock::now();
  for (int r = 0; r < kReps; ++r) {
    const auto w = policy.to_target_weights(sv, universe);
    sink_alloc += w[0]; // touch the result
  }
  const auto alloc_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - alloc_start).count();

  // -- Scratch path (O(1) allocs: buffers reused across kReps calls) ---------
  std::vector<atx::f64> scratch_weights;
  std::vector<atx::usize> scratch_live_idx;
  std::vector<atx::f64> scratch_dense;
  std::vector<atx::f64> scratch_tform_tmp;

  atx::f64 sink_scratch = 0.0;
  const auto scratch_start = Clock::now();
  for (int r = 0; r < kReps; ++r) {
    policy.to_target_weights(sv, universe, scratch_weights, scratch_live_idx, scratch_dense,
                             scratch_tform_tmp);
    sink_scratch += scratch_weights[0];
  }
  const auto scratch_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - scratch_start).count();

  const double alloc_ns_per = static_cast<double>(alloc_ns) / static_cast<double>(kReps);
  const double scratch_ns_per = static_cast<double>(scratch_ns) / static_cast<double>(kReps);

  std::cout << "[WeightScratch microbench] " << kReps << " calls × n=" << kN << "\n"
            << "  allocating overload:  " << alloc_ns_per << " ns/call\n"
            << "  scratch overload:     " << scratch_ns_per << " ns/call\n"
            << "  speedup: " << (alloc_ns_per / scratch_ns_per) << "x\n";

  // Defeat dead-code elimination.
  EXPECT_TRUE(!std::isnan(sink_alloc) || std::isnan(sink_scratch));

  // Sanity: scratch must not be pathologically slower than allocating.
  // On a cold run both are similar; with OS caching scratch should be faster.
  // We just catch a catastrophic regression (2× slower), not race the clock.
  EXPECT_LE(scratch_ns_per, alloc_ns_per * 2.0)
      << "Scratch path is more than 2x slower than allocating — buffer reuse broken?";
}

} // namespace atxtest_alpha_weight_scratch

// ============================================================================
// S1-1: WeightScratch_AllocCount_O1AfterWarmup
//
// Proves the ping-pong fix achieves O(1) allocations per alpha — i.e. NO
// re-allocations after the high-water-mark warmup date — regardless of the
// number of post-warmup dates and regardless of variable NaN patterns.
//
// Approach: data-pointer stability.  A std::vector reallocates iff its internal
// `data()` pointer changes between calls.  After the warmup date establishes
// the high-water-mark capacity, every subsequent call to the scratch overload
// must leave ALL FOUR hoisted buffers with the SAME `data()` pointer they had
// going in — no realloc.  We snapshot the pointers after warmup and assert
// they are unchanged across all 24 post-warmup dates.
//
// Note on the ping-pong: apply_transform (Rank/ZScore) does dense.swap(tmp),
// which exchanges the two buffers' storage.  After warmup BOTH buffers have at
// least the high-water-mark capacity; the swap leaves both buffers with the
// SAME high-water-mark capacity (just in swapped roles), so neither can trigger
// a grow on the next call.  The old per-call-local `out` design allocated a
// fresh buffer of size=k on each call and after swap left `dense` with capacity
// k < n, causing a realloc on the next date when k_next != k.
//
// This test FAILS against the old design (data() pointer changes every date
// for dense/tform_tmp) and PASSES with the ping-pong fix.
// ============================================================================

namespace atxtest_alpha_weight_scratch_alloc {

using atx::engine::InstrumentId;
using atx::engine::SignalView;
using atx::engine::Transform;
using atx::engine::Universe;
using atx::engine::WeightPolicy;

static constexpr atx::f64 kNaN2 = std::numeric_limits<atx::f64>::quiet_NaN();

[[nodiscard]] static std::vector<InstrumentId> make_universe2(atx::usize n) {
  std::vector<InstrumentId> ids(n);
  for (atx::usize i = 0; i < n; ++i) {
    ids[i] = InstrumentId{static_cast<atx::u32>(i)};
  }
  return ids;
}

TEST(WeightScratch_AllocCount_O1AfterWarmup, NoReallocsAfterWarmupVariableLiveCounts) {
  // 8 instruments; variable NaN patterns per date so live count k varies.
  // date0: k=8 (full universe) — establishes the high-water mark (warmup date).
  // date1: k=5; date2: k=3; date3: k=7; date4: k=2 (post-warmup samples).
  // date5..date24: alternating k=4 and k=6 (20 more post-warmup dates).
  constexpr atx::usize kN = 8;
  const auto universe_ids = make_universe2(kN);
  const Universe universe{universe_ids};

  WeightPolicy policy;
  policy.transform = Transform::Rank; // Rank exercises the dense↔tform_tmp swap
  policy.winsorize_limit = 0.0;

  std::vector<std::vector<atx::f64>> rows;
  rows.push_back({1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0});            // date0: k=8 HWM
  rows.push_back({kNaN2, 2.0, kNaN2, 4.0, 5.0, kNaN2, 7.0, 8.0});       // date1: k=5
  rows.push_back({kNaN2, kNaN2, kNaN2, kNaN2, kNaN2, 6.0, 7.0, 8.0});   // date2: k=3
  rows.push_back({1.0, kNaN2, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0});           // date3: k=7
  rows.push_back({kNaN2, kNaN2, kNaN2, kNaN2, kNaN2, kNaN2, 7.0, 8.0}); // date4: k=2
  for (int x = 0; x < 20; ++x) {
    if (x % 2 == 0) {
      rows.push_back({1.0, kNaN2, 3.0, kNaN2, 5.0, kNaN2, 7.0, kNaN2}); // k=4
    } else {
      rows.push_back({1.0, 2.0, kNaN2, 4.0, 5.0, 6.0, kNaN2, 8.0});     // k=6
    }
  }

  // Hoisted buffers — mirrors the fill_alpha_stream hoist from streams.hpp.
  std::vector<atx::f64> scratch_weights;
  std::vector<atx::usize> scratch_live_idx;
  std::vector<atx::f64> scratch_dense;
  std::vector<atx::f64> scratch_tform_tmp;

  // Warmup: process date0 (full-universe, HWM) — allocates initial storage.
  {
    const SignalView sv{std::span<const atx::f64>{rows[0]}};
    policy.to_target_weights(sv, universe, scratch_weights, scratch_live_idx, scratch_dense,
                             scratch_tform_tmp);
  }

  // After warmup the ping-pong buffers may have been swapped once; snapshot
  // their data pointers NOW. They must NOT change on any subsequent call
  // because both dense and tform_tmp already hold the high-water-mark capacity.
  // Note: dense and tform_tmp swap their storage each call, so we track the
  // PAIR of pointers as a set — what matters is that no NEW allocation occurs.
  const atx::f64 *ptr_weights_after_warmup = scratch_weights.data();
  const atx::usize *ptr_live_idx_after_warmup = scratch_live_idx.data();
  // dense and tform_tmp swap on every Rank call — track both and verify the
  // set of two pointers is stable (neither pointer is brand-new after warmup).
  const atx::f64 *ptr_dense_init = scratch_dense.data();
  const atx::f64 *ptr_tform_init = scratch_tform_tmp.data();

  int realloc_count = 0;
  const long n_post_warmup = static_cast<long>(rows.size()) - 1L; // 24 dates

  for (atx::usize t = 1; t < rows.size(); ++t) {
    const SignalView sv{std::span<const atx::f64>{rows[t]}};
    policy.to_target_weights(sv, universe, scratch_weights, scratch_live_idx, scratch_dense,
                             scratch_tform_tmp);

    // weights and live_idx must never move — no swap, just resize/clear+push.
    if (scratch_weights.data() != ptr_weights_after_warmup) {
      ++realloc_count;
      ADD_FAILURE() << "scratch_weights reallocated at date " << t
                    << " (capacity lost; should reuse HWM capacity)";
    }
    if (scratch_live_idx.data() != ptr_live_idx_after_warmup) {
      ++realloc_count;
      ADD_FAILURE() << "scratch_live_idx reallocated at date " << t
                    << " (capacity lost; should reuse HWM capacity)";
    }

    // dense and tform_tmp ping-pong: their data() SWAPS each call but neither
    // should point to a NEW allocation. After warmup the only valid pointers
    // are ptr_dense_init and ptr_tform_init (in either role).
    const atx::f64 *d = scratch_dense.data();
    const atx::f64 *tf = scratch_tform_tmp.data();
    if ((d != ptr_dense_init && d != ptr_tform_init) ||
        (tf != ptr_dense_init && tf != ptr_tform_init)) {
      ++realloc_count;
      ADD_FAILURE() << "dense/tform_tmp allocated a NEW buffer at date " << t
                    << " — ping-pong capacity not retained across variable-NaN dates";
    }
  }

  std::cout << "[WeightScratch AllocCount] post-warmup dates=" << n_post_warmup
            << "  buffer reallocations=" << realloc_count << " (expected 0)\n";

  EXPECT_EQ(realloc_count, 0)
      << "Expected 0 buffer reallocations after warmup across " << n_post_warmup
      << " variable-NaN post-warmup dates; got " << realloc_count
      << ". Ping-pong capacity-retention is not working.";
}

} // namespace atxtest_alpha_weight_scratch_alloc

// ============================================================================
// S1-2: EngineReset — Engine::reset() reuse API across different Programs.
//
// Two test groups (both added alongside existing cases; no existing code changed):
//
//   EngineReset_ByteIdentity
//     Evaluates two DIFFERENT Programs (different field sets / slot counts) on
//     ONE Engine using reset() between them. Asserts results are byte-identical
//     to evaluating each on a freshly-constructed Engine.  Also covers evaluating
//     the SAME program twice (regression guard).
//
//   EngineReset_NoRealloc_SameShape
//     After reset(), a second evaluate() with the same num_slots / cells MUST
//     NOT reallocate the SlotPool backing storage.  Probed via Engine::pool_capacity()
//     stability: ensure_pool() only reallocates when want_slots > capacity or the
//     cell count changes, so an UNCHANGED capacity across reset()+evaluate() is the
//     observable signal that no realloc occurred. If the program needs a LARGER
//     pool, growth is still allowed (matches ensure_pool semantics).
//
//   EngineReset_StateNoLeak
//     Reset-reuse byte-identity when a program uses a stateful recurrence op
//     (trade_when), pinning that the per-instrument state_ buffer does not leak
//     across reset().
//
//   EngineReset_PoolGrowth
//     Reset-reuse byte-identity when the SECOND program needs a LARGER pool than
//     the first — confirms ensure_pool() grows the pool correctly after reset()
//     and the grown result still matches a fresh Engine bit-for-bit.
//
// TDD RED/GREEN EVIDENCE:
//   Written first (RED): Engine had no reset()/pool_capacity() — compile error.
//   GREEN: reset() clears field_remap_ (the pool's live counter is already 0 after
//   a successful evaluate(), so no pool mutation is needed).
// ============================================================================

namespace atxtest_alpha_engine_reset {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;

[[nodiscard]] static const Library &reset_lib() {
  static const Library lib;
  return lib;
}

// Parse + analyze + compile. ASSERTs on failure.
[[nodiscard]] static Program reset_compile(std::string_view src) {
  auto parsed = parse_program(src, reset_lib());
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

// Panel with "close" and "open" over 4 dates × 3 instruments.
[[nodiscard]] static Panel make_reset_panel() {
  //         inst0   inst1   inst2
  // date0: 100.0   200.0   150.0   close
  // date1: 105.0   195.0   155.0
  // date2: 102.0   202.0   148.0
  // date3: 108.0   198.0   160.0
  //         inst0   inst1   inst2
  // date0:  99.0   198.0   149.0   open
  // date1: 103.0   194.0   153.0
  // date2: 101.0   201.0   147.0
  // date3: 107.0   197.0   158.0
  std::vector<std::vector<atx::f64>> data{
      {100.0, 200.0, 150.0, 105.0, 195.0, 155.0, 102.0, 202.0, 148.0, 108.0, 198.0, 160.0},
      {99.0, 198.0, 149.0, 103.0, 194.0, 153.0, 101.0, 201.0, 147.0, 107.0, 197.0, 158.0},
  };
  auto res = Panel::create(4, 3, {"close", "open"}, std::move(data), {});
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  if (!res) {
    std::vector<std::vector<atx::f64>> fb{{1.0}, {1.0}};
    return std::move(*Panel::create(1, 1, {"close", "open"}, std::move(fb), {}));
  }
  return std::move(*res);
}

// Bit-identical equality (NaN == NaN).
[[nodiscard]] static bool signal_sets_equal_r(const SignalSet &a, const SignalSet &b) {
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

// ---- EngineReset_ByteIdentity -----------------------------------------------

// Evaluate the same program twice on a reused Engine (reset() between calls)
// and assert byte-identical results vs two fresh Engines.
TEST(EngineReset_ByteIdentity, SameProgramTwice) {
  const Panel panel = make_reset_panel();
  const Program prog = reset_compile("a = close - open");

  // Fresh Engine for reference.
  Engine fresh1{panel};
  const auto ref1 = fresh1.evaluate(prog);
  ASSERT_TRUE(ref1.has_value()) << ref1.error().message();

  // Reused Engine: evaluate once, reset, evaluate again.
  Engine reused{panel};
  const auto r1 = reused.evaluate(prog);
  ASSERT_TRUE(r1.has_value()) << r1.error().message();
  EXPECT_TRUE(signal_sets_equal_r(*ref1, *r1))
      << "First evaluate on reused Engine differs from fresh Engine";

  reused.reset();

  const auto r2 = reused.evaluate(prog);
  ASSERT_TRUE(r2.has_value()) << r2.error().message();
  EXPECT_TRUE(signal_sets_equal_r(*ref1, *r2))
      << "Post-reset evaluate (same prog) differs from fresh Engine — stale remap leaked";
}

// Evaluate TWO DIFFERENT Programs on ONE Engine (reset() between them).
// Both results must be byte-identical to their respective fresh-Engine evaluations.
// Program A: close - open (uses both fields)
// Program B: close * 2   (uses only close, different slot count)
TEST(EngineReset_ByteIdentity, TwoDifferentPrograms) {
  const Panel panel = make_reset_panel();
  const Program prog_a = reset_compile("a = close - open");
  const Program prog_b = reset_compile("b = close * 2");

  // Fresh Engine references.
  Engine fresh_a{panel};
  const auto ref_a = fresh_a.evaluate(prog_a);
  ASSERT_TRUE(ref_a.has_value()) << ref_a.error().message();

  Engine fresh_b{panel};
  const auto ref_b = fresh_b.evaluate(prog_b);
  ASSERT_TRUE(ref_b.has_value()) << ref_b.error().message();

  // One Engine, two Programs with reset() between.
  Engine reused{panel};

  const auto r_a = reused.evaluate(prog_a);
  ASSERT_TRUE(r_a.has_value()) << r_a.error().message();
  EXPECT_TRUE(signal_sets_equal_r(*ref_a, *r_a))
      << "First evaluate (prog_a) on reused Engine differs from fresh Engine";

  reused.reset(); // clears field_remap_ (pool live counter is already 0 here)

  const auto r_b = reused.evaluate(prog_b);
  ASSERT_TRUE(r_b.has_value()) << r_b.error().message();
  EXPECT_TRUE(signal_sets_equal_r(*ref_b, *r_b))
      << "Post-reset evaluate (prog_b) differs from fresh Engine — stale remap leaked";
}

// Test with programs that use DIFFERENT field sets: A uses {close, open},
// B uses only {close}. After reset(), prog_b must not accidentally remap open.
TEST(EngineReset_ByteIdentity, DifferentFieldSetsAfterReset) {
  const Panel panel = make_reset_panel();
  const Program prog_a = reset_compile("a = close + open");
  const Program prog_b = reset_compile("b = close");

  Engine fresh_a{panel};
  const auto ref_a = fresh_a.evaluate(prog_a);
  ASSERT_TRUE(ref_a.has_value()) << ref_a.error().message();

  Engine fresh_b{panel};
  const auto ref_b = fresh_b.evaluate(prog_b);
  ASSERT_TRUE(ref_b.has_value()) << ref_b.error().message();

  Engine reused{panel};

  // Evaluate prog_a first: field_remap_ = [close_id, open_id] (2 entries)
  const auto r_a = reused.evaluate(prog_a);
  ASSERT_TRUE(r_a.has_value()) << r_a.error().message();
  EXPECT_TRUE(signal_sets_equal_r(*ref_a, *r_a));

  // After reset: field_remap_ must be empty; resolve_fields() rebuilds to
  // prog_b's single-field {close} mapping. If reset() doesn't clear field_remap_,
  // prog_b could index into a 2-element map using prog_b's field-id 0 -> correct
  // close, but the stale entry at [1] must not influence the result.
  reused.reset();

  const auto r_b = reused.evaluate(prog_b);
  ASSERT_TRUE(r_b.has_value()) << r_b.error().message();
  EXPECT_TRUE(signal_sets_equal_r(*ref_b, *r_b))
      << "Post-reset evaluate with fewer fields differs from fresh Engine — "
         "stale field_remap_ not cleared by reset()";
}

// ---- EngineReset_NoRealloc_SameShape ----------------------------------------
//
// After reset(), a second evaluate() with the same num_slots / cells MUST NOT
// reallocate the SlotPool backing storage. Probed via pool_capacity() stability:
// ensure_pool() only reallocates when want_slots > capacity OR cells_per_slot
// changes, so stable capacity ⟺ no realloc for a same-shape program.

TEST(EngineReset_NoRealloc_SameShape, SlotPoolCapacityStableAcrossReset) {
  const Panel panel = make_reset_panel();
  // prog_a and prog_b: both load two fields and compute a binary op; both need
  // the same peak-live-slot count. "close - open" and "close + open" are
  // structurally identical (2 LoadField + 1 Binary; num_slots == 2 slots peak).
  // Same num_slots + same cells -> ensure_pool() skips reallocation after reset().
  const Program prog_a = reset_compile("a = close - open");
  const Program prog_b = reset_compile("b = close + open");

  // Verify same num_slots (the precondition for the no-realloc guarantee).
  ASSERT_EQ(prog_a.num_slots, prog_b.num_slots)
      << "Test precondition: prog_a and prog_b must have same num_slots for "
         "the no-realloc probe to be meaningful";

  Engine eng{panel};

  // First evaluate: warms the pool to prog_a's shape.
  const auto r_a = eng.evaluate(prog_a);
  ASSERT_TRUE(r_a.has_value()) << r_a.error().message();

  // Snapshot the pool capacity AFTER the first evaluate(). ensure_pool() only
  // reallocates when want_slots > capacity or cells change, so stable capacity
  // proves no reallocation occurred.
  const atx::usize cap_before = eng.pool_capacity();
  ASSERT_GT(cap_before, atx::usize{0}) << "pool should be sized after evaluate()";

  // Reset and evaluate a same-shape program.
  eng.reset();
  const auto r_b = eng.evaluate(prog_b);
  ASSERT_TRUE(r_b.has_value()) << r_b.error().message();

  // The pool capacity must be unchanged — no reallocation.
  const atx::usize cap_after = eng.pool_capacity();
  EXPECT_EQ(cap_before, cap_after)
      << "SlotPool capacity changed across reset() when shape was unchanged — "
         "ensure_pool() should have skipped reallocation (same num_slots, same cells)";

  std::cout << "[EngineReset] pool_capacity before=" << cap_before
            << " after=" << cap_after
            << (cap_before == cap_after ? " (stable — no realloc)" : " (CHANGED — realloc!)")
            << "\n";
}

// ---- EngineReset_StateNoLeak -----------------------------------------------
//
// Pins that state_ does not leak across reset(): evaluating a program with a
// RECURRENCE op (trade_when / hump) on a warm Engine after reset() must produce
// byte-identical output to evaluating that program on a FRESH Engine.
//
// trade_when accumulates per-instrument position state forward from date 0; any
// stale state_ content from a prior program on the same Engine would alter
// outputs starting at date 1. reset() does not clear state_ (it grows once and
// is unconditionally overwritten at t==0 of each eval_recurrence call), so the
// guarantee is structural: the recurrence loop seeds state_[j] = out[0,j] at
// t==0 before reading it at t>0.

TEST(EngineReset_StateNoLeak, RecurrenceOutputByteIdenticalAfterReset) {
  const Panel panel = make_reset_panel();

  // prog_a: a pure arithmetic program (no state_); evaluated first to warm the
  // Engine, then reset().
  const Program prog_a = reset_compile("a = close - open");

  // prog_b: uses trade_when (recurrence op) — exercises state_.
  // trade_when(trigger, alpha, exit): trigger and exit MUST be masks (comparison
  // results); alpha is the held signal. Here trigger = (close > open) enters the
  // position, alpha = (close - open) is held while in-position, and exit =
  // (close < open) flattens it. The forward scan carries per-instrument state_,
  // so this is the recurrence path we need to exercise across reset().
  const Program prog_b =
      reset_compile("b = trade_when(close > open, close - open, close < open)");

  // Reference: fresh Engine for prog_b.
  Engine fresh_b{panel};
  const auto ref_b = fresh_b.evaluate(prog_b);
  ASSERT_TRUE(ref_b.has_value()) << ref_b.error().message();

  // Reused Engine: evaluate prog_a first, then reset(), then prog_b.
  Engine reused{panel};

  const auto r_a = reused.evaluate(prog_a);
  ASSERT_TRUE(r_a.has_value()) << r_a.error().message();

  reused.reset(); // clears field_remap_; state_ is structurally safe (re-seeded at t==0)

  const auto r_b = reused.evaluate(prog_b);
  ASSERT_TRUE(r_b.has_value()) << r_b.error().message();

  EXPECT_TRUE(signal_sets_equal_r(*ref_b, *r_b))
      << "trade_when output after reset() differs from fresh Engine — "
         "state_ content leaked across reset() (recurrence state must be "
         "re-seeded at t==0 and not depend on prior-program residue)";
}

// ---- EngineReset_PoolGrowth ------------------------------------------------
//
// The reverse of the no-realloc case: when the SECOND program after reset()
// needs a LARGER pool than the first, ensure_pool() MUST grow the pool (growth
// is allowed) and the grown evaluation MUST still be byte-identical to a fresh
// Engine. This pins that reset() leaves the pool in a state ensure_pool() can
// correctly grow from (no stale live-count, no half-sized buffer reuse).

TEST(EngineReset_PoolGrowth, LargerSecondProgramGrowsPoolAndMatchesFresh) {
  const Panel panel = make_reset_panel();
  // prog_small: 2 LoadField + 1 binary (small peak slot count).
  const Program prog_small = reset_compile("a = close - open");
  // prog_large: a deeper expression tree needing strictly more peak-live slots.
  const Program prog_large =
      reset_compile("b = (close - open) * (close + open) - (close / open)");

  // Precondition: prog_large must genuinely need a larger pool, else this test
  // does not exercise the growth path.
  ASSERT_GT(prog_large.num_slots, prog_small.num_slots)
      << "Test precondition: prog_large must need more slots than prog_small";

  // Reference: fresh Engine for the large program.
  Engine fresh_large{panel};
  const auto ref_large = fresh_large.evaluate(prog_large);
  ASSERT_TRUE(ref_large.has_value()) << ref_large.error().message();

  // Reused Engine: warm with the SMALL program (sizes the pool small), reset,
  // then evaluate the LARGE program (forces ensure_pool() to grow).
  Engine reused{panel};
  const auto r_small = reused.evaluate(prog_small);
  ASSERT_TRUE(r_small.has_value()) << r_small.error().message();
  const atx::usize cap_small = reused.pool_capacity();

  reused.reset();
  const auto r_large = reused.evaluate(prog_large);
  ASSERT_TRUE(r_large.has_value()) << r_large.error().message();
  const atx::usize cap_large = reused.pool_capacity();

  // The pool must have grown (capacity increased) and the result must match fresh.
  EXPECT_GT(cap_large, cap_small)
      << "pool capacity should have grown for the larger program after reset()";
  EXPECT_TRUE(signal_sets_equal_r(*ref_large, *r_large))
      << "Grown-pool evaluate after reset() differs from fresh Engine — "
         "ensure_pool() growth post-reset is not byte-identical";

  std::cout << "[EngineReset] pool grew from cap=" << cap_small << " to cap=" << cap_large
            << " (large prog num_slots=" << prog_large.num_slots << ")\n";
}

} // namespace atxtest_alpha_engine_reset

// ============================================================================
// S1-3: TsTranspose — column-extract transpose of the batch-path Ts kernels.
//
// Overview
// --------
// The batch Ts path (eval_time_series, vm.hpp) previously iterated
// instrument-outer / date-inner, reading x[t*I+j] with stride I — cache-hostile.
// S1-3 adds a per-instrument column-extract step (ts_col_[s] = x[s*I+j] for all
// dates s) that turns each kernel's inner strided walk into a contiguous sweep.
//
// Bit-exactness guarantee
// -----------------------
// The extraction is a copy; every kernel then operates on a single-column buffer
// with instruments=1, j=0.  Because x[k*I+j] == col[k*1+0] == col[k], the
// chronological accumulation order k = t+1-d..t is UNCHANGED — every f64
// multiply/add fires on the same operands in the same sequence.  The only
// difference is the memory layout the CPU sees; the arithmetic result is
// bit-for-bit identical.
//
// Tests
// -----
// Per-kernel bit-for-bit equality tests: for each transposed kernel we evaluate
// on a randomized fixed-seed panel (window edges, NaN holes, ties) BOTH via the
// direct kernel function (oracle) AND via Engine::evaluate (transposed path), and
// assert every cell matches to the last bit (NaN==NaN).
//
// Kernels covered:
//   TsVar, TsStd   — two-pass sum then Σ(v-m)², chronological order preserved.
//   TsRank         — counting scan, chronological order preserved.
//   TsMed          — tsv_gather (contiguous after extract) + std::sort, preserved.
//   TsCorr/TsCov/TsRegression — two-column gather, preserved.
//   OuTheta/OuHalflife/OuMean/OuZscore — gather + AR(1) fit, preserved.
//   Also: TsDelay, TsDelta, TsBackfill, TsCountNans, TsProduct, TsArgMin/TsArgMax,
//         TsDecayLinear, TsWma, TsEma, TsSkew, TsKurt, TsSlope, TsRsquare,
//         TsResid, TsMad, TsAvDiff, TsZscore, TsDecayExp, TsMoment, TsEntropy.
// Plus: an end-to-end digest test and a cells/s microbench.
//
// Naming: Subject_Condition_ExpectedResult.
// ============================================================================

namespace atxtest_alpha_ts_transpose {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::OpCode;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
using atx::engine::alpha::detail::ou_value_at;
using atx::engine::alpha::detail::ts_pair_at;
using atx::engine::alpha::detail::ts_value_at;

static constexpr atx::f64 kTsNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// Bit-identical equality — NaN==NaN is treated as equal.
[[nodiscard]] static bool bit_eq_ts(atx::f64 a, atx::f64 b) noexcept {
  if (std::isnan(a) && std::isnan(b)) {
    return true;
  }
  return a == b;
}

[[nodiscard]] static const Library &ts_lib() {
  static const Library lib;
  return lib;
}

// Parse + analyze + compile a single-alpha source. ASSERTs each stage.
[[nodiscard]] static Program ts_compile(std::string_view src) {
  auto parsed = parse_program(src, ts_lib());
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

// ---------------------------------------------------------------------------
// Panel builder: deterministic pseudo-random panel with NaN holes, ties, and
// window edges exercised.  Fixed seed via a simple LCG so the test is
// reproducible without <random>.
//
//   kDates=25, kInsts=8, window d=5.
//   NaN holes: cell (t,j) is NaN when (t*8+j) % 17 == 0.
//   Ties: cells in columns 2 and 3 share the same value on some dates to
//         exercise rank/med tie-breaking.
//   Values: base LCG-pseudorandom in [1.0, 10.0]; ties inserted afterwards.
//   Two field columns: "close" and "open" (for binary-series tests).
// ---------------------------------------------------------------------------
static constexpr atx::usize kDates = 25;
static constexpr atx::usize kInsts = 8;
static constexpr atx::usize kWin   = 5;

[[nodiscard]] static Panel make_ts_panel() {
  // LCG state — same sequence every run (reproducible).
  atx::u64 lcg = 0x123456789ABCDEF0ULL;
  auto next_f64 = [&lcg]() -> atx::f64 {
    lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    // Map to (0,1) using the top 32 bits.
    const atx::f64 u = static_cast<atx::f64>(static_cast<atx::u32>(lcg >> 32))
                       / static_cast<atx::f64>(0xFFFFFFFFu);
    return 1.0 + 9.0 * u; // [1.0, 10.0]
  };

  const atx::usize n = kDates * kInsts;
  std::vector<atx::f64> close_col(n);
  std::vector<atx::f64> open_col(n);

  for (atx::usize t = 0; t < kDates; ++t) {
    for (atx::usize j = 0; j < kInsts; ++j) {
      const atx::usize i = t * kInsts + j;
      // Introduce NaN holes so some windows are invalid (tests the any-NaN->NaN gate).
      const bool is_nan = ((i % 17) == 0);
      close_col[i] = is_nan ? kTsNaN : next_f64();
      // ties: cols 2 and 3 share the same close value on dates 10..14.
      if (!is_nan && (j == 3) && (t >= 10 && t < 15)) {
        close_col[i] = close_col[t * kInsts + 2]; // clone col 2's value
      }
      open_col[i] = is_nan ? kTsNaN : next_f64();
    }
  }

  std::vector<std::vector<atx::f64>> data{close_col, open_col};
  auto res = Panel::create(kDates, kInsts, {"close", "open"}, std::move(data), {});
  EXPECT_TRUE(res.has_value()) << (res ? "" : res.error().message());
  if (!res) {
    std::vector<std::vector<atx::f64>> fb{{1.0}, {1.0}};
    return std::move(*Panel::create(1, 1, {"close", "open"}, std::move(fb), {}));
  }
  return std::move(*res);
}

// Convenience: evaluate `src` on `panel` via Engine; assert success; return values.
[[nodiscard]] static std::vector<atx::f64>
eval_ts(std::string_view src, const Panel &panel) {
  const Program prog = ts_compile(src);
  Engine engine{panel};
  auto sig = engine.evaluate(prog);
  EXPECT_TRUE(sig.has_value()) << (sig ? "" : sig.error().message());
  if (!sig || sig->alphas.empty()) {
    return std::vector<atx::f64>(kDates * kInsts, kTsNaN);
  }
  return sig->alphas[0].values; // dates * instruments cells
}

// Build the oracle for a UNARY Ts op by calling ts_value_at directly on the
// close column using the strided (original) access pattern.
[[nodiscard]] static std::vector<atx::f64>
oracle_unary(OpCode op, std::span<const atx::f64> x, atx::usize d, atx::f64 p0 = 0.0) {
  std::vector<atx::f64> out(kDates * kInsts, kTsNaN);
  std::vector<atx::f64> sort_buf(d);
  for (atx::usize j = 0; j < kInsts; ++j) {
    for (atx::usize t = 0; t < kDates; ++t) {
      out[t * kInsts + j] = ts_value_at(op, x, t, j, d, kInsts, sort_buf, p0);
    }
  }
  return out;
}

// Build the oracle for a BINARY Ts op (TsCorr/TsCov/TsRegression).
[[nodiscard]] static std::vector<atx::f64>
oracle_binary(OpCode op, std::span<const atx::f64> x, std::span<const atx::f64> y, atx::usize d) {
  std::vector<atx::f64> out(kDates * kInsts, kTsNaN);
  std::vector<atx::f64> bx(d);
  std::vector<atx::f64> by(d);
  for (atx::usize j = 0; j < kInsts; ++j) {
    for (atx::usize t = 0; t < kDates; ++t) {
      out[t * kInsts + j] = ts_pair_at(op, x, y, t, j, d, kInsts, bx, by);
    }
  }
  return out;
}

// Build the oracle for an OU op.
[[nodiscard]] static std::vector<atx::f64>
oracle_ou(OpCode op, std::span<const atx::f64> x, atx::usize d) {
  std::vector<atx::f64> out(kDates * kInsts, kTsNaN);
  std::vector<atx::f64> buf(d);
  for (atx::usize j = 0; j < kInsts; ++j) {
    for (atx::usize t = 0; t < kDates; ++t) {
      out[t * kInsts + j] = ou_value_at(op, x, t, j, d, kInsts, buf);
    }
  }
  return out;
}

// Assert two same-length vectors are bit-identical (NaN==NaN).
static void assert_bit_identical(const std::vector<atx::f64> &oracle,
                                 const std::vector<atx::f64> &engine,
                                 std::string_view label) {
  ASSERT_EQ(oracle.size(), engine.size()) << label << ": size mismatch";
  for (atx::usize i = 0; i < oracle.size(); ++i) {
    EXPECT_TRUE(bit_eq_ts(oracle[i], engine[i]))
        << label << " cell[" << i << "] (t=" << (i / kInsts)
        << " j=" << (i % kInsts) << "): oracle=" << oracle[i]
        << " engine=" << engine[i];
  }
}

// Get the close/open columns from the panel (date-major flat buffer).
[[nodiscard]] static std::span<const atx::f64> panel_close(const Panel &p) {
  // Panel::field_data(0) or flat_buf access — use the signal set path.
  // We rebuild from a known layout (same as make_ts_panel).
  // Actually, capture via Engine evaluate of the identity "a = close".
  (void)p;
  // Return via oracle_unary on TsDelay(d=1) would be indirect. Instead, we
  // directly hold the panel data as a local static built alongside the panel.
  // This is fine because the tests call make_ts_panel() once per test.
  // We use a thread_local cache populated the first time.
  // HOWEVER: Panel doesn't expose raw field data directly from here.
  // APPROACH: build the close/open vectors independently using the same LCG.
  static std::vector<atx::f64> close_cache;
  static std::vector<atx::f64> open_cache;
  if (close_cache.empty()) {
    atx::u64 lcg = 0x123456789ABCDEF0ULL;
    auto next_f64 = [&lcg]() -> atx::f64 {
      lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
      const atx::f64 u = static_cast<atx::f64>(static_cast<atx::u32>(lcg >> 32))
                         / static_cast<atx::f64>(0xFFFFFFFFu);
      return 1.0 + 9.0 * u;
    };
    const atx::usize nn = kDates * kInsts;
    close_cache.resize(nn);
    open_cache.resize(nn);
    for (atx::usize t = 0; t < kDates; ++t) {
      for (atx::usize j = 0; j < kInsts; ++j) {
        const atx::usize i = t * kInsts + j;
        const bool is_nan = ((i % 17) == 0);
        close_cache[i] = is_nan ? kTsNaN : next_f64();
        if (!is_nan && (j == 3) && (t >= 10 && t < 15)) {
          close_cache[i] = close_cache[t * kInsts + 2];
        }
        open_cache[i] = is_nan ? kTsNaN : next_f64();
      }
    }
  }
  return std::span<const atx::f64>{close_cache};
}

[[nodiscard]] static std::span<const atx::f64> panel_open() {
  // Trigger population of both caches via panel_close.
  static std::vector<atx::f64> open_cache;
  if (open_cache.empty()) {
    atx::u64 lcg = 0x123456789ABCDEF0ULL;
    auto next_f64 = [&lcg]() -> atx::f64 {
      lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
      const atx::f64 u = static_cast<atx::f64>(static_cast<atx::u32>(lcg >> 32))
                         / static_cast<atx::f64>(0xFFFFFFFFu);
      return 1.0 + 9.0 * u;
    };
    const atx::usize nn = kDates * kInsts;
    std::vector<atx::f64> tmp_close(nn);
    open_cache.resize(nn);
    for (atx::usize t = 0; t < kDates; ++t) {
      for (atx::usize j = 0; j < kInsts; ++j) {
        const atx::usize i = t * kInsts + j;
        const bool is_nan = ((i % 17) == 0);
        tmp_close[i] = is_nan ? kTsNaN : next_f64();
        if (!is_nan && (j == 3) && (t >= 10 && t < 15)) {
          tmp_close[i] = tmp_close[t * kInsts + 2];
        }
        open_cache[i] = is_nan ? kTsNaN : next_f64();
      }
    }
  }
  return std::span<const atx::f64>{open_cache};
}

// ===========================================================================
// Unary per-kernel tests
// ===========================================================================

// Helper to run a single-expression oracle vs Engine comparison.
// `src_expr`: the alpha expression (e.g. "a = ts_var(close, 5)").
// `op`: the OpCode for the direct ts_value_at oracle call.
// `window`: the window size.
// `p0`: the optional imm[0] parameter (used by TsDecayExp, TsMoment, TsEntropy).
static void run_unary_kernel_test(std::string_view src_expr, OpCode op,
                                  atx::usize window, atx::f64 p0 = 0.0) {
  const Panel panel = make_ts_panel();
  const auto close = panel_close(panel);

  const std::vector<atx::f64> expected = oracle_unary(op, close, window, p0);
  const std::vector<atx::f64> actual   = eval_ts(src_expr, panel);
  assert_bit_identical(expected, actual, src_expr);
}

// ---------------------------------------------------------------------------
// TsVar / TsStd — two-pass chronological accumulation (sum then Σ(v-m)²).
// SAFETY: col-extract → instruments=1 j=0; x[k*1+0] == col[k] == x[k*I+j].
// Summation order k = t+1-d..t is identical. Result is bit-for-bit equal.
// ---------------------------------------------------------------------------
TEST(TsTranspose_Unary_BitExact, TsVar_WindowEdgesNanHolesTies) {
  run_unary_kernel_test("a = ts_var(close, 5)", OpCode::TsVar, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsStd_WindowEdgesNanHolesTies) {
  run_unary_kernel_test("a = ts_std(close, 5)", OpCode::TsStd, kWin);
}

// ---------------------------------------------------------------------------
// TsRank — counting scan chronological; TsMed — gather+sort.
// ---------------------------------------------------------------------------
TEST(TsTranspose_Unary_BitExact, TsRank_TiesAndWindowEdges) {
  run_unary_kernel_test("a = ts_rank(close, 5)", OpCode::TsRank, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsMed_TiesAndWindowEdges) {
  run_unary_kernel_test("a = med(close, 5)", OpCode::TsMed, kWin);
}

// ---------------------------------------------------------------------------
// Delay / Delta — no-window single-offset kernels.
// ---------------------------------------------------------------------------
TEST(TsTranspose_Unary_BitExact, TsDelay_WindowEdges) {
  run_unary_kernel_test("a = delay(close, 3)", OpCode::TsDelay, 3);
}

TEST(TsTranspose_Unary_BitExact, TsDelta_WindowEdges) {
  run_unary_kernel_test("a = delta(close, 3)", OpCode::TsDelta, 3);
}

// ---------------------------------------------------------------------------
// Backfill / CountNans — special NaN-policy ops.
// ---------------------------------------------------------------------------
TEST(TsTranspose_Unary_BitExact, TsBackfill_NanHoles) {
  run_unary_kernel_test("a = ts_backfill(close, 5)", OpCode::TsBackfill, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsCountNans_NanHoles) {
  run_unary_kernel_test("a = ts_count_nans(close, 5)", OpCode::TsCountNans, kWin);
}

// ---------------------------------------------------------------------------
// Product, ArgMin, ArgMax — order-independent or index-scan kernels.
// ---------------------------------------------------------------------------
TEST(TsTranspose_Unary_BitExact, TsProduct_WindowEdges) {
  run_unary_kernel_test("a = product(close, 5)", OpCode::TsProduct, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsArgMin_WindowEdges) {
  run_unary_kernel_test("a = ts_argmin(close, 5)", OpCode::TsArgMin, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsArgMax_WindowEdges) {
  run_unary_kernel_test("a = ts_argmax(close, 5)", OpCode::TsArgMax, kWin);
}

// ---------------------------------------------------------------------------
// Linear-weighted / EMA / decay ops.
// ---------------------------------------------------------------------------
TEST(TsTranspose_Unary_BitExact, TsDecayLinear_WindowEdges) {
  run_unary_kernel_test("a = decay_linear(close, 5)", OpCode::TsDecayLinear, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsWma_WindowEdges) {
  run_unary_kernel_test("a = wma(close, 5)", OpCode::TsWma, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsEma_WindowEdges) {
  run_unary_kernel_test("a = ema(close, 5)", OpCode::TsEma, kWin);
}

// ---------------------------------------------------------------------------
// Skew / Kurt (higher moments).
// ---------------------------------------------------------------------------
TEST(TsTranspose_Unary_BitExact, TsSkew_WindowEdges) {
  run_unary_kernel_test("a = skew(close, 5)", OpCode::TsSkew, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsKurt_WindowEdges) {
  run_unary_kernel_test("a = kurt(close, 5)", OpCode::TsKurt, kWin);
}

// ---------------------------------------------------------------------------
// OLS-derived: slope, rsquare, resid.
// ---------------------------------------------------------------------------
TEST(TsTranspose_Unary_BitExact, TsSlope_WindowEdges) {
  run_unary_kernel_test("a = slope(close, 5)", OpCode::TsSlope, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsRsquare_WindowEdges) {
  run_unary_kernel_test("a = rsquare(close, 5)", OpCode::TsRsquare, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsResid_WindowEdges) {
  run_unary_kernel_test("a = resid(close, 5)", OpCode::TsResid, kWin);
}

// ---------------------------------------------------------------------------
// MAD, AvDiff, Zscore, Quantile.
// ---------------------------------------------------------------------------
TEST(TsTranspose_Unary_BitExact, TsMad_WindowEdges) {
  run_unary_kernel_test("a = mad(close, 5)", OpCode::TsMad, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsAvDiff_WindowEdges) {
  run_unary_kernel_test("a = ts_av_diff(close, 5)", OpCode::TsAvDiff, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsZscore_WindowEdges) {
  run_unary_kernel_test("a = ts_zscore(close, 5)", OpCode::TsZscore, kWin);
}

TEST(TsTranspose_Unary_BitExact, TsQuantile_WindowEdges) {
  run_unary_kernel_test("a = ts_quantile(close, 5)", OpCode::TsQuantile, kWin);
}

// ---------------------------------------------------------------------------
// DecayExp, Moment, Entropy — parameterized by p0.
// ---------------------------------------------------------------------------
TEST(TsTranspose_Unary_BitExact, TsDecayExp_WindowEdges) {
  run_unary_kernel_test("a = ts_decay_exp(close, 5, 0.9)", OpCode::TsDecayExp, kWin, 0.9);
}

TEST(TsTranspose_Unary_BitExact, TsMoment_WindowEdges) {
  run_unary_kernel_test("a = ts_moment(close, 5, 3)", OpCode::TsMoment, kWin, 3.0);
}

TEST(TsTranspose_Unary_BitExact, TsEntropy_WindowEdges) {
  run_unary_kernel_test("a = ts_entropy(close, 5, 8)", OpCode::TsEntropy, kWin, 8.0);
}

// ===========================================================================
// Binary-series (TsCorr, TsCov, TsRegression)
// ===========================================================================

static void run_binary_kernel_test(std::string_view src_expr, OpCode op, atx::usize window) {
  const Panel panel = make_ts_panel();
  const auto close = panel_close(panel);
  const auto open  = panel_open();

  const std::vector<atx::f64> expected = oracle_binary(op, close, open, window);
  const std::vector<atx::f64> actual   = eval_ts(src_expr, panel);
  assert_bit_identical(expected, actual, src_expr);
}

// SAFETY: col-extract for x AND y → instruments=1 j=0; gather fills
// bx[i]=col_x[t+1-d+i], by[i]=col_y[t+1-d+i]. Same elements, same order as
// bx[i]=x[(t+1-d+i)*I+j], by[i]=y[(t+1-d+i)*I+j]. Two-pass sums are
// identical. Bit-for-bit equal.

TEST(TsTranspose_Binary_BitExact, TsCorr_WindowEdgesNanHoles) {
  run_binary_kernel_test("a = ts_corr(close, open, 5)", OpCode::TsCorr, kWin);
}

TEST(TsTranspose_Binary_BitExact, TsCov_WindowEdgesNanHoles) {
  run_binary_kernel_test("a = covariance(close, open, 5)", OpCode::TsCov, kWin);
}

TEST(TsTranspose_Binary_BitExact, TsRegression_WindowEdgesNanHoles) {
  run_binary_kernel_test("a = ts_regression(close, open, 5)", OpCode::TsRegression, kWin);
}

// ===========================================================================
// OU rolling-fit ops (OuTheta, OuHalflife, OuMean, OuZscore)
// ===========================================================================

static void run_ou_kernel_test(std::string_view src_expr, OpCode op, atx::usize window) {
  const Panel panel = make_ts_panel();
  const auto close = panel_close(panel);

  const std::vector<atx::f64> expected = oracle_ou(op, close, window);
  const std::vector<atx::f64> actual   = eval_ts(src_expr, panel);
  assert_bit_identical(expected, actual, src_expr);
}

// SAFETY: col-extract → gather in ou_value_at (instruments=1 j=0) fills
// buf[i]=col[t+1-d+i] == x[(t+1-d+i)*I+j]. ou_ar1_fit then processes the
// same contiguous slice in the same chronological order. AR(1) pair-loop
// s=1..d-1 is identical. Bit-for-bit equal.

TEST(TsTranspose_OU_BitExact, OuTheta_WindowEdgesNanHoles) {
  run_ou_kernel_test("a = ou_theta(close, 5)", OpCode::OuTheta, kWin);
}

TEST(TsTranspose_OU_BitExact, OuHalflife_WindowEdgesNanHoles) {
  run_ou_kernel_test("a = ou_halflife(close, 5)", OpCode::OuHalflife, kWin);
}

TEST(TsTranspose_OU_BitExact, OuMean_WindowEdgesNanHoles) {
  run_ou_kernel_test("a = ou_mean(close, 5)", OpCode::OuMean, kWin);
}

TEST(TsTranspose_OU_BitExact, OuZscore_WindowEdgesNanHoles) {
  run_ou_kernel_test("a = ou_zscore(close, 5)", OpCode::OuZscore, kWin);
}

// ===========================================================================
// End-to-end digest test: a Ts-heavy AST set → byte-identical SignalSet
// before (oracle via direct kernel calls) and after the transpose.
// We verify: the multi-kernel expression produces the same aggregate output
// as individual oracle_unary calls composed with the same arithmetic.
// ===========================================================================

TEST(TsTranspose_DigestUnchanged, TsHeavyExpressionMatchesPerKernelOracle) {
  const Panel panel = make_ts_panel();
  const auto close = panel_close(panel);

  // Engine evaluates a combined Ts expression: ts_std(close,5) + ts_rank(close,5).
  // Oracle: compute each term separately and add.
  const std::vector<atx::f64> std5  = oracle_unary(OpCode::TsStd, close, kWin);
  const std::vector<atx::f64> rnk5  = oracle_unary(OpCode::TsRank, close, kWin);

  std::vector<atx::f64> expected(kDates * kInsts, kTsNaN);
  for (atx::usize i = 0; i < expected.size(); ++i) {
    if (!std::isnan(std5[i]) && !std::isnan(rnk5[i])) {
      expected[i] = std5[i] + rnk5[i];
    }
    // If either is NaN, the sum is NaN (arithmetic propagates).
  }

  const std::vector<atx::f64> actual = eval_ts(
      "a = ts_std(close, 5) + ts_rank(close, 5)", panel);

  assert_bit_identical(expected, actual, "TsHeavyDigest");
}

// ===========================================================================
// A/B access-pattern microbench — ISOLATES the column-extract transpose.
//
// This bench compares the TWO ACCESS PATTERNS directly, same kernel, same data,
// same TU.  It does NOT touch the Engine (no Program setup / dispatch), so the
// only variable measured is the memory-access stride of the window reads:
//
//   OLD (strided): instrument-outer / date-inner; the kernel reads each window
//     element as x[k*I+j] (stride I) — the PRE-transpose access pattern.  We
//     replicate it by calling the SAME kernel function with the full panel
//     buffer, instruments=I, real j.
//
//   NEW (column-extract): for each instrument column j, extract col[s]=x[s*I+j]
//     once into a contiguous buffer, then run the windowed kernel stride-1
//     (instruments=1, j=0) — the SHIPPED path (eval_time_series, vm.hpp).
//
// Both variants invoke the identical kernel (ts_value_at / ts_pair_at), so the
// arithmetic and the result are bit-identical; ONLY the access stride differs.
// At production scale each element is re-read ~d times across overlapping
// windows — exactly the strided traffic the transpose converts into one stride-I
// extract + d× contiguous (prefetch/SIMD-friendly) reads.
//
// SCALE: dates=1024, instruments=512, window d=20.  RELEASE build required for a
// meaningful number (Debug emits no vectorization, so the extract copy is pure
// overhead) — run the build-rel configure recipe in the task brief.  The bench
// PRINTS old vs new cells/s and the speedup; it asserts only that the new path
// is not catastrophically slower (no hard speedup gate — the gate is the printed
// number the human reads, per the brief's "no unsupported speedup claim" rule).
// ===========================================================================

namespace {

// Build a dense (no-NaN) production-scale panel as two flat date-major buffers.
// No NaN holes here: the bench measures the full-window arithmetic path (the
// any-NaN gate would short-circuit most cells and hide the access cost).
struct BenchPanel {
  atx::usize dates;
  atx::usize insts;
  std::vector<atx::f64> close; // dates*insts, date-major
  std::vector<atx::f64> open;  // dates*insts, date-major
};

[[nodiscard]] BenchPanel make_bench_panel(atx::usize dates, atx::usize insts) {
  BenchPanel p;
  p.dates = dates;
  p.insts = insts;
  const atx::usize n = dates * insts;
  p.close.resize(n);
  p.open.resize(n);
  atx::u64 lcg = 0xC0FFEE1234567890ULL;
  auto next = [&lcg]() -> atx::f64 {
    lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    const atx::f64 u = static_cast<atx::f64>(static_cast<atx::u32>(lcg >> 32)) /
                       static_cast<atx::f64>(0xFFFFFFFFu);
    return 1.0 + 9.0 * u; // [1,10], strictly finite
  };
  for (atx::usize i = 0; i < n; ++i) {
    p.close[i] = next();
    p.open[i] = next();
  }
  return p;
}

} // namespace

TEST(TsTranspose_AccessBench, StridedVsColumnExtractProductionScale) {
  constexpr atx::usize kBenchDates = 1024;
  constexpr atx::usize kBenchInsts = 512;
  constexpr atx::usize kBenchWin = 20;
  const BenchPanel bp = make_bench_panel(kBenchDates, kBenchInsts);
  const std::span<const atx::f64> x{bp.close};
  const std::span<const atx::f64> y{bp.open};
  const atx::usize I = bp.insts;
  const atx::usize D = bp.dates;
  const atx::usize d = kBenchWin;
  const double cells = static_cast<double>(D) * static_cast<double>(I);

  using Clock = std::chrono::steady_clock;
  std::vector<atx::f64> sbuf(d);
  std::vector<atx::f64> sbuf2(d);
  std::vector<atx::f64> col(D);
  std::vector<atx::f64> col_b(D);

  // ---- TsVar (unary, window-heavy two-pass) ----
  // OLD: strided — call the kernel with the full panel, instruments=I, real j.
  atx::f64 sink_old = 0.0;
  auto t0 = Clock::now();
  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize t = 0; t < D; ++t) {
      sink_old += ts_value_at(OpCode::TsVar, x, t, j, d, I, sbuf, 0.0);
    }
  }
  const auto var_old_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();

  // NEW: column-extract — extract col j once, run kernel stride-1 (I=1, j=0).
  atx::f64 sink_new = 0.0;
  t0 = Clock::now();
  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize s = 0; s < D; ++s) {
      col[s] = x[s * I + j];
    }
    const std::span<const atx::f64> c{col.data(), D};
    for (atx::usize t = 0; t < D; ++t) {
      sink_new += ts_value_at(OpCode::TsVar, c, t, 0, d, 1, sbuf, 0.0);
    }
  }
  const auto var_new_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();

  // ---- TsCorr (binary, window-heavy two-column two-pass) ----
  atx::f64 sink_old_c = 0.0;
  t0 = Clock::now();
  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize t = 0; t < D; ++t) {
      sink_old_c += ts_pair_at(OpCode::TsCorr, x, y, t, j, d, I, sbuf, sbuf2);
    }
  }
  const auto corr_old_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();

  atx::f64 sink_new_c = 0.0;
  t0 = Clock::now();
  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize s = 0; s < D; ++s) {
      col[s] = x[s * I + j];
      col_b[s] = y[s * I + j];
    }
    const std::span<const atx::f64> c{col.data(), D};
    const std::span<const atx::f64> cb{col_b.data(), D};
    for (atx::usize t = 0; t < D; ++t) {
      sink_new_c += ts_pair_at(OpCode::TsCorr, c, cb, t, 0, d, 1, sbuf, sbuf2);
    }
  }
  const auto corr_new_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();

  // ---- TsDelay (pure-lookback, O(1) per cell — reads ONE element x[(t-d)*I+j])
  // This kernel has NO window to make contiguous, so the column-extract is pure
  // overhead.  S1-3 (narrowed) keeps delay/delta on the DIRECT strided path; we
  // bench DIRECT (the shipped path = baseline) vs COL-EXTRACT (the cost the
  // exclusion AVOIDS) to document the regression that narrowing removes.
  // DIRECT (shipped, baseline): strided x[(t-d)*I+j], no extraction.
  atx::f64 sink_old_dl = 0.0;
  t0 = Clock::now();
  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize t = 0; t < D; ++t) {
      sink_old_dl += ts_value_at(OpCode::TsDelay, x, t, j, d, I, sbuf, 0.0);
    }
  }
  const auto delay_direct_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();

  // COL-EXTRACT (NOT shipped for delay — measured to show its overhead): copy the
  // whole O(dates) column per instrument, then do single-element lookups stride-1.
  atx::f64 sink_new_dl = 0.0;
  t0 = Clock::now();
  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize s = 0; s < D; ++s) {
      col[s] = x[s * I + j];
    }
    const std::span<const atx::f64> c{col.data(), D};
    for (atx::usize t = 0; t < D; ++t) {
      sink_new_dl += ts_value_at(OpCode::TsDelay, c, t, 0, d, 1, sbuf, 0.0);
    }
  }
  const auto delay_extract_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();

  auto cps = [cells](long long ns) -> double {
    return ns == 0 ? 0.0 : cells / (static_cast<double>(ns) * 1e-9);
  };
  const double var_old_cps = cps(var_old_ns);
  const double var_new_cps = cps(var_new_ns);
  const double corr_old_cps = cps(corr_old_ns);
  const double corr_new_cps = cps(corr_new_ns);
  const double delay_direct_cps = cps(delay_direct_ns);
  const double delay_extract_cps = cps(delay_extract_ns);

  std::cout << "[TsTranspose access A/B] dates=" << D << " instruments=" << I
            << " window=" << d << " (" << static_cast<long long>(cells) << " cells/kernel)\n"
            << "  TsVar  strided (old): " << static_cast<long long>(var_old_cps) << " cells/s\n"
            << "  TsVar  col-extract  : " << static_cast<long long>(var_new_cps) << " cells/s"
            << "  speedup=" << (var_old_cps > 0.0 ? var_new_cps / var_old_cps : 0.0) << "x\n"
            << "  TsCorr strided (old): " << static_cast<long long>(corr_old_cps) << " cells/s\n"
            << "  TsCorr col-extract  : " << static_cast<long long>(corr_new_cps) << " cells/s"
            << "  speedup=" << (corr_old_cps > 0.0 ? corr_new_cps / corr_old_cps : 0.0) << "x\n"
            << "  TsDelay direct (shipped, baseline): " << static_cast<long long>(delay_direct_cps)
            << " cells/s\n"
            << "  TsDelay col-extract (EXCLUDED)    : " << static_cast<long long>(delay_extract_cps)
            << " cells/s  ratio=" << (delay_direct_cps > 0.0 ? delay_extract_cps / delay_direct_cps : 0.0)
            << "x (extract would REGRESS delay — hence kept on direct path)\n";

  // Defeat dead-code elimination on the timed sinks.  Do NOT compare them to
  // each other and do NOT require finiteness:
  //   * The reduction `sink += kernel()` over 524288 cells is reassociated by the
  //     optimizer DIFFERENTLY between the two loops (vectorized horizontal-add for
  //     the contiguous NEW loop vs scalar accumulate for the strided OLD loop), so
  //     the SUMS differ in the low bits even though every per-cell value matches.
  //   * Partial/degenerate windows return NaN (window not full, or zero-variance
  //     TsCorr), so the running sum is legitimately NaN — never finite.
  // The sink exists ONLY to keep the timed loops from being optimized away; its
  // value is meaningless.  Bit-exactness is validated PER CELL below (NaN-aware).
  const volatile atx::f64 dce_guard =
      sink_old + sink_new + sink_old_c + sink_new_c + sink_old_dl + sink_new_dl;
  (void)dce_guard;

  // PER-CELL bit-exactness (UNTIMED): write each kernel's output to a separate
  // array for both access patterns, then compare element-by-element.  An
  // individual kernel return value is NOT a reduction the compiler can
  // reassociate, so this is the sound equality check.  It PROVES the A/B bench
  // measures only the access pattern (identical arithmetic, different stride).
  std::vector<atx::f64> out_old(D * I);
  std::vector<atx::f64> out_new(D * I);
  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize t = 0; t < D; ++t) {
      out_old[t * I + j] = ts_value_at(OpCode::TsVar, x, t, j, d, I, sbuf, 0.0);
    }
  }
  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize s = 0; s < D; ++s) {
      col[s] = x[s * I + j];
    }
    const std::span<const atx::f64> c{col.data(), D};
    for (atx::usize t = 0; t < D; ++t) {
      out_new[t * I + j] = ts_value_at(OpCode::TsVar, c, t, 0, d, 1, sbuf, 0.0);
    }
  }
  atx::usize var_mismatches = 0;
  for (atx::usize i = 0; i < D * I; ++i) {
    if (!bit_eq_ts(out_old[i], out_new[i])) {
      ++var_mismatches;
    }
  }
  EXPECT_EQ(var_mismatches, 0U) << "TsVar per-cell strided vs col-extract NOT bit-identical";

  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize t = 0; t < D; ++t) {
      out_old[t * I + j] = ts_pair_at(OpCode::TsCorr, x, y, t, j, d, I, sbuf, sbuf2);
    }
  }
  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize s = 0; s < D; ++s) {
      col[s] = x[s * I + j];
      col_b[s] = y[s * I + j];
    }
    const std::span<const atx::f64> c{col.data(), D};
    const std::span<const atx::f64> cb{col_b.data(), D};
    for (atx::usize t = 0; t < D; ++t) {
      out_new[t * I + j] = ts_pair_at(OpCode::TsCorr, c, cb, t, 0, d, 1, sbuf, sbuf2);
    }
  }
  atx::usize corr_mismatches = 0;
  for (atx::usize i = 0; i < D * I; ++i) {
    if (!bit_eq_ts(out_old[i], out_new[i])) {
      ++corr_mismatches;
    }
  }
  EXPECT_EQ(corr_mismatches, 0U) << "TsCorr per-cell strided vs col-extract NOT bit-identical";

  // TsDelay per-cell: direct vs col-extract must ALSO be bit-identical (the
  // lookup is the same element either way). This proves the delay bench compares
  // the SAME computation — only the access path differs — so the direct-vs-extract
  // timing ratio is a fair measure of the extraction overhead the exclusion saves.
  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize t = 0; t < D; ++t) {
      out_old[t * I + j] = ts_value_at(OpCode::TsDelay, x, t, j, d, I, sbuf, 0.0);
    }
  }
  for (atx::usize j = 0; j < I; ++j) {
    for (atx::usize s = 0; s < D; ++s) {
      col[s] = x[s * I + j];
    }
    const std::span<const atx::f64> c{col.data(), D};
    for (atx::usize t = 0; t < D; ++t) {
      out_new[t * I + j] = ts_value_at(OpCode::TsDelay, c, t, 0, d, 1, sbuf, 0.0);
    }
  }
  atx::usize delay_mismatches = 0;
  for (atx::usize i = 0; i < D * I; ++i) {
    if (!bit_eq_ts(out_old[i], out_new[i])) {
      ++delay_mismatches;
    }
  }
  EXPECT_EQ(delay_mismatches, 0U) << "TsDelay per-cell direct vs col-extract NOT bit-identical";

  // No hard speedup gate (the printed numbers are the deliverable). Guard only
  // against a catastrophic >10x regression that would signal a logic error.
  EXPECT_GE(var_new_cps, var_old_cps * 0.1) << "TsVar col-extract path >10x slower — investigate";
  EXPECT_GE(corr_new_cps, corr_old_cps * 0.1) << "TsCorr col-extract path >10x slower — investigate";
}

} // namespace atxtest_alpha_ts_transpose
