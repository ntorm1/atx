#pragma once

// Cross-expiry regularity for per-expiry quote fits.
//
// A TermPrior carries, for each fitted expiry of ONE underlier at ONE valuation
// instant, its ATM volatility and its smile SHAPE as volatility multiples
// sigma(z)/sigma_ATM on a fixed standardized-moneyness grid
// (z = ln(K/F) / (sigma_ATM * sqrt(T)), the SpiderRock "LogStd" axis). A query at
// another variance time returns an interpolated ATM level and shape plus a
// confidence that decays with log-time distance to the nearest node. Sparse
// expiries use it as a prior, so they borrow level and shape from their
// neighbours instead of being refused.
//
// Interpolation rules (all in VARIANCE time):
//  * ATM total variance theta = sigma_ATM^2 * T is made non-decreasing across
//    nodes by weighted pool-adjacent-violators (calendar monotonicity of the ATM
//    level), then interpolated linearly in T; beyond the end nodes the ATM vol is
//    held flat (theta proportional to T), which keeps theta monotone.
//  * Shape multiples are interpolated linearly in T between the bracketing nodes
//    and held flat beyond the ends.
//  * confidence = exp(-|ln(T / T_nearest)| / decay_log_time) in (0, 1].
//
// Allocation: `create` allocates O(nodes); `at` is allocation-free (fixed-size
// stack buffers bounded by kTermPriorMaxNodes). Immutable after construction;
// concurrent const queries are safe.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp" // Result

namespace atx::vol {

// Central SpiderRock LogStd knots (|z| <= 3.75). Shapes beyond are held flat.
inline constexpr std::array<double, 17> kTermPriorGrid = {
    -3.75, -2.75, -2.0, -1.5, -1.0, -0.75, -0.5, -0.25, 0.0,
    0.25,  0.5,   0.75, 1.0,  1.5,  2.0,   2.75, 3.75};
inline constexpr std::size_t kTermPriorGridSize = kTermPriorGrid.size();
// Bound on nodes per surface; one node per listed expiry of one underlier.
inline constexpr std::size_t kTermPriorMaxNodes = 128;

struct TermPriorNode {
  double variance_years{};                              // > 0, distinct across nodes
  double atm_vol{};                                     // > 0
  std::array<double, kTermPriorGridSize> multiples{};   // > 0, on kTermPriorGrid
  double weight{};                                      // > 0, relative node reliability
  std::uint64_t id{};                                   // caller key for leave-one-out
};

struct TermPriorPoint {
  double atm_vol{};
  std::array<double, kTermPriorGridSize> multiples{};
  double confidence{}; // (0, 1]
  std::size_t nodes_used{};
};

struct TermPriorConfig {
  double decay_log_time{1.0}; // > 0; e-folding distance in |ln(T/T_nearest)|
};

class TermPrior {
public:
  // Validates finite positive times/levels/weights/multiples, distinct times and
  // at most kTermPriorMaxNodes nodes; copies and sorts them by time. An empty
  // node set is valid and answers every query with nullopt.
  [[nodiscard]] static Result<TermPrior> create(std::span<const TermPriorNode> nodes,
                                                const TermPriorConfig &config = {});

  // Prior at `variance_years` (> 0) from every node except the one whose id is
  // `exclude` (leave-one-out). The monotone ATM adjustment is recomputed without
  // the excluded node. Returns nullopt when no node remains or the time is invalid.
  [[nodiscard]] std::optional<TermPriorPoint>
  at(double variance_years, std::optional<std::uint64_t> exclude = std::nullopt) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

private:
  TermPrior() = default;
  std::vector<TermPriorNode> nodes_; // sorted by variance_years
  TermPriorConfig config_{};
};

} // namespace atx::vol
