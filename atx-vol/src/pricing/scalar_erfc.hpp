#pragma once

// Scalar Cody rational-erfc Φ / φ (K1) — a validated leaf, NOT wired into any hot
// path today.
//
// STATUS (kept, not deleted; corrected drift, PR-C2): K1 was SHELVED as
// perf-neutral — no production TU includes this header (src/implied_vol.cpp and
// src/black76.cpp do NOT), and it is exercised only by scalar_erfc_test.cpp. It is
// retained (with its ≈1.1e-16-vs-std::erfc test) as a revival-ready drop-in should
// a future scalar hot path want to avoid libm's std::erfc / std::exp.
//
// This is the SCALAR sibling of detail/vector_math.hpp's validated AVX2 kernels
// (erfc_nonneg_pd / norm_cdf_erfc_pd / exp_pd). Were it wired in, a scalar hot path
// could evaluate Φ and φ WITHOUT libm's std::erfc / std::exp, which dominate the
// per-Halley-step cost of an inverter (2·Φ + 1·φ per step ⇒ ~2 std::erfc + 1
// std::exp, and std::erfc alone is tens of ns). Unlike the AVX2 kernel — which
// evaluates all three Cody regions
// branchlessly and blends — the scalar routine BRANCH-SELECTS the single
// applicable region, so a typical call runs one rational plus (region 2/3) one
// exp, not three rationals plus an exp.
//
// It is a header-only, scalar, allocation-free leaf: it emits NO intrinsics and
// carries NO AVX2 build requirement, so unlike vector_math.hpp it may be
// included from baseline TUs (the whole reason a new header exists rather than
// reusing vector_math.hpp). Constants/helpers live in a nested `serfc` namespace
// so they never ODR-collide with vector_math.hpp's identically-named `atx::vol::
// detail::kCody*` should some future TU pull in both headers.
//
// ── Numerical basis (primary source, NOT from memory) ─────────────────────
// Φ(x) = ½·erfc(−x/√2), with erfc by W. J. Cody's near-minimax rational
// approximation. Primary source: W. J. Cody, "Rational Chebyshev Approximations
// for the Error Function", Math. Comp. 23 (1969), 631–637; coefficients are the
// double-precision constants from Cody's own reference implementation (SPECFUN /
// ACM TOMS Algorithm 715 CALERF, netlib.org/specfun). These are the SAME
// coefficients and the SAME Horner arrangement as vector_math.hpp's AVX2
// erfc_nonneg_pd, which is validated to ≈1e-16 vs the long-double oracle; the
// scalar routine here is measured at ≈1.1e-16 vs std::erfc. erfc(y), y ≥ 0, in
// three regions, all double precision:
//   y ≤ 0.46875     : erf via a degree-4/4 rational in y², erfc = 1 − erf (no exp).
//   0.46875 < y ≤ 4 : erfc = e·N(y)/D(y), e = exp(−y²), degree-8/8.
//   y > 4           : erfc = e·(1/√π − w·N(w)/D(w))/y, w = 1/y², degree-5/5.
//
// exp(−·) is the Cody-Waite reduction + degree-11 Taylor + 2^N scaling ported
// scalar from exp_pd (again same constants). Every exp argument on this hot path
// is ≤ 0 (exp(−y²), exp(−½d²)), so no overflow path is ever taken; the underflow
// flush-to-0 matches std::exp to within one denormal in the deep wings.
//
// Class: accuracy-improving-or-neutral. std::erfc is correctly-rounded-ish
// (≤ ~0.5 ULP); the Cody rational holds ≈1 ULP. Swapping one ≈1-ULP Φ for
// another ≈0.5-ULP Φ moves the round-trip σ by at most a few ULP — orders of
// magnitude inside the 1e-4 vol economic bound, and below the price-residual
// rounding-noise floor (~ε·df·max(F,K)) at which the inverter already
// terminates. See scalar_erfc_test.cpp for the ULP + round-trip gates.

#include <cmath>    // std::fabs, std::nearbyint
#include <cstdint>  // std::uint64_t / int64_t
#include <cstring>  // std::memcpy
#include <limits>  // std::numeric_limits (exp overflow)

