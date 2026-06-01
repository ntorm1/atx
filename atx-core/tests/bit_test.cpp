#include "atx/core/bit.hpp"

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

// atx:: holds the vocabulary types (u8/u16/u32/u64/…).
// atx::core:: holds the bit functions.
using namespace atx;       // NOLINT(google-build-using-namespace)
using namespace atx::core; // NOLINT(google-build-using-namespace)

// ---- next_pow2 -------------------------------------------------------------

TEST(Bit, NextPow2_SeedCases) {
    EXPECT_EQ(next_pow2(1u), 1u);
    EXPECT_EQ(next_pow2(5u), 8u);
    EXPECT_EQ(next_pow2(8u), 8u);
    EXPECT_EQ(next_pow2(1023u), 1024u);
}

TEST(Bit, NextPow2_ZeroReturnsOne) {
    // Documented: next_pow2(0) == 1  (std::bit_ceil(0) == 1 per [bit.ceil])
    EXPECT_EQ(next_pow2(0u), 1u);
    EXPECT_EQ(next_pow2(u32{0}), u32{1});
    EXPECT_EQ(next_pow2(u64{0}), u64{1});
}

TEST(Bit, NextPow2_OneReturnsOne) {
    EXPECT_EQ(next_pow2(u32{1}), u32{1});
    EXPECT_EQ(next_pow2(u64{1}), u64{1});
}

TEST(Bit, NextPow2_PowerOfTwoIsIdentity) {
    EXPECT_EQ(next_pow2(u32{2}),    u32{2});
    EXPECT_EQ(next_pow2(u32{4}),    u32{4});
    EXPECT_EQ(next_pow2(u32{256}),  u32{256});
    EXPECT_EQ(next_pow2(u64{1024}), u64{1024});
}

TEST(Bit, NextPow2_LargeU64) {
    // 2^63 should be its own next power-of-two
    constexpr u64 p63 = u64{1} << 63u;
    EXPECT_EQ(next_pow2(p63), p63);
    EXPECT_EQ(next_pow2(p63 - 1u), p63);
}

// ---- is_pow2 ----------------------------------------------------------------

TEST(Bit, IsPow2_SeedCases) {
    EXPECT_TRUE(is_pow2(1u));
    EXPECT_TRUE(is_pow2(64u));
    EXPECT_FALSE(is_pow2(0u));
    EXPECT_FALSE(is_pow2(3u));
}

TEST(Bit, IsPow2_ZeroIsFalse) {
    // std::has_single_bit(0) == false; 0 is not a power of two.
    EXPECT_FALSE(is_pow2(u32{0}));
    EXPECT_FALSE(is_pow2(u64{0}));
}

TEST(Bit, IsPow2_AllPowersOfTwo_u32) {
    for (u32 i = 0; i < 32u; ++i) {
        EXPECT_TRUE(is_pow2(u32{1} << i));
    }
}

TEST(Bit, IsPow2_NonPowersOfTwo) {
    EXPECT_FALSE(is_pow2(u32{3}));
    EXPECT_FALSE(is_pow2(u32{5}));
    EXPECT_FALSE(is_pow2(u32{6}));
    EXPECT_FALSE(is_pow2(u32{7}));
    EXPECT_FALSE(is_pow2(u32{9}));
    EXPECT_FALSE(is_pow2(std::numeric_limits<u32>::max())); // 0xFFFFFFFF
}

// ---- popcount ---------------------------------------------------------------

TEST(Bit, Popcount_SeedCase) {
    EXPECT_EQ(popcount(0b1011u), u32{3});
}

TEST(Bit, Popcount_Zero) {
    EXPECT_EQ(popcount(u32{0}), u32{0});
    EXPECT_EQ(popcount(u64{0}), u32{0});
}

TEST(Bit, Popcount_AllOnes_u32) {
    EXPECT_EQ(popcount(std::numeric_limits<u32>::max()), u32{32});
}

TEST(Bit, Popcount_AllOnes_u64) {
    EXPECT_EQ(popcount(std::numeric_limits<u64>::max()), u32{64});
}

TEST(Bit, Popcount_SingleBit) {
    EXPECT_EQ(popcount(u32{1}), u32{1});
    EXPECT_EQ(popcount(u32{1} << 31u), u32{1});
}

// ---- clz --------------------------------------------------------------------

TEST(Bit, Clz_Zero) {
    // countl_zero(0) == bit-width of the type
    EXPECT_EQ(clz(u32{0}), u32{32});
    EXPECT_EQ(clz(u64{0}), u32{64});
}

