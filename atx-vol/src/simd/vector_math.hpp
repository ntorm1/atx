#pragma once

// AVX2 (4-lane f64) vectorized transcendentals for the pricing/IV/greeks
// kernels: log, exp, standard-normal Φ (Cody rational-erfc) and φ.
//
// Ported from the C `ats-vol` library (ats_vol_math_simd.h); the Φ kernel is a
// full-range Cody rational-erfc (norm_cdf_erfc_pd, below) — the single Φ source
// the batch kernels share. Every function is a pure, allocation-free leaf.
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
//
// DOMAIN (A9, simd-review finding 4): valid ONLY for a positive NORMAL argument.
// The exponent/mantissa decode above assumes a normalized IEEE double, so:
//   • a DENORMAL x (e.g. an F/K ratio that underflowed) decodes its subnormal
//     exponent field as if normalized → a FINITE result near -709 (log of the
//     smallest normal), not the true (more negative) value;
//   • x == 0 likewise returns ~-709 rather than -inf;
//   • x == +inf returns ~+710 rather than +inf;
//   • x < 0 / NaN are undefined.
// Crucially the garbage is FINITE, so a downstream nonfinite_mask(d) cannot see it.
// Consumers that can feed a denormal/0/inf ratio (black76/greeks batches) therefore
// add an |log(F/K)| >= 708 escape to their patch mask — that band brackets exactly
// this garbage (and any genuine deep wing, priced exactly by the scalar fallback).
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
    // SAFETY (NaN domain): the CONSTANT is the first operand of both clamps on purpose.
    // x86 MAXPD/MINPD return their SECOND operand whenever EITHER operand is NaN, so
    // with `x` first a NaN lane was silently replaced by the clamp bound: xc = kExpLo
    // gave N = -1075, biased = -52, and _mm256_slli_epi64(-52, 52) rebuilt the double
    // -2^973 = -7.98e292. `underflow` could not see it either (_CMP_LT_OQ is false for
    // NaN), so a NaN argument left this function as a FINITE NEGATIVE number — invisible
    // to every nonfinite_mask / std::isfinite guard downstream, and via norm_pdf_pd a
    // negative probability density. With the constant first, MAXPD/MINPD return `x`
    // itself and the NaN propagates through the polynomial to a NaN result.
    //
    // Every FINITE lane is bit-identical under the swap: when the operands compare
    // equal both orders return the same bits, and otherwise the ordering rule picks the
    // same operand. +/-inf also keep IEEE exp semantics (+inf -> +inf, -inf -> 0).
    // Gated by SimdNanSafety.ExpPd_* in tests/simd_nan_safety_test.cpp.
    const __m256d xc = _mm256_max_pd(_mm256_set1_pd(kExpLo), x);

    // The upper clamp is applied to N, not to x. It exists only to keep the
    // _mm256_cvtpd_epi32 below in range; clamping x there instead DISCARDED the
    // overflow — every argument above kExpHi collapsed onto kExpHi and shared its
    // result. Clamping N keeps the Cody-Waite remainder r = x - N·ln2 growing with x,
    // so an argument past ln(DBL_MAX) drives the polynomial through +inf on its own
    // and exp_pd(+inf) stays +inf. Same NaN operand order as the max above: MINPD
    // returns SRC2 when either operand is NaN, so a NaN Nf survives as NaN.
    const __m256d Nf = _mm256_min_pd(
        _mm256_set1_pd(1024.0),
        _mm256_round_pd(_mm256_mul_pd(xc, _mm256_set1_pd(kInvLn2)),
                        _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));

    // r = xc - N·ln2. With N clamped (above) rather than x, r GROWS past the
    // reduction interval for an overflowing argument — which is exactly what drives
    // the polynomial to +inf there instead of saturating at kExpHi's value.
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
    // 2^N, with the ONE exponent the biased field cannot hold split off as a carry.
    // N reaches 1024 for x in [1023.5·ln2, kExpHi] ≈ [709.4362, 709.7827], and
    // biased = 2047 is the inf/NaN exponent pattern — so exp_pd(709.44) returned +inf
    // where std::exp returns 1.2760780590224528e308, and exp_pd(kExpHi) returned +inf
    // for 1.7976931348622732e308. (The scalar sibling exp_cody carried the identical
    // defect; fixing only that one would have left the two divergent.)
    //
    // A CONSTANT split (always 2^(N-1)·2) cannot work: N spans [-1022, 1024] after the
    // underflow flush, 2047 values, while the normal biased field holds 2046 — so any
    // fixed offset that saves the top breaks the bottom binade (N = -1022 would encode
    // as +0.0 and the deep Φ wings this kernel exists to reach would flush early).
    // Hence the conditional. It is branchless and it costs no extra MULTIPLY: the
    // missing factor of two is folded into `p` as a masked SELF-ADD (p + p is exact,
    // and p + 0.0 is bit-identical to p for the 0.707..1.414 range p occupies), which
    // measured cheaper than a blendv'd `carry` multiply — the latter lengthens the
    // final dependency chain. Non-`over` lanes are therefore bit-identical.
    // MEASURED (clang-cl 18 /O2 /arch:AVX2, standalone A/B, best-of-240 x 1Mi values,
    // Alder Lake): exp_pd alone +3.7..13.3%, and inside norm_cdf_erfc_pd — the way it
    // is actually consumed — +0.1..4.7%. The blendv+mul variant cost +14..25% and
    // +6..10.5% for the same fix, so this shape is what ships.
    const __m256i over = _mm256_cmpgt_epi64(biased, _mm256_set1_epi64x(2046));
    const __m256d two_to_N = _mm256_castsi256_pd(
        _mm256_slli_epi64(_mm256_add_epi64(biased, over), 52)); // -1 where over
    const __m256d p_carry =
        _mm256_add_pd(p, _mm256_and_pd(p, _mm256_castsi256_pd(over))); // 2p where over
    const __m256d result = _mm256_mul_pd(p_carry, two_to_N);
    // Flush the underflow lanes (x < ln(DBL_MIN)) to 0: andnot(mask, v) clears the
    // lanes where mask is all-ones, keeps the rest bit-for-bit.
    return _mm256_andnot_pd(underflow, result);
}

