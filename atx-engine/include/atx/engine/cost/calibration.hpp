#pragma once

// atx::engine::cost — Cost-coefficient calibration (Sprint S6-1).
//
// ===========================================================================
//  What this header does
// ===========================================================================
//  Recovers the impact coefficients (Y, δ, γ) of the ONE cost model
//  (exec::ImpactCfg) from realized cost observations and emits a calibrated
//  ImpactCfg plus an auditable FitReport (R², stderr, residual quantile, valid
//  row count). It introduces NO second cost formula — it inverts the exact
//  structural laws the execution simulator applies:
//
//      temp = Y · σ · p^δ        (temporary impact, folded into the fill price)
//      perm = 0.5 · γ · σ · p    (permanent impact, shifts the mark; α=1 imposed)
//
//  with p = participation = |fill_qty| / ADV.
//
// ===========================================================================
//  The two fits
// ===========================================================================
//  TEMP channel (Y, δ) — log-linearize the power law:
//      log(temp / σ) = log(Y) + δ · log(p)
//    A robust IRLS-Huber fit (cost/robust_ls.hpp) over design x=[1, log(p)]
//    against y=log(temp/σ). Heavy-tailed fill noise would let an OLS fit chase
//    outliers in the exponent; Huber bounds each residual's leverage (C7).
//    Y = exp(beta0); δ = clamp(beta1, 0.3, 0.9) (the admissible √-impact range).
//
//  PERM channel (γ) — α=1 is imposed (the no-manipulation / linear-through-
//    structure constraint), so perm = 0.5·γ·(σ·p) is a one-regressor
//    through-the-origin line: slope m = Σ(σp·perm)/Σ(σp)², γ = 2·m. When NO perm
//    is observed (the single-snapshot calibrate_impact path), γ stays at the
//    prior and r2_perm is 0.
//
// ===========================================================================
//  The δ-stderr / Y-stderr derivation (honest fit quality, C1)
// ===========================================================================
//  Treat the converged IRLS fit as an OLS fit of the temp design. With p=2
//  parameters and n valid rows:
//      σ̂² = RSS / (n − p),   RSS = Σ resid²
//      Cov(beta) = σ̂² · (XᵀX)⁻¹
//      delta_stderr = sqrt(Cov(1,1))                      (δ is beta1 directly)
//      Y = exp(beta0)  ⇒  by the delta method (dY/dbeta0 = Y):
//          Y_stderr = |Y| · sqrt(Cov(0,0))
//  Both are strictly > 0 for a real (n>p, non-degenerate) fit.
//
// ===========================================================================
//  Determinism + NaN policy (C5 / M8)
// ===========================================================================
//  * No RNG. The obs-builder, the row guards, the sort for resid_p95 and the
//    IRLS loop are all order-fixed and run-to-run byte-identical.
//  * A row is DROPPED from the temp design iff participation<=0 || sigma<=0 ||
//    temp<=0 (the log-domain guard — log of a non-positive number is undefined).
//    Degenerate rows never enter the design, so they cannot poison it with
//    NaN/Inf. n_fills counts the VALID temp rows.
//  * Empty/all-degenerate design (0 valid temp rows) ⇒ the prior is returned
//    unchanged with a zeroed report (the AllDegenerate fallback).
//  * C2 firewall: calibrate_impact admits only fills with t < trailing_window,
//    so appending strictly-later fills cannot move a closed window's fit.

#include <algorithm> // std::sort, std::clamp
#include <cmath>     // std::log, std::exp, std::abs, std::sqrt, std::llabs
#include <span>      // std::span (observation / fill views)
#include <utility>   // std::pair (stderr return), structured bindings
#include <vector>    // std::vector (design build, residual scratch)

#include "atx/core/types.hpp"         // f64, i64, usize
#include "atx/core/linalg/linalg.hpp" // MatX, VecX

#include "atx/engine/cost/robust_ls.hpp"     // RobustCfg, RobustFit, irls_huber
#include "atx/engine/exec/execution_sim.hpp" // ImpactCfg, SlippageCfg
#include "atx/engine/exec/payloads.hpp"      // FillPayload
#include "atx/engine/loop/market.hpp"        // Market, InstrumentStats
#include "atx/engine/loop/types.hpp"         // InstrumentId

