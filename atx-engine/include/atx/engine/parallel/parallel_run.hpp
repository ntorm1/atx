#pragma once

// atx::engine::parallel — parallel CPCV folds + per-alpha backtests, with a
// fixed-order result table and a reduce-by-sort aggregate (S2-3).
//
// ===========================================================================
//  DETERMINISM CONTRACT — the whole identity of this unit
// ===========================================================================
//  Every result here is byte-identical to the single-thread path AND invariant
//  across worker counts {1,2,4,8}. This is achieved BY CONSTRUCTION, not by a
//  clever reduction:
//
//   * MAP: each fold / each backtest is computed FULLY by one worker into a
//     pre-indexed out[i] slot. The reads are CONST (the AlphaStreams and the
//     fold index sets are read-only) and there is NO writable shared state
//     between items — so which worker grabs which index cannot touch a single
//     bit of any result.
//   * FIXED-ORDER ASSEMBLE: out is pre-sized before the parallel region and
//     item f/a writes ONLY out[f] / out[a]; the table is therefore in canonical
//     (alpha_id, fold_id) slot order, never completion order. result_table_digest
//     walks that slot order.
//   * REDUCE-BY-SORT (R3): the ONLY cross-item reduction — the aggregate over
//     per-fold scalars — copies the rows, stable-sorts them by (alpha_id,
//     fold_id), then does a SEQUENTIAL Neumaier fold. It is NEVER an atomic
//     running sum and NEVER a partial combine in completion order, so it is
//     bit-identical regardless of how (or in what order) the table was produced.
//   * NO std::thread::hardware_concurrency() in any result math; the pool's
//     atomic dispenser decides who runs what, and that does not enter the bits.
//
// ===========================================================================
//  AS-BUILT (v1) vs the heavyweight spec framing
// ===========================================================================
//  A "fold backtest" here is the faithful-but-light v1: a metric computed over
//  the fold's TEST observations gathered from the (const) AlphaStreams, via the
//  SAME combine::compute_metrics that S1 / P4 use. This is a pure map — there is
//  no full templated BacktestLoop and no shared mutable Portfolio / per-fold
//  ExecutionSimulator. The heavyweight isolated-Portfolio / ExecutionSimulator
//  framing in the sprint spec is recovered in a later unit; the determinism
//  guarantee (digest(parallel) == digest(sequential), invariant across worker
//  counts) is identical either way because the per-item compute is pure.
//
//  result_table_digest mirrors digest.hpp::signal_set_digest: a canonical-order
//  wyhash over RAW f64 bytes, so a 1-ULP drift under a different worker count or
//  build flips the digest (the point of a bit-identity oracle).
//
//  Header-only; every free function is inline. [[nodiscard]] / noexcept / const
//  discipline throughout. Reuses atx-core hash + combine::compute_metrics — no
//  new numeric primitive is introduced.

#include <algorithm> // std::stable_sort
#include <cmath>     // std::abs (Neumaier branch)
#include <cstddef>   // std::size_t (hash_combine seed, pool body params)
#include <span>      // std::span (read-only inputs)
#include <vector>    // std::vector (owned result table / gather buffers)

#include "atx/core/hash.hpp"  // hash_bytes / hash_combine
#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // f64 / u64 / usize

#include "atx/engine/alpha/streams.hpp"     // alpha::AlphaStreams
#include "atx/engine/combine/metrics.hpp"   // combine::compute_metrics, AlphaMetrics
#include "atx/engine/eval/cpcv.hpp"         // eval::CpcvFold
#include "atx/engine/parallel/det_pool.hpp" // DetPool
#include "atx/engine/parallel/fwd.hpp"      // struct FoldResult; (forward-declared)

