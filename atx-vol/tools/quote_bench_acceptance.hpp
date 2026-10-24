#pragma once

// Explicitly requested offline audit only. This invokes the final public API,
// never mutates the fit, and never supplies a target or a vendor label to it.
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "atx/vol/api/pricing/american.hpp"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace quote_bench_acceptance {
using Json = nlohmann::json;
using namespace atx::vol;

inline Json finite(double value) {
  return std::isfinite(value) ? Json(value) : Json(nullptr);
}

inline Json al_options(const std::optional<AlOpts> &options) {
  // nullopt selects ACCURATE, not AlOpts{}. al_fast_profile resolves that
  // preset's equivalent explicit knobs; retain Reference arithmetic here.
  AlOpts value = options ? *options : al_fast_profile(std::nullopt);
  if (!options) value.profile = AlProfile::Reference;
  return {{"explicit", options.has_value()}, {"n_collocation", value.n_collocation},
          {"n_quadrature", value.n_quadrature}, {"n_quad_price", value.n_quad_price},
          {"max_newton_iter", value.max_newton_iter}, {"tol", value.tol},
          {"profile", static_cast<unsigned>(value.profile)}};
}

inline double local_calendar_yield(const QuoteSliceFit &fit, double strike) {
  const auto &curve = fit.carry_curve;
  const auto hi = std::lower_bound(curve.begin(), curve.end(), strike,
      [](const auto &point, double value) { return point.strike < value; });
  if (curve.empty()) return fit.market.continuous_yield;
  if (hi == curve.end()) return curve.back().continuous_yield;
  if (hi == curve.begin() || hi->strike == strike) return hi->continuous_yield;
  const auto &lo = *(hi - 1);
  const double weight = std::log1p((strike - lo.strike) / lo.strike) /
                        std::log1p((hi->strike - lo.strike) / lo.strike);
  return std::lerp(lo.continuous_yield, hi->continuous_yield, weight);
}

// Cold forward evaluations, not a second inversion or a cache lookup. They are
// deliberately inside the explicitly nonqualifying audit clock only.
inline Json cold_node(const QuoteSliceFit &fit, double strike, Side side, double sigma) {
  Json result{{"model", "andersen_lake"}, {"options", al_options(fit.al_opts)}};
  if (fit.dividend_model == QuoteSliceDividendModel::DatedCash) {
    result["status"] = "unsupported_model";
    return result;
  }
  const auto &market = fit.market;
  const double calendar_q = local_calendar_yield(fit, strike);
  double q = calendar_q * (market.calendar_years / market.variance_years);
  if (calendar_q == market.continuous_yield) {
    q = fit.diagnostics.effective_yield;
  } else if (market.dividend_pv != 0.0 &&
      (fit.dividend_model == QuoteSliceDividendModel::AggregatePresentValue ||
       fit.dividend_model == QuoteSliceDividendModel::AggregateFundingPresentValue)) {
    const double fraction = fit.dividend_model == QuoteSliceDividendModel::AggregatePresentValue
        ? market.dividend_pv / market.spot
        : std::exp(std::log(market.dividend_pv) - std::log(market.spot) +
                   calendar_q * market.calendar_years);
    q -= std::log1p(-fraction) / market.variance_years;
  }
  constexpr double radius = 1.0e-7;
  result.update({{"status", "evaluated"}, {"spot", market.spot}, {"strike", strike},
                 {"variance_years", market.variance_years},
                 {"effective_rate", fit.diagnostics.effective_rate},
                 {"calendar_yield", finite(calendar_q)}, {"effective_yield", finite(q)},
                 {"sigma_lower", finite(sigma - radius)}, {"sigma_center", finite(sigma)},
                 {"sigma_upper", finite(sigma + radius)}, {"radius", radius},
                 {"floor_sigma", kIvMin}});
  for (const auto &[name, volatility] :
      {std::pair{"lower", sigma - radius}, std::pair{"center", sigma},
       std::pair{"upper", sigma + radius}, std::pair{"at_floor", kIvMin}}) {
    const auto price = andersen_lake(market.spot, strike, market.variance_years, volatility,
        fit.diagnostics.effective_rate, q, side, fit.al_opts);
    result[std::string("price_") + name] = price ? finite(*price) : Json(nullptr);
    if (!price) result[std::string("error_") + name] = price.error().to_string();
  }
  return result;
}

inline Json query(const QuoteSliceFit &fit, double strike, Side side, bool node) {
  const auto price = served_quote_slice_price(fit, strike, side);
  const auto typed = query_quote_slice(fit, strike, side);
  Json result{{"strike", strike}, {"side", side == Side::Call ? "call" : "put"},
              {"served_price", price ? finite(*price) : Json(nullptr)}};
  if (typed) {
    result["status"] = to_string(typed->status);
    result["price"] = finite(typed->price);
    result["volatility"] = finite(typed->volatility);
    result["reason"] = to_string(typed->reason);
    result["iv_lower"] = finite(typed->iv_lower);
    result["iv_upper"] = finite(typed->iv_upper);
    result["vol_convention"] = to_string(typed->vol_convention);
    if (node && typed->status == QuoteSliceEvaluationStatus::Success) {
      result["cold_node"] = cold_node(fit, strike, side, typed->volatility);
    }
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
               {"method", to_string(slice.method)},
               {"cold_fit_fallback", to_string(slice.cold_fit_fallback)},
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
               {"al_options", al_options(fit.al_opts)},
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
      const auto found = std::lower_bound(nodes.begin(), nodes.end(), strike,
          [](const auto &node, double value) { return node.strike < value; });
      const bool is_node = found != nodes.end() && found->strike == strike;
      for (const Side side : {Side::Call, Side::Put}) {
        entry["queries"].push_back(query(fit, strike, side, is_node));
      }
    }
    result.push_back(std::move(entry));
  }
  return result;
}
} // namespace quote_bench_acceptance
