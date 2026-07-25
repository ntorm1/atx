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

// ── Elastic AUTO budget (T1 / BT-T1) ────────────────────────────────────────
//
// A caller that runs long, uneven tasks under an OUTER pool needs the inner
// fan-outs reached from one of those tasks to ask "how much of the machine is
// idle RIGHT NOW", not "how much was idle when this task was claimed". A frozen
// scalar cannot express that: a straggler claimed while the outer pool is
// saturated keeps its claim-time width for its entire life, however empty the
// machine gets around it.
//
// So the AUTO (0) worker count gains an optional, thread-local, live resolver.
// Scope and blast radius are deliberately minimal:
//   - it is consulted ONLY here, for the `n_threads == 0` case, and only on the
//     one thread that installed it. `atx_auto_worker_count()` itself is NOT
//     hooked, so every other auto consumer (pricing_executor pool sizing,
//     essvi_calib's chain fan-out, calib_pool) resolves exactly as before;
//   - an EXPLICIT non-zero count is still honored verbatim, so a caller that
//     pre-sizes `scratch[nt]` and then passes `nt` can never be handed more
//     workers than it sized for;
//   - it is a raw function pointer + context, so resolution is noexcept and
//     allocation-free on a hot path;
//   - the workers a fan-out spawns UNDER an elastic budget resolve AUTO to 1
//     (see `elastic_serial_workers` below), which keeps the pre-existing guard
//     against two nested levels multiplying into H^2 runnable threads.
//
// Worker count is a PERF-only knob everywhere in this library — the block/slot
// partition is bit-identical for any count — so making it live cannot change a
// computed value.
namespace detail {

using ElasticWorkerFn = unsigned (*)(void*) noexcept;

inline thread_local ElasticWorkerFn tls_elastic_fn = nullptr;
inline thread_local void* tls_elastic_ctx = nullptr;

// Installed on threads spawned beneath an elastic budget: AUTO means serial
// there, never "the whole machine again".
inline unsigned elastic_serial_workers(void*) noexcept { return 1u; }

} // namespace detail

// RAII install/restore of the thread-local elastic AUTO resolver. Restores the
// previous resolver (not "none"), so nesting is safe.
class ScopedElasticWorkerBudget {
public:
  ScopedElasticWorkerBudget(detail::ElasticWorkerFn fn, void* ctx) noexcept
      : prev_fn_(detail::tls_elastic_fn), prev_ctx_(detail::tls_elastic_ctx) {
    detail::tls_elastic_fn = fn;
    detail::tls_elastic_ctx = ctx;
  }
  ~ScopedElasticWorkerBudget() {
    detail::tls_elastic_fn = prev_fn_;
    detail::tls_elastic_ctx = prev_ctx_;
  }
  ScopedElasticWorkerBudget(const ScopedElasticWorkerBudget&) = delete;
  ScopedElasticWorkerBudget& operator=(const ScopedElasticWorkerBudget&) = delete;

private:
  detail::ElasticWorkerFn prev_fn_;
  void* prev_ctx_;
};

// Resolve a fan-out's worker count: explicit counts verbatim, AUTO through the
// thread's elastic resolver when one is installed, else the env-capped machine
// width. Used by every fan-out in this header.
[[nodiscard]] inline unsigned atx_resolve_fanout_workers(unsigned n_threads) noexcept {
  if (n_threads != 0u) {
    return n_threads;
  }
  if (detail::tls_elastic_fn != nullptr) {
    const unsigned w = detail::tls_elastic_fn(detail::tls_elastic_ctx);
    return w == 0u ? 1u : w;
  }
  return atx_auto_worker_count();
}

