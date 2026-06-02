#include "atx/core/aligned.hpp"

#include <cstdint>
#include <numeric>

#include <gtest/gtest.h>

using namespace atx;       // NOLINT(google-build-using-namespace)
using namespace atx::core; // NOLINT(google-build-using-namespace)

// =====================================================================
//  AlignedBuffer — cache alignment & basic properties
// =====================================================================

TEST(Aligned, BufferIsCacheAligned) {
    AlignedBuffer<double, 16> b;
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b.data()) % kCacheLineSize, 0U);
    EXPECT_EQ(b.size(), 16U);
}

TEST(Aligned, BufferSize_EqualsN) {
    AlignedBuffer<int, 8>   b8;
    AlignedBuffer<float, 1> b1;
    EXPECT_EQ(b8.size(), 8U);
    EXPECT_EQ(b1.size(), 1U);
    // size() is static — callable without an instance
    EXPECT_EQ((AlignedBuffer<double, 32>::size()), 32U);
}

TEST(Aligned, BufferElementReadWrite) {
    AlignedBuffer<int, 4> b;
    b[0] = 10;
    b[1] = 20;
    b[2] = 30;
    b[3] = 40;
    EXPECT_EQ(b[0], 10);
    EXPECT_EQ(b[1], 20);
    EXPECT_EQ(b[2], 30);
    EXPECT_EQ(b[3], 40);
}

TEST(Aligned, BufferDefaultInitialised) {
    // Trivial types are zero-initialised by std::array<T,N>{} initialiser.
    AlignedBuffer<int, 8> b;
    for (usize i = 0U; i < b.size(); ++i) {
        EXPECT_EQ(b[i], 0);
    }
}

TEST(Aligned, BufferSpanRoundTrip) {
    AlignedBuffer<float, 8> b;
    std::span<float> s = b.span();
    EXPECT_EQ(s.size(), 8U);
    EXPECT_EQ(s.data(), b.data());
    // Write through span, read through buffer
    s[3] = 3.14F;
    EXPECT_FLOAT_EQ(b[3], 3.14F);
}

TEST(Aligned, BufferConstSpanRoundTrip) {
    AlignedBuffer<double, 4> b;
    b[0] = 1.0;
    b[2] = 2.0;
    const AlignedBuffer<double, 4>& cb = b;
    std::span<const double> s = cb.span();
    EXPECT_EQ(s.size(), 4U);
    EXPECT_DOUBLE_EQ(s[0], 1.0);
    EXPECT_DOUBLE_EQ(s[2], 2.0);
}

TEST(Aligned, BufferIterators) {
    AlignedBuffer<int, 5> b;
    // Fill via iterators
    std::iota(b.begin(), b.end(), 1);
    EXPECT_EQ(b[0], 1);
    EXPECT_EQ(b[4], 5);
    // Range-for (uses begin/end)
    int sum = 0;
    for (const int v : b) {
        sum += v;
    }
    EXPECT_EQ(sum, 15); // 1+2+3+4+5
}

TEST(Aligned, Buffer_DoubleAlignment_64bytes) {
    // double[8] is 64 bytes — exactly one cache line; check alignment for
    // several stack-allocated instances.
    AlignedBuffer<double, 8> b1;
    AlignedBuffer<double, 8> b2;
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b1.data()) % kCacheLineSize, 0U);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(b2.data()) % kCacheLineSize, 0U);
}

// =====================================================================
//  aligned_alloc_bytes / aligned_free — portable aligned heap allocation
// =====================================================================

TEST(Aligned, AllocFreeRoundTrip) {
    void* p = aligned_alloc_bytes(256U, 64U);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 64U, 0U);
    aligned_free(p);
}

TEST(Aligned, AllocAlignment_16) {
    void* p = aligned_alloc_bytes(128U, 16U);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 16U, 0U);
    aligned_free(p);
}

TEST(Aligned, AllocAlignment_32) {
    void* p = aligned_alloc_bytes(256U, 32U);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 32U, 0U);
    aligned_free(p);
}

TEST(Aligned, AllocAlignment_128) {
    void* p = aligned_alloc_bytes(512U, 128U);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % 128U, 0U);
    aligned_free(p);
}

TEST(Aligned, AllocZeroSize_ReturnsNullptr) {
    // size == 0 is explicitly returned as nullptr by our implementation for
    // portability (MSVC _aligned_malloc(0,…) is implementation-defined).
    void* p = aligned_alloc_bytes(0U, 64U);
    EXPECT_EQ(p, nullptr);
    // Must be safe to free nullptr
    aligned_free(p);
}

TEST(Aligned, FreeNullptr_IsSafe) {
    // aligned_free(nullptr) must be a no-op (both _aligned_free and free
    // are documented to accept nullptr).
    aligned_free(nullptr);
    SUCCEED(); // reaching here without crash is the test
}

TEST(Aligned, AllocWriteRead_BytePattern) {
    constexpr usize kBytes = 1024U;
    constexpr usize kAlign = 64U;
    void* p = aligned_alloc_bytes(kBytes, kAlign);
    ASSERT_NE(p, nullptr);

    // Write a pattern and read it back to confirm the memory is usable.
    auto* bytes = static_cast<unsigned char*>(p);
    for (usize i = 0U; i < kBytes; ++i) {
        bytes[i] = static_cast<unsigned char>(i & 0xFFU);
    }
    for (usize i = 0U; i < kBytes; ++i) {
        EXPECT_EQ(bytes[i], static_cast<unsigned char>(i & 0xFFU));
    }

    aligned_free(p);
}

TEST(Aligned, MultipleAllocsAllAligned) {
    constexpr usize kAlign = 64U;
    constexpr usize kCount = 8U;
    std::array<void*, kCount> ptrs{};

    for (usize i = 0U; i < kCount; ++i) {
        ptrs[i] = aligned_alloc_bytes(128U, kAlign);
        ASSERT_NE(ptrs[i], nullptr);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(ptrs[i]) % kAlign, 0U);
    }

    for (usize i = 0U; i < kCount; ++i) {
        aligned_free(ptrs[i]);
    }
}
