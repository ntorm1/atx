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
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <numeric>
#include <set>
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

// Concurrent top-level clients each run an INDEPENDENT job through the shared
// work-stealing queue (E2 retired the single per-level slot + dispatch_mtx). This
// verifies the property that outlived that refactor: one client's failure is
// ISOLATED to its own job (exactly one caught exception), every other client still
// writes its full disjoint result, and the pool remains reusable. (Name kept for
// history; the callers no longer serialize on a shared slot.)
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

// ── E1: explicit nested-budget dispatch (run_*_nested) ───────────────────────
//
// The plain run_* above keep inlining when nested (the two guard tests earlier).
// These exercise the opt-in second level: a dispatch issued from INSIDE a dispatch
// that borrows the outer's idle workers. They prove (a) no deadlock, (b) results
// are bit-identical for every outer/inner worker count, (c) the nested level really
// reaches a pool worker, and (d) the graceful fallback to inline when no worker is
// idle. If any of these hung the ctest timeout would fire (the deadlock guard).

// nested_budget() is 0 at top level and equals the idle window (P - active_outer)
// seen from within an outer dispatch, then restored to 0 on the way out.
TEST(PricingExecutor, NestedBudget_ReflectsIdleWindowAndRestores) {
  PricingExecutor ex;
  EXPECT_EQ(ex.nested_budget(), 0u); // top level: no enclosing dispatch
  if (ex.size() == 0u) {
    GTEST_SKIP() << "single-context executor never dispatches, so never nests";
  }
  std::atomic<unsigned> budget_in_body{~0u};
  // Two outer contexts => active_outer == 1 => idle window == P - 1.
  ex.run_ranges(4u, 2u, [&](std::size_t, std::size_t) {
    budget_in_body.store(ex.nested_budget(), std::memory_order_relaxed);
  });
  EXPECT_EQ(budget_in_body.load(std::memory_order_relaxed), ex.size() - 1u);
  EXPECT_EQ(ex.nested_budget(), 0u); // restored after the dispatch returns
}

// The keystone deadlock proof: nest a dispatch inside a dispatch (both levels the
// nested API) and join. Every cell written exactly once; completion == no deadlock.
TEST(PricingExecutor, NestedDispatch_NoDeadlockAndCoversEveryCellOnce) {
  PricingExecutor ex;
  constexpr std::size_t R = 96;
  constexpr std::size_t C = 40;
  std::vector<int> grid(R * C, 0);
  ex.run_ranges_nested(R, 8u, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t r = lo; r < hi; ++r) {
      // Issued from within an outer dispatch's body: dispatches onto the idle
      // window when one exists, else inlines — either way, disjoint correct writes.
      ex.run_blocks_nested(C, 8u, [&](std::size_t c) { grid[r * C + c] += 1; });
    }
  });
  for (std::size_t k = 0; k < R * C; ++k) {
    EXPECT_EQ(grid[k], 1) << k;
  }
}

// Determinism gate: the nested scatter must be byte-identical to the serial
// reference for EVERY outer/inner worker-count pair — the block partition never
// moves which context writes a cell, so the result cannot depend on thread count.
TEST(PricingExecutor, NestedDispatch_DeterministicAcrossWorkerCounts) {
  PricingExecutor ex;
  constexpr std::size_t R = 24;
  constexpr std::size_t C = 100;
  const auto cell = [](std::size_t r, std::size_t c) -> std::uint64_t {
    return (r + 1u) * 1000003ull + (c + 1u) * 97ull; // unique per (r, c)
  };
  std::vector<std::uint64_t> ref(R * C);
  for (std::size_t r = 0; r < R; ++r) {
    for (std::size_t c = 0; c < C; ++c) {
      ref[r * C + c] = cell(r, c);
    }
  }
  for (unsigned outer_nt : {1u, 2u, 3u, 4u, 8u, 16u}) {
    for (unsigned inner_nt : {0u, 1u, 2u, 3u, 5u, 8u, 16u}) {
      std::vector<std::uint64_t> out(R * C, 0u);
      ex.run_ranges(R, outer_nt, [&](std::size_t lo, std::size_t hi) {
        for (std::size_t r = lo; r < hi; ++r) {
          ex.run_blocks_nested(C, inner_nt,
                               [&](std::size_t c) { out[r * C + c] = cell(r, c); });
        }
      });
      ASSERT_EQ(out, ref) << "outer_nt=" << outer_nt << " inner_nt=" << inner_nt;
    }
  }
}

