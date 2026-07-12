#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/vol/arb.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/curve_fit.hpp"
#include "atx/vol/essvi_calib.hpp"
#include "atx/vol/prepared_fitting.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/vol_curve.hpp"

namespace {

using atx::vol::black76_price;
using atx::vol::CalibOpts;
using atx::vol::Chain;
using atx::vol::chain_index;
using atx::vol::CurveConfig;
using atx::vol::ObservationKey;
using atx::vol::ObservationRejectionReason;
using atx::vol::PreparedBoard;
using atx::vol::PreparedObservationPolicy;
using atx::vol::PreparedSlice;
using atx::vol::PreparedSliceInputs;
using atx::vol::QuoteFlag;
using atx::vol::Side;
using atx::vol::SurfaceParityInputs;
using atx::vol::Underlying;
using atx::vol::VolCurveKind;

constexpr double kS = 100.0;
constexpr double kF = 100.0;
constexpr double kT = 0.5;
constexpr double kR = 0.03;
constexpr double kDf = 0.9851119396030626;

[[nodiscard]] Chain make_chain() {
  Chain chain;
  chain.uid = 17u;
  chain.expiry_id = 42u;
  chain.T = kT;
  chain.strikes = {80.0, 85.0, 90.0, 95.0, 100.0, 105.0, 110.0, 115.0};
  const std::size_t n_sides = 2u * chain.strikes.size();
  chain.bids.assign(n_sides, 0.0);
  chain.asks.assign(n_sides, 0.0);
  chain.mids.assign(n_sides, 0.0);
  chain.ivs.assign(n_sides, std::numeric_limits<double>::quiet_NaN());
  chain.bid_sizes.assign(n_sides, 1u);
  chain.ask_sizes.assign(n_sides, 1u);
  chain.ts_ns.assign(n_sides, 0u);
  chain.flags.assign(n_sides, 0u);
  for (std::size_t strike_index = 0; strike_index < chain.strikes.size(); ++strike_index) {
    for (std::uint8_t side_value = 0u; side_value < 2u; ++side_value) {
      const Side side = static_cast<Side>(side_value);
      const std::size_t quote_index = chain_index(static_cast<std::uint16_t>(strike_index), side);
      const double mid = black76_price(kF, chain.strikes[strike_index], kT, 0.24, kDf, side);
      chain.mids[quote_index] = mid;
      chain.bids[quote_index] = mid - 0.02;
      chain.asks[quote_index] = mid + 0.02;
    }
  }

  const std::size_t stale = chain_index(1u, Side::Put);
  chain.flags[stale] = static_cast<std::uint8_t>(QuoteFlag::Stale);
  const std::size_t crossed = chain_index(6u, Side::Call);
  chain.bids[crossed] = chain.asks[crossed];
  return chain;
}

[[nodiscard]] PreparedSliceInputs configured_inputs() {
  PreparedSliceInputs inputs;
  inputs.expiry_index = 3u;
  inputs.S = kS;
  inputs.r = kR;
  inputs.F = kF;
  inputs.q_eff = kR;
  inputs.df = kDf;
  inputs.calib = CalibOpts{};
  inputs.policy = PreparedObservationPolicy::Configured;
  return inputs;
}

[[nodiscard]] std::vector<ObservationKey> accepted_keys(const PreparedSlice &slice) {
  std::vector<ObservationKey> keys;
  for (const auto &observation : slice.observations()) {
    if (observation.accepted()) {
      keys.push_back(observation.key);
    }
  }
  return keys;
}

} // namespace

TEST(PreparedFitting, ConfiguredPopulationIsDeterministicAndFamilyAgnostic) {
  const Chain chain = make_chain();
  const auto first = PreparedSlice::create(chain, configured_inputs());
  const auto second = PreparedSlice::create(chain, configured_inputs());
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  ASSERT_TRUE(second.has_value()) << second.error().to_string();

  const std::vector<ObservationKey> essvi_keys = accepted_keys(*first);
  EXPECT_TRUE(std::is_sorted(essvi_keys.begin(), essvi_keys.end()));
  EXPECT_TRUE(std::equal(first->rejections().begin(), first->rejections().end(),
                         second->rejections().begin(), second->rejections().end()));
  ASSERT_EQ(first->fit_observations().size(), essvi_keys.size());

  atx::vol::FitDiag essvi_diag;
  const auto essvi =
      atx::vol::essvi_fit_slice(first->fit_observations(), kT, kF, CalibOpts{}, &essvi_diag);
  ASSERT_TRUE(essvi.has_value()) << essvi.error().to_string();
  const std::vector<ObservationKey> after_essvi = accepted_keys(*first);

  CurveConfig generic;
  generic.kind = VolCurveKind::LinearVariance;
  const auto generic_fit =
      atx::vol::fit_slice_curve(generic, first->fit_observations(), kF, kT, kDf);
  ASSERT_TRUE(generic_fit.has_value()) << generic_fit.error().to_string();
  const std::vector<ObservationKey> after_generic = accepted_keys(*first);

  EXPECT_EQ(after_essvi, essvi_keys);
  EXPECT_EQ(after_generic, essvi_keys);
  EXPECT_EQ(accepted_keys(*second), essvi_keys);
}