namespace atx::engine::parallel {

// ===========================================================================
//  FoldResult — one row of the result table: a fold's (or a backtest's) summary,
//  pre-indexed by canonical (alpha_id, fold_id) for fixed-order assemble and
//  reduce-by-sort aggregation. Rule of Zero POD; owns nothing.
// ===========================================================================
struct FoldResult {
  atx::usize alpha_id{}; // which alpha this row summarizes
  atx::usize fold_id{};  // which CPCV fold (sentinel 0 for a full-sample backtest)
  atx::f64 sharpe{};     // combine::compute_metrics(...).sharpe over the row's obs
  atx::f64 returns{};    // combine::compute_metrics(...).returns over the row's obs
  atx::usize n_test{};   // number of test observations the metric used
};

// ===========================================================================
//  result_table_digest — canonical-order wyhash over RAW f64 bytes; the
//  determinism oracle for a result table.
//
//  Mirrors digest.hpp::signal_set_digest's raw-byte form: equal iff the two
//  tables are BIT-IDENTICAL in row order, every id / count, and every scalar's
//  raw f64 bits. A 1-ULP drift in sharpe or returns flips the digest (the whole
//  point — catches a result that changed under a different worker count). The
//  caller hands rows already in canonical slot order; the walk preserves it.
// ===========================================================================
[[nodiscard]] inline atx::u64 result_table_digest(std::span<const FoldResult> rows) noexcept {
  std::size_t h = atx::core::hash_combine(std::size_t{0}, rows.size());
  for (const FoldResult& r : rows) {
    h = atx::core::hash_combine(h, r.alpha_id, r.fold_id, r.n_test);
    // Raw f64 bytes (NOT a normalized numeric form): 1-ULP => digest flips.
    const atx::u64 sharpe_bits = atx::core::hash_bytes(&r.sharpe, sizeof r.sharpe);
    const atx::u64 returns_bits = atx::core::hash_bytes(&r.returns, sizeof r.returns);
    h = atx::core::hash_combine(h, sharpe_bits, returns_bits);
  }
  return static_cast<atx::u64>(h);
}

// ===========================================================================
//  run_one_fold — the per-fold metric (the pure map primitive).
//
//  Gathers the test-index subset of alpha `alpha_id`'s pnl (and the matching
//  position cross-sections) into contiguous buffers, then runs the SAME
//  combine::compute_metrics S1 / P4 use. Reads `streams` and `fold` strictly
//  CONST — no shared mutable state, so it is safe to invoke concurrently for
//  different fold ids. test_idx values are valid period indices (< n_periods),
//  the caller's contract (asserted in debug).
// ===========================================================================
[[nodiscard]] inline FoldResult run_one_fold(const atx::engine::alpha::AlphaStreams& streams,
                                             atx::usize alpha_id, atx::usize fold_id,
                                             const atx::engine::eval::CpcvFold& fold,
                                             atx::f64 book_size) {
  ATX_ASSERT(alpha_id < streams.n_alphas());
  const atx::usize n_inst = streams.n_instruments();
  const std::span<const atx::f64> pnl = streams.pnl(alpha_id);

  std::vector<atx::f64> test_pnl;
  test_pnl.reserve(fold.test_idx.size());
  std::vector<atx::f64> test_pos;
  test_pos.reserve(fold.test_idx.size() * n_inst);

  for (const atx::usize t : fold.test_idx) {
    ATX_ASSERT(t < streams.n_periods());
    test_pnl.push_back(pnl[t]);
    const std::span<const atx::f64> row = streams.positions(alpha_id, t);
    test_pos.insert(test_pos.end(), row.begin(), row.end());
  }

  const combine::AlphaMetrics m =
      combine::compute_metrics(test_pnl, test_pos, n_inst, book_size);
  return FoldResult{alpha_id, fold_id, m.sharpe, m.returns, fold.test_idx.size()};
}

// ===========================================================================
//  run_full_backtest — the per-alpha full-sample metric (the backtest map
//  primitive). Treats the whole period range as the observation set: runs
//  compute_metrics over alpha `alpha_id`'s entire pnl / position stream. Pure;
//  CONST reads only. fold_id is the sentinel 0.
// ===========================================================================
[[nodiscard]] inline FoldResult
run_full_backtest(const atx::engine::alpha::AlphaStreams& streams, atx::usize alpha_id,
                  atx::f64 book_size) {
  ATX_ASSERT(alpha_id < streams.n_alphas());
  const atx::usize n_periods = streams.n_periods();
  const atx::usize n_inst = streams.n_instruments();
  const std::span<const atx::f64> pnl = streams.pnl(alpha_id);

  // Gather the full position stream period-major then instrument-minor (the
  // layout compute_metrics expects). One contiguous buffer, no shared state.
  std::vector<atx::f64> pos;
  pos.reserve(n_periods * n_inst);
  for (atx::usize t = 0; t < n_periods; ++t) {
    const std::span<const atx::f64> row = streams.positions(alpha_id, t);
    pos.insert(pos.end(), row.begin(), row.end());
  }

  const combine::AlphaMetrics m = combine::compute_metrics(pnl, pos, n_inst, book_size);
  return FoldResult{alpha_id, 0, m.sharpe, m.returns, n_periods};
}

// ===========================================================================
//  parallel_cpcv — every fold of a SINGLE alpha across the pool, into out[f]
//  (fold-id order). Pure map: out is pre-sized before the parallel region and
//  item f writes ONLY out[f].
// ===========================================================================
[[nodiscard]] inline std::vector<FoldResult>
parallel_cpcv(std::span<const atx::engine::eval::CpcvFold> folds,
              const atx::engine::alpha::AlphaStreams& streams, atx::usize alpha_id,
              atx::f64 book_size, DetPool& pool) {
  std::vector<FoldResult> out(folds.size());
  // SAFETY: each f writes only out[f]; streams + folds are const; no cross-worker
  // FP accumulation -> byte-identical across {1,2,4,8} workers by construction.
  pool.parallel_for(folds.size(), [&](std::size_t f, std::size_t /*worker_id*/) {
    out[f] = run_one_fold(streams, alpha_id, f, folds[f], book_size);
  });
  return out;
}

// ===========================================================================
//  parallel_backtests — one full-sample backtest per alpha (different alphas are
//  independent items), into out[a] (alpha-id order). Pure map; out pre-sized
//  before the parallel region; item a writes ONLY out[a].
// ===========================================================================
[[nodiscard]] inline std::vector<FoldResult>
parallel_backtests(const atx::engine::alpha::AlphaStreams& streams, atx::f64 book_size,
                   DetPool& pool) {
  std::vector<FoldResult> out(streams.n_alphas());
  // SAFETY: each a writes only out[a]; streams is const; no cross-worker FP
  // accumulation -> byte-identical across {1,2,4,8} workers by construction.
  pool.parallel_for(streams.n_alphas(), [&](std::size_t a, std::size_t /*worker_id*/) {
    out[a] = run_full_backtest(streams, a, book_size);
  });
  return out;
}

// ===========================================================================
//  cpcv_aggregate_mean_sharpe — reduce-by-sort (R3): the mean of the per-row
//  `sharpe`, made deterministic for ANY input order.
//
//  Copies the rows, stable-sorts by (alpha_id, fold_id), then a SEQUENTIAL
//  Neumaier (compensated) fold of `sharpe`, divided by the count. The sort makes
//  it order-independent; the sequential compensated fold makes it bit-identical
//  regardless of how the table was produced. NEVER an atomic running sum. An
//  empty table returns 0.0 (documented sentinel).
// ===========================================================================
[[nodiscard]] inline atx::f64 cpcv_aggregate_mean_sharpe(std::span<const FoldResult> rows) noexcept {
  if (rows.empty()) {
    return 0.0; // documented sentinel for an empty table
  }
  // Copy + canonical (alpha_id, fold_id) stable-sort: order-independence.
  std::vector<FoldResult> sorted(rows.begin(), rows.end());
  std::stable_sort(sorted.begin(), sorted.end(), [](const FoldResult& a, const FoldResult& b) {
    return (a.alpha_id != b.alpha_id) ? (a.alpha_id < b.alpha_id) : (a.fold_id < b.fold_id);
  });

  // Sequential Neumaier (Kahan-Babuška) fold: bit-identical for a fixed order.
  atx::f64 sum = 0.0;
  atx::f64 comp = 0.0; // running compensation for lost low-order bits
  for (const FoldResult& r : sorted) {
    const atx::f64 t = sum + r.sharpe;
    comp += (std::abs(sum) >= std::abs(r.sharpe)) ? (sum - t) + r.sharpe : (r.sharpe - t) + sum;
    sum = t;
  }
  return (sum + comp) / static_cast<atx::f64>(sorted.size());
}

} // namespace atx::engine::parallel
