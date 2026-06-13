#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <new>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/parallel/executor.hpp"
#include "atx/engine/parallel/thread_executor.hpp"

namespace {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::core::ErrorCode;
using atx::core::Status;
using atx::engine::parallel::ExecutorConfig;
using atx::engine::parallel::InputView;
using atx::engine::parallel::lookup_workload;
using atx::engine::parallel::make_slot_view;
using atx::engine::parallel::register_workload;
using atx::engine::parallel::ShardId;
using atx::engine::parallel::SlotView;
using atx::engine::parallel::slot_view_bytes;
using atx::engine::parallel::ThreadExecutor;
using atx::engine::parallel::WorkloadId;

// ---------------------------------------------------------------------------
// Test fixtures: a pure, captureless ShardFn whose result is a deterministic
// function of (id, seed) only — the textbook map body. SplitMix64 on (id ^ seed)
// is a pure integer mix (no RNG state, no wall-clock), so the same (id, seed)
// always yields the same f64 in any process. seed is read from InputView bytes.
// ---------------------------------------------------------------------------

[[nodiscard]] u64 splitmix64(u64 x) noexcept {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// The canonical reference: what shard `id` must produce given `seed`. Used both
// inside the ShardFn and in the sequential oracle, so the two cannot drift.
[[nodiscard]] f64 mix(ShardId id, u64 seed) noexcept {
  const u64 bits = splitmix64(static_cast<u64>(id) ^ seed);
  // Map to a finite f64 in [0,1) deterministically (53-bit mantissa, no FP env).
  return static_cast<f64>(bits >> 11) * (1.0 / 9007199254740992.0); // 2^53
}

// Read the u64 seed from the front of the shared input region.
[[nodiscard]] u64 read_seed(InputView in) noexcept {
  u64 seed = 0;
  std::memcpy(&seed, in.bytes.data(), sizeof(seed));
  return seed;
}

// The registered Test workload: write mix(id, seed) into the shard's slot.
Status test_shard(InputView in, ShardId id, std::span<std::byte> out_slot) {
  const f64 v = mix(id, read_seed(in));
  std::memcpy(out_slot.data(), &v, sizeof(v));
  return atx::core::Ok();
}

// A failing variant for the lowest-id-error test: shards 5 and 9 return distinct
// errors, everyone else writes the normal value. The executor must surface the
// id-5 error (the lowest), invariant across worker counts.
Status failing_shard(InputView in, ShardId id, std::span<std::byte> out_slot) {
  if (id == 5) {
    return atx::core::Err(ErrorCode::OutOfRange, "shard 5 failed");
  }
  if (id == 9) {
    return atx::core::Err(ErrorCode::Internal, "shard 9 failed");
  }
  const f64 v = mix(id, read_seed(in));
  std::memcpy(out_slot.data(), &v, sizeof(v));
  return atx::core::Ok();
}

// Distinct WorkloadIds for tests that must not collide on the shared registry.
// Only WorkloadId::Test exists in the closed enum (S7-1 ships only Test); we
// reuse it across tests, registering whichever fn the test needs. Tests that
// need the failing fn re-register Test to failing_shard and restore after.
constexpr WorkloadId kTest = WorkloadId::Test;

// An id that is in-range of the table but never registered, to prove lookup and
// submit both reject it. static_cast is to the enum's underlying type domain.
constexpr WorkloadId kUnregistered = static_cast<WorkloadId>(7);

// Build a SlotView-backed buffer for n shards of one f64, run the workload, and
// return the wyhash digest of the whole (packed, slot_size-only) payload region.
// Packing into a contiguous f64 vector strips the cache-line padding so the
// digest is purely the logical results — identical across stride choices.
[[nodiscard]] u64 run_and_digest(ThreadExecutor& exec, WorkloadId wl, InputView in, usize n) {
  std::vector<std::byte> buf(slot_view_bytes(n, sizeof(f64)));
  const SlotView out = make_slot_view(buf, n, sizeof(f64));
  const Status st = exec.submit(wl, in, n, out);
  EXPECT_TRUE(st.has_value()) << "submit failed unexpectedly";

  std::vector<f64> packed(n);
  for (usize i = 0; i < n; ++i) {
    std::memcpy(&packed[i], out.cslot(i).data(), sizeof(f64));
  }
  return atx::core::hash_bytes(packed.data(), packed.size() * sizeof(f64));
}

} // namespace

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

TEST(ParallelExecutor, Registry_ReturnsRegisteredFn) {
  register_workload(kTest, &test_shard);
  EXPECT_EQ(lookup_workload(kTest), &test_shard);
}

TEST(ParallelExecutor, Registry_UnregisteredReturnsNullptr) {
  EXPECT_EQ(lookup_workload(kUnregistered), nullptr);
}

TEST(ParallelExecutor, Submit_UnregisteredWorkloadReturnsNotFound) {
  ThreadExecutor exec{};
  std::array<std::byte, sizeof(u64)> seed_bytes{};
  const InputView in{std::span<const std::byte>{seed_bytes}};

  std::vector<std::byte> buf(slot_view_bytes(4, sizeof(f64)));
  const SlotView out = make_slot_view(buf, 4, sizeof(f64));
  const Status st = exec.submit(kUnregistered, in, 4, out);

  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::NotFound);
}

