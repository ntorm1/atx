#pragma once

// Bounded per-expiry fan-out of the graded surface fit (lane P0, r6). Internal header
// (src/fitting); not public API.
//
// fit_quote_surface's per-expiry stages (pass A or the knot pass, B1, B2) used to start
// fit_workers fresh std::jthreads per stage (parallel_for_dynamic). They now run on the
// library's ONE persistent pool (pricing_executor(), api/pricing/pricing_executor.hpp):
// no thread is created per fit once the pool is up, the caller takes part as context 0,
// and the width is clamped DOWN to the pool (size() + 1), whose size follows the same
// ATX_VOL_FIT_WORKERS cap.
//
// Nested-dispatch rule (docs/all-core-scheduling.md): a fit task is pool work, so a plain
// pricing_executor().run_* issued inside it runs INLINE on the task's thread, and a
// fit_quote_surface called from inside pool work runs its stages inline too. Live fitting
// therefore never nests the pool and never oversubscribes it, however many surfaces fit
// concurrently: concurrent top-level fits share the pool's workers (help-first join).
//
// Determinism: which context claims a task never changes what the task computes - each
// task writes only its own expiry's slot from inputs no task writes (the caller's
// contract), and nothing a task reads depends on its thread (lane P0: Eigen's blocking is
// pinned at build time, root CMakeLists.txt). Served rows are therefore byte-identical for
// any width, any claim order and any core (tests/fit_fanout_determinism_test.cpp).

#include <cstddef>

#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "core/parallel_for.hpp" // atx_resolve_fanout_workers (the AUTO width)

namespace atx::vol::detail {

// The width a fan-out of `tasks` per-expiry tasks requests: `fit_workers` verbatim, AUTO
// (0) through the thread's elastic resolver or the ATX_VOL_FIT_WORKERS cap, never more
// than the tasks; 1 means serial on the caller. The pool may clamp it further.
[[nodiscard]] inline unsigned expiry_fanout_width(std::size_t tasks,
                                                  unsigned fit_workers) noexcept {
  if (tasks <= 1u) return 1u;
  unsigned width = atx_resolve_fanout_workers(fit_workers);
  if (static_cast<std::size_t>(width) > tasks) width = static_cast<unsigned>(tasks);
  return width == 0u ? 1u : width;
}

// Runs fn(task) once for every task in [0, n): serially on the caller at width 1 (the
// historical serial path, in task order), else on the persistent pool, tasks claimed in
// increasing order (so a longest-first task list starts its densest expiry first). The
// first exception any task throws is rethrown on the caller once every context has
// stopped (PricingExecutor::run_dynamic); which other tasks ran is then unspecified, so a
// caller treats every slot as indeterminate (fit_quote_surface returns its typed error).
template <class F> void run_expiry_tasks(std::size_t n, unsigned fit_workers, F &&fn) {
  const unsigned width = expiry_fanout_width(n, fit_workers);
  if (width <= 1u) {
    for (std::size_t task = 0; task < n; ++task) fn(task);
    return;
  }
  pricing_executor().run_dynamic(n, width, [&fn](std::size_t task, unsigned) { fn(task); });
}

} // namespace atx::vol::detail
