// PricingExecutor — the one process-wide persistent pricing thread pool (P1.4).
// See pricing_executor.hpp for the model, determinism argument, and sizing story.

#include "atx/vol/pricing_executor.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "atx/vol/counters.hpp"     // ATX_VOL_COUNT (opt-in; no-op when OFF)
#include "atx/vol/parallel_for.hpp" // atx_auto_worker_count (shared core budget)

#if defined(_MSC_VER)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace atx::vol {

namespace {

// ── kInlineThreshold ─────────────────────────────────────────────────────────
//
// Below this many units of work, dispatch/wake cost exceeds the parallel win, so
// run_blocks/run_ranges execute fully inline (no pool wake). MEASURED on the P0
// portfolio price_into path (build-rel, clang-cl Release; the pricing_executor_probe
// harness), sweeping the unique-contract count and timing the per-unique
// Andersen-Lake FULL-GREEKS solve serial (n_threads=1) vs. 2-/4-way, warm workspace.
// To expose the true crossover the threshold was temporarily set to 1 for the sweep.
// Per-call wall time (us), pool = 15 workers (H-1, H=16 on this box):
//
//     n   serial   2-way  4-way   2/1    4/1   verdict
//     1   2059.5  2469.2  2149.6  0.83   0.96  dispatch loses (1 unit can't split)
//     2   3374.2  2529.7  2576.7  1.33   1.31  2-way wins
//     3   5358.1  5472.9  2639.6  0.98   2.03  4-way wins (2-way at noise floor)
//     4   6580.3  4800.8  2615.8  1.37   2.52  dispatch wins solidly
//     8  13184.2  8925.6  4925.6  1.48   2.68  dispatch wins
//
// The cold Andersen-Lake FD-Greeks solve is expensive (~1.5-2 ms/unique here), so
// the crossover on that path is n≈2 (only n=1 clearly loses). The pool DISPATCH
// itself costs ~124 us/call on this box (a std::condition_variable notify_all wake
// of the parked workers) vs. ~1436 us for the old per-call jthread create+join it
// replaces (11.6x cheaper). Set to 4 — one threshold applied to BOTH run_* (the
// scatter is memory-bound with ~40x headroom per §4.5, so the solve drives it):
// safely past the cold crossover (dispatch wins 1.37x/2.52x at n=4) while inlining
// n<4, where the 2-way win is marginal/at-noise on the cold path and the cheaper
// served-greeks path (~us/unique) has too little work to amortize the ~124 us wake
// — matching the sprint's "n<4 is a few us; dispatching there only adds wake
// latency" guidance.
constexpr std::size_t kInlineThreshold = 4;

// ── Per-thread nesting state (E1: replaces the old `t_in_executor` bool) ──────
//
// CHANGE CLASS: infra; behavior-preserving for the non-nested (top-level) path.
// The old bool only encoded "am I inside executor work?" and forced EVERY nested
// run_* fully inline. This struct additionally carries the idle-worker budget the
// enclosing top-level dispatch left unused, so an OPT-IN nested dispatch
// (run_*_nested) can size a bounded second level to those spare workers. The plain
// run_* still inline when nested (byte-for-byte the old guard), so no existing call
// site changes behavior.
//
// `depth` counts the executor bodies live on THIS THREAD's stack (saturating at 2),
// which is NOT always the dispatch level of the job a body belongs to — a helping
// dispatcher runs foreign contexts from inside its own join. See run_context_body.
//
//   depth 0  — not inside a dispatch. A run_* here is a TOP-LEVEL dispatch onto the
//              whole pool (caller + P workers). Identical to the old `!t_in_executor`.
//   depth 1  — running the FIRST body on this thread, and it belongs to a top-level
//              job (the caller's block 0, or a pool worker). A plain run_* here runs
//              INLINE (the deadlock-safe default the old bool gave); a run_*_nested
//              here may make ONE nested dispatch sized to `idle_budget` (= H -
//              active_outer, the workers the top-level dispatch left parked), whose
//              contexts flow through the shared work-stealing queue.
//   depth>=2 — running a nested dispatch's body, OR any body that already has another
//              body under it on this thread. Every run_* runs INLINE. Only ONE nested
//              level is allowed: this bounds the live nesting to two.
//
// Deadlock-freedom (E2): a blocked dispatcher drains the shared queue help-first
// instead of parking on a slot, so no participant can wait on a slot it must itself
// service (plan §11.3 — the REAL nested-dispatch deadlock at HEAD). `idle_budget` now
// sizes the nested fan-out's degree; the queue, not a worker-index window, places it.
// See PricingExecutor::State below for the work-stealing machinery.
struct NestState {
  unsigned depth = 0;
  unsigned idle_budget = 0; // spare pool workers a nested dispatch may size itself to
};
thread_local NestState t_nest{};

// A resolved dispatch decision. `nt == 0` means "run this call inline".
struct DispatchPlan {
  unsigned level = 0; // nesting level of this dispatch: 0 = top-level, 1 = nested
  unsigned nt = 0;    // execution contexts (caller + workers); 0 => inline
};

// Decide how a run_* at the current NestState executes. At each level the available
// contexts are the caller plus the pool workers reachable at that level; the block
// partition and inline threshold are then applied exactly as the top-level path, so
// the resulting per-index partition (and thus the result) is identical for any
// worker count. `allow_nested` is set only by the run_*_nested entry points; a plain
// nested run_* falls through to the inline (nt == 0) return, preserving today's
// deadlock-safe default. `level` selects the nesting window a context body sees; the
// work-stealing substrate reaches workers through the shared queue, so no
// first-worker index is needed (E1's `base` retired with the fixed job slots).
[[nodiscard]] DispatchPlan plan_dispatch(unsigned pool_workers, std::size_t n,
                                         unsigned n_threads, bool allow_nested) {
  const NestState ns = t_nest;
  DispatchPlan plan;
  unsigned avail = 0u;
  if (ns.depth == 0u) {
    plan.level = 0u;
    avail = pool_workers + 1u; // caller + every worker
  } else if (ns.depth == 1u && allow_nested) {
    plan.level = 1u;
    avail = ns.idle_budget + 1u; // caller + only the parked (idle) workers
  } else {
    return plan; // nt == 0: plain nested run_* (inline), or depth >= 2 (inline)
  }
  unsigned nt = (n_threads == 0u) ? avail : n_threads;
  if (nt > avail) {
    nt = avail; // clamp DOWN to what this level may use: never grow / oversubscribe
  }
  if (static_cast<std::size_t>(nt) > n) {
    nt = static_cast<unsigned>(n);
  }
  if (nt <= 1u || n < kInlineThreshold) {
    return plan; // nt == 0: nothing to gain — inline
  }
  plan.nt = nt;
  return plan;
}

// One-time topology config, consumed by the process singleton on first build.
std::mutex g_cfg_mtx;
ExecutorConfig g_pending_cfg{};
bool g_built = false;

// ── Windows core-topology helpers (best-effort; behind _MSC_VER) ─────────────

#if defined(_MSC_VER)

// Logical CPUs (group 0, up to 64) belonging to the highest-EfficiencyClass
// physical cores = the performance cores. Empty on any failure -> caller falls
// back to Auto sizing.
[[nodiscard]] std::vector<unsigned> discover_pcores() {
  std::vector<unsigned> out;
  DWORD len = 0;
  ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  if (len == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return out;
  }
  std::vector<std::uint8_t> buf(len);
  if (!::GetLogicalProcessorInformationEx(
          RelationProcessorCore,
          reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len)) {
    return out;
  }
  // First pass: find the max EfficiencyClass (P-cores carry the highest value).
  BYTE max_eff = 0;
  for (DWORD off = 0; off < len;) {
    auto *info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
    if (info->Relationship == RelationProcessorCore) {
      max_eff = (std::max)(max_eff, info->Processor.EfficiencyClass);
    }
    off += info->Size;
  }
  // Second pass: collect logical CPUs of cores at max EfficiencyClass (group 0).
  for (DWORD off = 0; off < len;) {
    auto *info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
    if (info->Relationship == RelationProcessorCore && info->Processor.EfficiencyClass == max_eff &&
        info->Processor.GroupCount >= 1) {
      const KAFFINITY mask = info->Processor.GroupMask[0].Mask;
      for (unsigned b = 0; b < 64; ++b) {
        if ((mask >> b) & 1u) {
          out.push_back(b);
        }
      }
    }
    off += info->Size;
  }
  return out;
}

#endif // _MSC_VER

// A concrete pool plan: worker count + optional per-worker affinity mask (0 = do
// not pin). Auto/AllCores never pin; the affinity topologies pin one worker per
// selected logical CPU and fall back to Auto sizing when discovery/API is absent.
struct PoolPlan {
  unsigned workers = 0;
  std::vector<std::uint64_t> masks; // empty OR size == workers
};

[[nodiscard]] unsigned auto_worker_budget(bool honor_env_cap) {
  unsigned h = honor_env_cap ? atx_auto_worker_count() : std::thread::hardware_concurrency();
  if (h == 0u) {
    h = 1u;
  }
  return h;
}

[[nodiscard]] PoolPlan plan_pool(const ExecutorConfig &cfg) {
  PoolPlan plan;

#if defined(_MSC_VER)
  if (cfg.topology == Topology::ExplicitCpuSet) {
    if (cfg.cpu_mask != 0) {
      for (unsigned b = 0; b < 64; ++b) {
        if ((cfg.cpu_mask >> b) & 1ull) {
          plan.masks.push_back(std::uint64_t{1} << b);
        }
      }
      plan.workers = static_cast<unsigned>(plan.masks.size());
      return plan;
    }
    // empty mask -> fall through to Auto
  } else if (cfg.topology == Topology::PerformanceCores) {
    const std::vector<unsigned> pc = discover_pcores();
    if (!pc.empty()) {
      for (unsigned cpu : pc) {
        plan.masks.push_back(std::uint64_t{1} << cpu);
      }
      plan.workers = static_cast<unsigned>(plan.masks.size());
      return plan;
    }
    // discovery failed -> fall through to Auto
  }
#endif

  // Auto (default, env-capped) and AllCores (ignore the env cap): the calling
  // thread is the H-th execution context (it runs block 0), so the pool holds
  // H - 1 workers. No affinity.
  const bool all_cores = (cfg.topology == Topology::AllCores);
  const unsigned h = auto_worker_budget(/*honor_env_cap=*/!all_cores);
  plan.workers = (h > 1u) ? (h - 1u) : 0u;
  return plan;
}

} // namespace

