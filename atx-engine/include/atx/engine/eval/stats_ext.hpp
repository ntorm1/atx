#pragma once

// atx::engine::eval — distribution + higher-moment numeric helpers (Sprint S1).
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  A small, self-contained numeric base for the evaluation/validation spine:
//  the standard-normal CDF/inverse-CDF, population skewness/excess-kurtosis, an
//  exact (order-statistic) median, and equity-curve -> simple-return conversion.
//  These are the primitives the deflated-Sharpe (S1-2), PBO (S1-3), and the
//  added performance metrics (perf_metrics.hpp, S1-1) build on.
//
//  Why engine-local (the combine/correlation.hpp precedent): atx-core's stats
//  layer exposes online accumulators (RunningVariance/Welford) but NO normal
//  distribution, higher moments, or exact median. Rather than wrap or fork
//  atx-core, these are implemented here as pure, allocation-light free functions
//  over std::erfc / std::nth_element — one documented definition, no new
//  atx-core dependency. (Upstreaming to atx-core is a recorded sprint residual.)
//
// ===========================================================================
//  Numeric conventions (load-bearing, documented)
// ===========================================================================
//  * Moments are POPULATION (biased, divisor n), to stay consistent with
//    combine::compute_metrics's population std — the eval spine never mixes a
//    sample (n-1) convention into the same pipeline.
//  * Reductions are ORDER-FIXED: every sum walks the span in ascending index so
//    the floating-point result is deterministic and run-to-run byte-identical.
//  * No RNG anywhere. norm_ppf is a fixed rational approximation (Acklam) plus a
//    single deterministic Halley refinement step.
//
//  Ownership/threading: every function is pure (a function of its inputs only),
//  takes a non-owning std::span, and is thread-safe. median() makes ONE internal
//  copy (it must reorder to select order statistics) — see its SAFETY note.

#include <algorithm> // std::nth_element, std::max_element
#include <cmath>     // std::erfc, std::sqrt, std::exp, std::log, std::abs
#include <cstddef>   // std::ptrdiff_t (nth_element iterator offsets)
#include <span>      // std::span
#include <vector>    // std::vector (median scratch copy, returns_from_equity out)

#include "atx/core/types.hpp" // atx::f64, atx::usize