// Proof the nested level ACTUALLY parallelizes: with an idle window available, the
// static-partition nested dispatch runs on more than one thread (a parked pool
// worker helps), and — under -DATX_VOL_COUNTERS=ON — wakes the pool beyond the
// single outer dispatch (PoolDispatches >= 2).
TEST(PricingExecutor, NestedDispatch_ReachesAPoolWorkerAndWakesThePool) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;
  if (pricing_executor().size() < 2u) {
    GTEST_SKIP() << "needs >= 2 pool workers to leave an idle window for nesting";
  }
  PricingExecutor &ex = pricing_executor();
  constexpr std::size_t R = 2;
  constexpr std::size_t C = 4096;
  std::vector<int> grid(R * C, 0);
  std::mutex mu;
  std::vector<std::thread::id> nested_threads;

  const auto run = [&] {
    ex.run_ranges(R, 2u, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t r = lo; r < hi; ++r) {
        // Outer left P-1 workers parked; a nested block-partition dispatch gives a
        // non-empty block to each, so at least one runs on a different thread.
        ex.run_ranges_nested(C, 0u, [&](std::size_t clo, std::size_t chi) {
          for (std::size_t c = clo; c < chi; ++c) {
            grid[r * C + c] += 1;
          }
          std::lock_guard<std::mutex> lk(mu);
          nested_threads.push_back(std::this_thread::get_id());
        });
      }
    });
  };

  if constexpr (counters_enabled()) {
    atx::vol::counters::reset();
    run();
    const std::uint64_t dispatches =
        atx::vol::counters::snapshot().get(Counter::PoolDispatches);
    EXPECT_GE(dispatches, 2u)
        << "one outer + >= 1 nested pool wake expected; got " << dispatches;
  } else {
    run();
  }

  for (std::size_t k = 0; k < R * C; ++k) {
    EXPECT_EQ(grid[k], 1) << k;
  }
  std::sort(nested_threads.begin(), nested_threads.end());
  nested_threads.erase(std::unique(nested_threads.begin(), nested_threads.end()),
                       nested_threads.end());
  EXPECT_GE(nested_threads.size(), 2u)
      << "nested dispatch did not reach a parked pool worker";
}

// Fallback: when the outer dispatch claims the WHOLE pool (n_threads=0), the idle
// window is empty, so every nested dispatch inlines. Must still complete + be
// correct — proves the budget math cannot self-oversubscribe into a deadlock.
TEST(PricingExecutor, NestedDispatch_UnderFullPoolInlinesSafely) {
  PricingExecutor ex;
  constexpr std::size_t R = 64;
  constexpr std::size_t C = 40;
  std::vector<int> grid(R * C, 0);
  ex.run_ranges_nested(R, 0u, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t r = lo; r < hi; ++r) {
      ex.run_blocks_nested(C, 0u, [&](std::size_t c) { grid[r * C + c] += 1; });
    }
  });
  for (std::size_t k = 0; k < R * C; ++k) {
    EXPECT_EQ(grid[k], 1) << k;
  }
}

// run_dynamic_nested keeps a stable worker_id in [0, resolved_threads) per context
// even when nested, so nested per-worker scratch is safe. Every index handled once.
TEST(PricingExecutor, NestedDynamic_StableWorkerIdsCoverEveryIndexOnce) {
  PricingExecutor ex;
  constexpr std::size_t R = 8;
  constexpr std::size_t C = 257;
  std::vector<std::atomic<unsigned>> hits(R * C);
  std::atomic<bool> worker_id_out_of_range{false};
  ex.run_ranges(R, 4u, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t r = lo; r < hi; ++r) {
      const unsigned budget = ex.nested_budget();
      const unsigned contexts = budget + 1u; // caller + idle window
      ex.run_dynamic_nested(C, 0u, [&](std::size_t c, unsigned worker_id) {
        hits[r * C + c].fetch_add(1u, std::memory_order_relaxed);
        if (worker_id >= contexts) {
          worker_id_out_of_range.store(true, std::memory_order_relaxed);
        }
      });
    }
  });
  EXPECT_FALSE(worker_id_out_of_range.load(std::memory_order_relaxed));
  for (std::size_t k = 0; k < R * C; ++k) {
    EXPECT_EQ(hits[k].load(std::memory_order_relaxed), 1u) << k;
  }
}

