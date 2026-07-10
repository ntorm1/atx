// PricingExecutor — the one process-wide persistent pricing thread pool (P1.4).
// See pricing_executor.hpp for the model, determinism argument, and sizing story.

#include "atx/vol/pricing_executor.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "atx/vol/counters.hpp"      // ATX_VOL_COUNT (opt-in; no-op when OFF)
#include "atx/vol/parallel_for.hpp"  // atx_auto_worker_count (shared core budget)

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

// True while THIS thread is executing executor work — a pool worker (set for the
// worker's whole life) or the caller running its own block 0. A run_* issued from
// within executor work runs fully inline (the load-bearing nested guard): a pool
// worker must never block waiting on the pool it is running inside.
thread_local bool t_in_executor = false;

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
    if (info->Relationship == RelationProcessorCore &&
        info->Processor.EfficiencyClass == max_eff && info->Processor.GroupCount >= 1) {
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

  // Serializes TOP-LEVEL dispatchers: the pool has one shared job slot, and the
  // process singleton can be driven by several threads at once (PortfolioPricer is
  // documented concurrent-queryable). A second top-level caller waits here for the
  // first's parallel region to finish, then runs its own — which also avoids the
  // two big fan-outs oversubscribing the box. Nested calls never reach here: the
  // thread-local guard runs them inline.
  std::mutex dispatch_mtx;

  std::mutex m;
  std::condition_variable cv_go;      // workers wait here for the next dispatch
  std::condition_variable cv_done;    // the caller waits here for completion
  std::condition_variable cv_started; // start() waits here until all workers park
  unsigned started = 0;               // workers that have reached their park loop

  // Current job (all guarded by m).
  Trampoline fn = nullptr;
  void *closure = nullptr;
  std::size_t n = 0;
  std::size_t block = 0;
  unsigned active = 0;    // participating workers this dispatch (== nt - 1)
  unsigned remaining = 0; // participating workers not yet finished
  std::uint64_t epoch = 0;
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
    t_in_executor = true; // a pool worker runs executor work for its whole life
#if defined(_MSC_VER)
    if (affinity_mask != 0) {
      // Best-effort: a 0 return means the pin failed; we simply continue unpinned.
      (void)::SetThreadAffinityMask(::GetCurrentThread(),
                                    static_cast<DWORD_PTR>(affinity_mask));
    }
#else
    (void)affinity_mask;
#endif
    std::unique_lock<std::mutex> lk(m);
    std::uint64_t seen = epoch; // 0 — start()'s barrier keeps any dispatch until parked
    ++started;
    cv_started.notify_one();
    for (;;) {
      cv_go.wait(lk, [&] { return stopping || epoch != seen; });
      if (stopping) {
        return;
      }
      seen = epoch;
      // Snapshot the job under the lock, then run without holding it.
      const Trampoline fn_ = fn;
      void *const cl = closure;
      const std::size_t n_ = n;
      const std::size_t blk = block;
      const unsigned A = active;
      lk.unlock();
      const bool participates = (w < A);
      if (participates) {
        const std::size_t lo = static_cast<std::size_t>(w + 1) * blk;
        const std::size_t hi = (lo + blk < n_) ? (lo + blk) : n_;
        if (lo < hi) {
          fn_(cl, lo, hi);
        }
      }
      lk.lock();
      if (participates) {
        if (--remaining == 0) {
          cv_done.notify_one();
        }
      }
      // seen == epoch now, so the next wait blocks until the following dispatch.
    }
  }

  // Dispatch blocks 1..A to the pool, run block 0 inline on the caller, join.
  void dispatch(Trampoline fn_, void *cl, std::size_t n_, std::size_t blk, unsigned A) {
    std::lock_guard<std::mutex> dlk(dispatch_mtx); // one top-level dispatcher at a time
    {
      std::lock_guard<std::mutex> lk(m);
      fn = fn_;
      closure = cl;
      n = n_;
      block = blk;
      active = A;
      remaining = A;
      ++epoch;
    }
    cv_go.notify_all();
    ATX_VOL_COUNT(PoolDispatches); // one real pool wake (0 on the inline path)

    // Block 0 on the calling thread (t_in_executor is already set by run_erased).
    const std::size_t hi0 = (blk < n_) ? blk : n_;
    fn_(cl, 0, hi0);

    std::unique_lock<std::mutex> lk(m);
    cv_done.wait(lk, [&] { return remaining == 0; });
  }
};

// ── PricingExecutor ──────────────────────────────────────────────────────────

PricingExecutor::PricingExecutor(const ExecutorConfig &cfg) : state_(std::make_unique<State>()) {
  state_->start(cfg);
}

PricingExecutor::PricingExecutor() : PricingExecutor(ExecutorConfig{}) {}

PricingExecutor::~PricingExecutor() = default;

unsigned PricingExecutor::size() const noexcept { return state_->P; }

void PricingExecutor::run_erased(std::size_t n, unsigned n_threads, Trampoline fn, void *cl) {
  if (n == 0) {
    return;
  }
  State &s = *state_;
  const unsigned P = s.P;
  unsigned nt = (n_threads == 0) ? (P + 1u) : n_threads;
  if (nt > P + 1u) {
    nt = P + 1u; // clamp DOWN to the pool size: never grow the pool
  }
  if (static_cast<std::size_t>(nt) > n) {
    nt = static_cast<unsigned>(n);
  }
  // Fully inline when there is nothing to gain (single block, sub-threshold) or
  // when re-entered from within executor work (the nested-parallelism guard).
  if (nt <= 1u || n < kInlineThreshold || t_in_executor) {
    fn(cl, 0, n);
    return;
  }
  const std::size_t block = (n + nt - 1u) / nt;
  // Exception-safe restore of the nested-parallelism guard: if the caller's
  // block-0 body throws out of dispatch(), the thread-local flag must still be
  // reset (unreachable on today's non-throwing bodies, but a real latent footgun
  // — a leaked `true` would wrongly inline every subsequent run_* on this thread).
  const bool prev = t_in_executor;
  struct FlagRestorer {
    bool prev;
    ~FlagRestorer() { t_in_executor = prev; }
  } restorer{prev};
  t_in_executor = true;
  s.dispatch(fn, cl, n, block, nt - 1u);
}

// ── Process singleton + one-time configuration ───────────────────────────────

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
