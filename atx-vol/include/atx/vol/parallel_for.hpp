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
//   - n_threads == 0  =>  atx_auto_worker_count() (ATX_VOL_FIT_WORKERS env cap,
//     else std::thread::hardware_concurrency(), 0 falls back to 1)
//   - n_threads == 1  =>  serial (today's single-threaded path, byte-for-byte)
//   - n_threads == N  =>  N workers, each a contiguous chunk of [0, n)
//
// `fn` must be safe to call concurrently over disjoint indices. `std::jthread`
// joins on scope exit, which is the barrier: every worker has finished before
// `parallel_for` returns.

#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <system_error>
#include <thread>
#include <vector>

namespace atx::vol {

// Resolve the auto (0) worker count. 0 => ATX_VOL_FIT_WORKERS if set to a
// positive integer, else std::thread::hardware_concurrency() (>=1). Any
// explicit non-zero count passed to `parallel_for` is honored as-is and NOT
// capped -- a caller that requests a specific count means it. The env cap
// exists so nested parallelism (ctest -jN launching test processes that each
// fan out a fit) can avoid oversubscribing the box:
// `ATX_VOL_FIT_WORKERS=1 ctest -j16`. Worker count is a PERF-only knob: the
// block-partition fan-out `parallel_for` implements is bit-identical for any
// worker count, so this cannot change a fitted result.
//
// The value is read with _dupenv_s under MSVC/clang-cl: plain std::getenv
// trips /WX (-Wdeprecated-declarations) here -- matches the existing
// tests/support/bench_gate.hpp env-read pattern.
[[nodiscard]] inline unsigned atx_auto_worker_count() noexcept {
  unsigned fallback = std::thread::hardware_concurrency();
  if (fallback == 0u) {
    fallback = 1u;
  }

#if defined(_MSC_VER)
  char* e = nullptr;
  std::size_t n = 0;
  if (::_dupenv_s(&e, &n, "ATX_VOL_FIT_WORKERS") != 0 || e == nullptr) {
    return fallback;
  }
#else
  const char* e = std::getenv("ATX_VOL_FIT_WORKERS");
  if (e == nullptr) {
    return fallback;
  }
#endif

  const char* end = e;
  while (*end != '\0') {
    ++end;
  }
  unsigned v = 0u;
  const auto res = std::from_chars(e, end, v);
  const bool ok = (res.ec == std::errc{}) && (res.ptr == end) && (v >= 1u);

#if defined(_MSC_VER)
  std::free(e);
#endif

  return ok ? v : fallback;
}

template <class F>
void parallel_for(std::size_t n, unsigned n_threads, F&& fn) {
  if (n == 0) {
    return;
  }
  unsigned nt = n_threads;
  if (nt == 0) {
    nt = atx_auto_worker_count();
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
