#include <gtest/gtest.h>
#include <limits>
#include <vector>
#include "atx/engine/alpha/panel.hpp"
#include "sector_groups.hpp"

namespace { using atx::f64; using atx::usize; using atx::engine::alpha::Panel; }

TEST(SectorGroups, DistinctCodesBecomeDenseIds) {
  // 2 dates x 4 instruments; sector codes {10,10,30,20} -> dense ids by ascending code:
  // 10->0, 20->1, 30->2  => {0,0,2,1}
  const usize D = 2, N = 4;
  std::vector<f64> close(D * N, 100.0);
  std::vector<f64> sect = {10,10,30,20, 10,10,30,20};
  auto panel = Panel::create(D, N, {"close","sector"}, {close, sect}, {}).value();
  auto gm = atx::impl::sector_group_map(panel);
  ASSERT_EQ(gm.size(), N);
  EXPECT_EQ(gm[0], 0u); EXPECT_EQ(gm[1], 0u);
  EXPECT_EQ(gm[2], 2u); EXPECT_EQ(gm[3], 1u);
}

TEST(SectorGroups, MissingSectorFieldReturnsEmpty) {
  const usize D = 2, N = 3;
  std::vector<f64> close(D * N, 100.0);
  auto panel = Panel::create(D, N, {"close"}, {close}, {}).value();
  EXPECT_TRUE(atx::impl::sector_group_map(panel).empty());
}

TEST(SectorGroups, NanSectorGetsOwnTrailingGroup) {
  const usize D = 1, N = 3;
  std::vector<f64> close(D * N, 100.0);
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  std::vector<f64> sect = {5.0, nan, 5.0};
  auto panel = Panel::create(D, N, {"close","sector"}, {close, sect}, {}).value();
  auto gm = atx::impl::sector_group_map(panel);
  ASSERT_EQ(gm.size(), N);
  EXPECT_EQ(gm[0], 0u); EXPECT_EQ(gm[2], 0u);   // code 5 -> group 0
  EXPECT_EQ(gm[1], 1u);                          // NaN -> dedicated trailing group
}
