#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/fitting/american_price_slice.hpp"

namespace {

using atx::vol::AmericanPriceBounds;
using atx::vol::AmericanPriceNode;
using atx::vol::AmericanPriceSlice;
using atx::vol::check_american_calendar;
using atx::vol::Side;

constexpr AmericanPriceBounds kBounds{100.0, 1.0, 1.0, 100.0, 100.0};

[[nodiscard]] std::vector<AmericanPriceNode> clean_nodes() {
  return {{80.0, 22.0, 2.0},
          {90.0, 14.0, 4.0},
          {100.0, 8.0, 8.0},
          {110.0, 4.0, 14.0},
          {120.0, 2.0, 22.0}};
}

// Independent closed form: at zero rates/dividends American = European for
// both sides. No production pricing kernel is used to produce these fixtures.
[[nodiscard]] AmericanPriceNode flat_vol_node(double strike, double vol, double time) {
  const double scale = vol * std::sqrt(time);
  const double d1 = std::log(100.0 / strike) / scale + 0.5 * scale;
  const double d2 = d1 - scale;
  const auto cdf = [](double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); };
  return {strike, 100.0 * cdf(d1) - strike * cdf(d2), strike * cdf(-d2) - 100.0 * cdf(-d1)};
}

TEST(AmericanPriceSlice, IndependentFlatVolPricesRemainAdmissibleBetweenNodes) {
  std::vector<AmericanPriceNode> nodes;
  for (const double strike : {80.0, 90.0, 100.0, 110.0, 120.0}) {
    nodes.push_back(flat_vol_node(strike, 0.25, 0.5));
  }
  const auto fit = AmericanPriceSlice::create(nodes, kBounds);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  EXPECT_EQ(fit->nodes().size(), nodes.size());
  for (const auto &node : nodes) {
    EXPECT_EQ(fit->price(node.strike, Side::Call), node.call);
    EXPECT_EQ(fit->price(node.strike, Side::Put), node.put);
  }
  double previous_call = *fit->price(80.0, Side::Call);
  double previous_put = *fit->price(80.0, Side::Put);
  double previous_call_change = -1.0;
  double previous_put_change = 0.0;
  for (int i = 1; i <= 160; ++i) {
    const double strike = 80.0 + 0.25 * static_cast<double>(i);
    const double call = *fit->price(strike, Side::Call);
    const double put = *fit->price(strike, Side::Put);
    const auto truth = flat_vol_node(strike, 0.25, 0.5);
    EXPECT_GE(call + 1.0e-12, truth.call); // convex graph lies below each chord
    EXPECT_GE(put + 1.0e-12, truth.put);
    EXPECT_NEAR(call - put, 100.0 - strike, 1.0e-12);
    EXPECT_GE(call, std::max(100.0 - strike, 0.0));
    EXPECT_GE(put, std::max(strike - 100.0, 0.0));
    EXPECT_LE(call, 100.0);
    EXPECT_LE(put, strike);
    const double dc = call - previous_call;
    const double dp = put - previous_put;
    EXPECT_LE(dc, 1.0e-12);
    EXPECT_GE(dc, -0.25 - 1.0e-12);
    EXPECT_GE(dp, -1.0e-12);
    EXPECT_LE(dp, 0.25 + 1.0e-12);
    EXPECT_GE(dc, previous_call_change - 1.0e-12);
    EXPECT_GE(dp, previous_put_change - 1.0e-12);
    previous_call = call;
    previous_put = put;
    previous_call_change = dc;
    previous_put_change = dp;
  }
}

TEST(AmericanPriceSlice, OwnsNodesAndRefusesExtrapolationAndInvalidSide) {
  auto input = clean_nodes();
  const auto fit = AmericanPriceSlice::create(input, kBounds);
  ASSERT_TRUE(fit.has_value());
  input.front().call = 99.0;
  input.clear();
  EXPECT_EQ(fit->price(80.0, Side::Call), 22.0);
  EXPECT_EQ(fit->price(95.0, Side::Call), 11.0);
  EXPECT_EQ(fit->price(95.0, Side::Put), 6.0);
  for (const double strike : {79.99, 120.01, -1.0, std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity()}) {
    EXPECT_FALSE(fit->price(strike, Side::Call).has_value());
    EXPECT_FALSE(fit->price(strike, Side::Put).has_value());
  }
  EXPECT_FALSE(fit->price(100.0, static_cast<Side>(255)).has_value());
}

