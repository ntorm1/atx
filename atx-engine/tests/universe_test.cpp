#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "atx/engine/data/universe.hpp"

using atx::engine::data::median;
using atx::engine::data::top_n_by_median_notional;

TEST(Universe, MedianOddAndEven) {
  EXPECT_DOUBLE_EQ(median({3.0, 1.0, 2.0}), 2.0);
  EXPECT_DOUBLE_EQ(median({4.0, 1.0, 3.0, 2.0}), 2.5);
  EXPECT_DOUBLE_EQ(median({}), 0.0);
}

TEST(Universe, TopNByMedianWithTieBreak) {
  std::unordered_map<std::string, std::vector<double>> m{
      {"HIGH", {100.0, 100.0, 100.0}}, // median 100
      {"MIDB", {50.0, 50.0}},          // median 50, tie with MIDA
      {"MIDA", {50.0, 50.0}},          // median 50
      {"LOW", {1.0}},                  // median 1
  };
  auto top = top_n_by_median_notional(m, 3);
  ASSERT_EQ(top.size(), 3U);
  EXPECT_EQ(top[0], "HIGH");
  EXPECT_EQ(top[1], "MIDA"); // tie-break: symbol ascending
  EXPECT_EQ(top[2], "MIDB");
}
