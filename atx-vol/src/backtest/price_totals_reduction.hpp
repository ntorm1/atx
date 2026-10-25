#pragma once

// Shared deterministic accumulation for priced portfolio rows and carried marks.
// Availability is independent of price coverage; exact zero positions contribute
// zero risk without inventing per-share supply. Callers initialize fresh totals.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "pricing/american_greek_availability.hpp"

namespace atx::vol::detail {

inline constexpr std::array<double PriceTotals::*, 8> kTotalGreekMembers{
    &PriceTotals::delta, &PriceTotals::gamma, &PriceTotals::vega,  &PriceTotals::theta,
    &PriceTotals::rho,   &PriceTotals::vanna, &PriceTotals::volga, &PriceTotals::charm};

inline void initialize_price_totals(PriceTotals &t, GreekFieldMask requested) noexcept {
  t.requested = t.supported = requested;
  for (std::size_t i = 0; i < t.reasons.size(); ++i) {
    const auto field = static_cast<GreekFieldMask>(std::uint32_t{1} << i);
    t.reasons[i] = has_field(requested, field) ? AmericanGreekReason::ComputedEstimate
                                               : AmericanGreekReason::NotRequested;
  }
  if (requested == GreekFieldMask::Price) {
    for (const auto member : kTotalGreekMembers)
      t.*member = std::numeric_limits<double>::quiet_NaN();
  } else {
    t.abs_vega =
        has_field(requested, GreekFieldMask::Vega) ? 0.0 : std::numeric_limits<double>::quiet_NaN();
  }
}

inline void refuse_total_field(PriceTotals &t, std::size_t i, AmericanGreekReason reason) noexcept {
  if (i < kTotalGreekMembers.size())
    t.*kTotalGreekMembers[i] = std::numeric_limits<double>::quiet_NaN();
  else
    t.pv = std::numeric_limits<double>::quiet_NaN();
  // Signed and gross Vega share one availability bit. Neither may remain a
  // plausible finite exposure when the other has become unrepresentable.
  if (i == 2u)
    t.abs_vega = std::numeric_limits<double>::quiet_NaN();
  t.supported = static_cast<GreekFieldMask>(static_cast<std::uint32_t>(t.supported) &
                                            ~(std::uint32_t{1} << i));
  t.reasons[i] =
      detail::american_greek_supplied_reason(reason) || reason == AmericanGreekReason::NotRequested
          ? AmericanGreekReason::Nonfinite
          : reason;
}

inline void add_priced_row(PriceTotals &t, const AmericanGreeks &g, double pv, PriceStatus status,
                           bool zero_weight) noexcept {
  t.pv += pv;
  if (!std::isfinite(t.pv))
    refuse_total_field(t, 8u, AmericanGreekReason::Nonfinite);
  for (std::size_t i = 0; i < kTotalGreekMembers.size(); ++i) {
    const auto field = static_cast<GreekFieldMask>(std::uint32_t{1} << i);
    if (!has_field(t.requested, field))
      continue;
    double &sum = t.*kTotalGreekMembers[i];
    // An exactly zero position contributes exact zero without requiring the
    // unavailable per-share derivative. Its row still reports the refusal.
    const double contribution = zero_weight ? 0.0 : g.*detail::kAmericanGreekMembers[i];
    if ((!zero_weight && !detail::american_greeks_support(g, field)) ||
        !std::isfinite(contribution)) {
      if (has_field(t.supported, field))
        refuse_total_field(t, i, g.reasons[i]);
      sum = std::numeric_limits<double>::quiet_NaN();
    } else {
      sum += contribution;
      if (!std::isfinite(sum) && has_field(t.supported, field))
        refuse_total_field(t, i, AmericanGreekReason::Nonfinite);
    }
    if (field == GreekFieldMask::Vega) {
      t.abs_vega = has_field(t.supported, field) ? t.abs_vega + std::fabs(contribution)
                                                 : std::numeric_limits<double>::quiet_NaN();
      if (!std::isfinite(t.abs_vega) && has_field(t.supported, field))
        refuse_total_field(t, i, AmericanGreekReason::Nonfinite);
    }
  }
  ++t.n_priced;
  if (status == PriceStatus::Ok)
    ++t.n_ok;
  else
    ++t.n_greeks_unavailable;
}

} // namespace atx::vol::detail
