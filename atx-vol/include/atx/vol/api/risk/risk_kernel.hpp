#pragma once

// RiskKernel — the strategy that fills a RiskFrame for a PreparedRiskBook at one
// MarketState. Two shipped kernels:
//
//   family  — THE HFT kernel. Per (uid, expiry_ns, side) group: one Andersen-Lake
//             geometry (T, r, q_eff), one AlSigmaFamily over the group's sigma
//             range (n_sigma node solves), then `family_greeks` per contract (price +
//             requested Greeks in ONE fused quadrature pass, ~5-7 us). A lane the
//             family refuses (outside the sigma box, tau below the family floor,
//             near the exercise edge, a NotImplemented regime) is handed to the cold
//             route in a second pass and flagged ColdFallback; `rho` costs a carry-
//             bump set per group and is built only when RiskNeeds::rho is set.
//             NEAR THE EXERCISE EDGE (the family's own kAlFamilyGreekEdgeBand, 0.25
//             sigma sqrt T: `family_greeks` serves price / delta / vega but refuses gamma /
//             theta / charm there) the WHOLE lane goes cold and is flagged
//             NearExerciseEdge | ColdFallback. It stays in the totals: the cold Greeks
//             there are the best the library has and the flag says so.
//   cold    — the oracle: american_greeks_al (analytic, 5 boundary solves) or
//             american_greeks_fd (central FD) per contract on the resolved
//             (S, K, T, sigma, r, q_eff). Slow; every test pins the family kernel
//             against it inside the family's box and away from the edge.
//
// FROZEN API CONTRACT (sprint 2026-10-24; see risk_types.hpp banner).
//
// ## Determinism
//
// Groups are fanned over the process pricing executor; each row is written by
// exactly one worker into its own slot; nothing is reduced here. The per-row result
// is a pure function of (book, state, needs, policy) — never of the thread count.
//
// ## Allocation (bounded, counted; not zero)
//
// After `RiskWorkspace::reserve` and `RiskFrame::reserve` for a book, the per-row work
// of `evaluate` allocates nothing. The family kernel does allocate per state: an
// `AlSigmaFamily` fixes its side and anchor at construction (a call family is
// re-anchored at the state's spot, so it is reconstructed: ~6 KB each), a geometry
// `prepare` reuses its block but a new one costs ~90 KB, and
// `AlSigmaFamilyCarryBumps::prepare` builds and frees its bumped families. The bound is
// O(n_groups) allocations per state and `FrameCounters` reports families_built /
// carry_sets_built; the lane test pins the count, not zero.
//
// ## Pricer options precedence
//
// The Andersen-Lake preset (`AlOpts`) a lane is priced with: the policy's `al_opts`
// when set; else the uid's surface `pricing().al_opts` when the state has a surface for
// the uid; else `al_fast_opts()`. The cold kernel prices on the RESOLVED (spot, K, T,
// sigma, rate, q_eff) through `american_greeks_al` / `_fd`, never through
// `PricedSurface::evaluate_batch`, so family and cold see identical inputs; on a state
// built by `market_state_from_surfaces` (CarryFrom::Surface, surface spot / rate) the
// cold analytic kernel equals `PricedSurface::greeks_analytic` bit for bit.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "atx/vol/api/backtest/query_pricing.hpp" // QueryExecution
#include "atx/vol/api/pricing/american.hpp"       // AlOpts
#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/risk/market_state.hpp"
#include "atx/vol/api/risk/risk_book.hpp"
#include "atx/vol/api/risk/risk_frame.hpp"
#include "atx/vol/api/risk/risk_types.hpp"

namespace atx::vol {

enum class RiskKernelKind : std::uint8_t { Family = 0, Cold = 1 };

struct FamilyKernelPolicy {
  std::uint16_t n_sigma{12};        // Chebyshev sigma nodes per family (4..16)
  double sigma_box_pad{0.25};       // box = [min sigma * (1 - pad), max sigma * (1 + pad)]
  double sigma_box_floor{0.02};     // never below kAlSigmaFamilyMinSigma + headroom
  bool cold_near_edge{true};        // family `near_edge` lane => whole lane cold (above)
  bool cold_fallback{true};         // false => refused lanes stay Refused
  bool analytic_cold{true};         // cold route: analytic (true) or FD (false)
  std::optional<AlOpts> al_opts{};  // see "Pricer options precedence"
  unsigned n_threads{0};            // 0 => executor's auto degree
};

struct ColdKernelPolicy {
  bool analytic{true};              // american_greeks_al vs american_greeks_fd
  std::optional<AlOpts> al_opts{};  // see "Pricer options precedence"
  unsigned n_threads{0};
};

// Mutable scratch owned by the caller; never shared between overlapping calls.
// Retains the family / geometry state for the last book shape evaluated, so a
// sequence of states over one book rebuilds families but allocates nothing.
class RiskWorkspace {
public:
  RiskWorkspace();
  ~RiskWorkspace();
  RiskWorkspace(RiskWorkspace &&) noexcept;
  RiskWorkspace &operator=(RiskWorkspace &&) noexcept;
  RiskWorkspace(const RiskWorkspace &) = delete;
  RiskWorkspace &operator=(const RiskWorkspace &) = delete;

  // Pre-size for a prepared book. Idempotent, grow-only.
  void reserve(const PreparedRiskBook &book);

private:
  friend class RiskKernel;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class RiskKernel {
public:
  RiskKernel();
  ~RiskKernel();
  RiskKernel(RiskKernel &&) noexcept;
  RiskKernel &operator=(RiskKernel &&) noexcept;
  RiskKernel(const RiskKernel &) = delete;
  RiskKernel &operator=(const RiskKernel &) = delete;

  [[nodiscard]] static RiskKernel family(const FamilyKernelPolicy &policy = {});
  [[nodiscard]] static RiskKernel cold(const ColdKernelPolicy &policy = {});

  [[nodiscard]] RiskKernelKind kind() const noexcept;
  [[nodiscard]] const FamilyKernelPolicy &family_policy() const noexcept; // valid iff Family
  [[nodiscard]] const ColdKernelPolicy &cold_policy() const noexcept;     // valid iff Cold

  // Fill `out` for `book` at `state`. `out` is resized to the book (reserve is
  // grow-only). Per-lane failures land in `out.status` / `out.flags`; the call fails
  // (InvalidArgument) only on a structural error: a workspace / frame bound to a
  // different book, an unprepared book, a state whose TimeSpec refuses (OutOfRange
  // on VolTime). On a structural failure `out` is left in `clear_values()` state.
  [[nodiscard]] Status evaluate(const PreparedRiskBook &book, const MarketState &state,
                                RiskNeeds needs, RiskFrame &out, RiskWorkspace &ws) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::vol
