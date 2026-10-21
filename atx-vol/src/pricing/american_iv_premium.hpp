#pragma once

// De-Americanised (early-exercise-premium) American IV inversion with per-contract
// memory (lane L2e, 2026-10-19). Internal header (src/pricing); the public entry is
// IvOpts::strategy = IvStrategy::Premium in american_iv.hpp, whose contract this is.
//
// A caller that inverts several prices of ONE contract (the de-Am IV cache inverts a
// quote's mid, bid and ask) may pass an IvPremiumMemory: the cold Andersen-Lake points
// of an earlier inversion fix the remainder D = P_am - A of the seed leg and its sigma
// chord, so a later price of the same contract usually needs one cold solve (seed from
// the chord, accept with the chord of that solve and the remembered point). The memory
// only moves the seed: acceptance, tolerance and fallback are the same, so an answer
// differs by at most the accepted tolerance with or without it. Callers that need
// order-independent answers pass each later price a COPY of one fixed earlier
// inversion's memory (the cache: the mid's). It must only be shared between calls
// with identical S, K, T, r, q, side, AlOpts and seed_profile.

#include <array>
#include <cstddef>
#include <cstdint>

#include "atx/vol/api/core/types.hpp"
#include "atx/vol/api/pricing/american_iv.hpp"

namespace atx::vol {

// Why a Premium inversion handed the price to the Search route (None: accepted).
enum class IvPremiumFallback : std::uint8_t {
  None = 0,           // the premium root was accepted
  NotApplicable = 1,  // route Premium does not cover, or inputs the Search route refuses
  RangeEdge = 2,      // price at the identifiable-range edges or in the non-monotone band
  NoEuropeanSeed = 3, // no European IV for the quote or for its de-Americanised target
  NotIncreasing = 4,  // a cold-map chord that is not a usable increasing slope
  VolBound = 5,       // an iterate or the root at/below kIvMin or above the search ceiling
  PricerFailure = 6,  // the Andersen-Lake kernel refused at an iterate
  NoConvergence = 7,  // not accepted within kIvPremiumMaxSolves cold solves
};
inline constexpr std::size_t kIvPremiumFallbackCount = 8;

// Cold iterate solves one Premium attempt may spend before it falls back (plus one
// accurate confirming solve when IvOpts::seed_profile selects another kernel), and the
// extra solves it may add one at a time while the Newton distance keeps shrinking by at
// least kIvPremiumShrink per solve (SPY graded-v1: 392 of the 395 budget fallbacks were
// still converging geometrically at the 4th solve).
inline constexpr std::uint32_t kIvPremiumMaxSolves = 4;
inline constexpr std::uint32_t kIvPremiumExtraSolves = 2;
inline constexpr double kIvPremiumShrink = 0.25;
// Confirming solves of a seed-tier root on the confirm kernel (SPY1s Phase 1): the first
// confirms when its Newton distance is <= tol; otherwise its Newton step is taken and one
// more solve confirms (a Screen root sits up to 7.8e-4 max(P, 1) off the accurate map).
inline constexpr std::uint32_t kIvPremiumConfirmSolves = 2;

// The cold Andersen-Lake points of one contract: sigma and the remainder D = P_am - A
// of the seed leg A (Barone-Adesi-Whaley where the contract has an exercise region,
// Black-76 elsewhere; american_iv.cpp `Leg`).
struct IvPremiumMemory {
  std::array<double, 2> sigma{};
  std::array<double, 2> rest{};
  std::uint8_t points = 0; // valid entries, most recent first
};

struct IvPremiumStats {
  std::uint32_t premium_solves = 0; // cold Andersen-Lake solves of the Premium attempt
  IvPremiumFallback fallback = IvPremiumFallback::NotApplicable;
  // Accepted roots: |P_am - price| (USD) at the last accurate solve before the final
  // Newton correction, an upper bound on the served root's own residual.
  double last_residual_usd = 0.0;
  // Seed-tier wiring (IvOpts::seed_profile; SPY1s Phase 1, 2026-10-20). Of
  // premium_solves: the confirming solve on the confirm kernel (0 or 1) and the band
  // gate's chord solve on the seed tier (0 or 1); the rest are iterates. seed_nodes: the
  // boundary node count of the scheme the iterates ran on (al_screen_opts() 7; the
  // accurate preset 12). confirm_profile: the kernel profile of the confirming solve when
  // one ran. seed_routed: the price lay inside the seed tier's non-monotone gate, so the
  // iterates ran on the accurate kernel and no confirm was needed. confirm_skipped: the
  // band gate (IvPremiumGate) accepted the seed-tier root without a confirm.
  // gate_bound_vol: the gate's (eps_A + eps_r) / v_min in vol units when it was
  // evaluated (NaN otherwise) - the bound the skipped root carries.
  std::uint32_t confirm_solves = 0;
  std::uint32_t gate_solves = 0;
  std::uint16_t seed_nodes = 0;
  AlProfile confirm_profile = AlProfile::Reference;
  bool seed_routed = false;
  bool confirm_skipped = false;
  double gate_bound_vol = std::numeric_limits<double>::quiet_NaN();
};

// Tested price bounds of the seed tiers versus the accurate Reference kernel (american.hpp
// al_screen_opts() / AlProfile::Fast; pinned by AlScreenProfile.* and AlFastProfile.* in
// tests/american_test.cpp): |dP| <= bound * max(P, 1). Screen: 7.8e-4 on the whole
// grid, 4.6e-6 for T <= 0.1. Fast: 1e-12. Reference: 0 (it IS the accurate kernel).
inline constexpr double kAlScreenPriceBound = 7.8e-4;
inline constexpr double kAlScreenShortDatedPriceBound = 4.6e-6;
inline constexpr double kAlScreenShortDatedYears = 0.1;
inline constexpr double kAlFastPriceBound = 1.0e-12;
inline constexpr std::uint16_t kAlAccurateCollocation = 12; // the nullopt preset's nodes

[[nodiscard]] inline double al_profile_price_bound(AlProfile profile, double price,
                                                   double years) noexcept {
  const double scale = price > 1.0 ? price : 1.0;
  switch (profile) {
  case AlProfile::Reference:
    return 0.0;
  case AlProfile::Fast:
    return kAlFastPriceBound * scale;
  case AlProfile::Screen:
    return (years <= kAlScreenShortDatedYears ? kAlScreenShortDatedPriceBound
                                              : kAlScreenPriceBound) *
           scale;
  }
  return kAlScreenPriceBound * scale; // unreachable: every enumerator returns above
}

// Confirm policy of a seed-tier Premium root (research R2 / gap report 5a; SPY1s Phase
// 1). Always: every accepted root is confirmed by one solve on the confirm kernel (the
// historical seed_profile behaviour). Band: the confirm is skipped only when the
// acceptance statement (eps_A + eps_r) / v_min < half_spread_usd / k holds, with eps_A
// the seed tier's tested price bound at this contract, eps_r the root's own residual on
// the seed map, v_min the seed map's chord slope at the root less its chord bound,
// floored at min_relative_slope * max(price, 1); a non-positive v_min, a price inside
// the seed tier's non-monotone gate, a non-positive half-spread or a root at the
// volatility bounds always confirms. Never: the seed-tier root is accepted as is
// (measurement arm only; never a candidate default).
enum class IvConfirmPolicy : std::uint8_t { Always = 0, Band = 1, Never = 2 };

struct IvPremiumGate {
  IvConfirmPolicy policy = IvConfirmPolicy::Always;
  double k = 10.0;                 // > 0
  double half_spread_usd = 0.0;    // the quote's half-spread in price (<= 0: confirm)
  double min_relative_slope = 0.0; // floor on v_min relative to max(price, 1)
  // The confirming solve's kernel: nullopt = opts.al_opts (the accurate kernel), else
  // this scheme (the R3 measurement arm passes al_fast_profile(opts.al_opts)).
  std::optional<AlOpts> confirm_opts = std::nullopt;
  // Seed-tier routing gate (SPY1s Phase 2, opt-in): > 0 routes a price to the accurate
  // iterates only when price - zero_vol <= seed_gate_kappa * K * max(1, T^2) instead of
  // inside the seed tier's own non-monotone gate (al_non_monotone_gate of the seed opts;
  // Screen 4e-4, about 100x the dip L2f measured, 4.3e-6). 0 (the default): the seed
  // tier's own gate, bit for bit. Every root outside the gate is still confirmed (or
  // gated) on the confirm kernel exactly as before; only which iterates run changes.
  double seed_gate_kappa = 0.0;
};

// The seed-tier routing gate of a Premium attempt: gate->seed_gate_kappa when set, else
// the seed tier's own non-monotone gate (NaN for an invalid K or T, which routes).
[[nodiscard]] double iv_premium_seed_gate(double K, double T, const std::optional<AlOpts> &seed_opts,
                                          const IvPremiumGate *gate) noexcept;

// IvOpts-typed inversion under the Premium strategy with optional per-contract memory
// (nullptr: none). `opts.strategy` is ignored (this IS Premium); every other field
// means what it means in american_implied_vol(..., const IvOpts &), including
// refuse_non_monotone. stats.fallback says whether the Premium root was served; on a
// fallback the result equals the Search route's bit for bit. `gate` (nullptr: Always on
// opts.al_opts) governs the confirm of a seed-tier root (IvPremiumGate); it is ignored
// when seed_profile is unset or al_opts' own profile (nothing to confirm).
[[nodiscard]] IvResult american_implied_vol_premium(double price, double S, double K, double T,
                                                    double r, double q, Side side,
                                                    const IvOpts &opts, IvPremiumMemory *memory,
                                                    IvPremiumStats &stats,
                                                    const IvPremiumGate *gate = nullptr) noexcept;

} // namespace atx::vol