// ── E2: unified work-stealing scheduler (retires the per-level dispatch_mtx) ──
//
// E2 replaced E1's two fixed job slots + single-slot dispatch_mtx serialization
// with one shared task queue drained HELP-FIRST. These tests pin the new contract:
//   (a) determinism — a nested (outer -> inner) reduction is byte-identical across
//       worker counts {1,2,4,8}, even run from many concurrent external threads;
//   (b) deadlock-freedom — cross-dependent dispatchers that DEADLOCKED under the
//       old single-slot design now make progress (guarded by an in-body timeout so
//       a regression fails cleanly instead of hanging the suite);
//   (c) PoolDispatches counts outer + every nested pool wake;
//   (d) an EXTERNAL-outer (bounded-queue style) caller's slices run CONCURRENTLY on
//       the pool (no serialization) and reach pool workers.

namespace {
// Generous cap on every cross-thread rendezvous below: on the correct scheduler the
// counterpart arrives in microseconds, so this only elapses on a genuine deadlock —
// the wait_for then returns false, the body records it, and the dispatch unwinds so
// the test fails cleanly rather than hanging (and hanging the whole ctest process).
constexpr std::chrono::seconds kRendezvousTimeout{20};
} // namespace

// (a) Determinism across worker counts {1,2,4,8}, under CONCURRENT external drivers.
// The block partition fixes which context owns a cell; the work-stealing queue only
// moves which thread runs it, so every driver must reproduce the serial reference
// byte-for-byte for every (outer, inner) worker-count pair.
TEST(PricingExecutor, E2_ConcurrentNestedDeterministicAcrossWorkerCounts) {
  PricingExecutor ex;
  constexpr std::size_t R = 16;
  constexpr std::size_t C = 64;
  const auto cell = [](std::size_t r, std::size_t c) -> std::uint64_t {
    return (r + 1u) * 2654435761ull ^ ((c + 1u) * 40503ull); // unique per (r, c)
  };
  std::vector<std::uint64_t> ref(R * C);
  for (std::size_t r = 0; r < R; ++r) {
    for (std::size_t c = 0; c < C; ++c) {
      ref[r * C + c] = cell(r, c);
    }
  }
  for (unsigned outer_nt : {1u, 2u, 4u, 8u}) {
    for (unsigned inner_nt : {1u, 2u, 4u, 8u}) {
      constexpr unsigned T = 4; // concurrent external drivers sharing one pool
      std::vector<std::vector<std::uint64_t>> outs(T, std::vector<std::uint64_t>(R * C, 0u));
      std::vector<std::thread> drivers;
      drivers.reserve(T);
      for (unsigned t = 0; t < T; ++t) {
        drivers.emplace_back([&, t] {
          ex.run_ranges(R, outer_nt, [&](std::size_t lo, std::size_t hi) {
            for (std::size_t r = lo; r < hi; ++r) {
              ex.run_blocks_nested(C, inner_nt,
                                   [&](std::size_t c) { outs[t][r * C + c] = cell(r, c); });
            }
          });
        });
      }
      for (std::thread &d : drivers) {
        d.join();
      }
      for (unsigned t = 0; t < T; ++t) {
        ASSERT_EQ(outs[t], ref) << "outer_nt=" << outer_nt << " inner_nt=" << inner_nt << " t=" << t;
      }
    }
  }
}