// ── PricingExecutor::State — the persistent work-stealing pool (E2) ───────────
//
// CHANGE CLASS: pure-refactor of the scheduling substrate. The computed per-index
// results are BYTE-IDENTICAL to E1: the block partition (which context owns which
// output slot, `plan_dispatch` + `block`) is untouched — the ONLY change is WHICH
// thread runs a given context and HOW a blocked dispatcher waits. E1's two fixed
// `Job` slots + `dispatch_mtx[2]` single-slot serialization are retired for one
// shared task queue drained HELP-FIRST: a dispatcher waiting on its fan-out runs
// queued tasks instead of parking on a mutex. That removes the serialization that
// made concurrent top-level (and external-outer bounded-queue, the U5 case) callers
// wait one-at-a-time, and makes nested dispatch deadlock-free without the E1
// idle-window slot dance. Every nested work unit flows through THIS queue — never a
// bare std::async that re-enters the pool (plan §3, the known real deadlock) — which
// is exactly what makes help-first safe.
//
// Determinism proof (unchanged from E1): each output slot is written by exactly one
// context whose index range is fixed by (n, block) alone; the queue only moves which
// THREAD executes that context, never the slot it owns, so the result is identical
// for any worker count. Deadlock-freedom: a Job lives on its dispatcher's stack and
// is destroyed only after `refs` (its outstanding queued tasks) reaches 0, so no
// thread ever dereferences a freed Job; and a dispatcher blocked on its fan-out
// always makes progress by running a queued task (its own or another job's) rather
// than parking on a slot that a busy worker must first release.

