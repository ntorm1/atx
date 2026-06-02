// mpmc_queue_test.cpp — tests for the Vyukov bounded MPMC queue.

#include <atx/core/concurrent/mpmc_queue.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

using atx::core::concurrent::MpmcQueue;

namespace {

// Move-only element with a live-instance counter, used to prove the destructor
// drains and destroys leftover elements with no leak or double-free.
struct Counted {
    inline static std::atomic<int> live{0};
    int v;
    explicit Counted(int x) : v{x} { live.fetch_add(1); }
    Counted(Counted&& o) noexcept : v{o.v} { live.fetch_add(1); }
    Counted& operator=(Counted&& o) noexcept {
        v = o.v;
        return *this;
    }
    Counted(const Counted&)            = delete;
    Counted& operator=(const Counted&) = delete;
    ~Counted() { live.fetch_sub(1); }
};

} // namespace

TEST(MpmcQueue, SingleThreadFifo) {
    MpmcQueue<int, 8> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    int v = 0;
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 2);
    EXPECT_FALSE(q.try_pop(v));
}

TEST(MpmcQueue, FillsToCapacityThenRejects) {
    MpmcQueue<int, 4> q;
    EXPECT_EQ(q.capacity(), 4U);
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    EXPECT_TRUE(q.try_push(4));
    EXPECT_FALSE(q.try_push(5)); // full
    int v = 0;
    EXPECT_TRUE(q.try_pop(v));
    EXPECT_EQ(v, 1);
    EXPECT_TRUE(q.try_push(5)); // slot freed
}

TEST(MpmcQueue, WrapsAroundManyTimes) {
    MpmcQueue<int, 4> q;
    for (int round = 0; round < 1000; ++round) {
        EXPECT_TRUE(q.try_push(round));
        int v = -1;
        EXPECT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, round);
    }
}

TEST(MpmcQueue, MoveOnlyType) {
    MpmcQueue<std::unique_ptr<int>, 8> q;
    EXPECT_TRUE(q.try_push(std::make_unique<int>(7)));
    std::unique_ptr<int> p;
    EXPECT_TRUE(q.try_pop(p));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 7);
}

TEST(MpmcQueue, DestructorDrainsRemaining) {
    // Leave elements in the queue; the destructor must destroy them (verified
    // via Counted::live — no leak, no double-free).
    Counted::live.store(0);
    {
        MpmcQueue<Counted, 8> q;
        EXPECT_TRUE(q.try_push(Counted{1}));
        EXPECT_TRUE(q.try_push(Counted{2}));
        EXPECT_TRUE(q.try_push(Counted{3}));
        EXPECT_EQ(Counted::live.load(), 3);
    } // destructor drains 3 elements
    EXPECT_EQ(Counted::live.load(), 0);
}

// The headline guarantee: with several producers and consumers, every item is
// transferred exactly once (sum of all values is exactly preserved).
TEST(MpmcQueue, AllItemsTransferredExactlyOnce) {
    MpmcQueue<int, 1024> q;
    constexpr int P   = 4;
    constexpr int C   = 4;
    constexpr int PER = 100'000;
    std::atomic<long long> sum{0};
    std::atomic<int> consumed{0};
    std::vector<std::thread> th;
    th.reserve(static_cast<std::size_t>(P) + static_cast<std::size_t>(C));

    for (int p = 0; p < P; ++p) {
        th.emplace_back([&, p] {
            for (int i = 0; i < PER;) {
                if (q.try_push(p * PER + i)) {
                    ++i;
                }
            }
        });
    }
    for (int c = 0; c < C; ++c) {
        th.emplace_back([&] {
            int v = 0;
            while (consumed.load(std::memory_order_relaxed) < P * PER) {
                if (q.try_pop(v)) {
                    sum.fetch_add(v, std::memory_order_relaxed);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : th) {
        t.join();
    }

    long long expected = 0;
    for (long long i = 0; i < static_cast<long long>(P) * PER; ++i) {
        expected += i;
    }
    EXPECT_EQ(sum.load(), expected);
    EXPECT_EQ(consumed.load(), P * PER);
}
