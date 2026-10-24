#pragma once

// risk_types — the shared vocabulary of the atx-vol risk engine (api/risk/*).
//
// FROZEN API CONTRACT (sprint 2026-10-24, design
// docs/superpowers/specs/2026-10-24-atx-vol-risk-engine-design.md). Every lane
// builds against this header as committed on the sprint base; a change to it is
// requested in a lane report and applied on the base by the orchestrator, never
// edited in a lane.
//
// The engine's one rule about numbers: a value is served WITH its route and its
// flags, or it is not served. A refused lane is excluded from every total, counted,
// and its base premium is reported. No NaN ever enters a sum.

#include <cstddef>
#include <cstdint>

#include "atx/vol/api/core/types.hpp" // Result, Status, Side

namespace atx::vol {

// How a per-contract (price, Greeks) row was produced.
enum class RiskRoute : std::uint8_t {
  None = 0,         // not evaluated (default-constructed slot)
  Family = 1,       // sigma-family fused pass (american.hpp family_greeks)
  ColdAnalytic = 2, // american_greeks_al / PricedSurface::greeks_analytic (5 boundary solves)
  ColdFd = 3,       // american_greeks_fd (central FD, ~17 solves)
  Exercised = 4,    // intrinsic served with the exercise Greeks (delta ±1, rest 0)
  Share = 5,        // a delta-one share leg: mark = spot, delta = 1
  Refused = 6,      // no number: see RiskLaneStatus / flags
};

// Per-lane provenance bits (OR-ed into RiskFlags). A bit is a statement about the
// served number, never a reason to drop it: dropping is RiskLaneStatus' job.
enum class RiskFlag : std::uint32_t {
  None = 0,
  TenorExtrapolated = 1u << 0,     // T outside the surface's fitted [min_T, max_T]
  MoneynessExtrapolated = 1u << 1, // k outside the slice's data-supported range
  NearExerciseEdge = 1u << 2,      // inside the edge band: gamma/theta/charm suspect
  MarkMissing = 1u << 3,           // no ContractMarks entry; sigma came from the surface
  SurfaceMissing = 1u << 4,        // sigma came from marks; no surface for this uid
  OutsideFamilyBox = 1u << 5,      // sigma outside the family's sigma box -> cold route
  Rolled = 1u << 6,                // T <= 0 at this state (expired / rolled past expiry)
  PivotFromMarks = 1u << 7,        // vega-split pivots sampled from marks, not a surface
  VendorParityAvailable = 1u << 8, // vendor price / delta present for this contract
  ColdFallback = 1u << 9,          // family kernel handed this lane to the cold route
  CarryFromSurface = 1u << 10,     // forward / q_eff read from the surface, not the state
};
using RiskFlags = std::uint32_t;

[[nodiscard]] constexpr RiskFlags operator|(RiskFlag a, RiskFlag b) noexcept {
  return static_cast<RiskFlags>(a) | static_cast<RiskFlags>(b);
}
[[nodiscard]] constexpr RiskFlags operator|(RiskFlags a, RiskFlag b) noexcept {
  return a | static_cast<RiskFlags>(b);
}
constexpr RiskFlags &operator|=(RiskFlags &a, RiskFlag b) noexcept {
  a = a | b;
  return a;
}
[[nodiscard]] constexpr bool has_flag(RiskFlags value, RiskFlag flag) noexcept {
  return (value & static_cast<RiskFlags>(flag)) != 0u;
}

// Why a lane has, or has not, a number. `Ok` lanes enter totals; every other
// value is an exclusion reason counted (with its base premium) in the outputs.
enum class RiskLaneStatus : std::uint8_t {
  Ok = 0,
  ModelUnavailable = 1, // uid has neither marks nor a surface in the state
  NumericError = 2,     // pricer / Greek failure, non-finite output
  InvalidContract = 3,  // K <= 0, non-finite inputs, unknown side
  Rolled = 4,           // T <= 0 at the state (expired between base and shifted)
  Refused = 5,          // typed refusal (near edge with no cold route, policy Forbid, ...)
};
inline constexpr std::size_t kRiskLaneStatusCount = 6;

// Which Greek axes an evaluation must materialize. Price, delta, gamma, vega and
// theta are always computed. `second_order` adds vanna / volga; `rho` adds the rate
// axis (on the family kernel this costs the carry-bump families, so it is OFF by
// default — explain turns it on exactly when the two states' rates differ);
// `charm` adds d(delta)/dt.
struct RiskNeeds {
  bool second_order{true};
  bool rho{false};
  bool charm{true};
  [[nodiscard]] bool operator==(const RiskNeeds &) const = default;
};

[[nodiscard]] const char *to_string(RiskRoute value) noexcept;
[[nodiscard]] const char *to_string(RiskLaneStatus value) noexcept;
// Name of ONE flag bit (the lowest set bit of `value`); "" for None.
[[nodiscard]] const char *to_string(RiskFlag value) noexcept;

} // namespace atx::vol
