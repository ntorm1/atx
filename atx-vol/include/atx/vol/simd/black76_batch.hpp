#pragma once

// Batched (SoA) Black-76 European pricing — the vectorized pricing hot path.
//
// These entry points price a whole array of contracts in one call, dispatching
// to a 4-lane AVX2 kernel when the host supports it (atx::vol::simd::have_avx2())
// and to a scalar loop otherwise. The scalar loop calls the exact per-contract
// kernels in atx/vol/black76.hpp, so it is the numerical source of truth; the
// AVX2 path reproduces it to ~1e-12 absolute (degenerate and deep-wing lanes are
// patched through the scalar kernel, so parity is exact there).
//
// Layout: structure-of-arrays. Each input array has length n; F/K/T/sigma/df are
// contiguous doubles, side is one Side per contract. Output arrays have length n
// and must not alias the inputs. Passing n == 0 is a no-op.
//
// All functions are noexcept and allocation-free — safe to call concurrently
// from any threads (no shared mutable state; the CPUID cache is init-once).

#include <cstddef>

#include "atx/vol/types.hpp" // Side

namespace atx::vol::simd {

// Black-76 premium for each contract. price_out[i] = black76_price(F[i], K[i],
// T[i], sigma[i], df[i], side[i]). Degenerate (T ≤ 0 or sigma ≤ 0) collapses to
// discounted intrinsic, exactly as the scalar kernel.
void black76_price_batch(const double* F, const double* K, const double* T,
                         const double* sigma, const double* df,
                         const Side* side, double* price_out,
                         std::size_t n) noexcept;

// Fused premium + vega. vega_out[i] = F·df·φ(d1)·√T (identical for call/put).
// Sharing the d1/√T work makes this cheaper than separate price + vega passes;
// it is the kernel the calibrators lean on (residual + vega-Jacobian column).
void black76_value_vega_batch(const double* F, const double* K, const double* T,
                              const double* sigma, const double* df,
                              const Side* side, double* price_out,
                              double* vega_out, std::size_t n) noexcept;

} // namespace atx::vol::simd
