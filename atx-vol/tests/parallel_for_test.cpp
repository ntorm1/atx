#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "atx/vol/detail/parallel_for.hpp"  // atx_auto_worker_count, parallel_for

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

// ── Elastic AUTO budget: direct gates for detail::run_elastic_dynamic ────────
//
// rev2-ws-t N-I1. `ScopedElasticWorkerBudget` + `atx_resolve_fanout_workers` +
// `detail::run_elastic_dynamic` are ~200 lines of new concurrency in a SHARED
// header included by eleven library TUs across four workstreams, and until these
// tests they were exercised only indirectly, through corpus_test's build
// fixtures. Two things in particular had no coverage at all:
//
//   * the elastic exception path is NOT the tested fixed-width path. The calling
//     thread `break`s out of its claim loop and leaves the workers it already
//     spawned to keep draining `next`, then rethrows after the join. All three
//     `…ExceptionPropagatesToCaller` tests above pass an EXPLICIT count, which
//     bypasses the elastic path entirely, so none of them ever entered it.
//   * the load-bearing safety claim that an explicit non-zero count bypasses the
//     elastic path. That claim is the only thing keeping `essvi_calib`'s
//     `scratch[nt]` in bounds when a pool grows underneath a caller.
//
// Everything below is deterministic: the ramp is driven by the resolver's own
// call count and by a latch on worker identity, never by sleeping or by wall
// clock. The one timed construct is a bounded `wait_until` whose ONLY purpose is
// to turn a regression that stops the pool growing into a failure instead of a
// hang; no assertion reads it.

namespace {

// A stub elastic resolver that RAMPS one step per call: 1, 2, 3, … cap, cap, …
// `run_elastic_dynamic` asks its resolver once at entry and again after every
// task the CALLING thread completes, so a ramping stub reproduces exactly the
// shape the corpus reclaim sees as its outer pool drains — with no corpus, no
// fit and no clock.
struct RampBudget {
  std::atomic<unsigned> calls{0};
  unsigned cap = 4u;
};

unsigned ramp_resolver(void* raw) noexcept {
  auto* ctx = static_cast<RampBudget*>(raw);
  const unsigned k = ctx->calls.fetch_add(1, std::memory_order_relaxed) + 1u;
  return k < ctx->cap ? k : ctx->cap;
}

// A resolver that answers one fixed (deliberately large) width and RECORDS
// whether it was consulted at all. Used to prove the paths that must never ask.
struct FixedBudget {
  std::atomic<unsigned> calls{0};
  unsigned width = 8u;
};

unsigned fixed_resolver(void* raw) noexcept {
  auto* ctx = static_cast<FixedBudget*>(raw);
  ctx->calls.fetch_add(1, std::memory_order_relaxed);
  return ctx->width;
}

std::size_t count_distinct(std::vector<std::thread::id> ids) {
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  return ids.size();
}

}  // namespace

