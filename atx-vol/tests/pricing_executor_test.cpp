// pricing_executor_test.cpp — the persistent pricing pool (P1.4) contract.
//
// Direct unit tests on PricingExecutor: contiguous-disjoint coverage at every
// worker count, the nested-parallelism guard (a run_* issued from inside pool work
// runs inline instead of deadlocking), the inline-threshold path (0 pool dispatches
// below the crossover), and a repeated concurrent stress that would surface a
// data race on the disjoint-write design as nondeterminism.
//
// NOTE: clang-cl/Windows has no usable ThreadSanitizer, so bit-identity under
// repeated concurrent runs (here + the determinism guards in
// portfolio_pricer_test.cpp) is the correctness evidence for races; a TSan pass is
// deferred to CI. The dispatch/inline assertions are only meaningful under
// -DATX_VOL_COUNTERS=ON and fold to a disabled-sentinel check otherwise.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <vector>

#include "atx/vol/counters.hpp"
#include "atx/vol/pricing_executor.hpp"

using namespace atx::vol;

namespace {

// run_blocks writes every index in [0, n) exactly once, for any worker request.
TEST(PricingExecutor, RunBlocks_CoversEveryIndexOnce) {
  PricingExecutor ex;
  for (unsigned nt : {0u, 1u, 2u, 4u, 8u, 16u, 64u}) {
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{3}, std::size_t{5},
                          std::size_t{100}, std::size_t{1000}}) {
      std::vector<int> hits(n, 0);
      ex.run_blocks(n, nt, [&](std::size_t i) { hits[i] += 1; });
      for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(hits[i], 1) << "nt=" << nt << " n=" << n << " i=" << i;
      }
    }
  }
}

// run_ranges calls body(lo, hi) over contiguous disjoint ranges that tile [0, n).
TEST(PricingExecutor, RunRanges_TilesContiguousDisjoint) {
  PricingExecutor ex;
  for (unsigned nt : {0u, 1u, 2u, 4u, 8u, 64u}) {
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{257}}) {
      std::vector<int> hits(n, 0);
      std::atomic<std::size_t> covered{0};
      ex.run_ranges(n, nt, [&](std::size_t lo, std::size_t hi) {
        covered.fetch_add(hi - lo, std::memory_order_relaxed);
        for (std::size_t i = lo; i < hi; ++i) {
          hits[i] += 1;
        }
      });
      EXPECT_EQ(covered.load(), n) << "nt=" << nt << " n=" << n;
      for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(hits[i], 1) << "nt=" << nt << " n=" << n << " i=" << i;
      }
    }
  }
}

// Dynamic dispatch assigns each index exactly once while exposing one stable
// worker id per participating execution context. Explicit requests and auto (0)
// are both clamped to the fixed pool and the number of work items.
TEST(PricingExecutor, RunDynamic_CoversEveryIndexOnceWithStableWorkerIds) {
  PricingExecutor ex;
  for (const unsigned requested : {0u, 1u, 2u, 4u, 8u, 64u}) {
    constexpr std::size_t n = 257u;
    const unsigned requested_or_pool = requested == 0u ? ex.size() + 1u : requested;
    const unsigned expected_contexts = static_cast<unsigned>(
        std::min<std::size_t>(n, std::min(requested_or_pool, ex.size() + 1u)));
    std::vector<std::atomic<unsigned>> hits(n);
    std::vector<std::thread::id> thread_by_worker(expected_contexts);
    std::mutex thread_mutex;
    std::atomic<bool> unstable_worker_id{false};

    ex.run_dynamic(n, requested, [&](std::size_t index, unsigned worker_id) {
      hits[index].fetch_add(1u, std::memory_order_relaxed);
      if (worker_id >= expected_contexts) {
        unstable_worker_id.store(true, std::memory_order_relaxed);
        return;
      }
      const std::thread::id current = std::this_thread::get_id();
      std::lock_guard lock{thread_mutex};
      if (thread_by_worker[worker_id] == std::thread::id{}) {
        thread_by_worker[worker_id] = current;
      } else if (thread_by_worker[worker_id] != current) {
        unstable_worker_id.store(true, std::memory_order_relaxed);
      }
    });

    EXPECT_FALSE(unstable_worker_id.load(std::memory_order_relaxed)) << "requested=" << requested;
    for (std::size_t i = 0; i < n; ++i) {
      EXPECT_EQ(hits[i].load(std::memory_order_relaxed), 1u)
          << "requested=" << requested << " i=" << i;
    }
  }
}

