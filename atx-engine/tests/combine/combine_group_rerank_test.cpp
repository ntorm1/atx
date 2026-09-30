// combine_group_rerank_test.cpp -- group rank composite and its centred tied re-rank
// (platform v8 R-1, composition ew-theme-std-v1 in atx-impl).
//
// Suite: GroupRerank

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/group_rerank.hpp"

namespace atx_test_v8_group_rerank {

using atx::f64;
using atx::usize;
namespace cb = atx::engine::combine;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

std::map<usize, f64> ranks_of(std::vector<cb::RankedName> row) {
  std::map<usize, f64> out;
  cb::for_each_centered_rank(row, [&](usize i, f64 r) { out[i] = r; });
  return out;
}

f64 sd(const std::vector<f64>& v) {
  f64 mean = 0;
  for (const f64 x : v) mean += x;
  mean /= static_cast<f64>(v.size());
  f64 ss = 0;
  for (const f64 x : v) ss += (x - mean) * (x - mean);
  return std::sqrt(ss / static_cast<f64>(v.size()));
}

// Tie blocks share the mean of their positions; the ranks span [-0.5, 0.5] with mean 0,
// and the index breaks nothing but the (value, index) sort order.
TEST(GroupRerank, CenteredTiedRanksFollowTheBlockFormula) {
  const auto r = ranks_of({{3.0, 0}, {1.0, 1}, {3.0, 2}, {2.0, 3}, {-0.0, 4}});
  ASSERT_EQ(r.size(), 5U);
  EXPECT_DOUBLE_EQ(r.at(4), -0.5);
  EXPECT_DOUBLE_EQ(r.at(1), -0.25);
  EXPECT_DOUBLE_EQ(r.at(3), 0.0);
  EXPECT_DOUBLE_EQ(r.at(0), 0.375); // positions 3 and 4 of 0..4: (3 + 4) / 8 - 0.5
  EXPECT_DOUBLE_EQ(r.at(2), 0.375);
  EXPECT_TRUE(ranks_of({{1.0, 0}}).empty()); // one value ranks nothing
  EXPECT_TRUE(ranks_of({}).empty());
  // +0 and -0 tie.
  const auto zeros = ranks_of({{0.0, 0}, {-0.0, 1}, {1.0, 2}});
  EXPECT_DOUBLE_EQ(zeros.at(0), zeros.at(1));
}

TEST(GroupRerank, AccumulateReplacesTheSentinelThenSums) {
  f64 cell = kNaN;
  cb::accumulate_group_cell(cell, -0.25);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(cell), std::bit_cast<std::uint64_t>(-0.25));
  cb::accumulate_group_cell(cell, 0.125);
  EXPECT_DOUBLE_EQ(cell, -0.125);
  f64 zero = kNaN;
  cb::accumulate_group_cell(zero, 0.0); // a present member at the median rank is present
  EXPECT_FALSE(std::isnan(zero));
}

// Only present (non-NaN) names are ranked; absent names and a row with one present name
// leave `out` untouched; the weight scales the rank.
TEST(GroupRerank, RerankAddsWeightedRanksOverPresentNamesOnly) {
  constexpr usize names = 4;
  const std::vector<f64> plane{0.9, kNaN, -0.3, 0.1,  // date 0: names 0, 2, 3 present
                               kNaN, 5.0, kNaN, kNaN, // date 1: one present name
                               kNaN, kNaN, kNaN, kNaN};
  std::vector<f64> out{1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3};
  std::vector<cb::RankedName> scratch;
  scratch.reserve(names);
  ASSERT_TRUE(cb::add_group_rerank(plane, names, 0, 3, 0.5, out, scratch));
  EXPECT_DOUBLE_EQ(out[0], 1 + 0.5 * 0.5);
  EXPECT_DOUBLE_EQ(out[1], 1);
  EXPECT_DOUBLE_EQ(out[2], 1 - 0.5 * 0.5);
  EXPECT_DOUBLE_EQ(out[3], 1);
  for (usize k = 4; k < 8; ++k) EXPECT_DOUBLE_EQ(out[k], 2);
  for (usize k = 8; k < 12; ++k) EXPECT_DOUBLE_EQ(out[k], 3);
  // A date band touches only its own rows.
  std::vector<f64> band(12, 0);
  ASSERT_TRUE(cb::add_group_rerank(plane, names, 1, 3, 1.0, band, scratch));
  for (const f64 v : band) EXPECT_DOUBLE_EQ(v, 0);
}

// The point of the re-rank: a one-member group and a three-member group whose members
// disagree (so their mean rank is compressed toward 0) enter with the same dispersion.
TEST(GroupRerank, EveryGroupEntersWithTheSameDispersion) {
  constexpr usize names = 9;
  // Members i, (i + 8) % 9 and (8 i) % 9: permutations whose mean rank has nine distinct
  // values but a third of a single member's dispersion.
  constexpr std::array<usize, 3> slope{1, 1, 8}, shift{0, 8, 0};
  std::vector<f64> single(names, kNaN), three(names, kNaN);
  std::vector<cb::RankedName> row;
  for (usize member = 0; member < 3; ++member) {
    row.clear();
    for (usize i = 0; i < names; ++i)
      row.emplace_back(static_cast<f64>((i * slope[member] + shift[member]) % names), i);
    cb::for_each_centered_rank(row, [&](usize i, f64 r) { cb::accumulate_group_cell(three[i], r / 3.0); });
  }
  row.clear();
  for (usize i = 0; i < names; ++i) row.emplace_back(static_cast<f64>(i), i);
  cb::for_each_centered_rank(row, [&](usize i, f64 r) { cb::accumulate_group_cell(single[i], r); });
  EXPECT_NEAR(sd(three), sd(single) / 3.0, 1e-12); // the unranked composite is compressed
  std::vector<f64> a(names, 0), b(names, 0);
  std::vector<cb::RankedName> scratch;
  ASSERT_TRUE(cb::add_group_rerank(single, names, 0, 1, 1.0, a, scratch));
  ASSERT_TRUE(cb::add_group_rerank(three, names, 0, 1, 1.0, b, scratch));
  EXPECT_NEAR(sd(a), sd(b), 1e-15);
  EXPECT_NEAR(sd(a), sd(single), 1e-15);
}

TEST(GroupRerank, RefusesBadShapesRangesAndWeights) {
  const std::vector<f64> plane(8, 1.0);
  std::vector<f64> out(8, 0), short_out(7, 0);
  std::vector<cb::RankedName> scratch;
  EXPECT_FALSE(cb::add_group_rerank(plane, 0, 0, 1, 1.0, out, scratch));
  EXPECT_FALSE(cb::add_group_rerank(plane, 4, 0, 1, 1.0, short_out, scratch));
  EXPECT_FALSE(cb::add_group_rerank(plane, 3, 0, 1, 1.0, out, scratch)); // 8 cells are not whole rows of 3
  EXPECT_FALSE(cb::add_group_rerank(plane, 4, 0, 3, 1.0, out, scratch));
  EXPECT_FALSE(cb::add_group_rerank(plane, 4, 2, 1, 1.0, out, scratch));
  EXPECT_FALSE(cb::add_group_rerank(plane, 4, 0, 2, kNaN, out, scratch));
  EXPECT_TRUE(cb::add_group_rerank(plane, 4, 2, 2, 1.0, out, scratch)); // empty band
}

} // namespace atx_test_v8_group_rerank
