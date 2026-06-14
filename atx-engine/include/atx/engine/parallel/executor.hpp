#pragma once

// atx::engine::parallel — IExecutor: the substrate-agnostic execution seam (S7-1).
//
// THE WHOLE POINT (read before touching this file): the seam currency must
// survive crossing a PROCESS boundary in a later unit (S7.3 spawns worker
// *processes*). A captured std::function / lambda CANNOT cross processes. So:
//
//   * the work BODY is identified by a closed `WorkloadId` enum, mapped through a
//     process-portable registry to a PURE FREE FUNCTION `ShardFn` (a plain
//     function pointer, never a capturing lambda — it must be callable in any
//     process that links the same workload TU);
//   * the INPUT is one read-only byte region shared by all shards (`InputView`),
//     a span over an in-process buffer now, a PROT_READ shared-memory view later
//     — same type, different backing, one physical copy with N readers (R5);
//   * the OUTPUT is a pre-indexed POD slot region (`SlotView`): n fixed-size
//     slots, slot s written ONLY by shard s (R4). The byte layout is identical
//     in-process and in shared memory, and each slot's stride is padded to the
//     cache line so adjacent shards never false-share (R6).
//
// Threads (this unit's ThreadExecutor), processes (S7.3), and future nodes all
// implement `IExecutor` — nothing ABOVE the seam changes when the substrate does
// (R2). `submit` is a pure map: shard s reads only (inputs, s) and writes only
// out.slot(s); which worker runs which shard MUST NOT touch a result bit (R1),
// so the gathered output is byte-identical across worker counts BY CONSTRUCTION.
//
// Header-only: the slot helpers and the registry are `inline`; there is no .cpp
// in this unit (the OS-touching .cpp's arrive in S7.2 / S7.3).

#include <cstddef>
#include <functional> // std::function — in-process parallel_for body (cold dispatch)
#include <new>        // std::hardware_destructive_interference_size
#include <span>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp" // ATX_ASSERT — debug-only precondition guards
#include "atx/core/types.hpp"

namespace atx::engine::parallel {

// Canonical dense slot key in [0, n). One shard == one slot == one result row.
using ShardId = atx::usize;

// Closed, process-portable set of workloads. S7-1 ships ONLY `Test`; later units
// (S7.5) append Eval / Cpcv / Backtests / Mine. Fixed underlying type so the enum
// has the SAME byte width in every process that sends/receives a WorkloadId.
enum class WorkloadId : atx::u32 { Test = 0 };

// Cache-line stride used to pad slots so adjacent shards never share a line (R6).
//
// SAFETY: std::hardware_destructive_interference_size is an ABI-stability concern
// on some toolchains (the value is baked into headers at compile time); under
// clang-cl with the MSVC STL it is a clean constexpr 64 and the sprint's own
// hot-path code uses it directly, so we use it here too. If a future toolchain's
// /WX flags it (-Winterference-size), replace this line with
//   inline constexpr atx::usize kCacheLine = 64;
// — the geometry is otherwise unchanged.
inline constexpr atx::usize kCacheLine = std::hardware_destructive_interference_size;

// Read-only input region shared by ALL shards. In S7-1 it spans an in-process
// buffer; in S7.2+ it spans a PROT_READ shared-memory view — same type, different
// backing. One physical copy, N concurrent readers (R5).
struct InputView {
  std::span<const std::byte> bytes;
};

// Round `slot_size` up to a whole number of cache lines (>= one line so even a
// zero/tiny slot still occupies its own line and cannot false-share with a
// neighbour). Pure integer arithmetic; no overflow for any realistic slot_size.
[[nodiscard]] inline constexpr atx::usize padded_stride(atx::usize slot_size) noexcept {
  const atx::usize lines = (slot_size + kCacheLine - 1) / kCacheLine;
  return (lines == 0 ? atx::usize{1} : lines) * kCacheLine;
}

// Pre-indexed POD output region: n fixed-size slots, slot s written ONLY by shard
// s (R4). The byte layout is identical in-process and in shared memory. The
// per-slot STRIDE is padded to the cache line to kill false sharing (R6); the
// logical POD payload each shard writes/reads is `slot_size` (<= slot_stride).
struct SlotView {
  std::span<std::byte> bytes; // size == n * slot_stride
  atx::usize slot_stride{};   // padded per-slot size (>= slot_size)
  atx::usize slot_size{};     // logical POD payload size per slot

