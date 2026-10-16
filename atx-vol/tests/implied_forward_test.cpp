#include "atx/vol/api/marketdata/implied_forward.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/american.hpp"
#include "atx/vol/api/pricing/black76.hpp"

namespace atx::vol {
namespace {

struct Chain {
  double spot = 100.0;
  double years = 1.0;
  double rate = 0.05;
  double yield = 0.02;
  double sigma = 0.25;
  double half_spread = 0.02;
  double forward() const { return spot * std::exp((rate - yield) * years); }
  double discount() const { return std::exp(-rate * years); }
  ImpliedForwardMarket market() const { return {spot, years, rate, {}}; }
};

// Smile with a mild skew so call and put legs do not share one flat vol by
// accident of the generator.
double smile(const Chain &c, double strike) {
  return c.sigma - 0.05 * std::log(strike / c.forward());
}

ImpliedForwardPair quote_pair(double strike, double call, double put, double half_spread) {
  const auto bid = [&](double price) { return std::max(price - half_spread, 1.0e-3); };
  return {strike, bid(call), call + half_spread, bid(put), put + half_spread};
}

std::vector<ImpliedForwardPair> european_chain(const Chain &c, double lo = 60.0,
                                               double hi = 150.0, double step = 5.0) {
  std::vector<ImpliedForwardPair> pairs;
  for (double k = lo; k <= hi + 1.0e-9; k += step) {
    const double sigma = smile(c, k);
    const double call = black76_price(c.forward(), k, c.years, sigma, c.discount(), Side::Call);
    const double put = black76_price(c.forward(), k, c.years, sigma, c.discount(), Side::Put);
    pairs.push_back(quote_pair(k, call, put, c.half_spread));
  }
  return pairs;
}

// American chain priced by the library's own Andersen-Lake pricer (accurate
// preset) at continuous yield c.yield. One vol per strike, as a surface would.
std::vector<ImpliedForwardPair> american_chain(const Chain &c, double lo = 60.0,
                                               double hi = 150.0, double step = 5.0) {
  std::vector<ImpliedForwardPair> pairs;
  for (double k = lo; k <= hi + 1.0e-9; k += step) {
    const double sigma = smile(c, k);
    const auto call = andersen_lake(c.spot, k, c.years, sigma, c.rate, c.yield, Side::Call);
    const auto put = andersen_lake(c.spot, k, c.years, sigma, c.rate, c.yield, Side::Put);
    EXPECT_TRUE(call && put);
    pairs.push_back(quote_pair(k, *call, *put, c.half_spread));
  }
  return pairs;
}

ImpliedForwardConfig european_parity() {
  ImpliedForwardConfig cfg;
  cfg.exercise = ImpliedForwardExercise::European;
  cfg.method = ImpliedForwardMethod::Parity;
  return cfg;
}

TEST(ImpliedForward, EuropeanParityRecoversForwardYieldAndPrepaidDeficit) {
  const Chain c;
  const auto pairs = european_chain(c);
  const auto r = estimate_implied_forward(c.market(), pairs, european_parity());
  ASSERT_EQ(r.status, ImpliedForwardStatus::Ok) << r.reason;
  EXPECT_NEAR(r.forward / c.forward() - 1.0, 0.0, 1.0e-12);
  EXPECT_NEAR(r.implied_yield, c.yield, 1.0e-11);
  EXPECT_NEAR(r.prepaid_deficit, c.spot - c.forward() * c.discount(), 1.0e-9);
  EXPECT_DOUBLE_EQ(r.discount, c.discount());
  EXPECT_EQ(r.pairs_input, pairs.size());
  EXPECT_EQ(r.pairs_valid, pairs.size());
  EXPECT_GE(r.pairs_retained, 3u);
  EXPECT_LE(r.pairs_selected, 8u);
  EXPECT_TRUE(r.channel_consistent);
  EXPECT_LE(r.forward_lower, c.forward());
  EXPECT_GE(r.forward_upper, c.forward());
  EXPECT_DOUBLE_EQ(r.exercise_shift_sd, 0.0);
  EXPECT_TRUE(std::isnan(r.forward_offset));
  // The ATM straddle unit is a scale, not a fit: it must be in the right decade.
  EXPECT_NEAR(r.atm_total_vol, c.sigma * std::sqrt(c.years), 0.05);
}

TEST(ImpliedForward, SelectsNearMoneyPairsInIncreasingDistance) {
  const Chain c;
  const auto pairs = european_chain(c);
  const auto r = estimate_implied_forward(c.market(), pairs, european_parity());
  ASSERT_EQ(r.status, ImpliedForwardStatus::Ok) << r.reason;
  for (std::size_t i = 1; i < r.pairs_selected; ++i) {
    EXPECT_LE(std::fabs(std::log(r.pairs[i - 1].strike / c.forward())),
              std::fabs(std::log(r.pairs[i].strike / c.forward())) + 1.0e-12);
  }
  for (std::size_t i = 0; i < r.pairs_selected; ++i) {
    EXPECT_LE(std::fabs(std::log(r.pairs[i].strike / c.forward())),
              0.75 * r.atm_total_vol + 0.1);
  }
}

TEST(ImpliedForward, ReportsForwardOffsetAgainstReferenceCarry) {
  const Chain c;
  auto market = c.market();
  market.reference_yield = c.yield + 0.01;
  const auto r = estimate_implied_forward(market, european_chain(c), european_parity());
  ASSERT_EQ(r.status, ImpliedForwardStatus::Ok) << r.reason;
  const double reference = c.spot * std::exp((c.rate - c.yield - 0.01) * c.years);
  EXPECT_NEAR(r.forward_offset, c.forward() - reference, 1.0e-9);
  EXPECT_GT(r.forward_offset, 0.0);
}

TEST(ImpliedForward, AmericanAlignmentRecoversForwardOfAndersenLakeChain) {
  Chain c;
  c.years = 2.0;
  c.rate = 0.05;
  c.yield = 0.01;
  c.sigma = 0.30;
  const auto pairs = american_chain(c);
  ImpliedForwardConfig cfg;
  cfg.al_opts = std::nullopt; // the generator's own (accurate) preset
  const auto r = estimate_implied_forward(c.market(), pairs, cfg);
  ASSERT_EQ(r.status, ImpliedForwardStatus::Ok) << r.reason;
  EXPECT_NEAR(r.forward / c.forward() - 1.0, 0.0, 2.0e-6);
  EXPECT_NEAR(r.implied_yield, c.yield, 2.0e-6);
  // Mid parity is biased LOW by the put's early-exercise premium; the
  // alignment removes it and reports how large it was.
  EXPECT_LT(r.parity_forward, c.forward() * (1.0 - 1.0e-3));
  EXPECT_GT(r.exercise_shift_sd, 0.0);
  for (std::size_t i = 0; i < r.pairs_selected; ++i) {
    const auto &p = r.pairs[i];
    if (p.status != ImpliedForwardPairStatus::Retained) {
      continue;
    }
    EXPECT_GT(p.exercise_shift, 0.0);
    EXPECT_NEAR(p.sigma, smile(c, p.strike), 1.0e-4);
    EXPECT_GE(p.iterations, 1u);
  }
  EXPECT_NEAR(r.atm_total_vol, c.sigma * std::sqrt(c.years), 0.02);
}

TEST(ImpliedForward, AmericanAlignmentDefaultFastPresetHoldsEconomicAccuracy) {
  Chain c;
  c.years = 1.5;
  c.rate = 0.045;
  c.yield = 0.0;
  const auto pairs = american_chain(c);
  const auto r = estimate_implied_forward(c.market(), pairs);
  ASSERT_EQ(r.status, ImpliedForwardStatus::Ok) << r.reason;
  EXPECT_NEAR(r.forward / c.forward() - 1.0, 0.0, 5.0e-5);
}

TEST(ImpliedForward, AmericanAlignmentHandlesCallsCarryingEarlyExercise) {
  // q > r: the CALL carries the premium, and parity is biased HIGH instead.
  Chain c;
  c.years = 1.0;
  c.rate = 0.01;
  c.yield = 0.08;
  const auto pairs = american_chain(c);
  ImpliedForwardConfig cfg;
  cfg.al_opts = std::nullopt;
  const auto r = estimate_implied_forward(c.market(), pairs, cfg);
  ASSERT_EQ(r.status, ImpliedForwardStatus::Ok) << r.reason;
  EXPECT_NEAR(r.forward / c.forward() - 1.0, 0.0, 2.0e-6);
  EXPECT_GT(r.parity_forward, r.forward);
  EXPECT_LT(r.exercise_shift_sd, 0.0);
}

TEST(ImpliedForward, ParityRefusesAmericanChainWhoseExerciseBiasIsMaterial) {
  Chain c;
  c.years = 2.0;
  c.rate = 0.05;
  c.yield = 0.0;
  ImpliedForwardConfig cfg;
  cfg.method = ImpliedForwardMethod::Parity;
  const auto r = estimate_implied_forward(c.market(), american_chain(c), cfg);
  EXPECT_EQ(r.status, ImpliedForwardStatus::Contaminated) << r.reason;
  EXPECT_GT(r.exercise_shift_sd, cfg.max_exercise_shift_sd);
  EXPECT_LT(r.forward, c.forward());
}

TEST(ImpliedForward, ParityAcceptsShortDatedAmericanChain) {
  Chain c;
  c.years = 0.02;
  c.rate = 0.05;
  c.yield = 0.0;
  c.half_spread = 0.01;
  ImpliedForwardConfig cfg;
  cfg.method = ImpliedForwardMethod::Parity;
  const auto r = estimate_implied_forward(c.market(), american_chain(c, 80.0, 120.0, 1.0), cfg);
  ASSERT_EQ(r.status, ImpliedForwardStatus::Ok) << r.reason;
  EXPECT_LE(std::fabs(r.exercise_shift_sd), cfg.max_exercise_shift_sd);
  EXPECT_NEAR(r.forward / c.forward() - 1.0, 0.0, 2.0e-4);
}

// Adding the same amount to both legs leaves a pair's parity forward intact
// but prices its out-of-the-money anchor leg above the American bound (call >
// spot, put > strike), so its early-exercise bias cannot be measured.
void inflate_both_legs(ImpliedForwardPair &pair, double amount) {
  pair.call_bid += amount;
  pair.call_ask += amount;
  pair.put_bid += amount;
  pair.put_ask += amount;
}

TEST(ImpliedForward, ParityExcludesAnAmericanPairWhoseBiasCannotBeMeasured) {
  Chain c;
  c.years = 0.02;
  c.rate = 0.05;
  c.yield = 0.0;
  c.half_spread = 0.01;
  ImpliedForwardConfig cfg;
  cfg.method = ImpliedForwardMethod::Parity;
  auto pairs = american_chain(c, 80.0, 120.0, 1.0);
  // Strike 101 is selected near the money but is not the straddle pair (100).
  const auto corrupted = std::find_if(pairs.begin(), pairs.end(),
                                      [](const auto &p) { return p.strike == 101.0; });
  ASSERT_NE(corrupted, pairs.end());
  inflate_both_legs(*corrupted, 100.0);
  const auto r = estimate_implied_forward(c.market(), pairs, cfg);
  ASSERT_EQ(r.status, ImpliedForwardStatus::Ok) << r.reason;
  // Before the fix this pair stayed Retained and active, unscreened.
  EXPECT_EQ(r.pairs_unidentified, 1u);
  bool found = false;
  for (std::size_t i = 0; i < r.pairs_selected; ++i) {
    if (r.pairs[i].strike != 101.0) {
      continue;
    }
    found = true;
    EXPECT_EQ(r.pairs[i].status, ImpliedForwardPairStatus::Unidentified);
    EXPECT_EQ(r.pairs[i].robust_weight, 0.0);
  }
  EXPECT_TRUE(found);
  EXPECT_GE(r.pairs_retained, cfg.min_pairs);
  EXPECT_LE(std::fabs(r.exercise_shift_sd), cfg.max_exercise_shift_sd);
  EXPECT_NEAR(r.forward / c.forward() - 1.0, 0.0, 2.0e-4);

  // When no selected pair can be measured, nothing is published.
  for (auto &pair : pairs) {
    if (pair.strike != 101.0) {
      inflate_both_legs(pair, 100.0);
    }
  }
  const auto none = estimate_implied_forward(c.market(), pairs, cfg);
  EXPECT_EQ(none.status, ImpliedForwardStatus::Contaminated) << none.reason;
  EXPECT_EQ(none.pairs_unidentified, none.pairs_selected);
  EXPECT_EQ(none.pairs_retained, 0u);
  EXPECT_TRUE(std::isnan(none.forward));
}

TEST(ImpliedForward, RefusesSparseExpiryWithTooFewTwoSidedPairs) {
  const Chain c;
  auto pairs = european_chain(c);
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if (i != 6 && i != 7) {
      pairs[i].put_bid = 0.0; // bidless: an upper bound, never a point quote
    }
  }
  const auto r = estimate_implied_forward(c.market(), pairs, european_parity());
  EXPECT_EQ(r.status, ImpliedForwardStatus::InsufficientPairs);
  EXPECT_EQ(r.pairs_valid, 2u);
  EXPECT_TRUE(std::isnan(r.forward));
  const auto empty = estimate_implied_forward(c.market(), {}, european_parity());
  EXPECT_EQ(empty.status, ImpliedForwardStatus::InsufficientPairs);
}

TEST(ImpliedForward, RefusesWhenPairForwardsDisagree) {
  Chain c;
  c.years = 0.25;
  auto pairs = european_chain(c, 80.0, 120.0, 2.5);
  // Alternate +-1.5% forward errors across strikes: no robust center exists.
  // Errors are added to the call (raising F) or the put (lowering F), so every
  // bid stays positive.
  const double shift = 0.015 * c.forward() * c.discount();
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    if (i % 2 == 0) {
      pairs[i].call_bid += shift;
      pairs[i].call_ask += shift;
    } else {
      pairs[i].put_bid += shift;
      pairs[i].put_ask += shift;
    }
  }
  const auto r = estimate_implied_forward(c.market(), pairs, european_parity());
  EXPECT_EQ(r.status, ImpliedForwardStatus::Dispersion) << r.reason;
  // Either the strip disperses, or the robust location split it into modes.
  EXPECT_TRUE(r.dispersion_sd > 0.05 ||
              static_cast<double>(r.pairs_outlier) > 0.34 * (r.pairs_outlier + r.pairs_retained));

