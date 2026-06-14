#include "atx/engine/parallel/parallel_run.hpp"

#include <algorithm>  // std::stable_sort
#include <cmath>      // std::abs (Neumaier branch)
#include <cstddef>    // std::size_t (pool body params)
#include <functional> // std::function (dispatch wrapper signature)
#include <span>       // std::span (read-only inputs)
#include <vector>     // std::vector (owned result table / gather buffers)

#include "atx/core/macro.hpp" // ATX_ASSERT, ATX_CHECK
#include "atx/core/types.hpp" // f64 / u64 / usize

#include "atx/engine/alpha/streams.hpp"     // alpha::AlphaStreams
#include "atx/engine/combine/metrics.hpp"   // combine::compute_metrics, AlphaMetrics
#include "atx/engine/eval/cpcv.hpp"         // eval::CpcvFold
#include "atx/engine/parallel/det_pool.hpp" // DetPool

namespace atx::engine::parallel {

namespace {

// The per-item map body: body(item, worker_id), invoked once per item.
using MapBody = std::function<void(std::size_t, std::size_t)>;

// Substrate-agnostic dispatch: run body(item, worker_id) for item in [0, n),
// blocking until all complete and rethrowing the lowest-index body exception
// (the DetPool::parallel_for / IExecutor::parallel_for contract). Both the
// DetPool& and IExecutor& workload overloads build one of these so there is a
// SINGLE map per workload — the body (run_one_fold / run_full_backtest) is
// shared, only the dispatch differs.
using Dispatch = std::function<void(std::size_t, const MapBody &)>;

// Wrap a DetPool: parallel_for returns void and rethrows the lowest-index
// exception — no Status to check.
[[nodiscard]] Dispatch pool_dispatch(DetPool &pool) {
  return [&pool](std::size_t n, const MapBody &body) { pool.parallel_for(n, body); };
}

// Wrap an IExecutor: parallel_for returns a Status. These workloads are IN-PROCESS
// ONLY (the body is a closure); an in-process executor (ThreadExecutor) always
// returns Ok() for these non-throwing bodies. A non-Ok return means the caller
// handed an out-of-process substrate (ProcessExecutor, whose parallel_for rejects
// a closure body with Err(NotImplemented)) to an in-process-only map — a
// programmer error. We guard it with the ALWAYS-ON ATX_CHECK (NOT debug-only
// ATX_ASSERT): under NDEBUG a debug assert would elide, the map body would never
// run, and the function would return a zero-filled FoldResult table as if it had
// succeeded — silent data corruption. ATX_CHECK aborts loudly in BOTH debug and
// release, so misuse is fail-safe (no corrupt table) while these overloads keep
// their plain std::vector<FoldResult> return. The process substrate routes these
// workloads through the serialized submit() path in a later unit (S7.5c/d), never
// through this in-process-only overload.
[[nodiscard]] Dispatch exec_dispatch(IExecutor &exec) {
  return [&exec](std::size_t n, const MapBody &body) {
    const atx::core::Status s = exec.parallel_for(n, body);
    ATX_CHECK(s.has_value()); // out-of-process executor here is misuse: abort, never corrupt
    (void)s;
  };
}

// THE ONE parallel-CPCV map: every fold of a single alpha into out[f]. Shared by
// both overloads; only `dispatch` differs. out is pre-sized before the parallel
// region and item f writes ONLY out[f] — byte-identical across substrates / worker
// counts by construction (the §4 no-bit-contact map).
[[nodiscard]] std::vector<FoldResult>
parallel_cpcv_impl(std::span<const atx::engine::eval::CpcvFold> folds,
                   const atx::engine::alpha::AlphaStreams &streams, atx::usize alpha_id,
                   atx::f64 book_size, const Dispatch &dispatch) {
  std::vector<FoldResult> out(folds.size());
  // SAFETY: each f writes only out[f]; streams + folds are const; no cross-worker
  // FP accumulation -> byte-identical across {1,2,4,8} workers by construction.
  dispatch(folds.size(), [&](std::size_t f, std::size_t /*worker_id*/) {
    out[f] = run_one_fold(streams, alpha_id, f, folds[f], book_size);
  });
  return out;
}

// THE ONE parallel-backtests map: one full-sample backtest per alpha into out[a].
// Shared by both overloads; only `dispatch` differs.
[[nodiscard]] std::vector<FoldResult>
parallel_backtests_impl(const atx::engine::alpha::AlphaStreams &streams, atx::f64 book_size,
                        const Dispatch &dispatch) {
  std::vector<FoldResult> out(streams.n_alphas());
  // SAFETY: each a writes only out[a]; streams is const; no cross-worker FP
  // accumulation -> byte-identical across {1,2,4,8} workers by construction.
  dispatch(streams.n_alphas(), [&](std::size_t a, std::size_t /*worker_id*/) {
    out[a] = run_full_backtest(streams, a, book_size);
  });
  return out;
}

} // namespace

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
FoldResult run_one_fold(const atx::engine::alpha::AlphaStreams& streams,
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
FoldResult
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
std::vector<FoldResult>
parallel_cpcv(std::span<const atx::engine::eval::CpcvFold> folds,
              const atx::engine::alpha::AlphaStreams& streams, atx::usize alpha_id,
              atx::f64 book_size, DetPool& pool) {
  return parallel_cpcv_impl(folds, streams, alpha_id, book_size, pool_dispatch(pool));
}

// S7.5a — the SAME parallel CPCV over the IExecutor seam (THREAD substrate). Shares
// parallel_cpcv_impl with the DetPool& overload, substituting exec.parallel_for —
// the map body (run_one_fold) is UNCHANGED, output byte-identical to the DetPool& /
// sequential path and invariant across worker counts.
std::vector<FoldResult>
parallel_cpcv(std::span<const atx::engine::eval::CpcvFold> folds,
              const atx::engine::alpha::AlphaStreams& streams, atx::usize alpha_id,
              atx::f64 book_size, IExecutor& exec) {
  return parallel_cpcv_impl(folds, streams, alpha_id, book_size, exec_dispatch(exec));
}

// ===========================================================================
//  parallel_backtests — one full-sample backtest per alpha (different alphas are
//  independent items), into out[a] (alpha-id order). Pure map; out pre-sized
//  before the parallel region; item a writes ONLY out[a].
// ===========================================================================
std::vector<FoldResult>
parallel_backtests(const atx::engine::alpha::AlphaStreams& streams, atx::f64 book_size,
                   DetPool& pool) {
  return parallel_backtests_impl(streams, book_size, pool_dispatch(pool));
}

// S7.5a — the SAME parallel backtests over the IExecutor seam (THREAD substrate).
// Shares parallel_backtests_impl with the DetPool& overload, substituting
// exec.parallel_for — the map body (run_full_backtest) is UNCHANGED, output
// byte-identical to the DetPool& / sequential path and invariant across worker counts.
std::vector<FoldResult>
parallel_backtests(const atx::engine::alpha::AlphaStreams& streams, atx::f64 book_size,
                   IExecutor& exec) {
  return parallel_backtests_impl(streams, book_size, exec_dispatch(exec));
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
atx::f64 cpcv_aggregate_mean_sharpe(std::span<const FoldResult> rows) {
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