  // Mutable payload of shard s: the first `slot_size` bytes of its padded stride.
  // SAFETY: precondition s < n() and a well-formed view (built by make_slot_view);
  // the subspan is in-bounds because bytes.size() == n() * slot_stride and
  // slot_size <= slot_stride, so s*slot_stride + slot_size <= bytes.size().
  [[nodiscard]] std::span<std::byte> slot(ShardId s) const noexcept {
    ATX_ASSERT(s < n());                 // out-of-range s would form an OOB subspan (UB)
    ATX_ASSERT(slot_size <= slot_stride); // payload must fit within its padded stride
    return bytes.subspan(s * slot_stride, slot_size);
  }

  // Const payload of shard s, for the gather / reduce after submit() returns.
  [[nodiscard]] std::span<const std::byte> cslot(ShardId s) const noexcept {
    ATX_ASSERT(s < n());                 // out-of-range s would form an OOB subspan (UB)
    ATX_ASSERT(slot_size <= slot_stride); // payload must fit within its padded stride
    return std::span<const std::byte>{bytes}.subspan(s * slot_stride, slot_size);
  }

  // Number of slots this view describes. Zero stride => zero slots (empty view).
  [[nodiscard]] atx::usize n() const noexcept {
    return slot_stride == 0 ? atx::usize{0} : bytes.size() / slot_stride;
  }
};

// Total bytes a SlotView over n shards of `slot_size` POD bytes needs: n slots
// each padded to the cache-line stride (R6). Caller sizes its buffer with this.
[[nodiscard]] inline atx::usize slot_view_bytes(atx::usize n, atx::usize slot_size) noexcept {
  const atx::usize stride = padded_stride(slot_size); // always >= kCacheLine, so > 0
  // SAFETY: the n * stride product is unchecked size_t arithmetic; guard against a
  // silent wrap. stride > 0 (padded to >= one cache line) makes the division safe.
  ATX_ASSERT(n <= (static_cast<atx::usize>(-1) / stride));
  return n * stride;
}

// Build a SlotView over `buffer` for n shards of `slot_size` POD bytes, with the
// stride padded to the cache line (R6). The caller OWNS `buffer` and must size it
// with slot_view_bytes(n, slot_size); we span exactly n * stride of it so a
// larger buffer is fine (trailing bytes are simply not part of the view).
[[nodiscard]] inline SlotView make_slot_view(std::span<std::byte> buffer, atx::usize n,
                                             atx::usize slot_size) noexcept {
  const atx::usize stride = padded_stride(slot_size); // always >= kCacheLine, so > 0
  // SAFETY: this is a noexcept path, so it cannot return Err — the preconditions are
  // contract-enforced via assert. Guard the n * stride product against a silent
  // size_t wrap, then require the buffer to actually hold n full strides (a too-small
  // buffer would make the subspan below out-of-bounds, i.e. UB in release).
  ATX_ASSERT(n <= (static_cast<atx::usize>(-1) / stride));
  ATX_ASSERT(buffer.size() >= n * stride);
  return SlotView{buffer.subspan(0, n * stride), stride, slot_size};
}

// Tunables that pick the SUBSTRATE shape but never a result bit. workers == 0
// asks the substrate for its own default (e.g. DetPool's hw-derived count).
struct ExecutorConfig {
  atx::usize workers = 0;
  bool pin_workers = true;
};

// The process-portable PURE shard function: writes shard `id`'s POD result into
// `out_slot` from (inputs, id) and returns Ok(), or returns an Err describing an
// EXPECTED failure (no throw). MUST be a plain function pointer (NO capturing
// lambda) so it is callable in any process. Pure (R3): reads only (inputs, id),
// writes only out_slot; no wall-clock, no unseeded RNG, no shared mutable state.
using ShardFn = atx::core::Status (*)(InputView inputs, ShardId id, std::span<std::byte> out_slot);

namespace detail {

// Function-local-static registry table: WorkloadId -> ShardFn, indexed densely by
// the enum's underlying value. Sized to the largest WorkloadId S7 will ship; a
// fixed array (not a map) keeps lookup branchless and the table itself POD so the
// process path can reason about it. Magic-static init makes first-use thread-safe.
//
// SAFETY: the table is one shared mutable object, but registration happens ONCE
// per workload at TU init (before any submit), and lookup is read-only thereafter;
// the std::function-free, pointer-sized slots make a torn read impossible on the
// target ABI. We size generously (kSlots) so appending Eval/Cpcv/... in S7.5 needs
// no change here.
inline constexpr atx::usize kWorkloadSlots = 16;

[[nodiscard]] inline ShardFn* workload_table() noexcept {
  static ShardFn table[kWorkloadSlots] = {}; // zero-initialised => all nullptr
  return table;
}

} // namespace detail

// Register `fn` as the body for `workload`. Intended to be called ONCE, at the
// owning TU's init, before any submit of that workload. Out-of-range ids (beyond
// the table) are silently ignored — the only ids in flight are the closed enum's.
inline void register_workload(WorkloadId workload, ShardFn fn) noexcept {
  const atx::usize idx = static_cast<atx::usize>(workload);
  if (idx < detail::kWorkloadSlots) {
    detail::workload_table()[idx] = fn;
  }
}

// Resolve `workload` to its registered ShardFn, or nullptr if unregistered (or
// out of range). The executor turns a nullptr into Err(NotFound) at submit time.
[[nodiscard]] inline ShardFn lookup_workload(WorkloadId workload) noexcept {
  const atx::usize idx = static_cast<atx::usize>(workload);
  return idx < detail::kWorkloadSlots ? detail::workload_table()[idx] : nullptr;
}

// The execution seam. Threads (this unit), processes (S7.3) and future nodes
// implement THIS — nothing above it changes (R2). An executor is a PINNED object
// (it owns workers / a substrate): copy and assignment are deleted; the default
// ctor stays available to derived classes.
class IExecutor {
public:
  virtual ~IExecutor() = default;

