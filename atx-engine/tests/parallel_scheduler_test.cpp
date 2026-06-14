#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/parallel/builtin_test_workload.hpp"
#include "atx/engine/parallel/executor.hpp"
#include "atx/engine/parallel/scheduler.hpp"
#include "atx/engine/parallel/thread_executor.hpp"

// ---------------------------------------------------------------------------
// Scheduler — deterministic heterogeneous-cost dispatch + NUMA discipline (S7-4).
//
// THE LOAD-BEARING PROOF (§4.4 / §0.7 / R1): the scheduler is a PERFORMANCE layer
// with NO BIT CONTACT. We run the SAME job through a real executor while consuming
// shards in three different dispatch orders — {identity, cost-sorted-descending,
// reversed} — and assert the gathered-result digest is BYTE-IDENTICAL across all
// three. The dispatch order changes which shard runs when, never which slot a
// shard writes, so the reduce (canonical id order) yields ONE digest.
//
// Plus unit coverage: dispatch_order is a valid permutation of [0, n) for
// n in {0, 1, 2, large}; descending-cost ordering with deterministic tie-break;
// pool_size clamps to the bandwidth knee.
// ---------------------------------------------------------------------------

namespace {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::core::Status;
using atx::engine::parallel::ExecutorConfig;
using atx::engine::parallel::InputView;
using atx::engine::parallel::kTestNoFail;
using atx::engine::parallel::make_slot_view;
using atx::engine::parallel::query_topology;
using atx::engine::parallel::register_workload;
using atx::engine::parallel::Scheduler;
using atx::engine::parallel::ShardId;
using atx::engine::parallel::slot_view_bytes;
using atx::engine::parallel::SlotView;
using atx::engine::parallel::test_shard;
using atx::engine::parallel::test_value;
using atx::engine::parallel::ThreadExecutor;
using atx::engine::parallel::Topology;
using atx::engine::parallel::WorkloadId;

constexpr usize kSlotSize = sizeof(f64);
constexpr WorkloadId kTest = WorkloadId::Test;

// Build a Test input buffer: 8-byte seed, an 8-byte fail_at, an 8-byte crash_at
// (both default to kTestNoFail => the unperturbed happy path).
[[nodiscard]] std::vector<std::byte> make_input(u64 seed) {
  std::vector<std::byte> buf(3 * sizeof(u64));
  std::memcpy(buf.data(), &seed, sizeof(seed));
  const u64 no_fail = kTestNoFail;
  std::memcpy(buf.data() + sizeof(u64), &no_fail, sizeof(no_fail));
  std::memcpy(buf.data() + 2 * sizeof(u64), &no_fail, sizeof(no_fail));
  return buf;
}

// Run the Test workload through `exec` consuming shards in `order`, then digest
// the packed (padding-stripped) f64 payload region. The digest is over the slots
// in canonical id order regardless of dispatch order, so it is the bit-for-bit
// fingerprint the no-bit-contact proof compares.
[[nodiscard]] u64 run_and_digest(ThreadExecutor &exec, InputView in, usize n,
                                 std::span<const ShardId> order) {
  std::vector<std::byte> buf(slot_view_bytes(n, kSlotSize));
  const SlotView out = make_slot_view(buf, n, kSlotSize);
  const Status st = exec.submit(kTest, in, n, out, order);
  EXPECT_TRUE(st.has_value()) << "submit failed unexpectedly";

  std::vector<f64> packed(n);
  for (usize i = 0; i < n; ++i) {
    std::memcpy(&packed[i], out.cslot(i).data(), sizeof(f64));
  }
  return atx::core::hash_bytes(packed.data(), packed.size() * sizeof(f64));
}

// True iff `order` is exactly a permutation of [0, n): size n, every id in range,
// no duplicates. The test's own oracle for the permutation-validity asserts.
[[nodiscard]] bool is_permutation_of_range(std::span<const ShardId> order, usize n) {
  if (order.size() != n) {
    return false;
  }
  std::vector<bool> seen(n, false);
  for (const ShardId id : order) {
    if (id >= n || seen[id]) {
      return false;
    }
    seen[id] = true;
  }
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// THE PROOF: digest is byte-identical across {identity, cost-sorted, reversed}.
// ---------------------------------------------------------------------------

TEST(ParallelScheduler, DigestInvariantAcrossDispatchOrders) {
  register_workload(kTest, &test_shard);

  const usize n = 257; // a non-power-of-two to catch ragged-tail bugs
  const u64 seed = 0xA11CE5EED1234567ULL;
  const std::vector<std::byte> input = make_input(seed);
  const InputView in{std::span<const std::byte>{input}};

  // A heterogeneous cost hint: a deterministic non-monotone shape so identity,
  // cost-sorted, and reversed are three genuinely DIFFERENT permutations.
  std::vector<f64> cost(n);
  for (usize i = 0; i < n; ++i) {
    cost[i] = static_cast<f64>((i * 7919U) % 101U); // pseudo-random but pure
  }

  const Scheduler sched{query_topology()};

  // (1) identity order: an empty span => the executor's default identity dispatch.
  // (2) cost-sorted-descending: longest-processing-time-first (the scheduler hint).
  // (3) reversed: the cost-sorted order flipped — a third distinct permutation.
  const std::vector<ShardId> cost_sorted = sched.dispatch_order(cost);
  std::vector<ShardId> reversed = cost_sorted;
  std::reverse(reversed.begin(), reversed.end());

  // Sanity: cost-sorted and reversed are valid permutations and genuinely differ
  // from identity (else the "three orders" claim would be vacuous).
  ASSERT_TRUE(is_permutation_of_range(cost_sorted, n));
  ASSERT_TRUE(is_permutation_of_range(reversed, n));
  bool differs_from_identity = false;
  for (usize p = 0; p < n; ++p) {
    if (cost_sorted[p] != p) {
      differs_from_identity = true;
      break;
    }
  }
  ASSERT_TRUE(differs_from_identity)
      << "cost order must not be the identity (proof would be vacuous)";

  // Run all three orders across multiple worker counts; every digest must match.
  for (const usize w : {usize{1}, usize{2}, usize{4}, usize{8}}) {
    ThreadExecutor exec{ExecutorConfig{w, true}};

    const u64 d_identity = run_and_digest(exec, in, n, {});
    const u64 d_cost = run_and_digest(exec, in, n, cost_sorted);
    const u64 d_rev = run_and_digest(exec, in, n, reversed);

    EXPECT_EQ(d_identity, d_cost) << "cost-sorted order changed the digest at workers=" << w;
    EXPECT_EQ(d_identity, d_rev) << "reversed order changed the digest at workers=" << w;
  }

  // And it must equal the sequential reference (the ultimate oracle): slot i holds
  // test_value(i, seed) regardless of any dispatch order.
  std::vector<f64> ref(n);
  for (usize i = 0; i < n; ++i) {
    ref[i] = test_value(i, seed);
  }
  const u64 ref_digest = atx::core::hash_bytes(ref.data(), ref.size() * sizeof(f64));
  ThreadExecutor exec{ExecutorConfig{4, true}};
  EXPECT_EQ(run_and_digest(exec, in, n, sched.dispatch_order(cost)), ref_digest);
}

// A bad order must be rejected (Err), never silently dispatched (which would
// index a slot out of bounds or skip/duplicate a shard).
TEST(ParallelScheduler, Submit_RejectsNonPermutationOrder) {
  register_workload(kTest, &test_shard);
  ThreadExecutor exec{ExecutorConfig{4, true}};

  const usize n = 8;
  const std::vector<std::byte> input = make_input(1);
  const InputView in{std::span<const std::byte>{input}};
  std::vector<std::byte> buf(slot_view_bytes(n, kSlotSize));
  const SlotView out = make_slot_view(buf, n, kSlotSize);

  // Duplicate id 0, missing id 7 => not a permutation of [0, 8).
  std::vector<ShardId> bad = {0, 0, 1, 2, 3, 4, 5, 6};
  const Status st = exec.submit(kTest, in, n, out, bad);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), atx::core::ErrorCode::InvalidArgument);