struct PricingExecutor::State {
  unsigned P = 0;

  // A dispatched fan-out. Lives on the dispatcher's stack for the whole call. Its
  // (nt-1) worker contexts become queue Tasks; context 0 runs inline on the
  // dispatcher. `refs` counts queued tasks not yet run to completion; it reaching 0
  // means no other thread still references this Job (the dispatcher's join waits for
  // exactly this before letting the Job leave scope).
  struct Job {
    Trampoline fn = nullptr;
    void *closure = nullptr;
    std::size_t n = 0;
    std::size_t block = 0;
    unsigned nt = 0;               // total contexts (>= 2 whenever a Job is queued)
    unsigned child_depth = 0;      // nesting depth published to each context's body
    unsigned active_for_child = 0; // top-level worker contexts (sizes the idle window)
    std::atomic<unsigned> refs{0}; // outstanding queued tasks; 0 => Job unreferenced
    std::atomic_flag exception_captured{};
    std::exception_ptr exception{}; // single-writer winner of exception_captured
  };

  // One task = "run context `ctx` (in [1, nt)) of `job`". Context 0 is the
  // dispatcher's own inline block and is never queued.
  struct Task {
    Job *job = nullptr;
    unsigned ctx = 0;
  };

  std::mutex m;                       // guards `queue`, `started`, `stopping`
  std::condition_variable cv_q;       // workers AND helping dispatchers wait here
  std::condition_variable cv_started; // start() waits until every worker has parked
  std::deque<Task> queue;             // the shared work-stealing task queue
  bool stopping = false;
  unsigned started = 0;

