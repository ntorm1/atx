// atx::engine::parallel — S7.5c parallel_evaluate over the PROCESS boundary proof.
//
// THE CAPSTONE (R1/§0.5): parallel_evaluate runs over a ProcessExecutor with a
// gathered-SignalSet digest BYTE-IDENTICAL to the ThreadExecutor path AND the
// in-process sequential oracle (S7.5a already pinned in-process == single-thread
// batch), invariant across process worker counts {1,N}. The map crosses the process
// boundary via the registered WorkloadId::Eval + serialized (Programs + Panel)
// InputView seam — NOT a C++ closure. This suite proves:
//
//   1. DIGEST IDENTITY — digest(ProcessExecutor@{1,N}) == digest(ThreadExecutor@8)
//      == the in-process oracle (ThreadExecutor@1), byte for byte via
//      signal_set_digest. ALL THREE legs asserted.
//   2. MULTI-ROOT — a multi-root program (compile_batch) in the batch exercises
//      max_nroots>1 slot sizing + multi-column gather; the digest still matches.
//   3. EVAL ROUND-TRIP — serialize_eval_input then EvalInputView::parse reconstructs
//      a Program field-for-field AND a Panel that evaluates to the same digest.
//   4. PARSE REJECTION — truncated / bad-magic / overflowing-dimension / malformed
//      offset-table buffers are each rejected with Err (no crash, no OOB).
//
// The worker exe (atx-shm-worker) registers eval_shard in its own main; the test
// process only constructs ProcessExecutor (which spawns that exe).

#include <cstddef>
#include <cstdint>
#include <cstring> // std::memcpy (crafted-header overflow test)
#include <limits>
#include <span>
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

#include "atx/engine/parallel/batch_eval.hpp"
#include "atx/engine/parallel/digest.hpp"
#include "atx/engine/parallel/executor.hpp"
#include "atx/engine/parallel/process_executor.hpp"
#include "atx/engine/parallel/thread_executor.hpp"
#include "atx/engine/parallel/workload_eval.hpp"

namespace {

using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::compile_batch;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_program;
using atx::engine::alpha::Program;
using atx::engine::alpha::SignalSet;
using atx::engine::parallel::EvalInputView;
using atx::engine::parallel::ExecutorConfig;
using atx::engine::parallel::InputView;
using atx::engine::parallel::kEvalHeaderBytes;
using atx::engine::parallel::kEvalMagic;
using atx::engine::parallel::parallel_evaluate;
using atx::engine::parallel::ProcessExecutor;
using atx::engine::parallel::serialize_eval_input;
using atx::engine::parallel::signal_set_digest;
using atx::engine::parallel::ThreadExecutor;

// Process-lifetime Library: the Ast borrows OpSig pointers from it, so it must
// outlive every parse result.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// ---- synthetic panel (a trimmed mirror of parallel_batch_eval_test::make_panel) --
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
    cols[0][i] = base;                         // open
    cols[1][i] = hi;                           // high
    cols[2][i] = lo;                           // low
    cols[3][i] = lo + (hi - lo) * 0.5;         // close
    cols[4][i] = 1.0e4 + next() * 9.9e5;       // volume
    cols[5][i] = (hi + lo + cols[3][i]) / 3.0; // vwap
    cols[6][i] = next() * 0.1 - 0.05;          // returns
  }

  std::vector<std::uint8_t> universe(cells, std::uint8_t{1});
  // Delist a couple of instruments mid-sample (no survivorship; reads as NaN) so the
  // universe mask is NON-trivial and must round-trip across the boundary exactly.
  if (instruments >= 3 && dates >= 4) {
    const atx::usize delist_date = dates * 2 / 3;
    for (atx::usize d = delist_date; d < dates; ++d) {
      universe[d * instruments + 1] = 0;
      universe[d * instruments + (instruments - 1)] = 0;
    }
  }
  // Scatter a few NaN source cells (data gaps) so NaN==NaN paths cross the boundary.
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

// A single-root program named `a{k} = <src>` (matches compile_batch's auto-naming).
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

[[nodiscard]] Program compile_batch_ok(const std::vector<std::string_view> &srcs) {
  auto prog = compile_batch(std::span<const std::string_view>{srcs}, shared_lib());
  EXPECT_TRUE(prog.has_value()) << (prog ? "" : prog.error().message());
  return prog.value_or(Program{});
}

// A battery of distinct element-wise expressions (only ops the VM implements
// element-wise; no Cs*/Ts*). Several SHARE subexpressions so the per-program fan
// recompute is exercised, yet the digest must still agree across substrates.
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
      "high * 2 - low",
      "(open + high + low + close) / 4 - vwap",
  };
}

