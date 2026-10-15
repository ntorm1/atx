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
         std::isfinite(b.prepaid_max) && b.prepaid_max >= b.prepaid_min && b.discount_min <= 1.0 &&
         b.discount_max >= 1.0 && b.prepaid_min <= b.spot && b.prepaid_max >= b.spot;
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

[[nodiscard]] Status validate_call_lower_slopes(std::span<const AmericanPriceNode> nodes,
                                               double spot, double discount,
                                               const char *reason) {
  std::size_t maximum = 0u;
  for (std::size_t i = 1u; i < nodes.size(); ++i) {
    const auto &left = nodes[maximum];
    const auto &right = nodes[i];
    // Track the running maximum of C(K)+discount*K. Compare differences
    // instead of forming those sums, which can overflow for finite prices.
    const double change = right.call - left.call + discount * (right.strike - left.strike);
    if (change < -price_tolerance(spot, right.strike)) {
      return node_error(i, reason);
    }
    if (change > 0.0) maximum = i;
  }
  return Ok();
}

[[nodiscard]] Status validate_vertical_spreads(std::span<const AmericanPriceNode> nodes,
                                              const AmericanPriceBounds &bounds) {
  double minimum_call = nodes.front().call;
  double maximum_put = nodes.front().put;
  std::size_t minimum_discounted_put = 0u;
  for (std::size_t i = 1u; i < nodes.size(); ++i) {
    const auto &node = nodes[i];
    const auto &left = nodes[minimum_discounted_put];
    const double tol = price_tolerance(bounds.spot, node.strike);
    // P(K)-discount_max*K must be nonincreasing. Running extrema make
    // the tolerance apply to every pair, not once per adjacent segment.
    const double change =
        node.put - left.put - bounds.discount_max * (node.strike - left.strike);
    if (node.call - minimum_call > tol || maximum_put - node.put > tol || change > tol) {
      return node_error(i, "American vertical-spread slope bound violated");
    }
    minimum_call = std::min(minimum_call, node.call);
    maximum_put = std::max(maximum_put, node.put);
    if (change < 0.0) minimum_discounted_put = i;
  }
  return validate_call_lower_slopes(nodes, bounds.spot, bounds.discount_max,
                                     "American vertical-spread slope bound violated");
}

[[nodiscard]] Status validate_convex_envelope(std::span<const AmericanPriceNode> nodes,
                                             const AmericanPriceBounds &bounds, Side side,
                                             std::vector<std::size_t> &hull) {
  const auto value = [side](const AmericanPriceNode &node) noexcept {
    return side == Side::Call ? node.call : node.put;
  };
  hull.clear();
  for (std::size_t i = 0u; i < nodes.size(); ++i) {
    while (hull.size() >= 2u) {
      const auto &left = nodes[hull[hull.size() - 2u]];
      const auto &middle = nodes[hull.back()];
      const auto &right = nodes[i];
      const double fraction = (middle.strike - left.strike) / (right.strike - left.strike);
      // No admission tolerance here: retaining many individually small
      // concave bends can conceal a large butterfly over the full domain.
      if (value(middle) <= std::lerp(value(left), value(right), fraction)) break;
      hull.pop_back();
    }
    hull.push_back(i);
  }
  for (std::size_t segment = 1u; segment < hull.size(); ++segment) {
    const auto &left = nodes[hull[segment - 1u]];
    const auto &right = nodes[hull[segment]];
    for (std::size_t i = hull[segment - 1u] + 1u; i < hull[segment]; ++i) {
      const auto &node = nodes[i];
      const double fraction = (node.strike - left.strike) / (right.strike - left.strike);
      const double envelope = std::lerp(value(left), value(right), fraction);
      if (value(node) - envelope > price_tolerance(bounds.spot, node.strike)) {
        return node_error(i, "call/put convexity violated above the global lower convex envelope");
      }
    }
  }
  return Ok();
}

