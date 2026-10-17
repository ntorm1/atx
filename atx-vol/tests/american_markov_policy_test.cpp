#include "atx/vol/api/fitting/american_markov_policy.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

QuoteSliceMarket market(double years = 0.25) {
  return {.spot = 100.0, .variance_years = years, .calendar_years = years};
}

std::vector<QuoteSliceQuote> quotes(std::size_t count, double spread = 0.1) {
  std::vector<QuoteSliceQuote> out;
  for (std::size_t i = 0; i < count; ++i) {
    const double strike = 96.0 + 8.0 * static_cast<double>(i) / static_cast<double>(count);
    for (const auto side : {Side::Call, Side::Put}) {
      out.push_back({strike, side, 5.0 * (1.0 - 0.5 * spread), 5.0 * (1.0 + 0.5 * spread)});
    }
  }
  return out;
}

const std::array schedule{AmericanMarkovFitInterval{0.25, 0.25, 0.03, 0.01, 0.0}};

TEST(AmericanMarkovPolicy, RawRoutingReusesExistingClassifierAndExposesNumericConfiguration) {
  const auto profiles = default_american_markov_policy_profiles();
  const auto catalog = AmericanMarkovPolicyCatalog::create(profiles);
  ASSERT_TRUE(catalog) << catalog.error().to_string();
  for (const auto &[count, spread, expected, parameters] :
       {std::tuple{32U, 0.02, "dense", 5U}, {8U, 0.2, "ordinary", 3U},
        {2U, 0.02, "sparse", 1U}, {32U, 1.0, "sparse", 1U}}) {
    const auto input = quotes(count, spread);
    const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
    const auto resolved = catalog->resolve("UNSEEDED", snapshots, schedule);
    ASSERT_TRUE(resolved) << resolved.error().to_string();
    EXPECT_EQ(resolved->profile->name, expected);
    EXPECT_EQ(resolved->source, QuoteFitPolicySource::RawLiquidity);
    EXPECT_EQ(resolved->parameter_count, parameters);
    EXPECT_EQ(resolved->time_groups, 1U);
    const auto old_catalog = QuoteFitPolicyCatalog::create(default_quote_fit_profiles());
    ASSERT_TRUE(old_catalog);
    const auto old = old_catalog->resolve("UNSEEDED", snapshots);
    ASSERT_TRUE(old);
    EXPECT_EQ(old->profile->name, resolved->profile->name);
    EXPECT_EQ(old->features.near_money_contracts, resolved->features.near_money_contracts);
    EXPECT_GT(resolved->profile->fit_config.prior_weight, 0.0);
    EXPECT_GT(resolved->profile->model_config.max_stored_rows, 0U);
    EXPECT_GT(resolved->profile->grid_config.concentration_scale, 0.0);
  }
  EXPECT_GT(profiles[2].fit_config.prior_weight, profiles[1].fit_config.prior_weight);
  EXPECT_GT(profiles[1].fit_config.prior_weight, profiles[0].fit_config.prior_weight);
}

TEST(AmericanMarkovPolicy, ExactOverridesAreCaseSensitiveAndNeverChangeRawFeatures) {
  const std::array overrides{QuoteFitSymbolOverride{"FORCE", "dense"}};
  const auto catalog = AmericanMarkovPolicyCatalog::create(
      default_american_markov_policy_profiles(), overrides);
  ASSERT_TRUE(catalog);
  const auto input = quotes(2);
  const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
  const auto forced = catalog->resolve("FORCE", snapshots, schedule);
  const auto lower = catalog->resolve("force", snapshots, schedule);
  const auto ticker = catalog->resolve("SPY", snapshots, schedule);
  ASSERT_TRUE(forced);
  ASSERT_TRUE(lower);
  ASSERT_TRUE(ticker);
  EXPECT_EQ(forced->profile->name, "dense");
  EXPECT_EQ(forced->source, QuoteFitPolicySource::ExactSymbolOverride);
  EXPECT_EQ(lower->profile->name, "sparse");
  EXPECT_EQ(ticker->profile, lower->profile);
  EXPECT_EQ(forced->features.unique_contracts, lower->features.unique_contracts);
}