// The per-program FAN: program k is `a{k} = battery[k]` (single-root each).
[[nodiscard]] std::vector<Program> compile_fan(const std::vector<std::string_view> &srcs) {
  std::vector<Program> progs;
  progs.reserve(srcs.size());
  for (std::size_t k = 0; k < srcs.size(); ++k) {
    progs.push_back(compile_named(k, srcs[k]));
  }
  return progs;
}

} // namespace

// ===========================================================================
//  1. EVAL — process@{1,N} == thread@8 == in-process oracle (byte-identical).
// ===========================================================================
TEST(ParallelWorkloadEvalProcess, EvalDigestProcessEqualsThreadEqualsSequential) {
  const Panel panel = make_panel(24, 6, 0xABCDEF01ULL);
  const std::vector<Program> progs = compile_fan(battery());

  // ThreadExecutor@1 is the in-process oracle (S7.5a pinned it to single-thread batch).
  ThreadExecutor te1{ExecutorConfig{1}};
  const auto oracle = parallel_evaluate(progs, panel, te1);
  ASSERT_TRUE(oracle.has_value()) << (oracle ? "" : oracle.error().message());
  const atx::u64 want = signal_set_digest(oracle.value());

  // ThreadExecutor@8 (in-process, more workers) — must match.
  {
    ThreadExecutor te8{ExecutorConfig{8}};
    const auto got = parallel_evaluate(progs, panel, te8);
    ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().message());
    EXPECT_EQ(signal_set_digest(got.value()), want)
        << "ThreadExecutor@8 eval digest diverged from the in-process oracle";
  }

  // ProcessExecutor @ 1 and @ 4 — the cross-process legs (the whole point).
  for (std::size_t w : {std::size_t{1}, std::size_t{4}}) {
    ProcessExecutor pe{ExecutorConfig{w, false}};
    const auto got = parallel_evaluate(progs, panel, pe);
    ASSERT_TRUE(got.has_value()) << "ProcessExecutor@" << w << ": "
                                 << (got ? "" : got.error().message());
    EXPECT_EQ(signal_set_digest(got.value()), want)
        << "ProcessExecutor@" << w << " eval digest diverged from the in-process oracle";
  }
}

TEST(ParallelWorkloadEvalProcess, EvalProcessWorkerCountInvariant) {
  const Panel panel = make_panel(24, 6, 0xABCDEF01ULL);
  const std::vector<Program> progs = compile_fan(battery());
  ProcessExecutor pe1{ExecutorConfig{1, false}};
  ProcessExecutor peN{ExecutorConfig{4, false}};
  const auto d1 = parallel_evaluate(progs, panel, pe1);
  const auto dN = parallel_evaluate(progs, panel, peN);
  ASSERT_TRUE(d1.has_value());
  ASSERT_TRUE(dN.has_value());
  EXPECT_EQ(signal_set_digest(d1.value()), signal_set_digest(dN.value()))
      << "ProcessExecutor eval digest must be invariant 1 vs N workers";
}

// ===========================================================================
//  2. MULTI-ROOT — a multi-root program (compile_batch) exercises max_nroots>1
//     slot sizing + multi-column gather; the digest still matches the oracle.
// ===========================================================================
TEST(ParallelWorkloadEvalProcess, EvalMultiRootProgramMatches) {
  const Panel panel = make_panel(20, 5, 0xBEEFULL);

  // A mix: two single-root programs, then ONE multi-root program (4 roots via
  // compile_batch). max_nroots == 4 -> the slot is sized for 4 columns, and the
  // multi-column gather must reassemble all four in root order.
  std::vector<Program> progs;
  progs.push_back(compile_named(0, "close - open"));
  progs.push_back(compile_named(1, "high - low"));
  progs.push_back(compile_batch_ok(
      {"vwap - close", "volume * close", "returns * volume", "(high + low) / 2 - vwap"}));
  ASSERT_GE(progs[2].roots.size(), std::size_t{2})
      << "the batch program must be multi-root to exercise max_nroots>1";

  ThreadExecutor te1{ExecutorConfig{1}};
  const auto oracle = parallel_evaluate(progs, panel, te1);
  ASSERT_TRUE(oracle.has_value()) << (oracle ? "" : oracle.error().message());
  const atx::u64 want = signal_set_digest(oracle.value());

  ProcessExecutor pe{ExecutorConfig{4, false}};
  const auto got = parallel_evaluate(progs, panel, pe);
  ASSERT_TRUE(got.has_value()) << (got ? "" : got.error().message());
  EXPECT_EQ(signal_set_digest(got.value()), want)
      << "ProcessExecutor multi-root eval digest must match the in-process oracle";
}

