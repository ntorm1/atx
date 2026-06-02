// Tests for atx::core safe_math (checked & saturating integer arithmetic).
//
// Coverage strategy (agent profile §7):
//   - Happy path for all six operations.
//   - Boundary: max/min ± 1 for both positive and negative overflow.
//   - Special values: 0, 1, -1, INT_MIN, INT_MAX, UINT_MAX.
//   - Both signed (i32, i64) and unsigned (u32, u64) types.
//   - sat_*: verify no overflow occurs; verify saturation at both ends.
//   - checked_*: verify error code is OutOfRange.

#include "atx/core/safe_math.hpp"

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

// atx:: holds the vocabulary types (i32/u32/…).
// atx::core:: holds the safe_math functions.
using namespace atx;       // NOLINT(google-build-using-namespace)
using namespace atx::core; // NOLINT(google-build-using-namespace)

// ============================================================
//  checked_add — signed i32
// ============================================================

TEST(SafeMath, CheckedAddOk) {
    const auto r = checked_add<i32>(2, 3);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 5);
}

TEST(SafeMath, CheckedAdd_PositiveOverflowErrs) {
    const auto r = checked_add<i32>(INT32_MAX, 1);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(SafeMath, CheckedAdd_NegativeOverflowErrs) {
    const auto r = checked_add<i32>(INT32_MIN, -1);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(SafeMath, CheckedAdd_AtMaxIsOk) {
    // max + 0 is fine.
    const auto r = checked_add<i32>(INT32_MAX, 0);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, INT32_MAX);
}

TEST(SafeMath, CheckedAdd_NegativePlusPositiveOk) {
    const auto r = checked_add<i32>(-100, 50);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, -50);
}

// ============================================================
//  checked_add — signed i64
// ============================================================

TEST(SafeMath, CheckedAdd_I64_OverflowErrs) {
    const auto r = checked_add<i64>(INT64_MAX, 1LL);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(SafeMath, CheckedAdd_I64_MinMinusOneErrs) {
    const auto r = checked_add<i64>(INT64_MIN, -1LL);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

// ============================================================
//  checked_add — unsigned u32
// ============================================================

TEST(SafeMath, CheckedAdd_U32_Ok) {
    const auto r = checked_add<u32>(100U, 200U);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 300U);
}

TEST(SafeMath, CheckedAdd_U32_WrapErrs) {
    const auto r = checked_add<u32>(UINT32_MAX, 1U);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(SafeMath, CheckedAdd_U32_ZeroOk) {
    const auto r = checked_add<u32>(0U, 0U);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0U);
}

// ============================================================
//  checked_sub — signed i32
// ============================================================

TEST(SafeMath, CheckedSub_Ok) {
    const auto r = checked_sub<i32>(10, 3);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 7);
}

TEST(SafeMath, CheckedSub_NegativeUnderflowErrs) {
    // INT32_MIN - 1 underflows.
    const auto r = checked_sub<i32>(INT32_MIN, 1);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(SafeMath, CheckedSub_PositiveOverflowErrs) {
    // INT32_MAX - (-1) overflows.
    const auto r = checked_sub<i32>(INT32_MAX, -1);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(SafeMath, CheckedSub_MinusItself_IsZero) {
    const auto r = checked_sub<i32>(INT32_MIN, INT32_MIN);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0);
}

// ============================================================
//  checked_sub — unsigned u32
// ============================================================

TEST(SafeMath, CheckedSub_U32_Ok) {
    const auto r = checked_sub<u32>(10U, 3U);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 7U);
}

TEST(SafeMath, CheckedSub_U32_UnderflowErrs) {
    const auto r = checked_sub<u32>(0U, 1U);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(SafeMath, CheckedSub_U32_EqualIsZero) {
    const auto r = checked_sub<u32>(42U, 42U);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0U);
}

// ============================================================
//  checked_mul — signed
// ============================================================

TEST(SafeMath, CheckedMul_Ok) {
    const auto r = checked_mul<i32>(6, 7);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
}

TEST(SafeMath, CheckedMul_ByZero_IsZero) {
    const auto r = checked_mul<i32>(INT32_MAX, 0);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0);
}