// ---------------------------------------------------------------------------
// SlotView geometry
// ---------------------------------------------------------------------------

TEST(ParallelExecutor, SlotView_StridePaddedToCacheLine) {
  // A small slot_size (8 bytes) must round its stride up to the cache line so
  // adjacent slots never share a line (R6).
  const usize n = 5;
  std::vector<std::byte> buf(slot_view_bytes(n, sizeof(f64)));
  const SlotView v = make_slot_view(buf, n, sizeof(f64));

  EXPECT_EQ(v.slot_size, sizeof(f64));
  EXPECT_GE(v.slot_stride, sizeof(f64));
  EXPECT_EQ(v.slot_stride % std::hardware_destructive_interference_size, 0U);
  EXPECT_EQ(v.n(), n);
  EXPECT_EQ(slot_view_bytes(n, sizeof(f64)), n * v.slot_stride);
}

TEST(ParallelExecutor, SlotView_SlotsDisjointInBoundsAndSized) {
  const usize n = 8;
  std::vector<std::byte> buf(slot_view_bytes(n, sizeof(f64)));
  const SlotView v = make_slot_view(buf, n, sizeof(f64));

  const std::byte* base = buf.data();
  for (usize i = 0; i < n; ++i) {
    const std::span<std::byte> s = v.slot(i);
    EXPECT_EQ(s.size(), v.slot_size);
    // In-bounds: the slot lies wholly inside the buffer.
    const usize off = static_cast<usize>(s.data() - base);
    EXPECT_LE(off + s.size(), buf.size());
    // Disjoint & ordered: each slot starts exactly one stride past the previous.
    EXPECT_EQ(off, i * v.slot_stride);
  }
}

// ---------------------------------------------------------------------------
// Happy path + determinism — the load-bearing proof (R1 / R7).
// ---------------------------------------------------------------------------

TEST(ParallelExecutor, Submit_ByteIdenticalAcrossWorkerCounts) {
  register_workload(kTest, &test_shard);

  const usize n = 256;
  const u64 seed = 0xC0FFEE123456789ULL;
  std::array<std::byte, sizeof(u64)> seed_bytes{};
  std::memcpy(seed_bytes.data(), &seed, sizeof(seed));
  const InputView in{std::span<const std::byte>{seed_bytes}};

  // Sequential reference: the same mix in a plain loop, packed identically.
  std::vector<f64> ref(n);
  for (usize i = 0; i < n; ++i) {
    ref[i] = mix(i, seed);
  }
  const u64 ref_digest = atx::core::hash_bytes(ref.data(), ref.size() * sizeof(f64));

  // Every worker count must reproduce the sequential reference bit-for-bit.
  for (const usize w : {usize{1}, usize{2}, usize{4}, usize{8}}) {
    ThreadExecutor exec{ExecutorConfig{w, true}};
    const u64 d = run_and_digest(exec, kTest, in, n);
    EXPECT_EQ(d, ref_digest) << "digest diverged at workers=" << w;
  }
}