#if defined(_MSC_VER)
#  define ATX_SERFC_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#  define ATX_SERFC_INLINE inline __attribute__((always_inline))
#else
#  define ATX_SERFC_INLINE inline
#endif

namespace atx::vol::detail {

namespace serfc {

// ── exp (Cody-Waite) constants, transcribed from vector_math.hpp exp_pd ──────
inline constexpr double kLn2Hi = 0.6931471805599453;
inline constexpr double kLn2Lo = 2.3190468138462996e-17;
inline constexpr double kInvLn2 = 1.4426950408889634;
inline constexpr double kExpHi = 709.782712893384;
// ln(DBL_TRUE_MIN) — the point below which exp(x) is not representable at all.
// Documentation only since the kExpMinNormal early-out in exp_cody subsumes it.
inline constexpr double kExpLo = -745.13321910194;
// ln(DBL_MIN): the 2^N reconstruction builds only NORMALIZED doubles, so an
// argument below this (exp ≤ one denormal ULP) is flushed to exactly 0.
inline constexpr double kExpMinNormal = -708.3964185322641;

inline constexpr double kSqrt2 = 1.4142135623730951;
inline constexpr double kInvSqrt2Pi = 0.398942280401432677939946059934381868;

// ── Cody CALERF erfc constants (double precision) ────────────────────────────
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

// 2^n as a normalized double, built by placing the biased exponent — mirrors the
// integer-exponent reconstruction in exp_pd. Precondition n ∈ [-1022, 1023]
// (guaranteed here: exp arguments are ≤ 0 after the underflow flush).
ATX_SERFC_INLINE double pow2i(int n) noexcept {
  const std::uint64_t biased =
      static_cast<std::uint64_t>(static_cast<std::int64_t>(n) + 1023) << 52;
  double out;
  std::memcpy(&out, &biased, sizeof(out));
  return out;
}

} // namespace serfc

// exp(x), scalar Cody-Waite (port of exp_pd). Cody-Waite reduction to
// r ∈ [−ln2/2, ln2/2], degree-11 Taylor, 2^N scaling. Arguments below
// ln(DBL_MIN) flush to 0 (see kExpMinNormal). On this hot path x ≤ 0 always.
[[nodiscard]] ATX_SERFC_INLINE double exp_cody(double x) noexcept {
  // SAFETY (UB): the compare is NEGATED, not `x < kExpMinNormal`. Every comparison in
  // this function is ORDERED, so a NaN argument used to fall through the early-out AND
  // both clamps, reach `Nf = nearbyint(NaN * kInvLn2)` = NaN, and hit
  // `static_cast<int>(NaN)` at the bottom — a floating-to-integer conversion whose
  // source is not representable, which is UNDEFINED BEHAVIOUR ([conv.fpint]/1), not
  // merely an unspecified value. The path is real: norm_cdf_erfc(NaN) -> erfc_nonneg
  // (y = NaN, so `y <= 0.46875` is false) -> exp_cody(-NaN). `!(x >= lo)` takes the
  // branch for NaN as well as for genuine underflow; the two are then separated so a
  // NaN still leaves as a NaN (std::exp semantics) instead of as a plausible 0.0.
  if (!(x >= serfc::kExpMinNormal)) {
    return (x != x) ? x : 0.0; // NaN in => NaN out; else underflow to exactly 0
  }
  // Overflow, matching std::exp. kExpHi IS ln(DBL_MAX) to the last bit, so nothing
  // above it has a finite representation. This used to CLAMP to kExpHi and lean on the
  // 2^N reconstruction accidentally producing +inf for N = 1024; with that
  // reconstruction corrected (the pow2i split at the bottom) a clamp would instead
  // saturate and hand back a FINITE DBL_MAX for an argument that genuinely overflows.
  // The lower clamp that used to sit beside it (xc < kExpLo) is unreachable — the
  // early-out above already returned for every x below kExpMinNormal, which is greater
  // than kExpLo — so it is gone rather than left as dead code.
  if (x > serfc::kExpHi) {
    return std::numeric_limits<double>::infinity();
  }

  const double Nf = std::nearbyint(x * serfc::kInvLn2); // round-to-nearest-even
  // Cody-Waite two-part reduction, FUSED (std::fma) to mirror exp_pd's
  // _mm256_fnmadd_pd exactly. kLn2Hi is the full-width double for ln2 (not a
  // truncated hi part), so Nf·kLn2Hi is NOT exact for |Nf|>1; a NON-fused
  // subtraction would leak ~Nf·ULP (≈1e-14 in the deep wings, |Nf|~144),
  // dragging erfc/φ off the ≈1e-16 the fused AVX2 path holds. The single-
  // rounding fma keeps the product's low bits, restoring machine precision.
  double r = std::fma(-Nf, serfc::kLn2Hi, x);
  r = std::fma(-Nf, serfc::kLn2Lo, r);

  // Degree-13 Taylor (two terms past exp_pd's degree-11). At the reduction-
  // interval edge |r| ≤ ln2/2, the degree-11 remainder r¹²/12! ≈ 6e-15 is the
  // AVX2 kernel's accuracy ceiling; the degree-13 remainder r¹⁴/14! ≈ 4e-18 is
  // negligible, so the scalar exp becomes rounding-limited (≈1e-16) rather than
  // truncation-limited — the scalar Φ/φ swap is then accuracy-improving, not just
  // neutral, versus the deep-wing libm baseline. Two extra FMAs, off-libm.
  double p = 1.0 / 6227020800.0;         // 1/13!
  p = std::fma(p, r, 1.0 / 479001600.0); // 1/12!
  p = std::fma(p, r, 1.0 / 39916800.0);  // 1/11!
  p = std::fma(p, r, 1.0 / 3628800.0);
  p = std::fma(p, r, 1.0 / 362880.0);
  p = std::fma(p, r, 1.0 / 40320.0);
  p = std::fma(p, r, 1.0 / 5040.0);
  p = std::fma(p, r, 1.0 / 720.0);
  p = std::fma(p, r, 1.0 / 120.0);
  p = std::fma(p, r, 1.0 / 24.0);
  p = std::fma(p, r, 1.0 / 6.0);
  p = std::fma(p, r, 0.5);
  p = std::fma(p, r, 1.0);
  p = std::fma(p, r, 1.0);

  // SAFETY (precondition): pow2i builds a NORMALIZED double from a biased exponent and
  // is only defined for n in [-1022, 1023]. Nf reaches 1024 at the TOP of the clamped
  // range — kExpHi = 709.782712893384 rounds to exactly 1024·ln2 — and pow2i(1024)
  // lays down the inf/NaN exponent pattern, so exp_cody(kExpHi) returned +inf where
  // std::exp returns ≈1.7976931348622732e308, just under DBL_MAX. Splitting the scale
  // into 2^(n-1)·2 keeps every intermediate normalized and overflows only if the true
  // result genuinely does. n < -1022 cannot occur: the early-out above already
  // returned for every x below ln(DBL_MIN), whose Nf is ≥ -1022.
  const int n = static_cast<int>(Nf);
  if (n > 1023) {
    return (p * serfc::pow2i(n - 1)) * 2.0;
  }
  return p * serfc::pow2i(n);
}

// erfc(y) for y ≥ 0, scalar (port of erfc_nonneg_pd) — branch-selects the ONE
// applicable Cody region. Callers pass y = |x|·(1/√2) ≥ 0.
[[nodiscard]] ATX_SERFC_INLINE double erfc_nonneg(double y) noexcept {
  const double ysq = y * y;

  // Region 1 (y ≤ 0.46875): erf via degree-4/4 rational in ysq; erfc = 1 − erf.
  // No exp on this branch.
  // All Horner steps use std::fma to mirror the AVX2 erfc_nonneg_pd's _mm256_
  // fmadd_pd (single-rounding), holding the rationals to ≈1e-16.
  if (y <= serfc::kCodyThresh) {
    double num = serfc::kCodyA[4];
    num = std::fma(num, ysq, serfc::kCodyA[0]);
    num = std::fma(num, ysq, serfc::kCodyA[1]);
    num = std::fma(num, ysq, serfc::kCodyA[2]);
    num = std::fma(num, ysq, serfc::kCodyA[3]);
    double den = 1.0;
    den = std::fma(den, ysq, serfc::kCodyB[0]);
    den = std::fma(den, ysq, serfc::kCodyB[1]);
    den = std::fma(den, ysq, serfc::kCodyB[2]);
    den = std::fma(den, ysq, serfc::kCodyB[3]);
    const double erf = y * num / den;
    return 1.0 - erf;
  }

  const double e = exp_cody(-ysq); // shared by regions 2 and 3

  // Region 2 (0.46875 < y ≤ 4): erfc = e·N(y)/D(y), degree-8/8.
  if (y <= 4.0) {
    double num = serfc::kCodyC[8];
    num = std::fma(num, y, serfc::kCodyC[0]);
    num = std::fma(num, y, serfc::kCodyC[1]);
    num = std::fma(num, y, serfc::kCodyC[2]);
    num = std::fma(num, y, serfc::kCodyC[3]);
    num = std::fma(num, y, serfc::kCodyC[4]);
    num = std::fma(num, y, serfc::kCodyC[5]);
    num = std::fma(num, y, serfc::kCodyC[6]);
    num = std::fma(num, y, serfc::kCodyC[7]);
    double den = 1.0;
    den = std::fma(den, y, serfc::kCodyD[0]);
    den = std::fma(den, y, serfc::kCodyD[1]);
    den = std::fma(den, y, serfc::kCodyD[2]);
    den = std::fma(den, y, serfc::kCodyD[3]);
    den = std::fma(den, y, serfc::kCodyD[4]);
    den = std::fma(den, y, serfc::kCodyD[5]);
    den = std::fma(den, y, serfc::kCodyD[6]);
    den = std::fma(den, y, serfc::kCodyD[7]);
    return e * (num / den);
  }

  // Region 3 (y > 4): asymptotic erfc = e·(1/√π − w·N(w)/D(w))/y, w = 1/y².
  const double w = 1.0 / ysq;
  double num = serfc::kCodyP[5];
  num = std::fma(num, w, serfc::kCodyP[0]);
  num = std::fma(num, w, serfc::kCodyP[1]);
  num = std::fma(num, w, serfc::kCodyP[2]);
  num = std::fma(num, w, serfc::kCodyP[3]);
  num = std::fma(num, w, serfc::kCodyP[4]);
  double den = 1.0;
  den = std::fma(den, w, serfc::kCodyQ[0]);
  den = std::fma(den, w, serfc::kCodyQ[1]);
  den = std::fma(den, w, serfc::kCodyQ[2]);
  den = std::fma(den, w, serfc::kCodyQ[3]);
  den = std::fma(den, w, serfc::kCodyQ[4]);
  double r = w * (num / den);
  r = (serfc::kCodySqrtPiInv - r) / y;
  return e * r;
}

// Standard-normal CDF Φ(x) = ½·erfc(−x/√2), full range (Cody). Scalar sibling of
// norm_cdf_erfc_pd. Drop-in replacement for atx::core::norm_cdf on the hot path.
[[nodiscard]] ATX_SERFC_INLINE double norm_cdf_erfc(double x) noexcept {
  const double y = std::fabs(x) * (1.0 / serfc::kSqrt2);
  const double half_ea = 0.5 * erfc_nonneg(y);
  // x ≥ 0 → 1 − ½·erfc(|x|/√2); x < 0 → ½·erfc(|x|/√2).
  return (x >= 0.0) ? (1.0 - half_ea) : half_ea;
}

// Standard-normal density φ(x) = (1/√2π)·exp(−½x²), scalar Cody exp. Drop-in
// replacement for atx::core::norm_pdf on the hot path.
[[nodiscard]] ATX_SERFC_INLINE double norm_pdf_cody(double x) noexcept {
  return serfc::kInvSqrt2Pi * exp_cody(-0.5 * x * x);
}

} // namespace atx::vol::detail

#undef ATX_SERFC_INLINE
