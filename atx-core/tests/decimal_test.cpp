// Tests for atx::core Decimal — exact fixed-point money type (scale 1e-9).
//
// Coverage strategy (agent profile §7):
//   - Construction: from_int / from_raw / from_double / from_string, both signs.
//   - Round-trip: from_string -> to_string canonical form (trim trailing zeros,
//     keep at least one fractional digit).
//   - Arithmetic exactness: add/sub on mantissas.
//   - Multiply: rescaling, truncation toward zero for BOTH signs.
//   - Divide: rescaling, truncation toward zero, divide-by-zero precondition.
//   - round(): round-half-away-from-zero.
//   - Boundaries: kMaxWhole, INT64 edges, overflow preconditions (EXPECT_DEATH).
//   - Parsing: reject garbage, empty, double dot, out-of-range, too many digits.
//   - Ordering: operator<=> / operator==.
//
// Canonical to_string form (documented in decimal.hpp):
//   optional '-', integer part (at least one digit), '.', then the significant
//   fractional digits with trailing zeros trimmed but at least one digit kept.
//   Examples: 3 -> "3.0", 1.5 -> "1.5", -123.456789 -> "-123.456789", 0 -> "0.0".

#include "atx/core/decimal.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include <gtest/gtest.h>

using namespace atx;       // NOLINT(google-build-using-namespace) — i64 etc.
using namespace atx::core; // NOLINT(google-build-using-namespace) — Decimal.

// ============================================================
//  Construction & accessors
// ============================================================

TEST(Decimal, FromIntAndBack) {
  const auto d = Decimal::from_int(5);
  EXPECT_EQ(d.raw(), 5 * Decimal::kScale);
  EXPECT_DOUBLE_EQ(d.to_double(), 5.0);
}

TEST(Decimal, FromIntNegative) {
  const auto d = Decimal::from_int(-7);
  EXPECT_EQ(d.raw(), -7 * Decimal::kScale);
  EXPECT_DOUBLE_EQ(d.to_double(), -7.0);
}

TEST(Decimal, FromIntZero) {
  const auto d = Decimal::from_int(0);
  EXPECT_EQ(d.raw(), 0);
  EXPECT_EQ(d.to_string(), "0.0");
}

TEST(Decimal, FromRawIsExact) {
  const auto d = Decimal::from_raw(123456789LL); // 0.123456789
  EXPECT_EQ(d.raw(), 123456789LL);
  EXPECT_EQ(d.to_string(), "0.123456789");
}

TEST(Decimal, FromRawNegativeSubInteger) {
  const auto d = Decimal::from_raw(-1LL); // -1e-9
  EXPECT_EQ(d.raw(), -1LL);
  EXPECT_EQ(d.to_string(), "-0.000000001");
}

// ============================================================
//  from_int range checking (precondition aborts in debug)
// ============================================================

TEST(Decimal, FromIntAtMaxWholeOk) {
  const i64 max_whole = Decimal::kMaxWhole;
  const auto d = Decimal::from_int(max_whole);
  EXPECT_EQ(d.raw(), max_whole * Decimal::kScale);
}

TEST(Decimal, FromIntAtMinWholeOk) {
  const i64 min_whole = -Decimal::kMaxWhole;
  const auto d = Decimal::from_int(min_whole);
  EXPECT_EQ(d.raw(), min_whole * Decimal::kScale);
}

TEST(Decimal, FromIntRangeChecked) {
  EXPECT_DEATH((void)Decimal::from_int(10'000'000'000LL), "");
}

TEST(Decimal, FromIntRangeCheckedNegative) {
  EXPECT_DEATH((void)Decimal::from_int(-10'000'000'000LL), "");
}

// ============================================================
//  from_double
// ============================================================

TEST(Decimal, FromDoubleBasic) {
  const auto r = Decimal::from_double(1.5);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), 1'500'000'000LL);
}

TEST(Decimal, FromDoubleNegative) {
  const auto r = Decimal::from_double(-2.25);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), -2'250'000'000LL);
}

TEST(Decimal, FromDoubleRoundsHalfAwayFromZero) {
  // 0.0000000005 -> rounds away from zero to 0.000000001 (1 raw unit).
  const auto r = Decimal::from_double(0.5e-9);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), 1LL);
  const auto rn = Decimal::from_double(-0.5e-9);
  ASSERT_TRUE(rn.has_value());
  EXPECT_EQ(rn->raw(), -1LL);
}

