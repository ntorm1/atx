#include "atx/core/arena.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

using namespace atx;       // NOLINT(google-build-using-namespace)
using namespace atx::core; // NOLINT(google-build-using-namespace)

// =====================================================================
//  Helpers
// =====================================================================

/// Returns true if p is aligned to `alignment` bytes.
static bool is_aligned(const void* const p, const usize alignment) noexcept {
    return (reinterpret_cast<std::uintptr_t>(p) % alignment) == 0U;
}

// =====================================================================
//  Seed tests (from spec)
// =====================================================================

TEST(Arena, AllocatesAligned) {
    alignas(64) std::byte buf[1024];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    void* p = a.allocate(16U, 16U);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 16U, 0U);
}

TEST(Arena, ReturnsNullWhenExhausted) {
    alignas(64) std::byte buf[32];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    EXPECT_NE(a.allocate(32U, 1U), nullptr);
    EXPECT_EQ(a.allocate(1U, 1U), nullptr);
}

TEST(Arena, ResetReclaims) {
    alignas(64) std::byte buf[64];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    (void)a.allocate(64U, 1U);
    a.reset();
    EXPECT_NE(a.allocate(64U, 1U), nullptr);
}

// =====================================================================
//  Alignment correctness — all common power-of-two alignments
// =====================================================================

TEST(Arena, Alignment_1) {
    alignas(64) std::byte buf[256];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    void* p = a.allocate(1U, 1U);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned(p, 1U));
}

TEST(Arena, Alignment_8) {
    alignas(64) std::byte buf[256];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    // Force a mis-alignment of the bump pointer first.
    (void)a.allocate(1U, 1U); // offset is now 1
    void* p = a.allocate(8U, 8U);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned(p, 8U));
}

TEST(Arena, Alignment_16) {
    alignas(64) std::byte buf[256];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    (void)a.allocate(3U, 1U); // offset now 3 — needs 13 bytes of padding for align-16
    void* p = a.allocate(16U, 16U);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned(p, 16U));
}

TEST(Arena, Alignment_64) {
    alignas(64) std::byte buf[512];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    (void)a.allocate(1U, 1U); // offset now 1 — needs 63 bytes padding
    void* p = a.allocate(64U, 64U);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned(p, 64U));
}

TEST(Arena, MultipleAlignedAllocations_AllCorrect) {
    alignas(64) std::byte buf[1024];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};

    void* p1 = a.allocate(3U,  1U);
    void* p8 = a.allocate(8U,  8U);
    void* p16 = a.allocate(16U, 16U);
    void* p64 = a.allocate(64U, 64U);

    ASSERT_NE(p1,  nullptr);
    ASSERT_NE(p8,  nullptr);
    ASSERT_NE(p16, nullptr);
    ASSERT_NE(p64, nullptr);

    EXPECT_TRUE(is_aligned(p1,  1U));
    EXPECT_TRUE(is_aligned(p8,  8U));
    EXPECT_TRUE(is_aligned(p16, 16U));
    EXPECT_TRUE(is_aligned(p64, 64U));
}

// =====================================================================
//  Non-overlap: consecutive allocations must not share memory
// =====================================================================

TEST(Arena, AllocationsDoNotOverlap) {
    alignas(64) std::byte buf[256];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};

    // Allocate three non-overlapping regions and verify pointer ordering.
    auto* p1 = static_cast<std::byte*>(a.allocate(32U, 8U));
    auto* p2 = static_cast<std::byte*>(a.allocate(32U, 8U));
    auto* p3 = static_cast<std::byte*>(a.allocate(32U, 8U));

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    ASSERT_NE(p3, nullptr);

    // Each pointer must start at or after the end of the previous region.
    EXPECT_GE(p2, p1 + 32U);
    EXPECT_GE(p3, p2 + 32U);
}

TEST(Arena, WriteToAllocatedRegions_NoCorruption) {
    // Write distinct byte patterns into two allocations and verify neither
    // overwrites the other — catches any overlap in the bump math.
    alignas(64) std::byte buf[512];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};

    constexpr usize kSize = 64U;
    auto* p1 = static_cast<std::byte*>(a.allocate(kSize, 8U));
    auto* p2 = static_cast<std::byte*>(a.allocate(kSize, 8U));
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);

    for (usize i = 0U; i < kSize; ++i) {
        p1[i] = std::byte{0xAAU};
        p2[i] = std::byte{0xBBU};
    }
    for (usize i = 0U; i < kSize; ++i) {
        EXPECT_EQ(p1[i], std::byte{0xAAU});
        EXPECT_EQ(p2[i], std::byte{0xBBU});
    }
}

// =====================================================================
//  Exhaustion & near-boundary conditions
// =====================================================================

TEST(Arena, ExhaustionAfterPadding_ReturnsNull) {
    // Buffer large enough for one 8-byte-aligned alloc after 1-byte offset,
    // but not two.
    alignas(16) std::byte buf[16];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    (void)a.allocate(1U, 1U);          // offset = 1; 15 bytes left
    void* p = a.allocate(8U, 8U);      // needs padding to 8; uses bytes [8,15]
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(a.allocate(1U, 1U), nullptr); // 0 bytes left
}

