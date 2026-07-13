#include "atx/vol/surface_policy.hpp"

namespace atx::vol {

AdmissionDecision decide_risk_surface_admission(const ValidationDigest &validation,
                                                FitQualityMode quality_mode,
                                                std::uint64_t candidate_generation,
                                                std::uint64_t last_admitted_generation,
                                                SurfaceFallback fallback) noexcept {
  AdmissionDecision out;
  out.health.purpose = SurfacePurpose::Risk;
  out.health.quality_mode = quality_mode;
  out.health.candidate_generation = candidate_generation;
  out.health.validation = validation;
  out.health.reasons = validation.failures;

  if (candidate_generation != 0 && validation.admitted()) {
    out.publish_candidate = true;
    out.health.state = SurfaceState::Healthy;
    out.health.served_generation = candidate_generation;
    out.health.reasons = ValidationFailure::None;
    return out;
  }

  // Degraded-but-served: a candidate whose ONLY defect is a carry-coverage
  // gap (expiries dropped by the carry confidence gate) is published with the
  // gap surfaced — Degraded state, CarryGap reason retained — never silently
  // as Healthy and never rejected outright: the surviving slices passed the
  // full geometric/certification contract (§5.2). Any other failure bit still
  // rejects below.
  {
    constexpr auto kDegradedOnly =
        static_cast<std::uint32_t>(ValidationFailure::CarryGap);
    const auto raw = static_cast<std::uint32_t>(validation.failures);
    if (candidate_generation != 0 && raw != 0u && (raw & ~kDegradedOnly) == 0u) {
      out.publish_candidate = true;
      out.health.state = SurfaceState::Degraded;
      out.health.served_generation = candidate_generation;
      return out;
    }
  }

  if (candidate_generation == 0) {
    out.health.reasons |= ValidationFailure::InvalidDomain;
  }

  if (fallback == SurfaceFallback::LastKnownGood && last_admitted_generation != 0) {
    out.health.state = SurfaceState::Degraded;
    out.health.served_generation = last_admitted_generation;
    out.health.fallback_generation = last_admitted_generation;
  } else {
    out.health.state = SurfaceState::Rejected;
  }
  return out;
}

} // namespace atx::vol
