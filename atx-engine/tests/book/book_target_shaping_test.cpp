// book_target_shaping_test.cpp — platform v8 R-4/R-5: the desired-target shaping kernels.
//
//   apply_hold_band (R-4, hold-band-v1 rank hysteresis): first use sets every eligible name;
//   inside the band (|rank_now - rank_set| <= band, the boundary included) the previous value
//   is kept, outside it the fresh one is taken and the rank reset; an ineligible name's state
//   survives untouched; band 0 over rows with desired == rank_now is the identity bit for bit;
//   bad input is refused before anything changes.
//
// Suite: BookTargetShaping

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/book/target_shaping.hpp"

namespace atx_test_v8_book_target_shaping {

using atx::f64;
using atx::u8;
using atx::usize;
namespace eb = atx::engine::book;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

std::uint64_t bits(f64 x) { return std::bit_cast<std::uint64_t>(x); }

struct Rng {
  std::uint64_t s;
  f64 uni() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(s >> 11U) * 0x1.0p-53;
  }
};

// Centred ranks of a random permutation of n names (distinct, in [-.5, .5]).
std::vector<f64> random_ranks(Rng &g, usize n) {
  std::vector<f64> key(n);
  for (auto &k : key) {
    k = g.uni();
  }
  std::vector<f64> rank(n);
  for (usize i = 0U; i < n; ++i) {
    usize below = 0U;
    for (usize j = 0U; j < n; ++j) {
      below += key[j] < key[i] ? 1U : 0U;
    }
    rank[i] = static_cast<f64>(below) / static_cast<f64>(n - 1U) - 0.5;
  }
  return rank;
}

TEST(BookTargetShaping, HoldBandFirstUseSetsEveryEligibleName) {
  const std::vector<f64> rank{-0.5, -0.25, 0.0, 0.25, 0.5};
  std::vector<f64> desired = rank;
  const std::vector<u8> eligible{1, 1, 0, 1, 1};
  eb::HoldBandState state;
  const auto counts = eb::apply_hold_band(rank, desired, eligible, 0.1, state);
  ASSERT_TRUE(counts) << counts.error().to_string();
  EXPECT_EQ(counts->moved, 4U);
  EXPECT_EQ(counts->first_set, 4U);
  EXPECT_EQ(counts->kept, 0U);
  ASSERT_EQ(state.rank_set.size(), rank.size());
  ASSERT_EQ(state.desired_prev.size(), rank.size());
  for (usize i = 0U; i < rank.size(); ++i) {
    EXPECT_EQ(bits(desired[i]), bits(rank[i])) << i;
    if (eligible[i] != 0U) {
      EXPECT_EQ(bits(state.rank_set[i]), bits(rank[i])) << i;
      EXPECT_EQ(bits(state.desired_prev[i]), bits(rank[i])) << i;
    } else {
      EXPECT_TRUE(std::isnan(state.rank_set[i])) << i; // never eligible: still unset
      EXPECT_TRUE(std::isnan(state.desired_prev[i])) << i;
    }
  }
}

// Band .125 (exact in binary): a move of exactly .125 stays inside (the rule is strictly
// greater), a move of .1875 leaves; a kept name gets desired_prev, not its fresh value, and its
// rank_set does not follow the drift; a moved name takes the fresh value and resets its rank.
TEST(BookTargetShaping, HoldBandKeepsInsideAndMovesOutside) {
  eb::HoldBandState state{{0.25, 0.25, -0.25, 0.0}, {0.3, 0.3, -0.2, 0.05}};
  const std::vector<f64> rank{0.375, 0.4375, -0.3125, 0.0};
  std::vector<f64> desired{0.7, 0.8, -0.6, 0.01}; // fresh values distinct from the ranks
  const std::vector<u8> eligible(4U, 1U);
  const auto counts = eb::apply_hold_band(rank, desired, eligible, 0.125, state);
  ASSERT_TRUE(counts) << counts.error().to_string();
  EXPECT_EQ(counts->kept, 3U);
  EXPECT_EQ(counts->moved, 1U);
  EXPECT_EQ(counts->first_set, 0U);
  EXPECT_EQ(desired[0], 0.3);   // |.375 - .25| = .125: the boundary is inside
  EXPECT_EQ(desired[1], 0.8);   // |.4375 - .25| = .1875 > .125: moved
  EXPECT_EQ(desired[2], -0.2);  // |-.3125 + .25| = .0625: kept
  EXPECT_EQ(desired[3], 0.05);  // unchanged rank: kept
  EXPECT_EQ(state.rank_set[0], 0.25);
  EXPECT_EQ(state.rank_set[1], 0.4375);
  EXPECT_EQ(state.desired_prev[1], 0.8);
  EXPECT_EQ(state.rank_set[2], -0.25);
  EXPECT_EQ(state.desired_prev[2], -0.2);
}

