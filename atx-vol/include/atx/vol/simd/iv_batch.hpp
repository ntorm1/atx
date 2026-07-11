#pragma once

// Batched (SoA) implied-volatility inversion — the vectorized IV hot path.
//
// This entry point inverts a whole array of Black-76 premiums for their implied
// volatilities in one call, dispatching to a 4-lane AVX2 kernel when the host
// supports it (atx::vol::simd::have_avx2()) and to a scalar loop otherwise. The
// scalar loop calls the exact per-contract atx::vol::implied_vol (the numerical
// source of truth); the AVX2 path reproduces it — a lane-parallel Stefanica-
// Radoicic (2017) seed plus two Halley steps — and patches any degenerate,
// deep-wing, ill-conditioned, or non-converged lane back through the exact
// scalar inverter, so the recovered σ is bit-for-bit with scalar wherever the
// vector path would lose accuracy.
//
// Layout: structure-of-arrays. price/F/K/T/df are contiguous doubles of length
// n, side is one Side per contract. Outputs iv_out (length n) and ok_out (length
// n) must not alias the inputs. ok_out[i] == 1 iff the lane produced a valid,
// converged implied vol (mirrors the scalar implied_vol returning a value); on
// any failure ok_out[i] == 0 and iv_out[i] is NaN. A price at (or below)
// intrinsic clamps to kIvMin and succeeds (ok == 1), exactly as the scalar path.
// Passing n == 0 is a no-op.
//
// noexcept and allocation-free on the fast path — safe to call concurrently from
// any threads (no shared mutable state; the CPUID cache is init-once). The
// per-lane scalar patch may allocate an error string on a failing lane, matching
// the scalar implied_vol.

#include <cstddef>
#include <cstdint>

#include "atx/vol/types.hpp" // Side

namespace atx::vol::simd {

// Invert Black-76 premiums for implied volatility, lane-parallel.
// iv_out[i] / ok_out[i] == implied_vol(price[i], F[i], K[i], T[i], df[i],
// side[i]) unpacked into (value, 1) on success or (NaN, 0) on failure.
void implied_vol_batch(const double* price, const double* F, const double* K,
                       const double* T, const double* df, const Side* side,
                       double* iv_out, std::uint8_t* ok_out,
                       std::size_t n) noexcept;

} // namespace atx::vol::simd
