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

TEST(PwlStrike, AdjacentStrikeCrossingsRetainEndpointMaximaWithExplicitBound) {
  // Scaling by powers of two preserves the adjacent-strike geometry exactly.
  for (const int exponent : {-400, 0, 400}) {
    for (const Side side : {Side::Call, Side::Put}) {
      const double lo = std::ldexp(175.0, exponent);
      const double hi = std::nextafter(lo, std::numeric_limits<double>::infinity());
      const double child_spot = std::ldexp(
          std::nextafter(side == Side::Call ? 212.5 : 137.5, 0.0), exponent);
      const double exercise_spot = std::ldexp(side == Side::Call ? 193.75 : 156.25, exponent);
      const double weight = side == Side::Call ? 0.5000000000000002 : 0.4999999999999998;
      const auto child = ConvexPwlStrikeFunction::payoff(child_spot, side, lo, hi);
      ASSERT_TRUE(child);
      const std::array term{PwlStrikeTerm{&*child, weight}};
      const auto continuation = ConvexPwlStrikeFunction::weighted_sum(term);
      ASSERT_TRUE(continuation);
      const double gap_lo = *continuation->value(lo) - payoff(exercise_spot, lo, side);
      const double gap_hi = *continuation->value(hi) - payoff(exercise_spot, hi, side);
      ASSERT_NE(std::signbit(gap_lo), std::signbit(gap_hi));
      ASSERT_NE(gap_lo, 0.0);
      ASSERT_NE(gap_hi, 0.0);
      const auto result = continuation->max_intrinsic(exercise_spot, side);
      ASSERT_TRUE(result) << result.error().to_string();
      ASSERT_EQ(result->nodes().size(), 2u);
      EXPECT_EQ(result->value(lo), std::max(*continuation->value(lo),
                                           payoff(exercise_spot, lo, side)));
      EXPECT_EQ(result->value(hi), std::max(*continuation->value(hi),
                                           payoff(exercise_spot, hi, side)));
      // For two affine functions with opposing endpoint gaps a,b, the exact
      // maximum excess of their endpoint-max chord is a*b/(a+b).
      const double a = std::abs(gap_lo);
      const double b = std::abs(gap_hi);
      const double exact_chord_error = std::min(a, b) / (1.0 + std::min(a, b) / std::max(a, b));
      EXPECT_GE(result->crossing_resolution_error_bound(), exact_chord_error);
      EXPECT_GT(result->crossing_resolution_error_bound(), 0.0);
      EXPECT_EQ(continuation->crossing_resolution_error_bound(), 0.0);
    }
  }
}

TEST(PwlStrike, CrossingErrorSurvivesRepeatedPositiveCompositionAndExercise) {
  const double lo = 175.0;
  const double hi = std::nextafter(lo, std::numeric_limits<double>::infinity());
  const auto child = ConvexPwlStrikeFunction::payoff(
      std::nextafter(212.5, 0.0), Side::Call, lo, hi);
  ASSERT_TRUE(child);
  const std::array term{PwlStrikeTerm{&*child, 0.5000000000000002}};
  const auto base = ConvexPwlStrikeFunction::weighted_sum(term);
  ASSERT_TRUE(base);
  auto current = base->max_intrinsic(193.75, Side::Call);
  ASSERT_TRUE(current);
  ASSERT_GT(current->crossing_resolution_error_bound(), 0.0);
  for (int i = 0; i < 256; ++i) {
    const double inherited = current->crossing_resolution_error_bound();
    const std::array terms{PwlStrikeTerm{&*current, 0.5}, PwlStrikeTerm{&*base, 0.5}};
    const auto blended = ConvexPwlStrikeFunction::weighted_sum(terms);
    ASSERT_TRUE(blended) << blended.error().to_string();
    EXPECT_GE(blended->crossing_resolution_error_bound(), 0.5 * inherited);
    const double before_max = blended->crossing_resolution_error_bound();
    current = blended->max_intrinsic(193.75, Side::Call);
    ASSERT_TRUE(current) << current.error().to_string();
    EXPECT_GT(current->crossing_resolution_error_bound(), before_max);
    EXPECT_GE(*current->value(lo), payoff(193.75, lo, Side::Call));
    EXPECT_GE(*current->value(hi), payoff(193.75, hi, Side::Call));
  }
  const std::array zero{PwlStrikeTerm{&*current, 0.0}};
  const auto erased = ConvexPwlStrikeFunction::weighted_sum(zero);
  ASSERT_TRUE(erased);
  EXPECT_EQ(erased->crossing_resolution_error_bound(), 0.0);
}

TEST(PwlStrike, AmplifiedCrossingErrorCannotResetItsFixedBudget) {
  constexpr double lo = 1.0;
  const double hi = std::nextafter(lo, 2.0);
  const double ulp = hi - lo;
  const auto stock = ConvexPwlStrikeFunction::payoff(0.0, Side::Put, lo, hi);
  ASSERT_TRUE(stock);
  const std::array term{PwlStrikeTerm{&*stock, 3.5 * ulp}};
  const auto continuation = ConvexPwlStrikeFunction::weighted_sum(term);
  ASSERT_TRUE(continuation);
  const auto resolved = continuation->max_intrinsic(lo + 4.0 * ulp, Side::Call);
  ASSERT_TRUE(resolved) << resolved.error().to_string();
  ASSERT_GT(resolved->crossing_resolution_error_bound(), 0.0);
  // The absolute floor permits this tiny error initially. Amplifying the
  // expression must amplify its bound, not grant a fresh absolute allowance.
  const std::array amplified{PwlStrikeTerm{&*resolved, 1.0e16}};
  const auto rejected = ConvexPwlStrikeFunction::weighted_sum(amplified);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), atx::vol::ErrorCode::OutOfRange);
  EXPECT_NE(rejected.error().message().find("accumulated crossing"), std::string::npos);
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
