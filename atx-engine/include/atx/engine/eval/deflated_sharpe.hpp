#pragma once

// atx::engine::eval — Deflated Sharpe Ratio (DSR), Probabilistic Sharpe Ratio
// (PSR), expected-maximum Sharpe under selection, and the haircut Sharpe.
//
// ===========================================================================
//  What this header is
// ===========================================================================
//  The selection-bias correction for a backtest's per-period Sharpe ratio,
//  following Bailey & López de Prado (2014), "The Deflated Sharpe Ratio:
//  Correcting for Selection Bias, Backtest Overfitting and Non-Normality",
//  Journal of Portfolio Management 40(5), §3.3. A research process that tries
//  N strategy configurations and keeps the best inflates the observed Sharpe;
//  the DSR is the probability that the TRUE Sharpe exceeds the expected MAXIMUM
//  Sharpe that N independent random trials would have produced by chance.
//
//  Three layers (each a pure, allocation-free free function):
//   * probabilistic_sharpe(SR, SR*, T, γ3, κ) — PSR: P(true SR > SR* | data),
//     a non-normality-aware (skew/kurtosis-adjusted) one-sided confidence.
//   * expected_max_sharpe(N, V) — SR*_N: the expected maximum of N i.i.d.
//     Sharpe estimates each with variance V (the selection benchmark).
//   * deflated_sharpe(...) — DSR = PSR(SR*_N), plus the haircut Sharpe.
//
//  Reuses norm_cdf / norm_ppf from stats_ext.hpp (Acklam + one Halley step) —
//  this header introduces no new distribution code. All Sharpes here are
//  PER-PERIOD (NON-annualized); the caller annualizes downstream if desired.
//
// ===========================================================================
//  Numeric conventions (load-bearing)
// ===========================================================================
//  * γ3 is the (population) skewness, κ is the EXCESS kurtosis (raw = κ+3) —
//    matching stats_ext.hpp::skewness / excess_kurtosis.
//  * No RNG. No allocation. Every function is pure (a function of its inputs).
//  * The variance-of-Sharpe estimator V̂ = (1/T)·(1 − γ3·SR + ((κ+2)/4)·SR²)
//    is the single-stream form (Mertens / Bailey-LdP eq. for σ̂(SR)²); a caller
//    that measured the cross-trial variance of Sharpe estimates passes it via
//    the `var` argument instead.
//  * N == 1 means "no selection": SR*_1 = 0, so DSR collapses to PSR(0).

#include <algorithm> // std::max
#include <cmath>     // std::sqrt, std::exp
#include <cstddef>   // (atx::usize underlying)
#include <optional>  // std::optional (caller-supplied cross-trial variance)

#include "atx/core/types.hpp"           // atx::f64, atx::usize
#include "atx/engine/eval/stats_ext.hpp" // norm_cdf, norm_ppf

