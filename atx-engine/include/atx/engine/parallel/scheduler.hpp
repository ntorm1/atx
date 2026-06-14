#pragma once

// atx::engine::parallel — Scheduler: deterministic heterogeneous-cost dispatch
// ordering + NUMA/affinity discipline (S7-4).
//
// THE GOVERNING CONTRACT (§0.7 / §4.4 / R1): this is a PERFORMANCE layer with
// **NO BIT CONTACT** — it must be provably unable to change a single result bit.
// It does three things, none of which can perturb the digest:
//
//   (a) DISPATCH ORDER — `dispatch_order` returns a permutation of the shard ids
//       sorted by DESCENDING known cost (longest-processing-time-first), so the
//       expensive shards start first and the tail shortens. This reorders *which
//       shard runs when*, NOT *which slot a shard writes*: slot s is still written
//       only by shard s and the reduce reads slots in canonical id order. So the
//       digest is invariant to the dispatch order (the §4.4 proof: identity vs
//       cost-sorted vs reversed -> ONE digest). It is a PURE function of the cost
//       hint only — no clock, no thread id, no address-dependent ordering.
//   (b) AFFINITY + FIRST-TOUCH — `pin_and_first_touch` pins the calling worker to
//       a PU (spread across NUMA nodes, OMP_PROC_BIND=spread analogue, §1A B6) and
//       write-touches its own slot pages so they land NUMA-local. Best-effort: a
//       failure to pin is non-fatal (pinning is a hint, never correctness). All
//       <windows.h>/POSIX code lives behind this declaration in scheduler.cpp.
//   (c) POOL SIZE — `pool_size` clamps the requested worker count to the bandwidth
//       knee (configurable, default n_cores; all-cores can be SLOWER — §1A B6).
//   (d) OVERSUBSCRIPTION GUARD — `assert_single_pool` asserts exactly one global
//       pool exists, so a nested BLAS/OpenMP runtime cannot multiply against the
//       executor's threads; `assert_nested_blas_pinned` documents that a nested
//       numeric pool must be pinned to one thread.
//
// Header carries declarations + the PURE, OS-free `dispatch_order` (it is a hot,
// per-submit call and trivially inlineable; it touches no OS). Everything that
// queries the OS (topology, affinity, first-touch) is declared here and defined
// in scheduler.cpp behind the platform seam — mirroring shm_segment.hpp/.cpp.

#include <algorithm> // std::sort, std::is_sorted
#include <cstddef>
#include <span>
#include <vector>

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp"

#include "atx/engine/parallel/executor.hpp" // ShardId, kCacheLine

namespace atx::engine::parallel {

// Machine topology, queried ONCE on the cold path and cached on the executor.
// A conservative single-node fallback (n_numa_nodes == 1, every PU on node 0) is
// always valid and deterministic, so query_topology never fails.
struct Topology {
  atx::usize n_cores{1};              // physical/logical cores the scheduler will spread over
  atx::usize n_numa_nodes{1};         // NUMA nodes (>= 1; 1 on a non-NUMA / fallback box)
  atx::usize n_pus{1};                // processing units (logical processors) to pin onto
  std::vector<atx::usize> pu_to_node; // size == n_pus; pu -> NUMA node (first-touch + spread)
  atx::usize line_bytes{
      kCacheLine}; // == std::hardware_destructive_interference_size (slot padding)
};

// Query the machine topology (cold path; OS-queried with a conservative
// single-node fallback). Win32 GetLogicalProcessorInformationEx / POSIX
// sysconf + /sys/devices; on any query failure it returns a deterministic
// single-node topology (n_cores from the hw count, all PUs on node 0).
// `line_bytes` always equals kCacheLine so it cannot disagree with the SlotView
// padding (a mismatch would be a false-sharing bug, never a correctness one).
[[nodiscard]] Topology query_topology();

// Deterministic heterogeneous-cost dispatcher + NUMA discipline. Holds a cached
// Topology; all members are either PURE (dispatch_order, pool_size) or best-effort
// performance hints (pin_and_first_touch) — none can touch a result bit.
struct Scheduler {
  Topology topo{};

  // Bandwidth knee: the worker count past which adding cores stops helping (or
  // hurts — §1A B6). Default = n_cores; configurable so a bandwidth-bound caller
  // can cap below the core count. `pool_size` never returns more than this.
  atx::usize bandwidth_knee{topo.n_cores};

