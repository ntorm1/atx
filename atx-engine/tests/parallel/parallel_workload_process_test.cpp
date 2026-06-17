// atx::engine::parallel — S7.5b real workloads over the PROCESS boundary proof.
//
// THE CAPSTONE (plan §0.5/§4.6, R1): parallel_backtests and parallel_cpcv run over
// a ProcessExecutor with a gathered-result digest BYTE-IDENTICAL to the
// ThreadExecutor path AND the sequential oracle, invariant across process worker
// counts {1,N}. The real workloads cross the process boundary via the registered
// WorkloadId (Backtests / Cpcv) + serialized AlphaStreams (InputView) seam — NOT a
// C++ closure. This suite proves, for BOTH workloads:
//
//   1. DIGEST IDENTITY — digest(ProcessExecutor@{1,N}) ==
//      digest(ThreadExecutor@{1,8}) == digest(the sequential run_*_ loop), byte for
//      byte via result_table_digest.
//   2. PROCESS WORKER-COUNT INVARIANCE — Process@1 == Process@N.
//   3. PARSE VALIDATION — a malformed / truncated InputView is rejected by the
//      parse view with Err (no OOB), tested directly.
//
// The AlphaStreams + folds fixtures mirror parallel_workload_port_test.cpp /
// parallel_run_test.cpp so the oracle is the already-proven one. The worker exe
// (atx-shm-worker) registers backtests_shard / cpcv_shard in its own main; the test
// process only constructs ProcessExecutor (which spawns that exe).

#include <cstddef>
#include <cstdint>
#include <cstring> // std::memcpy (crafted-header overflow test)
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/eval/cpcv.hpp"

#include "atx/engine/parallel/executor.hpp"
#include "atx/engine/parallel/parallel_run.hpp"
#include "atx/engine/parallel/process_executor.hpp"
#include "atx/engine/parallel/thread_executor.hpp"
#include "atx/engine/parallel/workload_streams.hpp"

namespace {

namespace eval = atx::engine::eval;
namespace combine = atx::engine::combine;

using atx::engine::alpha::AlphaStreams;
using atx::engine::parallel::BacktestsInputView;
using atx::engine::parallel::CpcvInputView;
using atx::engine::parallel::ExecutorConfig;
using atx::engine::parallel::FoldResult;
using atx::engine::parallel::InputView;
using atx::engine::parallel::parallel_backtests;
using atx::engine::parallel::parallel_cpcv;
using atx::engine::parallel::ProcessExecutor;
using atx::engine::parallel::result_table_digest;
using atx::engine::parallel::run_full_backtest;
using atx::engine::parallel::run_one_fold;
using atx::engine::parallel::serialize_backtests_input;
using atx::engine::parallel::serialize_cpcv_input;
using atx::engine::parallel::ThreadExecutor;

constexpr atx::f64 kBook = 1.0e6;

// Synthetic AlphaStreams (mirror of parallel_workload_port_test.cpp::make_streams).
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
    v = nx() * 0.04 - 0.02;
  }
  for (auto& v : s.pos_flat) {
    v = nx() * 2.0 - 1.0;
  }
  return s;
}

[[nodiscard]] std::vector<eval::CpcvFold> make_cpcv_folds(std::size_t n_periods) {
  std::vector<eval::LabelSpan> spans;
  spans.reserve(n_periods);
  for (std::size_t i = 0; i < n_periods; ++i) {
    spans.push_back({i, i + 1});
  }
  return eval::cpcv_folds(spans, eval::CpcvConfig{6, 2, 0.0});
}

// Sequential oracle: run_full_backtest over each alpha in canonical alpha order.
[[nodiscard]] atx::u64 backtests_sequential_digest(const AlphaStreams& streams) {
  std::vector<FoldResult> seq(streams.n_alphas());
  for (std::size_t a = 0; a < streams.n_alphas(); ++a) {
    seq[a] = run_full_backtest(streams, a, kBook);
  }
  return result_table_digest(seq);
}

// Sequential oracle: run_one_fold over each fold in canonical fold order.
[[nodiscard]] atx::u64 cpcv_sequential_digest(const AlphaStreams& streams, std::size_t alpha_id,
                                              std::span<const eval::CpcvFold> folds) {
  std::vector<FoldResult> seq(folds.size());
  for (std::size_t f = 0; f < folds.size(); ++f) {
    seq[f] = run_one_fold(streams, alpha_id, f, folds[f], kBook);
  }
  return result_table_digest(seq);
}

} // namespace

