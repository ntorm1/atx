#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "atx/vol/api/fitting/american_price_slice.hpp"

namespace atx::vol::detail {

// One expiry, in increasing maturity order. Boxes are exact hard price bounds;
// no box is widened. Nodes change only on Feasible, after the complete surface
// passes the unchanged slice and calendar certificates.
struct JointInboxSlice {
  std::span<AmericanPriceNode> nodes;
  std::span<const AmericanPriceBox> boxes;
  AmericanPriceBounds bounds;
  // Actual terminal discount, when the caller knows it. Horizon envelopes alone
  // do not identify it for a general rate curve.
  std::optional<double> terminal_discount = std::nullopt;
  // Actual terminal prepaid spot at each node, when known. Empty uses the
  // conservative horizon envelope. Nonempty requires terminal_discount.
  std::span<const double> terminal_prepaid{};
};

struct JointInboxDiagnostics {
  std::size_t variables = 0;
  std::size_t constraints = 0;
  double primal_residual = 0.0;
  double dual_residual = 0.0;
  double duality_gap = 0.0;
  bool certificate_passed = false;
};

struct TerminalFloorRecovery {
  std::size_t raised_asks = 0;
  double max_ask_raise = 0.0;
};

// Explicit quote recovery: only quoted asks below their own proven intrinsic or
// terminal European floor are raised to that floor plus the fixed certificate
// allowance. Bids and other asks remain identical. The original quotes are never
// inputs/outputs here. quoted uses bit 1 for call and bit 2 for put.
// Requires actual terminal_discount and terminal_prepaid for every node.
[[nodiscard]] bool recover_terminal_floor_asks(const JointInboxSlice &slice,
                                               std::span<const std::uint8_t> quoted,
                                               std::span<AmericanPriceBox> boxes,
                                               TerminalFloorRecovery &record);

// Coupled call/put feasible projection, including affine carry bounds and all calendar
// union knots. Fixed variables are eliminated. An infeasible-start primal-dual
// method can reach feasible boundary-only sets as well as strict interiors.
// Iteration exhaustion is NonConverged, never evidence of infeasible data.
// A quadratic distance to the input guides the search; the returned feasible
// point is not claimed to be its exact minimizer.
[[nodiscard]] JointProjectionResult
project_joint_in_box(std::span<const JointInboxSlice> slices,
                     std::size_t max_iterations = 100,
                     JointInboxDiagnostics *diagnostics = nullptr);

} // namespace atx::vol::detail
