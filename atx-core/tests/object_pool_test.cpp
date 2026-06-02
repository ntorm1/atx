// object_pool_test.cpp — TDD tests for atx::core::ObjectPool
//
// Coverage:
//   - Basic acquire / release cycle
//   - Exhaustion returns nullptr
//   - Released slot is reused (address identity)
//   - size / available / capacity accounting
//   - acquire with constructor arguments
//   - All slots yield distinct addresses
//   - Release-all then reacquire-all (full round-trip)
//   - Non-trivial type: ctor/dtor call counting proves object lifetime

#include <atx/core/object_pool.hpp>

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

using namespace atx;       // NOLINT(google-build-using-namespace)
using namespace atx::core; // NOLINT(google-build-using-namespace)

// ---------------------------------------------------------------------------
// Instrumented type for ctor/dtor counting
// ---------------------------------------------------------------------------
struct LifetimeCounter {
    static int ctor_count;
    static int dtor_count;

    int value{0};

    explicit LifetimeCounter(int v) : value{v} { ++ctor_count; }

    // non-trivial destructor
    ~LifetimeCounter() { ++dtor_count; }

    // non-copyable / non-movable to avoid accidental double-count
    LifetimeCounter(const LifetimeCounter&)            = delete;
    LifetimeCounter& operator=(const LifetimeCounter&) = delete;
    LifetimeCounter(LifetimeCounter&&)                 = delete;
    LifetimeCounter& operator=(LifetimeCounter&&)      = delete;

    static void reset() noexcept {
        ctor_count = 0;
        dtor_count = 0;
    }
};

int LifetimeCounter::ctor_count = 0;
int LifetimeCounter::dtor_count = 0;

// ---------------------------------------------------------------------------
// TEST: Basic acquire / release and slot reuse
// ---------------------------------------------------------------------------
TEST(ObjectPool, AcquireRelease) {
    ObjectPool<int, 4> p;
    int* const a = p.acquire();
    int* const b = p.acquire();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);

    p.release(a);
    // The freed slot must be reused immediately (LIFO index stack).
    int* const a2 = p.acquire();
    EXPECT_EQ(a2, a);

    // Release all live objects before pool destruction.
    p.release(a2);
    p.release(b);
}

// ---------------------------------------------------------------------------
// TEST: Pool exhaustion returns nullptr
// ---------------------------------------------------------------------------
TEST(ObjectPool, ExhaustionReturnsNull) {
    ObjectPool<int, 2> p;
    int* const x = p.acquire();
    int* const y = p.acquire();
    EXPECT_EQ(p.acquire(), nullptr);
    // Release before pool destructs to satisfy the live_count_ == 0 invariant.
    if (x != nullptr) { p.release(x); }
    if (y != nullptr) { p.release(y); }
}

// ---------------------------------------------------------------------------
// TEST: Size accounting (size / available / capacity)
// ---------------------------------------------------------------------------
TEST(ObjectPool, SizeAccounting) {
    ObjectPool<int, 4> p;
    EXPECT_EQ(p.size(),      0U);
    EXPECT_EQ(p.available(), 4U);
    EXPECT_EQ(p.capacity(),  4U);

    int* const a = p.acquire();
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(p.size(),      1U);
    EXPECT_EQ(p.available(), 3U);

    int* const b = p.acquire();
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(p.size(),      2U);
    EXPECT_EQ(p.available(), 2U);

    p.release(a);
    EXPECT_EQ(p.size(),      1U);
    EXPECT_EQ(p.available(), 3U);

    p.release(b);
    EXPECT_EQ(p.size(),      0U);
    EXPECT_EQ(p.available(), 4U);
}

// ---------------------------------------------------------------------------
// TEST: acquire constructs T with forwarded arguments
// ---------------------------------------------------------------------------
TEST(ObjectPool, ConstructWithArgs) {
    ObjectPool<int, 4> p;
    int* const x = p.acquire(42);
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(*x, 42);
    p.release(x);
}