TEST(Decimal, FromDoubleRejectsNaN) {
  const auto r = Decimal::from_double(std::nan(""));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(Decimal, FromDoubleRejectsInf) {
  const auto r = Decimal::from_double(std::numeric_limits<f64>::infinity());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

TEST(Decimal, FromDoubleRejectsOutOfRange) {
  const auto r = Decimal::from_double(1e30);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

// ============================================================
//  from_string / to_string round-trip
// ============================================================

TEST(Decimal, FromStringInteger) {
  const auto r = Decimal::from_string("42");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), 42 * Decimal::kScale);
  EXPECT_EQ(r->to_string(), "42.0");
}

TEST(Decimal, FromStringWithFraction) {
  const auto r = Decimal::from_string("3.0");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), 3 * Decimal::kScale);
  EXPECT_EQ(r->to_string(), "3.0");
}

TEST(Decimal, FromStringFullPrecisionRoundTrip) {
  const auto r = Decimal::from_string("-123.456789");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->to_string(), "-123.456789");
}

TEST(Decimal, FromStringNineFractionalDigits) {
  const auto r = Decimal::from_string("0.123456789");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), 123'456'789LL);
  EXPECT_EQ(r->to_string(), "0.123456789");
}

TEST(Decimal, FromStringTrimsTrailingZeros) {
  const auto r = Decimal::from_string("1.500");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), 1'500'000'000LL);
  EXPECT_EQ(r->to_string(), "1.5");
}

TEST(Decimal, FromStringShortFraction) {
  // "0.1" -> 0.1 exactly -> raw 100000000.
  const auto r = Decimal::from_string("0.1");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), 100'000'000LL);
  EXPECT_EQ(r->to_string(), "0.1");
}

TEST(Decimal, FromStringLeadingPlus) {
  const auto r = Decimal::from_string("+5.25");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), 5'250'000'000LL);
}

TEST(Decimal, FromStringNegativeZeroFraction) {
  const auto r = Decimal::from_string("-0.5");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), -500'000'000LL);
  EXPECT_EQ(r->to_string(), "-0.5");
}

TEST(Decimal, FromStringTrailingDotIsInteger) {
  // "7." parses as 7 (no fractional digits after the dot is allowed -> .0).
  const auto r = Decimal::from_string("7.");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), 7 * Decimal::kScale);
}

// ----- parse rejections -----

TEST(Decimal, FromStringRejectsGarbage) {
  EXPECT_FALSE(Decimal::from_string("1.2.3").has_value());
}

TEST(Decimal, FromStringRejectsEmpty) {
  EXPECT_FALSE(Decimal::from_string("").has_value());
}

TEST(Decimal, FromStringRejectsAlpha) {
  EXPECT_FALSE(Decimal::from_string("abc").has_value());
}

TEST(Decimal, FromStringRejectsLoneDot) {
  EXPECT_FALSE(Decimal::from_string(".").has_value());
}

TEST(Decimal, FromStringRejectsLoneSign) {
  EXPECT_FALSE(Decimal::from_string("-").has_value());
}

TEST(Decimal, FromStringRejectsInternalSpace) {
  EXPECT_FALSE(Decimal::from_string("1 2").has_value());
}

TEST(Decimal, FromStringRejectsTooManyFractionalDigits) {
  // 10 fractional digits exceeds the 9-dp scale.
  const auto r = Decimal::from_string("0.1234567890");
  EXPECT_FALSE(r.has_value());
}

TEST(Decimal, FromStringRejectsIntegerOverflow) {
  // Way beyond kMaxWhole.
  const auto r = Decimal::from_string("100000000000000000000");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(Decimal, FromStringRejectsLeadingDotFraction) {
  // Require at least one integer digit before the dot.
  EXPECT_FALSE(Decimal::from_string(".5").has_value());
}

// ----- mantissa-overflow boundary: integer part within kMaxWhole but the
//       combined mantissa (whole*kScale + fraction) crosses INT64 limits -----

TEST(Decimal, FromStringAcceptsMaxMantissa) {
  // 9223372036.854775807 == INT64_MAX exactly (largest positive mantissa).
  const auto r = Decimal::from_string("9223372036.854775807");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), INT64_MAX);
}

