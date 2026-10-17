#include "fitting/term_prior.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

TermPriorNode node(double years, double atm, double weight, std::uint64_t id,
                   double skew = 0.0) {
  TermPriorNode out;
  out.variance_years = years;
  out.atm_vol = atm;
  out.weight = weight;
  out.id = id;
  for (std::size_t g = 0; g < kTermPriorGridSize; ++g) {
    out.multiples[g] = 1.0 + skew * kTermPriorGrid[g];
  }
  return out;
}

TEST(TermPrior, InterpolatesAtmTotalVarianceLinearlyInVarianceTime) {
  const std::vector<TermPriorNode> nodes{node(1.0, 0.30, 1.0, 2), node(0.25, 0.20, 1.0, 1)};
  const auto prior = TermPrior::create(nodes);
  ASSERT_TRUE(prior) << prior.error().to_string();
  const auto point = prior->at(0.5);
  ASSERT_TRUE(point);
  const double theta = 0.04 * 0.25 + (0.09 - 0.04 * 0.25) * (0.25 / 0.75);
  EXPECT_NEAR(point->atm_vol, std::sqrt(theta / 0.5), 1.0e-14);
  EXPECT_EQ(point->nodes_used, 2U);
}

TEST(TermPrior, HoldsAtmVolatilityFlatBeyondTheEndNodes) {
  const std::vector<TermPriorNode> nodes{node(0.25, 0.20, 1.0, 1), node(1.0, 0.30, 1.0, 2)};
  const auto prior = TermPrior::create(nodes);
  ASSERT_TRUE(prior);
  EXPECT_NEAR(prior->at(0.05)->atm_vol, 0.20, 1.0e-14);
  EXPECT_NEAR(prior->at(4.0)->atm_vol, 0.30, 1.0e-14);
}

TEST(TermPrior, PoolsAdjacentViolatorsIntoNonDecreasingTotalVariance) {
  // Total variances 0.04*0.25=0.010, then 0.0049*0.5=0.00245: a calendar violation.
  const std::vector<TermPriorNode> nodes{node(0.25, 0.20, 1.0, 1), node(0.5, 0.07, 3.0, 2),
                                         node(1.0, 0.30, 1.0, 3)};
  const auto prior = TermPrior::create(nodes);
  ASSERT_TRUE(prior);
  const double pooled = (0.010 * 1.0 + 0.00245 * 3.0) / 4.0;
  EXPECT_NEAR(prior->at(0.25)->atm_vol, std::sqrt(pooled / 0.25), 1.0e-14);
  EXPECT_NEAR(prior->at(0.5)->atm_vol, std::sqrt(pooled / 0.5), 1.0e-14);
  double previous = 0.0;
  for (double t = 0.1; t <= 2.0; t += 0.05) {
    const auto point = prior->at(t);
    ASSERT_TRUE(point);
    const double total = point->atm_vol * point->atm_vol * t;
    EXPECT_GE(total, previous - 1.0e-15) << t;
    previous = total;
  }
}

TEST(TermPrior, LeaveOneOutExcludesTheQueriedNode) {
  const std::vector<TermPriorNode> nodes{node(0.25, 0.20, 1.0, 7), node(1.0, 0.30, 1.0, 9)};
  const auto prior = TermPrior::create(nodes);
  ASSERT_TRUE(prior);
  EXPECT_NEAR(prior->at(0.25, 7)->atm_vol, 0.30, 1.0e-14);
  const std::vector<TermPriorNode> single{node(0.5, 0.25, 1.0, 4)};
  const auto lone = TermPrior::create(single);
  ASSERT_TRUE(lone);
  EXPECT_FALSE(lone->at(0.5, 4));
  EXPECT_TRUE(lone->at(0.5, 5));
}

TEST(TermPrior, ConfidenceDecaysWithLogTimeDistance) {
  const std::vector<TermPriorNode> nodes{node(0.5, 0.25, 1.0, 1)};
  const auto prior = TermPrior::create(nodes, TermPriorConfig{2.0});
  ASSERT_TRUE(prior);
  EXPECT_DOUBLE_EQ(prior->at(0.5)->confidence, 1.0);
  EXPECT_NEAR(prior->at(1.5)->confidence, std::exp(-std::log(3.0) / 2.0), 1.0e-14);
  EXPECT_NEAR(prior->at(0.5 / 3.0)->confidence, std::exp(-std::log(3.0) / 2.0), 1.0e-14);
}

TEST(TermPrior, InterpolatesShapeMultiplesLinearlyInTime) {
  const std::vector<TermPriorNode> nodes{node(0.25, 0.20, 1.0, 1, -0.10),
                                         node(0.75, 0.20, 1.0, 2, -0.02)};
  const auto prior = TermPrior::create(nodes);
  ASSERT_TRUE(prior);
  const auto point = prior->at(0.5);
  ASSERT_TRUE(point);
  for (std::size_t g = 0; g < kTermPriorGridSize; ++g) {
    EXPECT_NEAR(point->multiples[g], 1.0 - 0.06 * kTermPriorGrid[g], 1.0e-14);
  }
  EXPECT_EQ(prior->at(2.0)->multiples, nodes[1].multiples);
}

TEST(TermPrior, RejectsMalformedNodesAndEmptyQueries) {
  EXPECT_FALSE(TermPrior::create(std::vector<TermPriorNode>{node(0.0, 0.2, 1.0, 1)}));
  EXPECT_FALSE(TermPrior::create(std::vector<TermPriorNode>{node(0.5, 0.2, 0.0, 1)}));
  EXPECT_FALSE(TermPrior::create(
      std::vector<TermPriorNode>{node(0.5, 0.2, 1.0, 1), node(0.5, 0.3, 1.0, 2)}));
  auto bad_shape = node(0.5, 0.2, 1.0, 1);
  bad_shape.multiples[3] = 0.0;
  EXPECT_FALSE(TermPrior::create(std::vector<TermPriorNode>{bad_shape}));
  EXPECT_FALSE(TermPrior::create({}, TermPriorConfig{0.0}));
  std::vector<TermPriorNode> many;
  for (std::size_t i = 0; i <= kTermPriorMaxNodes; ++i) {
    many.push_back(node(0.01 * static_cast<double>(i + 1), 0.2, 1.0, i));
  }
  EXPECT_FALSE(TermPrior::create(many));
  const auto empty = TermPrior::create({});
  ASSERT_TRUE(empty);
  EXPECT_FALSE(empty->at(0.5));
  const auto prior = TermPrior::create(std::vector<TermPriorNode>{node(0.5, 0.2, 1.0, 1)});
  ASSERT_TRUE(prior);
  EXPECT_FALSE(prior->at(0.0));
  EXPECT_FALSE(prior->at(std::nan("")));
}

} // namespace
} // namespace atx::vol
