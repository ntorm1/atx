// atx::engine::parallel — S7.5a workload port proof.
//
// S7.5a gives the three as-built parallel workloads (parallel_evaluate /
// parallel_cpcv / parallel_backtests) an `IExecutor&` overload BESIDE their
// existing `DetPool&` one, with the map BODIES UNCHANGED, so they run over the
// substrate-agnostic seam (this unit's THREAD substrate = ThreadExecutor) with
// ZERO behavior change. This suite proves, for EACH of the three workloads:
//
//   1. DIGEST IDENTITY — digest(IExecutor& over ThreadExecutor@{1,2,4,8}) ==
//      digest(DetPool&) == digest(the sequential / single-thread oracle), byte
//      for byte. The oracle is the already-proven one (the DetPool& path the
//      S2 suites pin to the sequential reference), so this transitively proves
//      IExecutor& == sequential.
//   2. WORKER-COUNT INVARIANCE — the IExecutor& overload over ThreadExecutor@1
//      equals @8 (scheduling order touches no result bit).
//   3. DETERMINISTIC LOWEST-INDEX ERROR — a body failure (an eval error on one
//      program) surfaces as the LOWEST-index error through the IExecutor& path,
//      same as DetPool& (eval only — the cpcv/backtests bodies cannot fail).
//
// The panel / streams generators and the alpha battery mirror
// parallel_batch_eval_test.cpp and parallel_run_test.cpp so the oracle is the
// already-proven fixture, not a new one.

#include <cstddef>
#include <cstdint>
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
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"

#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/eval/cpcv.hpp"

#include "atx/engine/parallel/batch_eval.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/digest.hpp"
#include "atx/engine/parallel/executor.hpp"
#include "atx/engine/parallel/parallel_run.hpp"
#include "atx/engine/parallel/thread_executor.hpp"

namespace {

namespace eval = atx::engine::eval;
namespace combine = atx::engine::combine;

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
using atx::engine::parallel::ExecutorConfig;
using atx::engine::parallel::FoldResult;
using atx::engine::parallel::parallel_backtests;
using atx::engine::parallel::parallel_cpcv;
using atx::engine::parallel::parallel_evaluate;
using atx::engine::parallel::result_table_digest;
using atx::engine::parallel::run_one_fold;
using atx::engine::parallel::signal_set_digest;
using atx::engine::parallel::ThreadExecutor;

// The worker counts the determinism contract pins: 1 (sequential-equivalent),
// then 2/4/8. ThreadExecutor takes an explicit count via ExecutorConfig.workers.
constexpr std::size_t kWorkerCounts[] = {1, 2, 4, 8};

// ===========================================================================
//  Shared fixtures — mirrors of the already-proven S2 test generators.
// ===========================================================================

// Process-lifetime Library: the Ast borrows OpSig pointers from it.
[[nodiscard]] const Library &shared_lib() {
  static const Library lib;
  return lib;
}

// Synthetic panel (mirror of parallel_batch_eval_test.cpp's generator): delisted
// instruments (no survivorship), scattered NaN source cells.
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
  if (instruments >= 3 && dates >= 4) {
    const atx::usize delist_date = dates * 2 / 3;
    for (atx::usize d = delist_date; d < dates; ++d) {
      universe[d * instruments + 1] = 0;
      universe[d * instruments + (instruments - 1)] = 0;
    }
  }
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

// >=32 distinct bare element-wise expressions, several deliberately sharing
// subexpressions (mirror of parallel_batch_eval_test.cpp's battery()).
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

// Synthetic AlphaStreams (mirror of parallel_run_test.cpp's make_streams).
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
  for (auto &v : s.pnl_flat) {
    v = nx() * 0.04 - 0.02;
  }
  for (auto &v : s.pos_flat) {
    v = nx() * 2.0 - 1.0;
  }
  return s;
}

// ===========================================================================
//  WORKLOAD 1 — parallel_evaluate.
// ===========================================================================