template <class F>
void parallel_for(std::size_t n, unsigned n_threads, F&& fn) {
  if (n == 0) {
    return;
  }
  unsigned nt = atx_resolve_fanout_workers(n_threads);
  const bool elastic_parent = detail::tls_elastic_fn != nullptr;
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
  // A worker body may throw (e.g. std::bad_alloc from an allocating `fn`). An
  // exception escaping a std::jthread body calls std::terminate, so catch it,
  // record the FIRST one, and rethrow on the calling thread after every worker
  // has joined — the same capture-first/rethrow-after-join contract the dynamic
  // overloads below use, and which the nt<=1 serial path above already gives for
  // free. The happy path is unchanged: `fn(i)` runs identically, the catch is
  // never taken, no exception_ptr is set, and results stay bit-identical for any
  // worker count.
  std::exception_ptr worker_exc;
  std::atomic_flag exc_captured{};
  {
    std::vector<std::jthread> workers;
    workers.reserve(nt);
    for (unsigned t = 0; t < nt; ++t) {
      const std::size_t lo = static_cast<std::size_t>(t) * chunk;
      if (lo >= n) {
        break;
      }
      const std::size_t hi = (lo + chunk < n) ? (lo + chunk) : n;
      workers.emplace_back([lo, hi, elastic_parent, &fn, &worker_exc, &exc_captured] {
        const ScopedElasticWorkerBudget nested{
            elastic_parent ? &detail::elastic_serial_workers : nullptr, nullptr};
        try {
          for (std::size_t i = lo; i < hi; ++i) {
            fn(i);
          }
        } catch (...) {
          if (!exc_captured.test_and_set(std::memory_order_acq_rel)) {
            worker_exc = std::current_exception();
          }
        }
      });
    }
  } // std::jthread join here is the barrier + the happens-before for worker_exc.
  if (worker_exc) {
    std::rethrow_exception(worker_exc);
  }
}

namespace detail {

// ── Elastic dynamic fan-out (T1 / BT-T1) ────────────────────────────────────
//
// The same dynamic next-index claim as `parallel_for_dynamic`, except the pool
// WIDTH is re-resolved between tasks instead of being fixed at entry, so a
// fan-out that starts while the machine is busy widens as the machine empties.
// Selected only when the caller asked for AUTO *and* this thread has an elastic
// resolver installed; every other caller takes the fixed-width path below,
// unchanged.
//
// Why the width has to move DURING the fan-out rather than only at entry: a
// corpus board's dominant inner fan-out (the per-expiry Andersen-Lake prepass)
// is entered exactly ONCE per fit — measured, not assumed: an 8-board corpus
// build produces exactly 8 budget resolutions. Re-resolving only at fan-out
// entry is therefore indistinguishable from freezing the width when the board
// was claimed, for precisely the long-running boards the reclaim exists for.
//
// The calling thread is worker 0 and does the topping up between its own tasks:
// no watchdog thread, no polling, no sleeping, nothing timing-derived. The width
// is monotone (the resolver's `left` only falls), so the pool only ever grows and
// the caller's non-oversubscription bound still holds.
//
// Determinism is untouched: which worker claims an index cannot change the
// per-index result, which is the same contract the fixed-width overloads carry.
//
// CONTRACT for the worker-id form: under an elastic budget the live worker count
// GROWS, and ids are handed out densely from 0 as workers appear. A caller that
// pre-sizes per-worker scratch must therefore pass an EXPLICIT count — which
// bypasses this path entirely — rather than resolving AUTO itself and passing 0.
template <class Invoke> void run_elastic_dynamic(std::size_t n, Invoke &invoke) {
  const ElasticWorkerFn resolve = tls_elastic_fn;
  void *const resolve_ctx = tls_elastic_ctx;
  std::atomic<std::size_t> next{0};
  std::exception_ptr worker_exc;
  std::atomic_flag exc_captured{};
  {
    std::vector<std::jthread> workers; // topped up as the outer pool drains
    const auto spawn_up_to = [&](unsigned want) {
      if (static_cast<std::size_t>(want) > n) {
        want = static_cast<unsigned>(n);
      }
      while (workers.size() + 1u < static_cast<std::size_t>(want)) {
        const unsigned wid = static_cast<unsigned>(workers.size() + 1u);
        workers.emplace_back([n, wid, &next, &invoke, &worker_exc, &exc_captured] {
          // A fan-out reached from inside this task resolves AUTO to 1 — the
          // pre-existing guard against two nested levels multiplying out.
          const ScopedElasticWorkerBudget nested{&elastic_serial_workers, nullptr};
          for (;;) {
            const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= n) {
              break;
            }
            try {
              invoke(i, wid);
            } catch (...) {
              if (!exc_captured.test_and_set(std::memory_order_acq_rel)) {
                worker_exc = std::current_exception();
              }
              return;
            }
          }
        });
      }
    };
    spawn_up_to(resolve(resolve_ctx));
    for (;;) {
      const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= n) {
        break;
      }
      {
        const ScopedElasticWorkerBudget nested{&elastic_serial_workers, nullptr};
        try {
          invoke(i, 0u);
        } catch (...) {
          if (!exc_captured.test_and_set(std::memory_order_acq_rel)) {
            worker_exc = std::current_exception();
          }
          break;
        }
      }
      // <- the reclaim, re-asked mid-flight.
      //
      // KNOWN RESIDUAL, bounded and deliberate (rev2-ws-t N-M1). This is the ONLY
      // trigger: the calling thread finishing one of its OWN tasks. Spawned
      // workers (above) never call `spawn_up_to`; no watchdog does, no tick does.
      // Three consequences a reader of this line should know:
      //   (1) the reclaim's granularity is one task. The width cannot move DURING
      //       a task, however long that task runs;
      //   (2) so the calling thread's FIRST task always runs at the entry width.
      //       `run_deam_prepass` (curve_fit.cpp) sorts its schedule descending by
      //       n_strikes, and a saturated resolver answers 1 at entry, so the
      //       straggler board's single most expensive expiry is always the first
      //       one it runs, always at width 1, with no widening possible during
      //       it. The reclaim can only begin at that board's second chain;
      //   (3) work that is not `parallel_for_dynamic`-with-AUTO is untouched. The
      //       STATIC `parallel_for` resolves AUTO exactly once, at entry, and
      //       serial phases of a fit never re-ask at all.
      // This is a real residual, not a defect: it is bounded by one task's cost,
      // and every alternative (a watchdog thread, a polled tick, re-entering a
      // running task) reintroduces either timing-derived behaviour or a
      // preemption point inside `invoke`. Do not "fix" it without replacing that
      // trade-off deliberately. Gated by
      // ParallelForElastic.GrowsThePoolWhileTheFanOutIsStillRunning, which fails
      // on four axes if this line is removed.
      spawn_up_to(resolve(resolve_ctx));
    }
  } // std::jthread join here is the barrier + the happens-before for worker_exc.
  if (worker_exc) {
    std::rethrow_exception(worker_exc);
  }
}

} // namespace detail

