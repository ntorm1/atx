// column_test.cpp — TDD tests for atx::core::series::Column<T>
//
// Order: seed tests (from spec) + full coverage: capacity growth (doubling,
// monotonic), reserve, append_bulk, move/copy semantics, clear, sustained
// alignment across growths, at() out-of-range error, and the optional
// validity bitmap (append_null / is_valid).

#include <atx/core/series/column.hpp>

#include <cstdint> // std::uintptr_t
#include <span>
#include <vector>

#include <gtest/gtest.h>

using atx::core::series::Column;
using atx::usize;
using namespace atx::core; // ErrorCode, kCacheLineSize

// ============================================================
// Seed tests (from spec)
// ============================================================

TEST(Column, AppendAndView) {
    Column<double> c;
    for (int i = 0; i < 5; ++i) {
        c.append(static_cast<double>(i));
    }
    EXPECT_EQ(c.size(), 5U);
    ASSERT_EQ(c.view().size(), 5U);
    EXPECT_DOUBLE_EQ(c.view()[4], 4.0);
}

TEST(Column, CacheAlignedData) {
    Column<double> c;
    c.append(1.0);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(c.data()) % kCacheLineSize, 0U);
}

TEST(Column, AtBoundsChecked) {
    Column<double> c;
    c.append(1.0);
    EXPECT_FALSE(c.at(5).has_value());
    EXPECT_EQ(c.at(5).error().code(), ErrorCode::OutOfRange);
}

// ============================================================
// Empty-state observers
// ============================================================

TEST(Column, DefaultConstructedIsEmpty) {
    Column<int> c;
    EXPECT_TRUE(c.empty());
    EXPECT_EQ(c.size(), 0U);
    EXPECT_EQ(c.capacity(), 0U);
    EXPECT_EQ(c.data(), nullptr);
    EXPECT_TRUE(c.view().empty());
}

TEST(Column, ReserveZeroDoesNotCrash) {
    Column<int> c;
    c.reserve(0U);
    EXPECT_EQ(c.capacity(), 0U);
    EXPECT_TRUE(c.empty());
}

TEST(Column, AtOnEmptyIsOutOfRange) {
    Column<int> c;
    EXPECT_FALSE(c.at(0U).has_value());
    EXPECT_EQ(c.at(0U).error().code(), ErrorCode::OutOfRange);
}

// ============================================================
// Capacity growth
// ============================================================

TEST(Column, CapacityGrowsAndIsMonotonic) {
    Column<int> c;
    usize last_cap = c.capacity();
    for (int i = 0; i < 1000; ++i) {
        c.append(i);
        EXPECT_GE(c.capacity(), c.size());
        EXPECT_GE(c.capacity(), last_cap); // never shrinks during growth
        last_cap = c.capacity();
    }
    EXPECT_EQ(c.size(), 1000U);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(c[static_cast<usize>(i)], i);
    }
}

TEST(Column, CapacityDoublesOnGrowth) {
    Column<int> c;
    c.append(0); // first growth: 0 -> some base capacity
    const usize base = c.capacity();
    // Fill to capacity, then force one more growth: capacity must at least
    // double (doubling strategy).
    while (c.size() < base) {
        c.append(0);
    }
    EXPECT_EQ(c.capacity(), base);
    c.append(0); // triggers growth
    EXPECT_GE(c.capacity(), base * 2U);
}

TEST(Column, ReserveAvoidsReallocation) {
    Column<int> c;
    c.reserve(128U);
    EXPECT_GE(c.capacity(), 128U);
    const int* const base_ptr = c.data();
    const usize cap_after_reserve = c.capacity();
    for (int i = 0; i < 128; ++i) {
        c.append(i);
    }
    EXPECT_EQ(c.capacity(), cap_after_reserve); // no reallocation
    EXPECT_EQ(c.data(), base_ptr);              // buffer untouched
    EXPECT_EQ(c.size(), 128U);
}

TEST(Column, ReserveSmallerIsNoop) {
    Column<int> c;
    c.reserve(64U);
    const usize cap = c.capacity();
    c.reserve(8U); // smaller than current capacity
    EXPECT_EQ(c.capacity(), cap);
}

// ============================================================
// append_bulk
// ============================================================

TEST(Column, AppendBulkFromSpan) {
    Column<int> c;
    const std::vector<int> src = {10, 20, 30, 40, 50};
    c.append_bulk(std::span<const int>{src});
    ASSERT_EQ(c.size(), 5U);
    for (usize i = 0U; i < src.size(); ++i) {
        EXPECT_EQ(c[i], src[i]);
    }
}

TEST(Column, AppendBulkGrowsOnce) {
    Column<int> c;
    std::vector<int> src(500);
    for (int i = 0; i < 500; ++i) {
        src[static_cast<usize>(i)] = i;
    }
    c.append_bulk(std::span<const int>{src});
    EXPECT_EQ(c.size(), 500U);
    EXPECT_GE(c.capacity(), 500U);
    EXPECT_EQ(c.view()[499], 499);
}

TEST(Column, AppendBulkEmptySpanIsNoop) {
    Column<int> c;
    c.append(1);
    c.append_bulk(std::span<const int>{}); // empty
    EXPECT_EQ(c.size(), 1U);
    EXPECT_EQ(c[0], 1);
}

