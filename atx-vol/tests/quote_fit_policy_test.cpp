#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/vol/api/fitting/quote_fit_policy.hpp"

namespace {

using namespace atx::vol;

[[nodiscard]] QuoteSliceMarket market(double years = 0.25) {
  return {.spot = 100.0, .variance_years = years, .calendar_years = years};
}

[[nodiscard]] std::vector<QuoteSliceQuote> quotes(std::size_t strikes, double relative_spread) {
  std::vector<QuoteSliceQuote> result;
  for (std::size_t i = 0; i < strikes; ++i) {
    const double strike =
        95.0 + 10.0 * static_cast<double>(i) / std::max(1.0, static_cast<double>(strikes - 1u));
    for (const Side side : {Side::Call, Side::Put}) {
      result.push_back(
          {strike, side, 5.0 * (1.0 - 0.5 * relative_spread), 5.0 * (1.0 + 0.5 * relative_spread)});
    }
  }
  return result;
}

TEST(QuoteFitPolicy, RawFeaturesCountDistinctStrikesAndExcludeUnusableQuotes) {
  const std::vector<QuoteSliceQuote> input{
      {99.0, Side::Call, 4.0, 6.0},
      {99.0, Side::Put, 3.0, 5.0},
      {99.0, Side::Call, 4.0, 6.0},
      {101.0, Side::Call, 0.0, 1.0},
      {101.0, Side::Put, 1.0, 3.0},
      {200.0, Side::Call, 1.0, 2.0},
      {102.0, Side::Call, 3.0, 2.0},
      {103.0, Side::Put, 2.0, 2.0},
      {std::numeric_limits<double>::quiet_NaN(), Side::Call, 1.0, 2.0}};
  const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
  const auto features = quote_fit_liquidity_features(snapshots);
  ASSERT_TRUE(features.has_value());
  EXPECT_EQ(features->rows_seen, 9u);
  EXPECT_EQ(features->invalid_rows, 3u);
  EXPECT_EQ(features->unique_contracts, 5u);
  EXPECT_EQ(features->two_sided_contracts, 4u);
  EXPECT_EQ(features->near_money_two_sided_contracts, 3u);
  EXPECT_EQ(features->max_near_money_strikes, 2u);
  EXPECT_EQ(features->median_near_money_strikes, 2.0);
  EXPECT_EQ(features->bidless_contracts, 1u);
  EXPECT_DOUBLE_EQ(features->bidless_fraction, 0.2);
  EXPECT_DOUBLE_EQ(features->near_money_bidless_fraction, 0.25);
  ASSERT_TRUE(features->median_relative_spread.has_value());
  EXPECT_DOUBLE_EQ(*features->median_relative_spread, 0.5);
  EXPECT_EQ(features->classifier_inputs.n_atm_quotes, 3u);
  EXPECT_EQ(features->classifier_inputs.n_identifiable_expiries, 0u);
}

TEST(QuoteFitPolicy, DuplicateConsolidationIsConservativeAndOrderIndependent) {
  auto input = quotes(8, 0.1);
  input.push_back({input.front().strike, Side::Call, 0.0, 9.0});
  const std::array forward{QuoteFitExpirySnapshot{market(), input}};
  const auto first = quote_fit_liquidity_features(forward);
  ASSERT_TRUE(first.has_value());
  std::reverse(input.begin(), input.end());
  const std::array reverse{QuoteFitExpirySnapshot{market(), input}};
  const auto second = quote_fit_liquidity_features(reverse);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(first->unique_contracts, 16u);
  EXPECT_EQ(first->two_sided_contracts, 15u);
  EXPECT_EQ(first->bidless_contracts, 1u);
  EXPECT_EQ(first->max_near_money_strikes, 8u); // the paired put still identifies the strike
  EXPECT_EQ(first->median_relative_spread, second->median_relative_spread);
  EXPECT_EQ(first->two_sided_contracts, second->two_sided_contracts);
  EXPECT_EQ(first->bidless_fraction, second->bidless_fraction);
  EXPECT_EQ(first->classifier.kind, second->classifier.kind);
}

TEST(QuoteFitPolicy, DefaultRoutesDifferentiateDensityUncertaintyAndMissingBids) {
  const auto catalog = QuoteFitPolicyCatalog::create(default_quote_fit_profiles());
  ASSERT_TRUE(catalog.has_value()) << catalog.error().to_string();
  for (const auto &[count, spread, expected] :
       {std::tuple{32u, 0.02, "dense"}, std::tuple{12u, 0.20, "ordinary"},
        std::tuple{4u, 0.80, "sparse"}, std::tuple{32u, 0.90, "sparse"}}) {
    const auto input = quotes(count, spread);
    const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
    const auto resolved = catalog->resolve("UNSEEDED", snapshots);
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->profile->name, expected);
    EXPECT_EQ(resolved->source, QuoteFitPolicySource::RawLiquidity);
    EXPECT_TRUE(resolved->profile->config.curve.parametric.validate_no_arb);
    EXPECT_TRUE(resolved->profile->config.curve.parametric.residual_disable);
  }
  auto input = quotes(32, 0.02);
  for (std::size_t i = 0; i < 40u; ++i) {
    input[i].bid = 0.0;
  }
  const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
  const auto resolved = catalog->resolve("UNSEEDED", snapshots);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->profile->name, "sparse");
  EXPECT_GT(resolved->features.near_money_bidless_fraction, 0.5);
}

