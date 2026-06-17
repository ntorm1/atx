// atx::engine::parallel — S7.6 (B) ProcessExecutor FAULT-TOLERANCE.
//
// Two safety-critical properties of the multi-process substrate under a worker
// that dies mid-shard, proven on the SAME deterministic `Test` workload + the SAME
// crash-injection seam S7-3 shipped (parallel_process_executor_test.cpp's
// WorkerAbnormalExitSurfacesAsError) — extended here into the two contract proofs
// the sprint's done-gate names:
//
//   (1) DETERMINISTIC ERROR. When a worker process dies mid-shard (here: a shard
//       hits the `crash_at` sentinel and the worker `std::_Exit(7)`s BEFORE writing
//       its ErrorSlot — exactly the silent-corruption path), submit() returns a
//       deterministic Err — NOT a hang, NOT UB, NOT a partial/garbage Ok(). The
//       parent detects the abnormal OS exit (nonzero exit code on Windows / a
//       nonzero-or-signal wait on POSIX), reduces it to the lowest-index offender,
//       and surfaces ErrorCode::Internal. Asserted INVARIANT across worker counts
//       {1,2,4} and STABLE across repeated runs (same code every time).
//
//   (2) BIT-IDENTICAL RE-RUN. A transient worker fault does not perturb
//       determinism: a CLEAN run of the SAME workload AFTER a faulted run produces
//       the bit-identical digest a reference clean run (one that never faulted)
//       produces. digest(clean-after-fault) == digest(reference-clean). The fault
//       leaves no residue (fresh SHM segments per submit, no shared mutable state
//       across address spaces) — so recovery is exact, not approximate.
//
// The crash is injected via the workload INPUT (bytes[16..24] = crash_at), keeping
// the shard body a pure function of (inputs, id) — the determinism contract is
// never weakened to test the fault. The `Test` body (`test_shard`) is registered in
// the worker exe; the ThreadExecutor reference leg registers it in-process via the
// fixture (registration is idempotent).
//
// PLATFORM NOTE (consistent with the sprint's cross-platform residual): the crash
// path here is a worker SELF-EXIT, which is fully Windows-correct AND POSIX-correct
// (the .cpp's wait_all handles both a nonzero exit code and a POSIX signal kill).
// An EXTERNAL controlled kill (TerminateProcess / SIGKILL of a spawned worker by
// the parent) is the same parent-side detection path; it is not exercised here
// because the parent blocks in the spawn/barrier and cannot target a specific
// transient worker deterministically without a test-only hook into the spawn loop.
// The self-exit injection proves the SAME parent-side abnormal-exit contract on
// THIS host (Windows); the POSIX-signal leg of wait_all stays Linux-CI-pending,
// consistent with the sprint's existing residual.

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
using atx::engine::parallel::register_workload;
using atx::engine::parallel::SlotView;
using atx::engine::parallel::slot_view_bytes;
using atx::engine::parallel::test_shard;
using atx::engine::parallel::test_value;
using atx::engine::parallel::ThreadExecutor;
using atx::engine::parallel::WorkloadId;

constexpr usize kSlotSize = sizeof(f64);

// Build a Test input buffer: 8-byte seed, 8-byte fail_at, 8-byte crash_at (both
// fault fields default to kTestNoFail => the unperturbed happy path).
[[nodiscard]] std::vector<std::byte> make_input(u64 seed, u64 fail_at = kTestNoFail,
                                                u64 crash_at = kTestNoFail) {
  std::vector<std::byte> buf(3 * sizeof(u64));
  std::memcpy(buf.data(), &seed, sizeof(seed));
  std::memcpy(buf.data() + sizeof(u64), &fail_at, sizeof(fail_at));
  std::memcpy(buf.data() + 2 * sizeof(u64), &crash_at, sizeof(crash_at));
  return buf;
}

// The sequential reference digest: test_value(i, seed) packed in canonical id order
// (the same raw-f64-bytes recipe the engine's signal_set_digest uses; 1 ULP flips).
[[nodiscard]] u64 sequential_digest(u64 seed, usize n) {
  std::vector<std::byte> packed(n * kSlotSize);
  for (usize i = 0; i < n; ++i) {
    const f64 v = test_value(i, seed);
    std::memcpy(packed.data() + i * kSlotSize, &v, sizeof(v));
  }
  return atx::core::hash_bytes(packed.data(), packed.size());
}

