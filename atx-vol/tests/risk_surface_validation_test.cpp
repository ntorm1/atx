#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

#include "atx/vol/pricer_fitter.hpp"
#include "atx/vol/risk_surface_validation.hpp"
#include "atx/vol/surface_policy.hpp"
#include "atx/vol/vol_curve.hpp"

namespace atx::vol {
namespace {

enum class Shape : std::uint8_t {
  FlatVol,
  CalendarCrossing,
  AtmSpike,
  NonFinite,
  // Task 3 additions (rfx-task-3-brief 3a/3d/3e): a NaN only at one declared
  // calendar-only k (I-1); a wing slope exceeding the Roger-Lee ceiling
  // (I-5 gate coverage); a bump narrow enough to vanish at its two uniform
  // grid neighbors but visible once its own location is densified in (I-3).
  CalendarOnlyNaN,
  SteepWing,
  LocalKink,
};

struct SyntheticSurface {
  std::vector<double> maturities;
  Shape shape{Shape::FlatVol};
  double calendar_only_k{0.0};  // CalendarOnlyNaN: the one k that is NaN
  double kink_k{0.0};           // LocalKink: the bump center (a "node")
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
  case Shape::CalendarOnlyNaN:
    return k == surface.calendar_only_k ? std::numeric_limits<double>::quiet_NaN() : 0.04 * T;
  case Shape::SteepWing:
    return 1.0 + 5.0 * std::abs(k);  // |dw/dk| = 5 > default ceiling of 2
  case Shape::LocalKink: {
    // Width << the default 97-pt Balanced grid spacing (~0.0104): negligible
    // at the two uniform samples straddling kink_k, full amplitude only at
    // kink_k itself.
    const double d = (k - surface.kink_k) / 5.0e-4;
    return 0.04 * T + 0.20 * std::exp(-d * d);
  }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

// LocalKink's declared "node" location: only kink_k, so the densified grid
// gains exactly one extra sample (plus its within-tolerance duplicates are
// deduped away by build_slice_grid).
std::size_t synthetic_node_ks(const void *ctx, std::size_t /*slice*/,
                              std::span<double> out) noexcept {
  const auto &surface = *static_cast<const SyntheticSurface *>(ctx);
  if (surface.shape != Shape::LocalKink || out.empty()) {
    return 0;
  }
  out[0] = surface.kink_k;
  return 1;
}

RiskSurfaceView view_of(const SyntheticSurface &surface) noexcept {
  return RiskSurfaceView{&surface, synthetic_count, synthetic_maturity, synthetic_w};
}

// Same callbacks, but with the node-k hint wired — the I-3 fix path.
RiskSurfaceView densified_view_of(const SyntheticSurface &surface) noexcept {
  return RiskSurfaceView{&surface, synthetic_count, synthetic_maturity, synthetic_w,
                         synthetic_node_ks};
}

// MERGE (routing call): the v2 dual bundle is an EXPLICIT opt-in. A default
// PricerConfig names no v2 policy, so it is the legacy single-surface request
// that serves a MARK (da718f7/WP12: strict risk admission is opted into, never
// inherited). NAMING either v2 field is the request; once a Risk output is
// named, its independent admission is still mandatory and non-negotiable.
TEST(SurfacePolicy, PricerConfigDefaultsToLegacyMarkAndOptsIntoDualOutput) {
  const PricerConfig config;
  EXPECT_EQ(config.preset, FitPreset::Robust);
  EXPECT_FALSE(config.quality_mode.has_value());
  EXPECT_FALSE(config.outputs.has_value());
  // The v2 knobs that only modulate a v2 request keep their production defaults.
  EXPECT_EQ(config.risk_admission, RiskAdmission::Required);
  EXPECT_EQ(config.fallback, SurfaceFallback::LastKnownGood);

  PricerConfig v2;
  v2.quality_mode = FitQualityMode::Balanced;
  v2.outputs = SurfaceOutputs::MarketMarkAndRisk;
  ASSERT_TRUE(v2.outputs.has_value());
  EXPECT_TRUE(has_output(*v2.outputs, SurfacePurpose::MarketMark));
  EXPECT_TRUE(has_output(*v2.outputs, SurfacePurpose::Risk));
  EXPECT_EQ(v2.risk_admission, RiskAdmission::Required);
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

// Oracle I-1: Balanced's own grid mismatch is 97 strike points / 65 calendar
// points over the same [-0.5,0.5] band; 64 does not divide 96, so not every
// calendar-grid k also lands on the strike grid. Find one that does not (by
// construction, not by luck), make it the ONLY non-finite sample (everywhere
// else, including the whole strike grid, is finite flat vol), and confirm the
// calendar loop sets NonFinite itself instead of silently skipping it on the
// (pre-fix, false-for-Balanced) assumption "already reported on the strike
// grid".
TEST(RiskSurfaceValidation, CalendarOnlyNonFiniteSampleIsRejectedWithNonFinite) {
  RiskSurfaceValidationConfig config;
  config.strike_grid_points = 97;
  config.calendar_grid_points = 65;
  const auto grid_k = [&](std::uint32_t point, std::uint32_t n) {
    return config.k_min +
           (static_cast<double>(point) / static_cast<double>(n - 1)) *
               (config.k_max - config.k_min);
  };
  double k_star = std::numeric_limits<double>::quiet_NaN();
  for (std::uint32_t ci = 0; ci < config.calendar_grid_points; ++ci) {
    const double kc = grid_k(ci, config.calendar_grid_points);
    bool on_strike_grid = false;
    for (std::uint32_t si = 0; si < config.strike_grid_points; ++si) {
      if (std::fabs(grid_k(si, config.strike_grid_points) - kc) < 1.0e-9) {
        on_strike_grid = true;
        break;
      }
    }
    if (!on_strike_grid) {
      k_star = kc;
      break;
    }
  }
  ASSERT_TRUE(std::isfinite(k_star))
      << "test assumption broken: every Balanced calendar k also lands on the strike grid";

  SyntheticSurface surface{{0.10, 0.50}, Shape::CalendarOnlyNaN};
  surface.calendar_only_k = k_star;
  const auto result = validate_risk_surface(view_of(surface), config);
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->admitted());
  EXPECT_TRUE(has_validation_failure(result->failures, ValidationFailure::NonFinite));
  EXPECT_GT(result->n_non_finite, 0u);
}

// Oracle I-5: the Wing gate (Roger-Lee total-variance slope ceiling) had no
// direct test anywhere.
TEST(RiskSurfaceValidation, ExcessiveWingSlopeTriggersWingFailure) {
  const SyntheticSurface surface{{0.25}, Shape::SteepWing};
  const auto result = validate_risk_surface(view_of(surface));
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->admitted());
  EXPECT_TRUE(has_validation_failure(result->failures, ValidationFailure::Wing));
  EXPECT_GT(result->n_wing_violations, 0u);
  EXPECT_GT(result->max_wing_slope_excess, 0.0);
}

// Oracle I-5: InvalidDomain via non-ascending maturities had no direct test.
TEST(RiskSurfaceValidation, NonAscendingMaturitiesTriggerInvalidDomain) {
  const SyntheticSurface surface{{0.25, 0.10}, Shape::FlatVol};
  const auto result = validate_risk_surface(view_of(surface));
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->admitted());
  EXPECT_TRUE(has_validation_failure(result->failures, ValidationFailure::InvalidDomain));
}

// Oracle I-3: the uniform 97-pt Balanced strike grid (Delta k ~ 0.0104) is far
// coarser than the dense fit's own ATM-clustered node spacing, so a concave
// kink can sit entirely BETWEEN two uniform samples and average away. This
// documents the pre-fix hole LIVE (the undensified view admits despite the
// kink) and proves the fix (node-k densification via `densified_view_of`,
// which is what the real CurveSurface/VolaSession adapters now wire) rejects
// it once the kink's own location is unioned into the sampling grid.
TEST(RiskSurfaceValidation, NodeDensificationCatchesKinkTheUniformGridMisses) {
  RiskSurfaceValidationConfig config;  // Balanced: 97 strike pts, [-0.5,0.5]
  config.strike_grid_points = 97;
  const auto grid_k = [&](std::uint32_t point, std::uint32_t n) {
    return config.k_min +
           (static_cast<double>(point) / static_cast<double>(n - 1)) *
               (config.k_max - config.k_min);
  };
  // Midpoint between two arbitrary interior uniform samples: never coincides
  // with a uniform grid point (uniform points are at integer indices; this
  // is a half-integer index).
  const double kink_k = 0.5 * (grid_k(60, config.strike_grid_points) +
                               grid_k(61, config.strike_grid_points));

  SyntheticSurface surface{{0.25}, Shape::LocalKink};
  surface.kink_k = kink_k;

  const auto coarse = validate_risk_surface(view_of(surface), config);
  ASSERT_TRUE(coarse.has_value());
  EXPECT_TRUE(coarse->admitted())
      << "pre-fix hole did not reproduce: the uniform grid alone already sees the kink";

  const auto densified = validate_risk_surface(densified_view_of(surface), config);
  ASSERT_TRUE(densified.has_value());
  EXPECT_FALSE(densified->admitted());
  EXPECT_TRUE(has_validation_failure(densified->failures, ValidationFailure::Butterfly));
  EXPECT_GT(densified->n_butterfly_violations, 0u);
}

// Oracle I-5: every existing rejection test drives the hand-rolled
// SyntheticSurface callback view; the real make_risk_surface_view(CurveSurface)
// adapter — the one production risk serving actually uses for ConvexDense/SVI
// — had zero rejection tests. A two-slice CurveSurface with a deliberate
// calendar crossing must be rejected through the REAL adapter.
TEST(RiskSurfaceValidation, RealCurveSurfaceAdapterRejectsCalendarCrossing) {
  CurveSurface surface;
  const std::vector<double> k_nodes{-0.5, 0.0, 0.5};
  surface.push(std::make_unique<LinearVarianceCurve>(
      0.10, 100.0, 0.99, k_nodes, std::vector<double>{0.05, 0.04, 0.05}));
  // Longer-dated slice with LOWER total variance than the shorter one above:
  // a direct calendar-arbitrage crossing.
  surface.push(std::make_unique<LinearVarianceCurve>(
      0.50, 100.0, 0.97, k_nodes, std::vector<double>{0.02, 0.02, 0.02}));

  const auto result = validate_risk_surface(make_risk_surface_view(surface));
  ASSERT_TRUE(result.has_value());
  EXPECT_FALSE(result->admitted());
  EXPECT_TRUE(has_validation_failure(result->failures, ValidationFailure::Calendar));
  EXPECT_GT(result->n_calendar_violations, 0u);
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

// Task 2d (carry I5): CarryGap is the one publish-with-Degraded reason — a
// candidate whose only defect is expiries dropped by the carry gate is served
// with the gap surfaced. Combined with any other failure it still rejects.
TEST(RiskSurfaceAdmission, CarryGapOnlyPublishesDegradedWithReasonRetained) {
  ValidationDigest gapped;
  gapped.failures = ValidationFailure::CarryGap;
  const AdmissionDecision decision = decide_risk_surface_admission(
      gapped, FitQualityMode::Balanced, 7, 6, SurfaceFallback::LastKnownGood);
  EXPECT_TRUE(decision.publish_candidate);
  EXPECT_EQ(decision.health.state, SurfaceState::Degraded);
  EXPECT_EQ(decision.health.reasons, ValidationFailure::CarryGap);
  EXPECT_EQ(decision.health.served_generation, 7u);
  EXPECT_TRUE(decision.health.serving_candidate());
  EXPECT_FALSE(decision.health.using_fallback());

  ValidationDigest gapped_and_broken;
  gapped_and_broken.failures =
      ValidationFailure::CarryGap | ValidationFailure::Calendar;
  const AdmissionDecision rejected = decide_risk_surface_admission(
      gapped_and_broken, FitQualityMode::Balanced, 7, 6,
      SurfaceFallback::LastKnownGood);
  EXPECT_FALSE(rejected.publish_candidate);
  EXPECT_TRUE(rejected.health.using_fallback());
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
