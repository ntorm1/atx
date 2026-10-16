#include "atx/vol/api/pricing/pwl_strike.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

[[nodiscard]] Status validate_limits(const PwlStrikeLimits &limits) {
  if (limits.max_nodes < 2u || limits.max_nodes > kPwlStrikeMaxNodes ||
      limits.max_terms == 0u || limits.max_terms > kPwlStrikeMaxTerms ||
      limits.max_input_nodes < 2u || limits.max_input_nodes > kPwlStrikeMaxInputNodes ||
      limits.max_workspace_nodes < 2u ||
      limits.max_workspace_nodes > kPwlStrikeMaxWorkspaceNodes) {
    return Err(ErrorCode::InvalidArgument, "PWL strike: invalid resource limits");
  }
  return Ok();
}

[[nodiscard]] Status validate_payoff(double spot, Side side) {
  if (!std::isfinite(spot) || spot < 0.0 || (side != Side::Call && side != Side::Put)) {
    return Err(ErrorCode::InvalidArgument, "PWL strike: invalid exercise payoff");
  }
  return Ok();
}

[[nodiscard]] double intrinsic(double spot, double strike, Side side) noexcept {
  return std::max(side == Side::Call ? spot - strike : strike - spot, 0.0);
}

[[nodiscard]] double roundoff_allowance(std::span<const PwlStrikeNode> nodes) noexcept {
  double scale = 1.0;
  for (const auto &node : nodes) scale = std::max(scale, node.value);
  return 64.0 * std::numeric_limits<double>::epsilon() * scale;
}

// Outward rounding keeps the explicitly introduced crossing error conservative
// through arbitrarily many algebra operations. Zero contributes exactly zero.
[[nodiscard]] double add_up(double a, double b) noexcept {
  if (a == 0.0) return b;
  if (b == 0.0) return a;
  return std::nextafter(a + b, std::numeric_limits<double>::infinity());
}

[[nodiscard]] double multiply_up(double a, double b) noexcept {
  if (a == 0.0 || b == 0.0) return 0.0;
  return std::nextafter(a * b, std::numeric_limits<double>::infinity());
}

[[nodiscard]] Status validate_crossing_error(double error,
                                            std::span<const PwlStrikeNode> nodes) {
  if (!std::isfinite(error) || error > roundoff_allowance(nodes)) {
    return Err(ErrorCode::OutOfRange,
               "PWL strike: accumulated crossing resolution error exceeds roundoff budget");
  }
  return Ok();
}

[[nodiscard]] Status append(std::vector<PwlStrikeNode> &nodes, PwlStrikeNode node,
                            const PwlStrikeLimits &limits, std::size_t other_workspace) {
  if (nodes.size() >= limits.max_nodes || other_workspace >= limits.max_workspace_nodes ||
      nodes.size() >= limits.max_workspace_nodes - other_workspace) {
    return Err(ErrorCode::Unavailable, "PWL strike: output or workspace node limit exceeded");
  }
  if (!std::isfinite(node.value) || node.value < 0.0) {
    return Err(ErrorCode::OutOfRange, "PWL strike: nonfinite or negative computed value");
  }
  if (nodes.size() == nodes.capacity()) {
    const std::size_t capacity = std::min(
        limits.max_nodes, std::max(std::size_t{2u}, 2u * nodes.capacity()));
    nodes.reserve(capacity);
  }
  nodes.push_back(node);
  return Ok();
}

[[nodiscard]] Status validate_convexity(std::span<const PwlStrikeNode> nodes) {
  const double tolerance = roundoff_allowance(nodes);
  std::vector<std::size_t> hull;
  hull.reserve(nodes.size());
  for (std::size_t i = 0u; i < nodes.size(); ++i) {
    while (hull.size() >= 2u) {
      const auto &left = nodes[hull[hull.size() - 2u]];
      const auto &middle = nodes[hull.back()];
      const auto &right = nodes[i];
      const double fraction = (middle.strike - left.strike) / (right.strike - left.strike);
      if (middle.value <= std::lerp(left.value, right.value, fraction)) break;
      hull.pop_back();
    }
    hull.push_back(i);
  }
  for (std::size_t j = 1u; j < hull.size(); ++j) {
    const auto &left = nodes[hull[j - 1u]];
    const auto &right = nodes[hull[j]];
    for (std::size_t i = hull[j - 1u] + 1u; i < hull[j]; ++i) {
      const double fraction = (nodes[i].strike - left.strike) / (right.strike - left.strike);
      if (nodes[i].value - std::lerp(left.value, right.value, fraction) > tolerance) {
        return Err(ErrorCode::OutOfRange, "PWL strike: global convexity roundoff budget exceeded");
      }
    }
  }
  return Ok();
}

