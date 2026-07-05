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

// ── Pricing route (shared diagnostic tag) ─────────────────────────────────
//
// Which American pricing route a leg actually took. Ports the C
// `AtsVolPricingRoute`. Defined here (the shared vocabulary header) because it
// is BOTH a `profile.hpp` configuration input and a `portfolio.hpp`/`bulk`
// per-lane diagnostic output — two public headers that must agree on one type
// (they previously each defined their own, an ODR conflict for any TU that
// included both, e.g. via the `vol.hpp` umbrella). Numeric slot 3
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
// [0.5%, 1000%] annualized lognormal vol.
inline constexpr double kIvMin = 0.005;
inline constexpr double kIvMax = 10.0;

// IV solver tolerance (absolute vol units) and iteration cap. A rational
// initial guess plus one Householder step reaches machine precision for
// almost all inputs; the cap is a bounded-loop guard (JPL Rule 2).
inline constexpr double kIvTol = 1.0e-12;
inline constexpr int kIvMaxIter = 16;

} // namespace atx::vol