// (b) Deadlock-freedom, TOP LEVEL: two external callers whose block-0 bodies depend
// on each other. Under E1's dispatch_mtx[0] the second caller blocked acquiring the
// single slot the first holds for its whole call (block-0 included) -> neither flag
// is ever set -> deadlock. The work-stealing scheduler gives each its own job, so
// both make progress. A regression trips the in-body timeout and fails here.
TEST(PricingExecutor, E2_ConcurrentTopLevelDispatchersDoNotDeadlock) {
  PricingExecutor ex;
  if (ex.size() == 0u) {
    GTEST_SKIP() << "single-context executor cannot exercise concurrent dispatchers";
  }
  constexpr std::size_t n = 64;
  std::mutex mu;
  std::condition_variable cv;
  bool a_started = false;
  bool b_started = false;
  bool a_timed_out = false;
  bool b_timed_out = false;
  std::vector<int> ga(n, 0);
  std::vector<int> gb(n, 0);

  std::thread ta([&] {
    ex.run_ranges(n, 2u, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) {
        ga[i] += 1;
      }
      if (lo == 0u) { // block 0 on caller A's own thread
        std::unique_lock<std::mutex> lk(mu);
        a_started = true;
        cv.notify_all();
        if (!cv.wait_for(lk, kRendezvousTimeout, [&] { return b_started; })) {
          a_timed_out = true;
        }
      }
    });
  });
  std::thread tb([&] {
    ex.run_ranges(n, 2u, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) {
        gb[i] += 1;
      }
      if (lo == 0u) { // block 0 on caller B's own thread
        std::unique_lock<std::mutex> lk(mu);
        b_started = true;
        cv.notify_all();
        if (!cv.wait_for(lk, kRendezvousTimeout, [&] { return a_started; })) {
          b_timed_out = true;
        }
      }
    });
  });
  ta.join();
  tb.join();

  EXPECT_FALSE(a_timed_out) << "caller A never saw B start — dispatchers serialized";
  EXPECT_FALSE(b_timed_out) << "caller B never saw A start — dispatchers serialized";
  for (std::size_t i = 0; i < n; ++i) {
    EXPECT_EQ(ga[i], 1) << i;
    EXPECT_EQ(gb[i], 1) << i;
  }
}

// (b) Deadlock-freedom, NESTED: within ONE outer dispatch two concurrent contexts
// (context 0 on the dispatcher, context 1 on a worker) each issue a nested dispatch
// whose block-0 bodies depend on each other. Under E1's dispatch_mtx[1] the worker's
// nested dispatch blocked on the single nested slot the dispatcher holds while it
// waits on the worker -> deadlock. Help-first draining breaks it. Needs >= 2 pool
// workers so both nested dispatches actually reach the pool (idle window >= 1). The
// outer spans R=4 rows (>= kInlineThreshold, so it truly dispatches): with nt=2,
// context 0 owns rows [0,2) (row 0 waits) and context 1 owns rows [2,4) (row 2 sets).
TEST(PricingExecutor, E2_NestedCrossDependentDispatchesMakeProgress) {
  PricingExecutor ex;
  if (ex.size() < 2u) {
    GTEST_SKIP() << "needs >= 2 pool workers for both nested dispatches to reach the pool";
  }
  constexpr std::size_t R = 4;
  constexpr std::size_t C = 256;
  std::vector<int> grid(R * C, 0);
  std::mutex mu;
  std::condition_variable cv;
  bool setter_ran = false;
  bool waiter_timed_out = false;

  ex.run_ranges_nested(R, 2u, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t r = lo; r < hi; ++r) {
      const bool waiter = (r == 0u); // row 0 (context 0, dispatcher) waits
      const bool setter = (r == 2u); // row 2 (context 1, a worker) sets
      ex.run_blocks_nested(C, 0u, [&, waiter, setter](std::size_t c) {
        grid[r * C + c] += 1;
        if (c == 0u && setter) {
          std::unique_lock<std::mutex> lk(mu);
          setter_ran = true;
          cv.notify_all();
        } else if (c == 0u && waiter) {
          std::unique_lock<std::mutex> lk(mu);
          if (!cv.wait_for(lk, kRendezvousTimeout, [&] { return setter_ran; })) {
            waiter_timed_out = true;
          }
        }
      });
    }
  });

  EXPECT_FALSE(waiter_timed_out) << "nested dispatches serialized on a single slot";
  for (std::size_t k = 0; k < R * C; ++k) {
    EXPECT_EQ(grid[k], 1) << k;
  }
}