[[nodiscard]] double cursor_value(std::span<const PwlStrikeNode> nodes, std::size_t next,
                                  double strike) noexcept {
  if (nodes[next].strike == strike) return nodes[next].value;
  const auto &left = nodes[next - 1u];
  const auto &right = nodes[next];
  return std::lerp(left.value, right.value, (strike - left.strike) / (right.strike - left.strike));
}

[[nodiscard]] Status exercise_segment(std::vector<PwlStrikeNode> &output,
                                      const PwlStrikeNode &left, const PwlStrikeNode &right,
                                      double spot, Side side, const PwlStrikeLimits &limits,
                                      double &crossing_error) {
  const double left_payoff = intrinsic(spot, left.strike, side);
  const double right_payoff = intrinsic(spot, right.strike, side);
  const double left_gap = left.value - left_payoff;
  const double right_gap = right.value - right_payoff;
  if ((left_gap < 0.0 && right_gap > 0.0) || (left_gap > 0.0 && right_gap < 0.0)) {
    const double a = std::abs(left_gap);
    const double b = std::abs(right_gap);
    // Avoid overflowing a+b when two finite function values are very large.
    const double fraction = a > b ? 1.0 / (1.0 + b / a) : (a / b) / (1.0 + a / b);
    const double strike = std::lerp(left.strike, right.strike, fraction);
    if (!(strike > left.strike && strike < right.strike)) {
      // The maximum of the two affine segments lies below the chord of its
      // endpoint maxima by at most a*b/(a+b) <= min(a,b). There may be NO double
      // between the endpoints. Retain their exact maxima and account for this
      // unresolved crossing instead of manufacturing a duplicate strike.
      // One nextafter bounds rounding in the endpoint subtractions. Segment
      // interiors are disjoint, so their uniform errors combine by maximum.
      crossing_error = std::max(crossing_error,
          std::nextafter(std::min(a, b), std::numeric_limits<double>::infinity()));
    } else {
      const double continuation = std::lerp(
          left.value, right.value, (strike - left.strike) / (right.strike - left.strike));
      ATX_TRY_VOID(append(output, {strike, std::max(continuation, intrinsic(spot, strike, side))},
                          limits, 0u));
    }
  }
  return append(output, {right.strike, std::max(right.value, right_payoff)}, limits, 0u);
}

} // namespace

ConvexPwlStrikeFunction::ConvexPwlStrikeFunction(std::vector<PwlStrikeNode> nodes,
                                               double crossing_error) noexcept
    : nodes_(std::move(nodes)), crossing_resolution_error_bound_(crossing_error) {}

Result<ConvexPwlStrikeFunction>
ConvexPwlStrikeFunction::payoff(double spot, Side side, double min_strike, double max_strike,
                               PwlStrikeLimits limits) {
  ATX_TRY_VOID(validate_limits(limits));
  ATX_TRY_VOID(validate_payoff(spot, side));
  if (!std::isfinite(min_strike) || !std::isfinite(max_strike) || !(min_strike > 0.0) ||
      !(max_strike > min_strike)) {
    return Err(ErrorCode::InvalidArgument, "PWL strike: invalid strike domain");
  }
  std::vector<PwlStrikeNode> nodes;
  nodes.reserve(std::min(limits.max_nodes, std::size_t{3u}));
  ATX_TRY_VOID(append(nodes, {min_strike, intrinsic(spot, min_strike, side)}, limits, 0u));
  if (spot > min_strike && spot < max_strike) {
    ATX_TRY_VOID(append(nodes, {spot, 0.0}, limits, 0u));
  }
  ATX_TRY_VOID(append(nodes, {max_strike, intrinsic(spot, max_strike, side)}, limits, 0u));
  ATX_TRY_VOID(validate_convexity(nodes));
  return Ok(ConvexPwlStrikeFunction(std::move(nodes), 0.0));
}