namespace atx::engine::eval {

// ===========================================================================
//  Named constants (no magic numbers; portable — NOT the non-portable M_* macros
//  which are absent under /permissive-).
// ===========================================================================
// 1/sqrt(2): the argument scale for the erfc-based standard-normal CDF.
inline constexpr atx::f64 kSqrt1_2 = 0.70710678118654752440;
// sqrt(2*pi): the standard-normal density normaliser used by the Halley step.
inline constexpr atx::f64 kSqrt2Pi = 2.5066282746310002;

namespace detail {

// Acklam's rational-approximation coefficients for the inverse normal CDF.
// (P. J. Acklam, "An algorithm for computing the inverse normal cumulative
// distribution function"; relative error < 1.15e-9 before refinement.)
inline constexpr atx::f64 kAcklamA[6] = {
    -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
    1.383577518672690e+02,  -3.066479806614716e+01, 2.506628277459239e+00};
inline constexpr atx::f64 kAcklamB[5] = {
    -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
    6.680131188771972e+01,  -1.328068155288572e+01};
inline constexpr atx::f64 kAcklamC[6] = {
    -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
    -2.549732539343734e+00, 4.374664141464968e+00,  2.938163982698783e+00};
inline constexpr atx::f64 kAcklamD[4] = {
    7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
    3.754408661907416e+00};
// Tail/central split points for the piecewise approximation.
inline constexpr atx::f64 kAcklamPLow = 0.02425;
inline constexpr atx::f64 kAcklamPHigh = 1.0 - kAcklamPLow; // 0.97575

} // namespace detail

// ===========================================================================
//  norm_cdf — standard-normal cumulative distribution Φ(x).
//
//  Φ(x) = 0.5 * erfc(-x / sqrt(2)). Exact-to-the-library-erfc; Φ(0) = 0.5 and
//  Φ(1.959963985) ≈ 0.975. Pure, no allocation.
// ===========================================================================
[[nodiscard]] inline atx::f64 norm_cdf(atx::f64 x) noexcept {
  return 0.5 * std::erfc(-x * kSqrt1_2);
}

// ===========================================================================
//  norm_ppf — inverse standard-normal CDF Φ⁻¹(p), p in (0, 1).
//
//  Acklam's piecewise rational approximation followed by ONE Halley refinement
//  step against norm_cdf (lifts the accuracy to ~machine precision). Φ⁻¹(0.975)
//  ≈ 1.959963985. p outside (0, 1) is a precondition violation; the rational
//  forms degenerate there (the caller — DSR/PBO — only ever passes interior
//  probabilities), so no runtime guard is emitted on this pure hot path.
// ===========================================================================
[[nodiscard]] inline atx::f64 norm_ppf(atx::f64 p) noexcept {
  const atx::f64 *a = detail::kAcklamA;
  const atx::f64 *b = detail::kAcklamB;
  const atx::f64 *c = detail::kAcklamC;
  const atx::f64 *d = detail::kAcklamD;

  atx::f64 x = 0.0;
  if (p < detail::kAcklamPLow) {
    // Lower tail: q = sqrt(-2 ln p).
    const atx::f64 q = std::sqrt(-2.0 * std::log(p));
    x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  } else if (p <= detail::kAcklamPHigh) {
    // Central region: q = p - 0.5, r = q².
    const atx::f64 q = p - 0.5;
    const atx::f64 r = q * q;
    x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
        (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
  } else {
    // Upper tail: q = sqrt(-2 ln(1-p)); negated lower-tail form.
    const atx::f64 q = std::sqrt(-2.0 * std::log(1.0 - p));
    x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  }

  // One Halley step: refine x against the true CDF. e = Φ(x) - p; u scales the
  // Newton step by the (inverse) density, the trailing term is Halley's
  // second-order correction.
  const atx::f64 e = norm_cdf(x) - p;
  const atx::f64 u = e * kSqrt2Pi * std::exp(x * x / 2.0);
  x -= u / (1.0 + x * u / 2.0);
  return x;
}

// ===========================================================================
//  MeanStd — population mean + population standard deviation of a span.
// ===========================================================================
struct MeanStd {
  atx::f64 mean; // arithmetic mean; 0 for an empty span
  atx::f64 std;  // population std (divisor n); 0 for empty/constant span
};

// Population mean/std over r, ascending-index. Empty span -> {0, 0}.
[[nodiscard]] inline MeanStd mean_std_pop(std::span<const atx::f64> r) noexcept {
  const atx::usize n = r.size();
  if (n == 0U) {
    return MeanStd{0.0, 0.0};
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  atx::f64 sum = 0.0;
  for (const atx::f64 v : r) {
    sum += v;
  }
  const atx::f64 mean = sum / nf;
  atx::f64 sq = 0.0;
  for (const atx::f64 v : r) {
    const atx::f64 dv = v - mean;
    sq += dv * dv;
  }
  return MeanStd{mean, std::sqrt(sq / nf)};
}

// ===========================================================================
//  skewness — population (biased) skewness γ3 = (1/n) Σ ((rᵢ-μ)/σ)³.
//
//  σ == 0 (constant or empty span) is degenerate -> 0 (no shape information).
// ===========================================================================
[[nodiscard]] inline atx::f64 skewness(std::span<const atx::f64> r) noexcept {
  const MeanStd ms = mean_std_pop(r);
  if (ms.std == 0.0) {
    return 0.0;
  }
  const atx::f64 nf = static_cast<atx::f64>(r.size());
  const atx::f64 inv_std = 1.0 / ms.std;
  atx::f64 acc = 0.0;
  for (const atx::f64 v : r) {
    const atx::f64 z = (v - ms.mean) * inv_std;
    acc += z * z * z;
  }
  return acc / nf;
}

// ===========================================================================
//  excess_kurtosis — population (biased) excess kurtosis
//  κ = (1/n) Σ ((rᵢ-μ)/σ)⁴ − 3.
//
//  σ == 0 (constant or empty span) is degenerate -> 0.
// ===========================================================================
[[nodiscard]] inline atx::f64 excess_kurtosis(std::span<const atx::f64> r) noexcept {
  const MeanStd ms = mean_std_pop(r);
  if (ms.std == 0.0) {
    return 0.0;
  }
  const atx::f64 nf = static_cast<atx::f64>(r.size());
  const atx::f64 inv_std = 1.0 / ms.std;
  atx::f64 acc = 0.0;
  for (const atx::f64 v : r) {
    const atx::f64 z = (v - ms.mean) * inv_std;
    const atx::f64 z2 = z * z;
    acc += z2 * z2;
  }
  return acc / nf - 3.0;
}

// ===========================================================================
//  median — exact median via order statistics (deterministic).
//
//  Odd n: the central order statistic. Even n: the mean of the two central
//  order statistics. Empty span -> 0. Pure (the input span is never mutated).
// ===========================================================================
[[nodiscard]] inline atx::f64 median(std::span<const atx::f64> r) {
  const atx::usize n = r.size();
  if (n == 0U) {
    return 0.0;
  }
  // SAFETY: std::nth_element reorders its range in place, so we cannot operate on
  //         the caller's (const) span — we copy into owned scratch first. The copy
  //         keeps median() pure (caller data is untouched) and deterministic.
  std::vector<atx::f64> scratch(r.begin(), r.end());
  const atx::usize mid = n / 2U;
  std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid), scratch.end());
  const atx::f64 hi = scratch[mid];
  if (n % 2U == 1U) {
    return hi;
  }
  // Even n: the lower-central order statistic is the max of the left partition,
  // which nth_element has already placed before `mid`.
  const atx::f64 lo = *std::max_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid));
  return 0.5 * (lo + hi);
}

// ===========================================================================
//  returns_from_equity — simple periodic returns from an equity LEVEL series.
//
//  rᵢ = eq[i]/eq[i-1] − 1 for i ≥ 1 (output length n-1; empty for n < 2). A
//  non-positive prior level eq[i-1] <= 0 yields 0 for that step (a wiped-out or
//  invalid book has no well-defined return). Allocates the output vector.
// ===========================================================================
[[nodiscard]] inline std::vector<atx::f64> returns_from_equity(std::span<const atx::f64> equity) {
  const atx::usize n = equity.size();
  std::vector<atx::f64> out;
  if (n < 2U) {
    return out;
  }
  out.reserve(n - 1U);
  for (atx::usize i = 1U; i < n; ++i) {
    const atx::f64 prev = equity[i - 1U];
    out.push_back((prev <= 0.0) ? 0.0 : equity[i] / prev - 1.0);
  }
  return out;
}

} // namespace atx::engine::eval
