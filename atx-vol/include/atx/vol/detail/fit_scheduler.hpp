#pragma once

// Internal bounded scheduler shared by fit orchestration seams. This header is
// under detail/: callers outside atx-vol must not depend on it.

#include <cstddef>
#include <functional>

#include "atx/vol/types.hpp"

namespace atx::vol::detail {

using IndexedFitTask = std::function<Status(std::size_t)>;

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
// The function joins every started worker before returning. Task exceptions and
// thread-start failures are translated to ErrorCode::Internal on the caller
// thread; no exception escapes a jthread body. When multiple tasks fail, the
// lowest task index determines the returned Error, including its original code
// and message, independent of scheduling.
[[nodiscard]] Status run_bounded_fit_tasks(std::size_t task_count, unsigned worker_budget,
                                           const IndexedFitTask &task,
                                           const FitSchedulerTestHooks *test_hooks = nullptr);

} // namespace atx::vol::detail