// Digest a gathered SlotView's logical f64 payloads in canonical slot order.
[[nodiscard]] u64 digest_slots(const SlotView &out, usize n) {
  std::vector<std::byte> packed(n * kSlotSize);
  for (usize i = 0; i < n; ++i) {
    std::span<const std::byte> s = out.cslot(i);
    std::memcpy(packed.data() + i * kSlotSize, s.data(), kSlotSize);
  }
  return atx::core::hash_bytes(packed.data(), packed.size());
}

// Run a Test workload on `exec` and return the gathered SlotView (backed by `buf`).
[[nodiscard]] Status run(atx::engine::parallel::IExecutor &exec, std::span<const std::byte> input,
                         usize n, std::vector<std::byte> &buf) {
  buf.assign(slot_view_bytes(n, kSlotSize), std::byte{0});
  SlotView out = make_slot_view(buf, n, kSlotSize);
  return exec.submit(WorkloadId::Test, InputView{input}, n, out);
}

// Fixture: the ThreadExecutor reference legs run test_shard IN THIS process, so
// register it here. The ProcessExecutor legs run it in the worker exe and need no
// in-process registration; per-test (idempotent) registration is harmless.
class ParallelFaultTolerance : public ::testing::Test {
protected:
  void SetUp() override { register_workload(WorkloadId::Test, &test_shard); }
};

} // namespace

// ===========================================================================
//  (B.1) DETERMINISTIC ERROR — a worker that dies mid-shard surfaces a stable Err,
//  never a hang / UB / partial Ok(), invariant across worker counts.
// ===========================================================================
TEST_F(ParallelFaultTolerance, MidShardWorkerDeathSurfacesDeterministicError) {
  constexpr u64 kSeed = 0xFA17ULL;
  constexpr usize kN = 96;
  constexpr u64 kCrashAt = 40ULL; // a worker _Exit(7)s when it claims shard 40
  const std::vector<std::byte> input = make_input(kSeed, kTestNoFail, kCrashAt);

  // Across worker counts: each run must FAIL with the SAME ErrorCode::Internal
  // (the abnormal-exit corruption tripwire — the crashed worker never wrote its
  // ErrorSlot, so this is NOT a shard-level error). Never Ok, never a hang.
  for (const usize w : {usize{1}, usize{2}, usize{4}}) {
    ProcessExecutor pe{ExecutorConfig{w, false}};
    std::vector<std::byte> buf;
    const Status s = run(pe, input, kN, buf);
    ASSERT_FALSE(s.has_value()) << "process@" << w << " returned Ok despite a crashed worker";
    EXPECT_EQ(s.error().code(), ErrorCode::Internal)
        << "process@" << w << " surfaced the wrong error code";
  }
}

// The SAME fault re-run repeatedly on a fixed worker count yields the SAME error
// code every time — the error is a deterministic property of the input, not a
// race-dependent artifact of which worker happened to claim the crashing shard.
TEST_F(ParallelFaultTolerance, MidShardWorkerDeathErrorIsStableAcrossRepeats) {
  constexpr u64 kSeed = 0xBEEFULL;
  constexpr usize kN = 80;
  constexpr u64 kCrashAt = 33ULL;
  const std::vector<std::byte> input = make_input(kSeed, kTestNoFail, kCrashAt);

  ProcessExecutor pe{ExecutorConfig{4, false}};
  for (int rep = 0; rep < 4; ++rep) {
    std::vector<std::byte> buf;
    const Status s = run(pe, input, kN, buf);
    ASSERT_FALSE(s.has_value()) << "rep " << rep << " unexpectedly succeeded";
    EXPECT_EQ(s.error().code(), ErrorCode::Internal) << "rep " << rep << " error code drifted";
  }
}

// A DETERMINISTIC SHARD-LEVEL failure (a worker that reports Err via its ErrorSlot
// instead of crashing) must surface the lowest-id shard error, invariant of worker
// count — the complementary fault mode (a clean Err, not an abnormal exit). This
// pins that the deterministic-error contract holds for BOTH fault shapes.
TEST_F(ParallelFaultTolerance, ForcedShardFailureSurfacesLowestIdDeterministically) {
  constexpr u64 kSeed = 5ULL;
  constexpr usize kN = 64;
  constexpr u64 kFailAt = 17ULL;
  const std::vector<std::byte> input = make_input(kSeed, kFailAt);

  for (const usize w : {usize{1}, usize{2}, usize{4}}) {
    ProcessExecutor pe{ExecutorConfig{w, false}};
    std::vector<std::byte> buf;
    const Status s = run(pe, input, kN, buf);
    ASSERT_FALSE(s.has_value()) << "process@" << w << " unexpectedly succeeded";
    // test_shard returns OutOfRange at the forced id; the parent reduces to it.
    EXPECT_EQ(s.error().code(), ErrorCode::OutOfRange) << "process@" << w;
    EXPECT_NE(s.error().message().find("shard 17"), std::string::npos)
        << "process@" << w << " must name the lowest failing shard; got: " << s.error().message();
  }
}

