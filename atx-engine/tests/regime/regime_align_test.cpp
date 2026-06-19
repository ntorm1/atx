#include <cmath>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/regime/align.hpp"
#include "atx/engine/regime/series.hpp"

namespace atxtest_regime_align {
using atx::engine::regime::apply_derived;
using atx::engine::regime::build_master_axis;
using atx::engine::regime::DerivedSpec;
using atx::engine::regime::forward_fill;
using atx::engine::regime::NamedSeries;

constexpr atx::i64 kDay = 86400LL * 1000000000LL;

TEST(RegimeAlign, MasterAxis_UnionSortedDedupedFloored) {
  std::vector<NamedSeries> s = {
      {"a", {{2 * kDay, 1.0}, {5 * kDay, 2.0}}},
      {"b", {{1 * kDay, 9.0}, {5 * kDay, 8.0}}},
  };
  auto axis = build_master_axis(s, /*min_date_nanos=*/2 * kDay);
  ASSERT_EQ(axis.size(), 2u);   // 1*kDay dropped by floor
  EXPECT_EQ(axis[0], 2 * kDay);
  EXPECT_EQ(axis[1], 5 * kDay);
}

TEST(RegimeAlign, ForwardFill_CarriesAndLeavesNaNBeforeFirst) {
  std::vector<std::pair<atx::i64, atx::f64>> obs = {{2 * kDay, 10.0}, {4 * kDay, 20.0}};
  std::vector<atx::i64> axis = {1 * kDay, 2 * kDay, 3 * kDay, 4 * kDay, 5 * kDay};
  auto col = forward_fill(obs, axis);
  ASSERT_EQ(col.size(), 5u);
  EXPECT_TRUE(std::isnan(col[0]));     // before first obs
  EXPECT_DOUBLE_EQ(col[1], 10.0);
  EXPECT_DOUBLE_EQ(col[2], 10.0);      // carried
  EXPECT_DOUBLE_EQ(col[3], 20.0);
  EXPECT_DOUBLE_EQ(col[4], 20.0);      // carried past last obs
}

TEST(RegimeAlign, ApplyDerived_SubtractionElementwise) {
  std::vector<std::string> names = {"dgs2", "dgs10"};
  std::vector<std::vector<atx::f64>> cols = {{1.0, 2.0}, {3.0, 5.0}};
  DerivedSpec spec{"t10y2y", "dgs10", '-', "dgs2"};
  auto st = apply_derived(names, cols, spec);
  ASSERT_TRUE(st.has_value()) << (st ? "" : st.error().message());
  ASSERT_EQ(names.size(), 3u);
  EXPECT_EQ(names[2], "t10y2y");
  EXPECT_DOUBLE_EQ(cols[2][0], 2.0);   // 3 - 1
  EXPECT_DOUBLE_EQ(cols[2][1], 3.0);   // 5 - 2
}

TEST(RegimeAlign, ApplyDerived_MissingOperand_IsError) {
  std::vector<std::string> names = {"dgs10"};
  std::vector<std::vector<atx::f64>> cols = {{1.0}};
  DerivedSpec spec{"t10y2y", "dgs10", '-', "dgs2"};
  EXPECT_FALSE(apply_derived(names, cols, spec).has_value());
}
}  // namespace atxtest_regime_align
