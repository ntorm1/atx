#pragma once

// Internal bounded scheduler shared by fit orchestration seams. This header is
// under detail/: callers outside atx-vol must not depend on it.

#include <cstddef>
#include <functional>

#include "atx/vol/types.hpp"

namespace atx::vol::detail {

using IndexedFitTask = std::function<Status(std::size_t)>;

// C4 (perf): outer-fit-worker affinity policy. The fit path is mutex-free and its
// parallel scaling regresses past the physical P-core count on a hybrid P/E-core
// host (finding 13: 4 P-cores + 8 E-cores; unpinned outer jthreads spill onto the
// E-cores and oversubscribe them, which the OS scheduler + E-core parking then
// serialize). `PerformanceCores` pins each outer worker to a discovered
// performance-core logical CPU so the compute-bound board fits stay on the
// P-cores. Pinning changes only WHERE a task runs, never the numbers, so fit
// results stay byte-identical across affinity and worker count. Best-effort: falls
// back to unpinned when P-core discovery or the affinity API is unavailable.
enum class FitAffinity : unsigned char {
  None = 0,         // no pinning — the historical default
  PerformanceCores, // pin outer workers to discovered P-core logical CPUs
};

// Performance-core logical CPU count on this host (Windows: logical CPUs of the
// highest-EfficiencyClass physical cores; 0 when discovery is unavailable — e.g.
// non-Windows or an API failure). Exposed so the populate scheduler can cap its
// outer worker budget at the P-core count (the measured scaling knee). Uses the
// SAME discovery the pricing executor's Topology::PerformanceCores machinery uses
// (see fit_scheduler.cpp; mirrors pricing_executor.cpp).
[[nodiscard]] unsigned performance_core_count() noexcept;

// Deterministic fault-injection seam for scheduler tests. Production callers
// omit it. Both callbacks run on the CALLER thread, before any task has entered
// `task`, so a throw from either reproduces a PRE-TASK scheduler failure exactly:
//
//   * `before_worker_launch` runs immediately before each background jthread
//     construction and receives the zero-based background-worker ordinal. A throw
//     lands in the launch-gate abort path -> Err(Internal, "worker launch failed").
//   * `before_setup` (R1-a / review C-06) runs once, before the scratch vectors
//     are allocated. A throw lands in the outer catch(...) -> Err(Internal,
//     "scheduler setup failed"). That return is otherwise reachable only through a
//     real std::bad_alloc, which a test cannot provoke on demand — and it is the
//     path the production run actually took twice under memory pressure, so it
//     needs a deterministic injection point of its own rather than being asserted
//     by analogy with the launch path.
//
// Both are pre-task by construction: NOT ONE index has been claimed when either
// fires, which is precisely the condition that used to wedge the populate drain.
struct FitSchedulerTestHooks {
  std::function<void(std::size_t)> before_worker_launch{};
  std::function<void()> before_setup{};
};

// Run every index in [0, task_count) exactly once with at most `worker_budget`
// concurrent task invocations. A zero budget resolves to the machine worker
// count and is still clamped to task_count. Output publication remains the
// caller's responsibility: tasks should write only their own indexed slot. The
// callable and every shared capture it mutates must support concurrent calls;
// immutable captures require no synchronization, while shared mutable state
// must be synchronized by the caller.
//
// `affinity` (C4): when `PerformanceCores`, each execution context (the caller
// acting as worker 0 plus every background jthread) pins itself to a distinct
// discovered P-core logical CPU (wrapping when workers exceed P-cores), best-effort
// — the caller's original affinity is restored on return. Pinning never changes
// which index a context runs, so results stay byte-identical to the `None` path.
//
// The function joins every started worker before returning. Task exceptions and
// thread-start failures are translated to ErrorCode::Internal on the caller
// thread; no exception escapes a jthread body. When multiple tasks fail, the
// lowest task index determines the returned Error, including its original code
// and message, independent of scheduling.
[[nodiscard]] Status run_bounded_fit_tasks(std::size_t task_count, unsigned worker_budget,
                                           const IndexedFitTask &task,
                                           FitAffinity affinity = FitAffinity::None,
                                           const FitSchedulerTestHooks *test_hooks = nullptr);

} // namespace atx::vol::detail
