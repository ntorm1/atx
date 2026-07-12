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

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <system_error>
#include <thread>
#include <type_traits>
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

// Dynamic disjoint-index fan-out for irregular tasks. Results retain the same
// determinism contract as parallel_for when fn writes only slot i; only the
// worker that claims a slot changes. Use this for expiry fits whose cost varies
// materially with maturity/chain density, where contiguous blocks leave cores
// idle behind one expensive tail.
template <class F> void parallel_for_dynamic(std::size_t n, unsigned n_threads, F &&fn) {
  if (n == 0) {
    return;
  }
  unsigned nt = n_threads == 0 ? atx_auto_worker_count() : n_threads;
  if (nt > n) {
    nt = static_cast<unsigned>(n);
  }
  if (nt <= 1u) {
    for (std::size_t i = 0; i < n; ++i) {
      fn(i);
    }
    return;
  }
  std::atomic<std::size_t> next{0};
  // A worker body may throw (e.g. std::bad_alloc from an allocating `fn`). An
  // exception escaping a std::jthread body calls std::terminate, so catch it,
  // record the FIRST one, and rethrow on the calling thread after every worker
  // has joined — callers then handle it through their normal error path exactly
  // as the nt<=1 serial path above already propagates. The happy path is
  // unchanged: `fn(i)` runs identically, the catch is never taken, and no
  // exception_ptr is set, so results stay bit-identical for any worker count.
  std::exception_ptr worker_exc;
  std::atomic_flag exc_captured{};
  {
    std::vector<std::jthread> workers;
    workers.reserve(nt);
    for (unsigned t = 0; t < nt; ++t) {
      workers.emplace_back([n, &next, &fn, &worker_exc, &exc_captured] {
        for (;;) {
          const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= n) {
            break;
          }
          try {
            fn(i);
          } catch (...) {
            if (!exc_captured.test_and_set(std::memory_order_acq_rel)) {
              worker_exc = std::current_exception();
            }
            return;
          }
        }
      });
    }
  } // std::jthread join here is the barrier + the happens-before for worker_exc.
  if (worker_exc) {
    std::rethrow_exception(worker_exc);
  }
}

// Worker-id overload of the dynamic fan-out: `fn` is invoked as
// `fn(index, worker_id)` with `worker_id` in [0, nt). This is ADDITIVE — the
// single-argument overload above is unchanged; the constraint below (fn must be
// invocable with a trailing worker id) is strictly more constrained, so a
// one-argument callable still binds to the original and only a two-argument
// callable selects this one (partial ordering picks the constrained template).
//
// The worker id is the seam for per-worker scratch: a caller that pre-sizes a
// `scratch[nt]` array can index `scratch[worker_id]` race-free, because each
// worker id is owned by exactly one std::jthread for the fan-out's lifetime.
// Determinism is preserved exactly as in the single-argument overload — the id
// changes only WHICH thread claims a slot, never the per-slot result, so long
// as `fn` writes only slot `index` after pure reads of shared inputs.
template <class F>
  requires std::is_invocable_v<F&, std::size_t, unsigned>
void parallel_for_dynamic(std::size_t n, unsigned n_threads, F&& fn) {
  if (n == 0) {
    return;
  }
  unsigned nt = n_threads == 0 ? atx_auto_worker_count() : n_threads;
  if (nt > n) {
    nt = static_cast<unsigned>(n);
  }
  if (nt <= 1u) {
    for (std::size_t i = 0; i < n; ++i) {
      fn(i, 0u);
    }
    return;
  }
  std::atomic<std::size_t> next{0};
  // See the single-argument overload: a throwing worker body must not
  // std::terminate. Capture the first exception and rethrow after join; the
  // no-exception path is byte-identical.
  std::exception_ptr worker_exc;
  std::atomic_flag exc_captured{};
  {
    std::vector<std::jthread> workers;
    workers.reserve(nt);
    for (unsigned t = 0; t < nt; ++t) {
      workers.emplace_back([n, t, &next, &fn, &worker_exc, &exc_captured] {
        for (;;) {
          const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= n) {
            break;
          }
          try {
            fn(i, t);
          } catch (...) {
            if (!exc_captured.test_and_set(std::memory_order_acq_rel)) {
              worker_exc = std::current_exception();
            }
            return;
          }
        }
      });
    }
  } // std::jthread join here is the barrier + the happens-before for worker_exc.
  if (worker_exc) {
    std::rethrow_exception(worker_exc);
  }
}

}  // namespace atx::vol
