#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/eval/cpcv_date.hpp"

namespace atxtest_cpcv_date {
using atx::usize;
using namespace atx::engine::eval;

TEST(EvalCpcvDate, SparseCalendarAndExclusiveEndEmbargo) {
  const std::array<LabelSpan, 6> labels{{{0, 1}, {1, 2}, {2, 3}, {10, 11}, {11, 12}, {12, 13}}};
  const auto plan = cpcv_date_plan(labels, DateCpcvConfig{3, 1, 2});
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->folds[0].test_idx, (std::vector<usize>{0, 1}));
  EXPECT_EQ(plan->folds[0].train_idx, (std::vector<usize>{3, 4, 5}));
  const std::array<LabelSpan, 4> endpoints{{{0, 1}, {1, 2}, {3, 4}, {4, 5}}};
  const auto exact = cpcv_date_plan(endpoints, DateCpcvConfig{2, 1, 2});
  ASSERT_TRUE(exact);
  EXPECT_EQ(exact->folds[0].train_idx, (std::vector<usize>{3}));
}

TEST(EvalCpcvDate, CrossSectionRowsCannotSplitADate) {
  const std::array<LabelSpan, 9> labels{{{0, 1}, {0, 2}, {0, 1}, {1, 2},
      {2, 3}, {2, 4}, {3, 4}, {4, 5}, {5, 6}}};
  const auto plan = cpcv_date_plan(labels, DateCpcvConfig{3, 1, 0});
  ASSERT_TRUE(plan);
  EXPECT_EQ(plan->group_offsets, (std::vector<usize>{0, 4, 7, 9}));
  EXPECT_EQ(plan->folds[0].test_idx, (std::vector<usize>{0, 1, 2, 3}));
  for (const auto& fold : plan->folds) {
    for (usize i = 1; i < labels.size(); ++i) {
      if (labels[i].t0 != labels[i - 1].t0) continue;
      EXPECT_EQ(std::binary_search(fold.test_idx.begin(), fold.test_idx.end(), i),
                std::binary_search(fold.test_idx.begin(), fold.test_idx.end(), i - 1));
    }
  }
}

TEST(EvalCpcvDate, DisconnectedWindowsMatchBruteForceOverlapOracle) {
  std::vector<LabelSpan> labels;
  for (usize i = 0; i < 24; ++i) labels.push_back({3 * i, 3 * i + 1 + i % 4});
  const auto plan = cpcv_date_plan(labels, DateCpcvConfig{6, 2, 2});
  ASSERT_TRUE(plan);
  for (const auto& fold : plan->folds) {
    std::vector<usize> expected;
    for (usize i = 0; i < labels.size(); ++i) {
      bool keep = !std::binary_search(fold.test_idx.begin(), fold.test_idx.end(), i);
      for (const auto t : fold.test_idx)
        if (labels[i].t0 < labels[t].t1 + 2 && labels[t].t0 < labels[i].t1) keep = false;
      if (keep) expected.push_back(i);
    }
    EXPECT_EQ(fold.train_idx, expected);
  }
  // Test groups 0 and 5 leave the middle dates available; no convex-hull purge.
  EXPECT_FALSE(plan->folds[4].train_idx.empty());
}

TEST(EvalCpcvDate, PathsUseEachHeldOutOccurrenceOnce) {
  std::vector<LabelSpan> labels;
  for (usize i = 0; i < 24; ++i) labels.push_back({i, i + 1});
  const auto plan = cpcv_date_plan(labels, DateCpcvConfig{6, 3, 0});
  ASSERT_TRUE(plan);
  ASSERT_EQ(plan->folds.size(), 20U);
  ASSERT_EQ(plan->paths.size(), 10U);
  std::set<std::pair<usize, usize>> seen;
  for (const auto& path : plan->paths) {
    ASSERT_EQ(path.size(), 6U);
    for (usize g = 0; g < path.size(); ++g) {
      ASSERT_LT(path[g], plan->folds.size());
      EXPECT_TRUE(seen.emplace(path[g], g).second);
      const auto& test = plan->folds[path[g]].test_idx;
      for (auto row = plan->group_offsets[g]; row < plan->group_offsets[g + 1]; ++row)
        EXPECT_TRUE(std::binary_search(test.begin(), test.end(), row));
    }
  }
  EXPECT_EQ(seen.size(), 60U);
  const auto again = cpcv_date_plan(labels, DateCpcvConfig{6, 3, 0});
  ASSERT_TRUE(again);
  EXPECT_EQ(again->paths, plan->paths);
}

TEST(EvalCpcvDate, RejectsMalformedClockGeometryAndBudgetBeforeExpansion) {
  const std::array<LabelSpan, 2> valid{{{0, 1}, {1, 2}}};
  EXPECT_FALSE(cpcv_date_plan({}, DateCpcvConfig{2, 1, 0}));
  EXPECT_FALSE(cpcv_date_plan(valid, DateCpcvConfig{21, 1, 0}));
  EXPECT_FALSE(cpcv_date_plan(valid, DateCpcvConfig{2, 2, 0}));
  EXPECT_FALSE(cpcv_date_plan(valid, DateCpcvConfig{2, 1, 0, 1}));
  EXPECT_FALSE(cpcv_date_plan(valid, DateCpcvConfig{3, 1, 0}));
  const std::array<LabelSpan, 2> backwards{{{1, 2}, {0, 1}}};
  EXPECT_FALSE(cpcv_date_plan(backwards, DateCpcvConfig{2, 1, 0}));
  const std::array<LabelSpan, 2> empty{{{0, 0}, {1, 2}}};
  EXPECT_FALSE(cpcv_date_plan(empty, DateCpcvConfig{2, 1, 0}));
  const auto maximum = std::numeric_limits<usize>::max();
  const std::array<LabelSpan, 2> near_limit{{{maximum - 2, maximum - 1}, {maximum - 1, maximum}}};
  EXPECT_FALSE(cpcv_date_plan(near_limit, DateCpcvConfig{2, 1, 1}));
  EXPECT_TRUE(cpcv_date_plan(near_limit, DateCpcvConfig{2, 1, 0}));
}

TEST(EvalCpcvDate, UnitDateZeroEmbargoReproducesLegacyFolds) {
  std::vector<LabelSpan> labels;
  for (usize i = 0; i < 24; ++i) labels.push_back({i, i + 3});
  const auto legacy = cpcv_folds(labels, CpcvConfig{6, 2, 0.0});
  const auto plan = cpcv_date_plan(labels, DateCpcvConfig{6, 2, 0});
  ASSERT_TRUE(plan);
  ASSERT_EQ(plan->folds.size(), legacy.size());
  for (usize i = 0; i < legacy.size(); ++i) {
    EXPECT_EQ(plan->folds[i].train_idx, legacy[i].train_idx);
    EXPECT_EQ(plan->folds[i].test_idx, legacy[i].test_idx);
  }
}
} // namespace atxtest_cpcv_date
