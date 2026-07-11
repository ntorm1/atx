#pragma once

// Independent, read-only risk-surface admission oracle.
//
// The validator consumes a tiny type-erased sampling view instead of a fitter
// implementation. It therefore cannot accidentally trust optimizer status or
// curve-family diagnostics. Adapters are provided for both surface containers;
// synthetic and external surfaces can supply the same five callbacks.

#include <cstddef>
#include <cstdint>

#include "atx/vol/surface_policy.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

class CurveSurface;
class VolSurface;

struct RiskSurfaceView {
  using SliceCountFn = std::size_t (*)(const void *) noexcept;
  using MaturityFn = double (*)(const void *, std::size_t) noexcept;
  using TotalVarianceFn = double (*)(const void *, std::size_t, double) noexcept;

  const void *context{};
  SliceCountFn slice_count{};
  MaturityFn maturity{};
  TotalVarianceFn total_variance{};

  [[nodiscard]] constexpr bool valid() const noexcept {
    return context != nullptr && slice_count != nullptr && maturity != nullptr &&
           total_variance != nullptr;
  }
};

struct RiskSurfaceValidationConfig {
  // Declared risk band in forward log-moneyness, sampled inclusively.
  double k_min{-0.50};
  double k_max{0.50};
  std::uint32_t strike_grid_points{129};
  std::uint32_t calendar_grid_points{129};

  // Numerical tolerances are non-negative slacks, not switches. A zero value is
  // valid and strict; invalid/non-finite values reject the configuration.
  double price_bound_tolerance{1.0e-10};
  double strike_monotonicity_tolerance{1.0e-10};
  double convexity_slope_tolerance{1.0e-8};
  double calendar_total_variance_tolerance{1.0e-12};
  // Roger Lee's finite-moment asymptotic total-variance slope ceiling.
  double max_abs_wing_total_variance_slope{2.0};
  double wing_slope_tolerance{1.0e-8};
};

[[nodiscard]] RiskSurfaceView make_risk_surface_view(const CurveSurface &surface) noexcept;
[[nodiscard]] RiskSurfaceView make_risk_surface_view(const VolSurface &surface) noexcept;

// Invalid view/configuration is a caller error. A mathematically invalid
// candidate is a successful call carrying a non-admitted ValidationDigest.
[[nodiscard]] Result<ValidationDigest>
validate_risk_surface(RiskSurfaceView surface, const RiskSurfaceValidationConfig &config = {});

[[nodiscard]] inline Result<ValidationDigest>
validate_risk_surface(const CurveSurface &surface, const RiskSurfaceValidationConfig &config = {}) {
  return validate_risk_surface(make_risk_surface_view(surface), config);
}

[[nodiscard]] inline Result<ValidationDigest>
validate_risk_surface(const VolSurface &surface, const RiskSurfaceValidationConfig &config = {}) {
  return validate_risk_surface(make_risk_surface_view(surface), config);
}

} // namespace atx::vol