TEST(AmericanMarkovPolicy, ResolvedCopiesOwnKnotsInitialAndPriorAfterAllInputsAreDestroyed) {
  std::optional<ResolvedAmericanMarkovPolicy> result;
  {
    auto profiles = default_american_markov_policy_profiles();
    std::vector<double> knots{-0.3, 0.0, 0.4};
    std::vector<double> initial{0.30, 0.25, 0.23};
    std::vector<double> prior{0.28, 0.24, 0.22};
    profiles[1].fit_config.log_state_knots = knots;
    profiles[1].fit_config.initial_sigmas = initial;
    profiles[1].fit_config.prior_sigmas = prior;
    profiles[1].fit_config.prior_weight = 0.123;
    const std::array overrides{QuoteFitSymbolOverride{"OWNED", "ordinary"}};
    auto catalog = AmericanMarkovPolicyCatalog::create(profiles, overrides);
    ASSERT_TRUE(catalog);
    knots.assign(3, 99.0);
    initial.assign(3, 99.0);
    prior.assign(3, 99.0);
    profiles[1].fit_config.prior_weight = 999.0;
    auto copied_catalog = *catalog;
    auto moved_catalog = std::move(copied_catalog);
    const auto input = quotes(8);
    const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
    auto resolved = moved_catalog.resolve("OWNED", snapshots, schedule);
    ASSERT_TRUE(resolved);
    result = std::move(*resolved);
  }
  const auto copy = *result;
  result.reset();
  ASSERT_EQ(copy.profile->fit_config.log_state_knots.size(), 3U);
  EXPECT_DOUBLE_EQ(copy.profile->fit_config.log_state_knots.front(), -0.3);
  EXPECT_DOUBLE_EQ(copy.profile->fit_config.log_state_knots.back(), 0.4);
  EXPECT_DOUBLE_EQ(copy.profile->fit_config.initial_sigmas.front(), 0.30);
  EXPECT_DOUBLE_EQ(copy.profile->fit_config.prior_sigmas.back(), 0.22);
  EXPECT_DOUBLE_EQ(copy.profile->fit_config.prior_weight, 0.123);
}

TEST(AmericanMarkovPolicy, ActiveVarianceLayoutSkipsPausesAndRefusesOverflowWithoutShrinking) {
  auto profiles = default_american_markov_policy_profiles();
  profiles[0].fit_config.max_parameters = 10;
  const std::array overrides{QuoteFitSymbolOverride{"D", "dense"}};
  const auto catalog = AmericanMarkovPolicyCatalog::create(profiles, overrides);
  ASSERT_TRUE(catalog);
  const auto input = quotes(8);
  const std::array snapshots{QuoteFitExpirySnapshot{market(0.3), input}};
  std::array intervals{AmericanMarkovFitInterval{0.1, 0.1, 0.0, 0.0, 1.0},
                       AmericanMarkovFitInterval{0.2, 0.1, 0.0, 0.0, 0.0},
                       AmericanMarkovFitInterval{0.3, 0.3, 0.0, 0.0, 0.0}};
  const auto accepted = catalog->resolve("D", snapshots, intervals);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted->time_groups, 2U);
  EXPECT_EQ(accepted->parameter_count, 10U);
  EXPECT_EQ(accepted->profile->fit_config.log_state_knots.size(), 5U);
  intervals[1].variance_end = 0.2;
  const auto refused = catalog->resolve("D", snapshots, intervals);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::OutOfRange);
  const auto shared = catalog->resolve("ordinary", snapshots, intervals);
  ASSERT_TRUE(shared);
  EXPECT_EQ(shared->parameter_count, 3U);
  EXPECT_EQ(shared->time_groups, 1U);
}

TEST(AmericanMarkovPolicy, PerIntervalInitialVectorsMustMatchActualTimeGroups) {
  auto profiles = default_american_markov_policy_profiles();
  const std::array initial{0.2, 0.21, 0.22, 0.23, 0.24, 0.25, 0.26, 0.27, 0.28, 0.29};
  profiles[0].fit_config.initial_sigmas = initial;
  const std::array overrides{QuoteFitSymbolOverride{"D", "dense"}};
  const auto catalog = AmericanMarkovPolicyCatalog::create(profiles, overrides);
  ASSERT_TRUE(catalog);
  const auto input = quotes(8);
  const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
  EXPECT_FALSE(catalog->resolve("D", snapshots, schedule));
  const std::array two{AmericanMarkovFitInterval{0.1, 0.1, 0.0, 0.0, 0.0}, schedule[0]};
  EXPECT_TRUE(catalog->resolve("D", snapshots, two));
}