TEST(Column, AppendBulkAfterExistingData) {
    Column<int> c;
    c.append(1);
    c.append(2);
    const std::vector<int> more = {3, 4, 5};
    c.append_bulk(std::span<const int>{more});
    ASSERT_EQ(c.size(), 5U);
    EXPECT_EQ(c[0], 1);
    EXPECT_EQ(c[4], 5);
}

// ============================================================
// Move semantics — buffer transfer (pointer stolen)
// ============================================================

TEST(Column, MoveCtorTransfersBuffer) {
    Column<int> src;
    for (int i = 0; i < 100; ++i) {
        src.append(i);
    }
    const int* const stolen = src.data();
    const usize n = src.size();

    Column<int> dst{std::move(src)};
    EXPECT_EQ(dst.data(), stolen); // pointer stolen, no copy
    EXPECT_EQ(dst.size(), n);
    EXPECT_EQ(dst[99], 99);

    // Moved-from source is left empty and valid (NOLINT: intentional use after move).
    EXPECT_TRUE(src.empty());       // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(src.data(), nullptr); // NOLINT(bugprone-use-after-move)
}

TEST(Column, MoveAssignTransfersBuffer) {
    Column<int> src;
    for (int i = 0; i < 50; ++i) {
        src.append(i);
    }
    const int* const stolen = src.data();

    Column<int> dst;
    dst.append(999);
    dst = std::move(src);
    EXPECT_EQ(dst.data(), stolen);
    EXPECT_EQ(dst.size(), 50U);
    EXPECT_EQ(dst[0], 0);
    EXPECT_TRUE(src.empty()); // NOLINT(bugprone-use-after-move)
}

// ============================================================
// Copy semantics — deep, independent buffers
// ============================================================

TEST(Column, CopyCtorDeepCopies) {
    Column<int> src;
    for (int i = 0; i < 10; ++i) {
        src.append(i);
    }
    Column<int> copy{src};
    ASSERT_EQ(copy.size(), src.size());
    EXPECT_NE(copy.data(), src.data()); // independent buffer

    copy[0] = 777;          // mutate copy
    EXPECT_EQ(src[0], 0);   // source unchanged
    EXPECT_EQ(copy[0], 777);
}

TEST(Column, CopyAssignDeepCopies) {
    Column<int> src;
    for (int i = 0; i < 10; ++i) {
        src.append(i);
    }
    Column<int> dst;
    dst.append(42);
    dst = src;
    ASSERT_EQ(dst.size(), 10U);
    EXPECT_NE(dst.data(), src.data());
    dst[5] = -1;
    EXPECT_EQ(src[5], 5); // independent
}

TEST(Column, SelfCopyAssignIsSafe) {
    Column<int> c;
    for (int i = 0; i < 5; ++i) {
        c.append(i);
    }
    const Column<int>& alias = c;
    c = alias; // self-assignment
    ASSERT_EQ(c.size(), 5U);
    EXPECT_EQ(c[4], 4);
}

// ============================================================
// clear — keeps capacity
// ============================================================

TEST(Column, ClearKeepsCapacity) {
    Column<int> c;
    for (int i = 0; i < 20; ++i) {
        c.append(i);
    }
    const usize cap = c.capacity();
    const int* const ptr = c.data();
    c.clear();
    EXPECT_EQ(c.size(), 0U);
    EXPECT_TRUE(c.empty());
    EXPECT_EQ(c.capacity(), cap); // capacity retained
    EXPECT_EQ(c.data(), ptr);     // buffer retained
    c.append(123);                // reusable after clear
    EXPECT_EQ(c[0], 123);
}

// ============================================================
// Alignment sustained across growth
// ============================================================

TEST(Column, DataStaysAlignedAfterGrowths) {
    Column<double> c;
    for (int i = 0; i < 5000; ++i) {
        c.append(static_cast<double>(i));
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(c.data()) % kCacheLineSize, 0U);
    }
    EXPECT_EQ(c.size(), 5000U);
}

// ============================================================
// Element access and views
// ============================================================

TEST(Column, AtInRangeReturnsValue) {
    Column<int> c;
    c.append(11);
    c.append(22);
    const auto r = c.at(1U);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), 22);
}

TEST(Column, MutableViewMutatesUnderlyingBuffer) {
    Column<int> c;
    c.append(1);
    c.append(2);
    std::span<int> mv = c.mutable_view();
    ASSERT_EQ(mv.size(), 2U);
    mv[1] = 99;
    EXPECT_EQ(c[1], 99);
}

// ============================================================
// Optional validity bitmap
// ============================================================

TEST(Column, AppendedValuesAreValid) {
    Column<int> c;
    c.append(1);
    c.append(2);
    EXPECT_TRUE(c.is_valid(0U));
    EXPECT_TRUE(c.is_valid(1U));
}

TEST(Column, AppendNullIsInvalid) {
    Column<int> c;
    c.append(1);
    c.append_null();
    c.append(3);
    EXPECT_EQ(c.size(), 3U);
    EXPECT_TRUE(c.is_valid(0U));
    EXPECT_FALSE(c.is_valid(1U)); // the null slot
    EXPECT_TRUE(c.is_valid(2U));
}