// ===========================================================================
//  3a. ROUND-TRIP — Program reconstructs field-for-field through serialize/parse.
// ===========================================================================
TEST(ParallelWorkloadEvalProcess, EvalSerializeRoundTripsProgram) {
  const Panel panel = make_panel(12, 4, 0x77ULL);
  std::vector<Program> progs;
  progs.push_back(compile_named(0, "close - open"));
  progs.push_back(compile_batch_ok({"vwap - close", "high - low"}));

  const std::vector<std::byte> buf = serialize_eval_input(progs, panel);
  const auto parsed = EvalInputView::parse(InputView{std::span<const std::byte>{buf}});
  ASSERT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  const EvalInputView &v = parsed.value();

  ASSERT_EQ(v.n_programs(), progs.size());
  ASSERT_EQ(v.dates(), panel.dates());
  ASSERT_EQ(v.instruments(), panel.instruments());
  ASSERT_EQ(v.n_fields(), panel.num_fields());
  ASSERT_EQ(v.cells(), panel.cells());

  for (std::size_t k = 0; k < progs.size(); ++k) {
    const auto rp = v.program(k);
    ASSERT_TRUE(rp.has_value()) << "program " << k << ": " << (rp ? "" : rp.error().message());
    const Program &got = rp.value();
    const Program &want = progs[k];
    EXPECT_EQ(got.num_slots, want.num_slots) << "k=" << k;
    EXPECT_EQ(got.required_lookback, want.required_lookback) << "k=" << k;
    EXPECT_EQ(got.unique_nodes, want.unique_nodes) << "k=" << k;
    EXPECT_EQ(got.total_ast_nodes, want.total_ast_nodes) << "k=" << k;
    EXPECT_EQ(got.peak_live_slots, want.peak_live_slots) << "k=" << k;
    EXPECT_EQ(got.cache_hits, want.cache_hits) << "k=" << k;
    EXPECT_EQ(got.intern_attempts, want.intern_attempts) << "k=" << k;

    // Code bytes field-for-field (Instr is POD).
    ASSERT_EQ(got.code.size(), want.code.size()) << "k=" << k;
    EXPECT_EQ(std::memcmp(got.code.data(), want.code.data(),
                          want.code.size() * sizeof(atx::engine::alpha::Instr)),
              0)
        << "code stream mismatch k=" << k;

    // Roots: name + output index.
    ASSERT_EQ(got.roots.size(), want.roots.size()) << "k=" << k;
    for (std::size_t r = 0; r < want.roots.size(); ++r) {
      EXPECT_EQ(got.roots[r].name, want.roots[r].name) << "k=" << k << " r=" << r;
      EXPECT_EQ(got.roots[r].output, want.roots[r].output) << "k=" << k << " r=" << r;
    }

    // Program field dictionary.
    ASSERT_EQ(got.fields.size(), want.fields.size()) << "k=" << k;
    for (std::size_t f = 0; f < want.fields.size(); ++f) {
      EXPECT_EQ(got.fields[f], want.fields[f]) << "k=" << k << " f=" << f;
    }
  }
}

// ===========================================================================
//  3b. ROUND-TRIP — the reconstructed Panel evaluates to the same digest.
// ===========================================================================
TEST(ParallelWorkloadEvalProcess, EvalSerializeRoundTripsPanel) {
  const Panel panel = make_panel(16, 5, 0x99ULL);
  const Program prog = compile_batch_ok(battery());

  // Direct: evaluate the program over the SOURCE panel.
  Engine eng_src{panel};
  const auto direct = eng_src.evaluate(prog);
  ASSERT_TRUE(direct.has_value()) << (direct ? "" : direct.error().message());
  const atx::u64 want = signal_set_digest(direct.value());

  // Round-trip: serialize, parse, rebuild the borrowed Panel, evaluate over it.
  const std::vector<std::byte> buf =
      serialize_eval_input(std::span<const Program>{&prog, 1}, panel);
  const auto parsed = EvalInputView::parse(InputView{std::span<const std::byte>{buf}});
  ASSERT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  const auto rebuilt_panel = parsed.value().panel();
  ASSERT_TRUE(rebuilt_panel.has_value()) << (rebuilt_panel ? "" : rebuilt_panel.error().message());
  const auto rebuilt_prog = parsed.value().program(0);
  ASSERT_TRUE(rebuilt_prog.has_value()) << (rebuilt_prog ? "" : rebuilt_prog.error().message());

  Engine eng_rt{rebuilt_panel.value()};
  const auto rt = eng_rt.evaluate(rebuilt_prog.value());
  ASSERT_TRUE(rt.has_value()) << (rt ? "" : rt.error().message());
  EXPECT_EQ(signal_set_digest(rt.value()), want)
      << "round-tripped Panel + Program must evaluate to the same digest";
}

