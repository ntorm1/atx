#include "atx/core/macro.hpp"
#include "atx/engine/risk/cov_ewma.hpp"
#include "atx/engine/risk/eigen_adjust.hpp"
#include "atx/engine/risk/estimator_policy.hpp"
#include "atx/engine/risk/hybrid_factor_model.hpp"
#include "atx/engine/risk/model_validation.hpp"
#include "atx/engine/risk/specific_risk.hpp"
#include "atx/engine/risk/vol_regime.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>
#include <locale>
#include <numeric>
#include <vector>

namespace atxtest_risk_estimator_v2 {
using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using namespace atx::engine::risk;
constexpr f64 nan = std::numeric_limits<f64>::quiet_NaN();

TEST(RiskEstimatorV2, EwmaPreservesMissingSessionAgesAgainstScalarOracle) {
  const std::vector<f64> v{1, nan, 4, -2};
  const std::vector<usize> ages{0, 1, 4, 7};
  auto result = ewma_variance_v2(v, ages, 2, 0, 252);
  ASSERT_TRUE(result);
  f64 sw = 0, sw2 = 0, mean = 0, variance = 0;
  for (usize i = 0; i < v.size(); ++i)
    if (std::isfinite(v[i])) {
      const auto w = std::exp2(-static_cast<f64>(ages[i]) / 2);
      sw += w;
      sw2 += w * w;
      mean += w * v[i];
    }
  mean /= sw;
  for (usize i = 0; i < v.size(); ++i)
    if (std::isfinite(v[i]))
      variance += std::exp2(-static_cast<f64>(ages[i]) / 2) * (v[i] - mean) * (v[i] - mean);
  EXPECT_NEAR(result->variance, variance / sw, 1e-13);
  EXPECT_NEAR(result->effective_observations, sw * sw / sw2, 1e-13);
  EXPECT_EQ(result->observations, 3U);
  const auto compressed = ewma_variance_v2(v, {}, 2, 0, 252);
  ASSERT_TRUE(compressed);
  EXPECT_GT(std::abs(compressed->variance - result->variance), 0.01);
  EXPECT_FALSE(ewma_variance_v2(v, std::vector<usize>{0, 1, 1, 7}, 2, 0, 252));
}

TEST(RiskEstimatorV2, EigenUsesObservedEffectiveHistoryAndValidatesInactivePaths) {
  MatX f = MatX::Identity(2, 2);
  f(0, 0) = 0.4;
  auto result = eigen_adjust_v2(f, 23.4, 4, 1.4, 71);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->simulated_observations, 23U);
  const auto repeat = eigen_adjust_v2(f, 23.4, 4, 1.4, 71);
  ASSERT_TRUE(repeat);
  EXPECT_EQ(std::memcmp(result->covariance.data(), repeat->covariance.data(), 4 * sizeof(f64)), 0);
  f(0, 1) = 0.2;
  EXPECT_FALSE(eigen_adjust_v2(f, 23.4, 0, 1.4, 71));
  f = MatX::Identity(2, 2);
  f(0, 0) = -1;
  EXPECT_FALSE(eigen_adjust_v2(f, 23.4, 4, 0, 71));
  EXPECT_FALSE(eigen_adjust_v2(MatX::Identity(3, 3), 2, 2, 1.4, 71));
}

TEST(RiskEstimatorV2, VraRequiresPriorClockAndKeepsUnavailableEvidenceExplicit) {
  MatX realized = MatX::Constant(4, 2, 2.0);
  PriorVarianceForecasts prior;
  prior.variances = MatX::Ones(4, 2);
  prior.available_ages = {1, 2, 3, 4};
  auto result = vol_regime_multiplier_v2(realized, prior, {}, 42, 2);
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->available);
  EXPECT_DOUBLE_EQ(result->lambda2, 4);
  prior.available_ages[0] = 0;
  EXPECT_FALSE(vol_regime_multiplier_v2(realized, prior, {}, 42, 2));
  prior.available_ages.assign(4, std::numeric_limits<usize>::max());
  result = vol_regime_multiplier_v2(realized, prior, {}, 42, 2);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->available);
  EXPECT_DOUBLE_EQ(result->lambda2, 1);
  EXPECT_EQ(result->pairs_unavailable, 8U);
}

