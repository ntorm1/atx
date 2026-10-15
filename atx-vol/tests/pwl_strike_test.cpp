#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <future>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/pwl_strike.hpp"

namespace {

using atx::vol::ConvexPwlStrikeFunction;
using atx::vol::PwlStrikeLimits;
using atx::vol::PwlStrikeTerm;
using atx::vol::Side;

[[nodiscard]] double payoff(double spot, double strike, Side side) {
  return std::max(side == Side::Call ? spot - strike : strike - spot, 0.0);
}

TEST(PwlStrike, TerminalPayoffsRetainKinksAndRefuseExtrapolation) {
  for (const Side side : {Side::Call, Side::Put}) {
    for (const double spot : {0.0, 50.0, 100.0, 150.0, 200.0}) {
      const auto function = ConvexPwlStrikeFunction::payoff(spot, side, 50.0, 150.0);
      ASSERT_TRUE(function.has_value()) << function.error().to_string();
      EXPECT_EQ(function->nodes().size(), spot == 100.0 ? 3u : 2u);
      for (int i = 0; i <= 400; ++i) {
        const double strike = 50.0 + 0.25 * static_cast<double>(i);
        EXPECT_NEAR(*function->value(strike), payoff(spot, strike, side), 4.0e-14);
      }
      EXPECT_FALSE(function->value(49.0).has_value());
      EXPECT_FALSE(function->value(151.0).has_value());
      EXPECT_FALSE(function->value(std::numeric_limits<double>::quiet_NaN()).has_value());
    }
  }
}

TEST(PwlStrike, PositiveWeightedSumPreservesEveryCoincidentAndDistinctKnot) {
  const auto a = ConvexPwlStrikeFunction::payoff(80.0, Side::Call, 50.0, 150.0);
  const auto b = ConvexPwlStrikeFunction::payoff(120.0, Side::Call, 50.0, 150.0);
  const auto c = ConvexPwlStrikeFunction::payoff(80.0, Side::Put, 50.0, 150.0);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_TRUE(c.has_value());
  const std::array terms{PwlStrikeTerm{&*a, 0.25}, PwlStrikeTerm{&*b, 1.04},
                          PwlStrikeTerm{&*c, 0.75}};
  const auto sum = ConvexPwlStrikeFunction::weighted_sum(terms);
  ASSERT_TRUE(sum.has_value()) << sum.error().to_string();
  const std::array expected_strikes{50.0, 80.0, 120.0, 150.0};
  ASSERT_EQ(sum->nodes().size(), expected_strikes.size());
  for (std::size_t i = 0u; i < expected_strikes.size(); ++i) {
    EXPECT_EQ(sum->nodes()[i].strike, expected_strikes[i]);
  }
  double previous_slope = -std::numeric_limits<double>::infinity();
  for (std::size_t i = 1u; i < sum->nodes().size(); ++i) {
    const auto &left = sum->nodes()[i - 1u];
    const auto &right = sum->nodes()[i];
    const double slope = (right.value - left.value) / (right.strike - left.strike);
    EXPECT_GE(slope, previous_slope);
    previous_slope = slope;
  }
  for (int i = 0; i <= 400; ++i) {
    const double strike = 50.0 + 0.25 * static_cast<double>(i);
    const double expected = 0.25 * payoff(80.0, strike, Side::Call) +
                            1.04 * payoff(120.0, strike, Side::Call) +
                            0.75 * payoff(80.0, strike, Side::Put);
    EXPECT_NEAR(*sum->value(strike), expected, 3.0e-14);
  }
  const auto repeat = ConvexPwlStrikeFunction::weighted_sum(terms);
  ASSERT_TRUE(repeat.has_value());
  for (std::size_t i = 0u; i < sum->nodes().size(); ++i) {
    EXPECT_EQ(repeat->nodes()[i].strike, sum->nodes()[i].strike);
    EXPECT_EQ(repeat->nodes()[i].value, sum->nodes()[i].value);
  }
}

TEST(PwlStrike, ExerciseInsertsHiddenInteriorPolicyCrossingsOnBothSides) {
  for (const Side side : {Side::Call, Side::Put}) {
    const auto a = ConvexPwlStrikeFunction::payoff(80.0, side, 50.0, 150.0);
    const auto b = ConvexPwlStrikeFunction::payoff(120.0, side, 50.0, 150.0);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    const std::array terms{PwlStrikeTerm{&*a, 0.45}, PwlStrikeTerm{&*b, 0.45}};
    const auto continuation = ConvexPwlStrikeFunction::weighted_sum(terms);
    ASSERT_TRUE(continuation.has_value());
    const auto exercised = continuation->max_intrinsic(100.0, side);
    ASSERT_TRUE(exercised.has_value()) << exercised.error().to_string();
    const double crossing = side == Side::Call ? 46.0 / 0.55 : 64.0 / 0.55;
    ASSERT_EQ(exercised->nodes().size(), 6u);
    bool has_crossing = false;
    for (const auto &node : exercised->nodes()) {
      has_crossing = has_crossing || std::abs(node.strike - crossing) < 1.0e-12;
    }
    EXPECT_TRUE(has_crossing);
    for (int i = 0; i <= 800; ++i) {
      const double strike = 50.0 + 0.125 * static_cast<double>(i);
      const double expected = std::max(
          0.45 * payoff(80.0, strike, side) + 0.45 * payoff(120.0, strike, side),
          payoff(100.0, strike, side));
      EXPECT_NEAR(*exercised->value(strike), expected, 3.0e-14);
    }
    // Interpolating only the original knots would overprice this interior.
    const double probe = side == Side::Call ? 90.0 : 110.0;
    const double endpoint_chord =
        0.5 * (*exercised->value(probe - 10.0) + *exercised->value(probe + 10.0));
    EXPECT_GT(endpoint_chord - *exercised->value(probe), 0.5);
  }
}

TEST(PwlStrike, ZeroWeightsRetainKnotsAndExerciseRestoresPayoffExactly) {
  const auto a = ConvexPwlStrikeFunction::payoff(80.0, Side::Call, 50.0, 150.0);
  const auto b = ConvexPwlStrikeFunction::payoff(120.0, Side::Call, 50.0, 150.0);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const std::array terms{PwlStrikeTerm{&*a, 0.0}, PwlStrikeTerm{&*b, 0.0}};
  const auto zero = ConvexPwlStrikeFunction::weighted_sum(terms);
  ASSERT_TRUE(zero.has_value());
  EXPECT_EQ(zero->nodes().size(), 4u);
  for (const auto &node : zero->nodes()) EXPECT_EQ(node.value, 0.0);
  const auto exercised = zero->max_intrinsic(100.0, Side::Put);
  ASSERT_TRUE(exercised.has_value());
  EXPECT_EQ(exercised->nodes().size(), 5u);
  for (int i = 0; i <= 100; ++i) {
    const double strike = 50.0 + static_cast<double>(i);
    EXPECT_EQ(exercised->value(strike), payoff(100.0, strike, Side::Put));
  }
}

TEST(PwlStrike, RefusesMalformedInputsAndUnequalDomains) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const double invalid : {-1.0, nan, inf}) {
    EXPECT_FALSE(ConvexPwlStrikeFunction::payoff(invalid, Side::Call, 50.0, 150.0));
  }
  for (const std::array<double, 2> domain :
       {std::array{0.0, 1.0}, std::array{100.0, 100.0}, std::array{101.0, 100.0},
         std::array{nan, 150.0}, std::array{50.0, inf}}) {
    EXPECT_FALSE(ConvexPwlStrikeFunction::payoff(100.0, Side::Call, domain[0], domain[1]));
  }
  EXPECT_FALSE(ConvexPwlStrikeFunction::payoff(100.0, static_cast<Side>(255), 50.0, 150.0));
  const auto a = ConvexPwlStrikeFunction::payoff(80.0, Side::Call, 50.0, 150.0);
  const auto b = ConvexPwlStrikeFunction::payoff(80.0, Side::Call, 50.0, 151.0);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  EXPECT_FALSE(a->max_intrinsic(nan, Side::Call));
  EXPECT_FALSE(a->max_intrinsic(100.0, static_cast<Side>(255)));
  EXPECT_FALSE(ConvexPwlStrikeFunction::weighted_sum({}));
  for (const double invalid : {-1.0, nan, inf}) {
    const std::array terms{PwlStrikeTerm{&*a, invalid}};
    EXPECT_FALSE(ConvexPwlStrikeFunction::weighted_sum(terms));
  }
  const std::array null_term{PwlStrikeTerm{nullptr, 1.0}};
  EXPECT_FALSE(ConvexPwlStrikeFunction::weighted_sum(null_term));
  const std::array domains{PwlStrikeTerm{&*a, 1.0}, PwlStrikeTerm{&*b, 1.0}};
  EXPECT_FALSE(ConvexPwlStrikeFunction::weighted_sum(domains));
}