[[nodiscard]] Status validate_nondividend_segment(const AmericanPriceNode &left,
                                                  const AmericanPriceNode &right, double spot,
                                                  double df, std::size_t index) {
  const double width = right.strike - left.strike;
  const double tol = price_tolerance(spot, right.strike);
  if (right.call - left.call < -df * width - tol) {
    return node_error(index, "nondividend European call slope is below -df");
  }
  const double call_slope = (right.call - left.call) / width;
  const double put_slope = (right.put - left.put) / width;
  // K*f'(K)-f(K) is constant on each affine segment, including its left
  // endpoint under the right-derivative convention. Use one comparison per
  // represented segment rather than a sampled derivative stencil.
  const double gap = left.strike * (put_slope - call_slope) - (left.put - left.call + spot);
  if (!std::isfinite(gap) || gap < -tol) {
    return node_error(index, "nondividend American/European Legendre condition violated");
  }
  return Ok();
}

[[nodiscard]] Status validate_nondividend_upper(const AmericanPriceSlice &slice, double df) {
  const auto nodes = slice.nodes();
  const double lo = nodes.front().strike;
  const double hi = df * nodes.back().strike;
  if (hi < lo) {
    return Err(ErrorCode::Unavailable,
               "nondividend American: no supported strike interval for scaled upper bound");
  }
  const double spot = slice.bounds().spot;
  std::size_t native = 0u;
  std::size_t scaled = 0u;
  while (native < nodes.size() || scaled < nodes.size()) {
    const bool take_native =
        scaled == nodes.size() ||
        (native < nodes.size() && nodes[native].strike <= df * nodes[scaled].strike);
    const double strike = take_native ? nodes[native++].strike : df * nodes[scaled++].strike;
    if (native < nodes.size() && nodes[native].strike == strike) {
      ++native;
    }
    if (scaled < nodes.size() && df * nodes[scaled].strike == strike) {
      ++scaled;
    }
    if (strike < lo || strike > hi) {
      continue;
    }
    // Membership in [lo, df*Kmax] proves the exact quotient is supported.
    // Clamp only the quotient's last-bit endpoint rounding, never the domain.
    const double call_strike = std::clamp(strike / df, lo, nodes.back().strike);
    const auto call = slice.price(call_strike, Side::Call);
    const auto put = slice.price(strike, Side::Put);
    if (!call || !put) {
      return Err(ErrorCode::Internal, "nondividend American: supported price unavailable");
    }
    if (*put - (*call - spot + strike) > price_tolerance(spot, strike)) {
      return Err(ErrorCode::InvalidArgument,
                 "nondividend American: scaled European upper bound violated at strike " +
                     std::to_string(strike));
    }
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
    if (i > 0u && !(nodes[i].strike > nodes[i - 1u].strike)) {
      ATX_TRY_VOID(node_error(i, "strikes must be strictly increasing"));
    }
  }
  ATX_TRY_VOID(validate_vertical_spreads(nodes, bounds));
  std::vector<std::size_t> hull;
  hull.reserve(nodes.size());
  ATX_TRY_VOID(validate_convex_envelope(nodes, bounds, Side::Call, hull));
  ATX_TRY_VOID(validate_convex_envelope(nodes, bounds, Side::Put, hull));
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

Status check_nondividend_american(const AmericanPriceSlice &slice, double df) {
  if (!std::isfinite(df) || !(df > 0.0) || df > 1.0) {
    return Err(ErrorCode::InvalidArgument,
               "nondividend American: constant nonnegative rate requires 0 < df <= 1");
  }
  const auto nodes = slice.nodes();
  if (nodes.size() < 2u) {
    return Err(ErrorCode::InvalidArgument, "nondividend American: empty or moved-from slice");
  }
  const double spot = slice.bounds().spot;
  for (std::size_t i = 0u; i < nodes.size(); ++i) {
    const auto &node = nodes[i];
    const double tol = price_tolerance(spot, node.strike);
    const double european = node.call - spot + df * node.strike;
    if (node.call < std::max(spot - df * node.strike, 0.0) - tol || node.call - spot > tol) {
      return node_error(i, "nondividend European call price bound violated");
    }
    if (european - node.put > tol) {
      return node_error(i, "nondividend American put is below its European put");
    }
    if (i > 0u) {
      ATX_TRY_VOID(validate_nondividend_segment(nodes[i - 1u], node, spot, df, i - 1u));
    }
  }
  ATX_TRY_VOID(validate_call_lower_slopes(nodes, spot, df,
                                          "nondividend European call slope is below -df"));
  return validate_nondividend_upper(slice, df);
}

} // namespace atx::vol