TEST(AmericanPriceSlice, RejectsInvalidNodesAndCarryBounds) {
  const auto valid = clean_nodes();
  EXPECT_FALSE(AmericanPriceSlice::create({}, kBounds).has_value());
  EXPECT_FALSE(AmericanPriceSlice::create(std::span(valid).first(1), kBounds).has_value());
  for (const double bad :
       {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    for (const auto member :
         {&AmericanPriceNode::strike, &AmericanPriceNode::call, &AmericanPriceNode::put}) {
      auto input = valid;
      input[2].*member = bad;
      EXPECT_FALSE(AmericanPriceSlice::create(input, kBounds).has_value());
    }
    for (const auto member : {&AmericanPriceBounds::spot, &AmericanPriceBounds::discount_min,
                              &AmericanPriceBounds::discount_max, &AmericanPriceBounds::prepaid_min,
                              &AmericanPriceBounds::prepaid_max}) {
      auto bounds = kBounds;
      bounds.*member = bad;
      EXPECT_FALSE(AmericanPriceSlice::create(valid, bounds).has_value());
    }
  }
  auto duplicate = valid;
  duplicate[2].strike = duplicate[1].strike;
  EXPECT_FALSE(AmericanPriceSlice::create(duplicate, kBounds).has_value());
  auto unsorted = valid;
  std::swap(unsorted[1], unsorted[2]);
  EXPECT_FALSE(AmericanPriceSlice::create(unsorted, kBounds).has_value());
  auto bounds = kBounds;
  bounds.discount_min = 1.01;
  EXPECT_FALSE(AmericanPriceSlice::create(valid, bounds).has_value());
  bounds = kBounds;
  bounds.prepaid_min = 101.0;
  EXPECT_FALSE(AmericanPriceSlice::create(valid, bounds).has_value());
}

TEST(AmericanPriceSlice, RejectsHostileButterflyDespiteMonotoneBoundedPrices) {
  auto input = clean_nodes();
  input[2].call += 2.0;
  input[2].put += 2.0; // preserve exact cross-side parity
  const auto fit = AmericanPriceSlice::create(input, kBounds);
  ASSERT_FALSE(fit.has_value());
  EXPECT_NE(fit.error().message().find("convexity"), std::string::npos);
}

TEST(AmericanPriceSlice, RejectsMonotonicityAndCallAndPutSlopeViolations) {
  auto input = clean_nodes();
  input[1].call = 23.0;
  input[1].put = 13.0;
  auto fit = AmericanPriceSlice::create(input, kBounds);
  ASSERT_FALSE(fit.has_value());
  EXPECT_NE(fit.error().message().find("vertical-spread"), std::string::npos);

  constexpr AmericanPriceBounds wide{100.0, 1.0, 1.0, 80.0, 120.0};
  const std::array call_slope{AmericanPriceNode{80.0, 35.0, 2.0},
                              AmericanPriceNode{90.0, 20.0, 4.0}};
  fit = AmericanPriceSlice::create(call_slope, wide);
  ASSERT_FALSE(fit.has_value());
  EXPECT_NE(fit.error().message().find("vertical-spread"), std::string::npos);
  const std::array put_slope{AmericanPriceNode{80.0, 22.0, 2.0},
                             AmericanPriceNode{90.0, 14.0, 17.0}};
  fit = AmericanPriceSlice::create(put_slope, wide);
  ASSERT_FALSE(fit.has_value());
  EXPECT_NE(fit.error().message().find("vertical-spread"), std::string::npos);
}

TEST(AmericanPriceSlice, RejectsIntrinsicUpperAndCrossSideViolations) {
  auto input = clean_nodes();
  input.front().call = 19.0;
  auto fit = AmericanPriceSlice::create(input, kBounds);
  ASSERT_FALSE(fit.has_value());
  EXPECT_NE(fit.error().message().find("intrinsic"), std::string::npos);
  input = clean_nodes();
  input.front().call = 101.0;
  fit = AmericanPriceSlice::create(input, kBounds);
  ASSERT_FALSE(fit.has_value());
  EXPECT_NE(fit.error().message().find("upper price"), std::string::npos);
  input = clean_nodes();
  input.back().put = 121.0;
  fit = AmericanPriceSlice::create(input, kBounds);
  ASSERT_FALSE(fit.has_value());
  EXPECT_NE(fit.error().message().find("upper price"), std::string::npos);
  input = clean_nodes();
  input.front().put = 3.0;
  fit = AmericanPriceSlice::create(input, kBounds);
  ASSERT_FALSE(fit.has_value());
  EXPECT_NE(fit.error().message().find("call-minus-put"), std::string::npos);
}

TEST(AmericanPriceSlice, SupportsNegativeRatesAndPutSlopesAboveOne) {
  // Deterministic underlying with negative rates: calls exercise immediately;
  // puts optimally wait. This is independently known at zero volatility.
  constexpr AmericanPriceBounds bounds{100.0, 1.0, 1.1, 100.0, 100.0};
  const std::array nodes{AmericanPriceNode{80.0, 20.0, 0.0}, AmericanPriceNode{90.0, 10.0, 0.0},
                         AmericanPriceNode{100.0, 0.0, 10.0}, AmericanPriceNode{110.0, 0.0, 21.0},
                         AmericanPriceNode{120.0, 0.0, 32.0}};
  const auto fit = AmericanPriceSlice::create(nodes, bounds);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  EXPECT_NEAR(*fit->price(115.0, Side::Put), 26.5, 1.0e-12);
  EXPECT_FALSE(AmericanPriceSlice::create(nodes, kBounds).has_value());
}

TEST(AmericanPriceSlice, FixedToleranceAcceptsRoundoffButNotQuoteSizedBreaches) {
  auto input = clean_nodes();
  input.front().put += 0.5e-8;
  EXPECT_TRUE(AmericanPriceSlice::create(input, kBounds).has_value());
  input.front().put += 1.0e-5;
  EXPECT_FALSE(AmericanPriceSlice::create(input, kBounds).has_value());
}

TEST(AmericanPriceSlice, CalendarFindsCrossingAtTheOtherSlicesInteriorKnot) {
  const std::array sparse{AmericanPriceNode{90.0, 14.0, 4.0}, AmericanPriceNode{110.0, 4.0, 14.0}};
  const std::array dense{AmericanPriceNode{90.0, 14.0, 4.0}, AmericanPriceNode{100.0, 8.0, 8.0},
                         AmericanPriceNode{110.0, 4.0, 14.0}};
  const auto earlier = AmericanPriceSlice::create(sparse, kBounds);
  const auto later = AmericanPriceSlice::create(dense, kBounds);
  ASSERT_TRUE(earlier.has_value());
  ASSERT_TRUE(later.has_value());
  EXPECT_EQ(earlier->price(90.0, Side::Call), later->price(90.0, Side::Call));
  EXPECT_EQ(earlier->price(110.0, Side::Call), later->price(110.0, Side::Call));
  const auto crossing = check_american_calendar(*earlier, *later);
  ASSERT_FALSE(crossing.has_value());
  EXPECT_NE(crossing.error().message().find("100.000000"), std::string::npos);
  EXPECT_TRUE(check_american_calendar(*later, *earlier).has_value());
}

TEST(AmericanPriceSlice, CalendarChecksBothSidesOnPartialOverlapAndDisjointDomains) {
  constexpr AmericanPriceBounds wide{100.0, 0.95, 1.0, 95.0, 105.0};
  const std::array near_nodes{AmericanPriceNode{80.0, 22.0, 2.0},
                              AmericanPriceNode{100.0, 8.0, 8.0},
                              AmericanPriceNode{120.0, 2.0, 22.0}};
  const std::array far_nodes{AmericanPriceNode{90.0, 15.0, 4.0}, AmericanPriceNode{100.0, 9.0, 7.0},
                             AmericanPriceNode{110.0, 5.0, 14.0}};
  const auto earlier = AmericanPriceSlice::create(near_nodes, wide);
  const auto later = AmericanPriceSlice::create(far_nodes, wide);
  ASSERT_TRUE(earlier.has_value());
  ASSERT_TRUE(later.has_value()) << later.error().to_string();
  const auto crossing = check_american_calendar(*earlier, *later);
  ASSERT_FALSE(crossing.has_value());
  EXPECT_NE(crossing.error().message().find("put"), std::string::npos);
  const std::array far_domain{AmericanPriceNode{130.0, 0.5, 30.5},
                              AmericanPriceNode{140.0, 0.25, 40.25}};
  const auto disjoint = AmericanPriceSlice::create(far_domain, kBounds);
  ASSERT_TRUE(disjoint.has_value());
  EXPECT_TRUE(check_american_calendar(*earlier, *disjoint).has_value());
  EXPECT_TRUE(check_american_calendar(*disjoint, *earlier).has_value());
}

} // namespace