// Standard-normal density φ(x) = (1/√2π)·exp(-½x²), 4-lane.
ATX_FORCE_INLINE __m256d norm_pdf_pd(__m256d x) noexcept {
    const __m256d arg = _mm256_mul_pd(_mm256_set1_pd(-0.5), _mm256_mul_pd(x, x));
    return _mm256_mul_pd(_mm256_set1_pd(kInvSqrt2Pi), exp_pd(arg));
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
// This is the ONLY Φ the batch kernels evaluate: full double-precision across
// the ENTIRE real line — including the deep wings a degree-48 Chebyshev–Clenshaw
// fit (accurate only on |x| ≤ ~7, ~1e-11) could never reach — so the pricing
// kernels need no wing patch and route only genuinely degenerate lanes to
// scalar. It costs an exp(−y²) and two divisions a polynomial path avoids; the
// accuracy (≈1e-16 across the range, and correct denormal wings) is the point.
// Class:
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

    // Region selector, hoisted: regions 1 and 2 share ONE division (see below).
    const __m256d in_lo = _mm256_cmp_pd(y, _mm256_set1_pd(kCodyThresh), _CMP_LE_OQ);

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

    // ONE division for regions 1 and 2 (perf, bit-identical). Both regions divide a
    // numerator by a denominator with the SAME operator on the SAME lane, so blending
    // the (N, D) PAIR before the divide and blending the two post-processings after it
    // reproduces each selected lane's quotient bit-for-bit — a division is a pure
    // per-lane function of its two operands, and blendv is a pure bitwise select. Only
    // the UNSELECTED region's discarded value changes. That trades one _mm256_div_pd
    // (13-14 cycle latency, ~4-8 cycle reciprocal throughput) for two blends (1 cycle
    // each) in a kernel evaluated per quadrature node per sweep, twice per Φ pair.
    //
    // Region 3 is deliberately NOT folded in: it needs an algebraic rearrangement that
    // is not bit-identical, and this tree's American-boundary route is
    // reproducible-per-host by an explicit user ruling — see
    // american_boundary_batch.cpp:73-83. Not this lane's call to make.
    const __m256d num12 = _mm256_blendv_pd(num2, _mm256_mul_pd(y, num1), in_lo);
    const __m256d den12 = _mm256_blendv_pd(den2, den1, in_lo);
    const __m256d q12 = _mm256_div_pd(num12, den12);
    const __m256d r_lo = _mm256_sub_pd(_mm256_set1_pd(1.0), q12); // 1 − erf(y)
    const __m256d r_mid = _mm256_mul_pd(e, q12);                  // e·N(y)/D(y)

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

    // Select region by y (in_lo hoisted above the shared division).
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
// calls; kept as a paired entry so the two independent erfc chains overlap in the
// out-of-order window (hiding the div/exp latency), and so Black-76 (which always
// needs Φ(d1) AND Φ(d2)) reads as one call.
ATX_FORCE_INLINE void norm_cdf_erfc_pd2(__m256d x0, __m256d x1, __m256d &r0,
                                        __m256d &r1) noexcept {
    r0 = norm_cdf_erfc_pd(x0);
    r1 = norm_cdf_erfc_pd(x1);
}

} // namespace atx::vol::detail
