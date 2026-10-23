#include "fitting/quote_slice_call_floor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

const char *to_string(QuoteSliceCallFloor value) noexcept {
  switch (value) {
  case QuoteSliceCallFloor::Legacy:
    return "legacy";
  case QuoteSliceCallFloor::DiscountedIntrinsic:
    return "discounted_intrinsic";
  }
  return "unknown";
}

namespace {

// The non-dividend regime on an expiry's inputs (quote_surface_call_floor_lines).
[[nodiscard]] bool nondividend_inputs(const QuoteSurfaceExpiry &expiry) noexcept {
  const QuoteSliceMarket &m = expiry.market;
  if (!(std::isfinite(m.rate) && m.rate >= 0.0) || m.continuous_yield != 0.0 ||
      m.dividend_pv != 0.0 || !expiry.calendar_dividends.empty() ||
      !(std::isfinite(m.spot) && m.spot > 0.0) ||
      !(std::isfinite(m.calendar_years) && m.calendar_years >= 0.0)) {
    return false;
  }
  return std::all_of(expiry.quotes.begin(), expiry.quotes.end(), [](const QuoteSliceQuote &q) {
    return !q.continuous_yield || *q.continuous_yield == 0.0;
  });
}

[[nodiscard]] bool valid_line(const QuoteSliceCallFloorLine &line) noexcept {
  return std::isfinite(line.calendar_years) && line.calendar_years >= 0.0 &&
         std::isfinite(line.spot) && line.spot > 0.0 && std::isfinite(line.discount) &&
         line.discount > 0.0 && line.discount <= 1.0;
}

// The certificate's fixed numerical allowance (american_price_slice.cpp).
[[nodiscard]] double allowance(double spot, double strike) noexcept {
  return 1.0e-8 + 1.0e-9 * std::max(spot, strike);
}

} // namespace

std::vector<QuoteSliceCallFloorLine>
quote_surface_call_floor_lines(std::span<const QuoteSurfaceExpiry> expiries) {
  std::vector<QuoteSliceCallFloorLine> lines;
  for (const QuoteSurfaceExpiry &expiry : expiries) {
    if (!nondividend_inputs(expiry)) continue;
    const QuoteSliceMarket &m = expiry.market;
    lines.push_back({m.calendar_years, m.spot, std::exp(-m.rate * m.calendar_years)});
  }
  return lines;
}

namespace detail {

Status select_call_floor(QuoteSliceFit &fit, const QuoteSliceCallFloorConfig &config) {
  fit.diagnostics.call_floor_lines.clear();
  if (config.mode == QuoteSliceCallFloor::Legacy) {
    if (!config.lines.empty()) {
      return Err(ErrorCode::InvalidArgument,
                 "quote slice: call floor lines need discounted_intrinsic");
    }
    return Ok();
  }
  if (config.mode != QuoteSliceCallFloor::DiscountedIntrinsic ||
      !std::all_of(config.lines.begin(), config.lines.end(), valid_line)) {
    return Err(ErrorCode::InvalidArgument, "quote slice: invalid call floor configuration");
  }
  for (const QuoteSliceCallFloorLine &line : config.lines) {
    if (line.calendar_years <= fit.market.calendar_years) {
      fit.diagnostics.call_floor_lines.push_back(line);
    }
  }
  return Ok();
}

double call_floor_at(std::span<const QuoteSliceCallFloorLine> lines, double strike) noexcept {
  double floor = 0.0;
  for (const QuoteSliceCallFloorLine &line : lines) {
    floor = std::max(floor, line.spot - strike * line.discount);
  }
  return floor;
}

std::vector<std::uint8_t> lift_to_call_floor(QuoteSliceFit &fit,
                                              std::vector<AmericanPriceNode> &nodes,
                                              const AmericanPriceBounds &bounds) {
  auto &diag = fit.diagnostics;
  diag.call_floor_lifted = 0;
  diag.call_floor_refused = false;
  if (diag.call_floor_lines.empty()) return {};
  std::vector<AmericanPriceNode> lifted = nodes;
  std::vector<std::uint8_t> moved(nodes.size(), 0);
  std::size_t count = 0;
  for (std::size_t i = 0; i < lifted.size(); ++i) {
    const double floor = call_floor_at(diag.call_floor_lines, lifted[i].strike);
    if (lifted[i].call < floor - allowance(bounds.spot, lifted[i].strike)) {
      lifted[i].call = floor;
      moved[i] = 1u;
      ++count;
    }
  }
  if (count == 0) return {};
  // A floor the slice's own certificate cannot carry (e.g. an earlier expiry's spot above
  // this one's) is refused, never half applied: the historical prices stay.
  if (!AmericanPriceSlice::create(lifted, bounds)) {
    diag.call_floor_refused = true;
    return {};
  }
  nodes = std::move(lifted);
  diag.call_floor_lifted = count;
  return moved;
}

} // namespace detail
} // namespace atx::vol
