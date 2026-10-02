// book_inverse_vol_test.cpp -- platform v8 X (lane XCOMB): the inv-vol-v1 kernel.
//
//   scale_inverse_vol multiplies each eligible value by median / max(s, fraction x median): the
//   closed-form fixtures below are dyadic, so every multiplier but one is exact; the median is
//   the middle usable volatility (odd count) or the mean of the two middle ones (even count); a
//   name without a usable volatility takes the median (m = 1), one below the floor is raised to
//   it (m = 1 / fraction); equal volatilities and no usable volatility are the identity bit for
//   bit; ineligible names are neither read nor written; bad input is refused before anything
//   changes.
//
// Suite: BookInverseVol

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/inverse_vol.hpp"

namespace atx_test_v8x_book_inverse_vol {

using atx::f64;
using atx::u8;
using atx::usize;
namespace eb = atx::engine::book;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 kInf = std::numeric_limits<f64>::infinity();
constexpr f64 kFraction = 0.25; // atx-impl registers inv_vol_floor_fraction = .25

std::uint64_t bits(f64 x) { return std::bit_cast<std::uint64_t>(x); }

// Odd count: the usable eligible volatilities sort to 1/1024, 1/128, 1/64, 3/128, 1/32, so the
// median is 1/64 and the floor 1/256. Name 2 has none (filled, m = 1), name 4 is below the floor
// (m = 4), name 6 is ineligible (its volatility 1/2 would move the median if it were read).
TEST(BookInverseVol, OddCountClosedForm) {
  std::vector<f64> values{-0.5, -0.25, 0.125, 0.25, 0.5, -0.125, 9.0};
  const std::vector<f64> sigma{1.0 / 64, 1.0 / 32, kNaN, 1.0 / 128, 1.0 / 1024, 3.0 / 128, 0.5};
  const std::vector<u8> eligible{1, 1, 1, 1, 1, 1, 0};
  std::vector<f64> scratch;
  const auto stats = eb::scale_inverse_vol(values, sigma, eligible, kFraction, scratch);
  ASSERT_TRUE(stats) << stats.error().to_string();
  const f64 median = 1.0 / 64;
  const std::vector<f64> expected{-0.5, -0.125, 0.125, 0.5, 2.0, -0.125 * (median / (3.0 / 128)),
                                  9.0};
  for (usize i = 0U; i < values.size(); ++i) {
    EXPECT_EQ(bits(values[i]), bits(expected[i])) << i;
  }
  EXPECT_EQ(stats->scaled, 6U);
  EXPECT_EQ(stats->filled, 1U);
  EXPECT_EQ(stats->floored, 1U);
  EXPECT_EQ(bits(stats->median), bits(median));
  EXPECT_EQ(stats->max_multiplier, 4.0);
}

// Even count: 1/64, 1/32, 1/16, 1/8 give the median (1/32 + 1/16) / 2 = 3/64 (not the lower or
// upper middle one), so the multipliers are 3, 1.5, .75 and .375 exactly; nothing is floored
// (1/64 > 3/256).
TEST(BookInverseVol, EvenCountTakesTheMeanOfTheTwoMiddleOnes) {
  std::vector<f64> values{1.0, 1.0, -1.0, 1.0};
  const std::vector<f64> sigma{1.0 / 8, 1.0 / 64, 1.0 / 16, 1.0 / 32};
  const std::vector<u8> eligible{1, 1, 1, 1};
  std::vector<f64> scratch;
  const auto stats = eb::scale_inverse_vol(values, sigma, eligible, kFraction, scratch);
  ASSERT_TRUE(stats) << stats.error().to_string();
  const std::vector<f64> expected{0.375, 3.0, -0.75, 1.5};
  for (usize i = 0U; i < values.size(); ++i) {
    EXPECT_EQ(bits(values[i]), bits(expected[i])) << i;
  }
  EXPECT_EQ(bits(stats->median), bits(3.0 / 64));
  EXPECT_EQ(stats->scaled, 4U);
  EXPECT_EQ(stats->filled, 0U);
  EXPECT_EQ(stats->floored, 0U);
  EXPECT_EQ(stats->max_multiplier, 3.0);
}

// The fixture tells the rule from its near neighbours: the mean volatility instead of the
// median, the lower middle one for an even count, no floor, and a filled name at the floor.
TEST(BookInverseVol, FixturesTellWrongRulesApart) {
  const f64 median = 1.0 / 64;
  const f64 mean = (1.0 / 64 + 1.0 / 32 + 1.0 / 128 + 1.0 / 1024 + 3.0 / 128) / 5.0;
  EXPECT_NE(bits(mean), bits(median));
  EXPECT_NE(bits(1.0 / 32), bits(3.0 / 64));         // the lower middle of the even fixture
  EXPECT_NE(median / (1.0 / 1024), 4.0);             // unfloored, name 4 would take 16
  EXPECT_NE(median / (kFraction * median), 1.0);     // a filled name is not floored
}

// Equal usable volatilities are the identity bit for bit (m = median / median = 1, and a name
// without one takes the median), on random values; no usable volatility leaves the row as it is
// (median NaN, nothing scaled); in both the ineligible names are untouched.
TEST(BookInverseVol, EqualOrMissingVolatilitiesAreTheIdentity) {
  std::uint64_t s = 977U;
  const auto uni = [&s]() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(s >> 11U) * 0x1.0p-53 - 0.5;
  };
  constexpr usize n = 41;
  std::vector<f64> values(n), sigma(n, 0.0137);
  std::vector<u8> eligible(n, 1);
  for (auto &v : values) {
    v = uni();
  }
  sigma[3] = kNaN;
  sigma[7] = 0.0;
  eligible[11] = 0;
  sigma[11] = 0.9;
  values[12] = kNaN; // ineligible below: never read
  eligible[12] = 0;
  const auto before = values;
  std::vector<f64> scratch;
  const auto equal = eb::scale_inverse_vol(values, sigma, eligible, kFraction, scratch);
  ASSERT_TRUE(equal) << equal.error().to_string();
  for (usize i = 0U; i < n; ++i) {
    EXPECT_EQ(bits(values[i]), bits(before[i])) << i;
  }
  EXPECT_EQ(equal->scaled, n - 2U);
  EXPECT_EQ(equal->filled, 2U); // NaN and 0 are not usable
  EXPECT_EQ(equal->max_multiplier, 1.0);
  const std::vector<f64> none{kNaN, 0.0, -0.01, kInf, kNaN};
  std::vector<f64> row{0.5, -0.5, 0.25, -0.25, 0.0};
  const auto kept = row;
  const std::vector<u8> all(5U, 1);
  const auto bare = eb::scale_inverse_vol(row, none, all, kFraction, scratch);
  ASSERT_TRUE(bare) << bare.error().to_string();
  for (usize i = 0U; i < row.size(); ++i) {
    EXPECT_EQ(bits(row[i]), bits(kept[i])) << i;
  }
  EXPECT_TRUE(std::isnan(bare->median));
  EXPECT_EQ(bare->scaled, 0U);
  EXPECT_EQ(bare->max_multiplier, 0.0);
}

