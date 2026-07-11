#pragma once

// Product-level policy and validation vocabulary for volatility surfaces.
//
// Correctness is deliberately not represented as a quality mode. Latency,
// Balanced, and Accuracy may change how a candidate is built, but every Risk
// candidate must pass the same independent admission contract before it can be
// published. These types are fixed-size values so they can also be persisted in
// archives and telemetry without coupling those layers to a concrete fitter.

#include <cstdint>
#include <string_view>

#include "atx/vol/session.hpp" // FitPreset (one-release compatibility mapping)

namespace atx::vol {

enum class SurfacePurpose : std::uint8_t {
  MarketMark = 0,
  Risk = 1,
};

enum class FitQualityMode : std::uint8_t {
  Latency = 0,
  Balanced = 1,
  Accuracy = 2,
};

enum class SurfaceState : std::uint8_t {
  Healthy = 0,
  Degraded = 1,
  Stale = 2,
  Rejected = 3,
};

enum class SurfaceOutputs : std::uint8_t {
  MarketMark = 1u << 0,
  Risk = 1u << 1,
  MarketMarkAndRisk = (1u << 0) | (1u << 1),
};

[[nodiscard]] constexpr SurfaceOutputs operator|(SurfaceOutputs lhs, SurfaceOutputs rhs) noexcept {
  return static_cast<SurfaceOutputs>(static_cast<std::uint8_t>(lhs) |
                                     static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr bool has_output(SurfaceOutputs outputs, SurfacePurpose purpose) noexcept {
  const auto bit =
      purpose == SurfacePurpose::MarketMark ? SurfaceOutputs::MarketMark : SurfaceOutputs::Risk;
  return (static_cast<std::uint8_t>(outputs) & static_cast<std::uint8_t>(bit)) != 0u;
}

// There is intentionally no "Optional" or "Disabled" value for risk. A caller
// may request only a MarketMark output, but once Risk is requested its admission
// is mandatory.
enum class RiskAdmission : std::uint8_t {
  NotApplicable = 0,
  Required = 1,
};

enum class SurfaceFallback : std::uint8_t {
  None = 0,
  LastKnownGood = 1,
};

enum class ValidationFailure : std::uint32_t {
  None = 0,
  InvalidDomain = 1u << 0,
  NonFinite = 1u << 1,
  PriceBounds = 1u << 2,
  StrikeMonotonicity = 1u << 3,
  Butterfly = 1u << 4,
  Calendar = 1u << 5,
  Wing = 1u << 6,
  InversionResidual = 1u << 7,
  TimedOut = 1u << 8,
  StaleInput = 1u << 9,
  InsufficientData = 1u << 10,
};

[[nodiscard]] constexpr ValidationFailure operator|(ValidationFailure lhs,
                                                    ValidationFailure rhs) noexcept {
  return static_cast<ValidationFailure>(static_cast<std::uint32_t>(lhs) |
                                        static_cast<std::uint32_t>(rhs));
}

constexpr ValidationFailure &operator|=(ValidationFailure &lhs, ValidationFailure rhs) noexcept {
  lhs = lhs | rhs;
  return lhs;
}

[[nodiscard]] constexpr bool has_validation_failure(ValidationFailure failures,
                                                    ValidationFailure flag) noexcept {
  return (static_cast<std::uint32_t>(failures) & static_cast<std::uint32_t>(flag)) != 0u;
}

// Fixed-size result produced by the independent oracle. Counts are exhaustive
// over the declared validation grid; maxima are the worst positive constraint
// breaches (zero when that constraint passed). validation_id is a deterministic
// hash of the contract and observed result, suitable for replay comparisons.
struct ValidationDigest {
  ValidationFailure failures{ValidationFailure::None};
  std::uint64_t validation_id{};
  std::uint32_t n_slices{};
  std::uint32_t n_strike_samples{};
  std::uint32_t n_calendar_samples{};
  std::uint32_t n_non_finite{};
  std::uint32_t n_price_bound_violations{};
  std::uint32_t n_strike_monotonicity_violations{};
  std::uint32_t n_butterfly_violations{};
  std::uint32_t n_calendar_violations{};
  std::uint32_t n_wing_violations{};
  double max_price_bound_slack{};
  double max_strike_monotonicity_slack{};
  double max_butterfly_slack{};
  double max_calendar_slack{};
  double max_wing_slope_excess{};

  [[nodiscard]] constexpr bool admitted() const noexcept {
    return failures == ValidationFailure::None;
  }
};

struct SurfacePolicy {
  FitQualityMode quality_mode{FitQualityMode::Balanced};
  SurfaceOutputs outputs{SurfaceOutputs::MarketMarkAndRisk};
  RiskAdmission risk_admission{RiskAdmission::Required};
  SurfaceFallback fallback{SurfaceFallback::LastKnownGood};

  [[nodiscard]] constexpr bool requests(SurfacePurpose purpose) const noexcept {
    return has_output(outputs, purpose);
  }
};

// Operational state for the surface generation consumers are actually
// serving. candidate_generation identifies the attempted build; served_generation
// remains the prior admitted generation when the candidate is rejected.
struct SurfaceHealth {
  SurfacePurpose purpose{SurfacePurpose::Risk};
  FitQualityMode quality_mode{FitQualityMode::Balanced};
  SurfaceState state{SurfaceState::Rejected};
  ValidationFailure reasons{ValidationFailure::InsufficientData};
  std::uint64_t candidate_generation{};
  std::uint64_t served_generation{};
  std::uint64_t fallback_generation{};
  std::int64_t surface_age_ns{};
  ValidationDigest validation{};

  [[nodiscard]] constexpr bool serving_candidate() const noexcept {
    return candidate_generation != 0 && served_generation == candidate_generation;
  }
  [[nodiscard]] constexpr bool using_fallback() const noexcept {
    return fallback_generation != 0 && served_generation == fallback_generation;
  }
};

struct AdmissionDecision {
  bool publish_candidate{false};
  SurfaceHealth health{};
};

// Pure publication decision. It never mutates or owns a surface, making it
// usable by the fitter, database replay, and UI shadow pipeline alike.
[[nodiscard]] AdmissionDecision
decide_risk_surface_admission(const ValidationDigest &validation, FitQualityMode quality_mode,
                              std::uint64_t candidate_generation,
                              std::uint64_t last_admitted_generation = 0,
                              SurfaceFallback fallback = SurfaceFallback::LastKnownGood) noexcept;

struct LegacyPresetMapping {
  FitQualityMode quality_mode{FitQualityMode::Balanced};
  SurfacePurpose purpose{SurfacePurpose::Risk};
};

// One-release compatibility seam. Hft is a market-mark request, never an
// implicit risk request. Other legacy presets retain their intended work budget
// while inheriting mandatory independent risk admission.
[[nodiscard]] constexpr LegacyPresetMapping map_legacy_fit_preset(FitPreset preset) noexcept {
  switch (preset) {
  case FitPreset::Hft:
    return {FitQualityMode::Latency, SurfacePurpose::MarketMark};
  case FitPreset::Fast:
    return {FitQualityMode::Latency, SurfacePurpose::Risk};
  case FitPreset::Accurate:
    return {FitQualityMode::Accuracy, SurfacePurpose::Risk};
  case FitPreset::Robust:
  default:
    return {FitQualityMode::Balanced, SurfacePurpose::Risk};
  }
}

[[nodiscard]] constexpr std::string_view to_string(SurfacePurpose value) noexcept {
  return value == SurfacePurpose::MarketMark ? "market_mark" : "risk";
}

[[nodiscard]] constexpr std::string_view to_string(FitQualityMode value) noexcept {
  switch (value) {
  case FitQualityMode::Latency:
    return "latency";
  case FitQualityMode::Accuracy:
    return "accuracy";
  case FitQualityMode::Balanced:
  default:
    return "balanced";
  }
}

[[nodiscard]] constexpr std::string_view to_string(SurfaceState value) noexcept {
  switch (value) {
  case SurfaceState::Healthy:
    return "healthy";
  case SurfaceState::Degraded:
    return "degraded";
  case SurfaceState::Stale:
    return "stale";
  case SurfaceState::Rejected:
  default:
    return "rejected";
  }
}

} // namespace atx::vol