TEST(PwlStrike, ResourceCapsRefuseInsteadOfDroppingKnotsOrTerms) {
  const auto a = ConvexPwlStrikeFunction::payoff(80.0, Side::Call, 50.0, 150.0);
  const auto b = ConvexPwlStrikeFunction::payoff(120.0, Side::Call, 50.0, 150.0);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const std::array terms{PwlStrikeTerm{&*a, 0.45}, PwlStrikeTerm{&*b, 0.45}};
  for (int cap = 0; cap != 4; ++cap) {
    PwlStrikeLimits limits;
    if (cap == 0) limits.max_nodes = 3u;
    if (cap == 1) limits.max_terms = 1u;
    if (cap == 2) limits.max_input_nodes = 5u;
    if (cap == 3) limits.max_workspace_nodes = 5u;
    const auto sum = ConvexPwlStrikeFunction::weighted_sum(terms, limits);
    ASSERT_FALSE(sum.has_value());
    EXPECT_EQ(sum.error().code(), atx::vol::ErrorCode::Unavailable);
  }
  const auto continuation = ConvexPwlStrikeFunction::weighted_sum(terms);
  ASSERT_TRUE(continuation.has_value());
  PwlStrikeLimits limits;
  limits.max_nodes = 5u;
  EXPECT_FALSE(continuation->max_intrinsic(100.0, Side::Call, limits));
  limits = {};
  limits.max_nodes = 2u;
  EXPECT_FALSE(ConvexPwlStrikeFunction::payoff(100.0, Side::Call, 50.0, 150.0, limits));
  EXPECT_TRUE(ConvexPwlStrikeFunction::payoff(200.0, Side::Call, 50.0, 150.0, limits));
  limits.max_nodes = atx::vol::kPwlStrikeMaxNodes + 1u;
  EXPECT_FALSE(ConvexPwlStrikeFunction::payoff(100.0, Side::Call, 50.0, 150.0, limits));
}

