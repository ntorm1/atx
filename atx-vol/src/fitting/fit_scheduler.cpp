#include "fitting/fit_scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"

#if defined(_MSC_VER)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace atx::vol::detail {
namespace {

// C4 (perf): performance-core discovery + best-effort thread pinning. This
// MIRRORS the pricing executor's Topology::PerformanceCores machinery
// (pricing_executor.cpp discover_pcores + worker_loop's SetThreadAffinityMask) so
// the outer fit scheduler pins the SAME way the inner pricing pool does. Pinning
// changes only WHICH logical CPU a context runs on, never the per-index result,
// so fit outputs stay byte-identical to the unpinned path (finding 13: unpinned
// outer jthreads spill onto E-cores past the P-core count and oversubscribe them).
//
// Windows P/E discovery: RelationProcessorCore records carry an EfficiencyClass;
// the performance cores are those at the highest class (mirrors pricing_executor).
#if defined(_MSC_VER)

// Logical CPUs (group 0, up to 64) of the highest-EfficiencyClass physical cores
// when `want_performance`, or of every LOWER-class physical core when not (P3.1's
// efficiency tier). Empty on any failure -> caller falls back to the unpinned path.
// Also empty for the efficiency tier on a homogeneous host, which is the correct
// answer: there are no E-cores to spill onto, so the tier stays disengaged.
[[nodiscard]] std::vector<unsigned> discover_cores_impl(bool want_performance) {
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
  BYTE max_eff = 0;
  for (DWORD off = 0; off < len;) {
    auto *info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
    if (info->Relationship == RelationProcessorCore) {
      max_eff = (std::max)(max_eff, info->Processor.EfficiencyClass);
    }
    off += info->Size;
  }
  for (DWORD off = 0; off < len;) {
    auto *info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
    const bool wanted = info->Relationship == RelationProcessorCore &&
                        info->Processor.GroupCount >= 1 &&
                        ((info->Processor.EfficiencyClass == max_eff) == want_performance);
    if (wanted) {
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

// One-time core logical-CPU lists, discovered lazily. Empty when discovery is
// unavailable (non-Windows or an API failure) — every consumer then runs unpinned.
[[nodiscard]] const std::vector<unsigned> &core_cpus(bool want_performance) noexcept {
  static const std::vector<unsigned> pcores = []() noexcept -> std::vector<unsigned> {
#if defined(_MSC_VER)
    try {
      return discover_cores_impl(/*want_performance=*/true);
    } catch (...) {
      return std::vector<unsigned>{};
    }
#else
    return std::vector<unsigned>{};
#endif
  }();
  static const std::vector<unsigned> ecores = []() noexcept -> std::vector<unsigned> {
#if defined(_MSC_VER)
    try {
      return discover_cores_impl(/*want_performance=*/false);
    } catch (...) {
      return std::vector<unsigned>{};
    }
#else
    return std::vector<unsigned>{};
#endif
  }();
  return want_performance ? pcores : ecores;
}

[[nodiscard]] const std::vector<unsigned> &pcore_cpus() noexcept { return core_cpus(true); }
[[nodiscard]] const std::vector<unsigned> &ecore_cpus() noexcept { return core_cpus(false); }

// P3.1: the opt-in gate, read exactly once. Returns the raw first character of
// `ATX_VOL_FIT_ECORE_TIER` ('\0' when unset). Unset / "0" leaves the E-core second
// tier OFF, so the default schedule is bit- AND timing-identical to the C4
// P-core-only pin. '2' arms the tier WITHOUT the below-normal priority drop; any
// other non-zero value ('1') arms it with the drop.
//
// The two arming levels are separate because the priority drop is the part most
// likely to HURT: `run_bounded_fit_tasks` joins every worker before a date can
// drain, so one large board landing on a deprioritized E-core sets the whole date's
// makespan while being preempted by everything else on the box. Keeping them
// separable lets the tier be evaluated without conflating the two effects — and the
// measurements recorded in the P3.1 commit message show both levels regressing
// build-corpus against the unpinned default, which is why the default is OFF.
//
// Mirrors the ATX_VOL_FIT_WORKERS convention in parallel_for.hpp (same
// _dupenv_s / getenv split).
[[nodiscard]] char ecore_tier_env_value() noexcept {
  static const char value = []() noexcept -> char {
#if defined(_MSC_VER)
    char *raw = nullptr;
    std::size_t size = 0;
    if (::_dupenv_s(&raw, &size, "ATX_VOL_FIT_ECORE_TIER") != 0 || raw == nullptr) {
      return '\0';
    }
    const char first = raw[0];
    std::free(raw);
    return first;
#else
    const char *raw = std::getenv("ATX_VOL_FIT_ECORE_TIER");
    return raw == nullptr ? '\0' : raw[0];
#endif
  }();
  return value;
}

[[nodiscard]] bool ecore_tier_env_armed() noexcept {
  const char value = ecore_tier_env_value();
  return value != '\0' && value != '0';
}

// Whether armed E-core workers additionally drop to below-normal priority.
[[nodiscard]] bool ecore_tier_deprioritizes() noexcept { return ecore_tier_env_value() != '2'; }

// Pin the calling thread to `mask` (a single-bit affinity mask). Returns the
// thread's PREVIOUS affinity mask so the caller can restore it (0 on failure or
// when unavailable — best-effort, the caller simply stays wherever it was).
[[nodiscard]] std::uint64_t pin_current_thread(std::uint64_t mask) noexcept {
#if defined(_MSC_VER)
  if (mask != 0u) {
    return static_cast<std::uint64_t>(
        ::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(mask)));
  }
#else
  (void)mask;
#endif
  return 0u;
}

// P3.1: drop the calling thread to below-normal priority (best-effort, no-op when
// unavailable). Applied only to E-core-tier workers so the second tier scavenges
// idle E-core capacity instead of contending with anything running at normal
// priority. Priority steers scheduling only; it cannot change a task's result.
void deprioritize_current_thread() noexcept {
#if defined(_MSC_VER)
  (void)::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

// Restore a previously-saved affinity mask (no-op when 0 / unavailable).
void restore_current_thread(std::uint64_t saved) noexcept {
#if defined(_MSC_VER)
  if (saved != 0u) {
    (void)::SetThreadAffinityMask(::GetCurrentThread(), static_cast<DWORD_PTR>(saved));
  }
#else
  (void)saved;
#endif
}

enum class FailureKind : unsigned char {
  None,
  Status,
  Exception,
};

struct TaskFailure {
  Error error{};
  FailureKind kind{FailureKind::None};
};

static_assert(std::is_nothrow_move_assignable_v<Error>);
static_assert(std::is_nothrow_move_constructible_v<Error>);

enum class LaunchState : unsigned char {
  Waiting,
  Run,
  Abort,
};

class LaunchGate {
public:
  [[nodiscard]] bool await_run() noexcept {
    state_.wait(LaunchState::Waiting);
    return state_.load() == LaunchState::Run;
  }

  void start() noexcept {
    state_.store(LaunchState::Run);
    state_.notify_all();
  }

  void abort() noexcept {
    state_.store(LaunchState::Abort);
    state_.notify_all();
  }

private:
  std::atomic<LaunchState> state_{LaunchState::Waiting};
};

} // namespace

unsigned performance_core_count() noexcept { return static_cast<unsigned>(pcore_cpus().size()); }

unsigned efficiency_core_count() noexcept { return static_cast<unsigned>(ecore_cpus().size()); }

bool efficiency_core_tier_enabled() noexcept {
  return ecore_tier_env_armed() && !ecore_cpus().empty();
}

Status run_bounded_fit_tasks(std::size_t task_count, unsigned worker_budget,
                             const IndexedFitTask &task, FitAffinity affinity,
                             const FitSchedulerTestHooks *test_hooks) {
  if (task_count == 0u) {
    return atx::core::Ok();
  }
  if (!task) {
    return atx::core::Err(ErrorCode::InvalidArgument, "run_bounded_fit_tasks: empty task");
  }

  try {
    // R1-a (review C-06): deterministic injection point for the PRE-LAUNCH setup
    // failure the outer catch(...) below reports. Everything from here to the
    // launch gate is scratch allocation, so the real cause is std::bad_alloc —
    // which is what killed the production run twice and what a test cannot
    // provoke on demand.
    if (test_hooks != nullptr && test_hooks->before_setup) {
      test_hooks->before_setup();
    }
    const std::size_t requested =
        worker_budget != 0u ? static_cast<std::size_t>(worker_budget)
                            : std::max<std::size_t>(1u, std::thread::hardware_concurrency());
    const std::size_t worker_count = std::min(requested, task_count);
    std::vector<TaskFailure> failures(task_count);
    std::atomic_size_t next{0u};

    // C4 (perf): pin each execution context to a distinct discovered P-core logical
    // CPU when requested and discovery succeeded. `pin` is false (no pinning) on the
    // None path, on non-Windows, or when P-core discovery is empty — leaving the
    // historical unpinned behaviour byte-identical. Contexts wrap around the P-core
    // list when workers exceed P-cores. Pinning only steers scheduling; it never
    // changes which index a context runs, so per-index results are unchanged.
    const std::vector<unsigned> &pcores = pcore_cpus();
    const bool pin = (affinity == FitAffinity::PerformanceCores ||
                      affinity == FitAffinity::PerformanceThenEfficiencyCores) &&
                     !pcores.empty();
    // P3.1: the E-core second tier is engaged only for the two-tier affinity AND
    // only when the opt-in env flag is armed on a host that actually has E-cores.
    // Otherwise `ecores` is empty below and every ordinal takes the historical
    // P-core wrap — byte- and schedule-identical to FitAffinity::PerformanceCores.
    static const std::vector<unsigned> kNoCores{};
    const std::vector<unsigned> &ecores =
        (affinity == FitAffinity::PerformanceThenEfficiencyCores && efficiency_core_tier_enabled())
            ? ecore_cpus()
            : kNoCores;
    // Ordinals [0, pcores) take one P-core each. Past that, the E-tier (when
    // engaged) hands out one E-core each instead of WRAPPING back onto a P-core
    // that is already occupied; ordinals past both lists wrap the P-cores exactly
    // as before, so behaviour degrades continuously rather than falling off a
    // cliff on a host with more workers than logical CPUs.
    const auto cpu_mask = [&pcores, &ecores](std::size_t worker_ordinal) noexcept -> std::uint64_t {
      if (worker_ordinal < pcores.size()) {
        return std::uint64_t{1} << pcores[worker_ordinal];
      }
      const std::size_t spill = worker_ordinal - pcores.size();
      if (spill < ecores.size()) {
        return std::uint64_t{1} << ecores[spill];
      }
      return std::uint64_t{1} << pcores[worker_ordinal % pcores.size()];
    };
    // True for the ordinals the E-tier placed on an E-core — those and only those
    // run at below-normal priority.
    const auto on_efficiency_tier = [&pcores, &ecores](std::size_t worker_ordinal) noexcept {
      return worker_ordinal >= pcores.size() && (worker_ordinal - pcores.size()) < ecores.size();
    };

    const auto run_next = [&]() noexcept {
      for (std::size_t index = next.fetch_add(1u); index < task_count; index = next.fetch_add(1u)) {
        try {
          Status status = task(index);
          if (!status) {
            failures[index].error = std::move(status.error());
            failures[index].kind = FailureKind::Status;
          }
        } catch (...) {
          // SAFETY: the failure slot is preallocated and disjoint per index;
          // recording the compact tag performs no allocation and cannot throw.
          failures[index].kind = FailureKind::Exception;
        }
      }
    };

    // Caller acts as worker 0. Pin it to the first P-core (saving its prior
    // affinity) and restore on return so the caller's context is left as found.
    std::uint64_t caller_saved_affinity = 0u;
    if (pin) {
      caller_saved_affinity = pin_current_thread(cpu_mask(0u));
    }
    {
      // Declaration order is lifetime-critical: reverse destruction joins all
      // workers before destroying the gate they may still be observing.
      LaunchGate launch_gate;
      std::vector<std::jthread> workers;
      workers.reserve(worker_count - 1u);
      try {
        for (std::size_t worker = 1u; worker < worker_count; ++worker) {
          if (test_hooks != nullptr && test_hooks->before_worker_launch) {
            test_hooks->before_worker_launch(worker - 1u);
          }
          // Background worker `worker` pins to its own P-core (best-effort; the
          // jthread exits after run_next, so no restore is needed).
          const std::uint64_t worker_mask = pin ? cpu_mask(worker) : std::uint64_t{0};
          const bool worker_deprioritize =
              pin && on_efficiency_tier(worker) && ecore_tier_deprioritizes();
          workers.emplace_back(
              [&launch_gate, &run_next, worker_mask, worker_deprioritize]() noexcept {
            if (worker_mask != 0u) {
              (void)pin_current_thread(worker_mask);
            }
            if (worker_deprioritize) {
              deprioritize_current_thread();
            }
            if (launch_gate.await_run()) {
              run_next();
            }
          });
        }
      } catch (...) {
        // No worker has entered `task`: release every successfully-created
        // jthread down the abort path before joining it.
        launch_gate.abort();
        workers.clear();
        restore_current_thread(caller_saved_affinity);
        return atx::core::Err(ErrorCode::Internal, "run_bounded_fit_tasks: worker launch failed");
      }
      launch_gate.start();
      run_next();
    }
    restore_current_thread(caller_saved_affinity);

    for (TaskFailure &failure : failures) {
      if (failure.kind == FailureKind::Status) {
        return atx::core::Err(std::move(failure.error));
      }
      if (failure.kind == FailureKind::Exception) {
        return atx::core::Err(ErrorCode::Internal,
                              "run_bounded_fit_tasks: task threw an exception");
      }
    }
    return atx::core::Ok();
  } catch (...) {
    // Scratch allocation happens before launch on the caller. Worker-launch
    // failures use the transactional abort-and-join path above.
    return atx::core::Err(ErrorCode::Internal, "run_bounded_fit_tasks: scheduler setup failed");
  }
}

} // namespace atx::vol::detail
