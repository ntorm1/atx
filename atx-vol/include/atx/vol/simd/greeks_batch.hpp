#pragma once

// Batched (SoA) Black-76 Greeks — the vectorized risk hot path.
//
// Companion to atx/vol/simd/black76_batch.hpp: for a whole array of contracts
// this returns, in one call, the premium AND all eight analytic sensitivities
// (atx::vol::Greeks) per contract. It dispatches to a 4-lane AVX2 kernel when
// the host supports it (atx::vol::simd::have_avx2()) and to a scalar loop
// otherwise. The scalar loop calls the exact per-contract atx::vol::black76_greeks,
// so it is the numerical source of truth; the AVX2 path reproduces it to ~1e-9
// absolute (degenerate and deep-wing lanes are patched through the scalar
// kernel, so parity is exact there).
//
// Layout: structure-of-arrays. Each input array has length n; F/K/T/sigma/r/df
// are contiguous doubles, side is one Side per contract. greeks_out holds n
// Greeks and price_out n doubles; the outputs must not alias the inputs.
// Passing n == 0 is a no-op.
//
// The function is noexcept and allocation-free — safe to call concurrently from
// any threads (no shared mutable state; the CPUID cache is init-once).

#include <cstddef>

#include "atx/vol/greeks.hpp" // Greeks
#include "atx/vol/types.hpp"  // Side

namespace atx::vol::simd {

// Black-76 Greeks + premium for each contract:
//   {greeks_out[i], price_out[i]} = black76_greeks(F[i], K[i], T[i], sigma[i],
//                                                   r[i], df[i], side[i]).
// Degenerate (T <= 0 or sigma <= 0) collapses to the intrinsic-step delta with
// the other Greeks zero, exactly as the scalar kernel.
void black76_greeks_batch(const double* F, const double* K, const double* T,
                          const double* sigma, const double* r,
                          const double* df, const Side* side,
                          Greeks* greeks_out, double* price_out,
                          std::size_t n) noexcept;

} // namespace atx::vol::simd