Result<ConvexPwlStrikeFunction>
ConvexPwlStrikeFunction::weighted_sum(std::span<const PwlStrikeTerm> terms,
                                     PwlStrikeLimits limits) {
  ATX_TRY_VOID(validate_limits(limits));
  if (terms.empty()) {
    return Err(ErrorCode::InvalidArgument, "PWL strike: weighted sum requires at least one term");
  }
  if (terms.size() > limits.max_terms || terms.size() >= limits.max_workspace_nodes) {
    return Err(ErrorCode::Unavailable, "PWL strike: term or workspace limit exceeded");
  }
  std::size_t input_nodes = 0u;
  double crossing_error = 0.0;
  double lo = 0.0;
  double hi = 0.0;
  for (std::size_t i = 0u; i < terms.size(); ++i) {
    const auto &term = terms[i];
    if (!term.function || !std::isfinite(term.weight) || term.weight < 0.0 ||
        term.function->nodes().size() < 2u) {
      return Err(ErrorCode::InvalidArgument, "PWL strike: invalid weighted term");
    }
    const auto nodes = term.function->nodes();
    crossing_error = add_up(crossing_error,
        multiply_up(term.weight, term.function->crossing_resolution_error_bound()));
    if (nodes.size() > limits.max_input_nodes - input_nodes) {
      return Err(ErrorCode::Unavailable, "PWL strike: input node limit exceeded");
    }
    input_nodes += nodes.size();
    if (i == 0u) {
      lo = nodes.front().strike;
      hi = nodes.back().strike;
    } else if (nodes.front().strike != lo || nodes.back().strike != hi) {
      return Err(ErrorCode::InvalidArgument, "PWL strike: weighted domains differ");
    }
  }
  std::vector<std::size_t> cursors(terms.size(), 0u);
  std::vector<PwlStrikeNode> output;
  output.reserve(std::min(input_nodes, limits.max_nodes));
  for (;;) {
    double strike = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0u; i < terms.size(); ++i) {
      const auto nodes = terms[i].function->nodes();
      if (cursors[i] < nodes.size()) strike = std::min(strike, nodes[cursors[i]].strike);
    }
    if (!std::isfinite(strike)) break;
    double sum = 0.0;
    double correction = 0.0;
    for (std::size_t i = 0u; i < terms.size(); ++i) {
      const auto nodes = terms[i].function->nodes();
      const double value = cursor_value(nodes, cursors[i], strike);
      const double weighted = terms[i].weight * value;
      if (!std::isfinite(weighted)) {
        return Err(ErrorCode::OutOfRange, "PWL strike: weighted value overflow");
      }
      const double adjusted = weighted - correction;
      const double next = sum + adjusted;
      correction = (next - sum) - adjusted;
      sum = next;
      if (nodes[cursors[i]].strike == strike) ++cursors[i];
    }
    ATX_TRY_VOID(append(output, {strike, sum}, limits, terms.size()));
  }
  ATX_TRY_VOID(validate_convexity(output));
  ATX_TRY_VOID(validate_crossing_error(crossing_error, output));
  return Ok(ConvexPwlStrikeFunction(std::move(output), crossing_error));
}

Result<ConvexPwlStrikeFunction>
ConvexPwlStrikeFunction::max_intrinsic(double spot, Side side, PwlStrikeLimits limits) const {
  ATX_TRY_VOID(validate_limits(limits));
  ATX_TRY_VOID(validate_payoff(spot, side));
  if (nodes_.size() < 2u) {
    return Err(ErrorCode::InvalidArgument, "PWL strike: empty or moved-from continuation");
  }
  if (nodes_.size() > limits.max_input_nodes) {
    return Err(ErrorCode::Unavailable, "PWL strike: input node limit exceeded");
  }
  std::vector<PwlStrikeNode> output;
  double local_crossing_error = 0.0;
  output.reserve(std::min(nodes_.size(), limits.max_nodes));
  ATX_TRY_VOID(append(output, {nodes_.front().strike,
                              std::max(nodes_.front().value,
                                       intrinsic(spot, nodes_.front().strike, side))}, limits, 0u));
  for (std::size_t i = 1u; i < nodes_.size(); ++i) {
    const auto &left = nodes_[i - 1u];
    const auto &right = nodes_[i];
    if (spot > left.strike && spot < right.strike) {
      const PwlStrikeNode middle{spot, std::lerp(
          left.value, right.value, (spot - left.strike) / (right.strike - left.strike))};
      ATX_TRY_VOID(exercise_segment(
          output, left, middle, spot, side, limits, local_crossing_error));
      ATX_TRY_VOID(exercise_segment(
          output, middle, right, spot, side, limits, local_crossing_error));
    } else {
      ATX_TRY_VOID(exercise_segment(output, left, right, spot, side, limits, local_crossing_error));
    }
  }
  ATX_TRY_VOID(validate_convexity(output));
  // Maximum with a fixed payoff is 1-Lipschitz in the continuation value.
  const double crossing_error = add_up(crossing_resolution_error_bound_, local_crossing_error);
  ATX_TRY_VOID(validate_crossing_error(crossing_error, output));
  return Ok(ConvexPwlStrikeFunction(std::move(output), crossing_error));
}

std::optional<double> ConvexPwlStrikeFunction::value(double strike) const noexcept {
  if (!std::isfinite(strike) || nodes_.empty() || strike < nodes_.front().strike ||
      strike > nodes_.back().strike) {
    return std::nullopt;
  }
  const auto right = std::lower_bound(nodes_.begin(), nodes_.end(), strike,
                                     [](const auto &node, double k) { return node.strike < k; });
  if (right->strike == strike) return right->value;
  const auto left = right - 1;
  return std::lerp(left->value, right->value,
                   (strike - left->strike) / (right->strike - left->strike));
}

} // namespace atx::vol
