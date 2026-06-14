#pragma once

// atx::engine::parallel — ThreadExecutor: the in-process IExecutor (S7-1).
//
// The degenerate single-process case of the seam, AND the ORACLE the process
// path (S7.3) is later pinned to: a given (workload, inputs, n) must produce the
// SAME gathered slot bytes on ThreadExecutor and on ProcessExecutor (R7). It is a
// THIN adapter over the as-built DetPool — ZERO new scheduling logic:
//
//   * DetPool::parallel_for already guarantees every index in [0, n) runs exactly
//     once and erects a barrier (it blocks until all are done);
//   * DetPool rethrows the lowest-index *exception*, but our ShardFn reports
//     EXPECTED failure via Status (no throw), so we collect a pre-sized
//     std::vector<Status> and scan it ASCENDING ourselves to return the
//     deterministic lowest-id error — same ordering semantics, no exceptions.
//
// Because each shard writes only its own slot and there is no cross-shard state,
// the gathered output is byte-identical across {1,2,4,8} workers BY CONSTRUCTION
// (R1) — exactly DetPool's determinism contract surfaced through the seam.
//
// Header-only; owns a pinned DetPool, so ThreadExecutor is itself pinned (the
// IExecutor base already deletes copy/assign; DetPool deletes move, which deletes
// ours implicitly — Rule of Zero, nothing to declare).

#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/parallel/executor.hpp"

namespace atx::engine::parallel {

// In-process IExecutor backed by DetPool. `final` — there is nothing to override
// further; it is the leaf substrate for threads.
class ThreadExecutor final : public IExecutor {
public:
  // workers == 0 => DetPool's substrate default (max(1, hw_concurrency - 2)).
  // pin_workers is accepted for seam parity but is a no-op for DetPool, which has
  // no pinning hook yet; the process substrate (S7.3) honours it.
  explicit ThreadExecutor(ExecutorConfig c = {}) : pool_{c.workers} {}

  [[nodiscard]] atx::usize workers() const noexcept override { return pool_.n_workers(); }

  // See IExecutor::submit. Validates geometry, resolves the workload to its
  // ShardFn, fans the shards across DetPool, then reduces the per-shard Statuses
  // ascending to the deterministic lowest-id error.
  //
  // The optional `dispatch_order` permutation (S7-4) reorders ONLY which shard is
  // dispatched at which claim position — the slot a shard writes is unchanged, so
  // the gathered bytes are identical for any valid permutation (no bit contact).
  [[nodiscard]] atx::core::Status submit(WorkloadId workload, InputView inputs, atx::usize n,
                                         SlotView out,
                                         std::span<const ShardId> dispatch_order = {}) override {
    // Geometry precondition: the output must describe exactly n slots, else a
    // shard would write outside the buffer or leave a slot unwritten.
    if (out.n() != n) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "SlotView slot count does not match shard count n");
    }
    // A supplied order must be a valid permutation of exactly [0, n). An empty
    // span is the identity-order default (zero behavior change for existing
    // callers). We validate before any dispatch so a bad order can never index a
    // slot out of bounds (UB) or skip/duplicate a shard.
    if (!dispatch_order.empty() && !is_valid_permutation(dispatch_order, n)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "dispatch_order is not a permutation of [0, n)");
    }
    if (n == 0) {
      return atx::core::Ok(); // no shards => no dispatch, no barrier, empty output
    }

    const ShardFn fn = lookup_workload(workload);
    if (fn == nullptr) {
      return atx::core::Err(atx::core::ErrorCode::NotFound, "workload not registered");
    }

    // One Status slot per shard, pre-sized to Ok(): shard `id` writes ONLY
    // errs[id], so there is no shared mutable state and the fan-out stays
    // race-free (R1). The pool dispenses CLAIM POSITIONS p in [0, n); the body
    // maps position p -> shard id = order[p] (identity when no order is supplied).
    // The shard reads (inputs, id) and writes slot(id) ONLY — so which position a
    // worker grabs cannot move a result bit, exactly as for the identity case.
    const bool use_order = !dispatch_order.empty();
    std::vector<atx::core::Status> errs(n, atx::core::Ok());
    pool_.parallel_for(n, [&](atx::usize p, atx::usize /*wid*/) {
      const ShardId id = use_order ? dispatch_order[p] : p;
      errs[id] = fn(inputs, id, out.slot(id));
    });

    // Deterministic reduce: the lowest-id failing shard wins, independent of which
    // worker ran it or in what order — the seam's R1 guarantee made explicit.
    for (atx::usize i = 0; i < n; ++i) {
      if (!errs[i]) {
        return errs[i];
      }
    }
    return atx::core::Ok();
  }

private:
  // True iff `order` is exactly a permutation of [0, n): size n, every element in
  // range, no duplicates. O(n) with an n-bit seen-vector; pure, no allocation in
  // the steady-state submit path beyond this one cold validation (it runs once per
  // submit, not per shard). Keeps a bad order from forming an out-of-bounds slot
  // subspan (UB) in the dispatch body above.
  [[nodiscard]] static bool is_valid_permutation(std::span<const ShardId> order,
                                                 atx::usize n) noexcept {
    if (order.size() != n) {
      return false;
    }
    std::vector<bool> seen(n, false);
    for (const ShardId id : order) {
      if (id >= n || seen[id]) {
        return false; // out of range or duplicate => not a permutation
      }
      seen[id] = true;
    }
    return true; // size n + all-distinct in-range => covers [0, n) exactly
  }

  DetPool pool_; // owned; pinned (move deleted) => ThreadExecutor is pinned too
};

} // namespace atx::engine::parallel
