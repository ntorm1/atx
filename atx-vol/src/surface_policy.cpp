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
