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
  // P3.1 (perf): two-tier P-then-E schedule. Worker ordinals [0, n_pcore) pin to
  // P-core logical CPUs exactly as `PerformanceCores` does; ordinals at or past
  // n_pcore spill onto the discovered EFFICIENCY cores (one worker per E-core
  // logical CPU, at THREAD_PRIORITY_BELOW_NORMAL) instead of WRAPPING back onto
  // the P-cores. The wrap is what this fixes: `run_bounded_fit_tasks` clamps the
  // worker count only at `task_count`, so a ~60-board date with an auto budget
  // launches hardware_concurrency() == 16 workers on an i7-1260P and
  // `PerformanceCores` folds all 16 onto the 8 P-core logical CPUs -- 2x
  // oversubscribed -- while the 8 E-cores sit idle for the whole build.
  //
  // OPT-IN: engaged only when the `ATX_VOL_FIT_ECORE_TIER` environment variable is
  // set to a non-zero value (read once, process-wide) -- "1" arms the tier with the
  // below-normal priority drop, "2" arms it without. Unset (the default) makes
  // this value behave EXACTLY like `PerformanceCores` -- same masks, same worker
  // count, same priorities -- so a benchmark holding the P-core lease
  // (`configure_pricing_executor` with Topology::PerformanceCores) can never have a
  // background E-tier appear underneath it. Below-normal priority additionally
  // yields the E-cores to any foreground work rather than competing with it.
  //
  // Determinism is structural, not incidental: which worker claims which task index
  // is ALREADY nondeterministic under the existing shared-`next` counter, and every
  // task writes only its own indexed slot from inputs it does not share. Core
  // assignment, worker count, and thread priority therefore steer only WHERE and
  // WHEN a task runs, never its value -- the same argument that licenses the
  // existing C4 pin and the LPT claim reorder.
  PerformanceThenEfficiencyCores,
};

// Performance-core logical CPU count on this host (Windows: logical CPUs of the
// highest-EfficiencyClass physical cores; 0 when discovery is unavailable — e.g.
// non-Windows or an API failure). Exposed so the populate scheduler can cap its
// outer worker budget at the P-core count (the measured scaling knee). Uses the
// SAME discovery the pricing executor's Topology::PerformanceCores machinery uses
// (see fit_scheduler.cpp; mirrors pricing_executor.cpp).
[[nodiscard]] unsigned performance_core_count() noexcept;

// P3.1: efficiency-core logical CPU count on this host (Windows: logical CPUs of
// every physical core BELOW the highest EfficiencyClass; 0 when discovery is
// unavailable or the host is homogeneous). Companion to performance_core_count();
// same one-time discovery pass.
[[nodiscard]] unsigned efficiency_core_count() noexcept;

// P3.1: whether the opt-in E-core second tier is armed for this process, i.e.
// `ATX_VOL_FIT_ECORE_TIER` is set to a non-zero value AND E-core discovery found
// at least one E-core. When false, `PerformanceThenEfficiencyCores` degrades to
// `PerformanceCores` exactly. Read once and cached; exposed so tests and callers
// can assert the default-off contract without duplicating the env parsing.
[[nodiscard]] bool efficiency_core_tier_enabled() noexcept;

// Deterministic fault-injection seam for scheduler tests. Production callers
// omit it. The callback runs on the caller immediately before each background
// jthread construction and receives the zero-based background-worker ordinal.
struct FitSchedulerTestHooks {
  std::function<void(std::size_t)> before_worker_launch{};
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
// `PerformanceThenEfficiencyCores` (P3.1) additionally spills the wrap-around
// ordinals onto E-cores at below-normal priority when the opt-in env flag is
// armed; see the enum comment. It too never changes which index a context runs.
//
// `worker_budget` semantics are UNCHANGED by the affinity: a zero budget still
// resolves to hardware_concurrency() and is still clamped to task_count. The
// E-tier only redistributes the workers that would otherwise double up on a
// P-core; it never raises the worker count.
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
