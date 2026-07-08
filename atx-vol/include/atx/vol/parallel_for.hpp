#pragma once

// Deterministic block-partitioned fan-out over [0, n) — the shared primitive
// behind every bit-identical-regardless-of-thread-count hot path in this
// library (the `calibrate_pool` / `PricerFitter::value_chain` determinism
// pattern, and now `fit_curve_surface`'s per-chain de-Am pre-pass).
//
// Hoisted out of `pricer_fitter.cpp` (where it was TU-local) so more than one
// translation unit can share the single definition instead of hand-copying it.
//
// ## Contract
//
// Each worker owns a CONTIGUOUS index range [lo, hi) and writes only its own
// output slots (disjoint writes) after pure const reads of shared inputs, so
// the result is identical for any thread count:
//   - n_threads == 0  =>  std::thread::hardware_concurrency() (0 falls back to 1)
//   - n_threads == 1  =>  serial (today's single-threaded path, byte-for-byte)
//   - n_threads == N  =>  N workers, each a contiguous chunk of [0, n)
//
// `fn` must be safe to call concurrently over disjoint indices. `std::jthread`
// joins on scope exit, which is the barrier: every worker has finished before
// `parallel_for` returns.

#include <cstddef>
#include <thread>
#include <vector>

namespace atx::vol {

template <class F>
void parallel_for(std::size_t n, unsigned n_threads, F&& fn) {
  if (n == 0) {
    return;
  }
  unsigned nt = n_threads;
  if (nt == 0) {
    nt = std::thread::hardware_concurrency();
    if (nt == 0) {
      nt = 1u;
    }
  }
  if (nt > n) {
    nt = static_cast<unsigned>(n);
  }
  if (nt <= 1u) {
    for (std::size_t i = 0; i < n; ++i) {
      fn(i);
    }
    return;
  }
  const std::size_t chunk = (n + nt - 1u) / nt;
  std::vector<std::jthread> workers;
  workers.reserve(nt);
  for (unsigned t = 0; t < nt; ++t) {
    const std::size_t lo = static_cast<std::size_t>(t) * chunk;
    if (lo >= n) {
      break;
    }
    const std::size_t hi = (lo + chunk < n) ? (lo + chunk) : n;
    workers.emplace_back([lo, hi, &fn] {
      for (std::size_t i = lo; i < hi; ++i) {
        fn(i);
      }
    });
  }
  // std::jthread joins on destruction — the loop below (scope exit) is the
  // barrier; every worker has finished before parallel_for returns.
}

}  // namespace atx::vol
