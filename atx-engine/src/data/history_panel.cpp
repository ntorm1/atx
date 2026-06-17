#include "atx/engine/data/history_panel.hpp"

#include <vector>

#include "atx/engine/data/adjust.hpp"

namespace atx::engine::data {

std::vector<atx::f64> orats_total_return_close(std::span<const atx::f64> close,
                                               std::span<const atx::f64> cum_return_factor) {
  // Dividends are already folded into cum_return_factor, so the dividend input is 0:
  // adjust_total_return then yields total_return_index == close * cum_return_factor,
  // with the proven gap/NaN policy and return-invariance contract.
  const std::vector<atx::f64> zero_div(close.size(), 0.0);
  AdjustedSeries adj = adjust_total_return(close, cum_return_factor, zero_div);
  return std::move(adj.total_return_index);
}

} // namespace atx::engine::data
