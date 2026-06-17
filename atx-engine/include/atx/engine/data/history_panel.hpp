#pragma once

// atx::engine::data — ORATS history panel helpers (p3 S3-3+).
//
// orats_total_return_close: canonical total-return adjusted close for a single
// symbol derived from ORATS history data. Computed as close * cumulReturnFactor
// by delegating to adjust_total_return with zero cash dividends (dividends are
// already folded into cumulReturnFactor by ORATS). Inherits the proven NaN/gap
// policy from adjust_total_return: a NaN close OR NaN factor is a gap (NaN),
// never zero-filled.

#include <span>
#include <vector>

#include "atx/core/types.hpp"

namespace atx::engine::data {

// Canonical total-return adjusted close = close * cum_return_factor, computed by
// reusing adjust_total_return(close, cum_return_factor, zeros): its
// total_return_index equals close*cum_return_factor exactly (dividends are already
// folded into cum_return_factor, so the dividend input is 0). NaN policy inherited
// from adjust_total_return: a NaN close OR NaN factor is a gap (NaN), never 0.
// The two spans must be equal length (one symbol, ascending by date).
[[nodiscard]] std::vector<atx::f64>
orats_total_return_close(std::span<const atx::f64> close,
                         std::span<const atx::f64> cum_return_factor);

} // namespace atx::engine::data