TEST(AmericanMarkovPolicy, RejectsDuplicateExpiryInconsistentClocksAndCommonSpot) {
  const auto catalog =
      AmericanMarkovPolicyCatalog::create(default_american_markov_policy_profiles());
  ASSERT_TRUE(catalog);
  const auto input = quotes(8);
  std::array snapshots{QuoteFitExpirySnapshot{market(), input},
                       QuoteFitExpirySnapshot{market(), input}};
  EXPECT_FALSE(catalog->resolve("A", snapshots, schedule));
  snapshots[1].market = market(0.5);
  const std::array two{schedule[0], AmericanMarkovFitInterval{0.5, 0.5, 0.0, 0.0, 0.0}};
  EXPECT_TRUE(catalog->resolve("A", snapshots, two));
  snapshots[1].market.spot = 100.01;
  EXPECT_FALSE(catalog->resolve("A", snapshots, two));
  snapshots[1].market.spot = 100.0;
  snapshots[1].market.variance_years = 0.49;
  EXPECT_FALSE(catalog->resolve("A", snapshots, two));
  EXPECT_FALSE(catalog->resolve("A", {}, two));
}

TEST(AmericanMarkovPolicy, RejectsMalformedAndAllPausedCommonSchedulesEvenWithOverrides) {
  const std::array overrides{QuoteFitSymbolOverride{"A", "sparse"}};
  const auto catalog = AmericanMarkovPolicyCatalog::create(
      default_american_markov_policy_profiles(), overrides);
  ASSERT_TRUE(catalog);
  const auto input = quotes(2);
  const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
  std::array intervals = schedule;
  intervals[0].variance_end = 0.0;
  EXPECT_FALSE(catalog->resolve("A", snapshots, intervals));
  intervals = schedule;
  intervals[0].rate = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(catalog->resolve("A", snapshots, intervals));
  intervals = schedule;
  intervals[0].cash_dividend_at_end = -1.0;
  EXPECT_FALSE(catalog->resolve("A", snapshots, intervals));
  EXPECT_FALSE(catalog->resolve("A", snapshots, {}));
}

TEST(AmericanMarkovPolicy, RejectsInvalidNamesUnknownProfilesDuplicatesAndCatalogLimits) {
  auto profiles = default_american_markov_policy_profiles();
  profiles.push_back(profiles.front());
  EXPECT_FALSE(AmericanMarkovPolicyCatalog::create(profiles));
  profiles = default_american_markov_policy_profiles();
  const std::array missing{QuoteFitSymbolOverride{"A", "DENSE"}};
  EXPECT_FALSE(AmericanMarkovPolicyCatalog::create(profiles, missing));
  const std::array duplicate{QuoteFitSymbolOverride{"A", "dense"},
                             QuoteFitSymbolOverride{"A", "sparse"}};
  EXPECT_FALSE(AmericanMarkovPolicyCatalog::create(profiles, duplicate));
  for (const auto &name : {std::string{}, std::string{"bad name"}, std::string{"bad\nname"},
                           std::string(kAmericanMarkovPolicyMaxNameBytes + 1, 'a')}) {
    profiles[0].name = name;
    EXPECT_FALSE(AmericanMarkovPolicyCatalog::create(profiles));
  }
  profiles.resize(kAmericanMarkovPolicyMaxProfiles + 1);
  EXPECT_FALSE(AmericanMarkovPolicyCatalog::create(profiles));
  const std::vector<QuoteFitSymbolOverride> too_many(kAmericanMarkovPolicyMaxOverrides + 1);
  EXPECT_FALSE(AmericanMarkovPolicyCatalog::create(default_american_markov_policy_profiles(),
                                                   too_many));
  EXPECT_FALSE(AmericanMarkovPolicyCatalog::create({}));
}

