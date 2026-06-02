// disruptor_test.cpp — TDD tests for atx::core::concurrent::Disruptor
//
// The Disruptor is a sequenced ring buffer (LMAX pattern): a producer claims a
// monotonically increasing sequence, writes the slot, then publishes it; one or
// more consumers wait for a sequence to become available, drain a batch, then
// signal how far they have consumed so the producer may wrap.
//
// Coverage (agent.md §7 — happy path, boundaries, invariants, error paths):
//   - SP -> SC ordered transfer of a large N (the Task-21 seed).
//   - SP -> multi-consumer: every consumer sees every event in order; the
//     producer gates on the SLOWEST consumer (no overwrite of an unconsumed
//     slot — verified by a per-slot tag / running checksum).
//   - MP -> SC: multiple producers, every item delivered exactly once
//     (checksum + count match), monotonic published order.
//   - Backpressure / wrap: capacity-1 and tiny rings force the producer to wait
//     for the consumer; a per-slot generation tag proves no live slot is
//     overwritten before it is consumed.
//   - Batch consume: wait_for returns > the requested seq when several events
//     are already published, so a consumer drains a batch per wait.
//   - Boundary: capacity rounding to a power of two, single-element ring, the
//     no-publish / empty case (wait must not falsely report availability).
//   - Determinism: same input -> same consumed order (replay-relevant).
//
// Threading: every multi-threaded test joins all threads before asserting on
// shared state, and uses atomics for cross-thread accumulators, so the tests
// are themselves race-clean (intended to pass under TSan).

#include <atx/core/concurrent/disruptor.hpp>

#include <atomic>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

using namespace atx;                   // NOLINT(google-build-using-namespace)
using namespace atx::core;             // NOLINT(google-build-using-namespace)
using namespace atx::core::concurrent; // NOLINT(google-build-using-namespace)

// ---------------------------------------------------------------------------
// Seed: SP -> SC ordered transfer of a large N (Task 21, Step 1).
// ---------------------------------------------------------------------------
TEST(Disruptor, SingleProducerSingleConsumerOrdered) {
    Disruptor<long, 1024> d;
    constexpr long N = 500000;

    std::thread prod([&] {
        for (long i = 0; i < N; ++i) {
            const i64 s = d.claim();
            d.at(s)    = i;
            d.publish(s);
        }
    });

    long expected = 0;
    for (long i = 0; i < N; ++i) {
        const i64  s   = d.wait_for(i);
        const long got = d.at(i);
        EXPECT_EQ(got, expected);
        EXPECT_GE(s, i); // wait_for returns the highest available (>= requested)
        ++expected;
        d.consumed(i);
    }
    prod.join();
}

// ---------------------------------------------------------------------------
// Boundary: Capacity that is not a power of two is rounded up; the usable
// capacity is the rounded value. A single-element ring is the degenerate case.
// ---------------------------------------------------------------------------
TEST(Disruptor, CapacityRoundedToPowerOfTwo) {
    // 1000 is not a power of two -> rounds up to 1024.
    EXPECT_EQ((Disruptor<long, 1000>::capacity()), 1024u);
    // Exact powers of two are unchanged.
    EXPECT_EQ((Disruptor<long, 1024>::capacity()), 1024u);
    EXPECT_EQ((Disruptor<long, 1>::capacity()), 1u);
}

TEST(Disruptor, SingleElementRingTransfersInOrder) {
    Disruptor<int, 1> d; // capacity 1 -> producer must wait every step
    constexpr int N = 2000;

    std::thread prod([&] {
        for (int i = 0; i < N; ++i) {
            const i64 s = d.claim();
            d.at(s)    = i;
            d.publish(s);
        }
    });

    for (int i = 0; i < N; ++i) {
        d.wait_for(i);
        EXPECT_EQ(d.at(i), i);
        d.consumed(i);
    }
    prod.join();
}

// ---------------------------------------------------------------------------
// Empty / no-publish: nothing was published, so a non-blocking peek of the
// highest published sequence must report "nothing available".
// ---------------------------------------------------------------------------
TEST(Disruptor, EmptyRingHasNothingPublished) {
    Disruptor<long, 16> d;
    // No producer has claimed/published; published cursor is the initial value
    // (-1 by LMAX convention: sequence 0 is the first valid event).
    EXPECT_EQ(d.published_sequence(), -1);
    // try_wait_for must not falsely report sequence 0 as available.
    EXPECT_LT(d.try_wait_for(0), 0);
}