TEST(QuoteFitPolicy, TickerSpellingAndBidlessRemoteWingsDoNotCreatePriors) {
  const auto catalog = QuoteFitPolicyCatalog::create(default_quote_fit_profiles());
  ASSERT_TRUE(catalog.has_value());
  auto input = quotes(32, 0.02);
  for (int i = 0; i < 200; ++i) {
    input.push_back({200.0 + i, Side::Call, 0.0, 0.01});
  }
  const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
  const auto seeded_name = catalog->resolve("SPY", snapshots);
  const auto unknown_name = catalog->resolve("NO_PRIOR", snapshots);
  ASSERT_TRUE(seeded_name.has_value());
  ASSERT_TRUE(unknown_name.has_value());
  EXPECT_EQ(seeded_name->profile, unknown_name->profile);
  EXPECT_EQ(unknown_name->profile->name, "dense");
  EXPECT_GT(unknown_name->features.bidless_fraction, 0.5);
  EXPECT_EQ(unknown_name->features.near_money_bidless_fraction, 0.0);
}

TEST(QuoteFitPolicy, CountsIdentifiabilityPerExpiryInsteadOfPoolingStrikes) {
  const auto input = quotes(2, 0.02);
  const std::array snapshots{
      QuoteFitExpirySnapshot{market(0.01), input}, QuoteFitExpirySnapshot{market(0.02), input},
      QuoteFitExpirySnapshot{market(0.03), input}, QuoteFitExpirySnapshot{market(0.04), input}};
  const auto catalog = QuoteFitPolicyCatalog::create(default_quote_fit_profiles());
  ASSERT_TRUE(catalog.has_value());
  const auto resolved = catalog->resolve("THIN", snapshots);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->features.classifier_inputs.n_live_expiries, 4u);
  EXPECT_EQ(resolved->features.classifier_inputs.n_identifiable_expiries, 0u);
  EXPECT_EQ(resolved->features.classifier_inputs.n_front_expiries, 2u);
  EXPECT_EQ(resolved->features.max_near_money_strikes, 2u);
  EXPECT_EQ(resolved->profile->name, "sparse");
}