TEST(AmericanMarkovPolicy, RejectsInvalidNumericAndIndependentResourceConfigurations) {
  const auto reject = [](auto mutation) {
    auto profiles = default_american_markov_policy_profiles();
    mutation(profiles[0]);
    EXPECT_FALSE(AmericanMarkovPolicyCatalog::create(profiles));
  };
  reject([](auto &p) { p.model_config.cfl_fraction = 1.01; });
  reject([](auto &p) { p.model_config.max_states = kAmericanMarkovMaxStates + 1; });
  reject([](auto &p) { p.model_config.max_stored_rows = 1; });
  reject([](auto &p) { p.model_config.max_transition_work = 0; });
  reject([](auto &p) {
    p.model_config.max_transition_work = kAmericanMarkovMaxTransitionWork + 1;
  });
  reject([](auto &p) {
    p.model_config.diffusion_method = static_cast<AmericanMarkovDiffusionMethod>(99);
  });
  reject([](auto &p) { p.model_config.max_stored_volatility_values = 1; });
  reject([](auto &p) { p.grid_config.grid_nodes = 2049; });
  reject([](auto &p) { p.grid_config.concentration_scale = 0.0; });
  reject([](auto &p) { p.fit_config.max_parameters = 4; });
  reject([](auto &p) { p.fit_config.max_parameters = kAmericanMarkovFitMaxParameters + 1; });
  reject([](auto &p) { p.fit_config.max_quotes = kAmericanMarkovFitMaxQuotes + 1; });
  reject([](auto &p) { p.fit_config.max_evaluations = 0; });
  reject([](auto &p) { p.fit_config.max_batch_values = 1; });
  reject([](auto &p) { p.fit_config.max_rollback_work = 1; });
  reject([](auto &p) { p.fit_config.min_sigma = 0.0; });
  reject([](auto &p) { p.fit_config.initial_sigma = std::numeric_limits<double>::infinity(); });
  reject([](auto &p) { p.fit_config.prior_weight = -0.1; });
  reject([](auto &p) { p.fit_config.time_shape = static_cast<AmericanMarkovFitTimeShape>(99); });
  const std::array duplicate_knots{0.0, 0.0};
  reject([&](auto &p) { p.fit_config.log_state_knots = duplicate_knots; });
  const std::array outside_sigmas{200.0};
  reject([&](auto &p) { p.fit_config.initial_sigmas = outside_sigmas; });
}

TEST(AmericanMarkovPolicy, SearchAndScaledBudgetControlsShareTheFitterGate) {
  const auto reject = [](auto mutation) {
    auto profiles = default_american_markov_policy_profiles();
    mutation(profiles[1].fit_config);
    EXPECT_FALSE(AmericanMarkovPolicyCatalog::create(profiles));
  };
  reject([](auto &c) { c.evaluations_per_parameter = 64; }); // Scaling needs an explicit cap.
  reject([](auto &c) { c.max_scaled_evaluations = 1024; }); // Orphan cap.
  reject([](auto &c) {
    c.evaluations_per_parameter = 64;
    c.max_scaled_evaluations = c.max_evaluations - 1;
  });
  reject([](auto &c) {
    c.evaluations_per_parameter = 64;
    c.max_scaled_evaluations = kAmericanMarkovFitMaxEvaluations + 1;
  });
  reject([](auto &c) { c.search = static_cast<AmericanMarkovFitSearch>(9); });
  reject([](auto &c) { c.objective_tolerance = 1.5; });
  reject([](auto &c) { c.jacobian_log_step = 0.0; });
  reject([](auto &c) { c.max_damping = c.initial_damping * 0.5; });
  auto profiles = default_american_markov_policy_profiles();
  auto &fit = profiles[1].fit_config;
  fit.search = AmericanMarkovFitSearch::LevenbergMarquardt;
  fit.evaluations_per_parameter = 64;
  fit.max_scaled_evaluations = 1024;
  fit.objective_tolerance = 1.0e-6;
  const auto catalog = AmericanMarkovPolicyCatalog::create(profiles);
  ASSERT_TRUE(catalog) << catalog.error().to_string();
  const auto input = quotes(8, 0.2);
  const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
  const auto resolved = catalog->resolve("ORD", snapshots, schedule);
  ASSERT_TRUE(resolved) << resolved.error().to_string();
  ASSERT_EQ(resolved->profile->name, "ordinary");
  const auto &owned = resolved->profile->fit_config;
  EXPECT_EQ(owned.search, AmericanMarkovFitSearch::LevenbergMarquardt);
  EXPECT_EQ(american_markov_fit_evaluation_budget(owned, resolved->parameter_count),
            std::max<std::size_t>(owned.max_evaluations, 64 * resolved->parameter_count));
  EXPECT_EQ(owned.max_scaled_evaluations, 1024U);
}