// (1) digest(IExecutor& @{1,2,4,8}) == digest(DetPool&) == sequential batch oracle.
TEST(ParallelWorkloadPort, EvalDigestMatchesDetPoolAndSequential) {
  const auto srcs = battery();
  const Panel panel = make_panel(40, 16, 0xABCDEF01ULL);

  // Sequential oracle: the single-thread Engine::evaluate of the equivalent batch
  // program (the already-proven batch==singly reference the S2 suite pins to).
  const Program batch = compile_batch_ok(srcs);
  Engine eng{panel};
  const atx::u64 seq_digest = signal_set_digest(eng.evaluate(batch).value());

  // DetPool& path (the proven parallel oracle).
  std::vector<Program> progs = compile_fan(srcs);
  DetPool pool{4};
  const atx::u64 detpool_digest = signal_set_digest(parallel_evaluate(progs, panel, pool).value());
  EXPECT_EQ(detpool_digest, seq_digest) << "DetPool& oracle must match the sequential batch";

  // IExecutor& path over ThreadExecutor at each worker count — byte-identical.
  for (std::size_t w : kWorkerCounts) {
    ThreadExecutor exec{ExecutorConfig{w}};
    const atx::u64 exec_digest =
        signal_set_digest(parallel_evaluate(progs, panel, exec).value());
    EXPECT_EQ(exec_digest, seq_digest)
        << "IExecutor& eval (workers=" << w << ") must be byte-identical to sequential";
    EXPECT_EQ(exec_digest, detpool_digest)
        << "IExecutor& eval (workers=" << w << ") must be byte-identical to DetPool&";
  }
}

// (2) IExecutor& over ThreadExecutor@1 == @8 (worker-count invariance).
TEST(ParallelWorkloadPort, EvalWorkerCountInvariant) {
  const auto srcs = battery();
  const Panel panel = make_panel(40, 16, 0xABCDEF01ULL);
  std::vector<Program> progs = compile_fan(srcs);

  ThreadExecutor exec1{ExecutorConfig{1}};
  ThreadExecutor exec8{ExecutorConfig{8}};
  const atx::u64 d1 = signal_set_digest(parallel_evaluate(progs, panel, exec1).value());
  const atx::u64 d8 = signal_set_digest(parallel_evaluate(progs, panel, exec8).value());
  EXPECT_EQ(d1, d8) << "IExecutor& eval digest must be invariant from 1 to 8 workers";
}

// (3) A body error surfaces as the deterministic LOWEST-index error through the
// IExecutor& path, same as DetPool&. Programs 1 and 3 reference vwap on a panel
// without it -> Err(NotFound) at evaluate; the result is Err at every worker count.
TEST(ParallelWorkloadPort, EvalErrorPropagatesLowestIndex) {
  const Panel panel = make_panel_no_vwap(12, 5);
  std::vector<Program> progs;
  progs.push_back(compile_named(0, "close - open")); // ok
  progs.push_back(compile_named(1, "vwap - close")); // Err(NotFound) at eval (no vwap)
  progs.push_back(compile_named(2, "high - low"));   // ok
  progs.push_back(compile_named(3, "vwap / close")); // Err(NotFound) at eval (no vwap)

  // DetPool& reference: the lowest-index (program 1) error surfaces.
  DetPool pool{4};
  const auto ref = parallel_evaluate(progs, panel, pool);
  ASSERT_FALSE(ref.has_value()) << "DetPool& must surface the eval failure";
  const atx::core::ErrorCode ref_code = ref.error().code();

  // IExecutor& over ThreadExecutor at each worker count: SAME deterministic error.
  for (std::size_t w : kWorkerCounts) {
    ThreadExecutor exec{ExecutorConfig{w}};
    const auto got = parallel_evaluate(progs, panel, exec);
    ASSERT_FALSE(got.has_value()) << "IExecutor& eval (workers=" << w << ") must surface Err";
    EXPECT_EQ(got.error().code(), ref_code)
        << "IExecutor& eval (workers=" << w << ") error must match the DetPool& lowest-index error";
  }
}

// ===========================================================================
//  WORKLOAD 2 — parallel_cpcv.
// ===========================================================================

[[nodiscard]] std::vector<eval::CpcvFold> make_cpcv_folds(std::size_t n_periods) {
  std::vector<eval::LabelSpan> spans;
  spans.reserve(n_periods);
  for (std::size_t i = 0; i < n_periods; ++i) {
    spans.push_back({i, i + 1});
  }
  return eval::cpcv_folds(spans, eval::CpcvConfig{6, 2, 0.0});
}

