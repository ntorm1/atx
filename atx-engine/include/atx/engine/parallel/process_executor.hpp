#pragma once

// atx::engine::parallel — ProcessExecutor: the PRIMARY multi-process execution
// substrate over shared memory (S7-3), plus the worker-side loop it spawns.
//
// THE WHOLE POINT (read before touching this file): a given (workload, inputs, n)
// must produce the SAME gathered slot bytes on ProcessExecutor as on the
// in-process ThreadExecutor and the sequential reference (R1/R7/§0.5). The
// substrate, the worker count, and the steal order MUST NOT touch a single result
// bit. ProcessExecutor's determinism story is in fact STRONGER than threads': the
// workers are separate address spaces, so there is NO shared mutable state at all
// — the only shared writable memory is the disjoint, single-writer output slot
// region (R4) plus an integer claim cursor that publishes no result data.
//
// Architecture (mirrors DetPool lifted across the process boundary):
//   * PARENT (`submit`): copy the caller's input bytes into a fresh read-only
//     INPUT ShmSegment (R5), zero an OUTPUT slot segment (R4/R6), fill a CONTROL
//     segment (the shared POD `ControlBlock` + per-worker `ErrorSlot[]`), spawn N
//     `atx-shm-worker` processes, BARRIER on their exit, reduce the GLOBAL
//     lowest-id error across the workers, then copy the output slots back to the
//     caller and (the caller, never a worker) digests in ONE process (§0.3).
//   * WORKER (`run_shm_worker`, defined in the .cpp; `atx-shm-worker` is a thin
//     main): open the three segments, look up the workload's pure ShardFn, drain
//     the cross-process atomic claim cursor (the DetPool fetch_add dispenser
//     lifted into SHM), run each claimed shard into its own pre-indexed slot,
//     record its lowest failed shard in its ErrorSlot, and exit 0/1.
//
// The Win32 / POSIX split lives ENTIRELY in process_executor.cpp (mirroring
// shm_segment.cpp); this header is OS-agnostic. The wire structs are
// trivially-copyable POD with fixed-width fields (ABI-stable across the SAME
// binary — cross-machine is a recorded §0.3 lift), the lone exception being the
// `alignas(64)` claim cursor which is `usize` (same-binary same-machine, so the
// atomic width matches in parent and worker).

#include <atomic>     // std::atomic_ref (cursor alignment static_assert)
#include <cstddef>
#include <functional> // std::function — IExecutor::parallel_for body (rejected here)
#include <span>
#include <type_traits>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/parallel/executor.hpp" // IExecutor, ShardId, WorkloadId, InputView, SlotView, ExecutorConfig

