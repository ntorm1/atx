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
// ln(DBL_MIN): below this, exp(x) is denormal or zero. The 2^N reconstruction in
// exp_pd builds only NORMALIZED doubles (biased exponent ≥ 1), so it emits
// garbage there; lanes at or below this threshold are flushed to 0 instead.
inline constexpr double kExpMinNormal = -708.3964185322641;
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
//
// Underflow (correctness): the 2^N step below reconstructs a NORMALIZED double
// from a biased exponent, so it cannot represent a denormal or zero result and
// would emit garbage for x < ln(DBL_MIN). exp(x) there is ≤ one denormal ULP, so
// such lanes are flushed to exactly 0.0 — matching std::exp to within a denormal.
// This is what lets the Cody-erfc Φ and the φ pdf evaluate the DEEP wings
// (|d| ≳ 38, where exp(−½d²) underflows) directly on the vector path, after the
// K2 wing-patch removal retired the scalar detour that used to hide this.
ATX_FORCE_INLINE __m256d exp_pd(__m256d x) noexcept {
    const __m256d underflow = _mm256_cmp_pd(x, _mm256_set1_pd(kExpMinNormal), _CMP_LT_OQ);
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
    const __m256d result = _mm256_mul_pd(p, two_to_N);
    // Flush the underflow lanes (x < ln(DBL_MIN)) to 0: andnot(mask, v) clears the
    // lanes where mask is all-ones, keeps the rest bit-for-bit.
    return _mm256_andnot_pd(underflow, result);
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

// ── Full-range standard-normal CDF via Cody rational erfc (K2, W5.3) ───────
//
// Φ(x) = ½·erfc(−x/√2), with erfc evaluated by W. J. Cody's near-minimax
// rational approximation. Primary source: W. J. Cody, "Rational Chebyshev
// Approximations for the Error Function", Math. Comp. 23 (1969), 631–637; the
// coefficients below are transcribed from Cody's own reference implementation
// (SPECFUN / ACM TOMS Algorithm 715 CALERF, netlib.org/specfun) — NOT from
// memory. erfc(y) for y ≥ 0 uses three regions, all in double precision:
//   y ≤ 0.46875 : erf via a degree-4/4 rational in y², erfc = 1 − erf.
//   0.46875 < y ≤ 4 : erfc = e·N(y)/D(y), e = exp(−y²), degree-8/8.
//   y > 4 : erfc = e·(1/√π − w·N(w)/D(w))/y, w = 1/y², degree-5/5 (asymptotic).
//
// vs. the degree-48 Chebyshev–Clenshaw norm_cdf_pd above: this is full
// double-precision across the ENTIRE real line — including the deep wings the
// Chebyshev fit (accurate only on |x| ≤ ~7, ~1e-11) could never reach — so the
// pricing kernels no longer need a |d| > kNormCdfWing scalar wing patch. It
// costs an exp(−y²) and two divisions the polynomial path avoids; the accuracy
// (≈1e-16 vs 1e-11, and correct denormal wings) is the point. Class:
// accuracy-improving. All lanes evaluate every region branchlessly and select
// by y with blendv (a pure bitwise select), so the non-finite region-3 math on
// small-y lanes is computed but never selected.
//
// Cody CALERF coefficients (double precision):
inline constexpr double kCodyThresh = 0.46875;
inline constexpr double kCodySqrtPiInv = 5.6418958354775628695e-1; // 1/√π
inline constexpr double kCodyA[5] = {3.16112374387056560e00, 1.13864154151050156e02,
                                     3.77485237685302021e02, 3.20937758913846947e03,
                                     1.85777706184603153e-1};
inline constexpr double kCodyB[4] = {2.36012909523441209e01, 2.44024637934444173e02,
                                     1.28261652607737228e03, 2.84423683343917062e03};
inline constexpr double kCodyC[9] = {5.64188496988670089e-1, 8.88314979438837594e0,
                                     6.61191906371416295e01, 2.98635138197400131e02,
                                     8.81952221241769090e02, 1.71204761263407058e03,
                                     2.05107837782607147e03, 1.23033935479799725e03,
                                     2.15311535474403846e-8};
inline constexpr double kCodyD[8] = {1.57449261107098347e01, 1.17693950891312499e02,
                                     5.37181101862009858e02, 1.62138957456669019e03,
                                     3.29079923573345963e03, 4.36261909014324716e03,
                                     3.43936767414372164e03, 1.23033935480374942e03};
inline constexpr double kCodyP[6] = {3.05326634961232344e-1, 3.60344899949804439e-1,
                                     1.25781726111229246e-1, 1.60837851487422766e-2,
                                     6.58749161529837803e-4, 1.63153871373020978e-2};
inline constexpr double kCodyQ[5] = {2.56852019228982242e00, 1.87295284992346047e00,
                                     5.27905102951428412e-1, 6.05183413124413191e-2,
                                     2.33520497626869185e-3};

// erfc(y) for y ≥ 0, four lanes. Callers pass y = |x|·(1/√2) ≥ 0.
ATX_FORCE_INLINE __m256d erfc_nonneg_pd(__m256d y) noexcept {
    const __m256d ysq = _mm256_mul_pd(y, y);
    // e = exp(−y²), shared by regions 2 and 3.
    const __m256d e = exp_pd(_mm256_sub_pd(_mm256_setzero_pd(), ysq));

    // Region 1 (y ≤ 0.46875): erf via degree-4/4 rational in ysq; erfc = 1 − erf.
    __m256d num1 = _mm256_set1_pd(kCodyA[4]);
    num1 = _mm256_fmadd_pd(num1, ysq, _mm256_set1_pd(kCodyA[0]));
    num1 = _mm256_fmadd_pd(num1, ysq, _mm256_set1_pd(kCodyA[1]));
    num1 = _mm256_fmadd_pd(num1, ysq, _mm256_set1_pd(kCodyA[2]));
    num1 = _mm256_fmadd_pd(num1, ysq, _mm256_set1_pd(kCodyA[3]));
    __m256d den1 = _mm256_set1_pd(1.0);
    den1 = _mm256_fmadd_pd(den1, ysq, _mm256_set1_pd(kCodyB[0]));
    den1 = _mm256_fmadd_pd(den1, ysq, _mm256_set1_pd(kCodyB[1]));
    den1 = _mm256_fmadd_pd(den1, ysq, _mm256_set1_pd(kCodyB[2]));
    den1 = _mm256_fmadd_pd(den1, ysq, _mm256_set1_pd(kCodyB[3]));
    const __m256d erf = _mm256_div_pd(_mm256_mul_pd(y, num1), den1);
    const __m256d r_lo = _mm256_sub_pd(_mm256_set1_pd(1.0), erf);

    // Region 2 (0.46875 < y ≤ 4): erfc = e·N(y)/D(y), degree-8/8.
    __m256d num2 = _mm256_set1_pd(kCodyC[8]);
    num2 = _mm256_fmadd_pd(num2, y, _mm256_set1_pd(kCodyC[0]));
    num2 = _mm256_fmadd_pd(num2, y, _mm256_set1_pd(kCodyC[1]));
    num2 = _mm256_fmadd_pd(num2, y, _mm256_set1_pd(kCodyC[2]));
    num2 = _mm256_fmadd_pd(num2, y, _mm256_set1_pd(kCodyC[3]));
    num2 = _mm256_fmadd_pd(num2, y, _mm256_set1_pd(kCodyC[4]));
    num2 = _mm256_fmadd_pd(num2, y, _mm256_set1_pd(kCodyC[5]));
    num2 = _mm256_fmadd_pd(num2, y, _mm256_set1_pd(kCodyC[6]));
    num2 = _mm256_fmadd_pd(num2, y, _mm256_set1_pd(kCodyC[7]));
    __m256d den2 = _mm256_set1_pd(1.0);
    den2 = _mm256_fmadd_pd(den2, y, _mm256_set1_pd(kCodyD[0]));
    den2 = _mm256_fmadd_pd(den2, y, _mm256_set1_pd(kCodyD[1]));
    den2 = _mm256_fmadd_pd(den2, y, _mm256_set1_pd(kCodyD[2]));
    den2 = _mm256_fmadd_pd(den2, y, _mm256_set1_pd(kCodyD[3]));
    den2 = _mm256_fmadd_pd(den2, y, _mm256_set1_pd(kCodyD[4]));
    den2 = _mm256_fmadd_pd(den2, y, _mm256_set1_pd(kCodyD[5]));
    den2 = _mm256_fmadd_pd(den2, y, _mm256_set1_pd(kCodyD[6]));
    den2 = _mm256_fmadd_pd(den2, y, _mm256_set1_pd(kCodyD[7]));
    const __m256d r_mid = _mm256_mul_pd(e, _mm256_div_pd(num2, den2));

    // Region 3 (y > 4): asymptotic erfc = e·(1/√π − w·N(w)/D(w))/y, w = 1/y².
    // For small-y lanes w is huge and these results are non-finite, but they are
    // never selected below.
    const __m256d w = _mm256_div_pd(_mm256_set1_pd(1.0), ysq);
    __m256d num3 = _mm256_set1_pd(kCodyP[5]);
    num3 = _mm256_fmadd_pd(num3, w, _mm256_set1_pd(kCodyP[0]));
    num3 = _mm256_fmadd_pd(num3, w, _mm256_set1_pd(kCodyP[1]));
    num3 = _mm256_fmadd_pd(num3, w, _mm256_set1_pd(kCodyP[2]));
    num3 = _mm256_fmadd_pd(num3, w, _mm256_set1_pd(kCodyP[3]));
    num3 = _mm256_fmadd_pd(num3, w, _mm256_set1_pd(kCodyP[4]));
    __m256d den3 = _mm256_set1_pd(1.0);
    den3 = _mm256_fmadd_pd(den3, w, _mm256_set1_pd(kCodyQ[0]));
    den3 = _mm256_fmadd_pd(den3, w, _mm256_set1_pd(kCodyQ[1]));
    den3 = _mm256_fmadd_pd(den3, w, _mm256_set1_pd(kCodyQ[2]));
    den3 = _mm256_fmadd_pd(den3, w, _mm256_set1_pd(kCodyQ[3]));
    den3 = _mm256_fmadd_pd(den3, w, _mm256_set1_pd(kCodyQ[4]));
    __m256d r_hi = _mm256_mul_pd(w, _mm256_div_pd(num3, den3));
    r_hi = _mm256_div_pd(_mm256_sub_pd(_mm256_set1_pd(kCodySqrtPiInv), r_hi), y);
    r_hi = _mm256_mul_pd(e, r_hi);

    // Select region by y.
    const __m256d in_lo = _mm256_cmp_pd(y, _mm256_set1_pd(kCodyThresh), _CMP_LE_OQ);
    const __m256d in_hi = _mm256_cmp_pd(y, _mm256_set1_pd(4.0), _CMP_GT_OQ);
    __m256d erfc = _mm256_blendv_pd(r_mid, r_hi, in_hi); // mid vs asymptotic
    erfc = _mm256_blendv_pd(erfc, r_lo, in_lo);          // then the erf region
    return erfc;
}

// Standard-normal CDF Φ(x) = ½·erfc(−x/√2), full range (Cody). Four lanes.
ATX_FORCE_INLINE __m256d norm_cdf_erfc_pd(__m256d x) noexcept {
    const __m256d abs_x = _mm256_andnot_pd(_mm256_set1_pd(-0.0), x);
    const __m256d y = _mm256_mul_pd(abs_x, _mm256_set1_pd(1.0 / kSqrt2));
    const __m256d ea = erfc_nonneg_pd(y);
    const __m256d half_ea = _mm256_mul_pd(_mm256_set1_pd(0.5), ea);
    // x ≥ 0 → 1 − ½·erfc(|x|/√2); x < 0 → ½·erfc(|x|/√2).
    const __m256d hi = _mm256_sub_pd(_mm256_set1_pd(1.0), half_ea);
    const __m256d nonneg = _mm256_cmp_pd(x, _mm256_setzero_pd(), _CMP_GE_OQ);
    return _mm256_blendv_pd(half_ea, hi, nonneg);
}

// Φ for TWO independent vectors (Cody erfc). Same result as two norm_cdf_erfc_pd
// calls; kept as a paired entry so the pricing kernels read like the fused
// Chebyshev norm_cdf_pd2 they replace, and so the two independent erfc chains
// overlap in the out-of-order window (hiding the div/exp latency).
ATX_FORCE_INLINE void norm_cdf_erfc_pd2(__m256d x0, __m256d x1, __m256d &r0,
                                        __m256d &r1) noexcept {
    r0 = norm_cdf_erfc_pd(x0);
    r1 = norm_cdf_erfc_pd(x1);
}

} // namespace atx::vol::detail
