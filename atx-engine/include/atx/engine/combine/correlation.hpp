#pragma once

// atx::engine::combine — pairwise-complete Pearson correlation (the ONE shared
// NaN-policy helper, §3.3).
//
// ===========================================================================
//  Why this is a standalone helper, not a gate-private function
// ===========================================================================
//  The pairwise-complete NaN policy is the load-bearing numeric convention of
//  the combination layer: the gate (P4-3) uses it to compute a candidate's
//  correlation-to-pool, and the combiner (P4-4) reuses the EXACT same helper to
//  estimate the cross-alpha correlation matrix. Per the P4-3 contract ("the
//  pairwise-complete NaN policy is documented once and shared with the combiner
//  — one helper") this lives in its own header so both consumers share one
//  definition and one documented convention. atx-core only exposes a fixed-
//  window `RollingCorrelation<W>` (no pairwise-complete NaN skipping), so the
//  small single-pass Pearson is implemented here rather than wrapped.
//
// ===========================================================================
//  Pairwise-complete NaN policy (§3.3) — the documented convention
// ===========================================================================
//  pairwise_complete_corr(a, b) is the Pearson correlation computed ONLY over
//  the indices i where BOTH a[i] and b[i] are non-NaN. An index where EITHER leg
//  is NaN is skipped entirely (the pair contributes to nothing — neither mean,
//  variance, nor covariance). a and b must be the same length (asserted); the
//  i-th elements are paired observations of the same date.
//
//  DEGENERATE-PAIR CONVENTION (documented, load-bearing):
//    The function returns 0.0 when there are FEWER THAN 2 valid pairs, or when
//    either leg has zero variance over the valid pairs. Rationale: a degenerate
//    or non-overlapping pair carries no information about co-movement, so it must
//    contribute 0 to a magnitude gate rather than a spurious large |corr| — it
//    can never trip the gate falsely. In particular an ALL-NaN candidate has 0
//    valid pairs vs. every pool member, so its corr-to-pool is 0 and it PASSES
//    the correlation gate; that is correct — the quality floors (Sharpe/fitness/
//    turnover), checked first, are responsible for rejecting a junk candidate,
//    not the orthogonality gate.
//
//  STRUCTURAL INDEX-0 NOTE: the PnL streams carry a structural 0 at index 0
//  (AlphaStreams convention — period 0 has no prior). §5.2 says "over dates where
//  BOTH are non-NaN"; 0 is non-NaN, so per the verbatim spec index 0 IS a valid
//  pair and is INCLUDED here (no special-casing). This is intentional: Pearson is
//  mean-centered, so a shared near-constant leading point has negligible effect
//  on the correlation. (P4-2's metrics excludes index 0 from its MOMENTS for a
//  different reason — a structural 0 biases mean/variance — but a shared leading
//  point does not bias a correlation, so the two units' index-0 handling differs
//  deliberately and correctly.)
//
//  Numerics: single pass accumulating Σa, Σb, Σab, Σa², Σb², n over the valid
//  pairs, then Pearson = (n·Σab − Σa·Σb) / sqrt((n·Σa² − Σa²)(n·Σb² − Σb²)).
//  The denominator is guarded: if either factor is <= 0 (zero/negative variance
//  from rounding) the result is the 0.0 degenerate value. The output is clamped
//  to [-1, 1] to absorb floating-point overshoot at the perfect-correlation ends.
//
//  Thread-safety: a pure function of its inputs; no shared state.

#include <cmath> // std::isnan, std::sqrt
#include <span>  // std::span

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // atx::f64, atx::usize

namespace atx::engine::combine {

// ===========================================================================
//  pairwise_complete_corr — Pearson over the BOTH-non-NaN overlap of a and b.
//
//  PRECONDITION: a.size() == b.size() (asserted; paired observations). Returns
//  0.0 for < 2 valid pairs or a zero-variance leg (the documented degenerate
//  convention above). Otherwise returns the Pearson correlation in [-1, 1].
// ===========================================================================
[[nodiscard]] inline atx::f64 pairwise_complete_corr(std::span<const atx::f64> a,
                                                     std::span<const atx::f64> b) noexcept {
  ATX_ASSERT(a.size() == b.size());
  atx::f64 sa = 0.0;  // Σ a_i over valid pairs
  atx::f64 sb = 0.0;  // Σ b_i
  atx::f64 sab = 0.0; // Σ a_i·b_i
  atx::f64 saa = 0.0; // Σ a_i²
  atx::f64 sbb = 0.0; // Σ b_i²
  atx::usize n = 0U;  // count of valid (both-non-NaN) pairs
  for (atx::usize i = 0U; i < a.size(); ++i) {
    const atx::f64 ai = a[i];
    const atx::f64 bi = b[i];
    // Pairwise-complete: skip the index entirely if EITHER leg is NaN.
    if (std::isnan(ai) || std::isnan(bi)) {
      continue;
    }
    sa += ai;
    sb += bi;
    sab += ai * bi;
    saa += ai * ai;
    sbb += bi * bi;
    ++n;
  }

  // < 2 valid pairs: no co-movement information -> 0 (degenerate convention).
  if (n < 2U) {
    return 0.0;
  }
  const atx::f64 nf = static_cast<atx::f64>(n);
  const atx::f64 cov = nf * sab - sa * sb;   // n·Σab − Σa·Σb (∝ covariance)
  const atx::f64 var_a = nf * saa - sa * sa; // n·Σa² − (Σa)² (∝ var a)
  const atx::f64 var_b = nf * sbb - sb * sb; // n·Σb² − (Σb)² (∝ var b)
  // Zero (or rounding-negative) variance in either leg -> degenerate -> 0.
  if (var_a <= 0.0 || var_b <= 0.0) {
    return 0.0;
  }
  const atx::f64 corr = cov / std::sqrt(var_a * var_b);
  // Clamp floating-point overshoot at the perfect-correlation extremes.
  if (corr > 1.0) {
    return 1.0;
  }
  if (corr < -1.0) {
    return -1.0;
  }
  return corr;
}

} // namespace atx::engine::combine