TEST(PricingExecutor, RunDynamic_NestedDispatchRunsInlineWithWorkerIdZero) {
  PricingExecutor ex;
  constexpr std::size_t rows = 64u;
  constexpr std::size_t cols = 17u;
  std::vector<std::atomic<unsigned>> hits(rows * cols);
  std::atomic<bool> nonzero_nested_worker{false};

  ex.run_dynamic(rows, 8u, [&](std::size_t row, unsigned) {
    ex.run_dynamic(cols, 8u, [&](std::size_t col, unsigned nested_worker) {
      if (nested_worker != 0u) {
        nonzero_nested_worker.store(true, std::memory_order_relaxed);
      }
      hits[row * cols + col].fetch_add(1u, std::memory_order_relaxed);
    });
  });

  EXPECT_FALSE(nonzero_nested_worker.load(std::memory_order_relaxed));
  for (std::size_t i = 0; i < hits.size(); ++i) {
    EXPECT_EQ(hits[i].load(std::memory_order_relaxed), 1u) << i;
  }
}

// A caller-block exception must not unwind past the stack-backed closure until
// a participating worker has finished. The same executor must remain reusable.
TEST(PricingExecutor, CallerExceptionWaitsForWorkersAndPoolRemainsReusable) {
  PricingExecutor ex;
  if (ex.size() == 0u) {
    GTEST_SKIP() << "single-context executor cannot exercise a worker barrier";
  }
  const unsigned contexts = std::min(4u, ex.size() + 1u);
  constexpr std::size_t n = 128u;
  const std::size_t block = (n + contexts - 1u) / contexts;
  std::mutex mutex;
  std::condition_variable cv;
  bool worker_started = false;
  bool release_worker = false;
  bool worker_finished = false;

  EXPECT_THROW(ex.run_blocks(n, contexts,
                             [&](std::size_t i) {
                               if (i == block) {
                                 std::unique_lock lock{mutex};
                                 worker_started = true;
                                 cv.notify_all();
                                 cv.wait(lock, [&] { return release_worker; });
                                 worker_finished = true;
                                 return;
                               }
                               if (i == 0u) {
                                 std::unique_lock lock{mutex};
                                 cv.wait(lock, [&] { return worker_started; });
                                 release_worker = true;
                                 cv.notify_all();
                                 throw std::runtime_error{"caller failure"};
                               }
                             }),
               std::runtime_error);
  EXPECT_TRUE(worker_finished);

  std::vector<unsigned> reused(n, 0u);
  ex.run_blocks(n, contexts, [&](std::size_t i) { reused[i] = 1u; });
  EXPECT_EQ(std::accumulate(reused.begin(), reused.end(), 0u), n);
}