// ---------------------------------------------------------------------------
// TEST: All slots yield distinct, non-overlapping addresses
// ---------------------------------------------------------------------------
TEST(ObjectPool, DistinctAddresses) {
    constexpr usize N = 8U;
    ObjectPool<int, N> p;

    std::array<int*, N> ptrs{};
    for (usize i = 0U; i < N; ++i) {
        ptrs[i] = p.acquire();
        ASSERT_NE(ptrs[i], nullptr);
    }

    for (usize i = 0U; i < N; ++i) {
        for (usize j = i + 1U; j < N; ++j) {
            EXPECT_NE(ptrs[i], ptrs[j]);
        }
    }

    for (usize i = 0U; i < N; ++i) {
        p.release(ptrs[i]);
    }
}

// ---------------------------------------------------------------------------
// TEST: Release-all then reacquire-all (full round-trip)
// ---------------------------------------------------------------------------
TEST(ObjectPool, ReleaseAllThenReacquireAll) {
    constexpr usize N = 4U;
    ObjectPool<int, N> p;

    std::array<int*, N> ptrs{};
    for (usize i = 0U; i < N; ++i) {
        ptrs[i] = p.acquire();
        ASSERT_NE(ptrs[i], nullptr);
    }
    // Pool is now full.
    EXPECT_EQ(p.acquire(), nullptr);

    // Release all.
    for (usize i = 0U; i < N; ++i) {
        p.release(ptrs[i]);
    }
    EXPECT_EQ(p.size(),      0U);
    EXPECT_EQ(p.available(), N);

    // Reacquire all — pool must accept them.
    for (usize i = 0U; i < N; ++i) {
        ptrs[i] = p.acquire();
        EXPECT_NE(ptrs[i], nullptr);
    }
    EXPECT_EQ(p.size(), N);

    // Clean up.
    for (usize i = 0U; i < N; ++i) {
        p.release(ptrs[i]);
    }
}

// ---------------------------------------------------------------------------
// TEST: Non-trivial type — ctor fires on acquire, dtor fires on release
// ---------------------------------------------------------------------------
TEST(ObjectPool, NonTrivialCtorDtor) {
    LifetimeCounter::reset();

    {
        ObjectPool<LifetimeCounter, 4> p;

        LifetimeCounter* const lc1 = p.acquire(10);
        ASSERT_NE(lc1, nullptr);
        EXPECT_EQ(lc1->value,             10);
        EXPECT_EQ(LifetimeCounter::ctor_count, 1);
        EXPECT_EQ(LifetimeCounter::dtor_count, 0);

        LifetimeCounter* const lc2 = p.acquire(20);
        ASSERT_NE(lc2, nullptr);
        EXPECT_EQ(lc2->value,             20);
        EXPECT_EQ(LifetimeCounter::ctor_count, 2);

        // release must call ~LifetimeCounter()
        p.release(lc1);
        EXPECT_EQ(LifetimeCounter::dtor_count, 1);

        p.release(lc2);
        EXPECT_EQ(LifetimeCounter::dtor_count, 2);

        // reacquire the freed slot and construct again
        LifetimeCounter* const lc3 = p.acquire(30);
        ASSERT_NE(lc3, nullptr);
        EXPECT_EQ(LifetimeCounter::ctor_count, 3);
        EXPECT_EQ(lc3->value, 30);

        p.release(lc3);
        EXPECT_EQ(LifetimeCounter::dtor_count, 3);

        // Pool destructor — all objects already released, dtor count unchanged.
    }

    EXPECT_EQ(LifetimeCounter::ctor_count, 3);
    EXPECT_EQ(LifetimeCounter::dtor_count, 3);
}

// ---------------------------------------------------------------------------
// TEST: Pool reports correct capacity constant
// ---------------------------------------------------------------------------
TEST(ObjectPool, CapacityConstant) {
    ObjectPool<double, 16> p;
    EXPECT_EQ(p.capacity(), 16U);
}
