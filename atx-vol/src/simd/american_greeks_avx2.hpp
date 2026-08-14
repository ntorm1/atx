#pragma once

// Internal declaration for the AVX2 laned ANALYTIC American-put Greeks bundle (K3).
// Defined in american_greeks_avx2.cpp (built -mavx2 -mfma via the src/simd/*_avx2.cpp
// glob) and called only from american_batch.cpp behind an atx::vol::simd::have_avx2()
// + ship-gate guard. Not a public header.

#include <cstddef>
#include <optional>

#include "atx/vol/api/pricing/american.hpp" // AmericanGreeks, AlOpts

namespace atx::vol::simd::detail {

// K4 first-order tier selector: which cost-bearing boundary solves the caller needs.
// price + delta + gamma + theta ride the BASE boundary alone (always computed). The
// other greeks each gate a group of solves: vega/volga/vanna -> the sigma+/- solves;
// rho -> the r+/- solves; charm -> the wide S+/-2h speed stencils. A default-true
// GreekNeeds keeps the full 5-solve bundle (backward-compatible).
//
// FIX-5/M2: this is one of TWO independent GreekNeeds structs — the other is
// `atx::vol::GreekNeeds` in include/atx/vol/priced_surface.hpp, which is what every
// caller above american_boundary_batch.cpp speaks. There is no converting constructor
// and no static_assert between them; they meet in american_boundary_batch.cpp, which
// now initializes this one by DESIGNATED initializer, so a field reorder on either
// side can no longer silently mis-map the mask. Keep the field NAMES
// (vega/rho/charm) identical across the two; the ORDER is no longer load-bearing.
struct GreekNeeds {
    bool vega = true;  // vega, volga, vanna (the sigma+/- solves)
    bool rho = true;   // rho (the r+/- solves)
    bool charm = true; // charm's 5-point speed stencil
};

// Compute the analytic greeks bundle + price for up to `n` genuine early-exercise
// American PUTS, 4 lanes at a time, matching scalar american_greeks_al within the
// documented economic gate. `needs` (K4) skips the boundary solves the requested
// greeks don't need. For each lane l solved on all needed bump states, `out_greeks[l]`
// is filled (unrequested greeks left 0) and `handled[l]` set true; a lane not genuine
// early-exercise on every needed state (or non-finite in a needed greek) is left
// `handled[l] = false` for the caller's scalar patch. Lengths >= n. noexcept.
void american_put_greeks_batch_avx2(const double* S, const double* K, const double* T,
                                    const double* sigma, const double* r, const double* q,
                                    std::size_t n, const std::optional<AlOpts>& opts,
                                    AmericanGreeks* out_greeks, bool* handled,
                                    GreekNeeds needs = {}) noexcept;

// Call-native mirror (P1b). Same contract as american_put_greeks_batch_avx2 but for a
// span of genuine early-exercise American CALLS, matching scalar american_greeks_al(...,
// Side::Call). Under the McDonald-Schroder map C(S,K,r,q)=P(K,S,q,r) each call reduces to
// an internal put solved at internal-strike=S (the call spot), internal-rate=q, internal-
// yield=r, priced at internal-spot=K (the fixed call strike). The call's spot stencils
// vary the internal STRIKE (rescaled by strike homogeneity, one solve per bump state), and
// theta/charm come from the continuation-region PDE in the ORIGINAL (S,r,q) with the call
// intrinsic S-K. Eligibility is governed by the internal-put short rate q (American iff
// q>0); the r± stencils bump the internal YIELD only, so no rate-regime crossing is
// possible on a call (unlike the put's r-hr guard). Lanes not genuine early-exercise on
// every needed bump state, or non-finite, are left handled[l]=false for the scalar patch.
void american_call_greeks_batch_avx2(const double* S, const double* K, const double* T,
                                     const double* sigma, const double* r, const double* q,
                                     std::size_t n, const std::optional<AlOpts>& opts,
                                     AmericanGreeks* out_greeks, bool* handled,
                                     GreekNeeds needs = {}) noexcept;

} // namespace atx::vol::simd::detail