// Refused, nothing modified: widths that differ, a floor fraction outside (0, 1] (NaN included)
// and an eligible non-finite value; an ineligible non-finite value is accepted.
TEST(BookInverseVol, RefusesBadInputBeforeAnythingChanges) {
  const std::vector<f64> sigma{0.01, 0.02, 0.03};
  const std::vector<u8> eligible{1, 1, 1};
  std::vector<f64> scratch;
  const auto refused = [&](std::vector<f64> values, std::span<const f64> s,
                           std::span<const u8> e, f64 fraction) {
    const auto before = values;
    const auto r = eb::scale_inverse_vol(values, s, e, fraction, scratch);
    for (usize i = 0U; i < values.size(); ++i) {
      if (bits(values[i]) != bits(before[i])) {
        return false;
      }
    }
    return !r && r.error().code() == atx::core::ErrorCode::InvalidArgument;
  };
  const std::vector<f64> row{0.5, 0.0, -0.5};
  EXPECT_TRUE(refused(row, std::span<const f64>(sigma).first(2), eligible, kFraction));
  EXPECT_TRUE(refused(row, sigma, std::span<const u8>(eligible).first(2), kFraction));
  for (const f64 fraction : {0.0, -0.25, 1.5, kNaN, kInf}) {
    EXPECT_TRUE(refused(row, sigma, eligible, fraction)) << fraction;
  }
  EXPECT_TRUE(refused({0.5, kNaN, -0.5}, sigma, eligible, kFraction));
  EXPECT_TRUE(refused({0.5, kInf, -0.5}, sigma, eligible, kFraction));
  std::vector<f64> ok{0.5, kNaN, -0.5};
  const std::vector<u8> skip{1, 0, 1};
  EXPECT_TRUE(eb::scale_inverse_vol(ok, sigma, skip, 1.0, scratch));
  EXPECT_TRUE(std::isnan(ok[1])); // the ineligible NaN is untouched
}

} // namespace atx_test_v8x_book_inverse_vol
