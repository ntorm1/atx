#pragma once

// Independent, read-only risk-surface admission oracle.
//
// The validator consumes a tiny type-erased sampling view instead of a fitter
// implementation. It therefore cannot accidentally trust optimizer status or
// curve-family diagnostics. Adapters are provided for both surface containers;
// synthetic and external surfaces can supply the same five callbacks.

#include <cstddef>
#include <cstdint>
#include <span>

#include "atx/vol/surface_policy.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

class CurveSurface;
class VolaSession;
class VolSurface;

struct RiskSurfaceView {
  using SliceCountFn = std::size_t (*)(const void *) noexcept;
  using MaturityFn = double (*)(const void *, std::size_t) noexcept;
  using TotalVarianceFn = double (*)(const void *, std::size_t, double) noexcept;
  // Writes up to out.size() of `slice`'s own node log-moneyness locations
  // (any order) into `out`; returns the number written. 0 (out untouched) for
  // an adapter/curve family with no discrete node grid (e.g. a parametric
  // eSSVI/SVI/C8 slice) — the validator then samples that slice on the
  // uniform grid alone. Lets the independent validator densify its sampling
  // to the SERVED fit's own knot spacing (oracle finding I-3: the dense
  // fit's ATM-clustered nodes can sit 5-20x closer than the uniform grid,
  // letting a node-level kink alias between samples) without becoming
  // curve-family-aware itself: this is a location HINT the validator still
  // independently evaluates total_variance/price at, never a value it trusts.
  using NodeKsFn = std::size_t (*)(const void *, std::size_t, std::span<double>) noexcept;

  const void *context{};
  SliceCountFn slice_count{};
  MaturityFn maturity{};
  TotalVarianceFn total_variance{};
  NodeKsFn node_ks{};

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
  // Total variance is recovered through price-to-IV bisection for dense price
  // curves; 1e-8 absorbs only solver/projection roundoff (far below a vol tick).
  double calendar_total_variance_tolerance{1.0e-8};
  // Roger Lee's finite-moment asymptotic total-variance slope ceiling.
  double max_abs_wing_total_variance_slope{2.0};
  double wing_slope_tolerance{1.0e-8};
};

namespace detail {
// The oracle's inclusive uniform sample formula, exported so the producer-side
// strict-recovery path can repair on the same SOURCE expression, in the same
// order, that the oracle evaluates. Fraction first — reordering the arithmetic
// can move a sample by an ulp, so both sides of the producer/oracle contract
// must agree on expression order. This is not a bit-for-bit guarantee across
// translation units or compiler flags (e.g. FMA contraction can still shift an
// individual sample by an ulp); the strict-recovery caller's tolerance margin
// (0.1x this config's, ConvexRepairSpec::tolerance) and exact-node promotion
// (ConvexRepairSpec::extra_node_ks) are what actually absorb that drift.
[[nodiscard]] inline double validation_grid_k(const RiskSurfaceValidationConfig &config,
                                              std::uint32_t point,
                                              std::uint32_t n_points) noexcept {
  const double fraction = static_cast<double>(point) / static_cast<double>(n_points - 1u);
  return config.k_min + fraction * (config.k_max - config.k_min);
}
} // namespace detail

[[nodiscard]] RiskSurfaceView make_risk_surface_view(const CurveSurface &surface) noexcept;
[[nodiscard]] RiskSurfaceView make_risk_surface_view(const VolaSession &surface) noexcept;
[[nodiscard]] RiskSurfaceView make_risk_surface_view(const VolSurface &surface) noexcept;

// Invalid view/configuration is a caller error. A mathematically invalid
// candidate is a successful call carrying a non-admitted ValidationDigest.
[[nodiscard]] Result<ValidationDigest>
validate_risk_surface(RiskSurfaceView surface, const RiskSurfaceValidationConfig &config = {});

// Recompute the deterministic ID after an admission layer merges independent
// carry, inversion, freshness, or timeout failures into the geometric digest.
void finalize_validation_digest(ValidationDigest &digest,
                                const RiskSurfaceValidationConfig &config = {}) noexcept;

[[nodiscard]] inline Result<ValidationDigest>
validate_risk_surface(const CurveSurface &surface, const RiskSurfaceValidationConfig &config = {}) {
  return validate_risk_surface(make_risk_surface_view(surface), config);
}

[[nodiscard]] inline Result<ValidationDigest>
validate_risk_surface(const VolaSession &surface, const RiskSurfaceValidationConfig &config = {}) {
  return validate_risk_surface(make_risk_surface_view(surface), config);
}

[[nodiscard]] inline Result<ValidationDigest>
validate_risk_surface(const VolSurface &surface, const RiskSurfaceValidationConfig &config = {}) {
  return validate_risk_surface(make_risk_surface_view(surface), config);
}

} // namespace atx::vol
