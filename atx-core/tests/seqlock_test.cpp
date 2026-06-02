// seqlock_test.cpp — tests for SeqLock<T> single-writer/many-reader snapshot.

#include <atx/core/concurrent/seqlock.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using atx::core::concurrent::SeqLock;

namespace {

struct Quote {
    double bid;
    double ask;
};

} // namespace

TEST(SeqLock, StoreThenLoadSingleThread) {
    SeqLock<Quote> q;
    q.store({1.0, 2.0});
    const Quote r = q.load();
    EXPECT_DOUBLE_EQ(r.bid, 1.0);
    EXPECT_DOUBLE_EQ(r.ask, 2.0);
}

TEST(SeqLock, SeedingConstructorIsVisible) {
    SeqLock<Quote> q{Quote{10.0, 11.0}};
    const Quote r = q.load();
    EXPECT_DOUBLE_EQ(r.bid, 10.0);
    EXPECT_DOUBLE_EQ(r.ask, 11.0);
}

TEST(SeqLock, DefaultConstructedReadsZeroValue) {
    SeqLock<Quote> q;
    const Quote r = q.load();
    EXPECT_DOUBLE_EQ(r.bid, 0.0);
    EXPECT_DOUBLE_EQ(r.ask, 0.0);
}

TEST(SeqLock, RepeatedStoresLoadLatest) {
    SeqLock<int> q;
    for (int i = 0; i < 1000; ++i) {
        q.store(i);
        EXPECT_EQ(q.load(), i);
    }
}

// The reader must never observe a torn snapshot: bid/ask are written together
// such that ask - bid == 1.0 always holds for any consistent read.
TEST(SeqLock, ReadSeesConsistentSnapshotUnderConcurrentWriter) {
    SeqLock<Quote> q;
    q.store({1.0, 2.0});
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        double x = 0.0;
        while (!stop.load(std::memory_order_relaxed)) {
            q.store({x, x + 1.0});
            x += 1.0;
        }
    });

    for (int i = 0; i < 1'000'000; ++i) {
        const Quote r = q.load();
        EXPECT_DOUBLE_EQ(r.ask - r.bid, 1.0);
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();
}

// Many concurrent readers + one writer: every reader always sees a value that
// satisfies the cross-field invariant, exercising shared-line acquire loads.
TEST(SeqLock, ManyReadersOneWriterStayConsistent) {
    struct Pair {
        long a;
        long b; // invariant: b == -a
    };
    SeqLock<Pair> q;
    q.store({0, 0});
    std::atomic<bool> stop{false};
    std::atomic<long> mismatches{0};

    std::vector<std::thread> readers;
    readers.reserve(4);
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const Pair p = q.load();
                if (p.a + p.b != 0) {
                    mismatches.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (long x = 1; x <= 500'000; ++x) {
        q.store({x, -x});
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& r : readers) {
        r.join();
    }
    EXPECT_EQ(mismatches.load(), 0);
}
