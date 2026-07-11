#pragma once

// FastDeterministic (AVX2 detail/vector_math.hpp) transcendentals in plain array
// form, exposed for the P3.3 math-mode work: the accuracy gate
// (VectorMath_FastDeterministic_BoundedVsReference) grades these against scalar libm,
// and the bakeoff microbench (bench/simd_vector_math_bench.cpp) times them against
// the per-lane libm baseline. This is the SAME log_pd/exp_pd/norm_cdf_pd the batch
// kernels use — a thin AoS wrapper so a baseline TU can call them without pulling in
// AVX2 intrinsics (defined in vector_math_probe_avx2.cpp under -mavx2 -mfma).
//
// AVX2-ONLY: like every *_avx2 kernel these emit AVX2 instructions. Call ONLY after
// atx::vol::simd::have_avx2() (or use_avx2()) returns true, else they SIGILL on a
// non-AVX2 host. n need not be a multiple of 4 — the tail is padded internally so the
// last (<4) lanes still run the vector path (a fair FastDeterministic measurement).

#include <cstddef>

namespace atx::vol::simd {

// out[i] = log_pd(x[i])       (x[i] > 0, the pricing domain)
void fd_log_batch(const double* x, double* out, std::size_t n) noexcept;

// out[i] = exp_pd(x[i])
void fd_exp_batch(const double* x, double* out, std::size_t n) noexcept;

// out[i] = norm_cdf_pd(x[i])  (Chebyshev-Φ; interior |x| ≤ kNormCdfWing is trusted,
//                              the wing is clamped — the kernels patch it to scalar)
void fd_norm_cdf_batch(const double* x, double* out, std::size_t n) noexcept;

} // namespace atx::vol::simd