TEST(PricingExecutor, WorkerExceptionIsRethrownAfterCallerFinishesAndPoolRemainsReusable) {
  PricingExecutor ex;
  if (ex.size() == 0u) {
    GTEST_SKIP() << "single-context executor cannot exercise a worker exception";
  }
  const unsigned contexts = std::min(4u, ex.size() + 1u);
  constexpr std::size_t n = 128u;
  const std::size_t block = (n + contexts - 1u) / contexts;
  std::mutex mutex;
  std::condition_variable cv;
  bool caller_started = false;
  bool worker_threw = false;
  bool caller_finished = false;

  EXPECT_THROW(ex.run_blocks(n, contexts,
                             [&](std::size_t i) {
                               if (i == 0u) {
                                 std::unique_lock lock{mutex};
                                 caller_started = true;
                                 cv.notify_all();
                                 cv.wait(lock, [&] { return worker_threw; });
                                 caller_finished = true;
                                 return;
                               }
                               if (i == block) {
                                 std::unique_lock lock{mutex};
                                 cv.wait(lock, [&] { return caller_started; });
                                 worker_threw = true;
                                 cv.notify_all();
                                 throw std::runtime_error{"worker failure"};
                               }
                             }),
               std::runtime_error);
  EXPECT_TRUE(caller_finished);

  std::atomic<std::size_t> reused{0u};
  ex.run_dynamic(n, contexts,
                 [&](std::size_t, unsigned) { reused.fetch_add(1u, std::memory_order_relaxed); });
  EXPECT_EQ(reused.load(std::memory_order_relaxed), n);
}

TEST(PricingExecutor, DynamicExceptionIsRethrownAndPoolRemainsReusable) {
  PricingExecutor ex;
  constexpr std::size_t n = 257u;
  std::atomic<bool> threw{false};
  EXPECT_THROW(ex.run_dynamic(n, 8u,
                              [&](std::size_t index, unsigned) {
                                if (index == 31u &&
                                    !threw.exchange(true, std::memory_order_relaxed)) {
                                  throw std::runtime_error{"dynamic failure"};
                                }
                              }),
               std::runtime_error);

  std::vector<std::atomic<unsigned>> hits(n);
  ex.run_dynamic(n, 8u, [&](std::size_t index, unsigned) {
    hits[index].fetch_add(1u, std::memory_order_relaxed);
  });
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(hits[i].load(std::memory_order_relaxed), 1u) << i;
  }
}