// (a) The pool WIDTH moves while the fan-out is still running — the whole point
// of the elastic path, and the property whose loss T-I1 was about. The latch
// makes the ramp a fact rather than a race: every SPAWNED worker parks on its
// first task until all `cap` worker ids have been seen, and worker 0 parks once
// it has completed `cap` tasks, so no thread can drain the queue out from under
// the ramp. If the mid-flight `spawn_up_to` (parallel_for.hpp: the reclaim, after
// each of worker 0's tasks) is ever removed, entry sizing alone answers 1 and no
// worker id above 0 is ever handed out.
TEST(ParallelForElastic, GrowsThePoolWhileTheFanOutIsStillRunning) {
  constexpr std::size_t kN = 64;
  constexpr unsigned kCap = 4u;
  constexpr unsigned kFullMask = (1u << kCap) - 1u;

  RampBudget budget;
  budget.cap = kCap;

  std::mutex mu;
  std::condition_variable cv;
  unsigned seen_mask = 0u;
  std::vector<unsigned> owner(kN, kCap + 99u);
  std::vector<std::thread::id> ran_on(kN);
  std::atomic<std::size_t> invocations{0};
  std::atomic<unsigned> worker0_tasks{0};

  {
    const atx::vol::ScopedElasticWorkerBudget elastic{&ramp_resolver, &budget};
    atx::vol::parallel_for_dynamic(kN, 0u, [&](std::size_t i, unsigned wid) {
      owner[i] = wid;
      ran_on[i] = std::this_thread::get_id();
      invocations.fetch_add(1, std::memory_order_relaxed);

      std::unique_lock<std::mutex> lk(mu);
      if (wid < 32u) {
        seen_mask |= (1u << wid);
      }
      cv.notify_all();
      if (wid == 0u &&
          worker0_tasks.fetch_add(1, std::memory_order_relaxed) + 1u != kCap) {
        return;  // worker 0 is the only thread that can widen the pool: never stall it early
      }
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      cv.wait_until(lk, deadline, [&] { return seen_mask == kFullMask; });
    });
  }

  EXPECT_EQ(invocations.load(), kN);
  EXPECT_EQ(seen_mask, kFullMask)
      << "the pool never reached its resolved width while the fan-out was running: "
       "seen worker-id mask 0x"
      << std::hex << seen_mask << std::dec;
  unsigned max_wid = 0u;
  for (std::size_t i = 0; i < kN; ++i) {
    ASSERT_LT(owner[i], kCap) << "index " << i << " ran on worker id " << owner[i]
                              << ", outside the dense [0, " << kCap << ") contract";
    max_wid = owner[i] > max_wid ? owner[i] : max_wid;
  }
  EXPECT_EQ(max_wid, kCap - 1u)
      << "the elastic pool never grew past width " << (max_wid + 1u)
      << ": the budget is frozen at fan-out ENTRY instead of being re-asked between tasks";
  EXPECT_GT(budget.calls.load(), 1u) << "the resolver was asked only at entry";
  EXPECT_EQ(count_distinct(ran_on), static_cast<std::size_t>(kCap))
      << "one OS thread per worker id, no more: the pool must not oversubscribe its budget";
}

// (b) The elastic exception path. `break` on the calling thread leaves the
// already-spawned workers draining, so this asserts BOTH halves of that contract:
// the first exception reaches the caller (never std::terminate), and every index
// was still claimed and invoked exactly once before the call returned — which is
// also the evidence that every spawned jthread was joined before the rethrow,
// i.e. nothing is left running or leaked.
//
// cap = 2 makes the throw deterministic: entry resolves to 1, so no worker exists
// yet and the calling thread necessarily claims index 0; that task is benign and
// the resolver's second answer (2) spawns worker 1; the calling thread's SECOND
// task then throws with worker 1 alive and draining.
TEST(ParallelForElastic, WorkerExceptionPropagatesAfterTheGrownPoolDrains) {
  constexpr std::size_t kN = 64;
  RampBudget budget;
  budget.cap = 2u;

  std::vector<std::atomic<unsigned>> claimed(kN);
  std::atomic<std::size_t> invocations{0};
  std::atomic<unsigned> worker0_tasks{0};
  bool threw = false;

  try {
    const atx::vol::ScopedElasticWorkerBudget elastic{&ramp_resolver, &budget};
    atx::vol::parallel_for_dynamic(kN, 0u, [&](std::size_t i, unsigned wid) {
      claimed[i].fetch_add(1, std::memory_order_relaxed);
      invocations.fetch_add(1, std::memory_order_relaxed);
      if (wid == 0u && worker0_tasks.fetch_add(1, std::memory_order_relaxed) + 1u == 2u) {
        throw std::runtime_error("elastic worker boom");
      }
    });
  } catch (const std::runtime_error& e) {
    threw = true;
    EXPECT_STREQ(e.what(), "elastic worker boom");
  }

  EXPECT_TRUE(threw) << "a throw on the elastic path must propagate to the caller, "
                        "not be swallowed by the calling thread's break and not terminate";
  EXPECT_EQ(invocations.load(), kN)
      << "the workers spawned before the throw must drain the remaining indices and be joined "
         "before the rethrow";
  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(claimed[i].load(), 1u) << "index " << i << " was claimed " << claimed[i].load()
                                     << " times, not exactly once";
  }
  EXPECT_GE(budget.calls.load(), 2u);
}

