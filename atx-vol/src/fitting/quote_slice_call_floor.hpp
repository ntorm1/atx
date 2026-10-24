#pragma once

// Internal seam (lane C3): the opt-in served call floor (QuoteSliceCallFloor). Called from
// quote_slice.cpp at two points only - prepare_fit (select_call_floor) and certify_and_serve
// (lift_to_call_floor, on the certified slice). Definitions live in
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

// Lifts every call node of the CERTIFIED slice (after certify_and_serve's own repairs) more
// than the certificate allowance below the expiry's floor onto it, when the lifted nodes pass
// AmericanPriceSlice::create with the slice's bounds; then replaces `certified` and ORs 1
// (call) into `moved` (sized to the nodes on first use) at every lifted node. Otherwise leaves
// both unchanged and sets fit.diagnostics.call_floor_refused. A fit without lines returns at
// once.
void lift_to_call_floor(QuoteSliceFit &fit, AmericanPriceSlice &certified,
                        std::vector<std::uint8_t> &moved);

} // namespace atx::vol::detail
