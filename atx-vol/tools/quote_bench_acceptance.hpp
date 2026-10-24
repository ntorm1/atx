#pragma once

// Explicitly requested offline audit only. This invokes the final public API,
// never mutates the fit, and never supplies a target or a vendor label to it.
#include "atx/vol/api/fitting/quote_slice.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace quote_bench_acceptance {
using Json = nlohmann::json;
using namespace atx::vol;

inline Json finite(double value) {
  return std::isfinite(value) ? Json(value) : Json(nullptr);
}

inline Json query(const QuoteSliceFit &fit, double strike, Side side) {
  const auto price = served_quote_slice_price(fit, strike, side);
  const auto typed = query_quote_slice(fit, strike, side);
  Json result{{"strike", strike}, {"side", side == Side::Call ? "call" : "put"},
              {"served_price", price ? finite(*price) : Json(nullptr)}};
  if (typed) {
    result["status"] = to_string(typed->status);
    result["price"] = finite(typed->price);
    result["volatility"] = finite(typed->volatility);
    result["reason"] = to_string(typed->reason);
  } else {
    result["status"] = "error";
    result["error"] = typed.error().to_string();
    result["price"] = nullptr;
    result["volatility"] = nullptr;
  }
  return result;
}

inline Json surface(std::span<const QuoteSurfaceSlice> slices,
                    std::span<const std::uint64_t> chains) {
  std::set<double> union_knots;
  for (const auto &slice : slices) {
    if (slice.fit.prices) {
      for (const auto &node : slice.fit.prices->nodes()) union_knots.insert(node.strike);
    }
  }
  Json result = Json::array();
  for (std::size_t index = 0; index < slices.size(); ++index) {
    const auto &slice = slices[index];
    const auto &fit = slice.fit;
    Json entry{{"chain_id", chains[index]}, {"grade", to_string(slice.grade)},
               {"refusal", slice.refusal}, {"fit_status", to_string(fit.fit_status)},
               {"fit_error", fit.fit_error}, {"record_attached", bool(fit.serve_record)},
               {"calendar_years", finite(fit.market.calendar_years)},
               {"market", {{"spot", finite(fit.market.spot)},
                            {"rate", finite(fit.market.rate)},
                            {"calendar_years", finite(fit.market.calendar_years)},
                            {"variance_years", finite(fit.market.variance_years)},
                            {"continuous_yield", finite(fit.market.continuous_yield)},
                            {"dividend_pv", finite(fit.market.dividend_pv)}}},
               {"dividend_model", to_string(fit.dividend_model)},
               {"carry_curve", Json::array()},
               {"withheld_nodes", fit.withheld_nodes},
               {"price_only_nodes", fit.price_only_nodes}, {"moved_nodes", fit.moved_nodes},
               {"nodes", Json::array()}, {"queries", Json::array()}};
    for (const auto &point : fit.carry_curve) {
      entry["carry_curve"].push_back({{"strike", point.strike},
                                      {"continuous_yield", point.continuous_yield}});
    }
    if (!fit.prices) {
      result.push_back(std::move(entry));
      continue;
    }
    const auto &bounds = fit.prices->bounds();
    entry["bounds"] = {{"spot", bounds.spot}, {"discount_min", bounds.discount_min},
                        {"discount_max", bounds.discount_max},
                        {"prepaid_min", bounds.prepaid_min},
                        {"prepaid_max", bounds.prepaid_max}};
    const auto nodes = fit.prices->nodes();
    std::set<double> strikes;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      const auto &node = nodes[i];
      entry["nodes"].push_back({{"strike", node.strike}, {"call", node.call},
                                {"put", node.put}});
      strikes.insert(node.strike);
      if (i == 0) continue;
      for (const double fraction : {0.25, 0.5, 0.75}) {
        strikes.insert(std::lerp(nodes[i - 1].strike, node.strike, fraction));
      }
    }
    for (const double strike : union_knots) {
      if (strike >= nodes.front().strike && strike <= nodes.back().strike) {
        strikes.insert(strike);
      }
    }
    for (const double strike : strikes) {
      for (const Side side : {Side::Call, Side::Put}) {
        entry["queries"].push_back(query(fit, strike, side));
      }
    }
    result.push_back(std::move(entry));
  }
  return result;
}
} // namespace quote_bench_acceptance
