// fixed_vector_test.cpp — TDD tests for atx::core::container::FixedVector
//
// Order: seed tests (from spec) + full coverage: boundaries, non-trivial T
// lifetime, copy/move semantics, at() OOR error, contiguous layout, iteration.

#include <atx/core/container/fixed_vector.hpp>

#include <functional> // std::reference_wrapper
#include <numeric>    // std::iota, std::accumulate
#include <string>

#include <gtest/gtest.h>

using namespace atx::core::container;
using namespace atx::core; // ErrorCode

// ============================================================
// Seed tests (from spec)
// ============================================================

TEST(FixedVector, PushBackAndIndex) {
    FixedVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    EXPECT_EQ(v.size(), 2U);
    EXPECT_EQ(v[0], 1);
    EXPECT_EQ(v[1], 2);
}

TEST(FixedVector, FullReturnsFalseOnTryPush) {
    FixedVector<int, 2> v;
    v.push_back(1);
    v.push_back(2);
    EXPECT_FALSE(v.try_push_back(3));
}

TEST(FixedVector, PopBackAndClear) {
    FixedVector<int, 4> v;
    v.push_back(1);
    v.pop_back();
    EXPECT_TRUE(v.empty());
}

// ============================================================
// Capacity / full / empty observers
// ============================================================

TEST(FixedVector, CapacityIsStaticAndCorrect) {
    FixedVector<int, 8> v;
    EXPECT_EQ(v.capacity(), 8U);
    EXPECT_TRUE(v.empty());
    EXPECT_FALSE(v.full());
}

TEST(FixedVector, FullAfterCapacityPushes) {
    FixedVector<int, 3> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    EXPECT_TRUE(v.full());
    EXPECT_EQ(v.size(), 3U);
}

TEST(FixedVector, TryPushReturnsTrueWhenRoom) {
    FixedVector<int, 4> v;
    EXPECT_TRUE(v.try_push_back(42));
    EXPECT_EQ(v.size(), 1U);
}

TEST(FixedVector, TryPushReturnsFalseWhenFull) {
    FixedVector<int, 2> v;
    static_cast<void>(v.try_push_back(1));
    static_cast<void>(v.try_push_back(2));
    EXPECT_FALSE(v.try_push_back(3));
    EXPECT_EQ(v.size(), 2U); // unchanged
}

// ============================================================
// push_back move overload
// ============================================================

TEST(FixedVector, PushBackRvalueRef) {
    FixedVector<std::string, 4> v;
    std::string s = "hello";
    v.push_back(std::move(s));
    EXPECT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0], "hello");
}

// ============================================================
// emplace_back
// ============================================================

TEST(FixedVector, EmplaceBackConstructsInPlace) {
    FixedVector<std::string, 4> v;
    std::string& ref = v.emplace_back(3U, 'x'); // string(count, ch)
    EXPECT_EQ(v.size(), 1U);
    EXPECT_EQ(ref, "xxx");
    EXPECT_EQ(v[0], "xxx");
}

TEST(FixedVector, EmplaceBackReturnsCorrectRef) {
    FixedVector<int, 4> v;
    int& r = v.emplace_back(99);
    r = 100;
    EXPECT_EQ(v[0], 100);
}

// ============================================================
// operator[] mutable write-back
// ============================================================

TEST(FixedVector, IndexOperatorMutableWriteback) {
    FixedVector<int, 4> v;
    v.push_back(10);
    v[0] = 20;
    EXPECT_EQ(v[0], 20);
}

// ============================================================
// front() / back()
// ============================================================

TEST(FixedVector, FrontAndBack) {
    FixedVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    EXPECT_EQ(v.front(), 1);
    EXPECT_EQ(v.back(), 3);
}

TEST(FixedVector, FrontAndBackMutableWriteback) {
    FixedVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v.front() = 10;
    v.back() = 20;
    EXPECT_EQ(v[0], 10);
    EXPECT_EQ(v[1], 20);
}

// ============================================================
// at() — bounds-checked Result<T*>
// ============================================================

TEST(FixedVector, AtReturnsPointerOnValidIndex) {
    FixedVector<int, 4> v;
    v.push_back(42);
    auto result = v.at(0U);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result.value(), 42);
}

TEST(FixedVector, AtReturnsOutOfRangeErrWhenEmpty) {
    FixedVector<int, 4> v;
    auto result = v.at(0U);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
}

TEST(FixedVector, AtReturnsOutOfRangeErrBeyondSize) {
    FixedVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    auto result = v.at(2U);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::OutOfRange);
}