  // A unimodal but noisy strip refuses on dispersion itself.
  auto noisy = european_chain(c, 80.0, 120.0, 2.5);
  const double offsets[] = {0.0, 0.012, -0.004, 0.009, -0.011, 0.006, -0.008, 0.003,
                            -0.010, 0.011, -0.002, 0.007, -0.006, 0.010, -0.012, 0.001, 0.004};
  for (std::size_t i = 0; i < noisy.size(); ++i) {
    const double move = 2.0 * offsets[i % 17] * c.forward() * c.discount();
    if (move >= 0.0) {
      noisy[i].call_bid += move;
      noisy[i].call_ask += move;
    } else {
      noisy[i].put_bid -= move;
      noisy[i].put_ask -= move;
    }
  }
  const auto n = estimate_implied_forward(c.market(), noisy, european_parity());
  EXPECT_EQ(n.status, ImpliedForwardStatus::Dispersion) << n.reason;
  EXPECT_GT(n.dispersion_sd, 0.05);
}

TEST(ImpliedForward, RobustLocationRejectsOneCorruptedPair) {
  const Chain c;
  auto pairs = european_chain(c, 80.0, 120.0, 2.5);
  // Corrupt the pair nearest the forward by a 3% forward error.
  std::size_t nearest = 0;
  for (std::size_t i = 1; i < pairs.size(); ++i) {
    if (std::fabs(pairs[i].strike - c.forward()) < std::fabs(pairs[nearest].strike - c.forward())) {
      nearest = i;
    }
  }
  const double shift = 0.03 * c.forward() * c.discount();
  pairs[nearest].call_bid += shift;
  pairs[nearest].call_ask += shift;
  const auto r = estimate_implied_forward(c.market(), pairs, european_parity());
  ASSERT_EQ(r.status, ImpliedForwardStatus::Ok) << r.reason;
  EXPECT_NEAR(r.forward / c.forward() - 1.0, 0.0, 1.0e-10);
  bool found = false;
  for (std::size_t i = 0; i < r.pairs_selected; ++i) {
    if (r.pairs[i].strike == pairs[nearest].strike) {
      found = true;
      EXPECT_EQ(r.pairs[i].status, ImpliedForwardPairStatus::Outlier);
      EXPECT_DOUBLE_EQ(r.pairs[i].robust_weight, 0.0);
    }
  }
  EXPECT_TRUE(found);
}