// (c) PoolDispatches counts the outer wake plus every nested pool wake. The outer
// spans R=4 rows (>= kInlineThreshold, so it truly dispatches: +1) and each row
// issues one nested dispatch that actually dispatches (idle window >= 1 with >= 2
// workers: +4), so >= 1 + 2 real wakes are recorded under nesting.
TEST(PricingExecutor, E2_PoolDispatchesCountsOuterPlusNestedWakes) {
  using atx::vol::counters::Counter;
  using atx::vol::counters::counters_enabled;
  if (pricing_executor().size() < 2u) {
    GTEST_SKIP() << "needs >= 2 pool workers to leave an idle window for nesting";
  }
  PricingExecutor &ex = pricing_executor(); // singleton owns the global counter
  constexpr std::size_t R = 4;
  constexpr std::size_t C = 256;
  const auto run = [&] {
    std::vector<int> grid(R * C, 0);
    ex.run_ranges(R, 2u, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t r = lo; r < hi; ++r) {
        ex.run_blocks_nested(C, 0u, [&](std::size_t c) { grid[r * C + c] += 1; });
      }
    });
    for (std::size_t k = 0; k < R * C; ++k) {
      EXPECT_EQ(grid[k], 1) << k;
    }
  };

  if constexpr (counters_enabled()) {
    { // warm the pool so worker launches don't perturb anything measured
      std::vector<int> warm(4096, 0);
      ex.run_blocks(warm.size(), 8u, [&](std::size_t i) { warm[i] = 1; });
    }
    atx::vol::counters::reset();
    run();
    const std::uint64_t dispatches = atx::vol::counters::snapshot().get(Counter::PoolDispatches);
    EXPECT_GE(dispatches, 3u) << "expected >= 1 outer + >= 2 nested pool wakes; got " << dispatches;
  } else {
    run();
  }
}

// (d) External-outer concurrency: several external (bounded-queue style) callers each
// drive a pricing slice. Their block-0 contexts rendezvous at a K-way barrier that
// can ONLY complete if all K slices are in flight at once — under E1's dispatch_mtx[0]
// they ran one-at-a-time and the barrier timed out. It also records that the slices'
// non-zero blocks reached pool worker threads (each slice uses > 1 worker).
TEST(PricingExecutor, E2_ExternalOuterSlicesRunConcurrentlyAndUseWorkers) {
  PricingExecutor ex;
  if (ex.size() == 0u) {
    GTEST_SKIP() << "single-context executor has no pool workers to share";
  }
  constexpr unsigned K = 3;      // external bounded-queue-style callers
  constexpr std::size_t n = 512; // >= pool so non-zero blocks land on workers
  std::mutex mu;
  std::condition_variable cv;
  unsigned at_barrier = 0;
  bool released = false;
  bool barrier_timed_out = false;
  std::mutex tmu;
  std::set<std::thread::id> worker_block_threads;
  const std::thread::id nil{};

  const auto slice = [&] {
    ex.run_ranges(n, 0u, [&](std::size_t lo, std::size_t hi) {
      if (lo == 0u) { // block 0 runs on this external caller thread: rendezvous
        std::unique_lock<std::mutex> lk(mu);
        if (++at_barrier == K) {
          released = true;
          cv.notify_all();
        } else if (!cv.wait_for(lk, kRendezvousTimeout, [&] { return released; })) {
          barrier_timed_out = true;
        }
      } else { // a non-zero block: dispatched to a pool worker
        std::lock_guard<std::mutex> lk(tmu);
        worker_block_threads.insert(std::this_thread::get_id());
      }
      for (std::size_t i = lo; i < hi; ++i) {
        (void)i;
      }
    });
  };

  std::vector<std::thread> callers;
  callers.reserve(K);
  for (unsigned k = 0; k < K; ++k) {
    callers.emplace_back(slice);
  }
  for (std::thread &c : callers) {
    c.join();
  }

  EXPECT_FALSE(barrier_timed_out)
      << "external-outer slices serialized — the pool served them one at a time";
  EXPECT_FALSE(worker_block_threads.count(nil));
  EXPECT_GE(worker_block_threads.size(), 1u)
      << "external-outer slices never reached a pool worker";
}

} // namespace
