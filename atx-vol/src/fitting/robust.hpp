#pragma once

// Robust-statistics / calibration-math helpers for the atx-vol calibrators.
//
// Ported from the C `ats-vol` library (ats_vol_huber.h / ats_vol_huber.c). The
// C8 and CStar surface calibrators drive a block-coordinate Levenberg-Marquardt
// fit whose outer loop is IRLS with Huber reweighting; the single shared helper
// they depend on is the q90-anchored strided Huber weight
// (`ats_vol_huber_weights_strided`, ported here as `huber_weights_strided`).
//
// Alongside the ported calibrator helper this header supplies the two textbook
// Huber M-estimator primitives — the loss rho(r) and the IRLS weight w(r) — plus
// an exact order-statistic quantile. atx-core already provides the generic
// numeric layer (clamp/lerp/sign/norm_cdf in atx/core/math.hpp) and a *streaming*
// P2Quantile estimator (atx/core/stats/quantile.hpp); the latter is an
// approximation, so the exact q90 the C calibrator anchors its scale on is
// computed here instead.
//
// Every entry is a pure function of its arguments — no globals, no dynamic
// allocation (the strided helper's subsample lives in a fixed 64-slot stack
// buffer) — so concurrent calls from any threads are safe. No exceptions.

#include <algorithm> // std::sort
#include <array>     // std::array
#include <concepts>  // std::floating_point
#include <cstddef>   // std::size_t
#include <span>      // std::span

