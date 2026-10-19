#pragma once

// Vol-space channel of the quote slice (lane L2a, 2026-10-18; opt-in
// QuoteSliceFitSpace::VolChannel). Internal header (src/fitting).
//
// The channel objective (slice_channel.hpp) is a VOLATILITY-space objective:
// every contract's bid/ask band enters as American implied volatilities, and
// the knot-spline Newton (or a family's point fitter) never prices an option.
// What the PriceChannel path pays per fit is the band itself - two model prices
// and up to three American inversions per quote, repeated by every rung that
// refits the chain. The vol-space path reads the bands from the chain's
// de-Americanised IV cache (deam_iv_cache.hpp) and hands them to the SAME curve
// family dispatch; the fitted curve is then repriced once and certified by the
// unchanged certificate in quote_slice.cpp (certify_and_serve).
//
// Total-variance band: w = sigma^2 * T is strictly increasing in sigma > 0, so
// the band [sigma_bid^2 T, sigma_ask^2 T] contains w(k) exactly when
// [sigma_bid, sigma_ask] contains sigma(k); the solver's hinge in the volatility
// unit is the variance-band hinge divided by the local Jacobian 2 sigma T. The
// zero-violation set - the only thing certification and containment see - is
// the same in either unit, so the bands stay in volatility units and the family
// solvers are reused unchanged.

#include <span>
#include <vector>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "fitting/deam_iv_cache.hpp"
#include "fitting/slice_channel.hpp"

namespace atx::vol {

// The channel observations of `quotes` from the cache, with exactly the
// selection, edges, mid anchors and in-the-money mid weights of the
// PriceChannel builder (quote_slice.cpp, build_channel_observations): on the
// accurate kernel the two observation sets are bit-identical. Admitted rows
// (out.rows, after point admission) keep their admission mid volatility. Fills
// out.diagnostics.channel's used / channel_quotes / mid_quotes / mid_strikes.
[[nodiscard]] std::vector<ChannelObservation>
vol_space_channel_observations(QuoteSliceFit &out, std::span<const QuoteSliceQuote> quotes,
                               DeAmIvView &cache, const QuoteSliceChannelConfig &channel);

} // namespace atx::vol
