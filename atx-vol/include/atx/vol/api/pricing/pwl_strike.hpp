#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

struct PwlStrikeNode {
  double strike{};
  double value{};
};

inline constexpr std::size_t kPwlStrikeMaxNodes = 1048576u;
inline constexpr std::size_t kPwlStrikeMaxTerms = 1024u;
inline constexpr std::size_t kPwlStrikeMaxInputNodes = 4194304u;
inline constexpr std::size_t kPwlStrikeMaxWorkspaceNodes = 2097152u;

struct PwlStrikeLimits {
  std::size_t max_nodes = 16384u;
  std::size_t max_terms = 64u;
  std::size_t max_input_nodes = 65536u;
  // Scratch slots, excluding the owned output: a double or size_t per slot.
  // Weighted sums use one cursor per term plus a convex-hull index per node;
  // payoff/exercise construction uses only the convex-hull index workspace.
  std::size_t max_workspace_nodes = 32768u;
};

class ConvexPwlStrikeFunction;

// Borrowed only during weighted_sum(). The output owns all its nodes.
// Zero weights are allowed; the corresponding knots are still retained.
struct PwlStrikeTerm {
  const ConvexPwlStrikeFunction *function{};
  double weight{};
};

// Nonnegative convex piecewise-affine function on a finite positive strike
// interval. The public construction algebra preserves convexity: payoff,
// nonnegative weighted sum, and maximum with an exercise payoff. Every input
// knot and representable interior policy crossing is retained; there is no
// resampling, smoothing, extrapolation, or approximate knot compression.
//
// Algebra is exact apart from floating-point rounding. After each operation,
// the entire function must lie within 64*epsilon*max(1,max_node_value) of its
// global lower convex envelope. This fixed numerical allowance is independent
// of the number of knots and cannot be configured. A crossing too close to an
// endpoint to represent distinctly is an explicit numerical failure.
//
// Construction/copies allocate. value() and nodes() do not; concurrent const
// access is safe. A moved-from function is empty and cannot be composed.
class ConvexPwlStrikeFunction {
public:
  ConvexPwlStrikeFunction(const ConvexPwlStrikeFunction &) = default;
  ConvexPwlStrikeFunction &operator=(const ConvexPwlStrikeFunction &) = default;
  ConvexPwlStrikeFunction(ConvexPwlStrikeFunction &&) noexcept = default;
  ConvexPwlStrikeFunction &operator=(ConvexPwlStrikeFunction &&) noexcept = default;

  // Spot may be zero. The domain must satisfy 0 < min_strike < max_strike.
  [[nodiscard]] static Result<ConvexPwlStrikeFunction>
  payoff(double spot, Side side, double min_strike, double max_strike,
         PwlStrikeLimits limits = {});

  // At least one term; every function must have exactly the same endpoints.
  // Weights need not sum to one: include any positive discount in the weights.
  [[nodiscard]] static Result<ConvexPwlStrikeFunction>
  weighted_sum(std::span<const PwlStrikeTerm> terms, PwlStrikeLimits limits = {});

  [[nodiscard]] Result<ConvexPwlStrikeFunction>
  max_intrinsic(double spot, Side side, PwlStrikeLimits limits = {}) const;

  [[nodiscard]] std::optional<double> value(double strike) const noexcept;
  [[nodiscard]] std::span<const PwlStrikeNode> nodes() const noexcept { return nodes_; }

private:
  explicit ConvexPwlStrikeFunction(std::vector<PwlStrikeNode> nodes) noexcept;
  std::vector<PwlStrikeNode> nodes_;
};

} // namespace atx::vol