TEST(QuoteFitPolicy, ExactSymbolOverrideWinsButKeepsMandatoryPointAdmission) {
  const std::array overrides{QuoteFitSymbolOverride{"FORCE", "ordinary"}};
  const auto catalog = QuoteFitPolicyCatalog::create(default_quote_fit_profiles(), overrides);
  ASSERT_TRUE(catalog.has_value());
  const auto input = quotes(2, 0.02);
  const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
  const auto explicit_policy = catalog->resolve("FORCE", snapshots);
  const auto different_case = catalog->resolve("force", snapshots);
  ASSERT_TRUE(explicit_policy.has_value());
  ASSERT_TRUE(different_case.has_value());
  EXPECT_EQ(explicit_policy->profile->name, "ordinary");
  EXPECT_EQ(explicit_policy->source, QuoteFitPolicySource::ExactSymbolOverride);
  EXPECT_EQ(different_case->profile->name, "sparse");
  EXPECT_EQ(explicit_policy->profile->config.min_quotes, 6u);
  const auto fit = fit_quote_slice(market(), input, explicit_policy->profile->config);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  EXPECT_EQ(fit->fit_status, QuoteSliceFitStatus::InsufficientQuotes);
  EXPECT_EQ(fit->curve, nullptr);
}

TEST(QuoteFitPolicy, ResolvedCompleteConfigurationOwnsAllBorrowedInputStorage) {
  std::optional<ResolvedQuoteFitPolicy> resolved;
  {
    auto profiles = default_quote_fit_profiles();
    std::vector<double> grid{-2.0, -1.0, 0.0, 1.0, 2.0};
    QuoteFitProfile custom{"custom", ProfileKind::LiquidSingleName, {}};
    custom.config.curve.kind = VolCurveKind::SplineVol;
    custom.config.curve.spline.grid = grid;
    custom.config.curve.spline.lambda = 0.123;
    custom.config.curve.convex_repair = ConvexRepairSpec{};
    custom.config.curve.convex_repair->extra_node_ks = {-0.1, 0.2};
    custom.config.min_quotes = 7u;
    custom.config.side_policy = QuoteSliceSidePolicy::All;
    custom.config.dividend_model = QuoteSliceDividendModel::DatedCash;
    custom.config.spread_floor = 0.05;
    custom.config.al_opts = al_fast_opts();
    profiles.push_back(custom);
    std::vector<QuoteFitSymbolOverride> overrides{{"OWNED", "custom"}};
    auto catalog = QuoteFitPolicyCatalog::create(profiles, overrides);
    ASSERT_TRUE(catalog.has_value()) << catalog.error().to_string();
    grid.assign(grid.size(), 99.0);
    overrides.front().profile = "sparse";
    profiles.back().config.min_quotes = 999u;
    auto copied_catalog = *catalog;
    auto moved_catalog = std::move(copied_catalog);
    auto result = moved_catalog.resolve("OWNED", {});
    ASSERT_TRUE(result.has_value());
    resolved = std::move(*result);
  }
  ASSERT_TRUE(resolved.has_value());
  const auto copy = *resolved;
  resolved.reset();
  EXPECT_EQ(copy.profile->name, "custom");
  EXPECT_EQ(copy.profile->config.min_quotes, 7u);
  EXPECT_EQ(copy.profile->config.side_policy, QuoteSliceSidePolicy::All);
  EXPECT_EQ(copy.profile->config.dividend_model, QuoteSliceDividendModel::DatedCash);
  EXPECT_EQ(copy.profile->config.spread_floor, 0.05);
  ASSERT_TRUE(copy.profile->config.al_opts.has_value());
  EXPECT_EQ(copy.profile->config.al_opts->n_collocation, al_fast_opts().n_collocation);
  EXPECT_EQ(copy.profile->config.curve.spline.lambda, 0.123);
  ASSERT_EQ(copy.profile->config.curve.spline.grid.size(), 5u);
  EXPECT_EQ(copy.profile->config.curve.spline.grid.front(), -2.0);
  EXPECT_EQ(copy.profile->config.curve.spline.grid.back(), 2.0);
  EXPECT_EQ(copy.profile->config.curve.convex_repair->extra_node_ks.front(), -0.1);
}