struct Inputs {
  MatX returns = MatX(48, 1), residuals = MatX(48, 20), exposures = MatX::Ones(20, 1);
  std::vector<f64> caps = std::vector<f64>(20);
  Inputs() {
    for (Eigen::Index t = 0; t < 48; ++t) {
      returns(t, 0) = 0.01 * std::sin(static_cast<f64>(t) * 0.73);
      for (Eigen::Index i = 0; i < 20; ++i)
        residuals(t, i) = (0.01 + 0.002 * static_cast<f64>(i)) *
                          std::sin(static_cast<f64>(t) * 0.91 + static_cast<f64>(i));
    }
    for (usize i = 0; i < caps.size(); ++i)
      caps[i] = 100 + static_cast<f64>(i) * 10;
  }
};

TEST(RiskEstimatorV2, SpecificThinFallbackAndCapWeightedDecileFormula) {
  Inputs in;
  in.residuals.col(19).setConstant(nan);
  SpecificRiskConfigV2 cfg;
  cfg.min_observations = 8;
  cfg.nw_lags = 0;
  cfg.bayesian_q = 0;
  const auto unshrunk = specific_risk_v2(in.residuals, in.exposures, in.caps, {}, cfg);
  ASSERT_TRUE(unshrunk);
  EXPECT_EQ(unshrunk->structural_fallback[19], 1);
  EXPECT_EQ(unshrunk->observations[19], 0U);
  EXPECT_GT(unshrunk->variances[19], 0);
  cfg.bayesian_q = .1;
  const auto shrunk = specific_risk_v2(in.residuals, in.exposures, in.caps, {}, cfg);
  ASSERT_TRUE(shrunk);
  const f64 a = std::sqrt(unshrunk->variances[0]), b = std::sqrt(unshrunk->variances[1]);
  const f64 target = (a * in.caps[0] + b * in.caps[1]) / (in.caps[0] + in.caps[1]);
  const f64 dispersion = std::sqrt(((a - target) * (a - target) + (b - target) * (b - target)) / 2);
  const f64 intensity = .1 * std::abs(a - target) / (dispersion + .1 * std::abs(a - target));
  EXPECT_NEAR(shrunk->shrinkage_weight[0], intensity, 1e-14);
  EXPECT_NEAR(std::sqrt(shrunk->variances[0]), (1 - intensity) * a + intensity * target, 1e-14);
  in.caps[3] = nan;
  EXPECT_FALSE(specific_risk_v2(in.residuals, in.exposures, in.caps, {}, cfg));
}

TEST(RiskEstimatorV2, CleaningSeparatesFactorAndSpecificRegimeAndBindsStatus) {
  Inputs in;
  auto cfg = effective_history_v2();
  cfg.eigen_simulations = 0;
  cfg.structural_min_observations = 8;
  const auto unadjusted =
      clean_risk_estimates_v2(in.returns, in.residuals, in.exposures, in.caps, {}, cfg);
  ASSERT_TRUE(unadjusted);
  EXPECT_EQ(unadjusted->diagnostics.factor_vra, PriorAdjustmentStatus::UnavailableUnverified);
  RiskVraEvidence prior;
  prior.identity = "synthetic-observed-prior-v1";
  prior.factor.realized = MatX::Constant(12, 1, 2);
  prior.specific.realized = MatX::Constant(12, 20, 3);
  prior.factor.forecasts.variances = MatX::Ones(12, 1);
  prior.specific.forecasts.variances = MatX::Ones(12, 20);
  prior.specific.forecasts.weights = MatX::Ones(12, 20);
  for (usize i = 0; i < 12; ++i) {
    prior.factor.forecasts.available_ages.push_back(i + 1);
    prior.specific.forecasts.available_ages.push_back(i + 1);
  }
  const auto adjusted =
      clean_risk_estimates_v2(in.returns, in.residuals, in.exposures, in.caps, {}, cfg, &prior);
  ASSERT_TRUE(adjusted);
  EXPECT_DOUBLE_EQ(adjusted->diagnostics.factor_lambda2, 4);
  EXPECT_DOUBLE_EQ(adjusted->diagnostics.specific_lambda2, 9);
  EXPECT_DOUBLE_EQ(adjusted->factor_covariance(0, 0), unadjusted->factor_covariance(0, 0) * 4);
  EXPECT_DOUBLE_EQ(adjusted->specific_variances[0], unadjusted->specific_variances[0] * 9);
  EXPECT_EQ(adjusted->diagnostics.evidence_identity, prior.identity);
  cfg.missing_prior = MissingPriorForecastRule::RequireObservedV1;
  EXPECT_FALSE(clean_risk_estimates_v2(in.returns, in.residuals, in.exposures, in.caps, {}, cfg));
  EXPECT_TRUE(
      clean_risk_estimates_v2(in.returns, in.residuals, in.exposures, in.caps, {}, cfg, &prior));
}

