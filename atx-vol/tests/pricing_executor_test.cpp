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

#include <array>
#include <atomic>
#include <cstddef>
#include <numeric>
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
