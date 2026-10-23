#pragma once

// K2 (r5, 2026-10-22): the batch IV inversion's typed per-lane outcome (the contract is in
// american_iv_batch.hpp). Its own light header so the Premium route's stats
// (american_iv_premium.hpp) carry the native attempt's outcome without the family
// internals. NOT a public API.

#include <cstddef>
#include <cstdint>

namespace atx::vol::iv_batch {

enum class InverseStatus : std::uint8_t {
  Inactive = 0,      // masked off (active[i] == 0): nothing computed, outputs untouched
  Converged = 1,     // root found (Newton distance <= tol, or an evaluated bracket <= 2 tol)
  LowVega = 2,       // the cap (or a collapsed bracket) on a map whose slope is <= the floor
                     // and not on the exercise plateau: σ not identified to tol
  ExerciseFlat = 3,  // the target sits on the exercise plateau (price at intrinsic, vega 0):
                     // price-only / an interval, never a unique σ
  SigmaBelowBox = 4, // P(σ_lo) > target: the root lies below the family's σ box
  SigmaAboveBox = 5, // P(σ_hi) < target: the root lies above the family's σ box
  QOutsideBox = 6,   // q[i] outside the family's q box (no evaluation)
  NonConverged = 7,  // the evaluation cap with a slope above the floor; bracket retained
  InvalidInput = 8,  // a non-finite / non-positive K, target or S, a non-finite q, the
                     // wrong side for the family, an inverted bracket, a bad anchor
  UnsupportedIsa = 9 // InverseIsa::Avx2 required and not executable (host or source)
};
inline constexpr std::size_t kInverseStatusCount = 10;

[[nodiscard]] const char *inverse_status_name(InverseStatus s) noexcept;

} // namespace atx::vol::iv_batch
