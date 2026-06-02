// small_vector_test.cpp — TDD test suite for SmallVector<T, N>
//
// Name uniqueness: all helper types are prefixed "SV" (e.g. TrackedSV) to
// avoid ODR collisions with Tracked/Counter types in other test translation
// units that link into the same atx-core-tests binary.

#include <atx/core/container/small_vector.hpp>

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>

using namespace atx::core::container;

// =====================================================================
//  ODR-safe tracker for non-trivial-T lifetime tests
// =====================================================================

/// Tracks construction / destruction to detect leaks and double-frees.
struct TrackedSV {
    static int s_live;    // count of live instances (ctor - dtor)
    static int s_moves;   // move-construction count
    static int s_copies;  // copy-construction count

    int value{0};
    bool moved_from{false};

    explicit TrackedSV(int v) : value{v} { ++s_live; }

    TrackedSV(const TrackedSV& o) : value{o.value} {
        ++s_live;
        ++s_copies;
    }

    TrackedSV(TrackedSV&& o) noexcept : value{o.value} {
        o.moved_from = true;
        ++s_live;
        ++s_moves;
    }

    TrackedSV& operator=(const TrackedSV& o) {
        if (this != &o) {
            value      = o.value;
            moved_from = false;
            ++s_copies;
        }
        return *this;
    }

    TrackedSV& operator=(TrackedSV&& o) noexcept {
        if (this != &o) {
            value       = o.value;
            moved_from  = false;
            o.moved_from = true;
            ++s_moves;
        }
        return *this;
    }

    ~TrackedSV() { --s_live; }

    static void reset() noexcept {
        s_live   = 0;
        s_moves  = 0;
        s_copies = 0;
    }
};

int TrackedSV::s_live   = 0;
int TrackedSV::s_moves  = 0;
int TrackedSV::s_copies = 0;

// =====================================================================
//  Fixture that auto-resets the counter on each test
// =====================================================================
class SmallVectorTracked : public ::testing::Test {
protected:
    void SetUp() override    { TrackedSV::reset(); }
    void TearDown() override { EXPECT_EQ(TrackedSV::s_live, 0) << "TrackedSV leak or double-free"; }
};

// =====================================================================
//  1. Basic inline operation
// =====================================================================

TEST(SmallVector, StaysInlineUnderN) {
    SmallVector<int, 4> v;
    for (int i = 0; i < 4; ++i) {
        v.push_back(i);
    }
    EXPECT_FALSE(v.spilled());
    EXPECT_EQ(v.size(), 4U);
}

TEST(SmallVector, DefaultConstructedIsEmpty) {
    SmallVector<int, 4> v;
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.size(), 0U);
    EXPECT_EQ(v.capacity(), 4U);
    EXPECT_FALSE(v.spilled());
}

TEST(SmallVector, PushBackAndIndexInline) {
    SmallVector<int, 4> v;
    v.push_back(10);
    v.push_back(20);
    EXPECT_EQ(v[0U], 10);
    EXPECT_EQ(v[1U], 20);
    EXPECT_EQ(v.size(), 2U);
    EXPECT_FALSE(v.spilled());
}

TEST(SmallVector, PopBackDecreasesSize) {
    SmallVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v.pop_back();
    EXPECT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0U], 1);
}

TEST(SmallVector, EmplaceBackInline) {
    SmallVector<std::string, 4> v;
    v.emplace_back(3U, 'x');   // "xxx"
    EXPECT_EQ(v[0U], "xxx");
    EXPECT_FALSE(v.spilled());
}

TEST(SmallVector, BeginEndIteratorsInline) {
    SmallVector<int, 4> v;
    for (int i = 0; i < 4; ++i) {
        v.push_back(i * 10);
    }
    int expected = 0;
    for (const int& x : v) {
        EXPECT_EQ(x, expected);
        expected += 10;
    }
}

TEST(SmallVector, ClearResetsToEmpty) {
    SmallVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v.clear();
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.size(), 0U);
    EXPECT_FALSE(v.spilled());
}

