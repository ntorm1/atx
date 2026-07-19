#pragma once

// Internal declaration for the AVX2 laned ANALYTIC American-put Greeks bundle (K3).
// Defined in american_greeks_avx2.cpp (built -mavx2 -mfma via the src/simd/*_avx2.cpp
// glob) and called only from american_batch.cpp behind an atx::vol::simd::have_avx2()
// + ship-gate guard. Not a public header.

#include <cstddef>
#include <optional>

#include "atx/vol/american.hpp" // AmericanGreeks, AlOpts

namespace atx::vol::simd::detail {

// K4 first-order tier selector: which cost-bearing boundary solves the caller needs.
// price + delta + gamma + theta ride the BASE boundary alone (always computed). The
// other greeks each gate a group of solves: vega/volga/vanna -> the sigma+/- solves;
// rho -> the r+/- solves; charm -> the wide S+/-2h speed stencils. A default-true
// GreekNeeds keeps the full 5-solve bundle (backward-compatible).
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

} // namespace atx::vol::simd::detail