TEST(RiskEstimatorV2, HybridRetainsThinNamesAndRejectsUnknownCurrentInputs) {
  ReturnPanel ret;
  ret.r.resize(65, 8);
  for (Eigen::Index t = 0; t < 65; ++t)
    for (Eigen::Index i = 0; i < 8; ++i)
      ret.r(t, i) = .01 * std::sin(static_cast<f64>(t) * .4) +
                    .005 * std::cos(static_cast<f64>(t) * .83 + static_cast<f64>(i));
  for (Eigen::Index t = 3; t < 65; ++t)
    ret.r(t, 7) = nan;
  ExposureSeries exp;
  exp.cap.assign(8, 100);
  exp.market = true;
  HybridCfg cfg;
  cfg.window = 60;
  cfg.select = StatFactorSelect::Fixed;
  cfg.n_stat_fixed = 0;
  auto old = HybridFactorModelBuilder::build(ret, exp, cfg);
  ASSERT_TRUE(old);
  EXPECT_EQ(old->assets.size(), 7U);
  auto old_again = HybridFactorModelBuilder::build(ret, exp, cfg);
  ASSERT_TRUE(old_again);
  EXPECT_EQ(std::memcmp(old->model.specific_var().data(), old_again->model.specific_var().data(),
                        7 * sizeof(f64)),
            0);
  cfg.estimator = effective_history_v2();
  cfg.estimator.eigen_simulations = 0;
  cfg.estimator.structural_min_observations = 8;
  const auto corrected = HybridFactorModelBuilder::build(ret, exp, cfg);
  ASSERT_TRUE(corrected);
  EXPECT_EQ(corrected->assets.size(), 8U);
  EXPECT_GT(corrected->model.specific_var()[7], 0);
  EXPECT_EQ(corrected->model.estimator_diagnostics().structural_fallback_assets, 1U);
  EXPECT_EQ(corrected->model.estimator_diagnostics().statistical_fallback_assets, 0U);
  FactorModel copy = corrected->model;
  EXPECT_EQ(copy.estimator_diagnostics().recipe, risk_estimator_recipe(cfg.estimator));
  cfg.estimator.missing_prior = MissingPriorForecastRule::RequireObservedV1;
  EXPECT_FALSE(HybridFactorModelBuilder::build(ret, exp, cfg));
  cfg.estimator.missing_prior = MissingPriorForecastRule::LeaveUnadjustedUnverifiedV2;
  exp.cap[0] = nan;
  EXPECT_FALSE(HybridFactorModelBuilder::build(ret, exp, cfg));
  exp.cap[0] = 100;
  exp.industry.assign(8, 0);
  exp.n_industries = std::numeric_limits<atx::u32>::max();
  EXPECT_FALSE(HybridFactorModelBuilder::build(ret, exp, cfg));
}

TEST(RiskEstimatorV2, Validation21FixesHoldingsAndMakesMissingBooksUnavailable) {
  ReturnPanel ret;
  ret.r = MatX::Constant(70, 2, .01);
  const RiskModelFactory factory = [](usize a) -> atx::core::Result<ModelSnapshot> {
    ATX_TRY(auto model, FactorModel::create(MatX::Ones(2, 1), MatX::Constant(1, 1, .0001),
                                            VecX::Constant(2, .0002), a, a + 20));
    return atx::core::Ok(ModelSnapshot{std::move(model), {0, 1}});
  };
  Validation21Cfg cfg;
  cfg.n_periods = 2;
  cfg.n_min_variance = 2;
  cfg.n_optimized = 1;
  const auto full = validate_risk_model_21d(factory, ret, cfg);
  ASSERT_TRUE(full);
  ASSERT_FALSE(full->metrics.empty());
  EXPECT_EQ(full->metrics[0].observations, 2U);
  EXPECT_NEAR(full->metrics[0].mean_pred_vol, std::sqrt(21 * .0002), 1e-14);
  EXPECT_EQ(full->unverified_vra_dates, 2U);
  ret.r(0, 0) = nan;
  const auto missing = validate_risk_model_21d(factory, ret, cfg);
  ASSERT_TRUE(missing);
  EXPECT_EQ(missing->metrics[0].observations, 1U);
  EXPECT_EQ(missing->metrics[0].unavailable, 1U);
  EXPECT_FALSE(missing->metrics[0].defined);
  EXPECT_NE(missing->to_json().find("\"bias\":null"), std::string::npos);
  cfg.first_as_of = 20;
  EXPECT_FALSE(validate_risk_model_21d(factory, ret, cfg));
  cfg.first_as_of = 21;
  cfg.n_periods = std::numeric_limits<usize>::max();
  EXPECT_FALSE(validate_risk_model_21d(factory, ret, cfg));
}

