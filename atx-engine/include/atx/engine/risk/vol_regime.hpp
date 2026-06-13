#pragma once

// atx::engine::risk — Volatility Regime Adjustment (VRA) bias statistic + λ² (S8.5).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  A fixed half-life UNDER-forecasts risk in a sudden volatility regime: the
//  forecast factor covariance F was estimated over a window that pre-dates the
//  shift, so its diagonal variances are too small for the realized recent moves.
//  vol_regime_multiplier(fseries, f_forecast, vra_halflife) measures this with a
//  market-wide volatility-regime multiplier λ² and returns it alongside the
//  per-date factor cross-sectional bias-statistic series B_t (the statistical-
//  analysis diagnostic surface).
//
//  Per kept date t (row t of `fseries`, row 0 = NEWEST — same convention as
//  cov_ewma.hpp):
//      B_t = √( (1/Kt) · Σ_j ( f_{t,j} / σ_j )² ) ,   σ_j = √F_jj
//  i.e. the RMS of the realized factor returns standardized by their forecast
//  vol. A factor j with F_jj ≤ 0 (a degenerate forecast column) is EXCLUDED from
//  the sum and from the count Kt (a divide-by-σ_j=0 guard); if EVERY factor is
//  degenerate for a row, Kt==0 ⇒ B_t = 0. E[B_t²]=1 if the forecast is unbiased;
//  B_t²>1 in a high-vol regime the fixed half-life under-forecast.
//
//  The multiplier is the half-life-weighted mean of B_t² (the SAME geometric decay
//  as cov_ewma.hpp's ewma_weights, row 0 newest, w_t = 2^(−t/H)):
//      λ² = EWMA(B_t² ; H) = Σ_t w_t·B_t² / Σ_t w_t .
//  λ² > 0 always (a convex combination of non-negative B_t² with positive weights;
//  it can only be exactly 0 if every B_t is 0, the all-degenerate-forecast case).
//
//  The BUILDER (factor_model.hpp) calls this ONLY when vra_halflife>0, so the
//  kernel never sees H==0 in the build path. (H==0 would be ewma_weights' equal-
//  weight sentinel; we keep that behavior for direct callers, but vra_halflife==0
//  is the no-VRA sentinel that makes the builder skip VRA entirely.) The builder
//  then rescales F ← λ²·F (PSD-preserving: a positive scalar) and D ← λ²·D (the
//  SAME market-wide multiplier on the specific variances — a documented
//  simplification; per-name specific VRA needs a dense specific panel and is a
//  recorded backlog residual). The diagnostic B_t series is NOT threaded into
//  FactorModel — the apply interface (risk/apply_inverse/neutralize) is unchanged.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG, no clock, no map iteration. Every reduction runs in canonical ascending
//  order (row/date t, then factor j). PURE given (fseries, f_forecast, vra_halflife)
//  ⇒ byte-identical output for identical inputs.
//
//  Research §4.4 (USE4). Citation: MSCI Barra USE4 Methodology Notes Tables
//  4.1/5.1; Menchero-Wang-Orr.

#include <cmath>   // std::sqrt
#include <utility> // std::move

#include <Eigen/Dense>

#include "atx/core/types.hpp" // f64, usize

#include "atx/core/linalg/linalg.hpp" // MatX, VecX (column-major Eigen)

