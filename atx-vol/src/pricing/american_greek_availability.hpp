#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol::detail {

inline constexpr std::array<double AmericanGreeks::*, 9> kAmericanGreekMembers{
    &AmericanGreeks::delta, &AmericanGreeks::gamma, &AmericanGreeks::vega,
    &AmericanGreeks::theta, &AmericanGreeks::rho, &AmericanGreeks::vanna,
    &AmericanGreeks::volga, &AmericanGreeks::charm, &AmericanGreeks::price};

[[nodiscard]] constexpr GreekFieldMask american_greek_request(
    bool need_vega, bool need_rho, bool need_charm) noexcept {
  auto fields = GreekFieldMask::Price | GreekFieldMask::Delta |
                GreekFieldMask::Gamma | GreekFieldMask::Theta;
  if (need_vega) fields = fields | GreekFieldMask::Vega | GreekFieldMask::Vanna | GreekFieldMask::Volga;
  if (need_rho) fields = fields | GreekFieldMask::Rho;
  if (need_charm) fields = fields | GreekFieldMask::Charm;
  return fields;
}

[[nodiscard]] constexpr bool american_greek_supplied_reason(AmericanGreekReason reason) noexcept {
  return reason == AmericanGreekReason::ComputedEstimate || reason == AmericanGreekReason::Exercised;
}

// Producer assertion, not a numerical accuracy check. Refusal poisons only the
// selected fields; the other values/availability (including price) are retained.
inline void mark_american_greek_fields(AmericanGreeks &g, GreekFieldMask fields,
                                     AmericanGreekReason reason) noexcept {
  auto requested = static_cast<std::uint32_t>(g.requested);
  auto supported = static_cast<std::uint32_t>(g.supported);
  const auto selected = static_cast<std::uint32_t>(fields);
  for (std::size_t i = 0; i < kAmericanGreekMembers.size(); ++i) {
    const auto bit = std::uint32_t{1} << i;
    if ((selected & bit) == 0u) continue;
    supported &= ~bit;
    if (reason == AmericanGreekReason::NotRequested) {
      requested &= ~bit;
      g.reasons[i] = reason;
      continue;
    }
    requested |= bit;
    double &value = g.*kAmericanGreekMembers[i];
    const bool supplied = american_greek_supplied_reason(reason);
    g.reasons[i] = supplied && !std::isfinite(value) ? AmericanGreekReason::Nonfinite : reason;
    if (supplied && std::isfinite(value)) {
      supported |= bit;
    } else {
      value = std::numeric_limits<double>::quiet_NaN();
    }
  }
  g.requested = static_cast<GreekFieldMask>(requested);
  g.supported = static_cast<GreekFieldMask>(supported);
}

// Narrows metadata without altering legacy unrequested numeric slots. Expanding
// a request cannot resurrect a value: newly requested fields become unsupported.
inline void narrow_american_greek_request(AmericanGreeks &g, GreekFieldMask requested) noexcept {
  for (std::size_t i = 0; i < kAmericanGreekMembers.size(); ++i) {
    const auto field = static_cast<GreekFieldMask>(std::uint32_t{1} << i);
    if (!has_field(requested, field)) {
      mark_american_greek_fields(g, field, AmericanGreekReason::NotRequested);
    } else if (!has_field(g.requested, field)) {
      mark_american_greek_fields(g, field, AmericanGreekReason::UnsupportedAxis);
    }
  }
}

// Exact-field consumption gate. Coarse solve needs must be converted to a mask
// at the caller; a requested delta does not require an unrequested theta.
[[nodiscard]] inline bool american_greeks_support(const AmericanGreeks &g,
                                                 GreekFieldMask required) noexcept {
  constexpr auto all = static_cast<std::uint32_t>(GreekFieldMask::All);
  const auto need = static_cast<std::uint32_t>(required);
  const auto requested = static_cast<std::uint32_t>(g.requested);
  const auto supported = static_cast<std::uint32_t>(g.supported);
  if (((need | requested | supported) & ~all) != 0u ||
      (supported & ~requested) != 0u || (requested & need) != need ||
      (supported & need) != need) return false;
  for (std::size_t i = 0; i < kAmericanGreekMembers.size(); ++i) {
    if ((need & (std::uint32_t{1} << i)) != 0u &&
        (!american_greek_supplied_reason(g.reasons[i]) ||
         !std::isfinite(g.*kAmericanGreekMembers[i]))) return false;
  }
  return true;
}

} // namespace atx::vol::detail
