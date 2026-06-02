// Tests for atx::core math — floating-point numeric helpers.
//
// Coverage strategy (agent profile §7):
//   - isclose: floating-point rounding, inf==inf, large/tiny values, negatives.
//   - clamp:   above hi, below lo, in range, negative range, exact boundaries.
//   - lerp:    t=0 → a, t=1 → b, t=0.5 midpoint.
//   - sign:    positive, negative, zero, negative zero, f32.
//   - static_assert constexpr probes for isclose/clamp/lerp/sign.

#include "atx/core/math.hpp"
#include "atx/core/types.hpp"

#include <limits>

#include <gtest/gtest.h>

using namespace atx;       // NOLINT(google-build-using-namespace)
using namespace atx::core; // NOLINT(google-build-using-namespace)

// ============================================================
//  isclose
// ============================================================

TEST(Math, IsClose_FloatingPointRounding) {
    // 0.1 + 0.2 ≠ 0.3 exactly in IEEE-754, but should be within rel_tol.
    EXPECT_TRUE(isclose(0.1 + 0.2, 0.3));
}

TEST(Math, IsClose_NotClose) {
    EXPECT_FALSE(isclose(1.0, 1.1));
}

TEST(Math, IsClose_ExactEqual) {
    EXPECT_TRUE(isclose(3.14, 3.14));
}

TEST(Math, IsClose_InfEqualToSelf) {
    // inf == inf must be true (exact-equal fast path).
    const f64 inf = std::numeric_limits<f64>::infinity();
    EXPECT_TRUE(isclose(inf, inf));
}

TEST(Math, IsClose_PosInfNotNegInf) {
    const f64 inf = std::numeric_limits<f64>::infinity();
    EXPECT_FALSE(isclose(inf, -inf));
}

TEST(Math, IsClose_InfNotFinite) {
    const f64 inf = std::numeric_limits<f64>::infinity();
    EXPECT_FALSE(isclose(inf, 1.0e300));
}

TEST(Math, IsClose_BothZero) {
    EXPECT_TRUE(isclose(0.0, 0.0));
}

TEST(Math, IsClose_NearZeroWithAbsTol) {
    // 1e-13 and 0 differ by 1e-13; abs_tol default is 1e-12 → close.
    EXPECT_TRUE(isclose(1.0e-13, 0.0));
}

TEST(Math, IsClose_LargeValues) {
    // 1e15 vs 1e15 + 1 — relatively tiny difference.
    EXPECT_TRUE(isclose(1.0e15, 1.0e15 + 1.0));
}

TEST(Math, IsClose_TinyValues) {
    // Very small numbers below abs_tol threshold.
    EXPECT_TRUE(isclose(1.0e-13, 2.0e-13));
}

TEST(Math, IsClose_Negative) {
    EXPECT_TRUE(isclose(-1.0, -1.0));
    EXPECT_FALSE(isclose(-1.0, -1.1));
}

TEST(Math, IsClose_CustomTol) {
    // With very tight tolerances, 0.1+0.2 vs 0.3 should fail.
    EXPECT_FALSE(isclose(0.1 + 0.2, 0.3, 0.0, 0.0));
}

TEST(Math, IsClose_F32) {
    EXPECT_TRUE(isclose(0.1F + 0.2F, 0.3F));
}

// static_assert proves constexpr usability.
static_assert(isclose(1.0, 1.0), "exact equal must be constexpr-true");
static_assert(!isclose(1.0, 2.0), "far apart must be constexpr-false");

// ============================================================
//  clamp
// ============================================================

TEST(Math, Clamp_AboveHi) {
    EXPECT_EQ(clamp(5.0, 0.0, 3.0), 3.0);
}

TEST(Math, Clamp_BelowLo) {
    EXPECT_EQ(clamp(-1.0, 0.0, 3.0), 0.0);
}

TEST(Math, Clamp_InRange) {
    EXPECT_EQ(clamp(1.5, 0.0, 3.0), 1.5);
}

TEST(Math, Clamp_AtLoBoundary) {
    EXPECT_EQ(clamp(0.0, 0.0, 3.0), 0.0);
}

TEST(Math, Clamp_AtHiBoundary) {
    EXPECT_EQ(clamp(3.0, 0.0, 3.0), 3.0);
}

TEST(Math, Clamp_NegativeRange) {
    EXPECT_EQ(clamp(-5.0, -3.0, -1.0), -3.0);
    EXPECT_EQ(clamp(-2.0, -3.0, -1.0), -2.0);
    EXPECT_EQ(clamp(0.0, -3.0, -1.0), -1.0);
}

TEST(Math, Clamp_F32) {
    EXPECT_EQ(clamp(10.0F, 0.0F, 5.0F), 5.0F);
}

static_assert(clamp(5.0, 0.0, 3.0) == 3.0, "clamp above hi constexpr");
static_assert(clamp(-1.0, 0.0, 3.0) == 0.0, "clamp below lo constexpr");
static_assert(clamp(2.0, 0.0, 3.0) == 2.0, "clamp in range constexpr");

// ============================================================
//  lerp
// ============================================================

TEST(Math, Lerp_TZero) {
    // t=0 must return a exactly.
    EXPECT_DOUBLE_EQ(lerp(0.0, 10.0, 0.0), 0.0);
}

TEST(Math, Lerp_TOne) {
    // t=1 must return b exactly.
    EXPECT_DOUBLE_EQ(lerp(0.0, 10.0, 1.0), 10.0);
}

TEST(Math, Lerp_Midpoint) {
    EXPECT_DOUBLE_EQ(lerp(0.0, 10.0, 0.5), 5.0);
}

TEST(Math, Lerp_NegativeRange) {
    EXPECT_DOUBLE_EQ(lerp(-10.0, 10.0, 0.5), 0.0);
}

TEST(Math, Lerp_Extrapolate) {
    // t outside [0,1] is mathematically valid — just extrapolation.
    EXPECT_DOUBLE_EQ(lerp(0.0, 10.0, 2.0), 20.0);
}

TEST(Math, Lerp_F32) {
    EXPECT_FLOAT_EQ(lerp(0.0F, 10.0F, 0.5F), 5.0F);
}

// ============================================================
//  sign
// ============================================================

TEST(Math, Sign_Positive) {
    EXPECT_EQ(sign(2.0), 1);
    EXPECT_EQ(sign(1.0e300), 1);
}

TEST(Math, Sign_Negative) {
    EXPECT_EQ(sign(-2.0), -1);
    EXPECT_EQ(sign(-1.0e-300), -1);
}

TEST(Math, Sign_Zero) {
    EXPECT_EQ(sign(0.0), 0);
}

TEST(Math, Sign_NegativeZero) {
    // IEEE-754: -0.0 compares equal to 0.0; sign must return 0.
    EXPECT_EQ(sign(-0.0), 0);
}

TEST(Math, Sign_F32) {
    EXPECT_EQ(sign(-3.0F), -1);
    EXPECT_EQ(sign(0.0F), 0);
    EXPECT_EQ(sign(3.0F), 1);
}

static_assert(sign(5.0) == 1, "sign positive constexpr");
static_assert(sign(-5.0) == -1, "sign negative constexpr");
static_assert(sign(0.0) == 0, "sign zero constexpr");
