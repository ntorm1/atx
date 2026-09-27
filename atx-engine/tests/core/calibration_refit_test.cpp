#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/cost/calibration.hpp"

namespace atx_test_w1_b1_calibration_refit {
namespace cost = atx::engine::cost;
using atx::f64;
using atx::usize;
constexpr f64 scale = 0.8;
constexpr f64 mean_log_p = -3.0;

// Nine symmetric log participations with an exact analytic power law. A slope
// constraint leaves symmetric residuals, so the robust intercept has a known
// closed form without threshold/seed search or simulator-generated fills.
std::vector<cost::CostObs> observations(f64 slope) {
  std::vector<cost::CostObs> out;
  for (usize i = 0U; i < 9U; ++i) {
    const f64 x = -5.0 + 0.5 * static_cast<f64>(i);
    const f64 p = std::exp(x);
    out.push_back({p, 0.02, scale * 0.02 * std::exp(slope * x), 0.0});
  }
  return out;
}

void check_applied_diagnostics(const cost::CalibratedCost& result,
                               const std::vector<cost::CostObs>& obs) {
  std::vector<f64> residuals;
  f64 mean = 0.0;
  for (const auto& row : obs) mean += std::log(row.temp) - std::log(row.sigma);
  mean /= static_cast<f64>(obs.size());
  f64 rss = 0.0, sst = 0.0;
  for (const auto& row : obs) {
    const auto y = std::log(row.temp) - std::log(row.sigma);
    const auto predicted = std::log(result.impact.Y) + result.impact.delta * std::log(row.participation);
    const auto error = y - predicted;
    residuals.push_back(std::abs(error)); rss += error * error;
    sst += (y - mean) * (y - mean);
  }
  std::sort(residuals.begin(), residuals.end());
  EXPECT_NEAR(result.report.r2_temp, 1.0 - rss / sst, 1e-12);
  EXPECT_NEAR(result.report.resid_p95, residuals[7], 1e-12);
  const f64 expected_y_se = result.impact.Y *
      std::sqrt(rss / static_cast<f64>(obs.size() - 1U) / static_cast<f64>(obs.size()));
  EXPECT_NEAR(result.report.Y_stderr, expected_y_se, 1e-12);
  EXPECT_EQ(result.report.delta_stderr, 0.0);
}

TEST(CostCalibrationRefit, UpperAndLowerClampsRefitInterceptAndAppliedModelDiagnostics) {
  constexpr std::array<std::array<f64, 2>, 2> cases{{{1.2, 0.9}, {0.1, 0.3}}};
  for (const auto& [raw, applied] : cases) {
    const auto obs = observations(raw);
    const auto result = cost::calibrate_from_obs(obs);
    ASSERT_EQ(result.report.status, cost::CostCalibrationStatus::Applied);
    EXPECT_EQ(result.report.rule, cost::CostCalibrationRule::RefitClampedV2);
    EXPECT_TRUE(result.report.delta_clamped); EXPECT_FALSE(result.report.delta_fixed);
    EXPECT_EQ(result.report.uncertainty, cost::CostCalibrationUncertainty::ConditionalOnAppliedDelta);
    EXPECT_NEAR(result.report.raw_Y, scale, 1e-11);
    EXPECT_NEAR(result.report.raw_delta, raw, 1e-11);
    EXPECT_DOUBLE_EQ(result.impact.delta, applied);
    EXPECT_NEAR(result.impact.Y, scale * std::exp((raw - applied) * mean_log_p), 1e-11);
    EXPECT_DOUBLE_EQ(result.report.applied_Y, result.impact.Y);
    EXPECT_DOUBLE_EQ(result.report.applied_delta, result.impact.delta);
    EXPECT_NEAR(result.report.r2_temp, 1.0 - (raw - applied) * (raw - applied) / (raw * raw), 1e-11);
    check_applied_diagnostics(result, obs);
  }
}

TEST(CostCalibrationRefit, ExplicitLegacyKeepsOldUnconstrainedInterceptAndDiagnostics) {
  const auto obs = observations(1.2);
  const auto legacy = cost::calibrate_from_obs(obs, {}, cost::CostCalibrationRule::LegacyClampV1);
  const auto corrected = cost::calibrate_from_obs(obs);
  EXPECT_EQ(legacy.report.rule, cost::CostCalibrationRule::LegacyClampV1);
  EXPECT_NEAR(legacy.impact.Y, scale, 1e-11);
  EXPECT_DOUBLE_EQ(legacy.impact.delta, 0.9);
  EXPECT_NEAR(legacy.report.r2_temp, 1.0, 1e-12); // explicitly the old, pre-clamp diagnostic
  EXPECT_EQ(legacy.report.uncertainty, cost::CostCalibrationUncertainty::OlsApproximation);
  EXPECT_GT(legacy.impact.Y, 2.0 * corrected.impact.Y);
  const auto ordinary = cost::calibrate_from_obs(observations(0.55));
  EXPECT_FALSE(ordinary.report.delta_clamped);
  EXPECT_NEAR(ordinary.impact.Y, scale, 1e-11);
  EXPECT_NEAR(ordinary.impact.delta, 0.55, 1e-11);
}

TEST(CostCalibrationRefit, FixedSquareRootRefitsRatherThanTransplantingAnotherExponentScale) {
  const auto obs = observations(0.7);
  auto result = cost::calibrate_from_obs_fixed_delta(obs, 0.5); ASSERT_TRUE(result);
  EXPECT_EQ(result->report.status, cost::CostCalibrationStatus::Applied);
  EXPECT_TRUE(result->report.delta_fixed); EXPECT_FALSE(result->report.delta_clamped);
  EXPECT_DOUBLE_EQ(result->impact.delta, 0.5);
  EXPECT_TRUE(std::isnan(result->report.raw_delta)); // fixed fit does not estimate an unused slope
  EXPECT_NEAR(result->impact.Y, scale * std::exp(0.2 * mean_log_p), 1e-11);
  check_applied_diagnostics(*result, obs);
  for (const f64 invalid : {0.0, 1.0, std::numeric_limits<f64>::quiet_NaN(),
                            std::numeric_limits<f64>::infinity()})
    EXPECT_FALSE(cost::calibrate_from_obs_fixed_delta(obs, invalid));
  EXPECT_FALSE(cost::calibrate_from_obs_fixed_delta({}, 0.5));
}

TEST(CostCalibrationRefit, FiniteRowGuardsAndSingularDesignHaveExplicitOutcomes) {
  auto obs = observations(0.55);
  const auto nan = std::numeric_limits<f64>::quiet_NaN();
  const auto inf = std::numeric_limits<f64>::infinity();
  obs.push_back({inf, 0.02, 0.01, 0.0});
  obs.push_back({0.1, inf, 0.01, 0.0});
  obs.push_back({0.1, 0.02, inf, 0.0});
  obs.push_back({nan, 0.02, 0.01, 0.0});
  obs.push_back({0.0, 0.02, 0.01, 0.0});
  const auto result = cost::calibrate_from_obs(obs);
  ASSERT_EQ(result.report.status, cost::CostCalibrationStatus::Applied);
  EXPECT_EQ(result.report.n_fills, 9U); EXPECT_EQ(result.report.n_dropped, 5U);
  EXPECT_NEAR(result.impact.Y, scale, 1e-11);
  const std::vector<cost::CostObs> constant(5U, {0.01, 0.02, 0.8 * 0.02 * 0.1, 0.0});
  const auto unknown_slope = cost::calibrate_from_obs(constant);
  EXPECT_EQ(unknown_slope.report.status, cost::CostCalibrationStatus::DegenerateParticipation);
  EXPECT_EQ(unknown_slope.report.uncertainty, cost::CostCalibrationUncertainty::Unavailable);
  auto fixed = cost::calibrate_from_obs_fixed_delta(constant, 0.5); ASSERT_TRUE(fixed);
  EXPECT_NEAR(fixed->impact.Y, 0.8, 1e-11);
  EXPECT_TRUE(std::isnan(fixed->report.raw_delta)); // slope was not identifiable
  auto bad_prior = atx::engine::exec::ImpactCfg{}; bad_prior.gamma = nan;
  EXPECT_EQ(cost::calibrate_from_obs(obs, bad_prior).report.status, cost::CostCalibrationStatus::InvalidPrior);
  EXPECT_FALSE(cost::calibrate_from_obs_fixed_delta(obs, 0.5, bad_prior));
}

TEST(CostCalibrationRefit, FiniteExtremeRatioUsesLogDifferenceAndUnavailablePermanentMarksKeepPrior) {
  auto obs = observations(0.5);
  for (auto& row : obs) {
    // Both raw inputs remain finite; temp/sigma would overflow. The fitted scale
    // is unrepresentable, so the result must be an explicit fallback, not inf.
    row.sigma = 1e-300; row.temp = 1e100 * std::sqrt(row.participation);
    row.perm = std::numeric_limits<f64>::quiet_NaN();
  }
  const auto prior = atx::engine::exec::ImpactCfg{};
  const auto failed = cost::calibrate_from_obs(obs, prior);
  EXPECT_EQ(failed.report.status, cost::CostCalibrationStatus::NumericalFailure);
  EXPECT_DOUBLE_EQ(failed.impact.Y, prior.Y);
  EXPECT_DOUBLE_EQ(failed.report.applied_Y, prior.Y);
  obs = observations(0.5);
  for (auto& row : obs) row.perm = std::numeric_limits<f64>::infinity();
  const auto temporary_only = cost::calibrate_from_obs(obs, prior);
  ASSERT_EQ(temporary_only.report.status, cost::CostCalibrationStatus::Applied);
  EXPECT_FALSE(temporary_only.report.permanent_fit_applied);
  EXPECT_DOUBLE_EQ(temporary_only.impact.gamma, prior.gamma);
  EXPECT_EQ(temporary_only.report.r2_perm, 0.0);
}

TEST(CostCalibrationRefit, NearConstantLargeLogParticipationsDoNotAbortTheRobustSolver) {
  std::vector<cost::CostObs> obs;
  for (usize i = 0U; i < 9U; ++i) {
    const f64 log_p = -700.0 + 1e-10 * (static_cast<f64>(i) - 4.0);
    obs.push_back({std::exp(log_p), 0.02, scale * 0.02 * std::exp(0.5 * log_p), 0.0});
  }
  const auto estimated = cost::calibrate_from_obs(obs);
  ASSERT_EQ(estimated.report.status, cost::CostCalibrationStatus::Applied);
  EXPECT_TRUE(std::isfinite(estimated.impact.Y));
  EXPECT_GE(estimated.impact.delta, 0.3); EXPECT_LE(estimated.impact.delta, 0.9);
  auto fixed = cost::calibrate_from_obs_fixed_delta(obs, 0.5); ASSERT_TRUE(fixed);
  EXPECT_NEAR(fixed->impact.Y, scale, 1e-10);
  EXPECT_TRUE(std::isnan(fixed->report.raw_delta));
  // Even exactly singular raw slope data are usable with the slope declared.
  for (auto& row : obs) row = obs.front();
  auto singular_fixed = cost::calibrate_from_obs_fixed_delta(obs, 0.5); ASSERT_TRUE(singular_fixed);
  EXPECT_NEAR(singular_fixed->impact.Y, scale, 1e-10);
}
} // namespace atx_test_w1_b1_calibration_refit