  // ---- (a) DISPATCH ORDER -------------------------------------------------
  // A PURE, DETERMINISTIC permutation of [0, n) where n == cost_hint.size(),
  // sorted by DESCENDING cost (longest-processing-time-first). Ties (and the
  // empty / uniform-cost case) break by ASCENDING ShardId, so the result is a
  // deterministic function of the input ONLY — no clock, no thread id, no
  // address-dependent ordering. ALWAYS a valid permutation of exactly [0, n):
  // each id in [0, n) appears exactly once. Costs are HINTS — a wrong hint costs
  // wall-clock, never bits (the slots are still written by shard id).
  //
  // Precondition (R1 determinism): cost_hint must be NaN-free (a NaN breaks the
  // strict-weak-ordering `>` comparator, which is UB for std::sort). The caller's
  // cost estimate (node count / fold length) is a finite count, so this holds; we
  // ATX_ASSERT it as a programmer-error precondition rather than silently degrade.
  [[nodiscard]] std::vector<ShardId> dispatch_order(std::span<const atx::f64> cost_hint) const {
    const atx::usize n = cost_hint.size();
    std::vector<ShardId> order(n);
    for (atx::usize i = 0; i < n; ++i) {
      order[i] = i; // identity seed; the sort below is a stable total order
    }
    // Total order: higher cost first; equal cost -> lower ShardId first. This is a
    // strict weak ordering (irreflexive, transitive, with a deterministic tie-break
    // on the unique id), so std::sort yields one canonical permutation. We do NOT
    // require std::stable_sort because the id tie-break already makes the order
    // total — two distinct ids never compare equal.
    std::sort(order.begin(), order.end(), [cost_hint](ShardId a, ShardId b) noexcept {
      const atx::f64 ca = cost_hint[a];
      const atx::f64 cb = cost_hint[b];
      ATX_ASSERT(ca == ca && cb == cb); // NaN-free precondition (NaN breaks the order)
      if (ca != cb) {
        return ca > cb; // descending cost: the expensive shard dispatches first
      }
      return a < b; // deterministic tie-break: ascending ShardId
    });
    return order;
  }

  // ---- (b) AFFINITY + FIRST-TOUCH ----------------------------------------
  // Pin the CALLING thread (worker `worker_id`) to a PU, spread across NUMA nodes
  // for bandwidth, and FIRST-TOUCH (write a byte per page of) `my_slots` so the
  // pages land on this worker's local node. BEST-EFFORT: a failure to pin is
  // logged and ignored — pinning is a performance hint, never correctness (R1).
  // Defined in scheduler.cpp behind the platform seam (no OS headers here).
  void pin_and_first_touch(atx::usize worker_id, std::span<std::byte> my_slots) const noexcept;

  // ---- (c) POOL SIZE ------------------------------------------------------
  // Clamp `requested` (0 => topo.n_cores) to the bandwidth knee. Adding cores past
  // the knee buys nothing and can hurt (§1A B6), so the pool never exceeds it. The
  // result is >= 1 (a degenerate knee of 0 still yields a single worker). PURE.
  [[nodiscard]] atx::usize pool_size(atx::usize requested) const noexcept {
    const atx::usize want = (requested == 0) ? topo.n_cores : requested;
    const atx::usize knee = (bandwidth_knee == 0) ? atx::usize{1} : bandwidth_knee;
    const atx::usize capped = (want < knee) ? want : knee;
    return (capped == 0) ? atx::usize{1} : capped; // always >= 1 worker
  }

  // ---- (d) OVERSUBSCRIPTION GUARD ----------------------------------------
  // Assert there is EXACTLY ONE global execution pool in this process. Calling
  // this a second time without an intervening release ATX_ASSERT-fails (a nested
  // pool would multiply threads against the executor — §1A oversubscription, the
  // oneTBB "quadratic threads" failure). Defined in scheduler.cpp (it touches a
  // process-global flag). The guard is a debug precondition (programmer error to
  // create two pools), not an expected-failure Status.
  static void claim_single_pool() noexcept;              // mark the one global pool live
  static void release_single_pool() noexcept;            // release it (pool teardown)
  [[nodiscard]] static bool single_pool_live() noexcept; // is a pool currently claimed?

  // Document + enforce that any nested BLAS/OpenMP pool is pinned to ONE thread:
  // the caller must have set OMP_NUM_THREADS=1 / the BLAS thread count to 1 before
  // spinning the executor. This is advisory (we cannot reach into a third-party
  // runtime), so it returns the recommended nested thread count (always 1) for the
  // caller to apply; it never silently changes global state.
  [[nodiscard]] static constexpr atx::usize nested_pool_threads() noexcept { return 1; }
};

} // namespace atx::engine::parallel