TEST(PreparedFitting, ConfiguredPopulationRetainsStableRejectedKeysAndReasons) {
  const Chain chain = make_chain();
  const auto prepared = PreparedSlice::create(chain, configured_inputs());
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();

  const auto &rejections = prepared->rejections();
  ASSERT_EQ(rejections.size(), 2u);
  EXPECT_EQ(rejections[0].key, (ObservationKey{3u, 1u, Side::Put}));
  EXPECT_EQ(rejections[0].reason, ObservationRejectionReason::QuoteFlag);
  EXPECT_EQ(rejections[1].key, (ObservationKey{3u, 6u, Side::Call}));
  EXPECT_EQ(rejections[1].reason, ObservationRejectionReason::InvalidBidAsk);
}

TEST(PreparedFitting, LegacyCompatibilityPreservesHistoricalPermissiveRowsAndWeights) {
  const Chain chain = make_chain();
  PreparedSliceInputs inputs = configured_inputs();
  inputs.policy = PreparedObservationPolicy::LegacyEssviCompatibility;
  const auto prepared = PreparedSlice::create(chain, inputs);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();

  // Historical eSSVI ignored quote flags and allowed locked bid==ask rows. The
  // explicit compatibility policy retains both; Configured rejects both.
  ASSERT_EQ(prepared->fit_observations().size(), chain.n_strikes());
  EXPECT_TRUE(prepared->rejections().empty());
  EXPECT_EQ(prepared->provenance().policy, PreparedObservationPolicy::LegacyEssviCompatibility);
  EXPECT_DOUBLE_EQ(prepared->provenance().S, kS);
  EXPECT_DOUBLE_EQ(prepared->provenance().q_eff, kR);

  const atx::vol::FitObs &row = prepared->fit_observations().front();
  const double spread =
      chain.asks[chain_index(0u, Side::Put)] - chain.bids[chain_index(0u, Side::Put)];
  const double vega =
      atx::vol::black76_value_and_vega(kF, chain.strikes.front(), kT, row.sigma_mkt, kDf, Side::Put)
          .vega;
  const double two_sigma_t = 2.0 * row.sigma_mkt * kT;
  const double expected_weight = (vega * vega) / (spread * spread * two_sigma_t * two_sigma_t);
  EXPECT_DOUBLE_EQ(row.weight_w, expected_weight);
  EXPECT_DOUBLE_EQ(row.active_weight_w, expected_weight);
}

TEST(PreparedFitting, ConfiguredCapAppliesIdenticallyToFitKeysAndScoringRows) {
  const Chain chain = make_chain();
  PreparedSliceInputs inputs = configured_inputs();
  inputs.calib.max_obs_per_slice = 5u;
  const auto prepared = PreparedSlice::create(chain, inputs);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();

  const std::vector<ObservationKey> keys = accepted_keys(*prepared);
  ASSERT_EQ(keys.size(), 5u);
  ASSERT_EQ(prepared->fit_observations().size(), keys.size());
  ASSERT_EQ(prepared->score_keys().size(), keys.size());
  ASSERT_EQ(prepared->score_columns().strike.size(), keys.size());
  for (std::size_t index = 0; index < keys.size(); ++index) {
    EXPECT_EQ(prepared->score_keys()[index], keys[index]);
    EXPECT_EQ(prepared->fit_observations()[index].source_strike_index, keys[index].strike_index);
  }
  EXPECT_EQ(std::count_if(prepared->rejections().begin(), prepared->rejections().end(),
                          [](const atx::vol::ObservationRejection &rejection) {
                            return rejection.reason == ObservationRejectionReason::ObservationCap;
                          }),
            1);
}

