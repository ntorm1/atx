#pragma once

// Longest-expected-first dispatch order for the per-expiry fan-outs
// (`fit_curve_surface`'s prepass, carry repair and starved retry;
// `fit_quote_surface`'s pass A and channel stages).
//
// `parallel_for_dynamic` and the persistent pool's `run_dynamic` (the
// `fit_quote_surface` stages, `expiry_pool.hpp`) hand out task positions in
// increasing order, so the order of the task list is the order in which the
// expiries start. With the
// densest expiries first, the cheap ones fill in behind them. A dense expiry
// claimed last would otherwise leave one worker running long after every other
// worker has gone idle. The order is a pure function of the inputs: a stable
// sort on a per-expiry cost known before the fit starts, with no timing feedback
// and nothing adaptive. It decides only which worker runs an expiry and when,
// never what that expiry computes, because every task writes only its own
// output slot. Results are therefore identical for any order and any worker
// count.
//
// Test seam (the counters.hpp `t_step_sink` pattern): a trace armed on the
// calling thread records each task's expiry index as that task starts on that
// thread. At fit_workers = 1 every task runs inline on the caller, so the trace
// is the complete dispatch order. Above one worker only the tasks the caller
// itself runs are recorded (none on `parallel_for_dynamic`'s spawned workers; the
// caller's share on the pool, where it takes part as context 0), because the
// caller's thread_local stays armed and every other thread's stays null.
// Production never arms it, and an unarmed trace costs one thread_local load per
// expiry.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <span>
#include <vector>

namespace atx::vol::detail {

// `tasks` (expiry indices) stably sorted by `cost[task]` descending: the most
// expensive first, and equal costs in their incoming order.
// Precondition: every task < cost.size() (asserted in debug builds).
[[nodiscard]] inline std::vector<std::size_t> longest_first(std::vector<std::size_t> tasks,
                                                            std::span<const std::size_t> cost) {
  assert(std::all_of(tasks.begin(), tasks.end(),
                     [&cost](std::size_t task) { return task < cost.size(); }));
  std::stable_sort(tasks.begin(), tasks.end(),
                   [cost](std::size_t a, std::size_t b) { return cost[a] > cost[b]; });
  return tasks;
}

inline thread_local std::vector<std::size_t> *t_expiry_dispatch_trace = nullptr;

// Called by each fan-out task as its first statement. It records only on a
// thread that armed a trace (test-only), and push_back may then allocate.
inline void note_expiry_dispatch(std::size_t expiry) {
  if (t_expiry_dispatch_trace != nullptr) {
    t_expiry_dispatch_trace->push_back(expiry);
  }
}

// RAII arm and restore of the calling thread's dispatch trace (tests only).
class ScopedExpiryDispatchTrace {
public:
  explicit ScopedExpiryDispatchTrace(std::vector<std::size_t> &sink) noexcept
      : prev_(t_expiry_dispatch_trace) {
    t_expiry_dispatch_trace = &sink;
  }
  ~ScopedExpiryDispatchTrace() { t_expiry_dispatch_trace = prev_; }
  ScopedExpiryDispatchTrace(const ScopedExpiryDispatchTrace &) = delete;
  ScopedExpiryDispatchTrace &operator=(const ScopedExpiryDispatchTrace &) = delete;

private:
  std::vector<std::size_t> *prev_;
};

// Fault-injection seam (tests only; lane L2c): a process-wide hook that every
// fit_quote_surface fan-out task calls first, on whichever thread runs it, with its
// expiry index. A test installs one that throws to prove that an exception raised in
// a task (a worker's std::bad_alloc, rethrown on the caller after the join) is
// returned as the surface's typed error and never escapes fit_quote_surface. Never
// installed in production: one acquire load per expiry task.
using ExpiryTaskHook = void (*)(std::size_t expiry);
inline std::atomic<ExpiryTaskHook> g_expiry_task_hook{nullptr};

inline void run_expiry_task_hook(std::size_t expiry) {
  if (const ExpiryTaskHook hook = g_expiry_task_hook.load(std::memory_order_acquire)) {
    hook(expiry);
  }
}

// RAII install and restore of the process-wide task hook (tests only; not nestable
// across threads: install it from the test thread around one fit).
class ScopedExpiryTaskHook {
public:
  explicit ScopedExpiryTaskHook(ExpiryTaskHook hook) noexcept
      : prev_(g_expiry_task_hook.exchange(hook, std::memory_order_acq_rel)) {}
  ~ScopedExpiryTaskHook() { g_expiry_task_hook.store(prev_, std::memory_order_release); }
  ScopedExpiryTaskHook(const ScopedExpiryTaskHook &) = delete;
  ScopedExpiryTaskHook &operator=(const ScopedExpiryTaskHook &) = delete;

private:
  ExpiryTaskHook prev_;
};

} // namespace atx::vol::detail
