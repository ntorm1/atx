#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include "atx/ui/vol_surface_source.hpp"

namespace atx::ui {

struct QuoteEdgeMetrics {
  double price_edge{0.0};
  double iv_edge_bp{0.0};
  bool model_in_bid_ask{false};
};

[[nodiscard]] inline QuoteEdgeMetrics quote_edge_metrics(const VolQuotePoint &quote) noexcept {
  return QuoteEdgeMetrics{
      .price_edge = quote.theoretical_price - quote.mid_price,
      .iv_edge_bp = (quote.model_iv - quote.mid_iv) * 10000.0,
      .model_in_bid_ask = quote.model_iv >= quote.bid_iv && quote.model_iv <= quote.ask_iv,
  };
}

[[nodiscard]] inline std::optional<std::size_t>
nearest_expiry_by_days(std::span<const ExpiryInfo> expiries, double days) noexcept {
  if (expiries.empty() || !std::isfinite(days)) {
    return std::nullopt;
  }
  std::size_t best = 0;
  double best_distance = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < expiries.size(); ++i) {
    const double distance = std::fabs(expiries[i].years * 365.25 - days);
    if (distance < best_distance) {
      best = i;
      best_distance = distance;
    }
  }
  return best;
}

[[nodiscard]] inline std::optional<std::size_t>
choose_initial_expiry(std::span<const ExpiryInfo> expiries, std::string_view requested,
                      double default_tenor_days = 180.0) noexcept {
  if (expiries.empty()) {
    return std::nullopt;
  }
  if (!requested.empty()) {
    for (std::size_t i = 0; i < expiries.size(); ++i) {
      if (expiries[i].iso_date == requested) {
        return i;
      }
    }
  }
  return nearest_expiry_by_days(expiries, default_tenor_days);
}

} // namespace atx::ui