namespace atx::engine::cost {

using atx::f64;
using atx::i64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;

// ---------------------------------------------------------------------------
//  Observation + outputs
// ---------------------------------------------------------------------------

/// One realized cost observation. `participation` = |qty|/ADV; `sigma` is the
/// instrument's per-bar volatility; `temp`/`perm` are the realized temporary and
/// permanent impact magnitudes. `perm == 0` ⇒ permanent impact unobserved (γ
/// stays at the prior). Positional aggregate-init order is {p, σ, temp, perm}.
struct CostObs {
  f64 participation;
  f64 sigma;
  f64 temp;
  f64 perm;
};

/// Auditable fit diagnostics emitted alongside a CalibratedCost (C1).
struct FitReport {
  f64 r2_temp{0.0};      ///< R² of the temp-channel (Y, δ) fit.
  f64 r2_perm{0.0};      ///< R² of the perm-channel (γ) fit; 0 if γ not fit.
  f64 delta_stderr{0.0}; ///< standard error of δ (> 0 on a real fit).
  f64 Y_stderr{0.0};     ///< standard error of Y, via the delta method.
  usize n_fills{0};      ///< count of VALID (non-dropped) temp rows in the design.
  f64 resid_p95{0.0};    ///< 95th-percentile |residual| of the temp fit.
};

/// Calibrated impact/slippage coefficients + the fit provenance.
struct CalibratedCost {
  exec::ImpactCfg impact;
  exec::SlippageCfg slippage;
  FitReport report;
};

namespace detail {

// δ is constrained to the admissible √-impact range (Appendix A: 0.45–0.65,
// widened to the structural 0.3–0.9 bound the simulator accepts).
inline constexpr f64 kDeltaLo = 0.3;
inline constexpr f64 kDeltaHi = 0.9;

/// 95th-percentile |value| over a vector, by the deterministic nearest-rank
/// rule: sort ascending, take index floor(0.95·(n−1)). Empty ⇒ 0.
[[nodiscard]] inline f64 abs_p95(std::vector<f64> v) {
  if (v.empty()) {
    return 0.0;
  }
  for (f64& x : v) {
    x = std::abs(x);
  }
  std::sort(v.begin(), v.end());
  const usize idx = static_cast<usize>(0.95 * static_cast<f64>(v.size() - 1));
  return v[idx];
}

/// Standard errors of (Y, δ) from the temp design and its converged residuals,
/// treating the fit as OLS: σ̂² = RSS/(n−p); Cov = σ̂²·(XᵀX)⁻¹; delta_stderr =
/// sqrt(Cov(1,1)); Y_stderr = |Y|·sqrt(Cov(0,0)) (delta method on Y=exp(beta0)).
/// Returns (Y_stderr, delta_stderr); zeros when n <= p (no residual d.o.f.).
[[nodiscard]] inline std::pair<f64, f64>
temp_stderrs(const MatX& X, const std::vector<f64>& resid, f64 Y) {
  const usize n = static_cast<usize>(X.rows());
  constexpr usize p = 2;
  if (n <= p) {
    return {0.0, 0.0};
  }
  f64 rss = 0.0;
  for (const f64 r : resid) {
    rss += r * r;
  }
  const f64 sigma2 = rss / static_cast<f64>(n - p);
  const MatX cov = sigma2 * (X.transpose() * X).inverse();
  const f64 delta_se = std::sqrt(cov(1, 1));
  const f64 Y_se = std::abs(Y) * std::sqrt(cov(0, 0));
  return {Y_se, delta_se};
}

/// Through-the-origin slope of `perm` on the single regressor σ·p, and its R².
/// m = Σ(σp·perm)/Σ(σp)²; γ = 2·m. Only rows with perm>0 && σ>0 && p>0 count.
/// Returns (gamma, r2_perm, n_perm_rows); n_perm_rows == 0 ⇒ γ unobserved.
struct PermFit {
  f64 gamma{0.0};
  f64 r2{0.0};
  usize n{0};
};
[[nodiscard]] inline PermFit fit_perm(std::span<const CostObs> obs) {
  f64 sxx = 0.0; // Σ (σp)²
  f64 sxy = 0.0; // Σ (σp · perm)
  usize n = 0;
  for (const CostObs& o : obs) {
    if (o.perm > 0.0 && o.sigma > 0.0 && o.participation > 0.0) {
      const f64 x = o.sigma * o.participation;
      sxx += x * x;
      sxy += x * o.perm;
      ++n;
    }
  }
  if (n == 0 || sxx <= 0.0) {
    return PermFit{};
  }
  const f64 m = sxy / sxx;
  // R² of the through-origin fit: 1 − Σ(perm − m·x)² / Σ perm² (no intercept ⇒
  // total sum of squares is about 0, not the mean, matching the model form).
  f64 ss_res = 0.0;
  f64 ss_tot = 0.0;
  for (const CostObs& o : obs) {
    if (o.perm > 0.0 && o.sigma > 0.0 && o.participation > 0.0) {
      const f64 x = o.sigma * o.participation;
      const f64 e = o.perm - m * x;
      ss_res += e * e;
      ss_tot += o.perm * o.perm;
    }
  }
  const f64 r2 = (ss_tot > 0.0) ? (1.0 - ss_res / ss_tot) : 0.0;
  return PermFit{2.0 * m, r2, n};
}

} // namespace detail

// ---------------------------------------------------------------------------
//  Core entry point — fit over explicit observations.
// ---------------------------------------------------------------------------

/// Calibrate (Y, δ, γ) from explicit cost observations, emitting a new
/// exec::ImpactCfg + an auditable FitReport. Drops log-undefined temp rows
/// (M8); on an empty/all-degenerate temp design returns `prior` unchanged with a
/// zeroed report. γ comes from the perm channel when observed, else from prior.
[[nodiscard]] inline CalibratedCost
calibrate_from_obs(std::span<const CostObs> obs, const exec::ImpactCfg& prior = exec::ImpactCfg{}) {
  // --- Build the temp design from VALID rows only (M8 log-domain guard). ---
  std::vector<f64> log_p; // x1 = log(participation)
  std::vector<f64> ly;    // y  = log(temp / σ)
  log_p.reserve(obs.size());
  ly.reserve(obs.size());
  for (const CostObs& o : obs) {
    if (o.participation > 0.0 && o.sigma > 0.0 && o.temp > 0.0) {
      log_p.push_back(std::log(o.participation));
      ly.push_back(std::log(o.temp / o.sigma));
    }
  }
  const usize n = log_p.size();

  // --- All-degenerate design: fall back to the prior, zeroed report. ---
  if (n < 2) {
    return CalibratedCost{prior, exec::SlippageCfg{}, FitReport{}};
  }

  // --- Robust log-linear fit of the power law (C7). ---
  MatX X(static_cast<Eigen::Index>(n), 2);
  VecX y(static_cast<Eigen::Index>(n));
  for (usize i = 0; i < n; ++i) {
    const auto r = static_cast<Eigen::Index>(i);
    X(r, 0) = 1.0;
    X(r, 1) = log_p[i];
    y(r) = ly[i];
  }
  const RobustFit fit = irls_huber(X, y, RobustCfg{});
  const f64 Y = std::exp(fit.beta[0]);
  const f64 delta = std::clamp(fit.beta[1], detail::kDeltaLo, detail::kDeltaHi);

  // --- Perm channel (γ): from the data when observed, else from prior. ---
  const detail::PermFit pf = detail::fit_perm(obs);
  const f64 gamma = (pf.n > 0) ? pf.gamma : prior.gamma;

  // --- Honest fit quality (C1). ---
  const auto [y_se, delta_se] = detail::temp_stderrs(X, fit.residuals, Y);
  FitReport report{};
  report.r2_temp = fit.r2;
  report.r2_perm = (pf.n > 0) ? pf.r2 : 0.0;
  report.delta_stderr = delta_se;
  report.Y_stderr = y_se;
  report.n_fills = n;
  report.resid_p95 = detail::abs_p95(fit.residuals);

  return CalibratedCost{exec::ImpactCfg{Y, delta, gamma}, exec::SlippageCfg{}, report};
}

// ---------------------------------------------------------------------------
//  Convenience entry point — build observations from a fill stream + Market.
// ---------------------------------------------------------------------------

/// Calibrate from realized fills against a point-in-time Market snapshot. Only
/// fills strictly INSIDE the trailing window (t.unix_nanos() < trailing_window)
/// enter the design — the C2 fit/apply firewall: appending strictly-later fills
/// cannot move a closed window's coefficients. participation = |qty|/ADV and σ
/// come from market.stats(id); temp = |fill.impact|; perm is unobservable from a
/// single snapshot (no realized mark-shift), so it is 0 and γ stays at the prior.
[[nodiscard]] inline CalibratedCost
calibrate_impact(std::span<const exec::FillPayload> fills, const atx::engine::Market& market,
                 usize trailing_window, const exec::ImpactCfg& prior = exec::ImpactCfg{}) {
  std::vector<CostObs> built;
  built.reserve(fills.size());
  for (const exec::FillPayload& f : fills) {
    if (f.t.unix_nanos() >= static_cast<i64>(trailing_window)) {
      continue; // strictly-later fill — excluded by the closed-window cutoff.
    }
    const InstrumentStats& s = market.stats(f.id);
    if (s.adv <= 0.0) {
      continue; // no ADV ⇒ participation is undefined; drop the row.
    }
    const f64 qty_abs = static_cast<f64>(std::llabs(f.qty));
    built.push_back(CostObs{qty_abs / s.adv, s.sigma, std::abs(f.impact), 0.0});
  }
  return calibrate_from_obs(std::span<const CostObs>{built}, prior);
}

} // namespace atx::engine::cost