// Concurrent top-level clients share one executor job slot. One failing client
// must release dispatch_mtx after its barrier/rethrow so the other clients finish,
// and the pool must remain reusable afterward.
TEST(PricingExecutor, ConcurrentTopLevelCallersSerializeAcrossFailureAndRemainReusable) {
  PricingExecutor ex;
  constexpr unsigned callers = 4u;
  constexpr std::size_t n = 257u;
  std::vector<std::atomic<unsigned>> hits(callers * n);
  std::atomic<unsigned> ready{0u};
  std::atomic<bool> go{false};
  std::atomic<unsigned> caught{0u};
  std::vector<std::jthread> clients;
  clients.reserve(callers);
  for (unsigned caller = 0u; caller < callers; ++caller) {
    clients.emplace_back([&, caller] {
      ready.fetch_add(1u, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      try {
        if (caller == 0u) {
          ex.run_dynamic(n, 8u, [](std::size_t index, unsigned) {
            if (index == 31u) {
              throw std::runtime_error{"concurrent failure"};
            }
          });
        } else {
          ex.run_blocks(n, 8u, [&](std::size_t index) {
            hits[static_cast<std::size_t>(caller) * n + index].fetch_add(1u,
                                                                         std::memory_order_relaxed);
          });
        }
      } catch (const std::runtime_error &) {
        caught.fetch_add(1u, std::memory_order_relaxed);
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != callers) {
    std::this_thread::yield();
  }
  go.store(true, std::memory_order_release);
  clients.clear(); // jthread destructors join every concurrent caller

  EXPECT_EQ(caught.load(std::memory_order_relaxed), 1u);
  for (unsigned caller = 1u; caller < callers; ++caller) {
    for (std::size_t index = 0u; index < n; ++index) {
      EXPECT_EQ(hits[static_cast<std::size_t>(caller) * n + index].load(std::memory_order_relaxed),
                1u)
          << "caller=" << caller << " index=" << index;
    }
  }

  std::atomic<std::size_t> reused{0u};
  ex.run_dynamic(n, 8u,
                 [&](std::size_t, unsigned) { reused.fetch_add(1u, std::memory_order_relaxed); });
  EXPECT_EQ(reused.load(std::memory_order_relaxed), n);
}

// The nested guard: a run_ranges whose body itself calls run_blocks must complete
// (the inner call runs inline on the pool worker) with correct disjoint results —
// no deadlock, no hang, even when the outer dispatch occupies the whole pool.
TEST(PricingExecutor, NestedRunExecutesInlineNoDeadlock) {
  PricingExecutor ex;
  constexpr std::size_t R = 96;
  constexpr std::size_t C = 40;
  std::vector<int> grid(R * C, 0);
  ex.run_ranges(R, 8, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t r = lo; r < hi; ++r) {
      // Issued from within a pool worker (or the caller's block 0): runs inline.
      ex.run_blocks(C, 8, [&](std::size_t c) { grid[r * C + c] += 1; });
    }
  });
  for (std::size_t k = 0; k < R * C; ++k) {
    EXPECT_EQ(grid[k], 1) << k;
  }
}

// Below the inline threshold: 0 pool dispatches (fully inline), and the output is
// identical to the above-threshold dispatched path's per-index formula.
TEST(PricingExecutor, InlineThreshold_NoDispatchBelowCrossover) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;
  PricingExecutor &ex = pricing_executor(); // the singleton owns the global counter

  // Warm the pool (creates workers once; irrelevant to the dispatch count below).
  {
    std::vector<int> warm(2048, 0);
    ex.run_blocks(warm.size(), 8, [&](std::size_t i) { warm[i] = 1; });
  }

  if constexpr (counters_enabled()) {
    // n = 3 sits below kInlineThreshold (=4): fully inline, no dispatch.
    atx::vol::counters::reset();
    std::array<int, 3> small{};
    ex.run_blocks(small.size(), 8, [&](std::size_t i) { small[i] = static_cast<int>(i) + 1; });
    EXPECT_EQ(atx::vol::counters::snapshot().get(Counter::PoolDispatches), 0u);

    // A large n dispatches (only when the pool actually has workers) and produces
    // the identical per-index result.
    atx::vol::counters::reset();
    std::vector<int> big(512, 0);
    ex.run_blocks(big.size(), 8, [&](std::size_t i) { big[i] = static_cast<int>(i) + 1; });
    if (ex.size() > 0) {
      EXPECT_GT(atx::vol::counters::snapshot().get(Counter::PoolDispatches), 0u);
    }
    for (std::size_t i = 0; i < small.size(); ++i) {
      EXPECT_EQ(small[i], static_cast<int>(i) + 1);
    }
    for (std::size_t i = 0; i < big.size(); ++i) {
      EXPECT_EQ(big[i], static_cast<int>(i) + 1);
    }
  } else {
    EXPECT_FALSE(atx::vol::counters::snapshot().enabled);
    SUCCEED();
  }
}

// Repeated concurrent stress: the same disjoint-write reduction, run many times at
// several worker counts, must yield the identical result every time. A race on the
// disjoint partition would surface here as a nondeterministic sum.
TEST(PricingExecutor, RepeatedConcurrent_DeterministicDisjointWrites) {
  PricingExecutor ex;
  constexpr std::size_t n = 4096;
  std::vector<std::uint64_t> ref(n);
  std::iota(ref.begin(), ref.end(), std::uint64_t{1});

  // 20 reps: enough to surface disjoint-write races; full soak was 200 (moved to bench-scale).
  for (int rep = 0; rep < 20; ++rep) {
    for (unsigned nt : {1u, 2u, 4u, 8u}) {
      std::vector<std::uint64_t> out(n, 0);
      ex.run_blocks(n, nt, [&](std::size_t i) { out[i] = static_cast<std::uint64_t>(i) + 1; });
      ASSERT_EQ(out, ref) << "rep=" << rep << " nt=" << nt;
    }
  }
}

} // namespace