TEST(Decimal, FromStringRejectsOneAboveMaxMantissa) {
  // INT64_MAX+1: integer part 9223372036 <= kMaxWhole, but the mantissa would
  // overflow i64 (signed-overflow UB if combined naively) -> OutOfRange.
  const auto r = Decimal::from_string("9223372036.854775808");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(Decimal, FromStringAcceptsMinMantissa) {
  // -9223372036.854775808 == INT64_MIN exactly (representable, magnitude is
  // |INT64_MIN| == INT64_MAX+1).
  const auto r = Decimal::from_string("-9223372036.854775808");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->raw(), INT64_MIN);
}

TEST(Decimal, FromStringRejectsOneBelowMinMantissa) {
  // One ulp below INT64_MIN -> not representable -> OutOfRange.
  const auto r = Decimal::from_string("-9223372036.854775809");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

// ============================================================
//  Addition / subtraction (exact)
// ============================================================

TEST(Decimal, AddExact) {
  EXPECT_EQ(Decimal::from_int(2) + Decimal::from_int(3), Decimal::from_int(5));
}

TEST(Decimal, AddFractionExact) {
  const auto a = *Decimal::from_string("0.1");
  const auto b = *Decimal::from_string("0.2");
  const auto c = *Decimal::from_string("0.3");
  EXPECT_EQ(a + b, c); // exact, unlike binary floating point
}

TEST(Decimal, SubExact) {
  EXPECT_EQ(Decimal::from_int(10) - Decimal::from_int(3), Decimal::from_int(7));
}

TEST(Decimal, SubNegativeResult) {
  EXPECT_EQ(Decimal::from_int(3) - Decimal::from_int(10), Decimal::from_int(-7));
}

TEST(Decimal, CheckedAddOk) {
  const auto r = Decimal::from_int(2).checked_add(Decimal::from_int(3));
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, Decimal::from_int(5));
}

TEST(Decimal, CheckedAddOverflowErrs) {
  const auto big = Decimal::from_raw(INT64_MAX);
  const auto r = big.checked_add(Decimal::from_raw(1));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(Decimal, CheckedSubOverflowErrs) {
  const auto small = Decimal::from_raw(INT64_MIN);
  const auto r = small.checked_sub(Decimal::from_raw(1));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::OutOfRange);
}

TEST(Decimal, AddOverflowAborts) {
  const auto big = Decimal::from_raw(INT64_MAX);
  EXPECT_DEATH((void)(big + Decimal::from_raw(1)), "");
}

// ============================================================
//  Multiplication (rescale, truncate toward zero)
// ============================================================

TEST(Decimal, MulRescales) {
  const auto a = *Decimal::from_string("1.5");
  const auto b = *Decimal::from_string("2.0");
  EXPECT_EQ((a * b), *Decimal::from_string("3.0"));
}

TEST(Decimal, MulNegativeTimesPositive) {
  const auto a = *Decimal::from_string("-1.5");
  const auto b = *Decimal::from_string("2.0");
  EXPECT_EQ((a * b), *Decimal::from_string("-3.0"));
}

TEST(Decimal, MulNegativeTimesNegative) {
  const auto a = *Decimal::from_string("-1.5");
  const auto b = *Decimal::from_string("-2.0");
  EXPECT_EQ((a * b), *Decimal::from_string("3.0"));
}

TEST(Decimal, MulTruncatesTowardZero) {
  const auto a = Decimal::from_raw(3); // 3e-9
  const auto b = Decimal::from_raw(2); // 2e-9 -> product 6e-18 -> 0 at scale 1e-9
  EXPECT_EQ((a * b).raw(), 0);
}

TEST(Decimal, MulTruncatesTowardZeroNegative) {
  // (-3e-9) * (2e-9) = -6e-18 -> truncates toward zero -> 0 (not -1).
  const auto a = Decimal::from_raw(-3);
  const auto b = Decimal::from_raw(2);
  EXPECT_EQ((a * b).raw(), 0);
}

