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
// ## Nesting: inline-by-default, with an opt-in one-level nested budget (E1)
//
// A pool worker must never block waiting on the pool it is running inside — that
// self-oversubscribes and would deadlock a fully-occupied pool. So the DEFAULT
// `run_blocks` / `run_ranges` / `run_dynamic` still run FULLY INLINE when issued
// from within executor work (a pool worker, or the caller running its own block 0):
// byte-for-byte the historical re-entrancy guard, and the safe choice for every
// existing call site.
//
// The `*_nested` variants add an EXPLICIT, bounded second level. A per-thread
// `{depth, idle_budget}` records how many pool workers the enclosing TOP-LEVEL
// dispatch left parked (`H - active_outer`); a nested dispatch sizes its fan-out to
// that window and pushes its contexts onto the shared work-stealing queue (E2). A
// dispatcher blocked on its own fan-out drains that queue HELP-FIRST — it runs
// queued tasks instead of parking on a slot — so a nested (or concurrent, or
// external-outer) dispatch can never wait on a slot a busy worker must first free,
// and cannot deadlock. A second nested level (depth >= 2) always inlines, bounding
// the live nesting to two. That bound is on the THREAD's stack, not on any one
// job: a helping dispatcher running a foreign job's context counts it as a level,
// so such a body inlines its own run_* and reports a 0 `nested_budget()` even when
// the job it belongs to is top-level. `nested_budget()` exposes the current window
// for callers that size their own fan-out (the fit/universe scheduler, WS-5). At
// top level the `*_nested` variants are identical to their plain counterparts.
//
// A body exception is captured at its first caller/worker observation. Every
// participant still reaches the join barrier before that exception is rethrown on
// the calling thread, so stack-backed closures remain alive and the pool remains
// reusable. An inline/nested call has no participants to join and propagates
// directly.
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
//
// ## Thread-safety (plan 4.7)
//
// This header declares the library's ONE piece of process-global mutable state,
// so its rules are stated rather than inferred.
//
// THE SINGLETON. `pricing_executor()` returns a reference to a function-local
// static, so its construction is thread-safe and happens EXACTLY ONCE: concurrent
// first callers block until the pool is built, and every later call is a plain
// reference read. Construction is not free — it creates `size()` OS threads and
// returns only after a startup barrier sees all of them parked — so the first
// pricing call on a process pays for the whole pool.
//
// THE POOL IS SHARED, AND MEANT TO BE. Every `run_*` from any thread dispatches
// onto the same workers. Concurrent TOP-LEVEL dispatches are supported: contexts
// go onto the shared work-stealing queue and a dispatcher blocked on its own
// fan-out drains that queue help-first, so no dispatch can wait on a slot another
// dispatch must first free. Each dispatch's own partition is what fixes its
// results, and that partition does not depend on how many other dispatches are in
// flight — the bit-identity claim above survives concurrency. What callers still
// owe is the body contract: disjoint writes per index, and no body that blocks on
// the pool it is running inside (the plain `run_*` inline when nested precisely so
// that mistake cannot deadlock; only the `*_nested` variants add one real level).
//
// `size()` is fixed for the pool's life and safe to read from anywhere.
// `nested_budget()` reads PER-THREAD state only, so it answers about the calling
// thread and never synchronizes with the pool.
//
// CONFIGURATION IS ORDER-SENSITIVE, NOT RACE-PRONE. `configure_pricing_executor`
// and the singleton's builder take the same internal mutex, so calling them
// concurrently cannot corrupt anything. What concurrency does NOT give you is a
// deterministic ORDER: whether your topology is recorded in time to be consumed,
// or refused with AlreadyExists because another thread's pricing call built the
// pool first, is decided by whichever acquires that mutex first. So configure ONCE,
// from ONE thread, before any library entry that can price — that is the only
// usage under which the return value means what it says.
//
// SHUTDOWN. The workers are joined by the static's destructor, which runs after
// `main`. Do not dispatch from a static destructor that may be sequenced after it.
//
// KNOWN SHARP EDGE, recorded rather than papered over: `pricing_executor()` is
// declared `noexcept`, but the construction it performs allocates and spawns
// threads. If that allocation or a `std::thread` creation fails, the exception
// escapes a `noexcept` function and the process terminates rather than reporting.
// That is the CURRENT behaviour; treating pool-construction failure as a reportable
// error is a signature change, not a comment.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "atx/vol/types.hpp" // Status, ErrorCode

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
               static_cast<void *>(std::addressof(body)), /*allow_nested=*/false);
  }

  // Run `body(lo, hi)` ONCE per contiguous block (so a worker can amortize
  // per-range setup). Same clamp / partition / inline rules as run_blocks; the
  // body must write only slots derived from its own [lo, hi).
  template <class F> void run_ranges(std::size_t n, unsigned n_threads, F &&body) {
    run_erased(n, n_threads, &trampoline_ranges<std::remove_reference_t<F>>,
               static_cast<void *>(std::addressof(body)), /*allow_nested=*/false);
  }

  // Nested-capable variants of run_blocks / run_ranges (E1). IDENTICAL to their
  // plain counterparts at TOP LEVEL. The difference is only when called from INSIDE
  // another dispatch's body: the plain run_* run inline (the deadlock-safe default),
  // while these may make ONE additional level of real pool dispatch, borrowing up to
  // `nested_budget()` idle workers (the `H - active_outer` the enclosing top-level
  // dispatch left parked). A second nested level (depth >= 2) always inlines. The
  // nested dispatch never wakes a worker busy in the enclosing dispatch, so it
  // cannot self-oversubscribe or deadlock, and the block partition keeps the result
  // bit-identical for any worker count. This is the explicit nested-budget path the
  // fit / universe fan-outs (WS-5) build on.
  template <class F> void run_blocks_nested(std::size_t n, unsigned n_threads, F &&body) {
    run_erased(n, n_threads, &trampoline_blocks<std::remove_reference_t<F>>,
               static_cast<void *>(std::addressof(body)), /*allow_nested=*/true);
  }
  template <class F> void run_ranges_nested(std::size_t n, unsigned n_threads, F &&body) {
    run_erased(n, n_threads, &trampoline_ranges<std::remove_reference_t<F>>,
               static_cast<void *>(std::addressof(body)), /*allow_nested=*/true);
  }

  // On successful completion, run `body(index, worker_id)` exactly once for every
  // index in [0, n), dynamically claiming indices to balance irregular work. If
  // a body throws, all active contexts finish before rethrow, but unclaimed work
  // is not part of the exceptional-completion contract. A participating
  // execution context owns one stable worker_id in [0, resolved_threads) for
  // the dispatch lifetime, so callers may safely index pre-sized per-worker
  // scratch. Auto/clamping, inline-threshold, join-barrier, and nested-inline
  // behavior match run_blocks/run_ranges; inline work always uses worker_id 0.
  template <class F> void run_dynamic(std::size_t n, unsigned n_threads, F &&body) {
    run_dynamic_erased(n, n_threads, &trampoline_dynamic<std::remove_reference_t<F>>,
                       static_cast<void *>(std::addressof(body)), /*allow_nested=*/false);
  }

  // Nested-capable run_dynamic (E1). Identical to run_dynamic at top level; from
  // inside another dispatch's body it may make ONE nested dispatch over the idle
  // window (see run_blocks_nested). Each participating context still owns one stable
  // worker_id in [0, resolved_threads) for the nested dispatch's lifetime, so nested
  // per-worker scratch stays race-free.
  template <class F> void run_dynamic_nested(std::size_t n, unsigned n_threads, F &&body) {
    run_dynamic_erased(n, n_threads, &trampoline_dynamic<std::remove_reference_t<F>>,
                       static_cast<void *>(std::addressof(body)), /*allow_nested=*/true);
  }

  // Pool worker count (fixed for the pool's life). May be 0 on a single-core box
  // (everything then runs inline). `n_threads` requests never exceed size() + 1.
  [[nodiscard]] unsigned size() const noexcept;

  // Idle pool workers a `*_nested` dispatch may borrow on THIS thread right now: the
  // `H - active_outer` window the enclosing top-level dispatch left parked. 0 at top
  // level, at nested depth >= 2, or when the outer dispatch claimed the whole pool.
  // A caller sizing a nested fan-out (e.g. WS-5 U4's shared worker budget) reads this
  // to pick n_threads; a `*_nested` call then fans over up to this many extra workers
  // plus the calling context. Reads only per-thread state (no pool interaction).
  [[nodiscard]] unsigned nested_budget() const noexcept;

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
  using DynamicTrampoline = void (*)(void *, std::size_t, unsigned);

  template <class U> static void trampoline_blocks(void *c, std::size_t lo, std::size_t hi) {
    U &f = *static_cast<U *>(c);
    for (std::size_t i = lo; i < hi; ++i) {
      f(i);
    }
  }
  template <class U> static void trampoline_ranges(void *c, std::size_t lo, std::size_t hi) {
    (*static_cast<U *>(c))(lo, hi);
  }
  template <class U>
  static void trampoline_dynamic(void *c, std::size_t index, unsigned worker_id) {
    (*static_cast<U *>(c))(index, worker_id);
  }

  void run_erased(std::size_t n, unsigned n_threads, Trampoline fn, void *closure,
                  bool allow_nested);
  void run_dynamic_erased(std::size_t n, unsigned n_threads, DynamicTrampoline fn, void *closure,
                          bool allow_nested);

  struct State;
  std::unique_ptr<State> state_;
};

// THE one executor. Lazily built (workers spawned) on first call; workers joined
// at process exit (the singleton is a function-local static, destroyed after main).
[[nodiscard]] PricingExecutor &pricing_executor() noexcept;

// Optional one-time topology configuration. Must be called BEFORE the first
// `pricing_executor()` use: once the pool is built its topology is fixed for the
// life of the process and this call applies nothing.
//
// 4.3 — this used to answer in a bare `bool` that its one shipped caller
// discarded, so "your topology was ignored, the pool was already built" was
// unobservable. It now reports through the library's one error channel; the
// refusal is AlreadyExists (the pool already exists) and names the ordering
// constraint it enforces. Not `noexcept`: an Error carries a message string.
//
// @return Ok() when `cfg` was recorded for the pool's construction.
[[nodiscard]] Status configure_pricing_executor(const ExecutorConfig &cfg);

} // namespace atx::vol