// A name ineligible for a day (a nonmember) keeps its state: on return it is compared with the
// rank of its last set value, not re-set.
TEST(BookTargetShaping, HoldBandStateSurvivesAnIneligibleDay) {
  eb::HoldBandState state;
  std::vector<f64> day0{-0.5, 0.0, 0.5};
  ASSERT_TRUE(eb::apply_hold_band(day0, day0, std::vector<u8>{1, 1, 1}, 0.1, state));
  // Day 1: name 1 missing; its row value (0 for a nonmember) must not touch its state.
  std::vector<f64> day1{-0.5, 0.0, 0.5};
  const auto missing = eb::apply_hold_band(day1, day1, std::vector<u8>{1, 0, 1}, 0.1, state);
  ASSERT_TRUE(missing);
  EXPECT_EQ(missing->moved + missing->kept, 2U);
  EXPECT_EQ(state.rank_set[1], 0.0);
  EXPECT_EQ(state.desired_prev[1], 0.0);
  // Day 2: name 1 returns at .0625 (inside the band around its old 0): kept at 0.
  std::vector<f64> day2{-0.5, 0.0625, 0.5};
  const auto back = eb::apply_hold_band(day2, day2, std::vector<u8>{1, 1, 1}, 0.1, state);
  ASSERT_TRUE(back);
  EXPECT_EQ(back->first_set, 0U);
  EXPECT_EQ(day2[1], 0.0);
  EXPECT_EQ(state.rank_set[1], 0.0);
}

// Band 0 over rows whose fresh value is the rank (rank_now and desired one buffer, as the
// strategy wiring passes them) returns every fresh row bit for bit, across decisions with
// repeated and changed ranks and a name dropping out and back.
TEST(BookTargetShaping, HoldBandZeroBandIsTheIdentity) {
  Rng g{7U};
  constexpr usize n = 40U;
  eb::HoldBandState state;
  std::vector<f64> previous;
  for (usize day = 0U; day < 30U; ++day) {
    const auto fresh = day % 3U == 1U ? previous : random_ranks(g, n); // repeats exercise kept
    std::vector<f64> row = fresh;
    std::vector<u8> eligible(n, 1U);
    eligible[day % n] = 0U;
    const auto counts = eb::apply_hold_band(row, row, eligible, 0.0, state);
    ASSERT_TRUE(counts) << counts.error().to_string();
    EXPECT_EQ(counts->moved + counts->kept, n - 1U);
    for (usize i = 0U; i < n; ++i) {
      EXPECT_EQ(bits(row[i]), bits(fresh[i])) << day << ' ' << i;
    }
    if (day % 3U == 1U && day > 1U) {
      EXPECT_GT(counts->kept, 0U) << day; // repeated ranks are kept, still bit-identical
    }
    previous = fresh;
  }
}

TEST(BookTargetShaping, HoldBandRefusesBadInputWithoutChangingAnything) {
  const std::vector<f64> rank{-0.5, 0.0, 0.5};
  const std::vector<u8> eligible{1, 1, 1};
  const eb::HoldBandState before{{-0.5, kNaN, 0.5}, {-0.4, kNaN, 0.4}};
  const auto refused = [&](std::vector<f64> row, std::vector<u8> mask, f64 band,
                           eb::HoldBandState state) {
    const std::vector<f64> row_before = row;
    const auto result = eb::apply_hold_band(rank, row, mask, band, state);
    bool unchanged = row.size() == row_before.size();
    for (usize i = 0U; unchanged && i < row.size(); ++i) {
      unchanged = bits(row[i]) == bits(row_before[i]);
    }
    return !result && unchanged;
  };
  EXPECT_TRUE(refused({0.1, 0.2}, eligible, 0.1, before));                    // width
  EXPECT_TRUE(refused({0.1, 0.2, 0.3}, {1, 1}, 0.1, before));                 // mask width
  EXPECT_TRUE(refused({0.1, 0.2, 0.3}, eligible, -0.1, before));              // band < 0
  EXPECT_TRUE(refused({0.1, 0.2, 0.3}, eligible, kNaN, before));              // band NaN
  EXPECT_TRUE(refused({0.1, kNaN, 0.3}, eligible, 0.1, before));              // eligible NaN
  EXPECT_TRUE(refused({0.1, 0.2, 0.3}, eligible, 0.1, {{0.0}, {0.0}}));       // state width
  EXPECT_TRUE(refused({0.1, 0.2, 0.3}, eligible, 0.1, {{0.0, 0.0, 0.0}, {0.0, kNaN, 0.0}}));
  // Controls: an ineligible NaN value and an unset rank with any desired_prev are admitted.
  eb::HoldBandState ok = before;
  std::vector<f64> row{0.1, kNaN, 0.3};
  EXPECT_TRUE(eb::apply_hold_band(rank, row, std::vector<u8>{1, 0, 1}, 0.1, ok));
}

} // namespace atx_test_v8_book_target_shaping
