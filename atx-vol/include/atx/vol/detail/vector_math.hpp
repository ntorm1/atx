#pragma once

// AVX2 (4-lane f64) vectorized transcendentals for the pricing/IV/greeks
// kernels: log, exp, standard-normal Φ (Chebyshev-Clenshaw) and φ.
//
// Ported from the C `ats-vol` library (ats_vol_math_simd.h) with the Φ kernel
// re-expressed over atx-vol's runtime-built Chebyshev table (detail/
// norm_cdf_cheb.hpp). Every function is a pure, allocation-free leaf.
//
// COMPILE-TIME CONTRACT: this header emits AVX2+FMA intrinsics, so it must only
// be included by translation units built with `-mavx2 -mfma` (the `*_avx2.cpp`
// kernels). The dispatch layer guarantees these kernels are *called* only when
// runtime CPUID confirms AVX2+FMA (atx::vol::simd::have_avx2()); compiling the
// header into a baseline TU would define these functions but they would SIGILL
// on a non-AVX2 host, so the guard below fails the build instead.

#if !defined(__AVX2__) || !defined(__FMA__)
#  error "vector_math.hpp requires -mavx2 -mfma (include only from *_avx2.cpp)"
#endif

#include "atx/core/macro.hpp" // ATX_FORCE_INLINE
#include "atx/vol/detail/norm_cdf_cheb.hpp"

#include <immintrin.h>

namespace atx::vol::detail {

// log(2) split high+low (Cody-Waite) and reciprocal, plus exp clamp bounds.
inline constexpr double kLn2Hi = 0.6931471805599453;
inline constexpr double kLn2Lo = 2.3190468138462996e-17;
inline constexpr double kInvLn2 = 1.4426950408889634;
inline constexpr double kSqrt2 = 1.4142135623730951;
inline constexpr double kExpHi = 709.782712893384;
inline constexpr double kExpLo = -745.13321910194;
inline constexpr double kInvSqrt2Pi = 0.398942280401432677939946059934381868;

// Natural log, 4-lane. Full double accuracy on positive normals; the caller
// guarantees x > 0 (pricing feeds F/K > 0). Cody-Waite range reduction to
// m ∈ [√½, √2), then an odd-power series in s = (m-1)/(m+1).
ATX_FORCE_INLINE __m256d log_pd(__m256d x) noexcept {
    const __m256i ix = _mm256_castpd_si256(x);
    __m256i exp_bits = _mm256_srli_epi64(ix, 52);
    exp_bits = _mm256_and_si256(exp_bits, _mm256_set1_epi64x(0x7FF));
    __m256i e_int = _mm256_sub_epi64(exp_bits, _mm256_set1_epi64x(1023));

    __m256i mant_bits = _mm256_or_si256(
        _mm256_and_si256(ix, _mm256_set1_epi64x(0x000FFFFFFFFFFFFFLL)),
        _mm256_set1_epi64x(0x3FF0000000000000LL));
    __m256d m = _mm256_castsi256_pd(mant_bits);

    // Symmetric mantissa shift: m ≥ √2 → m /= 2; e += 1.
    const __m256d sqrt2 = _mm256_set1_pd(kSqrt2);
    const __m256d above = _mm256_cmp_pd(m, sqrt2, _CMP_GE_OQ);
    m = _mm256_blendv_pd(m, _mm256_mul_pd(m, _mm256_set1_pd(0.5)), above);
    const __m256i above_int = _mm256_castpd_si256(above); // -1 on selected lanes
    e_int = _mm256_sub_epi64(e_int, above_int);           // subtract -1 == add 1

    // int64 → double via the 2^52 magic-constant trick.
    const __m256i magic_int =
        _mm256_add_epi64(e_int, _mm256_set1_epi64x(0x4338000000000000LL));
    const __m256d e =
        _mm256_sub_pd(_mm256_castsi256_pd(magic_int), _mm256_set1_pd(0x1.8p52));

    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d s =
        _mm256_div_pd(_mm256_sub_pd(m, one), _mm256_add_pd(m, one));
    const __m256d s2 = _mm256_mul_pd(s, s);

    // P(s²) = 1 + s²/3 + s⁴/5 + … + s¹⁴/15. Horner from the top.
    __m256d p = _mm256_set1_pd(1.0 / 15.0);
    p = _mm256_fmadd_pd(p, s2, _mm256_set1_pd(1.0 / 13.0));
    p = _mm256_fmadd_pd(p, s2, _mm256_set1_pd(1.0 / 11.0));
    p = _mm256_fmadd_pd(p, s2, _mm256_set1_pd(1.0 / 9.0));
    p = _mm256_fmadd_pd(p, s2, _mm256_set1_pd(1.0 / 7.0));
    p = _mm256_fmadd_pd(p, s2, _mm256_set1_pd(1.0 / 5.0));
    p = _mm256_fmadd_pd(p, s2, _mm256_set1_pd(1.0 / 3.0));
    p = _mm256_fmadd_pd(p, s2, one);
    const __m256d log_m = _mm256_mul_pd(_mm256_add_pd(s, s), p);

    __m256d r = _mm256_fmadd_pd(e, _mm256_set1_pd(kLn2Hi), log_m);
    r = _mm256_fmadd_pd(e, _mm256_set1_pd(kLn2Lo), r);
    return r;
}

// exp, 4-lane. Clamped to the representable range; Cody-Waite reduction to
// r ∈ [-ln2/2, ln2/2] then a degree-11 Taylor series and 2^N scaling.
ATX_FORCE_INLINE __m256d exp_pd(__m256d x) noexcept {
    __m256d xc = _mm256_min_pd(_mm256_max_pd(x, _mm256_set1_pd(kExpLo)),
                               _mm256_set1_pd(kExpHi));

    const __m256d Nf = _mm256_round_pd(
        _mm256_mul_pd(xc, _mm256_set1_pd(kInvLn2)),
        _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);

    __m256d r = _mm256_fnmadd_pd(Nf, _mm256_set1_pd(kLn2Hi), xc);
    r = _mm256_fnmadd_pd(Nf, _mm256_set1_pd(kLn2Lo), r);

    __m256d p = _mm256_set1_pd(1.0 / 39916800.0);
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 3628800.0));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 362880.0));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 40320.0));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 5040.0));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 720.0));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 120.0));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 24.0));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0 / 6.0));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(0.5));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0));
    p = _mm256_fmadd_pd(p, r, _mm256_set1_pd(1.0));

    const __m128i N32 = _mm256_cvtpd_epi32(Nf);
    const __m256i N64 = _mm256_cvtepi32_epi64(N32);
    const __m256i biased = _mm256_add_epi64(N64, _mm256_set1_epi64x(1023));
    const __m256d two_to_N = _mm256_castsi256_pd(_mm256_slli_epi64(biased, 52));
    return _mm256_mul_pd(p, two_to_N);
}

