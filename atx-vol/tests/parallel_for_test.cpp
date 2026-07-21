#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "atx/vol/parallel_for.hpp"  // atx_auto_worker_count, parallel_for

// S0-4': `atx_auto_worker_count()` resolves the auto (0) worker count for
// `parallel_for` -- ATX_VOL_FIT_WORKERS if set to a positive integer, else
// std::thread::hardware_concurrency() (>=1). This is a pure env-parsing unit
// test; it never touches a fit. See curve_fit_parallel_test.cpp for the
// (optional) cross-check that the env cap changes only performance, never
// the fitted result.

namespace {

#if defined(_MSC_VER)
void set_env(const char* value) { ::_putenv_s("ATX_VOL_FIT_WORKERS", value); }
void unset_env() { ::_putenv_s("ATX_VOL_FIT_WORKERS", ""); }
#else
void set_env(const char* value) { ::setenv("ATX_VOL_FIT_WORKERS", value, 1); }
void unset_env() { ::unsetenv("ATX_VOL_FIT_WORKERS"); }
#endif

}  // namespace

TEST(ParallelFor, AutoWorkerCountHonorsEnvCap) {
  // Save the prior value (if any) so this test cannot leak state into any
  // other test that happens to read ATX_VOL_FIT_WORKERS.
#if defined(_MSC_VER)
  char* prev = nullptr;
  std::size_t prev_n = 0;
  const bool had_prev = (::_dupenv_s(&prev, &prev_n, "ATX_VOL_FIT_WORKERS") == 0) &&
                         (prev != nullptr);
  const std::string prev_val = had_prev ? std::string(prev) : std::string();
  if (prev != nullptr) {
    std::free(prev);
  }
#else
  const char* prev_c = std::getenv("ATX_VOL_FIT_WORKERS");
  const bool had_prev = prev_c != nullptr;
  const std::string prev_val = had_prev ? std::string(prev_c) : std::string();
#endif

  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0u) {
    hw = 1u;
  }

  set_env("1");
  EXPECT_EQ(atx::vol::atx_auto_worker_count(), 1u);

  set_env("3");
  EXPECT_EQ(atx::vol::atx_auto_worker_count(), 3u);

  // "0" is not a positive integer -- falls back to hardware_concurrency.
  set_env("0");
  EXPECT_EQ(atx::vol::atx_auto_worker_count(), hw);

  // Non-numeric -- falls back.
  set_env("banana");
  EXPECT_EQ(atx::vol::atx_auto_worker_count(), hw);

  // Unset -- falls back.
  unset_env();
  const unsigned resolved = atx::vol::atx_auto_worker_count();
  EXPECT_EQ(resolved, hw);
  EXPECT_GE(resolved, 1u);

  // Restore whatever was there before this test ran.
  if (had_prev) {
    set_env(prev_val.c_str());
  } else {
    unset_env();
  }
}

TEST(ParallelFor, DynamicSchedulerVisitsEverySlotExactlyOnce) {
  constexpr std::size_t kN = 257;
  std::vector<std::size_t> got(kN, kN);
  atx::vol::parallel_for_dynamic(kN, 7, [&](std::size_t i) { got[i] = i * i; });
  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(got[i], i * i);
  }
}

// A7 (simd-review finding 3): the STATIC block-partitioned parallel_for owns the
// same throughput hot paths as the dynamic overloads; its multithreaded path must
// preserve the disjoint-write determinism contract. Sanity-check the happy path
// under nt > 1 before exercising the exception behaviour below.
TEST(ParallelFor, StaticSchedulerWritesEverySlotExactlyOnce) {
  constexpr std::size_t kN = 257;
  std::vector<std::size_t> got(kN, kN);
  atx::vol::parallel_for(kN, 7, [&](std::size_t i) { got[i] = i * i; });
  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(got[i], i * i);
  }
}

// A7 (simd-review finding 3): a throwing worker body in the STATIC block
// scheduler must NOT std::terminate the process. Pre-fix the static variant had
// no try/catch in its jthread bodies, so an exception escaping `fn` (e.g.
// std::bad_alloc) escaped the jthread and called std::terminate — it would have
// taken the whole test process down rather than failing cleanly, so this test
// could not have been written as a plain EXPECT before the fix. Post-fix the
// static variant matches the dynamic overloads: it captures the first exception,
// joins every worker, and rethrows on the calling thread so the caller's normal
// error handling applies. nt >= 2 forces the multithreaded jthread path (the
// bug's blast radius). The schedule is contiguous [lo,hi) blocks — with kN=64,
// nt=4 (chunk 16) index 17 lands in worker 1's block and is always processed, so
// the throw is deterministic.
TEST(ParallelFor, StaticWorkerExceptionPropagatesToCaller) {
  constexpr std::size_t kN = 64;
  bool threw = false;
  try {
    atx::vol::parallel_for(kN, 4, [](std::size_t i) {
      if (i == 17) {
        throw std::runtime_error("static worker boom");
      }
    });
  } catch (const std::runtime_error& e) {
    threw = true;
    EXPECT_STREQ(e.what(), "static worker boom");
  }
  EXPECT_TRUE(threw) << "a throwing static worker must propagate, not terminate";
}

// WP7: a worker body that throws must NOT std::terminate the process. The
// dynamic fan-out captures the first exception, joins every worker, and
// rethrows on the calling thread so the caller's normal error handling applies.
// nt >= 2 forces the multithreaded jthread path (the bug's blast radius); the
// dynamic scheduler visits every slot exactly once, so index 17 is always
// processed and the throw is deterministic.
TEST(ParallelFor, DynamicWorkerExceptionPropagatesToCaller) {
  constexpr std::size_t kN = 64;
  bool threw = false;
  try {
    atx::vol::parallel_for_dynamic(kN, 4, [](std::size_t i) {
      if (i == 17) {
        throw std::runtime_error("worker boom");
      }
    });
  } catch (const std::runtime_error& e) {
    threw = true;
    EXPECT_STREQ(e.what(), "worker boom");
  }
  EXPECT_TRUE(threw) << "a throwing worker must propagate, not terminate";
}

// Same contract for the worker-id overload (fn(index, worker_id)).
TEST(ParallelFor, DynamicWorkerIdExceptionPropagatesToCaller) {
  constexpr std::size_t kN = 64;
  std::atomic<int> ran{0};
  bool threw = false;
  try {
    atx::vol::parallel_for_dynamic(kN, 4, [&](std::size_t i, unsigned worker_id) {
      (void)worker_id;
      ran.fetch_add(1, std::memory_order_relaxed);
      if (i == 42) {
        throw std::runtime_error("worker-id boom");
      }
    });
  } catch (const std::runtime_error& e) {
    threw = true;
    EXPECT_STREQ(e.what(), "worker-id boom");
  }
  EXPECT_TRUE(threw) << "a throwing worker-id worker must propagate, not terminate";
}
