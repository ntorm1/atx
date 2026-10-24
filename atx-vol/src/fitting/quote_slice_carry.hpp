#pragma once

// Internal seam between fit_quote_slice and the opt-in carry resolution
// (api/fitting/quote_slice_carry.hpp). Not public API: callers select the mode
// through QuoteSliceConfig::carry and call fit_quote_slice.

#include <span>

#include "atx/vol/api/fitting/quote_slice.hpp"

namespace atx::vol::detail {

// Resolve the expiry carry under config.carry.mode (which must not be
// Supplied), then run the Supplied fit on the resolved inputs and record the
// resolution on the returned fit. A refused estimate fits the original inputs.
[[nodiscard]] Result<QuoteSliceFit>
fit_quote_slice_resolving_carry(const QuoteSliceMarket &market,
                                std::span<const QuoteSliceQuote> quotes,
                                const QuoteSliceConfig &config,
                                std::span<const CashDividend> calendar_dividends);

} // namespace atx::vol::detail
