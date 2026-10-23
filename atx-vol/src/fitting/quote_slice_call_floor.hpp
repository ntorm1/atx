#pragma once

// Internal seam (lane C3): the opt-in served call floor (QuoteSliceCallFloor). Called from
// quote_slice.cpp at two points only - prepare_fit (select_call_floor) and certify_and_serve
// (lift_to_call_floor, before the first certificate). Definitions live in
// quote_slice_call_floor.cpp. Not public API.

#include <cstdint>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"

namespace atx::vol::detail {

// Validates `config` and records on `fit.diagnostics` the lines this expiry is held to:
// every line whose calendar_years is at most fit.market.calendar_years (fit.market must be
// set). InvalidArgument for an unknown mode, Legacy with lines, or an invalid line.
[[nodiscard]] Status select_call_floor(QuoteSliceFit &fit, const QuoteSliceCallFloorConfig &config);

// The shared floor at `strike`: the largest spot - strike * discount over `lines`, and 0
// when there are none (or all are negative).
[[nodiscard]] double call_floor_at(std::span<const QuoteSliceCallFloorLine> lines,
                                   double strike) noexcept;

// Lifts every call node more than the certificate allowance below the expiry's floor onto
// it, when the lifted nodes pass AmericanPriceSlice::create(`bounds`); otherwise leaves
// `nodes` unchanged and sets fit.diagnostics.call_floor_refused. Returns the lifted mask
// (1 = call, one entry per node) or an empty vector when nothing moved. A fit without lines
// returns empty at once, without touching `nodes`.
[[nodiscard]] std::vector<std::uint8_t> lift_to_call_floor(QuoteSliceFit &fit,
                                                           std::vector<AmericanPriceNode> &nodes,
                                                           const AmericanPriceBounds &bounds);

} // namespace atx::vol::detail