// =====================================================================
//  2. Spill to heap
// =====================================================================

TEST(SmallVector, SpillsToHeapOverN) {
    SmallVector<int, 2> v;
    for (int i = 0; i < 10; ++i) {
        v.push_back(i);
    }
    EXPECT_TRUE(v.spilled());
    EXPECT_EQ(v.size(), 10U);
    EXPECT_EQ(v[9U], 9);
}

TEST(SmallVector, SpillBoundary_ExactlyN) {
    SmallVector<int, 3> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    EXPECT_FALSE(v.spilled());
    v.push_back(4);    // triggers spill
    EXPECT_TRUE(v.spilled());
    EXPECT_EQ(v.size(), 4U);
    EXPECT_EQ(v[3U], 4);
    // All original elements survive spill
    EXPECT_EQ(v[0U], 1);
    EXPECT_EQ(v[1U], 2);
    EXPECT_EQ(v[2U], 3);
}

TEST(SmallVector, MultipleGrowthDoublings) {
    SmallVector<int, 2> v;
    for (int i = 0; i < 100; ++i) {
        v.push_back(i);
    }
    EXPECT_TRUE(v.spilled());
    EXPECT_EQ(v.size(), 100U);
    for (atx::usize i = 0U; i < 100U; ++i) {
        EXPECT_EQ(v[i], static_cast<int>(i));
    }
}

TEST(SmallVector, CapacityGrowsMonotonically) {
    SmallVector<int, 2> v;
    atx::usize prev_cap = v.capacity();
    for (int i = 0; i < 20; ++i) {
        v.push_back(i);
        EXPECT_GE(v.capacity(), prev_cap);
        prev_cap = v.capacity();
    }
}

TEST(SmallVector, DataPointerValidAfterSpill) {
    SmallVector<int, 2> v;
    v.push_back(42);
    v.push_back(99);
    const int* before = v.data();
    (void)before;
    v.push_back(7);   // spills
    EXPECT_EQ(*v.data(), 42);
    EXPECT_EQ(v.data()[2U], 7);
}

// =====================================================================
//  3. at() bounds-checked access
// =====================================================================

TEST(SmallVector, AtSuccessInBounds) {
    SmallVector<int, 4> v;
    v.push_back(55);
    auto res = v.at(0U);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res.value(), 55);
}

TEST(SmallVector, AtReturnsOutOfRangeError) {
    SmallVector<int, 4> v;
    v.push_back(1);
    auto res = v.at(5U);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code(), atx::core::ErrorCode::OutOfRange);
}

TEST(SmallVector, AtConstOverloadInBounds) {
    SmallVector<int, 4> v;
    v.push_back(77);
    const auto& cv = v;
    auto res = cv.at(0U);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(*res.value(), 77);
}

// =====================================================================
//  4. reserve()
// =====================================================================

TEST(SmallVector, ReserveWithinInlineDoesNotSpill) {
    SmallVector<int, 8> v;
    v.reserve(4U);
    EXPECT_FALSE(v.spilled());
    EXPECT_GE(v.capacity(), 4U);
    EXPECT_EQ(v.size(), 0U);
}

TEST(SmallVector, ReserveAboveInlineForcesSpill) {
    SmallVector<int, 2> v;
    v.reserve(16U);
    EXPECT_TRUE(v.spilled());
    EXPECT_GE(v.capacity(), 16U);
    EXPECT_EQ(v.size(), 0U);
}

TEST(SmallVector, ReserveSmallerThanCurrentCapacityIsNoop) {
    SmallVector<int, 4> v;
    v.push_back(1);
    atx::usize cap_before = v.capacity();
    v.reserve(1U);
    EXPECT_EQ(v.capacity(), cap_before);
}

// =====================================================================
//  5. Move semantics — inline vs spilled
// =====================================================================