  std::vector<std::thread> workers;

  void start(const ExecutorConfig &cfg) {
    const PoolPlan plan = plan_pool(cfg);
    P = plan.workers;
    workers.reserve(P);
    for (unsigned w = 0; w < P; ++w) {
      const std::uint64_t mask = (w < plan.masks.size()) ? plan.masks[w] : std::uint64_t{0};
      workers.emplace_back([this, mask] { worker_loop(mask); });
      ATX_VOL_COUNT(WorkerLaunches); // thread ACTUALLY created (once, at first use)
    }
    // Startup barrier: return only once every worker has reached its park loop.
    std::unique_lock<std::mutex> lk(m);
    cv_started.wait(lk, [&] { return started == P; });
  }

  ~State() {
    {
      std::lock_guard<std::mutex> lk(m);
      stopping = true; // no dispatch is in flight at destruction, so `queue` is empty
    }
    cv_q.notify_all();
    for (std::thread &t : workers) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  void worker_loop(std::uint64_t affinity_mask) {
#if defined(_MSC_VER)
    if (affinity_mask != 0) {
      // Best-effort: a 0 return means the pin failed; we simply continue unpinned.
      (void)::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(affinity_mask));
    }
#else
    (void)affinity_mask;
#endif
    std::unique_lock<std::mutex> lk(m);
    ++started;
    cv_started.notify_one();
    for (;;) {
      cv_q.wait(lk, [&] { return stopping || !queue.empty(); });
      if (stopping) {
        return;
      }
      const Task t = queue.front();
      queue.pop_front();
      lk.unlock();
      run_task(t);
      lk.lock();
    }
  }

  // Run one queued task to completion (WITHOUT holding `m`), then release its Job
  // ref. The last task of a job wakes anyone waiting on that job's completion.
  void run_task(const Task &t) {
    run_context_body(*t.job, t.ctx);
    if (t.job->refs.fetch_sub(1u, std::memory_order_acq_rel) == 1u) {
      std::lock_guard<std::mutex> lk(m);
      cv_q.notify_all();
    }
  }

