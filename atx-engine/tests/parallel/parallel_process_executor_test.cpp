#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/parallel/builtin_test_workload.hpp"
#include "atx/engine/parallel/executor.hpp"
#include "atx/engine/parallel/process_executor.hpp"
#include "atx/engine/parallel/thread_executor.hpp"

// ---------------------------------------------------------------------------
// ParallelProcessExecutor — the multi-process executor's identity test (S7-3).
//
// THE WHOLE POINT (R1/R7/§0.5): for a fixed (workload=Test, seed, n), the
// gathered output bytes are BYTE-IDENTICAL across
//   { sequential reference, ThreadExecutor@{1,4}, ProcessExecutor@{1,4} }
// — one digest across all six paths. The substrate and worker count never touch
// a result bit. The `Test` workload body (`test_shard`) is registered IN THE
// WORKER EXE; the ProcessExecutor does not run it in-process, so the test process
// only needs it registered for the ThreadExecutor leg (done in the fixture).
// ---------------------------------------------------------------------------

namespace {

using atx::f64;
using atx::u64;
using atx::usize;
using atx::core::ErrorCode;
using atx::core::Status;
using atx::engine::parallel::ExecutorConfig;
using atx::engine::parallel::InputView;
using atx::engine::parallel::kTestNoFail;
using atx::engine::parallel::make_slot_view;
using atx::engine::parallel::ProcessExecutor;
using atx::engine::parallel::ShardId;
using atx::engine::parallel::SlotView;
using atx::engine::parallel::slot_view_bytes;
using atx::engine::parallel::test_shard;
using atx::engine::parallel::test_value;
using atx::engine::parallel::ThreadExecutor;
using atx::engine::parallel::WorkloadId;

constexpr usize kSlotSize = sizeof(f64);

// Build a Test input buffer: 8-byte seed, an 8-byte fail_at, and an 8-byte
// crash_at (both default to kTestNoFail => the unperturbed happy path).
[[nodiscard]] std::vector<std::byte> make_input(u64 seed, u64 fail_at = kTestNoFail,
                                                u64 crash_at = kTestNoFail) {
  std::vector<std::byte> buf(3 * sizeof(u64));
  std::memcpy(buf.data(), &seed, sizeof(seed));
  std::memcpy(buf.data() + sizeof(u64), &fail_at, sizeof(fail_at));
  std::memcpy(buf.data() + 2 * sizeof(u64), &crash_at, sizeof(crash_at));
  return buf;
}

// Pack the logical f64 result of each slot into a contiguous little buffer and
// digest it — the same "raw f64 bytes, canonical order" recipe the engine's
// signal_set_digest uses; 1 ULP flips it. Reads the LOGICAL slot payload only
// (strips the cache-line padding), so the digest is independent of slot_stride.
[[nodiscard]] u64 digest_slots(const SlotView &out, usize n) {
  std::vector<std::byte> packed(n * kSlotSize);
  for (usize i = 0; i < n; ++i) {
    std::span<const std::byte> s = out.cslot(i);
    std::memcpy(packed.data() + i * kSlotSize, s.data(), kSlotSize);
  }
  return atx::core::hash_bytes(packed.data(), packed.size());
}

// The sequential reference: test_value(i, seed) packed in canonical id order.
[[nodiscard]] u64 sequential_digest(u64 seed, usize n) {
  std::vector<std::byte> packed(n * kSlotSize);
  for (usize i = 0; i < n; ++i) {
    const f64 v = test_value(i, seed);
    std::memcpy(packed.data() + i * kSlotSize, &v, sizeof(v));
  }
  return atx::core::hash_bytes(packed.data(), packed.size());
}

// Fixture: the ThreadExecutor leg runs test_shard IN THIS process, so register it
// here (the ProcessExecutor leg runs it in the worker exe and needs no in-process
// registration). Registration is idempotent; doing it per-test is harmless.
class ParallelProcessExecutor : public ::testing::Test {
protected:
  void SetUp() override {
    atx::engine::parallel::register_workload(WorkloadId::Test, &test_shard);
  }
};

// Run a workload on `exec` and return the gathered SlotView (backed by `buf`).
[[nodiscard]] Status run(atx::engine::parallel::IExecutor &exec, std::span<const std::byte> input,
                         usize n, std::vector<std::byte> &buf) {
  buf.assign(slot_view_bytes(n, kSlotSize), std::byte{0});
  SlotView out = make_slot_view(buf, n, kSlotSize);
  return exec.submit(WorkloadId::Test, InputView{input}, n, out);
}

} // namespace