namespace atx::engine::parallel {

// Per-worker error report, written by exactly ONE worker (its own slot). The
// parent reduces the GLOBAL lowest failed shard across all workers post-barrier
// (the deterministic DetPool rethrow-lowest rule, lifted across processes).
struct ErrorSlot {
  atx::u64 has_error;           // 0 == clean, 1 == this worker hit at least one failing shard
  atx::u64 lowest_failed_shard; // the smallest ShardId this worker failed (valid iff has_error)
  atx::u32 code;                // static_cast<u32>(atx::core::ErrorCode) of that failure
  atx::u32 _pad;                // explicit tail pad -> no implicit padding bits in the wire image
};

// Wire control block at offset 0 of the control segment: written by the parent
// BEFORE spawn, read by every worker. Fixed-width fields only (ABI-stable across
// the same binary, §0.3) EXCEPT the cache-line-isolated claim cursor. The
// `ErrorSlot[n_workers]` array follows IMMEDIATELY after `sizeof(ControlBlock)`.
struct ControlBlock {
  atx::u32 magic;     // kControlMagic — a worker sanity-checks this before trusting the block
  atx::u32 n_workers; // number of worker processes (== ErrorSlot count)
  atx::u64 n_shards;  // == submit()'s n (total shards in [0, n))
  atx::u32 workload;  // static_cast<u32>(WorkloadId) — the closed dispatch key (§0.8)
  atx::u32 _pad0;     // keep the following u64 fields 8-byte aligned in the wire image
  atx::u64 input_bytes; // logical INPUT payload size (0 allowed; the segment is sized max(1,this))
  atx::u64 slot_stride; // padded per-slot stride (R6) — matches the caller's SlotView
  atx::u64 slot_size;   // logical POD payload bytes per slot (<= slot_stride)
  char input_name[256]; // NUL-terminated OS name of the input segment
  char output_name[256]; // NUL-terminated OS name of the output segment
  // The cross-process work dispenser, addressed via std::atomic_ref by every
  // worker. ALIGNMENT (the load-bearing invariant): ShmSegment prepends an
  // 8-byte length header, so the ControlBlock is mapped at page+8 — NOT a 16/64B
  // boundary. The cursor is therefore INTENTIONALLY only 8-aligned: that matches
  // the page+8 segment placement and keeps alignof(ControlBlock) == 8, which is
  // exactly what makes `reinterpret_cast<ControlBlock*>(cbuf.data())` onto page+8
  // WELL-DEFINED (an object may not be placed below its declared alignment).
  // Declaring `alignas(>8)` on any member would silently make that cast UB — at
  // Release /O2 /Ob2 the optimizer would assume the higher alignment and vectorize
  // the field stores into a 16B `movaps` to a page+8 (8-mod-16) address → #GP →
  // segfault in every multi-process mine from a vectorizing TU. The static_assert
  // below is the tripwire that forbids re-introducing such an alignas. The
  // atomic_ref is well-defined because std::atomic_ref<usize>::required_alignment
  // is 8 (asserted below), `usize` is 8-aligned, offsetof(claim_cursor) is a
  // multiple of 8, and page+8 is 8-aligned — so &claim_cursor is 8-aligned. (No
  // cache-line isolation is attempted; the cursor shares no segment with result
  // data, so it cannot false-share with a result bit — R6 is unaffected.) `usize`
  // (not a fixed width) is sound because parent and worker are the SAME binary on
  // the SAME machine, so the atomic operand width is identical in both (§0.3).
  atx::usize claim_cursor;
};

// Wire-format invariants: the structs cross a process boundary as raw bytes, so
// they MUST be trivially copyable (no vtable, no non-trivial members) for the
// memcpy/placement to be defined. Same-binary use makes the layout identical in
// parent and worker.
static_assert(std::is_trivially_copyable_v<ErrorSlot>, "ErrorSlot must be a POD wire type");
static_assert(std::is_trivially_copyable_v<ControlBlock>, "ControlBlock must be a POD wire type");
static_assert(std::is_standard_layout_v<ControlBlock>, "ControlBlock must be standard-layout");

// The ControlBlock is mapped at page + ShmSegment's 8-byte length header (page+8),
// so a reinterpret_cast<ControlBlock*> onto it is only well-defined if
// alignof(ControlBlock) <= 8. Declaring any member alignas(>8) silently makes that
// cast UB (Release vectorizes the field stores into a movaps -> #GP). This tripwire
// forbids re-introducing it (it would have FAILED to compile the original, which
// had alignof 64 > 8).
static_assert(alignof(ControlBlock) <= 8,
              "ControlBlock is mapped at page+8; its alignment must be <= 8 — do not alignas a member higher");

// The claim cursor is only 8-aligned within the mapped segment (ControlBlock at
// page+8, see the field comment). atomic_ref over it is UB unless its required
// alignment is satisfied by that 8-alignment — assert the toolchain agrees. If a
// future target needed >8 alignment, the segment header would have to be padded
// to restore 64B isolation; this static_assert is the tripwire for that.
static_assert(std::atomic_ref<atx::usize>::required_alignment <= 8,
              "cursor is only 8-aligned within the shm segment (ControlBlock at page+8)");
static_assert(offsetof(ControlBlock, claim_cursor) % 8 == 0,
              "claim_cursor must be 8-aligned within ControlBlock for the atomic_ref");

// Sanity magic the parent stamps and every worker checks ('ATX7' big-endian).
inline constexpr atx::u32 kControlMagic = 0x41545837U;

// Total control-segment size: the block followed by one ErrorSlot per worker.
[[nodiscard]] inline atx::usize control_segment_bytes(atx::usize n_workers) noexcept {
  return sizeof(ControlBlock) + n_workers * sizeof(ErrorSlot);
}

// View the ErrorSlot array that the parent appended right after the ControlBlock.
// SAFETY: the parent sized the control segment as control_segment_bytes(n_workers)
// and zero-initialised it, so the [sizeof(ControlBlock), +n_workers*sizeof(ErrorSlot))
// range is in-bounds, suitably aligned (ErrorSlot's alignment is 8; sizeof(ControlBlock)
// is a multiple of 8 because its largest/last member is the 8-aligned usize cursor),
// and holds N trivially-copyable ErrorSlot
// objects. The reinterpret_cast from a byte cursor to ErrorSlot* is the standard
// shared-memory typed-view idiom and is required to address the array.
[[nodiscard]] inline ErrorSlot *error_slots(std::span<std::byte> ctl) noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  return reinterpret_cast<ErrorSlot *>(ctl.data() + sizeof(ControlBlock));
}

