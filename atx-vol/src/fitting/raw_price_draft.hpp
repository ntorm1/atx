#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/api/fitting/quote_slice.hpp"
#include "fitting/joint_inbox_projection.hpp"

namespace atx::vol::detail {

enum class RawPriceDraftRefusal : std::uint8_t {
  None, InvalidInput, UnsupportedModel, UnsupportedCarryPolicy, InvalidQuote,
  MissingPairedSide, EmptyQuoteIntersection, UnsupportedRegime
};

// Private staging only. prepared has market/carry metadata, no curve/prices and no
// Success status. Projection cannot make it a queryable public QuoteSliceFit.
struct RawPriceDraft {
  QuoteSliceFit prepared;
  std::vector<QuoteSliceQuote> original_quotes;
  std::vector<std::size_t> quote_nodes;
  std::vector<AmericanPriceNode> nodes;
  std::vector<AmericanPriceNode> raw_targets; // original midpoints; projection never replaces them
  std::vector<AmericanPriceBox> boxes;
  AmericanPriceBounds bounds{};
  std::vector<double> terminal_prepaid;
  TerminalFloorRecovery recovery{};
  std::optional<AmericanPriceSlice> projected;
};

struct RawPriceDraftResult {
  RawPriceDraftRefusal refusal = RawPriceDraftRefusal::InvalidInput;
  std::string error;
  std::optional<RawPriceDraft> draft;
};

// Exact original boxes, including zero bids and duplicate intersections. Only
// explicitly selected terminal-floor ask recovery can widen an original ask.
// Missing paired sides and unsupported model/carry policies return typed refusals;
// no row is silently dropped. No American inversion or model price is evaluated.
[[nodiscard]] RawPriceDraftResult make_raw_price_draft(
    const QuoteSliceMarket &market, std::span<const QuoteSliceQuote> quotes,
    const QuoteSliceConfig &config, std::span<const CashDividend> dividends = {});

// Shared prepare_fit validation/clock/carry seam. Requires sorted distinct nodes.
// Internal only; it never sets a Success status or builds a curve/serving slice.
[[nodiscard]] Status prepare_raw_price_draft_metadata(
    RawPriceDraft &draft, const QuoteSliceMarket &market,
    const QuoteSliceConfig &config, std::span<const CashDividend> dividends);

// Sorts drafts by calendar maturity for the solve, without reordering the caller's
// slots. On every failure all drafts remain unchanged. No public fit is published.
[[nodiscard]] JointProjectionResult project_raw_price_drafts(
    std::span<RawPriceDraft> drafts, std::size_t max_iterations = 100,
    JointInboxDiagnostics *diagnostics = nullptr, JointInboxProfile *profile = nullptr);

// Consumes a successfully projected private draft. Classifies each final node side,
// builds a genuine ConvexDense curve from identified final OTM observations, then
// publishes Success with all node prices served off that curve. Every original row
// remains unadmitted ProjectedPrice with market_iv NaN. On any failure no public fit
// is returned; the surface caller can atomically fall back to its existing policy.
[[nodiscard]] Result<QuoteSliceFit> finalize_raw_price_draft(
    RawPriceDraft &&draft, const QuoteSliceConfig &config);

} // namespace atx::vol::detail