  IExecutor(const IExecutor&) = delete;
  IExecutor& operator=(const IExecutor&) = delete;

  // Run shards [0, n) of `workload` over the shared read-only `inputs`, each
  // shard writing ONLY its own slot in `out`. Block until all shards finish.
  // Returns the LOWEST-id failing shard's Status (deterministic), else Ok().
  // No cross-shard state: which worker runs which shard MUST NOT change a result
  // bit (R1). Precondition out.n() == n (else Err(InvalidArgument)).
  //
  // `dispatch_order` (S7-4) is an OPTIONAL permutation of [0, n): a worker claims
  // a POSITION p and processes shard `dispatch_order[p]`, writing slot
  // `dispatch_order[p]`. It only changes the ORDER shards are dispatched (a
  // scheduling hint — e.g. cost-sorted to shorten the tail), NEVER which slot a
  // shard writes, so the gathered output is byte-identical for ANY valid
  // permutation (the §4.4 no-bit-contact proof). An empty span (the default)
  // means identity order — ZERO behavior change for existing callers. If supplied
  // it must be a valid permutation of exactly [0, n) (else Err(InvalidArgument)).
  [[nodiscard]] virtual atx::core::Status
  submit(WorkloadId workload, InputView inputs, atx::usize n, SlotView out,
         std::span<const ShardId> dispatch_order = {}) = 0;

  // In-process deterministic map (S7.5a): invoke body(shard_id, worker_id) once
  // per shard_id in [0, n), worker_id in [0, workers()). Block until all shards
  // finish; the LOWEST-shard body exception is rethrown deterministically (the
  // SAME contract as DetPool::parallel_for). n == 0 is a no-op.
  //
  // WHY THIS LIVES BESIDE submit(): the engine's three as-built parallel
  // workloads (parallel_evaluate / parallel_cpcv / parallel_backtests) are pure
  // in-process maps whose body needs the WORKER ID — each worker owns its own
  // stateful Engine (Engine is NOT thread-safe) — and relies on the lowest-index
  // exception being rethrown. A std::function body cannot cross a process
  // boundary, so this entry is IN-PROCESS ONLY: ThreadExecutor forwards it to
  // DetPool; ProcessExecutor MUST reject it (those workloads take the serialized
  // submit(WorkloadId, ...) path in a later unit). std::function is fine here —
  // this is cold, coarse-grained dispatch (n shards, not n cells), so the one
  // allocation per call is immaterial.
  [[nodiscard]] virtual atx::core::Status
  parallel_for(atx::usize n, const std::function<void(ShardId, atx::usize)>& body) = 0;

  // Number of workers this substrate runs (the resolved count, never 0).
  [[nodiscard]] virtual atx::usize workers() const noexcept = 0;

protected:
  IExecutor() = default;
};

} // namespace atx::engine::parallel