TEST(PwlStrike, ArithmeticOverflowReturnsExplicitFailure) {
  const auto function = ConvexPwlStrikeFunction::payoff(100.0, Side::Call, 50.0, 150.0);
  ASSERT_TRUE(function.has_value());
  const std::array terms{
      PwlStrikeTerm{&*function, std::numeric_limits<double>::max()}};
  const auto sum = ConvexPwlStrikeFunction::weighted_sum(terms);
  ASSERT_FALSE(sum.has_value());
  EXPECT_EQ(sum.error().code(), atx::vol::ErrorCode::OutOfRange);
}

TEST(PwlStrike, ResultOwnsItsChildrenAndSupportsConcurrentConstQueries) {
  const auto make_owned = [] {
    const auto a = ConvexPwlStrikeFunction::payoff(80.0, Side::Call, 50.0, 150.0);
    const auto b = ConvexPwlStrikeFunction::payoff(120.0, Side::Call, 50.0, 150.0);
    const std::array terms{PwlStrikeTerm{&*a, 0.25}, PwlStrikeTerm{&*b, 0.75}};
    return ConvexPwlStrikeFunction::weighted_sum(terms);
  };
  const auto owned = make_owned();
  ASSERT_TRUE(owned.has_value());
  std::vector<std::future<double>> readers;
  for (int i = 0; i != 4; ++i) {
    readers.push_back(std::async(std::launch::async, [&owned] {
      double sum = 0.0;
      for (int j = 0; j != 10000; ++j) {
        sum += *owned->value(50.0 + 0.01 * static_cast<double>(j));
      }
      return sum;
    }));
  }
  const double first = readers.front().get();
  for (std::size_t i = 1u; i < readers.size(); ++i) EXPECT_EQ(readers[i].get(), first);
  auto copy = *owned;
  const auto moved = std::move(copy);
  EXPECT_EQ(moved.value(100.0), 15.0);
  EXPECT_FALSE(copy.value(100.0).has_value());
  const std::array moved_term{PwlStrikeTerm{&copy, 1.0}};
  EXPECT_FALSE(ConvexPwlStrikeFunction::weighted_sum(moved_term));
}

} // namespace