// ===========================================================================
//  BACKTESTS — process@{1,N} == thread@{1,8} == sequential (byte-identical).
// ===========================================================================
TEST(ParallelWorkloadProcess, BacktestsDigestProcessEqualsThreadEqualsSequential) {
  const AlphaStreams streams = make_streams(20, 48, 4, 0x1234ULL);
  const atx::u64 want = backtests_sequential_digest(streams);

  // ThreadExecutor (the in-process oracle) @ 1 and @ 8.
  for (std::size_t w : {std::size_t{1}, std::size_t{8}}) {
    ThreadExecutor te{ExecutorConfig{w}};
    EXPECT_EQ(result_table_digest(parallel_backtests(streams, kBook, te)), want)
        << "ThreadExecutor@" << w << " backtests digest diverged from sequential";
  }

  // ProcessExecutor @ 1 and @ 4 — the cross-process legs (the whole point).
  for (std::size_t w : {std::size_t{1}, std::size_t{4}}) {
    ProcessExecutor pe{ExecutorConfig{w, false}};
    EXPECT_EQ(result_table_digest(parallel_backtests(streams, kBook, pe)), want)
        << "ProcessExecutor@" << w << " backtests digest diverged from sequential";
  }
}

TEST(ParallelWorkloadProcess, BacktestsProcessWorkerCountInvariant) {
  const AlphaStreams streams = make_streams(20, 48, 4, 0x1234ULL);
  ProcessExecutor pe1{ExecutorConfig{1, false}};
  ProcessExecutor peN{ExecutorConfig{4, false}};
  const atx::u64 d1 = result_table_digest(parallel_backtests(streams, kBook, pe1));
  const atx::u64 dN = result_table_digest(parallel_backtests(streams, kBook, peN));
  EXPECT_EQ(d1, dN) << "ProcessExecutor backtests digest must be invariant 1 vs N workers";
}

// ===========================================================================
//  CPCV — process@{1,N} == thread@{1,8} == sequential (byte-identical).
// ===========================================================================
TEST(ParallelWorkloadProcess, CpcvDigestProcessEqualsThreadEqualsSequential) {
  const AlphaStreams streams = make_streams(1, 64, 5, 0xC0FFEEULL);
  const auto folds = make_cpcv_folds(64);
  const atx::u64 want = cpcv_sequential_digest(streams, 0, folds);

  for (std::size_t w : {std::size_t{1}, std::size_t{8}}) {
    ThreadExecutor te{ExecutorConfig{w}};
    EXPECT_EQ(result_table_digest(parallel_cpcv(folds, streams, 0, kBook, te)), want)
        << "ThreadExecutor@" << w << " cpcv digest diverged from sequential";
  }

  for (std::size_t w : {std::size_t{1}, std::size_t{4}}) {
    ProcessExecutor pe{ExecutorConfig{w, false}};
    EXPECT_EQ(result_table_digest(parallel_cpcv(folds, streams, 0, kBook, pe)), want)
        << "ProcessExecutor@" << w << " cpcv digest diverged from sequential";
  }
}

TEST(ParallelWorkloadProcess, CpcvProcessWorkerCountInvariant) {
  const AlphaStreams streams = make_streams(1, 64, 5, 0xC0FFEEULL);
  const auto folds = make_cpcv_folds(64);
  ProcessExecutor pe1{ExecutorConfig{1, false}};
  ProcessExecutor peN{ExecutorConfig{4, false}};
  const atx::u64 d1 = result_table_digest(parallel_cpcv(folds, streams, 0, kBook, pe1));
  const atx::u64 dN = result_table_digest(parallel_cpcv(folds, streams, 0, kBook, peN));
  EXPECT_EQ(d1, dN) << "ProcessExecutor cpcv digest must be invariant 1 vs N workers";
}

// ===========================================================================
//  Multi-alpha cpcv: alpha_id != 0 still resolves correctly over the boundary.
// ===========================================================================
TEST(ParallelWorkloadProcess, CpcvMultiAlphaSelectsCorrectAlpha) {
  const AlphaStreams streams = make_streams(5, 48, 3, 0xBEEFULL);
  const auto folds = make_cpcv_folds(48);
  constexpr std::size_t kAlpha = 3;
  const atx::u64 want = cpcv_sequential_digest(streams, kAlpha, folds);

  ProcessExecutor pe{ExecutorConfig{4, false}};
  EXPECT_EQ(result_table_digest(parallel_cpcv(folds, streams, kAlpha, kBook, pe)), want)
      << "ProcessExecutor cpcv must select alpha " << kAlpha << " correctly across the boundary";
}

