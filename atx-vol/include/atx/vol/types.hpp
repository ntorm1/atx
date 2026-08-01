#pragma once

// Shared vocabulary for atx-vol: option side, exercise style, and the
// numerical constants that bound the pricing/IV kernels.
//
// Ported from ats-vol (C17 `ats_vol_types.h`). The C library encoded errors
// as a negative-integer `AtsVolStatus` enum; the C++ port routes expected
// failures through `atx::core::Result<T>` / `Status` (agent profile §4), so
// only the option/exercise enums and the numeric constants survive here.
//
// Thread-safety: every type in this header is a trivially-copyable value with
// no shared state — safe to read and copy from any thread.

#include <cstdint>

#include "atx/core/error.hpp"

namespace atx::vol {

// atx-core error vocabulary, re-exported so callers of atx-vol need only one
// include for the Result/Status machinery.
using atx::core::Error;
using atx::core::ErrorCode;
using atx::core::Result;
using atx::core::Status;

// ── Option side ─────────────────────────────────────────────────────────
enum class Side : std::uint8_t {
  Call = 0,
  Put = 1,
};

// ── Exercise style ──────────────────────────────────────────────────────
enum class ExerciseStyle : std::uint8_t {
  European = 0,
  American = 1,
};

// ── Quote-delta convention (E5 / AN-P2-6) ───────────────────────────────────
//
// THE library-wide delta vocabulary. Introduced in `projection.hpp` (its first
// consumer) and moved here by FIX-E M-9: a vocabulary shared by projection,
// analytics and portfolio risk belongs with the rest of the shared vocabulary,
// not behind an include that drags in `vol_surface.hpp`, `correction.hpp`,
// `rates_curve.hpp` and `universe.hpp` to name one enum.
//
// AN-P2-6 is a convention-FRAGMENTATION defect — analytics solved American
// |delta|, projection solved European B76 forward delta, and
// contract_projection solved American delta seeded from a carry-discounted spot
// inversion, i.e. three different answers to "what is the 25-delta strike". So
// this enum is extended IN PLACE; a second enum beside it would deepen exactly
// the disease it exists to cure.
//
// Not every consumer supports every convention. `projection.cpp`'s coordinate
// solves accept `Forward` ONLY and return NotImplemented otherwise. The
// analytics wing/RR/BF solves (`analytics.hpp`) accept both and default to
// `American`, which is their shipped behaviour.
enum class DeltaConvention : std::uint8_t {
  // European Black-76 FORWARD delta: N(d1) for a call, N(d1) - 1 for a put, with
  // d1 = (ln(F/K) + sigma^2*T/2) / (sigma*sqrt(T)). No discounting, no early
  // exercise. The vendor-standard quote convention.
  Forward = 0,
  // AMERICAN spot delta, dP/dS on the American mark — what the analytics
  // wings/RR/BF have always used (`resolve_strike_by_delta`, strategy.hpp). On a
  // high-carry or deep-ITM-early-exercise name this resolves a materially
  // different strike from `Forward`.
  American = 1,
};

// ── Pricing route (shared diagnostic tag) ─────────────────────────────────
//
// Which American pricing route a leg actually took. Ports the C
// `AtsVolPricingRoute`. Defined here (the shared vocabulary header) because it
// is BOTH a `profile.hpp` configuration input and a per-lane pricing diagnostic
// output — public headers that must agree on one type (they previously each
// defined their own, an ODR conflict for any TU that included both, e.g. via
// the `vol.hpp` umbrella). Numeric slot 3
// (`DISCRETE_DIV_FD_CACHE`) was removed in the C and stays reserved, so
// `PriorSurface` keeps its wire value of 4.
enum class PricingRoute : std::uint8_t {
  B76Only = 0,     // European Black-76
  B76AlCache = 1,  // Black-76 + cached American correction premium
  B76AlCold = 2,   // American, no correction cache (spot-delta scaling only)
  PriorSurface = 4,
};

// ── Numerical constants ─────────────────────────────────────────────────

// Below this year-fraction we treat the option as "at expiry" and collapse to
// discounted intrinsic to avoid sigma-coordinate blow-up. 1/525600 ≈ one
// minute in year units.
inline constexpr double kTMinEval = 1.0 / 525600.0;

// Implied-vol search bounds. Equity options effectively never sit outside
// [0.5%, 1000%] annualized lognormal vol. kIvMin is the unified IV floor (A6,
// core-review finding 6): it is BOTH the reported floor of every inverter and the
// lower bound of the American IV search bracket (american_iv.cpp kSigmaLo), so no
// representable IV sits below it and there is no bracket-vs-report discontinuity.
inline constexpr double kIvMin = 0.005;
inline constexpr double kIvMax = 10.0;

// IV solver tolerance in VOL units and iteration cap. A rational initial guess
// plus one Householder step reaches machine precision for almost all inputs; the
// cap is a bounded-loop guard (JPL Rule 2). `kIvTol` governs the vol-step
// termination test `|Δσ| < kIvTol` directly (Δσ is already in vol units).
inline constexpr double kIvTol = 1.0e-12;
inline constexpr int kIvMaxIter = 16;

// Multiplier on the price-residual rounding-noise floor `ε·df·max(F,K)` used by
// the price-residual termination test (K1). price_model is formed from the terms
// df·F·Φ(d1) and df·K·Φ(d2), each of magnitude ~df·max(F,K), so their difference
// carries an absolute rounding error of ~ε·df·max(F,K); the residual cannot be
// driven below that regardless of σ. Comparing the price residual against this
// notional-scaled floor (instead of the mis-scaled absolute `kIvTol`, which for
// high-notional options sits *below* the floor and never fires) terminates the
// inversion exactly when σ has reached machine precision. The 8× headroom keeps
// the test above the accumulated multi-term rounding noise.
inline constexpr double kIvResidNoiseFloor = 8.0;

} // namespace atx::vol