TEST(SmallVector, MovePreservesElements) {
    SmallVector<int, 2> a;
    for (int i = 0; i < 5; ++i) {
        a.push_back(i);
    }
    SmallVector<int, 2> b = std::move(a);
    EXPECT_EQ(b.size(), 5U);
    EXPECT_EQ(b[4U], 4);
}

TEST(SmallVector, MoveFromSpilledStealsHeapPointer) {
    SmallVector<int, 2> a;
    for (int i = 0; i < 8; ++i) {
        a.push_back(i);
    }
    ASSERT_TRUE(a.spilled());
    const int* original_data = a.data();

    SmallVector<int, 2> b = std::move(a);
    EXPECT_TRUE(b.spilled());
    EXPECT_EQ(b.data(), original_data);   // pointer stolen, no copy
    EXPECT_EQ(b.size(), 8U);
    EXPECT_EQ(a.size(), 0U);             // NOLINT — moved-from is valid+empty
    EXPECT_FALSE(a.spilled());           // moved-from returned to inline state
}

TEST(SmallVector, MoveFromInlineCopiesElements) {
    SmallVector<int, 4> a;
    a.push_back(11);
    a.push_back(22);
    ASSERT_FALSE(a.spilled());
    const int* a_data_before = a.data();

    SmallVector<int, 4> b = std::move(a);
    EXPECT_FALSE(b.spilled());
    // b's data() points into b's own inline storage, not a's
    EXPECT_NE(b.data(), a_data_before);
    EXPECT_EQ(b.size(), 2U);
    EXPECT_EQ(b[0U], 11);
    EXPECT_EQ(b[1U], 22);
    EXPECT_EQ(a.size(), 0U);           // NOLINT — moved-from valid+empty
}

TEST(SmallVector, MoveAssignmentFromSpilled) {
    SmallVector<int, 2> a;
    for (int i = 0; i < 6; ++i) {
        a.push_back(i);
    }
    SmallVector<int, 2> b;
    b.push_back(99);
    b = std::move(a);
    EXPECT_TRUE(b.spilled());
    EXPECT_EQ(b.size(), 6U);
    EXPECT_EQ(b[5U], 5);
    EXPECT_EQ(a.size(), 0U);           // NOLINT
}

// =====================================================================
//  6. Copy semantics — deep copy
// =====================================================================

TEST(SmallVector, CopyConstructorDeepCopiesInline) {
    SmallVector<int, 4> a;
    a.push_back(1);
    a.push_back(2);
    SmallVector<int, 4> b{a};
    EXPECT_EQ(b.size(), 2U);
    EXPECT_EQ(b[0U], 1);
    // Mutate b — a must be unchanged
    b[0U] = 99;
    EXPECT_EQ(a[0U], 1);
}

TEST(SmallVector, CopyConstructorDeepCopiesSpilled) {
    SmallVector<int, 2> a;
    for (int i = 0; i < 8; ++i) {
        a.push_back(i);
    }
    SmallVector<int, 2> b{a};
    EXPECT_EQ(b.size(), 8U);
    EXPECT_TRUE(b.spilled());
    // Different heap buffers
    EXPECT_NE(b.data(), a.data());
    b[0U] = 999;
    EXPECT_EQ(a[0U], 0);
}

TEST(SmallVector, CopyAssignmentDeepCopies) {
    SmallVector<int, 4> a;
    a.push_back(5);
    a.push_back(6);
    SmallVector<int, 4> b;
    b.push_back(100);
    b = a;
    EXPECT_EQ(b.size(), 2U);
    EXPECT_EQ(b[0U], 5);
    b[0U] = 0;
    EXPECT_EQ(a[0U], 5);   // a unchanged
}

// =====================================================================
//  7. Non-trivial-T lifetime across spill/grow/move/copy
// =====================================================================

TEST_F(SmallVectorTracked, NoLeakOnDestructionInline) {
    {
        SmallVector<TrackedSV, 4> v;
        v.emplace_back(1);
        v.emplace_back(2);
        EXPECT_EQ(TrackedSV::s_live, 2);
    }
    // TearDown asserts s_live == 0
}

