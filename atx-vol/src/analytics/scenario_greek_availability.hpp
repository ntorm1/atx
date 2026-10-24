#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "atx/vol/api/analytics/scenario_grid.hpp"
#include "pricing/american_greek_availability.hpp"

namespace atx::vol::detail {

// Derivatives actually read by the existing second-order Taylor term set.
// Price is not an input. Exact zero shocks need no derivative estimate.
[[nodiscard]] inline GreekFieldMask scenario_taylor_fields(double dS, double dvol, double dt,
                                                           double dr) noexcept {
  auto fields = GreekFieldMask::None;
  if (dS != 0.)
    fields = fields | GreekFieldMask::Delta | GreekFieldMask::Gamma;
  if (dvol != 0.)
    fields = fields | GreekFieldMask::Vega | GreekFieldMask::Volga;
  if (dt != 0.)
    fields = fields | GreekFieldMask::Theta;
  if (dr != 0.)
    fields = fields | GreekFieldMask::Rho;
  if (dS != 0. && dvol != 0.)
    fields = fields | GreekFieldMask::Vanna;
  if (dS != 0. && dt != 0.)
    fields = fields | GreekFieldMask::Charm;
  return fields;
}

// Caller first checks supply for scenario_taylor_fields. Clear only nonfinite
// inactive operands so NaN*0 cannot poison an otherwise supported scenario.
// Finite operands and the public kernel's arithmetic order remain unchanged.
[[nodiscard]] inline double scenario_taylor_active_leg(const AmericanGreeks &g, double dS,
                                                       double dvol, double dt, double dr) noexcept {
  const auto fields = scenario_taylor_fields(dS, dvol, dt, dr);
  AmericanGreeks operands = g;
  for (std::size_t i = 0; i < 8u; ++i) {
    const auto field = static_cast<GreekFieldMask>(std::uint32_t{1} << i);
    double &value = operands.*kAmericanGreekMembers[i];
    if (!has_field(fields, field) && !std::isfinite(value))
      value = 0.;
  }
  return scenario_taylor_leg(operands, dS, dvol, dt, dr);
}

} // namespace atx::vol::detail