TEST(AmericanMarkovPolicy, ResolventWorkBoundsAreValidatedAndSurviveCatalogOwnership) {
  auto profiles = default_american_markov_policy_profiles();
  auto &profile = profiles[2];
  profile.model_config.diffusion_method = AmericanMarkovDiffusionMethod::VarianceResolvent;
  const auto minimum_work = 2 * profile.grid_config.grid_nodes;
  profile.model_config.max_transition_work = minimum_work - 1;
  EXPECT_FALSE(AmericanMarkovPolicyCatalog::create(profiles));
  profile.model_config.max_transition_work = minimum_work;
  profile.fit_config.max_rollback_work = minimum_work - 1;
  EXPECT_FALSE(AmericanMarkovPolicyCatalog::create(profiles));
  profile.fit_config.max_rollback_work = minimum_work;
  auto catalog = AmericanMarkovPolicyCatalog::create(profiles);
  ASSERT_TRUE(catalog) << catalog.error().to_string();
  profile.model_config.diffusion_method = AmericanMarkovDiffusionMethod::ExplicitEuler;
  profile.model_config.max_transition_work = 1;
  const auto input = quotes(2);
  const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
  const auto owned = catalog->resolve("A", snapshots, schedule);
  ASSERT_TRUE(owned) << owned.error().to_string();
  EXPECT_EQ(owned->profile->model_config.diffusion_method,
            AmericanMarkovDiffusionMethod::VarianceResolvent);
  EXPECT_EQ(owned->profile->model_config.max_transition_work, minimum_work);
  EXPECT_EQ(owned->profile->fit_config.max_rollback_work, minimum_work);
}

TEST(AmericanMarkovPolicy, ExtractedClassifierValidatesFeaturesAndPreservesThresholdRules) {
  const auto input = quotes(32, 0.02);
  const std::array snapshots{QuoteFitExpirySnapshot{market(), input}};
  const auto features = quote_fit_liquidity_features(snapshots);
  ASSERT_TRUE(features);
  const auto regime = classify_quote_fit_liquidity(*features);
  ASSERT_TRUE(regime);
  EXPECT_EQ(*regime, QuoteFitLiquidityRegime::Dense);
  auto invalid = *features;
  invalid.median_relative_spread = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(classify_quote_fit_liquidity(invalid));
  invalid = *features;
  invalid.near_money_bidless_fraction = 1.01;
  EXPECT_FALSE(classify_quote_fit_liquidity(invalid));
  QuoteFitRegimeConfig bad;
  bad.dense_max_median_relative_spread = bad.sparse_min_median_relative_spread;
  EXPECT_FALSE(classify_quote_fit_liquidity(*features, bad));
  EXPECT_FALSE(AmericanMarkovPolicyCatalog::create(default_american_markov_policy_profiles(),
                                                   {}, bad));
  const auto empty = classify_quote_fit_liquidity({});
  ASSERT_TRUE(empty);
  EXPECT_EQ(*empty, QuoteFitLiquidityRegime::Sparse);
}

