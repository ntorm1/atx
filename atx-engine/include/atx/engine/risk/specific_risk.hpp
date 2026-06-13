#pragma once

// atx::engine::risk — specific-risk model: EWMA + Newey-West + structural blend (S8.4).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  specific_risk_blend(x0, u_by_inst, window, spec_hl, spec_nw_lags, structural)
//  replaces the as-built plain population-variance specific risk
//  (detail::pop_variance of the WLS residual series) with the vendor-grade blended
//  idiosyncratic-variance estimator a risk shop actually trades:
//
//    1. TIME-SERIES estimate  σ_n^TS = √( ewma_var(r_n, spec_hl) · nw_inflation(r_n, L) )
//       EWMA-weighted variance of the per-instrument specific-return (residual)
//       series, INFLATED by a scalar Newey-West Bartlett long-run-variance ratio
//       (≥ 1 for positive autocorrelation; = 1 when L = 0).
//    2. STRUCTURAL model (opt-in via `structural`): regress ln σ_n^TS on the
//       instrument's exposure row over the names with FULL clean history (count ==
//       the max observed residual count), predict ln σ_n^STR for ALL names; the
//       structural σ is a smooth function of factor exposures, so it lends a sane
//       level to THIN-history names whose own σ^TS is a noisy near-zero.
//    3. BLEND  σ_n = γ_n·σ_n^TS + (1−γ_n)·σ_n^STR, with γ_n monotone in history
//       depth: γ_n = clamp(count_n / full, 0, 1). A full-history name keeps its own
//       σ^TS (γ=1); a thin name leans on the structural value (γ→0). D_n = σ_n²
//       (FactorModel::create re-floors at kSpecificVarFloor anyway, but D_n > 0).
//
//  This is OPT-IN: the builder dispatches here only for
//  SpecificRiskMethod::EwmaNeweyWestStructural; the default PopVariance path is the
//  as-built detail::pop_variance, byte-identical.
//
// ===========================================================================
//  Row order (LOAD-BEARING)
// ===========================================================================
//  `u_by_inst[inst]` is a per-universe-instrument residual series with INDEX 0 =
//  the NEWEST date (s = 0), the SAME newest-first ordering the EWMA factor series
//  uses (cov_ewma.hpp). The EWMA "k steps back" index is therefore the series index
//  itself, weight 2^(−k/H) on index k.
//
// ===========================================================================
//  ISC — issuer-specific covariance (INTERFACE ONLY this sprint)
// ===========================================================================
//  A multi-share-class universe wants same-issuer off-diagonal specific covariance
//  (two listings of one issuer share idiosyncratic shocks). We define the carrier
//  type IssuerSpecificCov + SpecificRisk{ variances, isc } so the result CAN later
//  carry those pairs, but LEAVE `isc` EMPTY and do NOT wire it into
//  FactorModel::create / the Woodbury path — D stays strictly DIAGONAL this sprint.
//  Populating + wiring is DEFERRED (it needs a multi-share-class universe and a
//  block-Woodbury apply path); recorded as an S8-b / backlog residual.
//
// ===========================================================================
//  Determinism
// ===========================================================================
//  NO RNG, no clock, no map iteration. Order-fixed reductions: ascending instrument,
//  then series index / lag. Trailing-window only (no look-ahead). Same input ⇒
//  byte-identical output.

#include <algorithm> // std::clamp, std::max
#include <cmath>     // std::log, std::exp, std::pow, std::sqrt
#include <utility>   // std::move
#include <vector>    // std::vector

#include <Eigen/Dense>

#include "atx/core/types.hpp" // f64, usize

#include "atx/core/linalg/linalg.hpp"     // MatX, VecX (column-major Eigen)
#include "atx/core/linalg/regression.hpp" // ols (structural ln-vol-on-exposures regression)

#include "atx/engine/risk/exposures.hpp" // ExposureMatrix