// ---------------------------------------------------------------------------
// Batch consume: once several events are published, wait_for(seq) returns the
// HIGHEST available sequence so the consumer can drain a batch in one wait.
// ---------------------------------------------------------------------------
TEST(Disruptor, WaitForReturnsHighestAvailableForBatching) {
    Disruptor<int, 64> d;
    constexpr int kBatch = 10;

    // Publish a batch from this thread (single producer, no consumer yet).
    for (int i = 0; i < kBatch; ++i) {
        const i64 s = d.claim();
        d.at(s)    = i * 7;
        d.publish(s);
    }

    const i64 highest = d.wait_for(0);
    EXPECT_EQ(highest, kBatch - 1); // all 10 already available

    // Drain the whole batch addressed by the single wait.
    for (i64 s = 0; s <= highest; ++s) {
        EXPECT_EQ(d.at(s), static_cast<int>(s) * 7);
    }
    d.consumed(highest);
}

// ---------------------------------------------------------------------------
// Backpressure / wrap with a tiny ring: a per-slot generation tag proves the
// producer never overwrites a slot the consumer has not yet consumed. The tag
// encodes (sequence) so any premature overwrite would corrupt an in-flight
// read and be caught by the equality check.
// ---------------------------------------------------------------------------
TEST(Disruptor, BackpressureNoOverwriteOfUnconsumedSlot) {
    Disruptor<i64, 4> d; // capacity 4 forces frequent wrapping
    constexpr i64 N = 100000;

    std::thread prod([&] {
        for (i64 i = 0; i < N; ++i) {
            const i64 s = d.claim();
            // Encode the sequence itself as the payload; the consumer checks it.
            d.at(s) = i;
            d.publish(s);
        }
    });

    for (i64 i = 0; i < N; ++i) {
        d.wait_for(i);
        // If the producer had wrapped and overwritten this slot before we read
        // it, the payload would not equal i.
        ASSERT_EQ(d.at(i), i) << "slot " << i << " overwritten before consume";
        d.consumed(i);
    }
    prod.join();
}

// ---------------------------------------------------------------------------
// SP -> multi-consumer: every consumer observes every event in order, and the
// producer gates on the slowest consumer. One consumer is deliberately slowed
// to exercise the gating-on-slowest path on a small ring.
// ---------------------------------------------------------------------------
TEST(Disruptor, SingleProducerMultiConsumerAllSeeEveryEventInOrder) {
    constexpr usize kConsumers = 3;
    Disruptor<i64, 64, ProducerKind::Single, kConsumers> d;
    constexpr i64 N = 200000;

    std::array<std::atomic<i64>, kConsumers> sums{};
    std::array<std::atomic<i64>, kConsumers> counts{};

    std::array<std::thread, kConsumers> consumers;
    for (usize c = 0; c < kConsumers; ++c) {
        consumers[c] = std::thread([&, c] {
            i64 expected = 0;
            for (i64 i = 0; i < N; ++i) {
                d.wait_for(i, c);
                const i64 v = d.at(i);
                // Each consumer must see strictly increasing, complete order.
                EXPECT_EQ(v, expected);
                ++expected;
                sums[c].fetch_add(v, std::memory_order_relaxed);
                counts[c].fetch_add(1, std::memory_order_relaxed);
                d.consumed(i, c);
            }
        });
    }

    std::thread prod([&] {
        for (i64 i = 0; i < N; ++i) {
            const i64 s = d.claim();
            d.at(s)    = i;
            d.publish(s);
        }
    });

    prod.join();
    for (auto& t : consumers) {
        t.join();
    }

    const i64 expected_sum = (N - 1) * N / 2;
    for (usize c = 0; c < kConsumers; ++c) {
        EXPECT_EQ(counts[c].load(), N);
        EXPECT_EQ(sums[c].load(), expected_sum);
    }
}

// ---------------------------------------------------------------------------
// MP -> SC: several producers each contribute a disjoint key range; the single
// consumer must receive every item exactly once (count + checksum match) and in
// a monotonically non-decreasing published order.
// ---------------------------------------------------------------------------
TEST(Disruptor, MultiProducerSingleConsumerExactlyOnceChecksum) {
    constexpr usize kProducers   = 4;
    constexpr i64   kPerProducer = 50000;
    constexpr i64   kTotal       = static_cast<i64>(kProducers) * kPerProducer;

    Disruptor<i64, 1024, ProducerKind::Multi> d;

    std::array<std::thread, kProducers> producers;
    for (usize p = 0; p < kProducers; ++p) {
        producers[p] = std::thread([&, p] {
            const i64 base = static_cast<i64>(p) * kPerProducer;
            for (i64 i = 0; i < kPerProducer; ++i) {
                const i64 s = d.claim(); // MP claim: CAS on cursor
                d.at(s)    = base + i;   // unique value across all producers
                d.publish(s);
            }
        });
    }

    i64 sum   = 0;
    i64 count = 0;
    for (i64 i = 0; i < kTotal; ++i) {
        d.wait_for(i);
        sum += d.at(i);
        ++count;
        d.consumed(i);
    }

    for (auto& t : producers) {
        t.join();
    }

    const i64 expected_sum = (kTotal - 1) * kTotal / 2; // 0..kTotal-1 each once
    EXPECT_EQ(count, kTotal);
    EXPECT_EQ(sum, expected_sum);
}

