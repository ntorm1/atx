#pragma once

// Longest-expected-first dispatch order for the per-expiry fan-outs
// (`fit_curve_surface`'s prepass, carry repair and starved retry;
// `fit_quote_surface`'s pass A and channel stages).
//
// `parallel_for_dynamic` hands out task positions in increasing order, so the
// order of the task list is the order in which the expiries start. With the
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
// is the complete dispatch order. When the fan-out runs on spawned workers,
// nothing is recorded, because the caller's thread_local stays armed and the
// workers' stays null. Production never arms it, and an unarmed trace costs one
// thread_local load per expiry.

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace atx::vol::detail {

// `tasks` (expiry indices) stably sorted by `cost[task]` descending: the most
// expensive first, and equal costs in their incoming order.
// Precondition: every task < cost.size().
[[nodiscard]] inline std::vector<std::size_t> longest_first(std::vector<std::size_t> tasks,
                                                            std::span<const std::size_t> cost) {
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

} // namespace atx::vol::detail