  // Execute context `ctx` of `j`: block [ctx*block, min((ctx+1)*block, n)) for
  // run_blocks/run_ranges; a single worker-id slot for run_dynamic (block == 1, so
  // `lo` IS the worker id the trampoline reads). Publishes the body's nesting window
  // (a level-0 job's body sees depth 1 + the idle budget; a nested job's body sees
  // depth 2 + no budget, so a further run_* inlines) and captures the FIRST body
  // exception. Never rethrows — the dispatcher rethrows after the join barrier.
  void run_context_body(Job &j, unsigned ctx) {
    const NestState prev = t_nest;
    // The published depth is the THREAD's live nesting, not the job's level: it is
    // the deeper of "one level below whatever body this thread was already running"
    // (prev.depth + 1) and "the level this job's contexts belong to"
    // (j.child_depth). The two agree everywhere except one path, and that path is
    // the whole point of the max:
    //
    //   A dispatcher joins HELP-FIRST — it drains the shared queue while sitting
    //   inside its own dispatch, so it can run a context of a FOREIGN job. A
    //   thread helping from inside a NESTED (level-1) dispatch that pops a
    //   TOP-LEVEL job's context already has two bodies on its stack; publishing
    //   that job's own child_depth (1) would hand the third body a fresh
    //   idle-worker budget and let it open a fourth level, and so on down the
    //   stack — unbounded live nesting and an oversubscribed pool, exactly what
    //   the two-level bound exists to prevent. `prev.depth + 1` saturates that
    //   case at 2 ("no further nesting"), which is the correct answer for any
    //   stack deeper than one.
    //
    // Everywhere else it is a no-op: a parked worker picking up work has
    // prev.depth == 0 (max(1, child_depth) == child_depth, which is >= 1), and a
    // dispatcher's own context 0 has child_depth == prev.depth + 1 by construction.
    // The idle budget follows the PUBLISHED depth, so a body raised to 2 gets none.
    const unsigned depth = std::max(prev.depth + 1u, j.child_depth);
    t_nest.depth = depth;
    t_nest.idle_budget = (depth == 1u) ? (P - j.active_for_child) : 0u;
    const std::size_t lo = static_cast<std::size_t>(ctx) * j.block;
    const std::size_t hi = (lo + j.block < j.n) ? (lo + j.block) : j.n;
    if (lo < hi) {
      try {
        j.fn(j.closure, lo, hi);
      } catch (...) {
        if (!j.exception_captured.test_and_set(std::memory_order_acq_rel)) {
          j.exception = std::current_exception();
        }
      }
    }
    t_nest = prev;
  }

  // Publish a fan-out of `nt` contexts: queue contexts 1..nt-1 as Tasks, run context
  // 0 inline on the calling thread, then HELP-FIRST join — drain the shared queue
  // (this job's tasks, or opportunistically another job's) until every task of THIS
  // job has completed. A blocked dispatcher therefore runs queued work instead of
  // parking behind a single slot, which is what retires E1's `dispatch_mtx` and makes
  // concurrent / nested dispatch deadlock-free. `child_depth` / `active_for_child`
  // size the nesting window each context body sees (semantics unchanged from E1).
  // The first captured body exception is rethrown once the barrier is reached.
  void dispatch(unsigned nt, Trampoline fn, void *closure, std::size_t n, std::size_t block,
                unsigned child_depth, unsigned active_for_child) {
    Job j;
    j.fn = fn;
    j.closure = closure;
    j.n = n;
    j.block = block;
    j.nt = nt;
    j.child_depth = child_depth;
    j.active_for_child = active_for_child;
    j.refs.store(nt - 1u, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lk(m);
      for (unsigned ctx = 1u; ctx < nt; ++ctx) {
        queue.push_back(Task{&j, ctx});
      }
    }
    cv_q.notify_all();
    ATX_VOL_COUNT(PoolDispatches); // one real pool wake (0 on the inline path)

    // Context 0 (block 0 / worker-id 0) on the calling thread, matching E1.
    run_context_body(j, 0u);

    // Help-first join: run queued tasks until this job has no outstanding task.
    std::unique_lock<std::mutex> lk(m);
    for (;;) {
      if (j.refs.load(std::memory_order_acquire) == 0u) {
        break;
      }
      if (!queue.empty()) {
        const Task t = queue.front();
        queue.pop_front();
        lk.unlock();
        run_task(t);
        lk.lock();
      } else {
        cv_q.wait(lk, [&] {
          return j.refs.load(std::memory_order_acquire) == 0u || !queue.empty();
        });
      }
    }
    const std::exception_ptr failure = j.exception;
    lk.unlock();
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
  }
};

// ── PricingExecutor ──────────────────────────────────────────────────────────

PricingExecutor::PricingExecutor(const ExecutorConfig &cfg) : state_(std::make_unique<State>()) {
  state_->start(cfg);
}

PricingExecutor::PricingExecutor() : PricingExecutor(ExecutorConfig{}) {}

PricingExecutor::~PricingExecutor() = default;

unsigned PricingExecutor::size() const noexcept { return state_->P; }