// (c) An EXPLICIT non-zero worker count must be honored verbatim and must never
// consult the elastic resolver, on any of the three fan-out overloads. This is
// the contract that keeps a caller which pre-sized `scratch[nt]` from being
// handed a worker id past the end of its array once the pool grows
// (essvi_calib.cpp's per-worker scratch is the live instance).
TEST(ParallelForElastic, ExplicitWorkerCountBypassesTheElasticPath) {
  constexpr std::size_t kN = 257;
  constexpr unsigned kExplicit = 3u;
  FixedBudget budget;  // would answer 8 if it were ever asked
  budget.width = 8u;

  std::vector<unsigned> owner(kN, 99u);
  std::atomic<std::size_t> dyn_hits{0};
  std::atomic<std::size_t> static_hits{0};

  {
    const atx::vol::ScopedElasticWorkerBudget elastic{&fixed_resolver, &budget};
    atx::vol::parallel_for_dynamic(kN, kExplicit,
                                   [&](std::size_t i, unsigned wid) { owner[i] = wid; });
    atx::vol::parallel_for_dynamic(kN, kExplicit,
                                   [&](std::size_t) { dyn_hits.fetch_add(1, std::memory_order_relaxed); });
    atx::vol::parallel_for(kN, kExplicit,
                           [&](std::size_t) { static_hits.fetch_add(1, std::memory_order_relaxed); });
  }

  EXPECT_EQ(budget.calls.load(), 0u)
      << "an explicit worker count consulted the elastic resolver: a caller that pre-sized "
         "per-worker scratch from its own count can now be handed a larger pool";
  EXPECT_EQ(dyn_hits.load(), kN);
  EXPECT_EQ(static_hits.load(), kN);
  for (std::size_t i = 0; i < kN; ++i) {
    ASSERT_LT(owner[i], kExplicit)
        << "index " << i << " ran on worker id " << owner[i]
        << ", past the end of a scratch array sized from the explicit count " << kExplicit;
  }
}

// (d) The H^2 guard. A fan-out reached from INSIDE an elastic fan-out must
// resolve AUTO to 1 — exactly what the old `fit_workers = 1` pin gave — so two
// nested levels cannot multiply into hardware_concurrency^2 runnable threads.
// The guard is installed both on the spawned workers and on the calling thread
// around each of its own tasks, so this must hold from every worker id; the same
// latch as (a) guarantees all `cap` ids actually run a task and therefore that
// both installs are exercised.
//
// The DISCRIMINATOR is the outer resolver's call count, not thread identity. A
// thread-identity check cannot separate "the guard held" from "the caller
// drained the inner fan-out before the spawned thread got scheduled". The call
// count can: `run_elastic_dynamic` asks its resolver exactly once at entry and
// once after every task the calling thread completes, so with the guard in place
// the outer budget must be consulted exactly `1 + (tasks run by worker 0)` times
// — a nested fan-out that reached the OUTER resolver instead of the serial one
// shows up as a surplus, whoever ran it and however the threads interleaved.
TEST(ParallelForElastic, NestedAutoFanOutResolvesToSerial) {
  constexpr std::size_t kN = 16;
  constexpr std::size_t kInner = 8;
  constexpr unsigned kCap = 4u;
  constexpr unsigned kFullMask = (1u << kCap) - 1u;

  RampBudget budget;
  budget.cap = kCap;

  std::mutex mu;
  std::condition_variable cv;
  unsigned seen_mask = 0u;
  std::vector<int> inner_off_thread(kN, -1);
  std::vector<std::size_t> inner_ran(kN, 0);
  std::atomic<unsigned> worker0_tasks{0};

  {
    const atx::vol::ScopedElasticWorkerBudget elastic{&ramp_resolver, &budget};
    atx::vol::parallel_for_dynamic(kN, 0u, [&](std::size_t i, unsigned wid) {
      const unsigned worker0_seq =
          wid == 0u ? worker0_tasks.fetch_add(1, std::memory_order_relaxed) + 1u : 0u;

      const auto outer_id = std::this_thread::get_id();
      std::atomic<int> off{0};
      std::atomic<std::size_t> ran{0};
      atx::vol::parallel_for_dynamic(kInner, 0u, [&](std::size_t) {
        ran.fetch_add(1, std::memory_order_relaxed);
        if (std::this_thread::get_id() != outer_id) {
          off.fetch_add(1, std::memory_order_relaxed);
        }
      });
      inner_off_thread[i] = off.load(std::memory_order_relaxed);
      inner_ran[i] = ran.load(std::memory_order_relaxed);

      std::unique_lock<std::mutex> lk(mu);
      if (wid < 32u) {
        seen_mask |= (1u << wid);
      }
      cv.notify_all();
      if (wid == 0u && worker0_seq != kCap) {
        return;
      }
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      cv.wait_until(lk, deadline, [&] { return seen_mask == kFullMask; });
    });
  }

  EXPECT_EQ(seen_mask, kFullMask)
      << "not every worker id ran a task, so the guard was not exercised on both installs";
  EXPECT_EQ(budget.calls.load(), worker0_tasks.load() + 1u)
      << "the outer elastic budget was consulted " << budget.calls.load() << " times for "
      << worker0_tasks.load()
      << " calling-thread tasks (expected tasks+1): a fan-out nested inside an elastic task "
         "reached the OUTER resolver instead of resolving AUTO to 1 — the H^2 guard is gone and "
         "two nested levels now multiply";
  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(inner_ran[i], kInner) << "inner fan-out " << i << " did not visit every slot";
    EXPECT_EQ(inner_off_thread[i], 0)
        << "the AUTO fan-out nested inside elastic task " << i << " ran " << inner_off_thread[i]
        << " slots on OTHER threads";
  }
}