TEST(PreparedFitting, DuplicateStrikesRetainDistinctSourceKeysThroughCap) {
  Chain chain = make_chain();
  chain.strikes[2] = chain.strikes[1];
  for (std::uint8_t side_value = 0u; side_value < 2u; ++side_value) {
    const Side side = static_cast<Side>(side_value);
    const std::size_t quote_index = chain_index(2u, side);
    const double mid = black76_price(kF, chain.strikes[2], kT, 0.24, kDf, side);
    chain.mids[quote_index] = mid;
    chain.bids[quote_index] = mid - 0.02;
    chain.asks[quote_index] = mid + 0.02;
  }
  chain.flags[chain_index(1u, Side::Put)] = 0u;
  PreparedSliceInputs inputs = configured_inputs();
  inputs.calib.max_obs_per_slice = 5u;
  const auto prepared = PreparedSlice::create(chain, inputs);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();

  ASSERT_EQ(prepared->fit_observations().size(), 5u);
  ASSERT_EQ(prepared->score_keys().size(), 5u);
  for (std::size_t index = 0; index < prepared->fit_observations().size(); ++index) {
    EXPECT_EQ(prepared->fit_observations()[index].source_strike_index,
              prepared->score_keys()[index].strike_index);
  }
  const auto first_duplicate =
      std::find_if(prepared->observations().begin(), prepared->observations().end(),
                   [](const auto &row) { return row.key == ObservationKey{3u, 1u, Side::Put}; });
  const auto second_duplicate =
      std::find_if(prepared->observations().begin(), prepared->observations().end(),
                   [](const auto &row) { return row.key == ObservationKey{3u, 2u, Side::Put}; });
  EXPECT_NE(first_duplicate, prepared->observations().end());
  EXPECT_NE(second_duplicate, prepared->observations().end());
}

TEST(PreparedFitting, AllEqualStrikesFillCapByDeterministicSourceKeyFallback) {
  Chain chain = make_chain();
  for (std::size_t strike_index = 0; strike_index < chain.n_strikes(); ++strike_index) {
    chain.strikes[strike_index] = kF;
    for (std::uint8_t side_value = 0u; side_value < 2u; ++side_value) {
      const Side side = static_cast<Side>(side_value);
      const std::size_t quote_index = chain_index(static_cast<std::uint16_t>(strike_index), side);
      const double mid = black76_price(kF, kF, kT, 0.24, kDf, side);
      chain.mids[quote_index] = mid;
      chain.bids[quote_index] = mid - 0.02;
      chain.asks[quote_index] = mid + 0.02;
      chain.flags[quote_index] = 0u;
    }
  }

  PreparedSliceInputs inputs = configured_inputs();
  inputs.calib.max_obs_per_slice = 5u;
  const auto prepared = PreparedSlice::create(chain, inputs);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  ASSERT_EQ(prepared->score_keys().size(), 5u);

  const std::vector<ObservationKey> expected{
      ObservationKey{3u, 0u, Side::Call}, ObservationKey{3u, 1u, Side::Call},
      ObservationKey{3u, 2u, Side::Call}, ObservationKey{3u, 3u, Side::Call},
      ObservationKey{3u, 7u, Side::Call}};
  EXPECT_TRUE(std::equal(prepared->score_keys().begin(), prepared->score_keys().end(),
                         expected.begin(), expected.end()));
}

TEST(PreparedFitting, BidOrAskAnchorScoringMarketIvStillComesFromRawMid) {
  const Chain chain = make_chain();
  for (const auto anchor : {atx::vol::CalibAnchorKind::Bid, atx::vol::CalibAnchorKind::Ask}) {
    PreparedSliceInputs inputs = configured_inputs();
    inputs.calib.anchor_kind = anchor;
    const auto prepared = PreparedSlice::create(chain, inputs);
    ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();

    ASSERT_FALSE(prepared->score_keys().empty());
    const ObservationKey key = prepared->score_keys().front();
    const std::size_t quote_index =
        chain_index(static_cast<std::uint16_t>(key.strike_index), key.side);
    const auto expected = atx::vol::european_equiv_iv(
        chain.mids[quote_index], kS, chain.strikes[key.strike_index], kT, kR, kR, key.side,
        inputs.method, inputs.al_opts, nullptr, inputs.iv_tolerance, inputs.iv_max_iterations);
    ASSERT_TRUE(expected.has_value()) << expected.error().to_string();
    EXPECT_DOUBLE_EQ(prepared->score_columns().market_iv.front(), *expected);
    EXPECT_NE(prepared->fit_observations().front().sigma_mkt,
              prepared->score_columns().market_iv.front());
  }
}

TEST(PreparedFitting, ScoringOptOutSkipsIndependentRawMidInversions) {
  const Chain chain = make_chain();
  PreparedSliceInputs without_scoring = configured_inputs();
  without_scoring.calib.anchor_kind = atx::vol::CalibAnchorKind::Bid;
  without_scoring.prepare_scoring = false;
  const auto skipped = PreparedSlice::create(chain, without_scoring);
  ASSERT_TRUE(skipped.has_value()) << skipped.error().to_string();
  EXPECT_TRUE(skipped->score_keys().empty());
  EXPECT_EQ(skipped->provenance().n_score_inversions, 0u);

  PreparedSliceInputs with_scoring = without_scoring;
  with_scoring.prepare_scoring = true;
  const auto prepared = PreparedSlice::create(chain, with_scoring);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  EXPECT_EQ(prepared->score_keys().size(), prepared->fit_observations().size());
  EXPECT_EQ(prepared->provenance().n_score_inversions, prepared->fit_observations().size());
}