TEST(Bit, Clz_One) {
    EXPECT_EQ(clz(u32{1}), u32{31});
    EXPECT_EQ(clz(u64{1}), u32{63});
}

TEST(Bit, Clz_HighBitSet) {
    EXPECT_EQ(clz(u32{1} << 31u), u32{0});
    EXPECT_EQ(clz(u64{1} << 63u), u32{0});
}

TEST(Bit, Clz_MaxValue) {
    EXPECT_EQ(clz(std::numeric_limits<u32>::max()), u32{0});
    EXPECT_EQ(clz(std::numeric_limits<u64>::max()), u32{0});
}

// ---- ctz --------------------------------------------------------------------

TEST(Bit, Ctz_Zero) {
    EXPECT_EQ(ctz(u32{0}), u32{32});
    EXPECT_EQ(ctz(u64{0}), u32{64});
}

TEST(Bit, Ctz_One) {
    EXPECT_EQ(ctz(u32{1}), u32{0});
    EXPECT_EQ(ctz(u64{1}), u32{0});
}

TEST(Bit, Ctz_HighBitOnly) {
    EXPECT_EQ(ctz(u32{1} << 31u), u32{31});
    EXPECT_EQ(ctz(u64{1} << 63u), u32{63});
}

TEST(Bit, Ctz_MaxValue) {
    // All ones — lowest set bit is bit 0 → ctz == 0
    EXPECT_EQ(ctz(std::numeric_limits<u32>::max()), u32{0});
    EXPECT_EQ(ctz(std::numeric_limits<u64>::max()), u32{0});
}

// ---- bit_width --------------------------------------------------------------

TEST(Bit, BitWidth_Zero) {
    EXPECT_EQ(bit_width(u32{0}), u32{0});
    EXPECT_EQ(bit_width(u64{0}), u32{0});
}

TEST(Bit, BitWidth_One) {
    EXPECT_EQ(bit_width(u32{1}), u32{1});
    EXPECT_EQ(bit_width(u64{1}), u32{1});
}

TEST(Bit, BitWidth_PowersOfTwo) {
    EXPECT_EQ(bit_width(u32{2}),   u32{2});
    EXPECT_EQ(bit_width(u32{4}),   u32{3});
    EXPECT_EQ(bit_width(u32{8}),   u32{4});
    EXPECT_EQ(bit_width(u32{256}), u32{9});
}

TEST(Bit, BitWidth_MaxU32) {
    EXPECT_EQ(bit_width(std::numeric_limits<u32>::max()), u32{32});
}

// ---- byteswap ---------------------------------------------------------------

TEST(Bit, Byteswap_u16) {
    EXPECT_EQ(byteswap(u16{0x1234}), u16{0x3412});
}

TEST(Bit, Byteswap_u32) {
    EXPECT_EQ(byteswap(u32{0x12345678u}), u32{0x78563412u});
}

TEST(Bit, Byteswap_u64) {
    EXPECT_EQ(byteswap(u64{0x0102030405060708ULL}), u64{0x0807060504030201ULL});
}

TEST(Bit, Byteswap_u8_IsIdentity) {
    // Single-byte swap is identity.
    EXPECT_EQ(byteswap(u8{0xAB}), u8{0xAB});
}

TEST(Bit, Byteswap_Zero) {
    EXPECT_EQ(byteswap(u32{0}), u32{0});
    EXPECT_EQ(byteswap(u64{0}), u64{0});
}

TEST(Bit, Byteswap_MaxValue) {
    // All-ones is its own byte-swap.
    EXPECT_EQ(byteswap(std::numeric_limits<u32>::max()),
              std::numeric_limits<u32>::max());
    EXPECT_EQ(byteswap(std::numeric_limits<u64>::max()),
              std::numeric_limits<u64>::max());
}

// ---- constexpr usability ----------------------------------------------------

TEST(Bit, ConstexprEvaluation) {
    // Verify all helpers are truly constexpr-evaluable.
    static_assert(next_pow2(u32{5})  == u32{8});
    static_assert(is_pow2(u32{64}));
    static_assert(!is_pow2(u32{0}));
    static_assert(popcount(u32{0b1011u}) == u32{3});
    static_assert(clz(u32{1})   == u32{31});
    static_assert(ctz(u32{4})   == u32{2});
    static_assert(bit_width(u32{8}) == u32{4});
    static_assert(byteswap(u32{0x12345678u}) == u32{0x78563412u});
    SUCCEED();
}
