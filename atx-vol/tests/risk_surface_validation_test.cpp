#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/risk_surface_validation.hpp"
#include "atx/vol/surface_policy.hpp"

namespace atx::vol {
namespace {

enum class Shape : std::uint8_t {
  FlatVol,
  CalendarCrossing,
  AtmSpike,
  NonFinite,
};

struct SyntheticSurface {
  std::vector<double> maturities;
  Shape shape{Shape::FlatVol};
};

std::size_t synthetic_count(const void *ctx) noexcept {
  return static_cast<const SyntheticSurface *>(ctx)->maturities.size();
}

double synthetic_maturity(const void *ctx, std::size_t slice) noexcept {
  const auto &surface = *static_cast<const SyntheticSurface *>(ctx);
  return slice < surface.maturities.size() ? surface.maturities[slice]
                                           : std::numeric_limits<double>::quiet_NaN();
}

double synthetic_w(const void *ctx, std::size_t slice, double k) noexcept {
  const auto &surface = *static_cast<const SyntheticSurface *>(ctx);
  if (slice >= surface.maturities.size()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double T = surface.maturities[slice];
  switch (surface.shape) {
  case Shape::FlatVol:
    return 0.04 * T;
  case Shape::CalendarCrossing:
    return slice == 0 ? 0.08 : 0.04;
  case Shape::AtmSpike:
    return 0.04 * T + 0.20 * std::exp(-500.0 * k * k);
  case Shape::NonFinite:
    return std::abs(k) < 1.0e-12 ? std::numeric_limits<double>::quiet_NaN() : 0.04 * T;
  }
  return std::numeric_limits<double>::quiet_NaN();
}

RiskSurfaceView view_of(const SyntheticSurface &surface) noexcept {
  return RiskSurfaceView{&surface, synthetic_count, synthetic_maturity, synthetic_w};
}

TEST(SurfacePolicy, PricerConfigDefaultsToBalancedDualOutputWithMandatoryAdmission) {
  const PricerConfig config;
  EXPECT_EQ(config.preset, FitPreset::Robust);
  EXPECT_EQ(config.quality_mode, FitQualityMode::Balanced);
  EXPECT_TRUE(has_output(config.outputs, SurfacePurpose::MarketMark));
  EXPECT_TRUE(has_output(config.outputs, SurfacePurpose::Risk));
  EXPECT_EQ(config.risk_admission, RiskAdmission::Required);
  EXPECT_EQ(config.fallback, SurfaceFallback::LastKnownGood);
}

TEST(SurfacePolicy, LegacyPresetMappingNeverTreatsHftAsImplicitRisk) {
  const auto hft = map_legacy_fit_preset(FitPreset::Hft);
  EXPECT_EQ(hft.quality_mode, FitQualityMode::Latency);
  EXPECT_EQ(hft.purpose, SurfacePurpose::MarketMark);

  const auto fast = map_legacy_fit_preset(FitPreset::Fast);
  EXPECT_EQ(fast.quality_mode, FitQualityMode::Latency);
  EXPECT_EQ(fast.purpose, SurfacePurpose::Risk);

  const auto robust = map_legacy_fit_preset(FitPreset::Robust);
  EXPECT_EQ(robust.quality_mode, FitQualityMode::Balanced);
  EXPECT_EQ(robust.purpose, SurfacePurpose::Risk);

  const auto accurate = map_legacy_fit_preset(FitPreset::Accurate);
  EXPECT_EQ(accurate.quality_mode, FitQualityMode::Accuracy);
  EXPECT_EQ(accurate.purpose, SurfacePurpose::Risk);
}

TEST(RiskSurfaceValidation, FlatVolTermStructurePassesEveryHardGate) {
  const SyntheticSurface surface{{0.10, 0.25, 0.50}, Shape::FlatVol};
  const auto first = validate_risk_surface(view_of(surface));
  const auto replay = validate_risk_surface(view_of(surface));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(replay.has_value());
  EXPECT_TRUE(first->admitted());
  EXPECT_EQ(first->failures, ValidationFailure::None);
  EXPECT_EQ(first->n_butterfly_violations, 0u);
  EXPECT_EQ(first->n_calendar_violations, 0u);
  EXPECT_NE(first->validation_id, 0u);
  EXPECT_EQ(first->validation_id, replay->validation_id);
}

TEST(RiskSurfaceValidation, CalendarCrossingIsRejectedOnIndependentGrid) {
  const SyntheticSurface surface{{0.10, 0.50}, Shape::CalendarCrossing};
  const auto result = validate_risk_surface(view_of(surface));
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->admitted());
  EXPECT_TRUE(has_validation_failure(result->failures, ValidationFailure::Calendar));
  EXPECT_GT(result->n_calendar_violations, 0u);
  EXPECT_GT(result->max_calendar_slack, 0.0);
}

TEST(RiskSurfaceValidation, LocalVolSpikeIsRejectedForPriceShape) {
  const SyntheticSurface surface{{0.25}, Shape::AtmSpike};
  RiskSurfaceValidationConfig config;
  config.k_min = -0.25;
  config.k_max = 0.25;
  config.strike_grid_points = 257;
  const auto result = validate_risk_surface(view_of(surface), config);
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->admitted());
  EXPECT_TRUE(has_validation_failure(result->failures, ValidationFailure::Butterfly) ||
              has_validation_failure(result->failures, ValidationFailure::StrikeMonotonicity));
  EXPECT_GT(result->n_butterfly_violations + result->n_strike_monotonicity_violations, 0u);
}

TEST(RiskSurfaceValidation, NonFiniteNodeIsAnAdmissionFailureNotACallError) {
  const SyntheticSurface surface{{0.25}, Shape::NonFinite};
  const auto result = validate_risk_surface(view_of(surface));
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->admitted());
  EXPECT_TRUE(has_validation_failure(result->failures, ValidationFailure::NonFinite));
  EXPECT_GT(result->n_non_finite, 0u);
}