// (e) Degenerate sizes under an elastic budget: n == 0 must not even ask, n == 1
// must stay on the calling thread, and a budget wider than the task count must
// still hand out DENSE ids inside [0, n).
TEST(ParallelForElastic, DegenerateSizesStayInsideTheWorkerIdContract) {
  FixedBudget budget;
  budget.width = 64u;  // deliberately far wider than any n below
  const atx::vol::ScopedElasticWorkerBudget elastic{&fixed_resolver, &budget};

  std::atomic<int> ran{0};
  atx::vol::parallel_for_dynamic(std::size_t{0}, 0u,
                                 [&](std::size_t, unsigned) { ran.fetch_add(1, std::memory_order_relaxed); });
  EXPECT_EQ(ran.load(), 0);
  EXPECT_EQ(budget.calls.load(), 0u) << "an empty fan-out must not consult the resolver at all";

  unsigned lone_wid = 99u;
  const auto caller_id = std::this_thread::get_id();
  std::thread::id lone_thread{};
  atx::vol::parallel_for_dynamic(std::size_t{1}, 0u, [&](std::size_t, unsigned wid) {
    lone_wid = wid;
    lone_thread = std::this_thread::get_id();
  });
  EXPECT_EQ(lone_wid, 0u);
  EXPECT_EQ(lone_thread, caller_id) << "a one-task fan-out must not spawn a thread";

  // want (64) > n (3). `spawn_up_to` clamps `want` to `n`, so exactly n threads
  // exist — the caller plus n-1 spawned — and the ids handed out are exactly
  // {0 … n-1}. The latch is what makes that a MEASUREMENT rather than a race:
  // every task parks until all n have been claimed, so the calling thread cannot
  // simply drain the queue before the spawned threads are scheduled and thereby
  // hide extra workers. With the clamp gone, 63 threads contend for 3 slots and
  // the ids that win are drawn from [0, 64), not [0, 3).
  constexpr std::size_t kSmall = 3;
  std::mutex mu;
  std::condition_variable cv;
  std::size_t claimed = 0;
  std::vector<unsigned> owner(kSmall, 99u);
  std::vector<std::thread::id> ran_on(kSmall);
  atx::vol::parallel_for_dynamic(kSmall, 0u, [&](std::size_t i, unsigned wid) {
    owner[i] = wid;
    ran_on[i] = std::this_thread::get_id();
    std::unique_lock<std::mutex> lk(mu);
    ++claimed;
    cv.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    cv.wait_until(lk, deadline, [&] { return claimed == kSmall; });
  });

  std::vector<unsigned> ids = owner;
  std::sort(ids.begin(), ids.end());
  for (std::size_t i = 0; i < kSmall; ++i) {
    EXPECT_EQ(ids[i], static_cast<unsigned>(i))
        << "worker ids " << ids[0] << "," << ids[1] << "," << ids[2]
        << " are not the dense set [0, " << kSmall << ") the contract promises when the resolved "
           "budget (" << budget.width << ") exceeds the task count";
  }
  EXPECT_EQ(count_distinct(ran_on), kSmall)
      << "one OS thread per worker id, and no more threads than tasks";
}
