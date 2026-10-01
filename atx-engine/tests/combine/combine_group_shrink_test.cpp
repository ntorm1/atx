// combine_group_shrink_test.cpp -- shrunk within-group shares and the cross-group member cap
// (platform v8 R-10, composition ic-shrink-v1 in atx-impl).
//
// Suites: GroupShrink, GroupCap

#include <limits>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/group_shrink.hpp"

namespace atx_test_v8_group_shrink {

using atx::f64;
using atx::usize;
namespace cb = atx::engine::combine;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 kInf = std::numeric_limits<f64>::infinity();

// Members interleaved across the groups: group 0 {3, 1, -1} (mean 1), group 1 {-2, -6} (mean
// -4), group 2 {5}.
const std::vector<f64> kEstimate{3.0, -2.0, 1.0, 5.0, -1.0, -6.0};
const std::vector<usize> kGroup{0, 1, 0, 2, 0, 1};

// Intensity .5: group 0 shrinks to {2, 1, 0} (the last floored), shares {2/3, 1/3, 0}; group 1
// shrinks to {-3, -5}, both floored, so it takes the equal shares; group 2 keeps its member.
TEST(GroupShrink, SharesShrinkTowardTheGroupMeanFloorAndNormalise) {
  const auto out = cb::group_shrink_shares(kEstimate, kGroup, 3, 0.5, 0.0);
  ASSERT_TRUE(out) << out.error().to_string();
  const std::vector<f64> shrunk{2.0, -3.0, 1.0, 5.0, 0.0, -5.0};
  const std::vector<f64> share{2.0 / 3.0, 0.5, 1.0 / 3.0, 1.0, 0.0, 0.5};
  ASSERT_EQ(out->shrunk.size(), kEstimate.size());
  ASSERT_EQ(out->share.size(), kEstimate.size());
  for (usize k = 0; k < kEstimate.size(); ++k) {
    EXPECT_DOUBLE_EQ(out->shrunk[k], shrunk[k]) << k;
    EXPECT_DOUBLE_EQ(out->share[k], share[k]) << k;
  }
  EXPECT_EQ(out->equal, (std::vector<atx::u8>{0, 1, 0}));
}

// The ends of the intensity range: 0 keeps the floored estimates' proportions, 1 gives every
// member its group's mean (equal shares); inside, without a floored member and with a positive
// group mean, share = intensity / n + (1 - intensity) * estimate / sum.
TEST(GroupShrink, IntensityRunsFromProportionalToEqualShares) {
  const std::vector<f64> est{3.0, 1.0, -1.0};
  const std::vector<usize> one{0, 0, 0};
  const auto raw = cb::group_shrink_shares(est, one, 1, 0.0, 0.0);
  ASSERT_TRUE(raw);
  EXPECT_DOUBLE_EQ(raw->share[0], 0.75);
  EXPECT_DOUBLE_EQ(raw->share[1], 0.25);
  EXPECT_DOUBLE_EQ(raw->share[2], 0.0);
  const auto flat = cb::group_shrink_shares(est, one, 1, 1.0, 0.0);
  ASSERT_TRUE(flat);
  for (const f64 s : flat->share) EXPECT_DOUBLE_EQ(s, 1.0 / 3.0);
  const std::vector<f64> positive{3.0, 1.0, 2.0};
  const auto mid = cb::group_shrink_shares(positive, one, 1, 0.25, 0.0);
  ASSERT_TRUE(mid);
  for (usize k = 0; k < positive.size(); ++k)
    EXPECT_NEAR(mid->share[k], 0.25 / 3.0 + 0.75 * positive[k] / 6.0, 1e-15) << k;
  EXPECT_NEAR(std::accumulate(mid->share.begin(), mid->share.end(), 0.0), 1.0, 1e-15);
}

// A positive minimum keeps every member in: floored values are the minimum, never zero.
TEST(GroupShrink, PositiveMinimumKeepsEveryMember) {
  const auto out = cb::group_shrink_shares(std::vector<f64>{3.0, 1.0, -1.0}, std::vector<usize>{0, 0, 0}, 1,
                                           0.5, 0.5);
  ASSERT_TRUE(out);
  EXPECT_DOUBLE_EQ(out->share[0], 2.0 / 3.5);
  EXPECT_DOUBLE_EQ(out->share[1], 1.0 / 3.5);
  EXPECT_DOUBLE_EQ(out->share[2], 0.5 / 3.5);
  EXPECT_EQ(out->equal, (std::vector<atx::u8>{0}));
}

TEST(GroupShrink, RefusesBadShapesAndValues) {
  const std::vector<f64> two{1.0, 2.0};
  const std::vector<usize> pair{0, 0};
  EXPECT_FALSE(cb::group_shrink_shares({}, {}, 1, 0.5, 0.0));                               // no member
  EXPECT_FALSE(cb::group_shrink_shares(two, std::vector<usize>{0}, 1, 0.5, 0.0));           // shapes
  EXPECT_FALSE(cb::group_shrink_shares(two, pair, 0, 0.5, 0.0));                             // no group
  EXPECT_FALSE(cb::group_shrink_shares(two, std::vector<usize>{0, 1}, 1, 0.5, 0.0));         // index out of range
  EXPECT_FALSE(cb::group_shrink_shares(two, pair, 2, 0.5, 0.0));                             // group 1 has no member
  for (const f64 bad : {-0.1, 1.1, kNaN}) EXPECT_FALSE(cb::group_shrink_shares(two, pair, 1, bad, 0.0)) << bad;
  for (const f64 bad : {-1.0, kInf, kNaN}) EXPECT_FALSE(cb::group_shrink_shares(two, pair, 1, 0.5, bad)) << bad;
  for (const f64 bad : {kNaN, kInf})
    EXPECT_FALSE(cb::group_shrink_shares(std::vector<f64>{1.0, bad}, pair, 1, 0.5, 0.0)) << bad;
  EXPECT_FALSE(cb::group_shrink_shares(std::vector<f64>{1e308, 1e308}, pair, 1, 0.5, 0.0)); // the sum overflows
  EXPECT_TRUE(cb::group_shrink_shares(two, pair, 1, 0.5, 0.0));
}

// One pass: the capped member's excess goes to the other group only, pro rata.
TEST(GroupCap, ExcessGoesProRataToTheOtherGroups) {
  std::vector<f64> w{0.6, 0.1, 0.2, 0.1};
  const auto passes = cb::cap_across_groups(w, std::vector<usize>{0, 0, 1, 1}, 2, 0.4, 1e-12);
  ASSERT_TRUE(passes) << passes.error().to_string();
  EXPECT_EQ(*passes, 1U);
  EXPECT_DOUBLE_EQ(w[0], 0.4);
  EXPECT_DOUBLE_EQ(w[1], 0.1);                        // its own group's member takes nothing
  EXPECT_NEAR(w[2], 0.2 + 0.2 * 0.2 / 0.3, 1e-15);
  EXPECT_NEAR(w[3], 0.1 + 0.2 * 0.1 / 0.3, 1e-15);
  EXPECT_NEAR(w[0] + w[1] + w[2] + w[3], 1.0, 1e-15);
}

// Two groups spill in the first pass; a receiver then exceeds the cap and a second pass
// spills it. Fractions: {3/10, 1/20 | 3/10, 1/20 | 1/5, 1/10}, cap 1/4 -> pass 1 gives
// {1/4, 2/35 | 1/4, 2/35 | 9/35, 9/70}; pass 2 caps 9/35 and spreads 1/140 over the two
// unfrozen members of the other groups: {1/4, 17/280 | 1/4, 17/280 | 1/4, 9/70}.
TEST(GroupCap, RepeatsToAFixedPoint) {
  std::vector<f64> w{0.3, 0.05, 0.3, 0.05, 0.2, 0.1};
  const auto passes = cb::cap_across_groups(w, std::vector<usize>{0, 0, 1, 1, 2, 2}, 3, 0.25, 1e-12);
  ASSERT_TRUE(passes) << passes.error().to_string();
  EXPECT_EQ(*passes, 2U);
  const std::vector<f64> want{0.25, 17.0 / 280.0, 0.25, 17.0 / 280.0, 0.25, 9.0 / 70.0};
  for (usize k = 0; k < w.size(); ++k) EXPECT_NEAR(w[k], want[k], 1e-15) << k;
  EXPECT_NEAR(std::accumulate(w.begin(), w.end(), 0.0), 1.0, 1e-15);
}

TEST(GroupCap, ToleranceInfeasibleCapsAndRefusals) {
  // Within the relative tolerance nothing moves.
  std::vector<f64> at{0.25 * (1.0 + 1e-13), 0.25, 0.25, 0.25 * (1.0 - 1e-13)};
  const auto before = at;
  const auto none = cb::cap_across_groups(at, std::vector<usize>{0, 0, 1, 1}, 2, 0.25, 1e-12);
  ASSERT_TRUE(none);
  EXPECT_EQ(*none, 0U);
  EXPECT_EQ(at, before);
  // No other group, or only weightless receivers: infeasible.
  std::vector<f64> alone{1.0};
  EXPECT_FALSE(cb::cap_across_groups(alone, std::vector<usize>{0}, 1, 0.5, 1e-12));
  std::vector<f64> weightless{0.75, 0.0, 0.25};
  EXPECT_FALSE(cb::cap_across_groups(weightless, std::vector<usize>{0, 1, 0}, 2, 0.5, 1e-12));
  // Shapes and values.
  std::vector<f64> w{0.5, 0.5};
  const std::vector<usize> g{0, 1};
  EXPECT_FALSE(cb::cap_across_groups(w, std::vector<usize>{0}, 2, 0.5, 1e-12));
  EXPECT_FALSE(cb::cap_across_groups(w, g, 0, 0.5, 1e-12));
  EXPECT_FALSE(cb::cap_across_groups(w, std::vector<usize>{0, 2}, 2, 0.5, 1e-12));
  for (const f64 cap : {0.0, -1.0, kInf, kNaN}) EXPECT_FALSE(cb::cap_across_groups(w, g, 2, cap, 1e-12)) << cap;
  for (const f64 tol : {-1e-12, kInf, kNaN}) EXPECT_FALSE(cb::cap_across_groups(w, g, 2, 0.5, tol)) << tol;
  for (const f64 bad : {-0.1, kNaN, kInf}) {
    std::vector<f64> x{bad, 0.5};
    EXPECT_FALSE(cb::cap_across_groups(x, g, 2, 0.5, 1e-12)) << bad;
  }
  // A group index without a member is fine for the cap.
  EXPECT_TRUE(cb::cap_across_groups(w, g, 3, 0.5, 1e-12));
}

} // namespace atx_test_v8_group_shrink