namespace atx::engine::risk {

// The VRA result: the market-wide volatility-regime multiplier λ² (applied to F and
// D in the build path) and the per-kept-date factor cross-sectional bias-statistic
// series B_t (length == fseries.rows() == T; the statistical-analysis diagnostic).
struct RegimeAdjust {
  atx::f64 lambda2 = 0.0; // EWMA(B_t² ; H) — the F/D rescale; > 0 unless all-degenerate
  atx::core::linalg::VecX bias_stats; // B_t per kept date (row 0 = newest); length T
};

namespace detail {

// Per-date factor cross-sectional bias statistic for every kept date t:
//   B_t = √( (1/Kt) · Σ_{j: F_jj>0} ( f_{t,j} / σ_j )² ) ,  σ_j = √F_jj
// `inv_sigma[j]` carries 1/σ_j precomputed for F_jj>0 (0 otherwise); a factor j is
// included in the K-average iff inv_sigma[j] > 0.0. Kt is computed ONCE before the
// row loop (degeneracy is a forecast property, constant across rows). Kt==0 ⇒
// B_t = 0 (no usable factor). Returns the length-T series. Order-fixed (ascending
// row t, then factor j).
[[nodiscard]] inline atx::core::linalg::VecX
bias_statistics(const atx::core::linalg::MatX &fseries, const atx::core::linalg::VecX &inv_sigma) {
  const Eigen::Index t = fseries.rows();
  const Eigen::Index k = fseries.cols();
  atx::core::linalg::VecX b(t);
  // The usable-factor count Kt is a forecast property (F_jj>0), so it is the same for
  // every row; compute it once from inv_sigma (a strictly positive entry == usable).
  Eigen::Index kt = 0;
  for (Eigen::Index j = 0; j < k; ++j) {
    if (inv_sigma[j] > 0.0) {
      ++kt;
    }
  }
  for (Eigen::Index r = 0; r < t; ++r) {
    if (kt == 0) {
      b[r] = 0.0; // no usable factor this row ⇒ B_t = 0 (degenerate-forecast guard)
      continue;
    }
    atx::f64 ss = 0.0; // Σ_j (f_{t,j}/σ_j)² over usable j (order-fixed ascending j)
    for (Eigen::Index j = 0; j < k; ++j) {
      const atx::f64 is = inv_sigma[j];
      if (is > 0.0) {
        const atx::f64 z = fseries(r, j) * is;
        ss += z * z;
      }
    }
    b[r] = std::sqrt(ss / static_cast<atx::f64>(kt));
  }
  return b;
}

// EWMA of B_t² at half-life H: λ² = Σ_t w_t·B_t² / Σ_t w_t, w_t = 2^(−t/H) (row 0
// newest, the SAME geometric decay as cov_ewma.hpp::detail::ewma_weights). H==0 ⇒
// equal weights (w_t ≡ 1). T must be >= 1 (the builder guarantees kept rows). The
// weights are strictly positive, so λ² is a convex combination of the non-negative
// B_t² ⇒ λ² >= 0, and λ² == 0 only if every B_t == 0. Order-fixed (ascending t).
[[nodiscard]] inline atx::f64 ewma_bias_square(const atx::core::linalg::VecX &bias_stats,
                                               atx::usize half_life) noexcept {
  const Eigen::Index t = bias_stats.size();
  const bool equal = (half_life == 0U);
  const atx::f64 inv_h = equal ? 0.0 : 1.0 / static_cast<atx::f64>(half_life);
  atx::f64 sw = 0.0;   // Σ_t w_t
  atx::f64 swbb = 0.0; // Σ_t w_t·B_t²
  for (Eigen::Index r = 0; r < t; ++r) {
    const atx::f64 w = equal ? 1.0 : std::pow(2.0, -static_cast<atx::f64>(r) * inv_h);
    const atx::f64 bb = bias_stats[r] * bias_stats[r];
    sw += w;
    swbb += w * bb;
  }
  return (sw > 0.0) ? swbb / sw : 0.0; // sw>0 always for T>=1; the guard is defensive
}

} // namespace detail

// ===========================================================================
//  vol_regime_multiplier — the S8.5 VRA kernel.
//
//  Contract:
//    * `fseries`     : T×K per-date factor returns, ROW 0 = NEWEST (see header).
//      Precondition T >= 1 and K >= 1 (the builder guarantees kept rows >= K >= 1).
//    * `f_forecast`  : the K×K forecast factor covariance; only its diagonal F_jj
//      (the forecast factor variance σ_j²) is read. Must be K×K (== fseries.cols()).
//    * `vra_halflife`: the EWMA half-life H for the λ² weighting. The builder only
//      calls with H >= 1 (vra_halflife==0 is its no-VRA sentinel); H==0 here is the
//      equal-weight path (kept for direct callers).
//    * Returns RegimeAdjust{ λ², B_t-series }. λ² > 0 unless every forecast diagonal
//      is non-positive (then λ² == 0 and B_t ≡ 0). PURE, RNG-free, order-fixed ⇒
//      byte-identical for identical inputs.
//  Steps: precompute 1/σ_j for F_jj>0 ⇒ per-date B_t ⇒ EWMA of B_t² ⇒ λ².
// ===========================================================================
[[nodiscard]] inline RegimeAdjust vol_regime_multiplier(const atx::core::linalg::MatX &fseries,
                                                        const atx::core::linalg::MatX &f_forecast,
                                                        atx::usize vra_halflife) {
  const Eigen::Index k = fseries.cols();
  // Precompute 1/σ_j (j with F_jj>0); 0 flags a degenerate factor to exclude from B_t.
  atx::core::linalg::VecX inv_sigma(k);
  for (Eigen::Index j = 0; j < k; ++j) {
    const atx::f64 var = f_forecast(j, j);
    inv_sigma[j] = (var > 0.0) ? 1.0 / std::sqrt(var) : 0.0;
  }
  atx::core::linalg::VecX bias_stats = detail::bias_statistics(fseries, inv_sigma);
  const atx::f64 lambda2 = detail::ewma_bias_square(bias_stats, vra_halflife);
  return RegimeAdjust{lambda2, std::move(bias_stats)};
}

} // namespace atx::engine::risk