TEST_F(SmallVectorTracked, NoLeakOnDestructionSpilled) {
    {
        SmallVector<TrackedSV, 2> v;
        for (int i = 0; i < 6; ++i) {
            v.emplace_back(i);
        }
        EXPECT_EQ(TrackedSV::s_live, 6);
    }
    // TearDown asserts s_live == 0
}

TEST_F(SmallVectorTracked, NoLeakAfterSpillBoundary) {
    {
        SmallVector<TrackedSV, 3> v;
        v.emplace_back(1);
        v.emplace_back(2);
        v.emplace_back(3);
        EXPECT_FALSE(v.spilled());
        v.emplace_back(4);   // triggers spill + move of existing 3 elements
        EXPECT_TRUE(v.spilled());
        EXPECT_EQ(TrackedSV::s_live, 4);
    }
}

TEST_F(SmallVectorTracked, NoLeakAfterMoveFromSpilled) {
    {
        SmallVector<TrackedSV, 2> a;
        for (int i = 0; i < 5; ++i) {
            a.emplace_back(i);
        }
        EXPECT_EQ(TrackedSV::s_live, 5);
        SmallVector<TrackedSV, 2> b = std::move(a);
        EXPECT_EQ(TrackedSV::s_live, 5);   // pointer stolen, no new objects
    }
}

TEST_F(SmallVectorTracked, NoLeakAfterMoveFromInline) {
    {
        SmallVector<TrackedSV, 4> a;
        a.emplace_back(10);
        a.emplace_back(20);
        SmallVector<TrackedSV, 4> b = std::move(a);
        EXPECT_EQ(TrackedSV::s_live, 2);
    }
}

TEST_F(SmallVectorTracked, NoLeakAfterCopy) {
    {
        SmallVector<TrackedSV, 4> a;
        a.emplace_back(1);
        a.emplace_back(2);
        SmallVector<TrackedSV, 4> b{a};
        EXPECT_EQ(TrackedSV::s_live, 4);
    }
}

TEST_F(SmallVectorTracked, NoLeakAfterClear) {
    SmallVector<TrackedSV, 2> v;
    for (int i = 0; i < 6; ++i) {
        v.emplace_back(i);
    }
    EXPECT_EQ(TrackedSV::s_live, 6);
    v.clear();
    EXPECT_EQ(TrackedSV::s_live, 0);
    EXPECT_TRUE(v.empty());
}

TEST_F(SmallVectorTracked, NoDoubleDestroyAfterGrow) {
    // This test catches the classic bug of destroying old elements twice
    // when growing — once in the grow logic and once in dtor.
    SmallVector<TrackedSV, 2> v;
    v.emplace_back(1);
    v.emplace_back(2);
    v.emplace_back(3);   // grow: 2 → 4
    v.emplace_back(4);
    v.emplace_back(5);   // grow: 4 → 8
    EXPECT_EQ(TrackedSV::s_live, 5);
}

TEST_F(SmallVectorTracked, PushBackRvalueNoExtraCopy) {
    SmallVector<TrackedSV, 4> v;
    TrackedSV t{42};
    EXPECT_EQ(TrackedSV::s_live, 1);
    v.push_back(std::move(t));
    // 1 original + 1 moved-into-vector = 2 live; one move recorded
    EXPECT_EQ(TrackedSV::s_live, 2);
    EXPECT_GE(TrackedSV::s_moves, 1);
}

// ============================================================
// Exception safety — partial-construction rollback
// ============================================================
//
// A type whose COPY ctor throws on demand.  It declares no move ctor, so the
// copy ctor also serves move-construction and std::move_if_noexcept selects it
// (the copy may throw).  This drives the rollback paths in copy_from,
// reallocate and steal_from from a single helper.
struct ThrowingCopySV {
    static int live;           // currently-alive instances
    static int copies_allowed; // successful copies permitted before throwing
    int value;