  // Wrong-size order (n-1 entries) => also rejected.
  std::vector<ShardId> short_order = {0, 1, 2, 3, 4, 5, 6};
  const Status st2 = exec.submit(kTest, in, n, out, short_order);
  ASSERT_FALSE(st2.has_value());
  EXPECT_EQ(st2.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// ---------------------------------------------------------------------------
// dispatch_order is a valid permutation of [0, n) for n in {0, 1, 2, large}.
// ---------------------------------------------------------------------------

TEST(ParallelScheduler, DispatchOrder_IsPermutation_ForVariousN) {
  const Scheduler sched{query_topology()};

  for (const usize n : {usize{0}, usize{1}, usize{2}, usize{1000}}) {
    std::vector<f64> cost(n);
    for (usize i = 0; i < n; ++i) {
      cost[i] = static_cast<f64>((i * 2654435761U) % 1009U); // arbitrary pure shape
    }
    const std::vector<ShardId> order = sched.dispatch_order(cost);
    EXPECT_EQ(order.size(), n) << "order size must equal n at n=" << n;
    EXPECT_TRUE(is_permutation_of_range(order, n)) << "order not a permutation at n=" << n;
  }
}

TEST(ParallelScheduler, DispatchOrder_EmptyCostIsEmpty) {
  const Scheduler sched{query_topology()};
  const std::vector<ShardId> order = sched.dispatch_order({});
  EXPECT_TRUE(order.empty());
}

TEST(ParallelScheduler, DispatchOrder_UniformCostIsIdentity) {
  // All costs equal => the descending sort is a no-op and the ascending-id
  // tie-break yields exactly the identity permutation (a stable total order).
  const Scheduler sched{query_topology()};
  const usize n = 16;
  std::vector<f64> cost(n, 3.5); // uniform
  const std::vector<ShardId> order = sched.dispatch_order(cost);
  ASSERT_EQ(order.size(), n);
  for (usize i = 0; i < n; ++i) {
    EXPECT_EQ(order[i], i) << "uniform cost must yield identity order at i=" << i;
  }
}

// ---------------------------------------------------------------------------
// Descending-cost ordering with deterministic tie-break.
// ---------------------------------------------------------------------------

TEST(ParallelScheduler, DispatchOrder_DescendingCostLongestFirst) {
  const Scheduler sched{query_topology()};
  // Costs: id0=1, id1=9, id2=5, id3=9, id4=2. Descending by cost: the two 9s
  // (ids 1 and 3) first, tie broken ascending-id => 1 then 3; then 5 (id2), then
  // 2 (id4), then 1 (id0). Expected order: [1, 3, 2, 4, 0].
  const std::vector<f64> cost = {1.0, 9.0, 5.0, 9.0, 2.0};
  const std::vector<ShardId> order = sched.dispatch_order(cost);
  const std::vector<ShardId> expected = {1, 3, 2, 4, 0};
  EXPECT_EQ(order, expected);

  // Costs must be read in strictly non-increasing order along the dispatch.
  for (usize p = 1; p < order.size(); ++p) {
    EXPECT_GE(cost[order[p - 1]], cost[order[p]]) << "dispatch not in descending cost at p=" << p;
  }
}

TEST(ParallelScheduler, DispatchOrder_IsPureFunctionOfCost) {
  // Determinism: the same cost vector yields the bit-identical order every call
  // (no clock, no thread id, no address dependence). Run twice and compare.
  const Scheduler sched{query_topology()};
  const usize n = 200;
  std::vector<f64> cost(n);
  for (usize i = 0; i < n; ++i) {
    cost[i] = static_cast<f64>((i * 40503U) % 251U);
  }
  const std::vector<ShardId> a = sched.dispatch_order(cost);
  const std::vector<ShardId> b = sched.dispatch_order(cost);
  EXPECT_EQ(a, b);
}

// ---------------------------------------------------------------------------
// pool_size clamps to the bandwidth knee.
// ---------------------------------------------------------------------------

TEST(ParallelScheduler, PoolSize_ClampsToKnee) {
  Topology topo{};
  topo.n_cores = 16;
  Scheduler sched{topo};
  sched.bandwidth_knee = 6; // the knee is below the core count (§1A B6: all-cores can be slower)

  EXPECT_EQ(sched.pool_size(0), 6U);   // 0 => n_cores (16), clamped to knee (6)
  EXPECT_EQ(sched.pool_size(4), 4U);   // a request below the knee passes through
  EXPECT_EQ(sched.pool_size(6), 6U);   // exactly the knee
  EXPECT_EQ(sched.pool_size(100), 6U); // a request above the knee is clamped
}

TEST(ParallelScheduler, PoolSize_DefaultKneeIsNCores) {
  Topology topo{};
  topo.n_cores = 8;
  const Scheduler sched{topo}; // bandwidth_knee defaults to topo.n_cores (8)

  EXPECT_EQ(sched.pool_size(0), 8U);  // 0 => n_cores, knee == n_cores => 8
  EXPECT_EQ(sched.pool_size(4), 4U);  // below the knee
  EXPECT_EQ(sched.pool_size(20), 8U); // above the knee => clamped to 8
}

TEST(ParallelScheduler, PoolSize_AlwaysAtLeastOne) {
  Topology topo{};
  topo.n_cores = 0; // degenerate (query failure shape)
  Scheduler sched{topo};
  sched.bandwidth_knee = 0; // pathological knee

  EXPECT_GE(sched.pool_size(0), 1U); // never returns a zero-worker pool
  EXPECT_GE(sched.pool_size(1), 1U);
}

// ---------------------------------------------------------------------------
// Topology query is well-formed (cold path; conservative fallback is valid).
// ---------------------------------------------------------------------------

TEST(ParallelScheduler, QueryTopology_IsWellFormed) {
  const Topology topo = query_topology();
  EXPECT_GE(topo.n_pus, 1U);
  EXPECT_GE(topo.n_cores, 1U);
  EXPECT_GE(topo.n_numa_nodes, 1U);
  EXPECT_EQ(topo.pu_to_node.size(), topo.n_pus);
  // line_bytes must equal the SlotView padding line size (a mismatch would be a
  // false-sharing bug); kCacheLine is the slot-padding constant.
  EXPECT_EQ(topo.line_bytes, atx::engine::parallel::kCacheLine);
  // Every pu_to_node entry is a valid node index.
  for (const usize node : topo.pu_to_node) {
    EXPECT_LT(node, topo.n_numa_nodes);
  }
}

// ---------------------------------------------------------------------------
// Oversubscription guard: exactly one global pool may be claimed at a time.
// ---------------------------------------------------------------------------

TEST(ParallelScheduler, SinglePoolGuard_ClaimAndRelease) {
  // Defensive: ensure no prior test left it claimed.
  Scheduler::release_single_pool();
  EXPECT_FALSE(Scheduler::single_pool_live());

  Scheduler::claim_single_pool();
  EXPECT_TRUE(Scheduler::single_pool_live());

  Scheduler::release_single_pool();
  EXPECT_FALSE(Scheduler::single_pool_live());

  EXPECT_EQ(Scheduler::nested_pool_threads(), 1U); // nested numeric pool pinned to 1
}

#ifndef NDEBUG
TEST(ParallelSchedulerDeathTest, DoubleClaimAborts) {
  Scheduler::release_single_pool(); // start clean
  Scheduler::claim_single_pool();   // one live pool
  // A second claim without release => oversubscription => ATX_ASSERT aborts.
  EXPECT_DEATH({ Scheduler::claim_single_pool(); }, ".*");
  Scheduler::release_single_pool(); // restore for any later test
}

TEST(ParallelSchedulerDeathTest, DispatchOrderNaNCostAborts) {
  const Scheduler sched{query_topology()};
  const f64 nan_cost = std::numeric_limits<f64>::quiet_NaN();
  const std::vector<f64> cost = {1.0, nan_cost, 2.0};
  // A NaN breaks the strict-weak-ordering comparator => ATX_ASSERT precondition
  // aborts (NaN is a programmer-error input; the cost hint is a finite count).
  EXPECT_DEATH({ (void)sched.dispatch_order(cost); }, ".*");
}
#endif
