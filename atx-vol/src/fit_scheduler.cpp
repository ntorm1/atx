#include "atx/vol/detail/fit_scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::vol::detail {
namespace {

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

Status run_bounded_fit_tasks(std::size_t task_count, unsigned worker_budget,
                             const IndexedFitTask &task, const FitSchedulerTestHooks *test_hooks) {
  if (task_count == 0u) {
    return atx::core::Ok();
  }
  if (!task) {
    return atx::core::Err(ErrorCode::InvalidArgument, "run_bounded_fit_tasks: empty task");
  }

  try {
    const std::size_t requested =
        worker_budget != 0u ? static_cast<std::size_t>(worker_budget)
                            : std::max<std::size_t>(1u, std::thread::hardware_concurrency());
    const std::size_t worker_count = std::min(requested, task_count);
    std::vector<TaskFailure> failures(task_count);
    std::atomic_size_t next{0u};

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
          workers.emplace_back([&launch_gate, &run_next]() noexcept {
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
        return atx::core::Err(ErrorCode::Internal, "run_bounded_fit_tasks: worker launch failed");
      }
      launch_gate.start();
      run_next();
    }

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
