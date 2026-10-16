#pragma once

// ── Internal seam: performance-core discovery and calling-thread pinning ──────
//
// The pricing pool pins one worker per performance-core logical CPU
// (pricing_executor.cpp, Topology::PerformanceCores). The 2026-10-16 pricing
// hot-path audit (§0, item 5a) found that the thread that does the MEASURING in
// every benchmark — the Google Benchmark main thread — was never pinned, so on
// the i7-1260P P/E hybrid it could land on an E-core or migrate mid-run and the
// single-thread rows swung 2.6-3x run to run. This header exposes the pool's own
// discovery and pinning so the bench main thread (and the tick path, which runs
// inline on one isolated P-core) is pinned exactly the way pool workers are.
//
// NOT a public API (src/, private include root). Best-effort on Windows; every
// function degrades to "nothing pinned" elsewhere, and affinity is a latency
// prior, never a correctness condition.

#include <cstdint>
#include <vector>

namespace atx::vol::detail {

// Logical CPUs (processor group 0, ids < 64) of the cores with the highest
// EfficiencyClass — the performance cores. Empty on any failure or non-Windows
// host, in which case callers fall back to unpinned behaviour.
[[nodiscard]] std::vector<unsigned> performance_core_cpus();

// One logical-CPU mask per performance core (its SMT siblings together; bit b =
// logical CPU b, group 0), in the same enumeration order as
// performance_core_cpus(), whose ids are exactly these masks' bits. Lets a caller
// keep a whole physical core free of pool workers (the bench main thread shares
// a core with nobody, not even a hyperthread sibling). Empty when discovery fails.
[[nodiscard]] std::vector<std::uint64_t> performance_core_masks();

// The pool mask for a measuring thread pinned to `main_cpu`: every performance-
// core logical CPU EXCEPT the physical core that holds `main_cpu` (both of its SMT
// siblings). 0 when there is no other performance core to give the pool (the
// caller then keeps the default topology).
[[nodiscard]] std::uint64_t pool_mask_excluding_core_of(
    const std::vector<std::uint64_t> &pcore_masks, unsigned main_cpu) noexcept;

// Set the calling thread's affinity to `mask` (bit b = logical CPU b, group 0).
// Returns true iff the OS accepted it. The pool workers call this with a single-
// bit mask; the bench main thread calls it through pin_current_thread_for_latency.
[[nodiscard]] bool pin_current_thread_affinity(std::uint64_t mask) noexcept;

// What pin_current_thread_for_latency actually achieved. Each field reports one
// independent OS call, so a partial pin is visible rather than collapsed into a
// single bool.
struct ThreadPinReport {
  unsigned requested_cpu = 0;
  bool affinity_set = false;        // SetThreadAffinityMask accepted the 1-bit mask
  bool priority_raised = false;     // THREAD_PRIORITY_HIGHEST applied
  bool throttling_disabled = false; // power throttling (EcoQoS) opted out for this thread
  int observed_cpu = -1;            // GetCurrentProcessorNumber() after the pin; -1 if unknown

  // The measurement contract: the thread is fixed to the requested CPU and the
  // scheduler has actually moved it there.
  [[nodiscard]] bool pinned() const noexcept {
    return affinity_set && observed_cpu == static_cast<int>(requested_cpu);
  }
};

// Pin the CALLING thread to logical CPU `cpu` (group 0), raise it to
// THREAD_PRIORITY_HIGHEST and opt it out of power throttling, then yield once so
// the scheduler migrates it before the observed CPU is read. Never throws; on a
// non-Windows host every field reports false/-1.
[[nodiscard]] ThreadPinReport pin_current_thread_for_latency(unsigned cpu) noexcept;

// Logical CPU the calling thread is running on right now, or -1 if unknown.
[[nodiscard]] int current_cpu() noexcept;

} // namespace atx::vol::detail
