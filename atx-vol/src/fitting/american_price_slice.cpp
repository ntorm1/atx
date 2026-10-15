#include "atx/vol/api/fitting/american_price_slice.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

[[nodiscard]] double price_tolerance(double spot, double strike) noexcept {
  return 1.0e-8 + 1.0e-9 * std::max(spot, strike);
}

[[nodiscard]] Status node_error(std::size_t index, const char *reason) {
  return Err(ErrorCode::InvalidArgument,
             "AmericanPriceSlice: node " + std::to_string(index) + ": " + reason);
}

[[nodiscard]] bool valid_bounds(const AmericanPriceBounds &b) noexcept {
  return std::isfinite(b.spot) && b.spot > 0.0 && std::isfinite(b.discount_min) &&
         b.discount_min > 0.0 && std::isfinite(b.discount_max) &&
         b.discount_max >= b.discount_min && std::isfinite(b.prepaid_min) && b.prepaid_min >= 0.0 &&
         std::isfinite(b.prepaid_max) && b.prepaid_max >= b.prepaid_min;
}

[[nodiscard]] Status validate_node(const AmericanPriceNode &node, const AmericanPriceBounds &bounds,
                                   std::size_t index) {
  if (!std::isfinite(node.strike) || !(node.strike > 0.0) || !std::isfinite(node.call) ||
      !std::isfinite(node.put) || node.call < 0.0 || node.put < 0.0) {
    return node_error(index, "strike/prices must be finite, strike positive, prices nonnegative");
  }
  const double tol = price_tolerance(bounds.spot, node.strike);
  const double discounted_max = node.strike * bounds.discount_max;
  const double discounted_min = node.strike * bounds.discount_min;
  if (!std::isfinite(discounted_max) || !std::isfinite(discounted_min)) {
    return node_error(index, "discounted strike is outside the finite numerical range");
  }
  if (node.call < std::max(bounds.spot - node.strike, 0.0) - tol ||
      node.put < std::max(node.strike - bounds.spot, 0.0) - tol) {
    return node_error(index, "American intrinsic bound violated");
  }
  if (node.call - bounds.prepaid_max > tol || node.put - discounted_max > tol) {
    return node_error(index, "American upper price bound violated");
  }
  const double difference = node.call - node.put;
  const double lower = bounds.prepaid_min - discounted_max;
  const double upper = bounds.prepaid_max - discounted_min;
  if (difference < lower - tol || difference > upper + tol) {
    return node_error(index, "affine call-minus-put bound violated");
  }
  return Ok();
}

[[nodiscard]] Status validate_segment(const AmericanPriceNode &left, const AmericanPriceNode &right,
                                      const AmericanPriceBounds &bounds, std::size_t index) {
  const double width = right.strike - left.strike;
  if (!(width > 0.0)) {
    return node_error(index, "strikes must be strictly increasing");
  }
  const double tol = price_tolerance(bounds.spot, right.strike);
  const double max_change = bounds.discount_max * width;
  const double call_change = right.call - left.call;
  const double put_change = right.put - left.put;
  if (call_change > tol || call_change < -max_change - tol || put_change < -tol ||
      put_change > max_change + tol) {
    return node_error(index, "American vertical-spread slope bound violated");
  }
  return Ok();
}

[[nodiscard]] Status validate_convexity(const AmericanPriceNode &left,
                                        const AmericanPriceNode &middle,
                                        const AmericanPriceNode &right,
                                        const AmericanPriceBounds &bounds, std::size_t index) {
  const double fraction = (middle.strike - left.strike) / (right.strike - left.strike);
  const double call_chord = std::lerp(left.call, right.call, fraction);
  const double put_chord = std::lerp(left.put, right.put, fraction);
  const double tol = price_tolerance(bounds.spot, right.strike);
  if (middle.call - call_chord > tol || middle.put - put_chord > tol) {
    return node_error(index, "adjacent call/put secants violate convexity");
  }
  return Ok();
}

} // namespace