TEST(FixedVector, AtMutatesValue) {
    FixedVector<int, 4> v;
    v.push_back(10);
    auto result = v.at(0U);
    ASSERT_TRUE(result.has_value());
    *result.value() = 99;
    EXPECT_EQ(v[0], 99);
}

// ============================================================
// pop_back — multiple pops
// ============================================================

TEST(FixedVector, PopBackMultipleTimes) {
    FixedVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    v.pop_back();
    EXPECT_EQ(v.size(), 2U);
    EXPECT_EQ(v.back(), 2);
    v.pop_back();
    EXPECT_EQ(v.size(), 1U);
    EXPECT_EQ(v.back(), 1);
    v.pop_back();
    EXPECT_TRUE(v.empty());
}

// ============================================================
// clear() — destroys all live elements
// ============================================================

TEST(FixedVector, ClearEmptiesVector) {
    FixedVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v.clear();
    EXPECT_TRUE(v.empty());
    EXPECT_EQ(v.size(), 0U);
}

TEST(FixedVector, ClearThenPushBack) {
    FixedVector<int, 4> v;
    v.push_back(1);
    v.push_back(2);
    v.clear();
    v.push_back(99);
    EXPECT_EQ(v.size(), 1U);
    EXPECT_EQ(v[0], 99);
}

// ============================================================
// data() — contiguous layout
// ============================================================

TEST(FixedVector, DataIsContiguous) {
    FixedVector<int, 4> v;
    v.push_back(10);
    v.push_back(20);
    v.push_back(30);
    const int* p = v.data();
    EXPECT_EQ(p[0], 10);
    EXPECT_EQ(p[1], 20);
    EXPECT_EQ(p[2], 30);
    // Addresses are strictly adjacent
    EXPECT_EQ(p + 1, &v[1]);
    EXPECT_EQ(p + 2, &v[2]);
}

// ============================================================
// begin() / end() — iteration + std::accumulate
// ============================================================

TEST(FixedVector, IterationSum) {
    FixedVector<int, 8> v;
    for (int i = 1; i <= 5; ++i) {
        v.push_back(i);
    }
    int sum = std::accumulate(v.begin(), v.end(), 0);
    EXPECT_EQ(sum, 15);
}

TEST(FixedVector, ConstIterationSum) {
    FixedVector<int, 8> v;
    v.push_back(2);
    v.push_back(4);
    v.push_back(6);
    const auto& cv = v;
    int sum = std::accumulate(cv.begin(), cv.end(), 0);
    EXPECT_EQ(sum, 12);
}

// ============================================================
// Non-trivial element type — ctor/dtor counting
// ============================================================

struct TrackedFV {
    static int constructs;
    static int destructs;
    static void reset() { constructs = destructs = 0; }

    int value;
    explicit TrackedFV(int v) : value{v} { ++constructs; }
    TrackedFV(const TrackedFV& o) : value{o.value} { ++constructs; }
    TrackedFV(TrackedFV&& o) noexcept : value{o.value} { ++constructs; o.value = -1; }
    ~TrackedFV() { ++destructs; }
    TrackedFV& operator=(const TrackedFV&) = default;
    TrackedFV& operator=(TrackedFV&&) noexcept = default;
};
int TrackedFV::constructs = 0;
int TrackedFV::destructs  = 0;

TEST(FixedVector, NonTrivialDtorCalledOnPopBack) {
    TrackedFV::reset();
    {
        FixedVector<TrackedFV, 4> v;
        v.emplace_back(1);
        v.emplace_back(2);
        EXPECT_EQ(TrackedFV::constructs, 2);
        v.pop_back();
        EXPECT_EQ(TrackedFV::destructs, 1);
    }
    // Destructor of v calls ~TrackedFV for the remaining element
    EXPECT_EQ(TrackedFV::destructs, 2);
}

TEST(FixedVector, NonTrivialDtorCalledOnClear) {
    TrackedFV::reset();
    FixedVector<TrackedFV, 4> v;
    v.emplace_back(1);
    v.emplace_back(2);
    v.emplace_back(3);
    v.clear();
    EXPECT_EQ(TrackedFV::destructs, 3);
    EXPECT_TRUE(v.empty());
}

TEST(FixedVector, NonTrivialDtorCalledOnVectorDtor) {
    TrackedFV::reset();
    {
        FixedVector<TrackedFV, 4> v;
        v.emplace_back(10);
        v.emplace_back(20);
    }
    EXPECT_EQ(TrackedFV::destructs, 2);
}

// ============================================================
// Copy constructor / copy assignment
// ============================================================