TEST(AmericanMarkovPolicy, ResolvedSparseProfileFitsRawIntervalUnderOnePositiveCommonProcess) {
  auto profiles = default_american_markov_policy_profiles();
  profiles[2].fit_config.max_evaluations = 2;
  const auto catalog = AmericanMarkovPolicyCatalog::create(profiles);
  ASSERT_TRUE(catalog);
  const std::array raw{QuoteSliceQuote{100.0, Side::Call, 0.0, 10.0}};
  const std::array snapshots{QuoteFitExpirySnapshot{market(), raw}};
  const auto policy = catalog->resolve("THIN", snapshots, schedule);
  ASSERT_TRUE(policy);
  ASSERT_EQ(policy->profile->name, "sparse");
  const auto grid = make_american_markov_sinh_grid(100.0, policy->profile->grid_config);
  ASSERT_TRUE(grid);
  const std::array fitting{AmericanMarkovFitQuote{0, 100.0, Side::Call, 0.0, 10.0, 1.0}};
  const auto fit = fit_american_markov_intervals(
      {100.0, schedule, grid->states, policy->profile->model_config}, fitting,
      policy->profile->fit_config);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_EQ(fit->diagnostics().parameter_count, policy->parameter_count);
  EXPECT_EQ(fit->diagnostics().time_groups, policy->time_groups);
  EXPECT_EQ(fit->diagnostics().quotes_inside_interval, 1U);
  EXPECT_LT(fit->model().diagnostics().max_relative_mean_error, 1.0e-14);
}

TEST(AmericanMarkovPolicy, RawExtractionLimitsAndMovedFromCatalogFailClosed) {
  auto catalog = AmericanMarkovPolicyCatalog::create(default_american_markov_policy_profiles());
  ASSERT_TRUE(catalog);
  const std::vector<QuoteSliceQuote> too_many(kAmericanMarkovPolicyMaxRawQuotes + 1);
  const std::array snapshots{QuoteFitExpirySnapshot{market(), too_many}};
  EXPECT_FALSE(catalog->resolve("A", snapshots, schedule));
  const std::vector<QuoteFitExpirySnapshot> too_many_expiries(
      kAmericanMarkovPolicyMaxSnapshots + 1);
  EXPECT_FALSE(catalog->resolve("A", too_many_expiries, schedule));
  auto moved = std::move(*catalog);
  const auto input = quotes(8);
  const std::array valid{QuoteFitExpirySnapshot{market(), input}};
  EXPECT_FALSE(catalog->resolve("A", valid, schedule));
  EXPECT_TRUE(moved.resolve("A", valid, schedule));
  EXPECT_FALSE(moved.resolve("bad symbol", valid, schedule));
}

std::vector<AmericanMarkovFitInterval> weekly_schedule(std::size_t count) {
  std::vector<AmericanMarkovFitInterval> out;
  for (std::size_t i = 0; i < count; ++i) {
    const double years = static_cast<double>(i + 1) / 52.0;
    out.push_back({years, years, 0.0, 0.0, 0.0});
  }
  return out;
}

TEST(AmericanMarkovPolicy, VarianceTimeKnotsResolveBoundedCountsAndOwnTheirCoordinates) {
  const auto intervals = weekly_schedule(300);
  const auto input = quotes(8);
  // 26/52 is exactly 0.5, matching the raw expiry clock bit for bit.
  const std::array snapshots{QuoteFitExpirySnapshot{market(0.5), input}};
  const std::array overrides{QuoteFitSymbolOverride{"KNOTS", "dense"}};
  std::optional<ResolvedAmericanMarkovPolicy> result;
  {
    auto profiles = default_american_markov_policy_profiles();
    std::vector<double> knots{0.0, 0.5, 2.0, 5.0};
    profiles[0].fit_config.time_shape = AmericanMarkovFitTimeShape::VarianceTimeKnots;
    profiles[0].fit_config.variance_time_knots = knots;
    auto catalog = AmericanMarkovPolicyCatalog::create(profiles, overrides);
    ASSERT_TRUE(catalog) << catalog.error().to_string();
    knots.assign(4, 99.0);
    auto resolved = catalog->resolve("KNOTS", snapshots, intervals);
    ASSERT_TRUE(resolved) << resolved.error().to_string();
    result = std::move(*resolved);
  }
  const auto copy = *result;
  result.reset();
  // Four time knots times five dense state knots, independent of 300 expiries.
  EXPECT_EQ(copy.time_groups, 4U);
  EXPECT_EQ(copy.parameter_count, 20U);
  const auto owned = copy.profile->fit_config.variance_time_knots;
  ASSERT_EQ(owned.size(), 4U);
  EXPECT_EQ(owned[0], 0.0);
  EXPECT_EQ(owned[1], 0.5);
  EXPECT_EQ(owned[2], 2.0);
  EXPECT_EQ(owned[3], 5.0);
  // The unmodified per-interval dense profile cannot represent this schedule.
  const auto legacy = AmericanMarkovPolicyCatalog::create(
      default_american_markov_policy_profiles(), overrides);
  ASSERT_TRUE(legacy);
  const auto refused = legacy->resolve("KNOTS", snapshots, intervals);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::OutOfRange);
}

