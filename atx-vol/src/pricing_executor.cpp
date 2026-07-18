// PricingExecutor — the one process-wide persistent pricing thread pool (P1.4).
// See pricing_executor.hpp for the model, determinism argument, and sizing story.

#include "atx/vol/pricing_executor.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
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
// (run_*_nested) can safely fan a second level onto exactly those parked workers.
// The plain run_* still inline when nested (byte-for-byte the old guard), so no
// existing call site changes behavior.
//
//   depth 0  — not inside a dispatch. A run_* here is a TOP-LEVEL dispatch onto the
//              whole pool (caller + P workers). Identical to the old `!t_in_executor`.
//   depth 1  — running a top-level dispatch's body (the caller's block 0, or a pool
//              worker). A plain run_* here runs INLINE (the deadlock-safe default the
//              old bool gave); a run_*_nested here may make ONE nested dispatch onto
//              [idle_base, idle_base+idle_budget) — the H - active_outer workers the
//              top-level dispatch left parked.
//   depth>=2 — running a nested dispatch's body. Every run_* runs INLINE. Only ONE
//              nested level is allowed: this bounds recursion and the number of live
//              job slots to two.
//
// Deadlock-freedom: a nested dispatch NEVER wakes a worker already busy on the
// enclosing dispatch (it borrows only parked workers), so no participant can block
// waiting on a slot it must itself service (plan §11.3 — the REAL nested-dispatch
// deadlock at HEAD). See PricingExecutor::State below for the two-slot machinery.
struct NestState {
  unsigned depth = 0;
  unsigned idle_base = 0;   // first parked pool-worker index (meaningful at depth 1)
  unsigned idle_budget = 0; // parked pool workers a nested dispatch may borrow
};
thread_local NestState t_nest{};

// A resolved dispatch decision. `nt == 0` means "run this call inline".
struct DispatchPlan {
  unsigned level = 0; // which job slot: 0 = top-level, 1 = nested
  unsigned base = 0;  // first pool-worker index that services the job
  unsigned nt = 0;    // execution contexts (caller + workers); 0 => inline
};