namespace atx::engine::eval {

// ===========================================================================
//  Named constants (no magic numbers).
// ===========================================================================
// Euler–Mascheroni constant γₑ — the extreme-value weight in the expected
// maximum of N standard normals (Bailey-LdP §3.3, eq. for E[max]).
inline constexpr atx::f64 kEulerMascheroni = 0.5772156649015329;

// ===========================================================================
//  DsrResult — the four scalars produced by deflated_sharpe.
//
//  Trivial aggregate (Rule of Zero); owns nothing. Aggregate-initialized in
//  declaration order.
// ===========================================================================
struct DsrResult {
  atx::f64 psr;            // PSR against the selection benchmark SR*_N (== dsr)
  atx::f64 sr_star;        // SR*_N: expected-maximum Sharpe across N trials
  atx::f64 dsr;            // Deflated Sharpe Ratio = PSR(SR*_N)
  atx::f64 haircut_sharpe; // max(0, SR − SR*_N): selection-adjusted Sharpe
};

// ===========================================================================
//  probabilistic_sharpe — PSR(SR*): P(true SR > SR* | observed SR, moments).
//
//  PSR = Φ( (SR − SR*)·√(T−1) / √(1 − γ3·SR + ((κ+2)/4)·SR²) )   (Bailey-LdP).
//  The denominator is the per-observation standard error of the Sharpe estimate
//  scaled out (the √T cancels against the √(T−1) numerator factor up to the
//  finite-sample (T−1) correction). When SR == SR* the argument is 0 ⇒ PSR =
//  0.5 (Φ(0)). T < 2 makes √(T−1) degenerate; the caller (DSR over a real
//  backtest) always has T ≥ 2, so no runtime guard is emitted on this pure path.
// ===========================================================================
[[nodiscard]] inline atx::f64 probabilistic_sharpe(atx::f64 sr, atx::f64 sr_star, atx::usize T,
                                                   atx::f64 skew, atx::f64 exkurt) noexcept {
  // Variance scaling term 1 − γ3·SR + ((κ+2)/4)·SR² (the per-T variance of the
  // Sharpe estimate, times T). Strictly positive for the SR/moment ranges DSR
  // ever sees; std::sqrt of a non-negative argument is well-defined.
  const atx::f64 var_term = 1.0 - skew * sr + ((exkurt + 2.0) / 4.0) * sr * sr;
  const atx::f64 tf = static_cast<atx::f64>(T);
  const atx::f64 z = (sr - sr_star) * std::sqrt(tf - 1.0) / std::sqrt(var_term);
  return norm_cdf(z);
}

// ===========================================================================
//  expected_max_sharpe — SR*_N: expected MAXIMUM Sharpe across N i.i.d. trials.
//
//  SR*_N = √V · [ (1 − γₑ)·Φ⁻¹(1 − 1/N) + γₑ·Φ⁻¹(1 − 1/(N·e)) ]   (Bailey-LdP,
//  the Gumbel-limit approximation to E[max] of N standard normals, rescaled by
//  the per-trial Sharpe standard deviation √V). N == 1 is "no selection" ⇒ 0
//  (and Φ⁻¹(0) would otherwise diverge, so this is also the well-defined guard).
//  `var` is V (the variance of the Sharpe estimates across trials); V ≥ 0.
// ===========================================================================
[[nodiscard]] inline atx::f64 expected_max_sharpe(atx::usize N, atx::f64 var) noexcept {
  if (N <= 1U) {
    return 0.0; // no selection across a single trial: benchmark is 0
  }
  const atx::f64 nf = static_cast<atx::f64>(N);
  const atx::f64 e = std::exp(1.0);
  // The two extreme-value quantiles, blended by the Euler–Mascheroni weight.
  const atx::f64 q_hi = norm_ppf(1.0 - 1.0 / nf);       // Φ⁻¹(1 − 1/N)
  const atx::f64 q_lo = norm_ppf(1.0 - 1.0 / (nf * e)); // Φ⁻¹(1 − 1/(N·e))
  const atx::f64 max_z = (1.0 - kEulerMascheroni) * q_hi + kEulerMascheroni * q_lo;
  return std::sqrt(var) * max_z;
}

// ===========================================================================
//  deflated_sharpe — DSR = PSR(SR*_N), with the haircut Sharpe.
//
//  When `var` is std::nullopt, the single-stream variance-of-Sharpe estimator
//  V̂ = (1/T)·(1 − γ3·SR + ((κ+2)/4)·SR²) is used for the expected-maximum
//  benchmark; otherwise the caller-supplied cross-trial variance V is used.
//  The PSR is then evaluated AGAINST that benchmark SR*_N. For N == 1, SR*_N = 0
//  so DSR == PSR(0). haircut_sharpe = max(0, SR − SR*_N) — the selection-
//  adjusted (deflated) point estimate of the Sharpe (never negative).
// ===========================================================================
[[nodiscard]] inline DsrResult deflated_sharpe(atx::f64 sr, atx::usize T, atx::f64 skew,
                                               atx::f64 exkurt, atx::usize N,
                                               std::optional<atx::f64> var) noexcept {
  // V̂: single-stream variance-of-Sharpe estimator (used when var absent). Same
  // var_term as PSR, divided by T.
  const atx::f64 var_term = 1.0 - skew * sr + ((exkurt + 2.0) / 4.0) * sr * sr;
  const atx::f64 v = var.has_value() ? *var : var_term / static_cast<atx::f64>(T);

  const atx::f64 sr_star = expected_max_sharpe(N, v);
  const atx::f64 psr = probabilistic_sharpe(sr, sr_star, T, skew, exkurt);
  const atx::f64 haircut = std::max(0.0, sr - sr_star);
  return DsrResult{psr, sr_star, psr, haircut};
}

} // namespace atx::engine::eval