// ===========================================================================
//  PARSE VALIDATION — a truncated / mis-tagged InputView is rejected with Err
//  (no OOB span). Tested directly on the parse views (the shard's first gate).
// ===========================================================================
TEST(ParallelWorkloadProcess, BacktestsParseRejectsTruncatedAndBadMagic) {
  const AlphaStreams streams = make_streams(4, 16, 3, 0x99ULL);
  const std::vector<std::byte> good = serialize_backtests_input(streams, kBook);

  // Full buffer parses.
  EXPECT_TRUE(BacktestsInputView::parse(InputView{std::span<const std::byte>{good}}).has_value());

  // Truncated mid-array: the pos region now overruns the buffer -> Err.
  for (std::size_t cut : {std::size_t{0}, std::size_t{4}, std::size_t{20},
                          good.size() - sizeof(atx::f64)}) {
    const std::span<const std::byte> trunc{good.data(), cut};
    EXPECT_FALSE(BacktestsInputView::parse(InputView{trunc}).has_value())
        << "truncated-to-" << cut << " buffer must be rejected";
  }

  // Corrupt magic -> Err.
  std::vector<std::byte> bad = good;
  bad[0] = std::byte{0xFF};
  EXPECT_FALSE(BacktestsInputView::parse(InputView{std::span<const std::byte>{bad}}).has_value());

  // Empty buffer -> Err (no magic).
  EXPECT_FALSE(BacktestsInputView::parse(InputView{std::span<const std::byte>{}}).has_value());
}

TEST(ParallelWorkloadProcess, CpcvParseRejectsTruncatedAndBadMagic) {
  const AlphaStreams streams = make_streams(2, 32, 3, 0x55ULL);
  const auto folds = make_cpcv_folds(32);
  const std::vector<std::byte> good = serialize_cpcv_input(streams, 1, kBook, folds);

  EXPECT_TRUE(CpcvInputView::parse(InputView{std::span<const std::byte>{good}}).has_value());

  // Truncate into the fold offset table / data region -> Err.
  for (std::size_t cut : {std::size_t{0}, std::size_t{16}, std::size_t{32},
                          good.size() - sizeof(atx::u64)}) {
    const std::span<const std::byte> trunc{good.data(), cut};
    EXPECT_FALSE(CpcvInputView::parse(InputView{trunc}).has_value())
        << "truncated-to-" << cut << " cpcv buffer must be rejected";
  }

  std::vector<std::byte> bad = good;
  bad[0] = std::byte{0x00};
  EXPECT_FALSE(CpcvInputView::parse(InputView{std::span<const std::byte>{bad}}).has_value());
}

// A crafted header with huge u32 dimensions over a tiny buffer must be rejected by
// the overflow-checked dimension product (na*np*ni would wrap usize) — no OOB alias.
TEST(ParallelWorkloadProcess, BacktestsParseRejectsOverflowingDimensions) {
  std::vector<std::byte> buf(atx::engine::parallel::kBacktestsHeaderBytes, std::byte{0});
  const atx::u32 magic = atx::engine::parallel::kBacktestsMagic;
  const atx::u32 huge = 0xFFFFFFFFU; // na = np = ni = u32_max => na*np*ni overflows usize
  std::memcpy(buf.data() + 0, &magic, 4);
  std::memcpy(buf.data() + 4, &huge, 4);  // n_alphas
  std::memcpy(buf.data() + 8, &huge, 4);  // n_periods
  std::memcpy(buf.data() + 12, &huge, 4); // n_instruments
  // book_size at [16,24) left zero. The dimension product overflows -> Err, not OOB.
  EXPECT_FALSE(BacktestsInputView::parse(InputView{std::span<const std::byte>{buf}}).has_value());
}

// ===========================================================================
//  ROUND-TRIP — parse view's per-alpha spans match the source AlphaStreams bytes.
// ===========================================================================
TEST(ParallelWorkloadProcess, BacktestsParseRoundTripsStreams) {
  const AlphaStreams streams = make_streams(3, 12, 4, 0x77ULL);
  const std::vector<std::byte> buf = serialize_backtests_input(streams, kBook);
  const auto parsed = BacktestsInputView::parse(InputView{std::span<const std::byte>{buf}});
  ASSERT_TRUE(parsed.has_value());
  const BacktestsInputView& v = parsed.value();

  ASSERT_EQ(v.n_alphas(), streams.n_alphas());
  ASSERT_EQ(v.n_periods(), streams.n_periods());
  ASSERT_EQ(v.n_instruments(), streams.n_instruments());
  EXPECT_EQ(v.book_size(), kBook);

  for (std::size_t a = 0; a < streams.n_alphas(); ++a) {
    const std::span<const atx::f64> got = v.pnl_of(a);
    const std::span<const atx::f64> want = streams.pnl(a);
    ASSERT_EQ(got.size(), want.size());
    for (std::size_t t = 0; t < got.size(); ++t) {
      EXPECT_EQ(got[t], want[t]) << "pnl mismatch a=" << a << " t=" << t;
    }
  }
}
