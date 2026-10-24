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
  double sigma_box_pad{0.05};       // box = [min sigma * (1 - pad), max sigma * (1 + pad)]
                                    // (lane A2: 0.25 -> 0.05, see ADAPTIVE NODE COUNT)
  double sigma_box_floor{0.02};     // never below kAlSigmaFamilyMinSigma + headroom
  bool cold_near_edge{true};        // family `near_edge` lane => whole lane cold (above)
  bool cold_fallback{true};         // false => refused lanes stay Refused
  bool analytic_cold{true};         // cold route: analytic (true) or FD (false)
  std::optional<AlOpts> al_opts{};  // see "Pricer options precedence"
  unsigned n_threads{0};            // 0 => executor's auto degree

  // ── Appended by lane A2 (2026-10-24, additive): family economics on sparse books ──
  //
  // ADAPTIVE NODE COUNT. A group's family gets
  //   n = clamp(ceil(log(sigma_hi / sigma_lo) / sigma_node_spacing) + 1, floor, n_sigma),
  //   floor = min(n_sigma_min, n_sigma)
  // nodes, so a narrow box is not solved at the wide-box count. `n_sigma` is the ceiling
  // and caps the floor (n_sigma = 4 solves every family at 4 nodes under the default
  // floor); n_sigma_min >= n_sigma (and pad 0.25) restores the pre-A2 fixed count.
  // Calibrated against the 1e-6 family-vs-cold price gate on BOTH bench stores
  // (atx-vol-risk-bench --kernel both, cold.max_rel_price_family; lane A2 report) and the
  // fixture books (RiskKernel.DISABLED_AdaptiveNodeSpacingSweep): the binding regime is a
  // narrow LONG-DATED box (T 0.5 - 2.3 y, log width ~ 0.2), where a 4-node family misses
  // the gate (licensed 2.9e-6); with n >= 5 and h = 0.06 the worst gap is 9.3e-8
  // (licensed) / 2.5e-8 (synthetic) at 5.2 / 7.7 nodes per family (12 before), and the
  // volga gap is back at the 12-node level. The box is rebuilt from each state's own
  // sigmas, so the pad buys no containment, only width.
  std::uint16_t n_sigma_min{5};     // >= 4; capped at n_sigma (the effective floor)
  double sigma_node_spacing{0.06};  // target log-sigma spacing h of the rule (> 0)
  // FAMILY ECONOMICS. A family costs n * family_node_cost_us to build and serves its
  // lanes at a few us; the cold route costs cold_lane_cost_us per lane. A group with
  // n * family_node_cost_us >= n_eligible * cold_lane_cost_us (both scaled by the
  // solve counts of the rho axis when RiskNeeds::rho) is priced cold-analytic
  // directly and flagged FamilyUneconomic. The decision reads only the group's static
  // shape (eligible lanes, box) and these constants, never a clock, so it is the same
  // for any thread count. Active only with cold_fallback and analytic_cold (the
  // economical route is the analytic one; with no cold route every group is tried).
  bool family_economics{true};
  // Measured IN SITU (RiskKernel.DISABLED_FamilyEconomicsCostSetup at 8 concurrent
  // workers, two samples on a loaded i7-1260P: 82 / 297 and 65 / 243 us; one thread gives
  // 33-36 / 140-149). Only their ratio (3.7 in situ, 4.2 on one thread) enters the rule.
  double family_node_cost_us{73.0}; // one sigma node of AlSigmaFamily::prepare
  double cold_lane_cost_us{270.0};  // one american_greeks_al lane (vega, charm)
  // BLOCK SCHEDULE. The (uid, expiry) blocks the kernel fans out are very uneven (a
  // short-tenor cold block, a one-lane family, a forty-lane family): false hands each
  // worker one contiguous static chunk (pricing_executor run_blocks), true lets workers
  // claim blocks one at a time (run_dynamic). Every block writes only its own rows,
  // slots and counters, so both give the same bits for any thread count. Dynamic is the
  // default: faster on both bench stores at 8 threads on a 4P + 8E host (lane A2).
  bool dynamic_schedule{true};
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