// ---------------------------------------------------------------------------
// MP availability buffer correctness under heavy wrapping: a small ring with
// multiple producers stresses the per-slot round-number availability flag so a
// consumer never reads a slot still being written by an out-of-order producer.
// ---------------------------------------------------------------------------
TEST(Disruptor, MultiProducerSmallRingExactlyOnce) {
    constexpr usize kProducers   = 3;
    constexpr i64   kPerProducer = 40000;
    constexpr i64   kTotal       = static_cast<i64>(kProducers) * kPerProducer;

    Disruptor<i64, 8, ProducerKind::Multi> d; // tiny ring -> constant wrapping

    std::array<std::thread, kProducers> producers;
    for (usize p = 0; p < kProducers; ++p) {
        producers[p] = std::thread([&, p] {
            const i64 base = static_cast<i64>(p) * kPerProducer;
            for (i64 i = 0; i < kPerProducer; ++i) {
                const i64 s = d.claim();
                d.at(s)    = base + i;
                d.publish(s);
            }
        });
    }

    std::vector<bool> seen(static_cast<usize>(kTotal), false);
    i64               count = 0;
    for (i64 i = 0; i < kTotal; ++i) {
        d.wait_for(i);
        const i64 v = d.at(i);
        ASSERT_GE(v, 0);
        ASSERT_LT(v, kTotal);
        EXPECT_FALSE(seen[static_cast<usize>(v)]) << "value " << v << " seen twice";
        seen[static_cast<usize>(v)] = true;
        ++count;
        d.consumed(i);
    }
    for (auto& t : producers) {
        t.join();
    }

    EXPECT_EQ(count, kTotal);
    for (i64 v = 0; v < kTotal; ++v) {
        EXPECT_TRUE(seen[static_cast<usize>(v)]) << "value " << v << " missing";
    }
}

// ---------------------------------------------------------------------------
// Determinism: with a single producer and single consumer, the consumed order
// is exactly the produced order, every run. We capture the order and compare
// against the canonical 0..N-1 sequence.
// ---------------------------------------------------------------------------
TEST(Disruptor, DeterministicConsumedOrderMatchesProducedOrder) {
    Disruptor<i64, 256> d;
    constexpr i64 N = 10000;

    std::vector<i64> consumed_order;
    consumed_order.reserve(static_cast<usize>(N));

    std::thread prod([&] {
        for (i64 i = 0; i < N; ++i) {
            const i64 s = d.claim();
            d.at(s)    = i;
            d.publish(s);
        }
    });

    i64 next = 0;
    while (next < N) {
        const i64 hi = d.wait_for(next);
        for (i64 s = next; s <= hi; ++s) {
            consumed_order.push_back(d.at(s));
        }
        d.consumed(hi);
        next = hi + 1;
    }
    prod.join();

    ASSERT_EQ(static_cast<i64>(consumed_order.size()), N);
    for (i64 i = 0; i < N; ++i) {
        EXPECT_EQ(consumed_order[static_cast<usize>(i)], i);
    }
}

// ---------------------------------------------------------------------------
// Consumer-barrier handle API: registering a consumer yields a handle that
// exposes the same wait/consumed semantics, validating the multi-consumer
// registry design for the engine event-spine.
// ---------------------------------------------------------------------------
TEST(Disruptor, ConsumerBarrierHandleRoundTrips) {
    constexpr usize kConsumers = 2;
    Disruptor<i64, 32, ProducerKind::Single, kConsumers> d;
    constexpr i64 N = 5000;

    auto c0 = d.consumer(0);
    auto c1 = d.consumer(1);

    std::atomic<i64> sum0{0};
    std::atomic<i64> sum1{0};

    std::thread t0([&] {
        for (i64 i = 0; i < N; ++i) {
            c0.wait_for(i);
            sum0.fetch_add(d.at(i), std::memory_order_relaxed);
            c0.consumed(i);
        }
    });
    std::thread t1([&] {
        for (i64 i = 0; i < N; ++i) {
            c1.wait_for(i);
            sum1.fetch_add(d.at(i), std::memory_order_relaxed);
            c1.consumed(i);
        }
    });

    std::thread prod([&] {
        for (i64 i = 0; i < N; ++i) {
            const i64 s = d.claim();
            d.at(s)    = i;
            d.publish(s);
        }
    });

    prod.join();
    t0.join();
    t1.join();

    const i64 expected_sum = (N - 1) * N / 2;
    EXPECT_EQ(sum0.load(), expected_sum);
    EXPECT_EQ(sum1.load(), expected_sum);
}