TEST(SafeMath, CheckedMul_ByOne_IsIdentity) {
    const auto r = checked_mul<i32>(INT32_MAX, 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, INT32_MAX);
}

TEST(SafeMath, CheckedMul_ByNegOne_Signed_IsNegation) {
    // -INT32_MAX * -1 = INT32_MAX; that's fine.
    const auto r = checked_mul<i32>(-INT32_MAX, -1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, INT32_MAX);
}

TEST(SafeMath, CheckedMul_MinByNegOne_Overflows) {
    // INT32_MIN * -1 = INT32_MAX + 1, which overflows.
    const auto r = checked_mul<i32>(INT32_MIN, -1);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(SafeMath, CheckedMul_OverflowErrs_I64) {
    const auto r = checked_mul<i64>(INT64_MAX, 2LL);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(SafeMath, CheckedMul_NegativeOverflowErrs) {
    // INT32_MIN * 2 overflows negatively.
    const auto r = checked_mul<i32>(INT32_MIN, 2);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

// ============================================================
//  checked_mul — unsigned
// ============================================================

TEST(SafeMath, CheckedMul_U32_Ok) {
    const auto r = checked_mul<u32>(1000U, 1000U);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 1000000U);
}

TEST(SafeMath, CheckedMul_U32_WrapErrs) {
    const auto r = checked_mul<u32>(UINT32_MAX, 2U);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(SafeMath, CheckedMul_U32_ByZero_IsZero) {
    const auto r = checked_mul<u32>(UINT32_MAX, 0U);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 0U);
}

// ============================================================
//  sat_add — signed
// ============================================================

TEST(SafeMath, SaturatingAdd_NoOverflow) {
    EXPECT_EQ(sat_add<i32>(10, 20), 30);
}

TEST(SafeMath, SaturatingAdd_PositiveSaturates) {
    EXPECT_EQ(sat_add<i32>(INT32_MAX, 10), INT32_MAX);
}

TEST(SafeMath, SaturatingAdd_NegativeSaturates) {
    EXPECT_EQ(sat_add<i32>(INT32_MIN, -10), INT32_MIN);
}

TEST(SafeMath, SaturatingAdd_AtMaxBoundary) {
    // max + 0 should not saturate.
    EXPECT_EQ(sat_add<i32>(INT32_MAX, 0), INT32_MAX);
}

TEST(SafeMath, SaturatingAdd_I64_Saturates) {
    EXPECT_EQ(sat_add<i64>(INT64_MAX, 1LL), INT64_MAX);
    EXPECT_EQ(sat_add<i64>(INT64_MIN, -1LL), INT64_MIN);
}

// ============================================================
//  sat_add — unsigned
// ============================================================

TEST(SafeMath, SaturatingAdd_U32_NoWrap) {
    EXPECT_EQ(sat_add<u32>(100U, 200U), 300U);
}

TEST(SafeMath, SaturatingAdd_U32_Saturates) {
    EXPECT_EQ(sat_add<u32>(UINT32_MAX, 1U), UINT32_MAX);
}

// ============================================================
//  sat_sub — signed
// ============================================================

TEST(SafeMath, SaturatingSub_NoOverflow) {
    EXPECT_EQ(sat_sub<i32>(10, 3), 7);
}

TEST(SafeMath, SaturatingSub_UnderflowSaturatesToMin) {
    EXPECT_EQ(sat_sub<i32>(INT32_MIN, 1), INT32_MIN);
}

TEST(SafeMath, SaturatingSub_OverflowSaturatesToMax) {
    // INT32_MAX - (-1) overflows; saturates to max.
    EXPECT_EQ(sat_sub<i32>(INT32_MAX, -1), INT32_MAX);
}

TEST(SafeMath, SaturatingSub_I64_UnderflowSaturates) {
    EXPECT_EQ(sat_sub<i64>(INT64_MIN, 1LL), INT64_MIN);
}

// ============================================================
//  sat_sub — unsigned
// ============================================================

TEST(SafeMath, SaturatingSub_U32_NoWrap) {
    EXPECT_EQ(sat_sub<u32>(10U, 3U), 7U);
}

TEST(SafeMath, SaturatingSub_U32_UnderflowSaturatesToZero) {
    EXPECT_EQ(sat_sub<u32>(0U, 1U), 0U);
    EXPECT_EQ(sat_sub<u32>(5U, 10U), 0U);
}

// ============================================================
//  sat_mul — signed
// ============================================================

TEST(SafeMath, SaturatingMul_NoOverflow) {
    EXPECT_EQ(sat_mul<i32>(6, 7), 42);
}

TEST(SafeMath, SaturatingMul_ByZero_IsZero) {
    EXPECT_EQ(sat_mul<i32>(INT32_MAX, 0), 0);
}

TEST(SafeMath, SaturatingMul_ByOne_IsIdentity) {
    EXPECT_EQ(sat_mul<i32>(INT32_MAX, 1), INT32_MAX);
}

TEST(SafeMath, SaturatingMul_PositiveOverflowSaturatesToMax) {
    EXPECT_EQ(sat_mul<i32>(INT32_MAX, 2), INT32_MAX);
}

TEST(SafeMath, SaturatingMul_NegativeOverflowSaturatesToMin) {
    // Positive * negative → negative overflow.
    EXPECT_EQ(sat_mul<i32>(INT32_MAX, -2), INT32_MIN);
}

TEST(SafeMath, SaturatingMul_MinByNegOne_SaturatesToMax) {
    // INT32_MIN * -1 would overflow; saturates to max.
    EXPECT_EQ(sat_mul<i32>(INT32_MIN, -1), INT32_MAX);
}

TEST(SafeMath, SaturatingMul_BothNegative_SaturatesToMax) {
    // Both negative → positive overflow.
    EXPECT_EQ(sat_mul<i32>(INT32_MIN, -2), INT32_MAX);
}

TEST(SafeMath, SaturatingMul_I64_Saturates) {
    EXPECT_EQ(sat_mul<i64>(INT64_MAX, 2LL), INT64_MAX);
}

// ============================================================
//  sat_mul — unsigned
// ============================================================

TEST(SafeMath, SaturatingMul_U32_NoWrap) {
    EXPECT_EQ(sat_mul<u32>(1000U, 1000U), 1000000U);
}

TEST(SafeMath, SaturatingMul_U32_WrapSaturatesToMax) {
    EXPECT_EQ(sat_mul<u32>(UINT32_MAX, 2U), UINT32_MAX);
}

TEST(SafeMath, SaturatingMul_U32_ByZero_IsZero) {
    EXPECT_EQ(sat_mul<u32>(UINT32_MAX, 0U), 0U);
}

// ============================================================
//  constexpr usability — sat_* must be constexpr-evaluable
// ============================================================

TEST(SafeMath, ConstexprSatAdd) {
    static_assert(sat_add<i32>(INT32_MAX, 1)  == INT32_MAX, "sat_add pos overflow");
    static_assert(sat_add<i32>(INT32_MIN, -1) == INT32_MIN, "sat_add neg overflow");
    static_assert(sat_add<i32>(2, 3)          == 5,         "sat_add normal");
    static_assert(sat_add<u32>(UINT32_MAX, 1U) == UINT32_MAX, "sat_add unsigned wrap");
    SUCCEED();
}

TEST(SafeMath, ConstexprSatSub) {
    static_assert(sat_sub<i32>(INT32_MIN, 1)  == INT32_MIN, "sat_sub underflow");
    static_assert(sat_sub<i32>(INT32_MAX, -1) == INT32_MAX, "sat_sub positive overflow");
    static_assert(sat_sub<i32>(10, 3)         == 7,         "sat_sub normal");
    static_assert(sat_sub<u32>(0U, 1U)        == 0U,        "sat_sub unsigned underflow");
    SUCCEED();
}

TEST(SafeMath, ConstexprSatMul) {
    static_assert(sat_mul<i32>(INT32_MAX, 2)  == INT32_MAX, "sat_mul pos overflow");
    static_assert(sat_mul<i32>(INT32_MAX, -2) == INT32_MIN, "sat_mul neg overflow");
    static_assert(sat_mul<i32>(6, 7)          == 42,        "sat_mul normal");
    static_assert(sat_mul<u32>(UINT32_MAX, 2U) == UINT32_MAX, "sat_mul unsigned wrap");
    SUCCEED();
}