// --- Under-alignment tripwire (companion to the static_assert in the header) ---
//
// The shm ControlBlock is mapped at page + ShmSegment's 8-byte length header
// (page+8), so reinterpret_cast<ControlBlock*> onto it is only well-defined when
// alignof(ControlBlock) <= 8. A member alignas(>8) would make alignof 16/64,
// silently turning that cast into UB (Release vectorizes the field stores into a
// movaps -> #GP, segfaulting every multi-process mine from a vectorizing TU). The
// header's static_assert is the build-time guard; this is its human-readable
// runtime companion.
TEST(ParallelProcessExecutorLayout, ControlBlockIsAtMostEightAligned) {
  EXPECT_LE(alignof(atx::engine::parallel::ControlBlock), std::size_t{8})
      << "ControlBlock is mapped at page+8; alignof must be <= 8 — do not alignas a member higher";
}

// --- The boundary pin: digest invariance across all six paths (R1/R7/§0.5) ---

TEST_F(ParallelProcessExecutor, DigestIsInvariantAcrossSubstrateAndWorkerCount) {
  constexpr u64 kSeed = 0xC0FFEE1234567890ULL;
  constexpr usize kN = 256;
  const std::vector<std::byte> input = make_input(kSeed);

  const u64 want = sequential_digest(kSeed, kN);

  // ThreadExecutor @ 1 and @ 4.
  for (usize w : {usize{1}, usize{4}}) {
    ThreadExecutor te{ExecutorConfig{w, false}};
    std::vector<std::byte> buf;
    const Status s = run(te, input, kN, buf);
    ASSERT_TRUE(s.has_value()) << "thread@" << w << ": " << s.error().to_string();
    SlotView out = make_slot_view(buf, kN, kSlotSize);
    EXPECT_EQ(digest_slots(out, kN), want) << "ThreadExecutor@" << w << " digest diverged";
  }

  // ProcessExecutor @ 1 and @ 4 — the cross-process legs (the whole point).
  for (usize w : {usize{1}, usize{4}}) {
    ProcessExecutor pe{ExecutorConfig{w, false}};
    std::vector<std::byte> buf;
    const Status s = run(pe, input, kN, buf);
    ASSERT_TRUE(s.has_value()) << "process@" << w << ": " << s.error().to_string();
    SlotView out = make_slot_view(buf, kN, kSlotSize);
    EXPECT_EQ(digest_slots(out, kN), want) << "ProcessExecutor@" << w << " digest diverged";
  }
}

// --- Pure-shard / disjoint slots: every slot == test_value(i, seed) (R3/R4) ---

TEST_F(ParallelProcessExecutor, ProcessResultTableMatchesPureKernelPerSlot) {
  constexpr u64 kSeed = 42ULL;
  constexpr usize kN = 100;
  const std::vector<std::byte> input = make_input(kSeed);

  ProcessExecutor pe{ExecutorConfig{4, false}};
  std::vector<std::byte> buf;
  const Status s = run(pe, input, kN, buf);
  ASSERT_TRUE(s.has_value()) << s.error().to_string();
  SlotView out = make_slot_view(buf, kN, kSlotSize);

  for (usize i = 0; i < kN; ++i) {
    f64 got = 0;
    std::memcpy(&got, out.cslot(i).data(), sizeof(got));
    EXPECT_EQ(got, test_value(i, kSeed)) << "slot " << i << " mismatch";
  }
}

// --- Boundaries -------------------------------------------------------------

TEST_F(ParallelProcessExecutor, EmptyShardCountIsOkAndSpawnsNothing) {
  ProcessExecutor pe{ExecutorConfig{4, false}};
  std::vector<std::byte> buf;
  // n == 0: a no-op (no worker process spawned). The SlotView is empty.
  SlotView out = make_slot_view(buf, 0, kSlotSize);
  const std::vector<std::byte> input = make_input(1);
  const Status s = pe.submit(WorkloadId::Test, InputView{input}, 0, out);
  EXPECT_TRUE(s.has_value()) << s.error().to_string();
}