TEST(ImpliedForward, ExerciseBoundLegsAreExcludedAndReportedAsContamination) {
  // Every put sits at immediate intrinsic (exercised), so no pair carries
  // forward information: the estimator must refuse rather than guess.
  Chain c;
  c.spot = 100.0;
  std::vector<ImpliedForwardPair> pairs;
  for (double k = 110.0; k <= 150.0; k += 5.0) {
    const double call = black76_price(c.forward(), k, c.years, 0.25, c.discount(), Side::Call);
    const double intrinsic = k - c.spot;
    pairs.push_back({k, call - 0.01, call + 0.01, intrinsic, intrinsic + 1.0e-5});
  }
  const auto r = estimate_implied_forward(c.market(), pairs);
  EXPECT_EQ(r.status, ImpliedForwardStatus::Contaminated) << r.reason;
  EXPECT_EQ(r.pairs_exercise_bound, r.pairs_selected);
  EXPECT_EQ(r.pairs_retained, 0u);
  for (std::size_t i = 0; i < r.pairs_selected; ++i) {
    EXPECT_EQ(r.pairs[i].status, ImpliedForwardPairStatus::ExerciseBound);
  }
}

TEST(ImpliedForward, InvalidInputsAreTypedRefusals) {
  const Chain c;
  const auto good = european_chain(c);
  auto unsorted = good;
  std::swap(unsorted[2], unsorted[3]);
  EXPECT_EQ(estimate_implied_forward(c.market(), unsorted).status, ImpliedForwardStatus::Invalid);
  auto duplicate = good;
  duplicate[3].strike = duplicate[2].strike;
  EXPECT_EQ(estimate_implied_forward(c.market(), duplicate).status,
            ImpliedForwardStatus::Invalid);
  auto bad_market = c.market();
  bad_market.spot = 0.0;
  EXPECT_EQ(estimate_implied_forward(bad_market, good).status, ImpliedForwardStatus::Invalid);
  bad_market = c.market();
  bad_market.years = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(estimate_implied_forward(bad_market, good).status, ImpliedForwardStatus::Invalid);
  bad_market = c.market();
  bad_market.reference_yield = std::numeric_limits<double>::infinity();
  EXPECT_EQ(estimate_implied_forward(bad_market, good).status, ImpliedForwardStatus::Invalid);
  ImpliedForwardConfig cfg;
  cfg.max_pairs = 2;
  cfg.min_pairs = 3;
  EXPECT_EQ(estimate_implied_forward(c.market(), good, cfg).status,
            ImpliedForwardStatus::Invalid);
  cfg = {};
  cfg.max_pairs = kImpliedForwardMaxSelected + 1;
  EXPECT_EQ(estimate_implied_forward(c.market(), good, cfg).status,
            ImpliedForwardStatus::Invalid);
  cfg = {};
  cfg.band_sd = 0.0;
  const auto r = estimate_implied_forward(c.market(), good, cfg);
  EXPECT_EQ(r.status, ImpliedForwardStatus::Invalid);
  EXPECT_STREQ(to_string(r.status), "invalid");
}

