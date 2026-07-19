#pragma once

// Internal declaration for the AVX2 laned ANALYTIC American-put Greeks bundle (K3).
// Defined in american_greeks_avx2.cpp (built -mavx2 -mfma via the src/simd/*_avx2.cpp
// glob) and called only from american_batch.cpp behind an atx::vol::simd::have_avx2()
// + ship-gate guard. Not a public header.

#include <cstddef>
#include <optional>

#include "atx/vol/american.hpp" // AmericanGreeks, AlOpts

namespace atx::vol::simd::detail {

// Compute the full 8-Greek analytic bundle + price for up to `n` genuine
// early-exercise American PUTS, 4 lanes at a time, matching scalar american_greeks_al
// within the documented economic gate. For each lane l that was solved on all five
// bump states, `out_greeks[l]` is filled and `handled[l]` set true; a lane that is not
// genuine early-exercise on every state (or non-finite) is left `handled[l] = false`
// for the caller's scalar american_greeks_al patch. `out_greeks` and `handled` must
// have length >= n. noexcept + allocation-free.
void american_put_greeks_batch_avx2(const double* S, const double* K, const double* T,
                                    const double* sigma, const double* r, const double* q,
                                    std::size_t n, const std::optional<AlOpts>& opts,
                                    AmericanGreeks* out_greeks, bool* handled) noexcept;

} // namespace atx::vol::simd::detail
