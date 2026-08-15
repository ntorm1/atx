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

#include "atx/vol/api/fitting/session.hpp" // FitPreset (one-release compatibility mapping)

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
  // One or more expiries were dropped from the fitted surface — their carry
  // failed its confidence gate, or the fit-inversion audit starved the slice
  // below the usable-observation floor (§5.2: uncertain inputs are surfaced,
  // not hidden). Unlike every other bit, admission PUBLISHES a candidate whose
  // only defect is this gap — as Degraded, with this reason retained — because
  // the remaining slices passed the full contract and refusing them would
  // serve nothing. New bit value: persisted digests written before it never
  // carry it, so archives remain forward-compatible (the archive/db known-
  // failure masks include it).
  CarryGap = 1u << 11,
  // T7a (stage 3, pre-registered): the candidate is a validation-fallback
  // SUBSTITUTE that measurably under-serves the primary it replaced — on the
  // COMMON quote population (FallbackComparisonRecord, pricer_fitter.hpp) it
  // reprices strictly fewer quotes inside bid/ask than the oracle-rejected
  // primary did. The oracle's rejection of the primary stands (a rejected
  // primary is never served); this bit keeps the substitution honest instead
  // of silent. Measured (lqbench+sp100, robust/production): 143/179 adopted
  // substitutes were worse on common support, median in-band loss 6.2%, and
  // 75.9% of the substitutes whose own-support min "improved" did NOT improve
  // on common support — the min was gamed by fitting less. Like CarryGap, and
  // only like CarryGap, this is a publish-with-Degraded reason: alone (or
  // with CarryGap) it demotes, never rejects — refusing the substitute
  // outright would serve nothing where something honest can serve. New bit
  // value: persisted digests written before it never carry it (archive/db
  // known-failure masks extended in lockstep).
  SubstituteUnderserve = 1u << 12,
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
  double first_non_finite_k{};
  std::uint32_t first_non_finite_slice{};
  double first_butterfly_k{};
  std::uint32_t first_butterfly_slice{};
  double first_calendar_k{};
  std::uint32_t first_calendar_long_slice{};
  double first_butterfly_slope_left{};
  double first_butterfly_slope_right{};
  double first_calendar_previous_w{};
  double first_calendar_current_w{};

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
  case FitPreset::Bulk: // Perf 2b: same quality contract as Populate, cheaper AL rung
  case FitPreset::Populate:
    // C3 bulk-populate tier (F3): a Robust-GRADE risk request — Balanced quality
    // mode (MonotoneFit, parity, audited inversions, carry confidence) with the
    // cheaper Andersen-Lake de-Am preset honored downstream via the preset-keyed
    // tier in PricerFitter::apply_risk_policy (F1). Made explicit so it no longer
    // silently falls through `default:`; the quality contract is deliberate, not
    // an accident of the fall-through.
    return {FitQualityMode::Balanced, SurfacePurpose::Risk};
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

// The half-band (absolute log-forward-moneyness) the independent risk
// validation oracle actually certifies a candidate built at `mode` over --
// the k_max a RiskSurfaceValidationConfig for that mode carries
// (risk_validation_config, pricer_fitter.cpp). Latency trades a narrower
// certified band for build speed; Balanced is the strip's own mode-blind
// default (strip::kCertifiedWingHalfBand, detail/strip_grid.hpp); Accuracy
// widens it. FIT-C7: a Latency-mode surface priced under the mode-blind
// default read [0.35, 0.5] as certified when nothing certified it there --
// a caller pricing a PricedSurface/SurfaceRef whose build quality mode it
// knows should resolve the wing-trust band through THIS function and pass
// the result as `var_swap_fair_strike`'s (etc.) `surface_certified_wing_band`
// argument, rather than let the strip fall back to the mode-blind constant.
//
// NOT compile-time linked to `risk_validation_config`: pricer_fitter.cpp owns
// the fit-time k_min/k_max literals, this owns the pricing-time copy of them,
// and only the Balanced case is statically cross-checked (derivatives.cpp,
// against `strip::kCertifiedWingHalfBand`). Update both switches together if
// `risk_validation_config`'s per-mode k_max ever moves.
[[nodiscard]] constexpr double certified_wing_half_band(FitQualityMode mode) noexcept {
  switch (mode) {
  case FitQualityMode::Latency:
    return 0.35;
  case FitQualityMode::Accuracy:
    return 0.60;
  case FitQualityMode::Balanced:
  default:
    return 0.50;
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
