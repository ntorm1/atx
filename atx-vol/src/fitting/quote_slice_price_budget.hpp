#pragma once

#include <cmath>

#include "atx/vol/api/fitting/quote_slice.hpp"

namespace atx::vol::detail {

// Shared by direct fitting and policy catalogs, so an invalid opt-in contract is
// rejected before profile resolution or any fitting/output work.
[[nodiscard]] inline bool valid_reimply_price_budget(const QuoteSliceConfig &cfg) noexcept {
  const double cap = cfg.reimply_price_cap;
  const double fraction = cfg.reimply_spread_fraction;
  return std::isfinite(cap) && cap >= 0.0 && std::isfinite(fraction) && fraction >= 0.0 &&
      ((cap > 0.0) == (fraction > 0.0)) &&
      (cap == 0.0 || (cfg.floor_policy == QuoteSliceFloorPolicy::PriceOnly &&
                     cfg.dividend_model != QuoteSliceDividendModel::DatedCash));
}

} // namespace atx::vol::detail
