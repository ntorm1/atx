#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/combine/metrics.hpp"
#include "atx/engine/eval/cpcv.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/parallel_run.hpp"

namespace eval = atx::engine::eval;
namespace combine = atx::engine::combine;
using atx::engine::alpha::AlphaStreams;
using atx::engine::parallel::cpcv_aggregate_mean_sharpe;
using atx::engine::parallel::DetPool;
using atx::engine::parallel::FoldResult;
using atx::engine::parallel::parallel_backtests;
using atx::engine::parallel::parallel_cpcv;
using atx::engine::parallel::result_table_digest;
using atx::engine::parallel::run_one_fold;

// ===========================================================================
//  Synthetic AlphaStreams fixture — deterministic LCG fill of the PUBLIC
//  members (no extract_streams, no <random>). pnl small returns, pos weights.
// ===========================================================================
[[nodiscard]] static AlphaStreams make_streams(std::size_t n_alphas, std::size_t n_periods,
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

// ---------------------------------------------------------------------------
//  Load-bearing #1: the parallel CPCV table digest is BIT-EQUAL to a plain
//  sequential loop computed with the same per-fold primitive (run_one_fold).
// ---------------------------------------------------------------------------
TEST(ParallelRun, CpcvDigestMatchesSequential) {
  const auto streams = make_streams(1, 64, 5, 0xC0FFEEULL);
  std::vector<eval::LabelSpan> spans;
  for (std::size_t i = 0; i < 64; ++i) {
    spans.push_back({i, i + 1});
  }
  const auto folds = eval::cpcv_folds(spans, eval::CpcvConfig{6, 2, 0.0});

  std::vector<FoldResult> seq(folds.size());
  for (std::size_t f = 0; f < folds.size(); ++f) {
    seq[f] = run_one_fold(streams, 0, f, folds[f], 1.0e6);
  }

  DetPool pool{4};
  const auto par = parallel_cpcv(folds, streams, 0, 1.0e6, pool);
  EXPECT_EQ(result_table_digest(par), result_table_digest(seq));
}

// ---------------------------------------------------------------------------
//  Load-bearing #2: the digest is invariant across worker counts {1,2,4,8}.
// ---------------------------------------------------------------------------
TEST(ParallelRun, CpcvThreadCountInvariant) {
  const auto streams = make_streams(1, 64, 5, 0xC0FFEEULL);
  std::vector<eval::LabelSpan> spans;
  for (std::size_t i = 0; i < 64; ++i) {
    spans.push_back({i, i + 1});
  }
  const auto folds = eval::cpcv_folds(spans, eval::CpcvConfig{6, 2, 0.0});

  atx::u64 ref = 0;
  for (std::size_t w : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
    DetPool pool{w};
    const auto d = result_table_digest(parallel_cpcv(folds, streams, 0, 1.0e6, pool));
    if (w == 1) {
      ref = d;
    } else {
      EXPECT_EQ(d, ref) << "workers=" << w;
    }
  }
}

// ---------------------------------------------------------------------------
//  Load-bearing #3 (backtests): parallel per-alpha backtests == sequential.
//  The sequential reference computes the SAME full-sample metric the impl
//  uses (compute_metrics over alpha a's whole pnl/pos), so this is a true
//  parallel-vs-sequential check, not a vacuous "it ran".
// ---------------------------------------------------------------------------
TEST(ParallelRun, BacktestsDigestMatchesSequential) {
  const auto streams = make_streams(20, 48, 4, 0x1234ULL);
  const std::size_t np = streams.n_periods();
  const std::size_t ni = streams.n_instruments();

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

  DetPool pool{4};
  const auto par = parallel_backtests(streams, 1.0e6, pool);
  EXPECT_EQ(result_table_digest(par), result_table_digest(seq));
}

// ---------------------------------------------------------------------------
//  Load-bearing #4: reduce-by-sort aggregate is ORDER-INDEPENDENT (sort +
//  sequential Neumaier fold) and bit-equal to a hand fold of the sorted rows.
// ---------------------------------------------------------------------------
TEST(ParallelRun, AggregateIsReduceBySortOrderIndependent) {
  std::vector<FoldResult> a = {
      {0, 0, 1.25, 0.10, 10},   {0, 1, -0.50, 0.02, 12}, {1, 0, 2.75, 0.30, 8},
      {1, 2, 0.125, 0.01, 9},   {0, 2, 3.5, 0.05, 11},   {1, 1, -1.0, 0.04, 7},
  };
  std::vector<FoldResult> b = a;
  std::reverse(b.begin(), b.end());
  // Order-independence: same rows, different order -> identical aggregate.
  EXPECT_EQ(cpcv_aggregate_mean_sharpe(a), cpcv_aggregate_mean_sharpe(b));

  // Bit-equal to a hand sequential Neumaier fold of the (alpha,fold)-sorted sharpes.
  std::vector<FoldResult> sorted = a;
  std::stable_sort(sorted.begin(), sorted.end(), [](const FoldResult& x, const FoldResult& y) {
    return (x.alpha_id != y.alpha_id) ? (x.alpha_id < y.alpha_id) : (x.fold_id < y.fold_id);
  });
  atx::f64 sum = 0.0;
  atx::f64 comp = 0.0; // Neumaier compensation
  for (const auto& r : sorted) {
    const atx::f64 t = sum + r.sharpe;
    comp += (std::abs(sum) >= std::abs(r.sharpe)) ? (sum - t) + r.sharpe : (r.sharpe - t) + sum;
    sum = t;
  }
  const atx::f64 expected = (sum + comp) / static_cast<atx::f64>(sorted.size());
  EXPECT_EQ(cpcv_aggregate_mean_sharpe(a), expected);
}

// ---------------------------------------------------------------------------
//  Empty input -> empty table (no work dispatched, no UB on the empty span).
// ---------------------------------------------------------------------------
TEST(ParallelRun, EmptyFoldsYieldEmptyTable) {
  const auto streams = make_streams(1, 8, 2, 1ULL);
  DetPool pool{4};
  EXPECT_TRUE(
      parallel_cpcv(std::span<const eval::CpcvFold>{}, streams, 0, 1.0e6, pool).empty());
  // Aggregate of an empty table is the documented sentinel (0.0).
  EXPECT_EQ(cpcv_aggregate_mean_sharpe(std::span<const FoldResult>{}), 0.0);
}