// ===========================================================================
//  4. PARSE REJECTION — malformed untrusted buffers each rejected with Err.
// ===========================================================================
TEST(ParallelWorkloadEvalProcess, EvalParseRejectsTruncatedAndBadMagic) {
  const Panel panel = make_panel(8, 4, 0x55ULL);
  std::vector<Program> progs;
  progs.push_back(compile_named(0, "close - open"));
  progs.push_back(compile_named(1, "high - low"));
  const std::vector<std::byte> good = serialize_eval_input(progs, panel);

  // The full buffer parses.
  EXPECT_TRUE(EvalInputView::parse(InputView{std::span<const std::byte>{good}}).has_value());

  // Truncated at various cut points: a region now overruns the buffer -> Err.
  for (std::size_t cut : {std::size_t{0}, std::size_t{4}, std::size_t{kEvalHeaderBytes},
                          good.size() / 2, good.size() - sizeof(atx::u64)}) {
    const std::span<const std::byte> trunc{good.data(), cut};
    EXPECT_FALSE(EvalInputView::parse(InputView{trunc}).has_value())
        << "truncated-to-" << cut << " buffer must be rejected";
  }

  // Corrupt magic -> Err.
  std::vector<std::byte> bad = good;
  bad[0] = std::byte{0xFF};
  EXPECT_FALSE(EvalInputView::parse(InputView{std::span<const std::byte>{bad}}).has_value());

  // Empty buffer -> Err (no magic).
  EXPECT_FALSE(EvalInputView::parse(InputView{std::span<const std::byte>{}}).has_value());
}

// A crafted header with huge u32 dimensions over a tiny buffer must be rejected by
// the overflow-checked dimension products (n_fields*cells / dates*instruments would
// wrap usize) — no OOB alias.
TEST(ParallelWorkloadEvalProcess, EvalParseRejectsOverflowingDimensions) {
  std::vector<std::byte> buf(kEvalHeaderBytes, std::byte{0});
  const atx::u32 magic = kEvalMagic;
  const atx::u32 huge = 0xFFFFFFFFU; // dates = instruments = u32_max => product overflows
  std::memcpy(buf.data() + 0, &magic, 4);
  // n_programs = 0 at [4,8) (zero is fine); dates/instruments huge.
  std::memcpy(buf.data() + 8, &huge, 4);  // dates
  std::memcpy(buf.data() + 12, &huge, 4); // instruments
  // n_fields / universe_present / cells left zero. dates*instruments overflows -> Err.
  EXPECT_FALSE(EvalInputView::parse(InputView{std::span<const std::byte>{buf}}).has_value());
}

// A malformed offset table must be rejected (no OOB program slice is ever formed).
// Bumping n_programs in the header makes the (n_programs+1) u64 table no longer fit
// the buffer, so the table-bounds check rejects it -> Err. (A direct non-monotonic
// corruption would require re-deriving the table offset the parser computes; the
// header-bump is a deterministic, robust exercise of the same rejection path.)
TEST(ParallelWorkloadEvalProcess, EvalParseRejectsMalformedOffsetTable) {
  const Panel panel = make_panel(6, 3, 0x1234ULL);
  std::vector<Program> progs;
  progs.push_back(compile_named(0, "close - open"));
  progs.push_back(compile_named(1, "high - low"));
  std::vector<std::byte> buf = serialize_eval_input(progs, panel);

  // The unmodified fixture is valid.
  ASSERT_TRUE(EvalInputView::parse(InputView{std::span<const std::byte>{buf}}).has_value());

  // n_programs = 1,048,576: the (n_programs+1) u64 offset table cannot fit -> Err.
  const atx::u32 bogus_nprog = 0x00100000U;
  std::memcpy(buf.data() + 4, &bogus_nprog, 4);
  EXPECT_FALSE(EvalInputView::parse(InputView{std::span<const std::byte>{buf}}).has_value())
      << "an n_programs that overruns the offset table must be rejected";
}