AmericanPriceSlice::AmericanPriceSlice(std::vector<AmericanPriceNode> nodes,
                                       AmericanPriceBounds bounds) noexcept
    : nodes_(std::move(nodes)), bounds_(bounds) {}

Result<AmericanPriceSlice> AmericanPriceSlice::create(std::span<const AmericanPriceNode> nodes,
                                                      const AmericanPriceBounds &bounds) {
  if (!valid_bounds(bounds)) {
    return Err(ErrorCode::InvalidArgument, "AmericanPriceSlice: invalid carry bounds");
  }
  if (nodes.size() < 2u) {
    return Err(ErrorCode::InvalidArgument, "AmericanPriceSlice: at least two nodes required");
  }
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    ATX_TRY_VOID(validate_node(nodes[i], bounds, i));
    if (i > 0u) {
      ATX_TRY_VOID(validate_segment(nodes[i - 1u], nodes[i], bounds, i));
    }
    if (i > 1u) {
      ATX_TRY_VOID(validate_convexity(nodes[i - 2u], nodes[i - 1u], nodes[i], bounds, i - 1u));
    }
  }
  return Ok(AmericanPriceSlice(std::vector<AmericanPriceNode>(nodes.begin(), nodes.end()), bounds));
}

std::optional<double> AmericanPriceSlice::price(double strike, Side side) const noexcept {
  if (!std::isfinite(strike) || nodes_.empty() || strike < nodes_.front().strike ||
      strike > nodes_.back().strike || (side != Side::Call && side != Side::Put)) {
    return std::nullopt;
  }
  const auto right =
      std::lower_bound(nodes_.begin(), nodes_.end(), strike,
                       [](const AmericanPriceNode &node, double k) { return node.strike < k; });
  const auto value = [side](const AmericanPriceNode &node) noexcept {
    return side == Side::Call ? node.call : node.put;
  };
  if (right->strike == strike || right == nodes_.begin()) {
    return value(*right);
  }
  const auto left = right - 1;
  const double fraction = (strike - left->strike) / (right->strike - left->strike);
  return std::lerp(value(*left), value(*right), fraction);
}

Status check_american_calendar(const AmericanPriceSlice &earlier, const AmericanPriceSlice &later) {
  const auto first = earlier.nodes();
  const auto second = later.nodes();
  if (first.empty() || second.empty()) {
    return Err(ErrorCode::InvalidArgument, "American calendar: empty or moved-from slice");
  }
  const double lo = std::max(first.front().strike, second.front().strike);
  const double hi = std::min(first.back().strike, second.back().strike);
  if (hi < lo) {
    return Ok();
  }
  const double spot = std::max(earlier.bounds().spot, later.bounds().spot);
  // Both node sets are already sorted and validated. A merge visits every
  // possible extremum of the piecewise affine calendar spread, once each.
  std::size_t i = 0u;
  std::size_t j = 0u;
  while (i < first.size() || j < second.size()) {
    const double strike =
        j == second.size() || (i < first.size() && first[i].strike <= second[j].strike)
            ? first[i++].strike
            : second[j++].strike;
    if (i < first.size() && first[i].strike == strike) {
      ++i;
    }
    if (j < second.size() && second[j].strike == strike) {
      ++j;
    }
    if (strike < lo || strike > hi) {
      continue;
    }
    const double tol = price_tolerance(spot, strike);
    for (const Side side : {Side::Call, Side::Put}) {
      const auto near_price = earlier.price(strike, side);
      const auto far_price = later.price(strike, side);
      if (!near_price || !far_price) {
        return Err(ErrorCode::Internal, "American calendar: overlap price unavailable");
      }
      if (*near_price - *far_price > tol) {
        return Err(ErrorCode::InvalidArgument,
                   "American calendar: later " + std::string(side == Side::Call ? "call" : "put") +
                       " price decreases at strike " + std::to_string(strike));
      }
    }
  }
  return Ok();
}

} // namespace atx::vol
