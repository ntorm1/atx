#pragma once

// PricingExecutor — the one process-wide persistent pricing thread pool (P1.4).
//
// ## Why
//
// `PortfolioPricer` is reused across many surface snapshots (a fixed book, a
// moving market — a 250-date backtest that reprices the same book each step, or a
// quote loop). T5/T6 made the prepared substrate and the output frame persistent
// and allocation-free across snapshots. The one thing still created and destroyed
// on every `price_into` / `price_totals` / `pnl_explain` call was the worker
// threads: the old TU-local `parallel_blocks` / `parallel_ranges` helpers each
// spun up a fresh `std::vector<std::jthread>` per call — thousands of thread
// create/join cycles on a sustained backtest, pure overhead.
//
// This executor removes that: a single, lazily-built, process-lifetime pool whose
// workers are created ONCE and reused for every subsequent call. A `run_*` call
// allocates nothing and creates no thread in steady state (proved by the
// `WorkerLaunches` / `PoolDispatches` counters).
//
// ## Determinism / bit-identity (the acceptance gate)
//
// `run_blocks` / `run_ranges` split `[0, n)` into `nt` CONTIGUOUS DISJOINT chunks
// exactly like the helpers they replace: block size `(n + nt - 1) / nt`, block 0
// `[0, min(n, block))` runs INLINE on the calling thread, blocks `1..nt-1` are
// dispatched to pool workers, and a join-barrier completes before the call
// returns. Every index is written by exactly one worker (disjoint writes over pure
// const reads), so the result is identical for any worker count — the partition
// never moves which slot an index lands in. This is a plumbing swap: the caller's
// serial scatter and fixed-order totals reduction are untouched.
//
// ## Nested-parallelism guard (real correctness, not cosmetic)
//
// A pool worker must never block waiting on the pool it is running inside — that
// self-oversubscribes and would deadlock a fully-occupied pool. A thread-local
// re-entrancy flag makes a `run_*` call issued from *within* executor work (a pool
// worker, or the caller while it is running its own block 0) execute FULLY INLINE
// instead of dispatching.
//
// ## Sizing / topology
//
// The pool draws from the same core budget the fitter's `parallel_for` uses: its
// `Auto` size honours `atx_auto_worker_count()` (the `ATX_VOL_FIT_WORKERS` env
// cap, else `hardware_concurrency`) so the two pools share one knob and nested
// `ctest -jN` runs do not oversubscribe. The pricing pool and the fitter pool draw
// from the same cores and are not meant to run concurrently oversubscribed.
//
// The pool holds `H - 1` workers (the calling thread is the H-th execution
// context: it runs block 0), so a full-degree dispatch uses caller + (H-1) workers
// = H threads — exactly the degree the old `n_threads == 0` auto path used.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace atx::vol {

// Which cores the pool's workers run on. `Auto` and `AllCores` are the fully
// implemented shipping paths; `PerformanceCores` / `ExplicitCpuSet` set worker
// affinity best-effort on Windows and fall back to `Auto` sizing if discovery or
// the affinity API fails (affinity is a latency prior, never a correctness gate).
enum class Topology : std::uint8_t {
  Auto,             // hardware_concurrency, honouring the ATX_VOL_FIT_WORKERS env cap
  PerformanceCores, // pin workers to discovered P-cores (Windows); else Auto
  AllCores,         // hardware_concurrency, ignoring the env cap
  ExplicitCpuSet,   // pin workers to the logical CPUs in cpu_mask
};

struct ExecutorConfig {
  Topology topology{Topology::Auto};
  std::uint64_t cpu_mask{0}; // used only for ExplicitCpuSet (bit i => logical CPU i)
};

class PricingExecutor {
public:
  PricingExecutor();
  ~PricingExecutor();
  PricingExecutor(const PricingExecutor &) = delete;
  PricingExecutor &operator=(const PricingExecutor &) = delete;
  PricingExecutor(PricingExecutor &&) = delete;
  PricingExecutor &operator=(PricingExecutor &&) = delete;

  // Run `body(i)` for every i in [0, n). Splits [0, n) into `nt` contiguous
  // disjoint blocks (see the file header); block 0 runs inline on the caller, the
  // pool runs the rest, a barrier joins before return. `n_threads == 0` requests
  // the full pool; any value is clamped DOWN to the pool size and to n (the pool
  // never grows). Below-threshold or nested calls run fully inline. Each index
  // must be written by exactly its own slot (disjoint writes) for bit-identity.
  template <class F> void run_blocks(std::size_t n, unsigned n_threads, F &&body) {
    run_erased(n, n_threads, &trampoline_blocks<std::remove_reference_t<F>>,
               static_cast<void *>(std::addressof(body)));
  }

  // Run `body(lo, hi)` ONCE per contiguous block (so a worker can amortize
  // per-range setup). Same clamp / partition / inline rules as run_blocks; the
  // body must write only slots derived from its own [lo, hi).
  template <class F> void run_ranges(std::size_t n, unsigned n_threads, F &&body) {
    run_erased(n, n_threads, &trampoline_ranges<std::remove_reference_t<F>>,
               static_cast<void *>(std::addressof(body)));
  }

  // Pool worker count (fixed for the pool's life). May be 0 on a single-core box
  // (everything then runs inline). `n_threads` requests never exceed size() + 1.
  [[nodiscard]] unsigned size() const noexcept;

private:
  // Build a pool with an explicit topology (used by the process singleton after it
  // has consumed any pending configure_pricing_executor()). Kept private so the
  // public surface stays "default-construct an Auto pool".
  explicit PricingExecutor(const ExecutorConfig &cfg);
  friend PricingExecutor &pricing_executor() noexcept;

  // Type-erased body: (closure, lo, hi). No per-call heap — the closure pointer is
  // the caller's `body` on its own stack, kept alive by the join-barrier; the
  // trampoline is a captureless-lambda function pointer (one per body type).
  using Trampoline = void (*)(void *, std::size_t, std::size_t);

  template <class U> static void trampoline_blocks(void *c, std::size_t lo, std::size_t hi) {
    U &f = *static_cast<U *>(c);
    for (std::size_t i = lo; i < hi; ++i) {
      f(i);
    }
  }
  template <class U> static void trampoline_ranges(void *c, std::size_t lo, std::size_t hi) {
    (*static_cast<U *>(c))(lo, hi);
  }

  void run_erased(std::size_t n, unsigned n_threads, Trampoline fn, void *closure);

  struct State;
  std::unique_ptr<State> state_;
};

// THE one executor. Lazily built (workers spawned) on first call; workers joined
// at process exit (the singleton is a function-local static, destroyed after main).
[[nodiscard]] PricingExecutor &pricing_executor() noexcept;

// Optional one-time topology configuration. Must be called BEFORE the first
// `pricing_executor()` use; once the pool is built this is a no-op that returns
// false. Returns whether the config was applied.
bool configure_pricing_executor(const ExecutorConfig &cfg) noexcept;

} // namespace atx::vol
