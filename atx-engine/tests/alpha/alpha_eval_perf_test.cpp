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
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

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
