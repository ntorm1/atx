#pragma once

#include <cmath>

#include "atx/vol/api/backtest/portfolio_pricer.hpp"
#include "pricing/american_greek_availability.hpp"

namespace atx::vol::detail {

// Aggregate availability is independent of row coverage: every contributing
// row may be complete while its sum overflows. Price means the priced-row PV.
[[nodiscard]] inline bool price_totals_support_request(const PriceTotals &t) noexcept {
  if (!has_field(t.requested, GreekFieldMask::Price)) return false;
  AmericanGreeks g{t.delta, t.gamma, t.vega, t.theta, t.rho,
                   t.vanna, t.volga, t.charm, t.pv};
  g.requested = t.requested;
  g.supported = t.supported;
  g.reasons = t.reasons;
  return american_greeks_support(g, t.requested) &&
         (!has_field(t.requested, GreekFieldMask::Vega) ||
          (std::isfinite(t.abs_vega) && t.abs_vega >= 0.0));
}

} // namespace atx::vol::detail