// Standard-normal density φ(x) = (1/√2π)·exp(-½x²), 4-lane.
ATX_FORCE_INLINE __m256d norm_pdf_pd(__m256d x) noexcept {
    const __m256d arg = _mm256_mul_pd(_mm256_set1_pd(-0.5), _mm256_mul_pd(x, x));
    return _mm256_mul_pd(_mm256_set1_pd(kInvSqrt2Pi), exp_pd(arg));
}

// Standard-normal CDF Φ(x), 4-lane, via Clenshaw over the shared Chebyshev
// table. `coefs` must be norm_cdf_cheb_coefs().data() (length kNormCdfChebN).
// Accurate to ~1e-11 absolute on |x| ≤ HalfRange; callers patch wing lanes
// (|d| > kNormCdfWing) through the exact scalar path for full price parity.
ATX_FORCE_INLINE __m256d norm_cdf_pd(__m256d x, const double* coefs) noexcept {
    const __m256d hr = _mm256_set1_pd(kNormCdfHalfRange);
    const __m256d xc = _mm256_min_pd(_mm256_max_pd(x, _mm256_sub_pd(_mm256_setzero_pd(), hr)), hr);
    const __m256d t = _mm256_mul_pd(xc, _mm256_set1_pd(1.0 / kNormCdfHalfRange));
    const __m256d two_t = _mm256_add_pd(t, t);

    __m256d bk1 = _mm256_setzero_pd();
    __m256d bk2 = _mm256_setzero_pd();
    for (std::size_t k = kNormCdfChebN - 1; k >= 1; --k) {
        const __m256d ck = _mm256_set1_pd(coefs[k]);
        const __m256d bk = _mm256_add_pd(_mm256_fmsub_pd(two_t, bk1, bk2), ck);
        bk2 = bk1;
        bk1 = bk;
    }
    const __m256d c0 = _mm256_set1_pd(coefs[0]);
    return _mm256_add_pd(c0, _mm256_fmsub_pd(t, bk1, bk2));
}

// Φ for TWO independent vectors in a single fused Clenshaw loop. The Chebyshev
// recurrence is a length-N serial FMA dependency chain (latency-bound), so a
// lone norm_cdf_pd leaves the vector FMA units mostly idle. Black-76 always
// needs Φ(d1) AND Φ(d2), which are independent — interleaving their two
// recurrences in one loop issues two FMAs per step, hiding the latency and
// nearly doubling Φ throughput. Same coefficients, so results are bit-identical
// to two norm_cdf_pd calls.
ATX_FORCE_INLINE void norm_cdf_pd2(__m256d x0, __m256d x1, const double* coefs,
                                   __m256d& r0, __m256d& r1) noexcept {
    const __m256d hr = _mm256_set1_pd(kNormCdfHalfRange);
    const __m256d nhr = _mm256_sub_pd(_mm256_setzero_pd(), hr);
    const __m256d inv = _mm256_set1_pd(1.0 / kNormCdfHalfRange);
    const __m256d t0 = _mm256_mul_pd(_mm256_min_pd(_mm256_max_pd(x0, nhr), hr), inv);
    const __m256d t1 = _mm256_mul_pd(_mm256_min_pd(_mm256_max_pd(x1, nhr), hr), inv);
    const __m256d tt0 = _mm256_add_pd(t0, t0);
    const __m256d tt1 = _mm256_add_pd(t1, t1);

    __m256d a1 = _mm256_setzero_pd(), a2 = _mm256_setzero_pd();
    __m256d b1 = _mm256_setzero_pd(), b2 = _mm256_setzero_pd();
    for (std::size_t k = kNormCdfChebN - 1; k >= 1; --k) {
        const __m256d ck = _mm256_set1_pd(coefs[k]);
        const __m256d na = _mm256_add_pd(_mm256_fmsub_pd(tt0, a1, a2), ck);
        const __m256d nb = _mm256_add_pd(_mm256_fmsub_pd(tt1, b1, b2), ck);
        a2 = a1; a1 = na;
        b2 = b1; b1 = nb;
    }
    const __m256d c0 = _mm256_set1_pd(coefs[0]);
    r0 = _mm256_add_pd(c0, _mm256_fmsub_pd(t0, a1, a2));
    r1 = _mm256_add_pd(c0, _mm256_fmsub_pd(t1, b1, b2));
}

} // namespace atx::vol::detail
