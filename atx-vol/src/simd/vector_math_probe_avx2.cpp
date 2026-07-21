// AVX2 array wrappers around the FastDeterministic transcendentals in
// detail/vector_math.hpp — the probe surface for the P3.3 bound test + bakeoff
// bench. Built with -mavx2 -mfma via the src/simd/*_avx2.cpp glob; called only
// behind atx::vol::simd::have_avx2() (see vector_math_probe.hpp).
//
// Each wrapper streams the input in 4-wide packs and pads a short tail into a
// stack buffer so the final (<4) lanes still take the vector path — i.e. the timed
// / graded values are exactly what the batch kernels would compute, tail included.

#include "atx/vol/simd/vector_math_probe.hpp"

#include "atx/vol/detail/vector_math.hpp" // log_pd/exp_pd/norm_cdf_erfc_pd (+ __AVX2__ guard)

#include <cstddef>

#if !defined(__AVX2__) || !defined(__FMA__)
#  error "vector_math_probe_avx2.cpp requires -mavx2 -mfma (build via src/simd/*_avx2.cpp)"
#endif

#include <immintrin.h>

namespace atx::vol::simd {

namespace {

using atx::vol::detail::exp_pd;
using atx::vol::detail::log_pd;
using atx::vol::detail::norm_cdf_erfc_pd;

// Apply a 1-arg __m256d kernel over x[0..n) into out, padding a short tail.
template <class Fn>
ATX_FORCE_INLINE void map4(const double* x, double* out, std::size_t n,
                           Fn&& fn) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        _mm256_storeu_pd(out + i, fn(_mm256_loadu_pd(x + i)));
    }
    const std::size_t rem = n - i;
    if (rem != 0) {
        alignas(32) double buf[4] = {1.0, 1.0, 1.0, 1.0}; // 1.0: safe log/exp arg
        for (std::size_t l = 0; l < rem; ++l) {
            buf[l] = x[i + l];
        }
        alignas(32) double res[4];
        _mm256_store_pd(res, fn(_mm256_load_pd(buf)));
        for (std::size_t l = 0; l < rem; ++l) {
            out[i + l] = res[l];
        }
    }
}

} // namespace

void fd_log_batch(const double* x, double* out, std::size_t n) noexcept {
    map4(x, out, n, [](__m256d v) { return log_pd(v); });
}

void fd_exp_batch(const double* x, double* out, std::size_t n) noexcept {
    map4(x, out, n, [](__m256d v) { return exp_pd(v); });
}

void fd_norm_cdf_batch(const double* x, double* out, std::size_t n) noexcept {
    map4(x, out, n, [](__m256d v) { return norm_cdf_erfc_pd(v); });
}

} // namespace atx::vol::simd