// (1) digest(IExecutor& @{1,2,4,8}) == digest(DetPool&) == sequential per-fold oracle.
TEST(ParallelWorkloadPort, CpcvDigestMatchesDetPoolAndSequential) {
  const auto streams = make_streams(1, 64, 5, 0xC0FFEEULL);
  const auto folds = make_cpcv_folds(64);

  // Sequential oracle: the SAME per-fold primitive in a plain loop.
  std::vector<FoldResult> seq(folds.size());
  for (std::size_t f = 0; f < folds.size(); ++f) {
    seq[f] = run_one_fold(streams, 0, f, folds[f], 1.0e6);
  }
  const atx::u64 seq_digest = result_table_digest(seq);

  DetPool pool{4};
  const atx::u64 detpool_digest =
      result_table_digest(parallel_cpcv(folds, streams, 0, 1.0e6, pool));
  EXPECT_EQ(detpool_digest, seq_digest) << "DetPool& cpcv oracle must match the sequential loop";

  for (std::size_t w : kWorkerCounts) {
    ThreadExecutor exec{ExecutorConfig{w}};
    const atx::u64 exec_digest =
        result_table_digest(parallel_cpcv(folds, streams, 0, 1.0e6, exec));
    EXPECT_EQ(exec_digest, seq_digest)
        << "IExecutor& cpcv (workers=" << w << ") must be byte-identical to sequential";
    EXPECT_EQ(exec_digest, detpool_digest)
        << "IExecutor& cpcv (workers=" << w << ") must be byte-identical to DetPool&";
  }
}

// (2) IExecutor& over ThreadExecutor@1 == @8 (worker-count invariance).
TEST(ParallelWorkloadPort, CpcvWorkerCountInvariant) {
  const auto streams = make_streams(1, 64, 5, 0xC0FFEEULL);
  const auto folds = make_cpcv_folds(64);

  ThreadExecutor exec1{ExecutorConfig{1}};
  ThreadExecutor exec8{ExecutorConfig{8}};
  const atx::u64 d1 = result_table_digest(parallel_cpcv(folds, streams, 0, 1.0e6, exec1));
  const atx::u64 d8 = result_table_digest(parallel_cpcv(folds, streams, 0, 1.0e6, exec8));
  EXPECT_EQ(d1, d8) << "IExecutor& cpcv digest must be invariant from 1 to 8 workers";
}

// ===========================================================================
//  WORKLOAD 3 — parallel_backtests.
// ===========================================================================

// (1) digest(IExecutor& @{1,2,4,8}) == digest(DetPool&) == sequential per-alpha oracle.
TEST(ParallelWorkloadPort, BacktestsDigestMatchesDetPoolAndSequential) {
  const auto streams = make_streams(20, 48, 4, 0x1234ULL);
  const std::size_t np = streams.n_periods();
  const std::size_t ni = streams.n_instruments();

  // Sequential oracle: compute_metrics over each alpha's whole pnl/pos (the same
  // full-sample metric run_full_backtest uses).
  std::vector<FoldResult> seq(streams.n_alphas());
  for (std::size_t a = 0; a < streams.n_alphas(); ++a) {
    const std::span<const atx::f64> pnl = streams.pnl(a);
    std::vector<atx::f64> pos;
    pos.reserve(np * ni);
    for (std::size_t t = 0; t < np; ++t) {
      const std::span<const atx::f64> row = streams.positions(a, t);
      pos.insert(pos.end(), row.begin(), row.end());
    }
    const auto m = combine::compute_metrics(pnl, pos, ni, 1.0e6);
    seq[a] = FoldResult{a, 0, m.sharpe, m.returns, np};
  }
  const atx::u64 seq_digest = result_table_digest(seq);

  DetPool pool{4};
  const atx::u64 detpool_digest = result_table_digest(parallel_backtests(streams, 1.0e6, pool));
  EXPECT_EQ(detpool_digest, seq_digest)
      << "DetPool& backtests oracle must match the sequential loop";

  for (std::size_t w : kWorkerCounts) {
    ThreadExecutor exec{ExecutorConfig{w}};
    const atx::u64 exec_digest = result_table_digest(parallel_backtests(streams, 1.0e6, exec));
    EXPECT_EQ(exec_digest, seq_digest)
        << "IExecutor& backtests (workers=" << w << ") must be byte-identical to sequential";
    EXPECT_EQ(exec_digest, detpool_digest)
        << "IExecutor& backtests (workers=" << w << ") must be byte-identical to DetPool&";
  }
}

// (2) IExecutor& over ThreadExecutor@1 == @8 (worker-count invariance).
TEST(ParallelWorkloadPort, BacktestsWorkerCountInvariant) {
  const auto streams = make_streams(20, 48, 4, 0x1234ULL);

  ThreadExecutor exec1{ExecutorConfig{1}};
  ThreadExecutor exec8{ExecutorConfig{8}};
  const atx::u64 d1 = result_table_digest(parallel_backtests(streams, 1.0e6, exec1));
  const atx::u64 d8 = result_table_digest(parallel_backtests(streams, 1.0e6, exec8));
  EXPECT_EQ(d1, d8) << "IExecutor& backtests digest must be invariant from 1 to 8 workers";
}

} // namespace
