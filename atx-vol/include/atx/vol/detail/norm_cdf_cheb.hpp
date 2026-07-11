#pragma once

// Chebyshev approximation table for the standard-normal CDF Φ, shared by the
// AVX2 pricing/greeks/IV kernels.
//
// The AVX2 kernels evaluate Φ lane-parallel via a Clenshaw recurrence over a
// degree-(N-1) Chebyshev expansion of Φ on [-HalfRange, +HalfRange]. That is
// far cheaper than a per-lane libm `erfc`, yet its uniform absolute error over
// the core region (~1e-11 at N=48, HalfRange=7) is well inside the accuracy the
// pricing kernels then patch on the wings. The coefficients are built ONCE from
// the exact `atx::core::norm_cdf` (erfc-based) so the vector path tracks the
// scalar source of truth — no hand-transcribed constants to drift.
//
// Convention: coefs[0] is the halved DC term, so the Clenshaw evaluation is
//   t   = clamp(x, -HR, HR) / HR              (map to [-1, 1])
//   b_k = coefs[k] + 2t·b_{k+1} - b_{k+2}     (k = N-1 … 1, b_N = b_{N+1} = 0)
//   Φ(x) ≈ coefs[0] + t·b_1 - b_2
// which matches the AVX2 Clenshaw loop in detail/vector_math.hpp.

#include <array>
#include <cstddef>

namespace atx::vol::detail {

inline constexpr std::size_t kNormCdfChebN = 48;
inline constexpr double kNormCdfHalfRange = 7.0;

// Beyond this |d| the pricing kernels patch a lane through the exact scalar
// path: near the ±HalfRange boundary the Chebyshev fit degrades, and the price
// formula's Φ(d1)-Φ(d2) subtraction amplifies any absolute error into large
// relative error. 6.0 keeps the vector path on its accurate interior.
inline constexpr double kNormCdfWing = 6.0;

// Clenshaw-ready Chebyshev coefficients of Φ over [-HalfRange, +HalfRange].
// Built once on first call (thread-safe function-local static). 64-byte aligned
// storage so the AVX2 loads that broadcast each coefficient stay cache-friendly.
[[nodiscard]] const std::array<double, kNormCdfChebN>& norm_cdf_cheb_coefs() noexcept;

} // namespace atx::vol::detail
