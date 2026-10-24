#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>

#include "pricing/american_greek_availability.hpp"

namespace atx::vol::detail {

// A solved exercise boundary and an active price clamp are both evidence that
// the continuation PDE cannot be used at this sample. The price stays untouched;
// admission concerns only the derivative that consumes the sample.
// A caller using an unfloored no-early-exercise price must instead supply a
// continuation sample: rounding to intrinsic is not evidence of a clamp there.
struct AmericanGreekSample {
  double price = std::numeric_limits<double>::quiet_NaN();
  bool exercised = false;
};

[[nodiscard]] inline AmericanGreekSample american_greek_sample(
    double price, double S, double K, Side side, double internal_boundary,
    bool boundary_required = true) noexcept {
  if (!std::isfinite(price) ||
      (boundary_required && (!(internal_boundary > 0.) || !std::isfinite(internal_boundary))))
    return {};
  const double intrinsic = side == Side::Call ? S-K : K-S;
  const double internal_spot = side == Side::Call ? K : S;
  const bool boundary_exercise = std::isfinite(internal_boundary) &&
                                 internal_spot <= internal_boundary;
  return {price, intrinsic > 0. && (boundary_exercise || price <= intrinsic)};
}

enum class AmericanGreekStencil : std::uint8_t {
  Nonfinite = 0,
  Continuation,
  Exercised,
  Crossing,
};

[[nodiscard]] inline AmericanGreekStencil american_greek_stencil(
    std::initializer_list<AmericanGreekSample> samples) noexcept {
  if (samples.size() == 0) return AmericanGreekStencil::Nonfinite;
  const bool exercised = samples.begin()->exercised;
  bool crossed = false;
  for (const auto &sample : samples) {
    if (!std::isfinite(sample.price)) return AmericanGreekStencil::Nonfinite;
    crossed = crossed || sample.exercised != exercised;
  }
  return crossed ? AmericanGreekStencil::Crossing :
      exercised ? AmericanGreekStencil::Exercised : AmericanGreekStencil::Continuation;
}

// Called after all arithmetic, so refusing gamma cannot poison another formula
// before that formula's own sample evidence is classified. No budget or accuracy
// guarantee is implied by a homogeneous continuation stencil.
inline void apply_american_greek_stencils(AmericanGreeks &g,
    const std::array<AmericanGreekStencil, 8> &stencils, Side side) noexcept {
  for (std::size_t i = 0; i < stencils.size(); ++i) {
    const auto field = static_cast<GreekFieldMask>(std::uint32_t{1} << i);
    if (!has_field(g.requested, field)) continue;
    auto reason = AmericanGreekReason::Nonfinite;
    switch (stencils[i]) {
    case AmericanGreekStencil::Continuation:
      reason = AmericanGreekReason::ComputedEstimate;
      break;
    case AmericanGreekStencil::Exercised:
      g.*kAmericanGreekMembers[i] = i == 0 ? (side == Side::Put ? -1. : 1.) : 0.;
      reason = AmericanGreekReason::Exercised;
      break;
    case AmericanGreekStencil::Crossing:
      reason = AmericanGreekReason::StencilCrossing;
      break;
    case AmericanGreekStencil::Nonfinite:
      break;
    }
    mark_american_greek_fields(g, field, reason);
  }
}

} // namespace atx::vol::detail