// ===========================================================================
//  (B.2) BIT-IDENTICAL RE-RUN — a transient worker fault leaves determinism
//  untouched: a CLEAN run after a faulted run reproduces the reference digest.
// ===========================================================================
TEST_F(ParallelFaultTolerance, CleanRunAfterFaultReproducesReferenceDigest) {
  constexpr u64 kSeed = 0xC0FFEE5EEDULL;
  constexpr usize kN = 128;
  const std::vector<std::byte> clean_input = make_input(kSeed); // no fault
  const std::vector<std::byte> fault_input =
      make_input(kSeed, kTestNoFail, /*crash_at*/ 50ULL); // worker dies at shard 50

  // The REFERENCE digest: a clean run on a pristine executor (no prior fault), and
  // it must equal the sequential oracle (so the reference itself is trustworthy).
  const u64 reference = sequential_digest(kSeed, kN);
  {
    ProcessExecutor ref{ExecutorConfig{4, false}};
    std::vector<std::byte> buf;
    const Status s = run(ref, clean_input, kN, buf);
    ASSERT_TRUE(s.has_value()) << "reference clean run failed: " << s.error().to_string();
    SlotView out = make_slot_view(buf, kN, kSlotSize);
    ASSERT_EQ(digest_slots(out, kN), reference) << "reference clean run != sequential oracle";
  }

  // Now: a faulted run, THEN a clean run on the SAME executor. The clean re-run must
  // reproduce the reference digest BIT-FOR-BIT — the fault perturbed nothing.
  ProcessExecutor pe{ExecutorConfig{4, false}};

  // Step 1 — the fault. submit() must surface the deterministic Err (not Ok).
  {
    std::vector<std::byte> buf;
    const Status faulted = run(pe, fault_input, kN, buf);
    ASSERT_FALSE(faulted.has_value()) << "the faulted run must report an error";
    EXPECT_EQ(faulted.error().code(), ErrorCode::Internal);
  }

  // Step 2 — the CLEAN re-run on the same executor. Bit-identical to the reference.
  {
    std::vector<std::byte> buf;
    const Status recovered = run(pe, clean_input, kN, buf);
    ASSERT_TRUE(recovered.has_value())
        << "the clean re-run after a fault must succeed: " << recovered.error().to_string();
    SlotView out = make_slot_view(buf, kN, kSlotSize);
    EXPECT_EQ(digest_slots(out, kN), reference)
        << "BIT-IDENTITY BREAK: a clean run after a fault must reproduce the reference digest";
  }
}

// Repeated fault -> clean cycles on ONE executor: every clean leg reproduces the
// reference (the executor is not left in a degraded state by a fault — recovery is
// idempotent, not a one-shot).
TEST_F(ParallelFaultTolerance, RepeatedFaultThenCleanCyclesAllReproduceReference) {
  constexpr u64 kSeed = 0xD06F00DULL;
  constexpr usize kN = 100;
  const std::vector<std::byte> clean_input = make_input(kSeed);
  const std::vector<std::byte> fault_input = make_input(kSeed, kTestNoFail, /*crash_at*/ 25ULL);
  const u64 reference = sequential_digest(kSeed, kN);

  ProcessExecutor pe{ExecutorConfig{4, false}};
  for (int cycle = 0; cycle < 3; ++cycle) {
    // Fault leg.
    {
      std::vector<std::byte> buf;
      const Status faulted = run(pe, fault_input, kN, buf);
      ASSERT_FALSE(faulted.has_value()) << "cycle " << cycle << " fault leg must error";
      EXPECT_EQ(faulted.error().code(), ErrorCode::Internal) << "cycle " << cycle;
    }
    // Clean leg — must reproduce the reference digest, every cycle.
    {
      std::vector<std::byte> buf;
      const Status clean = run(pe, clean_input, kN, buf);
      ASSERT_TRUE(clean.has_value())
          << "cycle " << cycle << " clean leg failed: " << clean.error().to_string();
      SlotView out = make_slot_view(buf, kN, kSlotSize);
      EXPECT_EQ(digest_slots(out, kN), reference)
          << "cycle " << cycle << " clean digest diverged from the reference";
    }
  }
}