TEST(FixedVector, CopyConstructor) {
    FixedVector<int, 4> a;
    a.push_back(1);
    a.push_back(2);
    a.push_back(3);

    FixedVector<int, 4> b{a};
    EXPECT_EQ(b.size(), 3U);
    EXPECT_EQ(b[0], 1);
    EXPECT_EQ(b[1], 2);
    EXPECT_EQ(b[2], 3);
}

TEST(FixedVector, CopyConstructorIndependent) {
    FixedVector<int, 4> a;
    a.push_back(10);
    FixedVector<int, 4> b{a};
    b[0] = 99;
    EXPECT_EQ(a[0], 10); // a unchanged
}

TEST(FixedVector, CopyAssignment) {
    FixedVector<int, 4> a;
    a.push_back(7);
    a.push_back(8);
    FixedVector<int, 4> b;
    b.push_back(1);
    b = a;
    EXPECT_EQ(b.size(), 2U);
    EXPECT_EQ(b[0], 7);
    EXPECT_EQ(b[1], 8);
}

TEST(FixedVector, CopyAssignmentSelf) {
    FixedVector<int, 4> a;
    a.push_back(5);
    // Self-assignment must not corrupt
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overloaded"
#endif
    a = a; // NOLINT
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    EXPECT_EQ(a.size(), 1U);
    EXPECT_EQ(a[0], 5);
}

TEST(FixedVector, CopyNonTrivialDtorCount) {
    TrackedFV::reset();
    {
        FixedVector<TrackedFV, 4> a;
        a.emplace_back(1);
        a.emplace_back(2);
        FixedVector<TrackedFV, 4> b{a}; // copy-constructs 2 more
        EXPECT_EQ(TrackedFV::constructs, 4);
    }
    // 4 dtors total (2 from a, 2 from b)
    EXPECT_EQ(TrackedFV::destructs, 4);
}

// ============================================================
// Move constructor / move assignment
// ============================================================

TEST(FixedVector, MoveConstructor) {
    FixedVector<int, 4> a;
    a.push_back(1);
    a.push_back(2);
    FixedVector<int, 4> b{std::move(a)};
    EXPECT_EQ(b.size(), 2U);
    EXPECT_EQ(b[0], 1);
    EXPECT_EQ(b[1], 2);
    // Moved-from must be valid and empty
    EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
}

TEST(FixedVector, MoveAssignment) {
    FixedVector<int, 4> a;
    a.push_back(3);
    a.push_back(4);
    FixedVector<int, 4> b;
    b.push_back(99);
    b = std::move(a);
    EXPECT_EQ(b.size(), 2U);
    EXPECT_EQ(b[0], 3);
    EXPECT_EQ(b[1], 4);
    EXPECT_TRUE(a.empty()); // NOLINT(bugprone-use-after-move)
}

TEST(FixedVector, MoveNonTrivialDtorCount) {
    TrackedFV::reset();
    {
        FixedVector<TrackedFV, 4> a;
        a.emplace_back(1);
        a.emplace_back(2);
        // Move constructs 2 new TrackedFV objects (move-ctor called twice)
        FixedVector<TrackedFV, 4> b{std::move(a)};
        // Source elements destroyed during move_from
        EXPECT_EQ(TrackedFV::destructs, 2); // src destroyed after move
        EXPECT_EQ(b.size(), 2U);
        EXPECT_TRUE(a.empty()); // NOLINT
    }
    // b goes out of scope: 2 more dtors
    EXPECT_EQ(TrackedFV::destructs, 4);
}

TEST(FixedVector, MoveAssignmentSelf) {
    FixedVector<int, 4> a;
    a.push_back(7);
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-move"
#endif
    a = std::move(a); // NOLINT
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    // Self-move-assignment: must remain valid (allowed to be empty or preserved)
    // We only require it doesn't crash/UB. Size consistency check:
    EXPECT_EQ(a.capacity(), 4U);
}

// ============================================================
// try_push_back move overload
// ============================================================

TEST(FixedVector, TryPushBackRvalueRef) {
    FixedVector<std::string, 2> v;
    std::string s = "world";
    EXPECT_TRUE(v.try_push_back(std::move(s)));
    EXPECT_EQ(v[0], "world");
}

// ============================================================
// Boundary: capacity == 1
// ============================================================

TEST(FixedVector, CapacityOneElement) {
    FixedVector<int, 1> v;
    EXPECT_TRUE(v.empty());
    v.push_back(42);
    EXPECT_TRUE(v.full());
    EXPECT_EQ(v[0], 42);
    EXPECT_FALSE(v.try_push_back(1));
    v.pop_back();
    EXPECT_TRUE(v.empty());
}
