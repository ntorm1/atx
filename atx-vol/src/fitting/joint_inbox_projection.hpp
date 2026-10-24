#pragma once

#include <array>
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

// Private research selection. Compiled uses the linked solver's explicit build
// mode; Original always bypasses optional corrections. An unavailable Centrality
// selection is InvalidInput, never silently relabeled as an experimental result.
enum class JointDirectionMode { Compiled, Original, Centrality };
[[nodiscard]] bool joint_centrality_compiled() noexcept;

// Optional observational profiling, never consulted by the solve. Null profile
// performs no clock reads. Times are cumulative nanoseconds; iteration includes
// residual/assembly/factor/direction/update, and total includes all stages.
struct JointInboxProfile {
  std::uint64_t total_ns = 0;
  std::uint64_t build_ns = 0;
  std::uint64_t symbolic_ns = 0;
  std::uint64_t iteration_ns = 0;
  std::uint64_t residual_ns = 0;
  std::uint64_t assembly_ns = 0;
  std::uint64_t factor_ns = 0;
  std::uint64_t rhs_ns = 0;
  std::uint64_t linear_solve_ns = 0;
  std::uint64_t slack_direction_ns = 0;
  std::uint64_t update_ns = 0;
  std::uint64_t certificate_ns = 0;
  std::array<std::size_t, 5> row_arities{};
  std::size_t bound_rows = 0;
  std::size_t row_storage_bytes = 0;
  std::size_t matrix_nonzeros = 0;
  std::size_t factor_nonzeros = 0; // strict lower L; D has one entry per variable
  std::size_t factorizations = 0;
  std::size_t directions = 0;
  bool centrality_enabled = false;
  std::uint64_t centrality_setup_ns = 0;
  std::uint64_t centrality_ns = 0; // nested within iteration/total, never additive
  std::size_t centrality_attempts = 0, centrality_solves = 0, centrality_accepted = 0;
  std::size_t centrality_zero_target = 0, centrality_nonfinite = 0;
  std::size_t centrality_nonimproving = 0, centrality_solve_refused = 0;
  std::size_t centrality_invalid_target = 0, centrality_input_refusals = 0;
  std::size_t centrality_setup_refusals = 0, centrality_allocation_refusals = 0;
  // Reserved/requested numeric payload plus the fixed workspace object;
  // excludes allocator metadata and existing Newton factor storage.
  std::size_t centrality_workspace_bytes = 0;
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
                     JointInboxDiagnostics *diagnostics = nullptr,
                     JointInboxProfile *profile = nullptr,
                     JointDirectionMode mode = JointDirectionMode::Compiled);

} // namespace atx::vol::detail