TEST(QuoteFitPolicy, RejectsDuplicateNamesUnknownReferencesAndInvalidThresholds) {
  auto profiles = default_quote_fit_profiles();
  profiles.push_back(profiles.front());
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(profiles).has_value());
  profiles = default_quote_fit_profiles();
  const std::array unknown{QuoteFitSymbolOverride{"A", "DENSE"}};
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(profiles, unknown).has_value());
  const std::array duplicate{QuoteFitSymbolOverride{"A", "dense"},
                             QuoteFitSymbolOverride{"A", "ordinary"}};
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(profiles, duplicate).has_value());
  for (const double bad :
       {-1.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
    QuoteFitRegimeConfig rules;
    rules.dense_max_median_relative_spread = bad;
    EXPECT_FALSE(QuoteFitPolicyCatalog::create(profiles, {}, rules).has_value());
    rules = {};
    rules.sparse_min_bidless_fraction = bad;
    EXPECT_FALSE(QuoteFitPolicyCatalog::create(profiles, {}, rules).has_value());
  }
  QuoteFitRegimeConfig unknown_default;
  unknown_default.sparse_profile = "absent";
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(profiles, {}, unknown_default).has_value());
}

TEST(QuoteFitPolicy, RejectsUnsafeOrUnderidentifiedCustomProfiles) {
  auto profiles = default_quote_fit_profiles();
  profiles.back().config.curve.kind = VolCurveKind::Svi;
  profiles.back().config.min_quotes = 2u;
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(profiles).has_value());
  profiles = default_quote_fit_profiles();
  profiles[1].config.curve.parametric.validate_no_arb = false;
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(profiles).has_value());
  profiles = default_quote_fit_profiles();
  profiles.front().config.curve.convex.bound_slope_below = false;
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(profiles).has_value());
  profiles = default_quote_fit_profiles();
  const std::array bad_grid{-1.0, 0.0, 0.0, 1.0};
  profiles[1].config.curve.spline.grid = bad_grid;
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(profiles).has_value());
}

TEST(QuoteFitPolicy, RejectsInvalidSnapshotClocksAndDuplicateExpiries) {
  const auto input = quotes(8, 0.1);
  std::array snapshots{QuoteFitExpirySnapshot{market(), input},
                       QuoteFitExpirySnapshot{market(), input}};
  EXPECT_FALSE(quote_fit_liquidity_features(snapshots).has_value());
  snapshots[1].market.calendar_years = 0.5;
  snapshots[1].market.spot = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(quote_fit_liquidity_features(snapshots).has_value());
  const auto empty = quote_fit_liquidity_features({});
  ASSERT_TRUE(empty.has_value());
  EXPECT_FALSE(empty->median_relative_spread.has_value());
  EXPECT_EQ(empty->classifier.kind, ProfileKind::IlliquidSmallCap);
}


TEST(QuoteFitPolicy, ChannelProfilesSkipPointMinimumsButValidateChannelControls) {
  auto profiles = default_quote_fit_profiles();
  // A knot-spline sparse profile keeps min_quotes 4 (it only grades point evidence).
  profiles.back().config.method = QuoteSliceFitMethod::ChannelSpline;
  profiles.back().config.curve.kind = VolCurveKind::SplineVol;
  profiles.back().config.channel.repair_half_spreads = 0.5;
  EXPECT_TRUE(QuoteFitPolicyCatalog::create(profiles).has_value());
  auto bad = profiles;
  bad.back().config.channel.prior_scale = 0.0;
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(bad).has_value());
  bad = profiles;
  bad.back().config.channel.mult_ceil = bad.back().config.channel.mult_floor;
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(bad).has_value());
  bad = profiles;
  bad.back().config.channel.min_channel_quotes = 0u;
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(bad).has_value());
  bad = profiles;
  bad.back().config.method = static_cast<QuoteSliceFitMethod>(9);
  EXPECT_FALSE(QuoteFitPolicyCatalog::create(bad).has_value());
}

} // namespace