// ---------------------------------------------------------------------------
// Boundaries
// ---------------------------------------------------------------------------

TEST(ParallelExecutor, Submit_ZeroShardsIsOkNoDispatch) {
  register_workload(kTest, &test_shard);
  ThreadExecutor exec{ExecutorConfig{4, true}};

  std::array<std::byte, sizeof(u64)> seed_bytes{};
  const InputView in{std::span<const std::byte>{seed_bytes}};

  // n == 0: empty SlotView, expect Ok() and no work.
  std::vector<std::byte> buf(slot_view_bytes(0, sizeof(f64)));
  const SlotView out = make_slot_view(buf, 0, sizeof(f64));
  EXPECT_EQ(out.n(), 0U);
  const Status st = exec.submit(kTest, in, 0, out);
  EXPECT_TRUE(st.has_value());
}

TEST(ParallelExecutor, Submit_SlotCountMismatchReturnsInvalidArgument) {
  register_workload(kTest, &test_shard);
  ThreadExecutor exec{};

  std::array<std::byte, sizeof(u64)> seed_bytes{};
  const InputView in{std::span<const std::byte>{seed_bytes}};

  // Build a view for 4 slots but claim n == 5: out.n() (4) != n (5) => Err.
  std::vector<std::byte> buf(slot_view_bytes(4, sizeof(f64)));
  const SlotView out = make_slot_view(buf, 4, sizeof(f64));
  const Status st = exec.submit(kTest, in, 5, out);

  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::InvalidArgument);
}

TEST(ParallelExecutor, Submit_SingleShardMatchesReference) {
  register_workload(kTest, &test_shard);
  ThreadExecutor exec{ExecutorConfig{4, true}};

  const u64 seed = 42;
  std::array<std::byte, sizeof(u64)> seed_bytes{};
  std::memcpy(seed_bytes.data(), &seed, sizeof(seed));
  const InputView in{std::span<const std::byte>{seed_bytes}};

  std::vector<std::byte> buf(slot_view_bytes(1, sizeof(f64)));
  const SlotView out = make_slot_view(buf, 1, sizeof(f64));
  ASSERT_TRUE(exec.submit(kTest, in, 1, out).has_value());

  f64 got = 0;
  std::memcpy(&got, out.cslot(0).data(), sizeof(got));
  EXPECT_EQ(got, mix(0, seed));
}

// ---------------------------------------------------------------------------
// Lowest-id error determinism.
// ---------------------------------------------------------------------------

TEST(ParallelExecutor, Submit_ReturnsLowestIdErrorAcrossWorkerCounts) {
  // Register the failing variant (shards 5 and 9 fail). The executor must always
  // surface the id-5 error (OutOfRange), never id-9 (Internal), at every w.
  register_workload(kTest, &failing_shard);

  const usize n = 16;
  std::array<std::byte, sizeof(u64)> seed_bytes{};
  const InputView in{std::span<const std::byte>{seed_bytes}};

  for (const usize w : {usize{1}, usize{2}, usize{4}, usize{8}}) {
    ThreadExecutor exec{ExecutorConfig{w, true}};
    std::vector<std::byte> buf(slot_view_bytes(n, sizeof(f64)));
    const SlotView out = make_slot_view(buf, n, sizeof(f64));
    const Status st = exec.submit(kTest, in, n, out);

    ASSERT_FALSE(st.has_value()) << "expected failure at workers=" << w;
    EXPECT_EQ(st.error().code(), ErrorCode::OutOfRange) << "wrong (non-lowest) error at workers=" << w;
  }

  // Restore the well-behaved fn so registry order between tests cannot leak.
  register_workload(kTest, &test_shard);
}

TEST(ParallelExecutor, Workers_ReportsResolvedCount) {
  ThreadExecutor exec{ExecutorConfig{4, true}};
  EXPECT_EQ(exec.workers(), 4U);

  ThreadExecutor def{}; // 0 => substrate default, always >= 1
  EXPECT_GE(def.workers(), 1U);
}