TEST_F(ParallelProcessExecutor, SlotCountMismatchIsInvalidArgument) {
  ProcessExecutor pe{ExecutorConfig{2, false}};
  // Buffer sized for 8 slots, but we claim n == 4 => out.n() (8) != 4.
  std::vector<std::byte> buf(slot_view_bytes(8, kSlotSize), std::byte{0});
  SlotView out = make_slot_view(buf, 8, kSlotSize);
  const std::vector<std::byte> input = make_input(1);
  const Status s = pe.submit(WorkloadId::Test, InputView{input}, 4, out);
  ASSERT_FALSE(s.has_value());
  EXPECT_EQ(s.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(ParallelProcessExecutor, SingleShardRuns) {
  constexpr u64 kSeed = 7ULL;
  const std::vector<std::byte> input = make_input(kSeed);
  ProcessExecutor pe{ExecutorConfig{4, false}};
  std::vector<std::byte> buf;
  const Status s = run(pe, input, 1, buf);
  ASSERT_TRUE(s.has_value()) << s.error().to_string();
  SlotView out = make_slot_view(buf, 1, kSlotSize);
  f64 got = 0;
  std::memcpy(&got, out.cslot(0).data(), sizeof(got));
  EXPECT_EQ(got, test_value(0, kSeed));
}

TEST_F(ParallelProcessExecutor, MoreWorkersThanShardsDoesNotHang) {
  // n_workers (8) > n (3): the capped/idle workers find the cursor already
  // drained and exit clean; the run must still complete and be correct.
  constexpr u64 kSeed = 99ULL;
  constexpr usize kN = 3;
  const std::vector<std::byte> input = make_input(kSeed);
  ProcessExecutor pe{ExecutorConfig{8, false}};
  std::vector<std::byte> buf;
  const Status s = run(pe, input, kN, buf);
  ASSERT_TRUE(s.has_value()) << s.error().to_string();
  SlotView out = make_slot_view(buf, kN, kSlotSize);
  EXPECT_EQ(digest_slots(out, kN), sequential_digest(kSeed, kN));
}

// --- Lowest-id error determinism across the process boundary (R3/§0.1) ------

TEST_F(ParallelProcessExecutor, ForcedFailureSurfacesAsError) {
  constexpr u64 kSeed = 5ULL;
  constexpr usize kN = 64;
  constexpr u64 kFailAt = 17ULL;
  const std::vector<std::byte> input = make_input(kSeed, kFailAt);

  ProcessExecutor pe{ExecutorConfig{4, false}};
  std::vector<std::byte> buf;
  const Status s = run(pe, input, kN, buf);
  ASSERT_FALSE(s.has_value());
  // test_shard returns OutOfRange at the forced id; the parent surfaces it.
  EXPECT_EQ(s.error().code(), ErrorCode::OutOfRange);
}

TEST_F(ParallelProcessExecutor, ForcedFailureIsTheLowestIdInvariantOfWorkerCount) {
  // With fail_at=10, every worker count must surface the SAME OutOfRange error,
  // and the message must name the lowest (only) failing id — deterministic
  // across the process boundary regardless of which worker claimed shard 10.
  constexpr u64 kSeed = 3ULL;
  constexpr usize kN = 128;
  constexpr u64 kFailAt = 10ULL;
  const std::vector<std::byte> input = make_input(kSeed, kFailAt);

  for (usize w : {usize{1}, usize{2}, usize{8}}) {
    ProcessExecutor pe{ExecutorConfig{w, false}};
    std::vector<std::byte> buf;
    const Status s = run(pe, input, kN, buf);
    ASSERT_FALSE(s.has_value()) << "process@" << w << " unexpectedly succeeded";
    EXPECT_EQ(s.error().code(), ErrorCode::OutOfRange) << "process@" << w;
    EXPECT_NE(s.error().message().find("shard 10"), std::string::npos)
        << "process@" << w << " message: " << s.error().message();
  }
}

// --- Silent-corruption guard: an abnormal worker exit must surface as error ---

TEST_F(ParallelProcessExecutor, WorkerAbnormalExitSurfacesAsError) {
  // crash_at makes a worker _Exit(nonzero) mid-shard WITHOUT writing its
  // ErrorSlot — the exact path that, unguarded, would gather a half-written
  // output and return Ok(). The parent must catch the abnormal OS exit and FAIL,
  // invariant across worker counts. (No ErrorSlot is set, so the surfaced error
  // is the Internal "exited abnormally" tripwire, not a shard-level error.)
  constexpr u64 kSeed = 11ULL;
  constexpr usize kN = 96;
  constexpr u64 kCrashAt = 40ULL;
  const std::vector<std::byte> input = make_input(kSeed, kTestNoFail, kCrashAt);

  for (usize w : {usize{1}, usize{2}, usize{4}}) {
    ProcessExecutor pe{ExecutorConfig{w, false}};
    std::vector<std::byte> buf;
    const Status s = run(pe, input, kN, buf);
    ASSERT_FALSE(s.has_value()) << "process@" << w << " returned Ok despite a crashed worker";
    EXPECT_EQ(s.error().code(), ErrorCode::Internal) << "process@" << w;
  }
}

// --- Death/spawn robustness: a missing worker exe => Err(NotFound), no hang ---

TEST_F(ParallelProcessExecutor, MissingWorkerExeReturnsNotFound) {
  // Point the override at a path that does not exist; submit must fail cleanly
  // (NotFound), never hang or crash. Restore the env afterwards.
#if defined(_WIN32)
  ::_putenv_s("ATX_SHM_WORKER", "C:\\atx-nonexistent-worker-xyz.exe");
#else
  ::setenv("ATX_SHM_WORKER", "/atx-nonexistent-worker-xyz", 1);
#endif
  ProcessExecutor pe{ExecutorConfig{2, false}};
  const std::vector<std::byte> input = make_input(1);
  std::vector<std::byte> buf;
  const Status s = run(pe, input, 8, buf);
#if defined(_WIN32)
  ::_putenv_s("ATX_SHM_WORKER", "");
#else
  ::unsetenv("ATX_SHM_WORKER");
#endif
  ASSERT_FALSE(s.has_value());
  EXPECT_EQ(s.error().code(), ErrorCode::NotFound);
}