TEST(RiskEstimatorV2, ValidationAdmitsAllPersistentBookGroupsBeforeCallingFactory) {
  ReturnPanel ret;
  ret.r = MatX::Zero(30, 1);
  usize calls = 0;
  const RiskModelFactory factory = [&](usize a) -> atx::core::Result<ModelSnapshot> {
    ++calls;
    ATX_TRY(auto model, FactorModel::create(MatX::Ones(1, 1), MatX::Constant(1, 1, .0001),
                                           VecX::Constant(1, .0002), a, a + 1));
    return atx::core::Ok(ModelSnapshot{std::move(model), {0}});
  };
  Validation21Cfg cfg;
  cfg.n_periods = 1;
  cfg.n_min_variance = cfg.n_optimized = 0;
  cfg.max_working_bytes = 128 * 1024;
  cfg.books.assign(10'000, NamedBook{"custom", {1.0}});
  EXPECT_FALSE(validate_risk_model_21d(factory, ret, cfg));
  EXPECT_EQ(calls, 0U);
  cfg.books.resize(2);
  ASSERT_TRUE(validate_risk_model_21d(factory, ret, cfg));
  EXPECT_EQ(calls, 1U);
  cfg.books[0].name.assign(128 * 1024, 'x');
  EXPECT_FALSE(validate_risk_model_21d(factory, ret, cfg));
  EXPECT_EQ(calls, 1U);
}

TEST(RiskEstimatorV2, ValidationModelWorkspaceUsesRemainingCombinedBudget) {
  ReturnPanel ret;
  ret.r = MatX::Zero(30, 20);
  usize calls = 0;
  const RiskModelFactory factory = [&](usize a) -> atx::core::Result<ModelSnapshot> {
    ++calls;
    ATX_TRY(auto model, FactorModel::create(MatX::Identity(20, 20),
        MatX::Identity(20, 20) * .0001, VecX::Constant(20, .0002), a, a + 1));
    std::vector<usize> assets(20);
    std::iota(assets.begin(), assets.end(), 0);
    return atx::core::Ok(ModelSnapshot{std::move(model), std::move(assets)});
  };
  Validation21Cfg cfg;
  cfg.n_periods = 1;
  cfg.n_min_variance = cfg.n_optimized = 0;
  cfg.max_working_bytes = 64 * 1024;
  EXPECT_FALSE(validate_risk_model_21d(factory, ret, cfg));
  EXPECT_EQ(calls, 1U);
  cfg.max_working_bytes = 1024 * 1024;
  EXPECT_TRUE(validate_risk_model_21d(factory, ret, cfg));
  EXPECT_EQ(calls, 2U);
}

TEST(RiskEstimatorV2, RecipeIdentityUsesClassicLocaleAndActiveKnobs) {
  struct Punct : std::numpunct<char> {
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
  };
  const auto cfg = effective_history_v2();
  const auto recipe = risk_estimator_recipe(cfg);
  const auto previous = std::locale();
  std::locale::global(std::locale(previous, new Punct));
  const auto changed_locale = risk_estimator_recipe(cfg);
  std::locale::global(previous);
  EXPECT_EQ(recipe, changed_locale);
  auto changed = cfg;
  changed.specific_nw_lags = 4;
  EXPECT_NE(recipe, risk_estimator_recipe(changed));
  changed = cfg;
  changed.missing_prior = MissingPriorForecastRule::RequireObservedV1;
  EXPECT_NE(recipe, risk_estimator_recipe(changed));
}

TEST(RiskEstimatorV2, NeweyWestCorrectionUsesConsistentWeightedLagMoments) {
  const std::vector<f64> values{1, 3, -1, 2, 4, -2};
  const std::vector<usize> ages{0, 1, 3, 4, 5, 8};
  auto variance = [&](f64 half, usize lags) {
    f64 sw = 0, mean = 0;
    for (usize i = 0; i < values.size(); ++i) {
      const auto w = std::exp2(-static_cast<f64>(ages[i]) / half);
      sw += w;
      mean += w * values[i];
    }
    mean /= sw;
    f64 total = 0;
    for (usize i = 0; i < values.size(); ++i)
      total +=
          std::exp2(-static_cast<f64>(ages[i]) / half) * (values[i] - mean) * (values[i] - mean);
    for (usize lag = 1; lag <= lags; ++lag) {
      f64 moment = 0;
      for (usize i = 0; i < values.size(); ++i)
        for (usize j = 0; j < values.size(); ++j)
          if (ages[j] >= ages[i] && ages[j] - ages[i] == lag)
            moment += std::exp2(-static_cast<f64>(ages[j]) / half) * (values[i] - mean) *
                      (values[j] - mean);
      total += 2 * (1 - static_cast<f64>(lag) / static_cast<f64>(lags + 1)) * moment;
    }
    return total / sw;
  };
  const auto result = ewma_variance_v2(values, ages, 2, 2, 6);
  ASSERT_TRUE(result);
  const auto expected = variance(2, 0) * std::max(0.0, variance(6, 2) / variance(6, 0));
  EXPECT_NEAR(result->variance, expected, 1e-12);
}

TEST(RiskEstimatorV2, RollingMradDetectsChangingBiasAndDoesNotCompactMissingMonths) {
  ReturnPanel ret;
  ret.r = MatX::Zero(506, 2);
  const f64 forecast_var = 21 * .0002;
  // Alternating signs keep each regime's mean zero. The full 24-period sample
  // variance is exactly one, while the first/last rolling years have different risk.
  const f64 low = .2, high = std::sqrt((23 - 12 * low * low) / 12);
  for (usize period = 0; period < 24; ++period) {
    const f64 z = (period % 2 == 0 ? 1.0 : -1.0) * (period < 12 ? low : high);
    for (usize day = period * 21; day < (period + 1) * 21; ++day)
      ret.r.row(static_cast<Eigen::Index>(day)).setConstant(z * std::sqrt(forecast_var) / 21);
  }
  const RiskModelFactory factory = [](usize a) -> atx::core::Result<ModelSnapshot> {
    ATX_TRY(auto model, FactorModel::create(MatX::Ones(2, 1), MatX::Constant(1, 1, .0001),
                                            VecX::Constant(2, .0002), a, a + 1));
    return atx::core::Ok(ModelSnapshot{std::move(model), {0, 1}});
  };
  Validation21Cfg cfg;
  cfg.n_periods = 24;
  cfg.n_min_variance = 0;
  cfg.n_optimized = 0;
  const auto full = validate_risk_model_21d(factory, ret, cfg);
  ASSERT_TRUE(full);
  EXPECT_NEAR(full->metrics[0].bias, 1, 1e-12);
  EXPECT_NEAR(full->metrics[0].absolute_bias_deviation, 0, 1e-12);
  EXPECT_EQ(full->metrics[0].rolling_windows, 13U);
  EXPECT_GT(full->metrics[0].mrad, .15);
  for (const auto &metric : full->metrics)
    if (metric.cohort == "specific_risk_decile")
      EXPECT_FALSE(metric.mrad_defined);
  // Missing twelfth month intersects twelve of the thirteen calendar windows;
  // only months13..24 remain eligible, rather than shifting a compacted series.
  ret.r(11 * 21, 0) = nan;
  const auto missing = validate_risk_model_21d(factory, ret, cfg);
  ASSERT_TRUE(missing);
  EXPECT_EQ(missing->metrics[0].rolling_windows, 1U);
  cfg.step = 20;
  const auto overlapping = validate_risk_model_21d(factory, ret, cfg);
  ASSERT_TRUE(overlapping);
  EXPECT_FALSE(overlapping->mrad_clock_eligible);
  EXPECT_FALSE(overlapping->metrics[0].mrad_defined);
}

} // namespace atxtest_risk_estimator_v2