namespace atx::engine::risk {

// Floor on a per-instrument specific VARIANCE so D_n > 0 even for an empty / constant
// residual series (FactorModel::create re-floors at kSpecificVarFloor; this keeps the
// kernel's own output strictly positive and the ln() in the structural model finite).
inline constexpr atx::f64 kSpecificRiskFloor = 1e-12;

// ===========================================================================
//  ISC carrier — interface-only this sprint (see header). One same-issuer
//  off-diagonal specific-covariance entry (i, j) -> cov. Symmetric pair; i != j.
// ===========================================================================
struct IssuerSpecificCov {
  atx::usize i; // current-cross-section row index (into x0.instrument_rows order)
  atx::usize j; // current-cross-section row index, i != j
  atx::f64 cov; // same-issuer idiosyncratic covariance D_ij (off-diagonal)
};

// The blended specific-risk result: the M diagonal specific variances PLUS the
// (this sprint EMPTY) issuer-specific off-diagonal carrier. `isc` stays empty and is
// NOT wired into the Woodbury path — D is strictly diagonal this sprint.
struct SpecificRisk {
  atx::core::linalg::VecX variances;  // M specific variances D_n (> 0)
  std::vector<IssuerSpecificCov> isc; // EMPTY this sprint (interface-only)
};

namespace detail {

// Scalar EWMA-weighted variance of a residual series `r` (index 0 = newest). For
// half-life H the weight on index k is w_k = 2^(−k/H); H == 0 ⇒ equal weights. The
// EWMA mean is μ = Σ w_k r_k / Σ w_k; the variance is Σ w_k (r_k−μ)² / Σ w_k. A
// series of length < 2 has no spread to estimate ⇒ returns 0.0 (the caller floors).
// Order-fixed (ascending index). noexcept leaf math.
[[nodiscard]] inline atx::f64 ewma_var(const std::vector<atx::f64> &r,
                                       atx::usize half_life) noexcept {
  const atx::usize n = r.size();
  if (n < 2U) {
    return 0.0;
  }
  atx::f64 sw = 0.0;
  atx::f64 swx = 0.0;
  const atx::f64 inv_h = (half_life == 0U) ? 0.0 : 1.0 / static_cast<atx::f64>(half_life);
  for (atx::usize k = 0U; k < n; ++k) {
    const atx::f64 w = (half_life == 0U) ? 1.0 : std::pow(2.0, -static_cast<atx::f64>(k) * inv_h);
    sw += w;
    swx += w * r[k];
  }
  const atx::f64 mu = swx / sw;
  atx::f64 ss = 0.0;
  for (atx::usize k = 0U; k < n; ++k) {
    const atx::f64 w = (half_life == 0U) ? 1.0 : std::pow(2.0, -static_cast<atx::f64>(k) * inv_h);
    const atx::f64 d = r[k] - mu;
    ss += w * d * d;
  }
  return ss / sw;
}

// Scalar Newey-West Bartlett long-run-variance INFLATION factor for a residual
// series `r`: the ratio of the HAC long-run variance to the contemporaneous variance
// Γ_0, i.e.
//   inflation = 1 + Σ_{d=1..L} (1 − d/(L+1)) · 2·Γ_d / Γ_0 ,
// with Γ_d = (1/T) Σ_{t=d..T−1} (r_t−μ)(r_{t−d}−μ) the EQUAL-WEIGHTED lag-d
// autocovariance (μ the equal-weighted mean). For a positively-autocorrelated series
// this is ≥ 1 (the long-run variance exceeds the per-observation variance); L == 0,
// L ≥ T, a series shorter than 2, or a non-positive Γ_0 all return 1.0 (no
// adjustment). Clamped at ≥ a small positive floor so the resulting σ² stays > 0 even
// for a strongly negatively-autocorrelated pathological series. Order-fixed
// (ascending d). noexcept leaf math.
[[nodiscard]] inline atx::f64 nw_inflation(const std::vector<atx::f64> &r,
                                           atx::usize nw_lags) noexcept {
  const atx::usize n = r.size();
  if (nw_lags == 0U || n < 2U || nw_lags >= n) {
    return 1.0;
  }
  atx::f64 mean = 0.0;
  for (atx::usize t = 0U; t < n; ++t) {
    mean += r[t];
  }
  mean /= static_cast<atx::f64>(n);
  const atx::f64 inv_t = 1.0 / static_cast<atx::f64>(n);
  atx::f64 gamma0 = 0.0;
  for (atx::usize t = 0U; t < n; ++t) {
    const atx::f64 d = r[t] - mean;
    gamma0 += d * d;
  }
  gamma0 *= inv_t;
  if (gamma0 <= 0.0) {
    return 1.0; // constant series ⇒ no long-run-variance correction
  }
  const atx::f64 lp1 = static_cast<atx::f64>(nw_lags + 1U);
  atx::f64 lrv = gamma0; // long-run variance Γ_0 + Σ_d (1−d/(L+1))·2Γ_d
  for (atx::usize d = 1U; d <= nw_lags; ++d) {
    atx::f64 gamma_d = 0.0;
    for (atx::usize t = d; t < n; ++t) {
      gamma_d += (r[t] - mean) * (r[t - d] - mean);
    }
    gamma_d *= inv_t;
    const atx::f64 bart = 1.0 - static_cast<atx::f64>(d) / lp1;
    lrv += bart * 2.0 * gamma_d;
  }
  const atx::f64 ratio = lrv / gamma0;
  // A pathological negatively-autocorrelated series can drive the HAC long-run
  // variance non-positive; clamp the inflation to a small positive floor so σ² > 0.
  return (ratio > 1e-6) ? ratio : 1e-6;
}

} // namespace detail

// ===========================================================================
//  specific_risk_blend — the S8.4 kernel.
//
//  Contract:
//    * `x0`        : the CURRENT cross-section ExposureMatrix (X[0]); `.x` is M×K and
//                    `.instrument_rows[r]` is the universe-instrument index of row r.
//    * `u_by_inst` : per-universe-instrument specific-return (WLS-residual) series,
//                    INDEX 0 = NEWEST (see header). `u_by_inst[x0.instrument_rows[r]]`
//                    is row r's series; an absent / empty series ⇒ a thin name.
//    * `window`    : the fit window (rows); the maximum residual count a name can have.
//    * `spec_hl`   : EWMA half-life for the specific-return variance; 0 ⇒ equal weights.
//    * `spec_nw_lags` : Newey-West Bartlett lag count; 0 ⇒ no autocorrelation inflation.
//    * `structural`: when true, blend thin names toward the ln-vol-on-exposures model;
//                    when false, γ_n ≡ 1 (every name keeps its own σ^TS).
//    * Returns SpecificRisk{ variances (length M, each > 0), isc (EMPTY) }. PURE,
//      RNG-free, order-fixed ⇒ byte-identical for identical inputs.
//
//  γ_n rule (documented): γ_n = clamp(count_n / full, 0, 1), full = max_n count_n
//  (the deepest observed clean history; a dense window has full == window). Monotone
//  in history depth: full-history ⇒ γ=1 (own σ^TS), thin ⇒ γ→0 (structural).
//
//  Structural-fallback (documented): the structural OLS needs > K clean-history names
//  (rows with count_n == full) to be full column rank. With ≤ K such names — or if the
//  OLS is rank-deficient — we SKIP the structural model and set γ_n ≡ 1 for ALL names
//  (the time-series estimate is then the whole answer; no spurious structural level).
// ===========================================================================
[[nodiscard]] inline SpecificRisk
specific_risk_blend(const ExposureMatrix &x0, const std::vector<std::vector<atx::f64>> &u_by_inst,
                    atx::usize window, atx::usize spec_hl, atx::usize spec_nw_lags,
                    bool structural) {
  const atx::usize m = x0.n_instruments();
  const atx::usize k = x0.n_factors();

  // Per current-cross-section row: residual count, the time-series σ^TS, and γ_n.
  std::vector<atx::usize> count(m, 0U);
  atx::core::linalg::VecX sigma_ts(static_cast<Eigen::Index>(m));
  atx::usize full = 0U; // deepest observed clean history (max count); >= 1 if any data
  for (atx::usize r = 0U; r < m; ++r) {
    const std::vector<atx::f64> &series = u_by_inst[x0.instrument_rows[r]];
    count[r] = series.size();
    full = std::max(full, count[r]);
    const atx::f64 var =
        detail::ewma_var(series, spec_hl) * detail::nw_inflation(series, spec_nw_lags);
    sigma_ts[static_cast<Eigen::Index>(r)] =
        std::sqrt((var > kSpecificRiskFloor) ? var : kSpecificRiskFloor);
  }
  // `window` caps a name's history; the deepest observed count is `full` (<= window).
  // Use `full` (not `window`) as the γ denominator so a dense window that lost a few
  // under-determined dates still maps the deepest names to γ=1. Guard full == 0.
  const atx::f64 inv_full = (full == 0U) ? 0.0 : 1.0 / static_cast<atx::f64>(full);
  (void)window; // window is the documented upper bound on `full`; `full` is the live denom.

  // Structural ln-vol-on-exposures model over the FULL-clean-history names. Skipped
  // (γ_n ≡ 1) when too few clean names to fit (< K+1) or the OLS is rank-deficient.
  std::vector<atx::usize> clean; // current-cross-section rows with full clean history
  clean.reserve(m);
  for (atx::usize r = 0U; r < m; ++r) {
    if (full > 0U && count[r] == full) {
      clean.push_back(r);
    }
  }
  atx::core::linalg::VecX sigma_str = sigma_ts; // default: structural == TS (γ≡1 path)
  bool have_structural = false;
  if (structural && clean.size() > k && k > 0U) {
    atx::core::linalg::MatX xc(static_cast<Eigen::Index>(clean.size()),
                               static_cast<Eigen::Index>(k));
    atx::core::linalg::VecX yc(static_cast<Eigen::Index>(clean.size()));
    for (atx::usize a = 0U; a < clean.size(); ++a) {
      xc.row(static_cast<Eigen::Index>(a)) = x0.x.row(static_cast<Eigen::Index>(clean[a]));
      yc[static_cast<Eigen::Index>(a)] =
          std::log(sigma_ts[static_cast<Eigen::Index>(clean[a])]); // ln σ^TS (floored ⇒ finite)
    }
    const auto fit = atx::core::linalg::ols(xc, yc);
    if (fit) {
      // Predict ln σ^STR for ALL names from their exposure row; σ^STR = exp(·).
      const atx::core::linalg::VecX pred = x0.x * fit->beta;
      for (atx::usize r = 0U; r < m; ++r) {
        sigma_str[static_cast<Eigen::Index>(r)] = std::exp(pred[static_cast<Eigen::Index>(r)]);
      }
      have_structural = true;
    }
  }

  // Blend σ_n = γ_n·σ^TS + (1−γ_n)·σ^STR; D_n = σ_n² (floored > 0). When the
  // structural model was skipped, γ_n ≡ 1 ⇒ σ_n == σ^TS for every name.
  atx::core::linalg::VecX variances(static_cast<Eigen::Index>(m));
  for (atx::usize r = 0U; r < m; ++r) {
    const atx::f64 gamma =
        have_structural ? std::clamp(static_cast<atx::f64>(count[r]) * inv_full, 0.0, 1.0) : 1.0;
    const atx::f64 sigma = gamma * sigma_ts[static_cast<Eigen::Index>(r)] +
                           (1.0 - gamma) * sigma_str[static_cast<Eigen::Index>(r)];
    const atx::f64 d = sigma * sigma;
    variances[static_cast<Eigen::Index>(r)] = (d > kSpecificRiskFloor) ? d : kSpecificRiskFloor;
  }

  return SpecificRisk{std::move(variances), {}}; // isc EMPTY (interface-only)
}

} // namespace atx::engine::risk