    explicit ThrowingCopySV(int v) : value{v} { ++live; }
    ThrowingCopySV(const ThrowingCopySV& o) : value{o.value} {
        if (copies_allowed <= 0) {
            throw std::runtime_error("copy budget exhausted");
        }
        --copies_allowed;
        ++live;
    }
    ThrowingCopySV& operator=(const ThrowingCopySV&) = default;
    ThrowingCopySV(ThrowingCopySV&&)                 = delete;
    ThrowingCopySV& operator=(ThrowingCopySV&&)      = delete;
    ~ThrowingCopySV() { --live; }

    static void reset(int allowed) noexcept {
        live           = 0;
        copies_allowed = allowed;
    }
};
int ThrowingCopySV::live           = 0;
int ThrowingCopySV::copies_allowed = 0;

// copy_from: a throw partway through a deep copy (spilled source) must leave no
// constructed element or heap buffer behind in the half-built destination.
TEST(SmallVectorExcept, CopyFromThrowsNoLeak) {
    ThrowingCopySV::reset(/*allowed=*/100);
    SmallVector<ThrowingCopySV, 2> src; // N=2 → spills to heap below
    for (int i = 0; i < 5; ++i) {
        src.emplace_back(i);
    }
    ASSERT_EQ(ThrowingCopySV::live, 5);

    ThrowingCopySV::copies_allowed = 3; // 4th copy throws mid copy_from
    auto copy_attempt = [&src]() {
        SmallVector<ThrowingCopySV, 2> dst{src};
        (void)dst;
    };
    EXPECT_THROW(copy_attempt(), std::runtime_error);

    // Only src's 5 elements remain; dst's 3 partial copies were destroyed and
    // its heap buffer freed.
    EXPECT_EQ(ThrowingCopySV::live, 5);
    EXPECT_EQ(src.size(), 5U);
}

// reallocate: a throw while transferring elements into the grown buffer must
// roll back (strong guarantee) — the source vector is unchanged and the new
// buffer is freed.
TEST(SmallVectorExcept, ReallocateThrowsNoLeakNoDoubleDestroy) {
    ThrowingCopySV::reset(/*allowed=*/100);
    SmallVector<ThrowingCopySV, 2> v;
    v.emplace_back(1);
    v.emplace_back(2); // full at N=2, still inline
    ASSERT_EQ(ThrowingCopySV::live, 2);

    // The next emplace grows 2→4 → reallocate transfers 2 elements via copy.
    ThrowingCopySV::copies_allowed = 1; // 2nd transfer copy throws
    EXPECT_THROW(v.emplace_back(3), std::runtime_error);

    // Strong guarantee: v is unchanged, the would-be element 3 never built, the
    // grown buffer rolled back and freed — no leak, no double-destroy.
    EXPECT_EQ(ThrowingCopySV::live, 2);
    EXPECT_EQ(v.size(), 2U);
    EXPECT_EQ(v[0].value, 1);
    EXPECT_EQ(v[1].value, 2);
}

// steal_from (inline path): a throw while element-wise transferring from an
// inline source during move-construction must roll back the destination and
// leave the source fully intact.
TEST(SmallVectorExcept, StealFromInlineThrowsNoLeak) {
    ThrowingCopySV::reset(/*allowed=*/100);
    SmallVector<ThrowingCopySV, 4> src; // inline (N=4)
    src.emplace_back(1);
    src.emplace_back(2);
    src.emplace_back(3);
    ASSERT_EQ(ThrowingCopySV::live, 3);

    ThrowingCopySV::copies_allowed = 1; // 2nd transfer throws mid steal_from
    auto move_attempt = [&src]() {
        SmallVector<ThrowingCopySV, 4> dst{std::move(src)};
        (void)dst;
    };
    EXPECT_THROW(move_attempt(), std::runtime_error);

    // src keeps all 3 elements (none destroyed); dst's 1 partial element was
    // rolled back.
    EXPECT_EQ(ThrowingCopySV::live, 3);
    EXPECT_EQ(src.size(), 3U);
}