unsigned PricingExecutor::nested_budget() const noexcept {
  // Parked pool workers a nested dispatch may borrow on THIS thread right now: the
  // H - active_outer window the enclosing top-level dispatch left idle. 0 at top
  // level, at depth >= 2, or when the outer claimed the whole pool. A caller sizing
  // a nested fan-out (WS-5 U4) reads this to pick n_threads (a nested run_*_nested
  // then fans over up to this many extra workers plus the calling context).
  return (t_nest.depth == 1u) ? t_nest.idle_budget : 0u;
}

void PricingExecutor::run_erased(std::size_t n, unsigned n_threads, Trampoline fn, void *cl,
                                 bool allow_nested) {
  if (n == 0) {
    return;
  }
  State &s = *state_;
  const DispatchPlan plan = plan_dispatch(s.P, n, n_threads, allow_nested);
  if (plan.nt == 0u) {
    // Inline: nothing to gain (single block / sub-threshold), a plain nested run_*
    // (the deadlock-safe default), or already at the max nested depth.
    fn(cl, 0, n);
    return;
  }
  const std::size_t block = (n + plan.nt - 1u) / plan.nt;
  const unsigned active = plan.nt - 1u;
  // Each context body runs one nesting level deeper than the dispatcher. A top-level
  // (level 0) job exposes the idle window (P - active parked workers) to its bodies;
  // a nested (level 1) job exposes none (a further run_* inlines). run_context_body
  // saves/sets/restores t_nest per context, so a body throwing cannot leak the depth.
  const unsigned child_depth = t_nest.depth + 1u;
  const unsigned active_for_child = (plan.level == 0u) ? active : 0u;
  s.dispatch(plan.nt, fn, cl, n, block, child_depth, active_for_child);
}

// ── Process singleton + one-time configuration ───────────────────────────────

void PricingExecutor::run_dynamic_erased(std::size_t n, unsigned n_threads, DynamicTrampoline fn,
                                         void *cl, bool allow_nested) {
  if (n == 0u) {
    return;
  }
  State &s = *state_;
  const DispatchPlan plan = plan_dispatch(s.P, n, n_threads, allow_nested);
  if (plan.nt == 0u) {
    for (std::size_t index = 0u; index < n; ++index) {
      fn(cl, index, 0u);
    }
    return;
  }
  const unsigned nt = plan.nt;

  struct DynamicDispatch {
    std::atomic<std::size_t> next{0u};
    std::size_t count{0u};
    DynamicTrampoline fn{nullptr};
    void *closure{nullptr};
  } dyn;
  dyn.count = n;
  dyn.fn = fn;
  dyn.closure = cl;
  const auto run_context = [](void *opaque, std::size_t lo, std::size_t) {
    auto &job = *static_cast<DynamicDispatch *>(opaque);
    const unsigned worker_id = static_cast<unsigned>(lo);
    for (;;) {
      const std::size_t index = job.next.fetch_add(1u, std::memory_order_relaxed);
      if (index >= job.count) {
        return;
      }
      job.fn(job.closure, index, worker_id);
    }
  };

  const unsigned active = nt - 1u;
  const unsigned child_depth = t_nest.depth + 1u;
  const unsigned active_for_child = (plan.level == 0u) ? active : 0u;
  // One fixed context per worker-id in [0, nt): with block == 1 and n == nt, context
  // k maps to the single slot [k, k+1), whose `lo` the trampoline reads as its stable
  // worker id, then drains the shared index counter until all work is claimed. The
  // context runs whole on one thread, so worker-id -> thread stays stable even nested.
  s.dispatch(nt, run_context, &dyn, nt, 1u, child_depth, active_for_child);
}

PricingExecutor &pricing_executor() noexcept {
  static PricingExecutor g{[] {
    std::lock_guard<std::mutex> lk(g_cfg_mtx);
    g_built = true;
    return g_pending_cfg;
  }()};
  return g;
}

bool configure_pricing_executor(const ExecutorConfig &cfg) noexcept {
  std::lock_guard<std::mutex> lk(g_cfg_mtx);
  if (g_built) {
    return false; // pool already built — too late
  }
  g_pending_cfg = cfg;
  return true;
}

} // namespace atx::vol