TEST(ImpliedForward, NonFiniteQuotesAreSkippedNotFatal) {
  const Chain c;
  auto pairs = european_chain(c);
  pairs.front().call_ask = std::numeric_limits<double>::quiet_NaN();
  pairs.back().put_bid = -1.0;
  const auto r = estimate_implied_forward(c.market(), pairs, european_parity());
  ASSERT_EQ(r.status, ImpliedForwardStatus::Ok) << r.reason;
  EXPECT_EQ(r.pairs_valid, pairs.size() - 2);
}

TEST(ImpliedForward, IsBitwiseDeterministic) {
  Chain c;
  c.years = 1.0;
  const auto pairs = american_chain(c);
  const auto a = estimate_implied_forward(c.market(), pairs);
  const auto b = estimate_implied_forward(c.market(), pairs);
  ASSERT_EQ(a.status, b.status);
  EXPECT_EQ(std::memcmp(&a.forward, &b.forward, sizeof(double)), 0);
  EXPECT_EQ(std::memcmp(&a.parity_forward, &b.parity_forward, sizeof(double)), 0);
  EXPECT_EQ(std::memcmp(&a.dispersion, &b.dispersion, sizeof(double)), 0);
  for (std::size_t i = 0; i < a.pairs_selected; ++i) {
    EXPECT_EQ(std::memcmp(&a.pairs[i].forward, &b.pairs[i].forward, sizeof(double)), 0);
  }
}

TEST(ImpliedForward, StatusNamesAreStable) {
  EXPECT_STREQ(to_string(ImpliedForwardStatus::Ok), "ok");
  EXPECT_STREQ(to_string(ImpliedForwardStatus::InsufficientPairs), "insufficient_pairs");
  EXPECT_STREQ(to_string(ImpliedForwardStatus::Dispersion), "dispersion");
  EXPECT_STREQ(to_string(ImpliedForwardStatus::Contaminated), "contaminated");
  EXPECT_STREQ(to_string(ImpliedForwardPairStatus::ExerciseBound), "exercise_bound");
  EXPECT_STREQ(to_string(ImpliedForwardMethod::AmericanAlignment), "american_alignment");
}

} // namespace
} // namespace atx::vol
