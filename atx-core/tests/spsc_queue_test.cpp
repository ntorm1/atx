// spsc_queue_test.cpp — tests for the wait-free SPSC ring buffer.

#include <atx/core/concurrent/spsc_queue.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <thread>

using atx::core::concurrent::SpscQueue;

TEST(SpscQueue, SingleThreadFifo) {
    SpscQueue<int, 8> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    int v = 0;
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 2);
    EXPECT_FALSE(q.try_pop(v)); // now empty
}

TEST(SpscQueue, FillsToCapacityThenRejects) {
    SpscQueue<int, 4> q;
    EXPECT_EQ(q.capacity(), 4U);
    EXPECT_TRUE(q.try_push(10));
    EXPECT_TRUE(q.try_push(20));
    EXPECT_TRUE(q.try_push(30));
    EXPECT_TRUE(q.try_push(40));
    EXPECT_EQ(q.size(), 4U);
    EXPECT_FALSE(q.try_push(50)); // full
    int v = 0;
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 10);
    EXPECT_TRUE(q.try_push(50)); // slot freed
}

TEST(SpscQueue, EmptyPopFails) {
    SpscQueue<int, 8> q;
    int v = 0;
    EXPECT_FALSE(q.try_pop(v));
    EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, WrapsAroundManyTimes) {
    SpscQueue<int, 4> q;
    for (int round = 0; round < 1000; ++round) {
        EXPECT_TRUE(q.try_push(round));
        int v = -1;
        EXPECT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, round);
    }
    EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, MoveOnlyType) {
    SpscQueue<std::unique_ptr<int>, 8> q;
    EXPECT_TRUE(q.try_push(std::make_unique<int>(42)));
    std::unique_ptr<int> p;
    EXPECT_TRUE(q.try_pop(p));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 42);
}

// The headline guarantee: every item crosses the queue exactly once and in
// order, with the producer and consumer on separate threads.
TEST(SpscQueue, ProducerConsumerAllItems) {
    SpscQueue<int, 1024> q;
    constexpr int N = 1'000'000;

    std::thread prod([&] {
        for (int i = 0; i < N;) {
            if (q.try_push(i)) {
                ++i;
            }
        }
    });

    // Windows is LLP64: `long` is 32-bit, so accumulate in long long (the sum
    // of 0..N-1 is ~5e11 and would overflow a 32-bit long).
    long long sum = 0;
    for (int i = 0; i < N;) {
        int v = 0;
        if (q.try_pop(v)) {
            EXPECT_EQ(v, i); // FIFO order preserved
            sum += v;
            ++i;
        }
    }
    prod.join();

    EXPECT_EQ(sum, static_cast<long long>(N - 1) * N / 2);
}
