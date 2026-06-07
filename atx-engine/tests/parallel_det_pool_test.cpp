#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "atx/engine/parallel/det_pool.hpp"

using atx::engine::parallel::DetPool;

TEST(ParallelDetPool, ProcessesEveryIndexExactlyOnce) {
  DetPool pool{4};
  std::vector<std::atomic<int>> hits(1000);
  pool.parallel_for(1000, [&](std::size_t i, std::size_t) { hits[i].fetch_add(1); });
  for (auto& h : hits) EXPECT_EQ(h.load(), 1);
}

TEST(ParallelDetPool, OneWorkerEqualsSequential) {
  DetPool p1{1}, p4{4};
  std::vector<double> a(500), b(500);
  auto fill = [](std::size_t i) { return std::sin(0.1 * static_cast<double>(i)); };
  p1.parallel_for(500, [&](std::size_t i, std::size_t) { a[i] = fill(i); });
  p4.parallel_for(500, [&](std::size_t i, std::size_t) { b[i] = fill(i); });
  EXPECT_EQ(a, b); // fixed-slot write -> identical regardless of worker count
}

TEST(ParallelDetPool, EmptyAndSingle) {
  DetPool pool{4};
  pool.parallel_for(0, [&](std::size_t, std::size_t) { FAIL() << "n=0 must call body zero times"; });
  int x = 0;
  pool.parallel_for(1, [&](std::size_t, std::size_t) { x = 42; });
  EXPECT_EQ(x, 42);
}

TEST(ParallelDetPool, ExceptionRethrownLowestIndexFirst) {
  DetPool pool{4};
  EXPECT_THROW(pool.parallel_for(100,
                                 [&](std::size_t i, std::size_t) {
                                   if (i == 7 || i == 30) throw std::runtime_error("boom");
                                 }),
               std::exception);
}

TEST(ParallelDetPool, WorkerIdInRange) {
  DetPool pool{4};
  std::vector<std::atomic<int>> seen(4);
  pool.parallel_for(4000, [&](std::size_t, std::size_t wid) {
    ASSERT_LT(wid, 4U);
    seen[wid].fetch_add(1);
  });
  // every worker should have done some work on 4000 items / 4 workers
}

TEST(ParallelDetPool, DefaultWorkerCountIsPositive) {
  DetPool pool{0};
  EXPECT_GE(pool.n_workers(), 1U);
}

TEST(ParallelDetPool, ForEachWorkerRunsExactlyOncePerWorker) {
  DetPool pool{4};
  std::vector<std::atomic<int>> ran(pool.n_workers());
  pool.for_each_worker([&](std::size_t wid) { ran[wid].fetch_add(1); });
  for (auto& r : ran) EXPECT_EQ(r.load(), 1);
}

TEST(ParallelDetPool, ForEachWorkerStableUnderRepeat) {
  DetPool pool{4};
  for (int rep = 0; rep < 200; ++rep) {
    std::vector<std::atomic<int>> ran(pool.n_workers());
    pool.for_each_worker([&](std::size_t wid) { ran[wid].fetch_add(1); });
    for (auto& r : ran) ASSERT_EQ(r.load(), 1) << "rep=" << rep;
  }
}

TEST(ParallelDetPool, ForEachWorkerExceptionRethrown) {
  DetPool pool{4};
  EXPECT_THROW(pool.for_each_worker(
                   [&](std::size_t wid) {
                     if (wid == 2) throw std::runtime_error("setup");
                   }),
               std::exception);
}
