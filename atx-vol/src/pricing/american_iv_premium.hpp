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
};

// IvOpts-typed inversion under the Premium strategy with optional per-contract memory
// (nullptr: none). `opts.strategy` is ignored (this IS Premium); every other field
// means what it means in american_implied_vol(..., const IvOpts &), including
// refuse_non_monotone. stats.fallback says whether the Premium root was served; on a
// fallback the result equals the Search route's bit for bit.
[[nodiscard]] IvResult american_implied_vol_premium(double price, double S, double K, double T,
                                                    double r, double q, Side side,
                                                    const IvOpts &opts, IvPremiumMemory *memory,
                                                    IvPremiumStats &stats) noexcept;

} // namespace atx::vol
