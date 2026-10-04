// book_normal_score_test.cpp — platform v8 Y (lane YCOMB): van der Waerden normal scores of a
// ranked row (atx::engine::book, normal_score.hpp), the kernel of norm-score-v1.
//
//   Closed form on four distinct names (u = .2, .4, .6, .8: the published Phi^{-1} values), a tie
//   block sharing one score at its mean rank, mirrored blocks of opposite sign, names outside the
//   row untouched, a row of one written nothing; wrong rules told apart (the uniform rank, Blom's
//   (r - 3/8) / (n + 1/4), the rank over n); refusals before any write.
//
// Suite: BookNormalScore

#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/normal_score.hpp"
#include "atx/engine/eval/stats_ext.hpp"

namespace atx_test_v8_book_normal_score {

using atx::f64;
using atx::usize;
namespace eb = atx::engine::book;
using Row = std::vector<std::pair<f64, usize>>;

constexpr f64 kSentinel = -99.0;

TEST(BookNormalScore, FourDistinctNamesAreThePublishedQuantiles) {
  const Row sorted{{-1.0, 2U}, {0.5, 0U}, {0.7, 3U}, {2.0, 1U}}; // u = .2, .4, .6, .8
  std::vector<f64> out(5U, kSentinel);
  const auto stats = eb::normal_scores(sorted, out);
  ASSERT_TRUE(stats) << stats.error().to_string();
  EXPECT_EQ(stats->scored, 4U);
  EXPECT_NEAR(out[2], -0.8416212335729143, 1e-12);
  EXPECT_NEAR(out[0], -0.2533471031357997, 1e-12);
  EXPECT_NEAR(out[3], 0.2533471031357997, 1e-12);
  EXPECT_NEAR(out[1], 0.8416212335729143, 1e-12);
  EXPECT_EQ(out[4], kSentinel); // not in the row: untouched
  EXPECT_NEAR(stats->max_abs, 0.8416212335729143, 1e-12);
}

// Five names, one tie block [1, 3): its two names share Phi^{-1}((1 + 3 + 1) / 12); the block
// [0, 1) and its mirror [4, 5) have opposite scores; the middle of a symmetric row is 0.
TEST(BookNormalScore, TiesShareTheMeanRankAndMirrorsAreOpposite) {
  const Row sorted{{1.0, 0U}, {2.0, 1U}, {2.0, 2U}, {3.0, 3U}, {4.0, 4U}};
  std::vector<f64> out(5U, kSentinel);
  ASSERT_TRUE(eb::normal_scores(sorted, out));
  EXPECT_EQ(out[1], out[2]);
  EXPECT_EQ(out[1], atx::engine::eval::norm_ppf(5.0 / 12.0));
  EXPECT_EQ(out[0], atx::engine::eval::norm_ppf(2.0 / 12.0));
  EXPECT_EQ(out[3], atx::engine::eval::norm_ppf(8.0 / 12.0));
  EXPECT_EQ(out[4], atx::engine::eval::norm_ppf(10.0 / 12.0));
  EXPECT_NEAR(out[0], -out[4], 1e-12);
  const Row symmetric{{-1.0, 0U}, {0.0, 1U}, {1.0, 2U}};
  std::vector<f64> mid(3U, kSentinel);
  ASSERT_TRUE(eb::normal_scores(symmetric, mid));
  EXPECT_NEAR(mid[1], 0.0, 1e-14);
  EXPECT_NEAR(mid[0], -0.6744897501960817, 1e-12); // u = 1/4
}

// The rule concentrates: on 1,001 names the largest score over the first quartile's |score| is
// about 4.6, against 2 for the uniform tied rank. Wrong maps differ at the extreme name.
TEST(BookNormalScore, FixtureTellsWrongRulesApart) {
  constexpr usize n = 1001U;
  Row sorted;
  for (usize k = 0U; k < n; ++k) sorted.emplace_back(static_cast<f64>(k), k);
  std::vector<f64> out(n, kSentinel);
  const auto stats = eb::normal_scores(sorted, out);
  ASSERT_TRUE(stats);
  const f64 top = out[n - 1U];
  EXPECT_EQ(top, atx::engine::eval::norm_ppf(static_cast<f64>(n) / static_cast<f64>(n + 1U)));
  EXPECT_NEAR(top, 3.0908, 1e-3); // Phi^{-1}(1001 / 1002)
  const f64 quarter = std::abs(out[n / 4U]);
  EXPECT_GT(top / quarter, 4.0);           // the tail carries more than the uniform rank's 2
  const f64 blom = atx::engine::eval::norm_ppf((static_cast<f64>(n) - 0.375) /
                                               (static_cast<f64>(n) + 0.25));
  const f64 over_n = atx::engine::eval::norm_ppf((static_cast<f64>(n) - 0.5) / static_cast<f64>(n));
  EXPECT_GT(std::abs(top - blom), 1e-3);
  EXPECT_GT(std::abs(top - over_n), 1e-3);
  EXPECT_GT(std::abs(top - 0.5), 1.0);     // the uniform tied rank of the top name
}

TEST(BookNormalScore, ShortRowsWriteNothingAndRefusalsWriteNothing) {
  std::vector<f64> out(3U, kSentinel);
  const auto one = eb::normal_scores(Row{{1.0, 1U}}, out);
  ASSERT_TRUE(one);
  EXPECT_EQ(one->scored, 0U);
  EXPECT_TRUE(eb::normal_scores(Row{}, out));
  for (const f64 v : out) EXPECT_EQ(v, kSentinel);
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  for (const Row& bad : {Row{{1.0, 0U}, {0.5, 1U}},          // out of order
                         Row{{0.0, 0U}, {nan, 1U}},          // NaN
                         Row{{0.0, 0U}, {1.0, 3U}}}) {       // index outside the row
    const auto r = eb::normal_scores(bad, out);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
  for (const f64 v : out) EXPECT_EQ(v, kSentinel);
}

} // namespace atx_test_v8_book_normal_score
