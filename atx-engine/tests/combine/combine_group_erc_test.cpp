// combine_group_erc_test.cpp -- equal risk contribution shares of a few groups (platform v8 X,
// lane XCOMB; composition theme-erc-v1 in atx-impl).
//
// Suite: GroupErc

#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/group_erc.hpp"

namespace atx_test_v8_group_erc {

using atx::f64;
using atx::u64;
using atx::usize;
namespace cb = atx::engine::combine;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr usize kSweeps = 10000;

// The 3 x 3 covariance built so that the shares (1/2, 1/3, 1/6) have equal risk contributions
// (off-diagonals 1, -1, 2; the diagonal solves b_t (C b)_t = 1): the same matrix as the shared
// fixture atx-impl/tests/fixtures/theme_erc_v1.json, there in another theme order.
std::vector<f64> general() {
  return {11.0 / 3.0, 1.0, -1.0, 1.0, 13.0 / 2.0, 2.0, -1.0, 2.0, 35.0};
}

// Two groups: s_1 = sigma_2 / (sigma_1 + sigma_2) whatever the correlation.
TEST(GroupErc, TwoGroupsAreInverseVolatilityWhateverTheCorrelation) {
  for (const f64 rho : {-0.5, 0.0, 0.7}) {
    const std::vector<f64> c{1.0, 2.0 * rho, 2.0 * rho, 4.0};
    const auto out = cb::group_erc_shares(c, 2, kSweeps);
    ASSERT_TRUE(out) << out.error().to_string();
    ASSERT_EQ(out->share.size(), 2U);
    EXPECT_NEAR(out->share[0], 2.0 / 3.0, 1e-15) << rho;
    EXPECT_NEAR(out->share[1], 1.0 / 3.0, 1e-15) << rho;
    EXPECT_LE(out->dispersion, 1e-14) << rho;
  }
}

// Equal correlations: s_t proportional to 1 / sigma_t (sigma 1, 2, 4 and rho .25: 4/7, 2/7, 1/7).
TEST(GroupErc, EquicorrelatedGroupsAreInverseVolatility) {
  const std::vector<f64> vol{1.0, 2.0, 4.0};
  std::vector<f64> c(9);
  for (usize i = 0; i < 3; ++i)
    for (usize j = 0; j < 3; ++j) c[i * 3 + j] = (i == j ? 1.0 : 0.25) * vol[i] * vol[j];
  const auto out = cb::group_erc_shares(c, 3, kSweeps);
  ASSERT_TRUE(out) << out.error().to_string();
  EXPECT_NEAR(out->share[0], 4.0 / 7.0, 1e-15);
  EXPECT_NEAR(out->share[1], 2.0 / 7.0, 1e-15);
  EXPECT_NEAR(out->share[2], 1.0 / 7.0, 1e-15);
}

// A covariance with unequal (and one negative) correlations: the shares are the constructed ones,
// the contributions are equal and reported, and inverse volatility would be wrong (by > 1e-2);
// the shares do not depend on the covariance's scale.
TEST(GroupErc, ContributionsAreEqualOnAGeneralCovariance) {
  const auto c = general();
  const auto out = cb::group_erc_shares(c, 3, kSweeps);
  ASSERT_TRUE(out) << out.error().to_string();
  ASSERT_EQ(out->contribution.size(), 3U);
  EXPECT_NEAR(out->share[0], 1.0 / 2.0, 1e-15);
  EXPECT_NEAR(out->share[1], 1.0 / 3.0, 1e-15);
  EXPECT_NEAR(out->share[2], 1.0 / 6.0, 1e-15);
  f64 sum = 0.0;
  for (usize t = 0; t < 3; ++t) {
    f64 marginal = 0.0;
    for (usize u = 0; u < 3; ++u) marginal += c[t * 3 + u] * out->share[u];
    EXPECT_NEAR(out->contribution[t], out->share[t] * marginal, 1e-15) << t;
    sum += out->share[t];
  }
  EXPECT_NEAR(sum, 1.0, 1e-15);
  EXPECT_NEAR(out->contribution[0], out->contribution[2], 1e-14);
  EXPECT_LE(out->dispersion, 1e-14);
  const f64 inverse = (1.0 / std::sqrt(c[0])) /
                      (1.0 / std::sqrt(c[0]) + 1.0 / std::sqrt(c[4]) + 1.0 / std::sqrt(c[8]));
  EXPECT_GT(std::abs(inverse - out->share[0]), 1e-2);
  auto small = c;
  for (auto& v : small) v *= 1e-6;
  const auto scaled = cb::group_erc_shares(small, 3, kSweeps);
  ASSERT_TRUE(scaled) << scaled.error().to_string();
  for (usize t = 0; t < 3; ++t) EXPECT_NEAR(scaled->share[t], out->share[t], 1e-15) << t;
}

// One group takes everything; the same inputs give the same bits; a converged solve does not move
// with more sweeps, and one sweep of a correlated problem is not converged (the caller's check).
TEST(GroupErc, OneGroupRepeatsAndConvergence) {
  const auto one = cb::group_erc_shares(std::vector<f64>{3.0}, 1, kSweeps);
  ASSERT_TRUE(one) << one.error().to_string();
  EXPECT_EQ(one->share, (std::vector<f64>{1.0}));
  EXPECT_EQ(one->dispersion, 0.0);
  const auto c = general();
  const auto a = cb::group_erc_shares(c, 3, kSweeps);
  const auto b = cb::group_erc_shares(c, 3, kSweeps);
  const auto more = cb::group_erc_shares(c, 3, 2 * kSweeps);
  ASSERT_TRUE(a && b && more);
  for (usize t = 0; t < 3; ++t) {
    EXPECT_EQ(std::bit_cast<u64>(a->share[t]), std::bit_cast<u64>(b->share[t])) << t;
    EXPECT_NEAR(more->share[t], a->share[t], 1e-15) << t;
  }
  const std::vector<f64> hard{1.0, 0.99, -0.5, 0.99, 4.0, 0.2, -0.5, 0.2, 9.0};
  const auto once = cb::group_erc_shares(hard, 3, 1);
  ASSERT_TRUE(once) << once.error().to_string();
  EXPECT_GT(once->dispersion, 1e-10);
  const auto done = cb::group_erc_shares(hard, 3, kSweeps);
  ASSERT_TRUE(done) << done.error().to_string();
  EXPECT_LE(done->dispersion, 1e-12);
}

TEST(GroupErc, RefusesBadShapesAndValues) {
  const auto c = general();
  EXPECT_FALSE(cb::group_erc_shares({}, 0, kSweeps));
  EXPECT_FALSE(cb::group_erc_shares(c, 2, kSweeps));                      // not groups x groups
  EXPECT_FALSE(cb::group_erc_shares(c, 3, 0));                            // no sweep
  auto asymmetric = c;
  asymmetric[1] += 1e-9;
  EXPECT_FALSE(cb::group_erc_shares(asymmetric, 3, kSweeps));
  auto zero = c;
  zero[4] = 0.0;
  EXPECT_FALSE(cb::group_erc_shares(zero, 3, kSweeps));
  auto negative = c;
  negative[8] = -1.0;
  EXPECT_FALSE(cb::group_erc_shares(negative, 3, kSweeps));
  auto missing = c;
  missing[2] = kNaN;
  missing[6] = kNaN;
  EXPECT_FALSE(cb::group_erc_shares(missing, 3, kSweeps));
  const auto refused = cb::group_erc_shares(asymmetric, 3, kSweeps);
  ASSERT_FALSE(refused);
  const std::string message = refused.error().to_string();
  EXPECT_NE(message.find("group erc: the covariance is not symmetric"), std::string::npos) << message;
}

} // namespace atx_test_v8_group_erc