namespace atx::vol::detail {

// Standard Huber tuning constant: 1.345 gives ~95% asymptotic efficiency at the
// Gaussian while bounding the influence of outliers. Selected by the C helper
// whenever the caller passes k <= 0.
template <std::floating_point T>
inline constexpr T kHuberDefaultK = T{1.345};

// Lower floor on the robust scale, so a degenerate all-zero residual chain
// (q90 == 0) does not divide by zero. Mirrors the C helper's 1e-9 clamp.
template <std::floating_point T>
inline constexpr T kHuberScaleFloor = T{1e-9};

// ── Textbook Huber M-estimator primitives ───────────────────────────────

// Huber loss rho(r): quadratic near zero, linear in the tails, C1 at |r| == k.
//
//   rho(r) = ½·r²                 for |r| <= k   (least-squares core)
//          = k·(|r| − ½·k)        for |r| >  k   (bounded-influence tail)
//
// The two branches meet in value and slope at |r| == k (both give ½·k² and
// slope k), so rho is continuously differentiable everywhere.
//
// @param r  Residual (signed; only its magnitude matters).
// @param k  Transition threshold (> 0). k <= 0 is out of contract.
// @return   The Huber loss at r.
template <std::floating_point T>
[[nodiscard]] constexpr T huber_loss(T r, T k) noexcept {
  const T a = (r < T{0}) ? -r : r; // |r|
  if (a <= k) {
    return T{0.5} * r * r; // == ½·a²
  }
  return k * (a - T{0.5} * k);
}

// Huber IRLS weight w(r): the multiplier that turns the M-estimate into an
// iteratively-reweighted least-squares step.
//
//   w(r) = 1        for |r| <= k
//        = k / |r|  for |r| >  k
//
// The division is only reached when |r| > k, so with the documented k > 0
// precondition |r| is strictly positive there — no division by zero.
//
// @param r  Residual (signed; only its magnitude matters).
// @param k  Transition threshold (> 0). k <= 0 is out of contract.
// @return   The IRLS weight at r, in (0, 1].
template <std::floating_point T>
[[nodiscard]] constexpr T huber_weight(T r, T k) noexcept {
  const T a = (r < T{0}) ? -r : r; // |r|
  if (a <= k) {
    return T{1};
  }
  return k / a;
}

// ── Exact order-statistic quantile ──────────────────────────────────────

// Lower/truncated-index p-quantile of an ASCENDING-sorted range.
//
// Returns the order statistic at index floor(p·(n−1)); it does NOT interpolate
// between neighbours. This is the convention the C `ats-vol` helper uses to pick
// its q90 anchor, reproduced exactly so the ported weights are bit-faithful.
//
// @param sorted  Range sorted ascending. Empty input returns T{0} (no data).
// @param p       Probability in [0, 1]; values outside are clamped by the index.
// @return        sorted[floor(p·(n−1))], or T{0} if sorted is empty.
template <std::floating_point T>
[[nodiscard]] constexpr T quantile_sorted_lower(std::span<const T> sorted,
                                                T p) noexcept {
  const std::size_t n = sorted.size();
  if (n == 0U) {
    return T{0};
  }
  const T rank = p * static_cast<T>(n - 1U);
  std::size_t idx = static_cast<std::size_t>(rank); // truncates toward zero
  if (idx >= n) {
    idx = n - 1U; // defensive clamp for p >= 1 or rounding at the top edge
  }
  return sorted[idx];
}

// ── q90-anchored strided Huber weights (calibrator helper) ──────────────

// Compute Huber weights `w_out[i]` from absolute residuals `r_abs[i]`, scaled by
// the q90 of a strided subsample (<= 64 entries) so the robust scale matches the
// chain's overall residual distribution rather than being biased toward one wing
// by raw strike order.
//
// Weight formula (the C calibrator's soft Holland-Welsch-style variant — note
// this is NOT the textbook k/|r| of huber_weight above):
//
//   scale  = max(q90, 1e-9)
//   rr     = max(r_abs[i], 0) / scale
//   excess = max(0, rr − k)
//   w[i]   = 1 / (1 + excess)²
//
// For a chain of n > 64 the q90 is taken over a uniform 64-point stride across
// [0, n); for n <= 64 all residuals are used. `k <= 0` selects the standard
// 1.345 default.
//
// @param r_abs  Absolute residuals (per observation). Empty input is a no-op.
// @param w_out  Output weights; MUST be the same length as r_abs (else no-op).
// @param k      Huber threshold; k <= 0 selects kHuberDefaultK (1.345).
//
// @pre w_out.size() == r_abs.size(). Mismatched or empty spans leave w_out
//      untouched.
template <std::floating_point T>
inline void huber_weights_strided(std::span<const T> r_abs, std::span<T> w_out,
                                  T k = kHuberDefaultK<T>) noexcept {
  const std::size_t n = r_abs.size();
  if (n == 0U || w_out.size() != n) {
    return;
  }

  // Build the (<= 64) subsample the q90 is measured over. Unlike the C helper,
  // which aliased the output buffer as sort scratch when n > 64, we always sort
  // a private stack buffer — the result is identical (w_out is fully rewritten
  // in the second pass) and the aliasing hazard is gone.
  constexpr std::size_t kMaxSub = 64U;
  const std::size_t ns = (n <= kMaxSub) ? n : kMaxSub;
  std::array<T, kMaxSub> buf{};
  if (n <= kMaxSub) {
    for (std::size_t i = 0U; i < ns; ++i) {
      buf[i] = r_abs[i];
    }
  } else {
    for (std::size_t i = 0U; i < ns; ++i) {
      // Fixed-point map [0, ns-1] -> [0, n-1]; ns-1 == 63 here so never /0.
      const std::size_t pos = (i * (n - 1U)) / (ns - 1U);
      buf[i] = r_abs[pos];
    }
  }

  std::span<T> sub{buf.data(), ns};
  std::sort(sub.begin(), sub.end());
  const T q90 = quantile_sorted_lower<T>(std::span<const T>{sub}, T{0.9});

  const T k_eff = (k > T{0}) ? k : kHuberDefaultK<T>;
  const T scale = (q90 > kHuberScaleFloor<T>) ? q90 : kHuberScaleFloor<T>;

  for (std::size_t i = 0U; i < n; ++i) {
    const T ri = (r_abs[i] > T{0}) ? r_abs[i] : T{0};
    const T rr = ri / scale;
    const T excess = (rr > k_eff) ? (rr - k_eff) : T{0};
    const T denom = T{1} + excess;
    w_out[i] = T{1} / (denom * denom);
  }
}

} // namespace atx::vol::detail