TEST(RiskSurfaceValidation, InvalidContractReturnsInvalidArgument) {
  const SyntheticSurface surface{{0.25}, Shape::FlatVol};
  RiskSurfaceValidationConfig config;
  config.k_min = 0.25;
  config.k_max = -0.25;
  const auto result = validate_risk_surface(view_of(surface), config);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskSurfaceAdmission, RejectedCandidateKeepsLastAdmittedGeneration) {
  ValidationDigest rejected;
  rejected.failures = ValidationFailure::Butterfly | ValidationFailure::Calendar;
  rejected.n_butterfly_violations = 3;
  rejected.n_calendar_violations = 2;

  const AdmissionDecision decision = decide_risk_surface_admission(
      rejected, FitQualityMode::Balanced, 42, 41, SurfaceFallback::LastKnownGood);
  EXPECT_FALSE(decision.publish_candidate);
  EXPECT_EQ(decision.health.state, SurfaceState::Degraded);
  EXPECT_EQ(decision.health.candidate_generation, 42u);
  EXPECT_EQ(decision.health.served_generation, 41u);
  EXPECT_EQ(decision.health.fallback_generation, 41u);
  EXPECT_TRUE(decision.health.using_fallback());
}

TEST(RiskSurfaceAdmission, ValidCandidatePublishesAndReplacesPriorGeneration) {
  ValidationDigest admitted;
  const AdmissionDecision decision = decide_risk_surface_admission(
      admitted, FitQualityMode::Latency, 42, 41, SurfaceFallback::LastKnownGood);
  EXPECT_TRUE(decision.publish_candidate);
  EXPECT_EQ(decision.health.state, SurfaceState::Healthy);
  EXPECT_EQ(decision.health.served_generation, 42u);
  EXPECT_TRUE(decision.health.serving_candidate());
  EXPECT_FALSE(decision.health.using_fallback());
}

TEST(RiskSurfaceAdmission, RejectedFirstGenerationLeavesRiskUnavailable) {
  ValidationDigest rejected;
  rejected.failures = ValidationFailure::NonFinite;
  const AdmissionDecision decision = decide_risk_surface_admission(
      rejected, FitQualityMode::Accuracy, 1, 0, SurfaceFallback::LastKnownGood);
  EXPECT_FALSE(decision.publish_candidate);
  EXPECT_EQ(decision.health.state, SurfaceState::Rejected);
  EXPECT_EQ(decision.health.served_generation, 0u);
  EXPECT_FALSE(decision.health.using_fallback());
}

} // namespace
} // namespace atx::vol