TEST(PreparedFitting, CurveFitParityOptOutReportsZeroScoreInversions) {
  Underlying underlying;
  underlying.uid = 17u;
  underlying.ticker = "SCORE_INTENT";
  underlying.spot = kS;
  underlying.chains.push_back(make_chain());

  SurfaceParityInputs inputs;
  inputs.S = kS;
  inputs.r = kR;
  inputs.deam.imply_borrow = false;
  inputs.deam.borrow_fixed = kR;
  inputs.calib.anchor_kind = atx::vol::CalibAnchorKind::Bid;
  inputs.score_parity = false;
  CurveConfig curve;
  curve.kind = VolCurveKind::LinearVariance;
  const auto skipped = atx::vol::fit_curve_surface(underlying, inputs, curve);
  ASSERT_TRUE(skipped.has_value()) << skipped.error().to_string();
  EXPECT_EQ(skipped->n_score_inversions, 0u);

  inputs.score_parity = true;
  const auto prepared = atx::vol::fit_curve_surface(underlying, inputs, curve);
  ASSERT_TRUE(prepared.has_value()) << prepared.error().to_string();
  EXPECT_GT(prepared->n_score_inversions, 0u);
}

TEST(PreparedFitting, InconsistentCarryOrDiscountIsRejected) {
  const Chain chain = make_chain();
  PreparedSliceInputs bad_carry = configured_inputs();
  bad_carry.q_eff += 0.01;
  EXPECT_FALSE(PreparedSlice::create(chain, bad_carry).has_value());

  PreparedSliceInputs bad_discount = configured_inputs();
  bad_discount.df *= 0.99;
  EXPECT_FALSE(PreparedSlice::create(chain, bad_discount).has_value());

  PreparedSliceInputs nonfinite = configured_inputs();
  nonfinite.F = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(PreparedSlice::create(chain, nonfinite).has_value());
}

TEST(PreparedFitting, LegacyCompatibilityRemainsWiredThroughEssviSurfaceEndToEnd) {
  Underlying underlying;
  underlying.uid = 17u;
  underlying.ticker = "LEGACY";
  underlying.spot = kS;
  underlying.chains.push_back(make_chain());

  SurfaceParityInputs inputs;
  inputs.S = kS;
  inputs.r = kR;
  inputs.deam.imply_borrow = false;
  inputs.deam.borrow_fixed = kR;
  const auto report = atx::vol::run_surface_parity(underlying, inputs);
  ASSERT_TRUE(report.has_value()) << report.error().to_string();
  ASSERT_EQ(report->n_slices, 1u);
  ASSERT_EQ(report->context.size(), 1u);
  EXPECT_EQ(report->context.front().n_used, underlying.chains.front().n_strikes());
  // Golden from the historical permissive eSSVI route on this deliberately
  // flagged/locked board. This pins the compatibility seam end to end.
  EXPECT_NEAR(report->surface.iv_on_slice(0u, 0.0), 0.35315599514657198, 1.0e-12);
}

TEST(PreparedFitting, BoardCanonicalizesSliceOrderAndRejectsDuplicateExpiryKeys) {
  const Chain chain = make_chain();
  PreparedSliceInputs later_inputs = configured_inputs();
  later_inputs.expiry_index = 9u;
  PreparedSliceInputs earlier_inputs = configured_inputs();
  earlier_inputs.expiry_index = 2u;
  auto later = PreparedSlice::create(chain, later_inputs);
  auto earlier = PreparedSlice::create(chain, earlier_inputs);
  ASSERT_TRUE(later.has_value());
  ASSERT_TRUE(earlier.has_value());

  std::vector<PreparedSlice> slices;
  slices.push_back(std::move(*later));
  slices.push_back(std::move(*earlier));
  auto board = PreparedBoard::create(std::move(slices));
  ASSERT_TRUE(board.has_value()) << board.error().to_string();
  ASSERT_EQ(board->slices().size(), 2u);
  EXPECT_EQ(board->slices()[0].expiry_index(), 2u);
  EXPECT_EQ(board->slices()[1].expiry_index(), 9u);

  auto duplicate_a = PreparedSlice::create(chain, configured_inputs());
  auto duplicate_b = PreparedSlice::create(chain, configured_inputs());
  ASSERT_TRUE(duplicate_a.has_value());
  ASSERT_TRUE(duplicate_b.has_value());
  std::vector<PreparedSlice> duplicates;
  duplicates.push_back(std::move(*duplicate_a));
  duplicates.push_back(std::move(*duplicate_b));
  EXPECT_FALSE(PreparedBoard::create(std::move(duplicates)).has_value());
}
