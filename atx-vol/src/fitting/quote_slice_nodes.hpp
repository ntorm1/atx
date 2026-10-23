#pragma once

// Internal seam (lanes L3a / L3a-b / C0): the certified node side of a contract, the
// price-only flag of a node side, the served-volatility floor rule and the
// identification of a served volatility. Shared by quote_slice.cpp (row publication,
// evaluate/query) and quote_surface.cpp (the ladder wing tail). Definitions live in
// quote_slice.cpp. Not public API.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "atx/vol/api/core/types.hpp" // Side
#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"

namespace atx::vol::detail {

// A contract's node side in a certified price slice: the node index and the side bit
// (1 call, 2 put) of QuoteSliceFit::moved_nodes and QuoteSliceFit::price_only_nodes.
struct NodeSide {
  std::size_t node = 0;
  std::uint8_t bit = 0;
};

// The node side of (strike, side) among `nodes` (sorted by strike, as
// AmericanPriceSlice::nodes() returns them); nullopt when `strike` is not a node.
[[nodiscard]] std::optional<NodeSide> node_side(std::span<const AmericanPriceNode> nodes,
                                                double strike, Side side) noexcept;

// Flags node side `bit` of node `node` among `nodes` nodes as served price-only
// (QuoteSliceFit::price_only_nodes, sized to `nodes` on first use). Precondition:
// node < nodes.
void mark_price_only(QuoteSliceFit &fit, std::size_t node, unsigned bit, std::size_t nodes);

// The served-volatility floor rule: true when `iv` is finite and at or below the
// inverter's reported floor kIvMin. False for NaN.
[[nodiscard]] bool at_vol_floor(double iv) noexcept;

// The identification of one served volatility: Success with `volatility`, or PriceOnly
// with the reason and the interval where known (QuoteSlicePriceOnlyReason).
struct VolIdentification {
  QuoteSliceEvaluationStatus status = QuoteSliceEvaluationStatus::PriceOnly;
  QuoteSlicePriceOnlyReason reason = QuoteSlicePriceOnlyReason::UnidentifiedInterval;
  double volatility = std::numeric_limits<double>::quiet_NaN();
  double iv_lower = std::numeric_limits<double>::quiet_NaN();
  double iv_upper = std::numeric_limits<double>::quiet_NaN();
};

// The FloorVol answer for certified `price` of (strike, side) at the contract's own
// carry: [0, kIvMin], or [0, sigma_ex] when the price sits on the Andersen-Lake
// kernel's exercise plateau (continuous carry only).
[[nodiscard]] VolIdentification floor_vol(const QuoteSliceFit &fit, double strike, Side side,
                                          double price);

// The volatility re-implied from certified `price` at the contract's own carry. Legacy
// is the historical rule bit for bit: an identified positive root is Success (a floor
// root included), an inverter refusal is UnidentifiedInterval. PriceOnly classifies a
// root at or below kIvMin as FloorVol and types a NonMonotoneRegion refusal of
// american_iv.hpp as FlatExerciseRegion or LowVega (QuoteSlicePriceOnlyReason).
[[nodiscard]] VolIdentification reimplied_vol(const QuoteSliceFit &fit, double strike,
                                              Side side, double price,
                                              QuoteSliceFloorPolicy policy);

// Lane C2: the served support (QuoteSliceFit::withheld_nodes). True when `strike` (inside
// the certified domain) is served on `side`: no mask, a node side not withheld, or a strike
// strictly between two nodes neither of which is withheld on that side. A mask whose size
// differs from the node count serves nothing. Defined in quote_slice_publication.cpp.
[[nodiscard]] bool serves(const QuoteSliceFit &fit, double strike, Side side) noexcept;

// Withholds node side `bit` of node `node` among `nodes` nodes (QuoteSliceFit::
// withheld_nodes, sized to `nodes` on first use). Precondition: node < nodes.
void withhold(QuoteSliceFit &fit, std::size_t node, unsigned bit, std::size_t nodes);

// The discount factor of the non-dividend regime in which a slice was certified with
// check_nondividend_american (certify_and_serve: zero effective carry at the fit and at
// every node strike, no cash dividend, nonnegative rate); nullopt outside it, or without
// certified prices. Defined in quote_slice.cpp.
[[nodiscard]] std::optional<double> nondividend_discount(const QuoteSliceFit &fit);

} // namespace atx::vol::detail