// Worker-side entry: open the control/input/output segments named on argv, run
// the claimed shards of the registered workload into the output slots, and write
// this worker's ErrorSlot. Returns the PROCESS exit code (0 == clean, 1 == this
// worker failed at least one shard or could not start). Defined in the .cpp so
// `atx-shm-worker`'s main() stays a thin two-liner. argv:
//   argv[1] = control-segment OS name, argv[2] = this worker's id (decimal).
// The CALLER (shm_worker_main) must register the workload(s) before calling this.
[[nodiscard]] int run_shm_worker(int argc, char **argv) noexcept;

// The PRIMARY multi-process IExecutor over shared memory. `final` — the leaf
// substrate for processes. Pinned (the IExecutor base deletes copy/assign; this
// type adds no movable owned state that would need a move ctor — Rule of Zero on
// the resolved worker count). One submit() is self-contained: it creates, uses,
// and RAII-frees its three ShmSegments and all process handles within the call.
class ProcessExecutor final : public IExecutor {
public:
  // workers == 0 => the substrate default (max(1, hardware_concurrency - 2),
  // matching DetPool's bandwidth-knee default). pin_workers is accepted for seam
  // parity; CPU pinning is the scheduler's job (S7-4), so it is a no-op here.
  explicit ProcessExecutor(ExecutorConfig c = {}) noexcept;

  // See IExecutor::submit. Copies inputs into a read-only SHM segment, spawns the
  // workers writing into an output SHM segment, barriers, reduces the global
  // lowest-id error, then copies the output back into `out.bytes`. The seam is
  // IDENTICAL to ThreadExecutor's (heap-backed spans in, heap-backed spans out);
  // the SHM is an internal implementation detail, not a caller concern.
  //
  // S7-4 NOTE: `dispatch_order` is accepted for seam parity but is currently
  // IGNORED here — the cross-process claim cursor already dispenses ids
  // dynamically, and threading a permutation through the control segment is
  // deferred to S7.6 (the digest is invariant to dispatch order regardless, so
  // ignoring the hint costs only the tail-shortening, never a bit). The
  // ThreadExecutor (the in-process oracle) honours the order and PROVES no bit
  // contact for the whole seam.
  [[nodiscard]] atx::core::Status
  submit(WorkloadId workload, InputView inputs, atx::usize n, SlotView out,
         std::span<const ShardId> dispatch_order = {}) override;

  // See IExecutor::parallel_for. UNSUPPORTED on the process substrate: the body is
  // a std::function (a captured closure), which CANNOT cross a process boundary —
  // a worker process links the same TU but cannot receive the parent's closure.
  // ProcessExecutor's contract is the process-portable submit(WorkloadId, ...)
  // path (a closed enum -> pure free function). So this returns Err(NotImplemented)
  // with a message pointing the caller at submit(); the in-process workloads that
  // need a closure-bodied map run on ThreadExecutor (this unit) and gain their
  // serialized multi-process path in a later unit (S7.5c/d). NotImplemented is the
  // closest existing ErrorCode (error.hpp has no Unimplemented).
  [[nodiscard]] atx::core::Status
  parallel_for(atx::usize n, const std::function<void(ShardId, atx::usize)>& body) override;

  [[nodiscard]] atx::usize workers() const noexcept override { return n_workers_; }

  // Multi-process substrate: the closure-bodied parallel_for path is rejected; the
  // serialized submit(WorkloadId, ...) path is the supported transport (S7.5b).
  [[nodiscard]] Substrate substrate() const noexcept override { return Substrate::MultiProcess; }

private:
  atx::usize n_workers_; // resolved worker count (never 0); capped to n per-submit
};

} // namespace atx::engine::parallel