TEST(Decimal, MulTruncationDirection) {
  // 1.000000001 * 1.000000001 ; product mantissa = 1000000001^2 = 1.000000002...e18
  // / 1e9 truncates toward zero.
  const auto a = Decimal::from_raw(1'000'000'001LL);
  const auto b = Decimal::from_raw(1'000'000'001LL);
  // 1000000001^2 = 1000000002000000001 ; /1e9 = 1000000002 (truncated).
  EXPECT_EQ((a * b).raw(), 1'000'000'002LL);
  // Negative: -(that) magnitude truncates toward zero identically in magnitude.
  const auto c = Decimal::from_raw(-1'000'000'001LL);
  EXPECT_EQ((c * b).raw(), -1'000'000'002LL);
}

TEST(Decimal, MulByZero) {
  const auto a = *Decimal::from_string("123.456");
  EXPECT_EQ((a * Decimal::from_int(0)).raw(), 0);
}

TEST(Decimal, MulLargeNoOverflow) {
  // 1000 * 1000 = 1e6 (value), well within the representable range. The
  // intermediate product of mantissas is 1e12 * 1e12 = 1e24, far beyond i64 —
  // this exercises the 128-bit product path without result overflow.
  const auto a = Decimal::from_int(1'000LL);
  const auto b = Decimal::from_int(1'000LL);
  EXPECT_EQ((a * b), Decimal::from_int(1'000'000LL));
}

// ============================================================
//  Division (rescale, truncate toward zero)
// ============================================================

TEST(Decimal, DivExact) {
  const auto a = Decimal::from_int(6);
  const auto b = Decimal::from_int(2);
  EXPECT_EQ((a / b), Decimal::from_int(3));
}

TEST(Decimal, DivFraction) {
  const auto a = Decimal::from_int(1);
  const auto b = Decimal::from_int(2);
  EXPECT_EQ((a / b), *Decimal::from_string("0.5"));
}

TEST(Decimal, DivTruncatesTowardZero) {
  // 1 / 3 = 0.333333333... -> truncates to 0.333333333 (raw 333333333).
  const auto a = Decimal::from_int(1);
  const auto b = Decimal::from_int(3);
  EXPECT_EQ((a / b).raw(), 333'333'333LL);
}

TEST(Decimal, DivTruncatesTowardZeroNegative) {
  // -1 / 3 -> -0.333333333 (truncate toward zero, magnitude floors).
  const auto a = Decimal::from_int(-1);
  const auto b = Decimal::from_int(3);
  EXPECT_EQ((a / b).raw(), -333'333'333LL);
}

TEST(Decimal, DivNegativeByNegative) {
  const auto a = Decimal::from_int(-6);
  const auto b = Decimal::from_int(-2);
  EXPECT_EQ((a / b), Decimal::from_int(3));
}

TEST(Decimal, DivByZeroAborts) {
  const auto a = Decimal::from_int(1);
  EXPECT_DEATH((void)(a / Decimal::from_int(0)), "");
}

// ============================================================
//  round() — round-half-away-from-zero to integer
// ============================================================

TEST(Decimal, RoundHalfAwayFromZeroPositive) {
  EXPECT_EQ((*Decimal::from_string("2.5")).round(), Decimal::from_int(3));
  EXPECT_EQ((*Decimal::from_string("2.4")).round(), Decimal::from_int(2));
}

TEST(Decimal, RoundHalfAwayFromZeroNegative) {
  EXPECT_EQ((*Decimal::from_string("-2.5")).round(), Decimal::from_int(-3));
  EXPECT_EQ((*Decimal::from_string("-2.4")).round(), Decimal::from_int(-2));
}

TEST(Decimal, RoundExactInteger) {
  EXPECT_EQ(Decimal::from_int(5).round(), Decimal::from_int(5));
}

// ============================================================
//  Comparison / ordering (defaulted <=> and ==)
// ============================================================

TEST(Decimal, EqualityAndInequality) {
  EXPECT_EQ(Decimal::from_int(3), Decimal::from_int(3));
  EXPECT_NE(Decimal::from_int(3), Decimal::from_int(4));
}

TEST(Decimal, Ordering) {
  const auto a = *Decimal::from_string("1.5");
  const auto b = *Decimal::from_string("2.5");
  EXPECT_LT(a, b);
  EXPECT_GT(b, a);
  EXPECT_LE(a, a);
  EXPECT_GE(b, b);
}

TEST(Decimal, OrderingNegatives) {
  const auto a = *Decimal::from_string("-2.5");
  const auto b = *Decimal::from_string("-1.5");
  EXPECT_LT(a, b);
}

// ============================================================
//  Constexpr usability of from_raw (compile-time exactness)
// ============================================================

TEST(Decimal, FromRawIsConstexpr) {
  constexpr auto d = Decimal::from_raw(1'500'000'000LL);
  static_assert(d.raw() == 1'500'000'000LL, "from_raw must be constexpr");
  EXPECT_EQ(d.raw(), 1'500'000'000LL);
}