// Dynamic disjoint-index fan-out for irregular tasks. Results retain the same
// determinism contract as parallel_for when fn writes only slot i; only the
// worker that claims a slot changes. Use this for expiry fits whose cost varies
// materially with maturity/chain density, where contiguous blocks leave cores
// idle behind one expensive tail.
template <class F> void parallel_for_dynamic(std::size_t n, unsigned n_threads, F &&fn) {
  if (n == 0) {
    return;
  }
  if (n_threads == 0u && detail::tls_elastic_fn != nullptr) {
    auto invoke = [&fn](std::size_t i, unsigned) { fn(i); };
    detail::run_elastic_dynamic(n, invoke);
    return;
  }
  unsigned nt = atx_resolve_fanout_workers(n_threads);
  const bool elastic_parent = detail::tls_elastic_fn != nullptr;
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
      workers.emplace_back([n, elastic_parent, &next, &fn, &worker_exc, &exc_captured] {
        const ScopedElasticWorkerBudget nested{
            elastic_parent ? &detail::elastic_serial_workers : nullptr, nullptr};
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
  if (n_threads == 0u && detail::tls_elastic_fn != nullptr) {
    auto invoke = [&fn](std::size_t i, unsigned wid) { fn(i, wid); };
    detail::run_elastic_dynamic(n, invoke);
    return;
  }
  unsigned nt = atx_resolve_fanout_workers(n_threads);
  const bool elastic_parent = detail::tls_elastic_fn != nullptr;
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
      workers.emplace_back([n, t, elastic_parent, &next, &fn, &worker_exc, &exc_captured] {
        const ScopedElasticWorkerBudget nested{
            elastic_parent ? &detail::elastic_serial_workers : nullptr, nullptr};
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