// Decide how a run_* at the current NestState executes. At each level the available
// contexts are the caller plus the pool workers reachable at that level; the block
// partition and inline threshold are then applied exactly as the top-level path, so
// the resulting per-index partition (and thus the result) is identical for any
// worker count. `allow_nested` is set only by the run_*_nested entry points; a plain
// nested run_* falls through to the inline (nt == 0) return, preserving today's
// deadlock-safe default.
[[nodiscard]] DispatchPlan plan_dispatch(unsigned pool_workers, std::size_t n,
                                         unsigned n_threads, bool allow_nested) {
  const NestState ns = t_nest;
  DispatchPlan plan;
  unsigned avail = 0u;
  if (ns.depth == 0u) {
    plan.level = 0u;
    plan.base = 0u;
    avail = pool_workers + 1u; // caller + every worker
  } else if (ns.depth == 1u && allow_nested) {
    plan.level = 1u;
    plan.base = ns.idle_base;
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

// ── PricingExecutor::State — the persistent pool + generation barrier ────────

struct PricingExecutor::State {
  unsigned P = 0;

  // One fork-join job slot PER nesting level: job[0] is the top-level dispatch,
  // job[1] the single permitted nested dispatch. A slot per level (rather than one
  // shared slot) is what lets a nested dispatch run CONCURRENTLY with its enclosing
  // one without corrupting the outer job's bookkeeping — the two slots carry
  // independent epoch / remaining / exception state and, crucially, service DISJOINT
  // worker sets: level 1 only ever wakes workers level 0 left parked.
  struct Job {
    Trampoline fn = nullptr;
    void *closure = nullptr;
    std::size_t n = 0;
    std::size_t block = 0;
    unsigned base = 0;      // first participating worker index for this job
    unsigned active = 0;    // participating workers this dispatch
    unsigned remaining = 0; // participating workers not yet finished
    std::uint64_t epoch = 0;
    std::atomic_flag exception_captured{};
    std::exception_ptr exception{}; // single-writer winner of exception_captured
  };

  // Serializes dispatchers WITHIN a level (each level has one shared slot).
  // dispatch_mtx[0] is the original top-level serialization: the singleton is
  // documented concurrent-queryable (PortfolioPricer), so a second top-level caller
  // waits for the first's region, which also stops the two big fan-outs
  // oversubscribing the box. dispatch_mtx[1] serializes the rarer nested dispatchers
  // that share the idle-worker window. Lock order is always dispatch_mtx[0] ->
  // dispatch_mtx[1] -> m (m is innermost and released before any longer wait), and
  // workers never take dispatch_mtx[0], so no cycle is possible. E2 retires both by
  // moving to a work-stealing deque.
  std::mutex dispatch_mtx[2];

  std::mutex m;
  std::condition_variable cv_go;      // workers wait here for the next dispatch
  std::condition_variable cv_done[2]; // the caller at each level waits for completion
  std::condition_variable cv_started; // start() waits here until all workers park
  unsigned started = 0;               // workers that have reached their park loop

  Job job[2];         // job[0] = top-level, job[1] = nested (all guarded by m)
  bool stopping = false;

  std::vector<std::thread> workers;

  void start(const ExecutorConfig &cfg) {
    const PoolPlan plan = plan_pool(cfg);
    P = plan.workers;
    workers.reserve(P);
    for (unsigned w = 0; w < P; ++w) {
      const std::uint64_t mask = (w < plan.masks.size()) ? plan.masks[w] : std::uint64_t{0};
      workers.emplace_back([this, w, mask] { worker_loop(w, mask); });
      ATX_VOL_COUNT(WorkerLaunches); // thread ACTUALLY created (once, at first use)
    }
    // Startup barrier: return only once every worker has parked (seen == epoch ==
    // 0). Each worker reads `seen`, bumps `started`, and parks all while holding
    // `m`, so no dispatch can advance `epoch` before every worker is waiting —
    // otherwise a late-starting worker would read the already-advanced epoch into
    // `seen`, skip the pending job, and hang the first barrier.
    std::unique_lock<std::mutex> lk(m);
    cv_started.wait(lk, [&] { return started == P; });
  }

  ~State() {
    {
      std::lock_guard<std::mutex> lk(m);
      stopping = true;
    }
    cv_go.notify_all();
    for (std::thread &t : workers) {
      if (t.joinable()) {
        t.join();
      }
    }
  }

  void worker_loop(unsigned w, std::uint64_t affinity_mask) {
#if defined(_MSC_VER)
    if (affinity_mask != 0) {
      // Best-effort: a 0 return means the pin failed; we simply continue unpinned.
      (void)::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(affinity_mask));
    }
#else
    (void)affinity_mask;
#endif
    std::unique_lock<std::mutex> lk(m);
    std::uint64_t seen0 = job[0].epoch; // 0 — start()'s barrier holds any dispatch until parked
    std::uint64_t seen1 = job[1].epoch; // 0
    ++started;
    cv_started.notify_one();
    for (;;) {
      cv_go.wait(lk, [&] {
        return stopping || job[0].epoch != seen0 || job[1].epoch != seen1;
      });
      if (stopping) {
        return;
      }
      // A nested (level-1) generation can be published WHILE this worker runs its
      // level-0 block, so handle both slots every wake — neither can be missed, and
      // a worker not in a slot's disjoint range simply advances `seen` and re-parks.
      run_generation(lk, w, job[0], cv_done[0], seen0, /*child_depth=*/1u);
      run_generation(lk, w, job[1], cv_done[1], seen1, /*child_depth=*/2u);
    }
  }

  // Advance ONE job slot's generation on worker `w`. Entered and left holding `lk`.
  // If the slot's epoch moved since `seen`, snapshot it and — when `w` participates —
  // run this worker's contiguous block, then decrement the slot's remaining count.
  // Non-participants only advance `seen` so they re-park instead of spinning.
  void run_generation(std::unique_lock<std::mutex> &lk, unsigned w, Job &j,
                      std::condition_variable &done_cv, std::uint64_t &seen,
                      unsigned child_depth) {
    if (j.epoch == seen) {
      return;
    }
    seen = j.epoch;
    // Snapshot the job under the lock, then run without holding it.
    const Trampoline fn_ = j.fn;
    void *const cl = j.closure;
    const std::size_t n_ = j.n;
    const std::size_t blk = j.block;
    const unsigned base = j.base;
    const unsigned active = j.active;
    if (w < base || w >= base + active) {
      return; // not a participant this generation (seen already advanced)
    }
    const unsigned k = w - base;                                   // 0-based rank in this job
    const std::size_t lo = static_cast<std::size_t>(k + 1u) * blk; // block 0 is the dispatcher's
    const std::size_t hi = (lo + blk < n_) ? (lo + blk) : n_;
    // Publish this worker's nesting state so a run_*_nested inside the body sees the
    // right depth/budget. A level-0 worker (child_depth 1) exposes the idle window;
    // a level-1 worker (child_depth 2) exposes none (a further run_* inlines).
    // Restored before we retouch shared state.
    const NestState prev = t_nest;
    t_nest.depth = child_depth;
    t_nest.idle_base = (child_depth == 1u) ? active : 0u;
    t_nest.idle_budget = (child_depth == 1u) ? (P - active) : 0u;
    lk.unlock();
    if (lo < hi) {
      try {
        fn_(cl, lo, hi);
      } catch (...) {
        if (!j.exception_captured.test_and_set(std::memory_order_acq_rel)) {
          j.exception = std::current_exception();
        }
      }
    }
    t_nest = prev;
    lk.lock();
    if (--j.remaining == 0u) {
      done_cv.notify_one();
    }
  }

  // Publish a job to slot `level`, run block 0 inline on the caller, join. `base`
  // is the first worker index that services it (0 at top level; the idle-window base
  // when nested); workers [base, base+active) run blocks 1..active. The caller's
  // NestState is already advanced by run_erased / run_dynamic_erased.
  void dispatch(unsigned level, unsigned base, unsigned active, Trampoline fn_, void *cl,
                std::size_t n_, std::size_t blk) {
    Job &j = job[level];
    std::lock_guard<std::mutex> dlk(dispatch_mtx[level]); // one dispatcher per level
    {
      std::lock_guard<std::mutex> lk(m);
      j.fn = fn_;
      j.closure = cl;
      j.n = n_;
      j.block = blk;
      j.base = base;
      j.active = active;
      j.remaining = active;
      j.exception = nullptr;
      j.exception_captured.clear(std::memory_order_release);
      ++j.epoch;
    }
    cv_go.notify_all();
    ATX_VOL_COUNT(PoolDispatches); // one real pool wake (0 on the inline path)

    // Block 0 on the calling thread (t_nest already advanced by the run_* wrapper).
    std::exception_ptr caller_failure;
    const std::size_t hi0 = (blk < n_) ? blk : n_;
    try {
      fn_(cl, 0, hi0);
    } catch (...) {
      if (!j.exception_captured.test_and_set(std::memory_order_acq_rel)) {
        caller_failure = std::current_exception();
      }
    }

    std::unique_lock<std::mutex> lk(m);
    if (caller_failure != nullptr) {
      j.exception = caller_failure;
    }
    cv_done[level].wait(lk, [&] { return j.remaining == 0u; });
    std::exception_ptr failure = j.exception;
    j.fn = nullptr;
    j.closure = nullptr;
    j.n = 0u;
    j.block = 0u;
    j.base = 0u;
    j.active = 0u;
    j.exception = nullptr;
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

// Advance this thread's NestState for the duration of a dispatch's block 0. At the
// top level (level 0) the workers [0, active) are busy and [active, P) stay parked,
// forming the idle window a single nested dispatch may borrow; a nested dispatch
// (level 1) exposes no budget of its own since a further run_* would inline.
namespace {
void enter_nesting(NestState &prev, unsigned level, unsigned active, unsigned P) {
  prev = t_nest;
  t_nest.depth = prev.depth + 1u;
  t_nest.idle_base = (level == 0u) ? active : 0u;
  t_nest.idle_budget = (level == 0u) ? (P - active) : 0u;
}
} // namespace

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
  // Exception-safe save/restore of this thread's nesting state across block 0: if
  // the caller's block-0 body throws out of dispatch(), the TLS must still be reset
  // (a leaked depth would wrongly inline — or misroute — every later run_* here).
  NestState prev;
  enter_nesting(prev, plan.level, active, s.P);
  struct Restorer {
    NestState prev;
    ~Restorer() { t_nest = prev; }
  } restorer{prev};
  s.dispatch(plan.level, plan.base, active, fn, cl, n, block);
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
  } dispatch;
  dispatch.count = n;
  dispatch.fn = fn;
  dispatch.closure = cl;
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
  NestState prev;
  enter_nesting(prev, plan.level, active, s.P);
  struct Restorer {
    NestState prev;
    ~Restorer() { t_nest = prev; }
  } restorer{prev};
  // One fixed range per execution context: lo is its stable worker id in [0, nt).
  // Each context dynamically drains the shared index counter until all work is claimed.
  s.dispatch(plan.level, plan.base, active, run_context, &dispatch, nt, 1u);
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