TEST(Arena, AllocExactCapacity_Succeeds) {
    alignas(8) std::byte buf[128];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    EXPECT_NE(a.allocate(128U, 1U), nullptr);
    EXPECT_EQ(a.allocate(1U,   1U), nullptr);
}

TEST(Arena, ZeroSizeAlloc_ReturnsNullptr) {
    alignas(8) std::byte buf[64];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    EXPECT_EQ(a.allocate(0U, 1U), nullptr);
    // Offset must not have advanced.
    EXPECT_EQ(a.used(), 0U);
}

// =====================================================================
//  used / capacity / remaining accounting
// =====================================================================

TEST(Arena, UsedCapacityRemaining_InitialState) {
    alignas(8) std::byte buf[256];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    EXPECT_EQ(a.capacity(),  256U);
    EXPECT_EQ(a.used(),        0U);
    EXPECT_EQ(a.remaining(), 256U);
}

TEST(Arena, UsedAccountsForPaddingAndSize) {
    alignas(64) std::byte buf[512];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};

    (void)a.allocate(1U, 1U);    // used = 1
    EXPECT_EQ(a.used(), 1U);

    (void)a.allocate(8U, 8U);    // aligned to 8 → start at 8, end at 16; used = 16
    EXPECT_EQ(a.used(), 16U);

    EXPECT_EQ(a.remaining(), 512U - 16U);
    EXPECT_EQ(a.capacity(),  512U);
}

TEST(Arena, RemainingPlusUsedEqualsCapacity) {
    alignas(64) std::byte buf[256];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    (void)a.allocate(37U, 1U);
    EXPECT_EQ(a.used() + a.remaining(), a.capacity());
}

TEST(Arena, UsedAfterReset_IsZero) {
    alignas(8) std::byte buf[64];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    (void)a.allocate(32U, 1U);
    a.reset();
    EXPECT_EQ(a.used(),        0U);
    EXPECT_EQ(a.remaining(), 64U);
}

// =====================================================================
//  create<T> — typed construction
// =====================================================================

TEST(Arena, Create_ConstructsValue) {
    alignas(16) std::byte buf[256];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    const int* p = a.create<int>(42);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 42);
}

TEST(Arena, Create_Alignment_IsCorrect) {
    alignas(64) std::byte buf[512];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    (void)a.allocate(1U, 1U); // knock offset off alignment
    const double* p = a.create<double>(3.14);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned(p, alignof(double)));
    EXPECT_DOUBLE_EQ(*p, 3.14);
}

TEST(Arena, Create_MultipleObjects_NoOverlap) {
    alignas(16) std::byte buf[256];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};

    const int*    pi  = a.create<int>(1);
    const double* pd  = a.create<double>(2.0);
    const int*    pi2 = a.create<int>(3);

    ASSERT_NE(pi,  nullptr);
    ASSERT_NE(pd,  nullptr);
    ASSERT_NE(pi2, nullptr);

    EXPECT_EQ(*pi,  1);
    EXPECT_DOUBLE_EQ(*pd, 2.0);
    EXPECT_EQ(*pi2, 3);
}

TEST(Arena, Create_ReturnsNull_WhenExhausted) {
    alignas(8) std::byte buf[4]; // only 4 bytes — too small for an 8-byte double
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    const double* p = a.create<double>(1.0);
    EXPECT_EQ(p, nullptr);
}

TEST(Arena, Create_AfterReset_Succeeds) {
    alignas(16) std::byte buf[64];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    for (int i = 0; i < 4; ++i) {
        (void)a.allocate(16U, 1U);
    }
    // Arena should be full (4 * 16 == 64).
    EXPECT_EQ(a.remaining(), 0U);
    a.reset();
    const int* p = a.create<int>(99);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(*p, 99);
}

TEST(Arena, Create_StructWithMultipleFields) {
    struct Point { int x; int y; };

    alignas(8) std::byte buf[256];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    const Point* p = a.create<Point>(Point{10, 20});
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->x, 10);
    EXPECT_EQ(p->y, 20);
}

// =====================================================================
//  Pointer constructor (data* + size overload)
// =====================================================================

TEST(Arena, PointerConstructor_WorksLikeSpan) {
    alignas(16) std::byte buf[128];
    Arena a{buf, sizeof(buf)};
    EXPECT_EQ(a.capacity(), 128U);
    void* p = a.allocate(16U, 16U);
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(is_aligned(p, 16U));
}

// =====================================================================
//  Reset then re-use: verify offset is truly zeroed
// =====================================================================

TEST(Arena, ResetMultipleTimes_OffsetStaysZero) {
    alignas(8) std::byte buf[64];
    Arena a{std::span<std::byte>(buf, sizeof(buf))};
    for (int round = 0; round < 5; ++round) {
        (void)a.allocate(64U, 1U);
        EXPECT_EQ(a.remaining(), 0U);
        a.reset();
        EXPECT_EQ(a.used(), 0U);
    }
}