TEST(AmericanMarkovPolicy, RejectsInvalidVarianceTimeKnotProfiles) {
  const auto create = [](auto mutation) {
    auto profiles = default_american_markov_policy_profiles();
    mutation(profiles[0].fit_config);
    return AmericanMarkovPolicyCatalog::create(profiles);
  };
  const auto knot_shape = [](AmericanMarkovFitConfig &c, std::span<const double> knots) {
    c.time_shape = AmericanMarkovFitTimeShape::VarianceTimeKnots;
    c.variance_time_knots = knots;
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::array two{0.0, 1.0};
  // Coordinates without the knot shape, and the knot shape without valid coordinates.
  EXPECT_FALSE(create([&](auto &c) { c.variance_time_knots = two; }));
  EXPECT_FALSE(create([&](auto &c) { knot_shape(c, {}); }));
  for (const auto &bad : std::vector<std::vector<double>>{
           {0.5}, {1.0, 0.5}, {0.5, 0.5}, {-0.1, 0.5}, {0.0, nan}}) {
    EXPECT_FALSE(create([&](auto &c) { knot_shape(c, bad); }));
  }
  // A refusal from the validator shared with the fitter keeps its code but
  // speaks with the catalog's own prefix.
  const std::vector<double> unordered{1.0, 0.5};
  const auto refused = create([&](auto &c) { knot_shape(c, unordered); });
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(refused.error().message(), "Markov policy: invalid variance-time knot grid");
  // Dense has five state knots and 64 parameters: 12 time knots fit, 13 do not.
  std::vector<double> many(13);
  for (std::size_t i = 0; i < many.size(); ++i) many[i] = static_cast<double>(i);
  EXPECT_TRUE(create([&](auto &c) {
    knot_shape(c, std::span<const double>(many).first(12));
  }));
  EXPECT_FALSE(create([&](auto &c) { knot_shape(c, many); }));
  // Initial/prior vectors must match the knot layout exactly.
  const std::vector<double> five(5, 0.25);
  const std::vector<double> ten(10, 0.25);
  EXPECT_FALSE(create([&](auto &c) {
    knot_shape(c, two);
    c.initial_sigmas = five;
  }));
  EXPECT_FALSE(create([&](auto &c) {
    knot_shape(c, two);
    c.prior_sigmas = five;
  }));
  EXPECT_TRUE(create([&](auto &c) {
    knot_shape(c, two);
    c.initial_sigmas = ten;
    c.prior_sigmas = ten;
  }));
}

TEST(AmericanMarkovPolicy, VarianceTimeKnotScheduleRespectsVolatilityScratchCapacity) {
  auto profiles = default_american_markov_policy_profiles();
  auto &sparse = profiles[2];
  const std::array knots{0.0, 1.0};
  sparse.fit_config.time_shape = AmericanMarkovFitTimeShape::VarianceTimeKnots;
  sparse.fit_config.variance_time_knots = knots;
  // Interval-times-state-knot scratch admits at most 76/1 intervals; the compiler
  // separately charges each interval's sampled grid volatilities at compile time.
  sparse.model_config.max_stored_volatility_values = 76;
  const std::array overrides{QuoteFitSymbolOverride{"A", "sparse"}};
  const auto catalog = AmericanMarkovPolicyCatalog::create(profiles, overrides);
  ASSERT_TRUE(catalog) << catalog.error().to_string();
  const auto input = quotes(2);
  const std::array snapshots{QuoteFitExpirySnapshot{market(0.5), input}};
  EXPECT_TRUE(catalog->resolve("A", snapshots, weekly_schedule(76)));
  const auto refused = catalog->resolve("A", snapshots, weekly_schedule(77));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code(), ErrorCode::OutOfRange);
}

} // namespace
} // namespace atx::vol
