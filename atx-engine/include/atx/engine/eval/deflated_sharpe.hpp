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
#include <limits>    // std::numeric_limits (quiet_NaN for degenerate inputs)
#include <optional>  // std::optional (caller-supplied cross-trial variance)
#include <span>      // std::span (net-cost helper inputs)
#include <vector>    // std::vector (net-series scratch in deflated_sharpe_net_cost)

#include "atx/core/types.hpp"            // atx::f64, atx::usize
#include "atx/engine/eval/stats_ext.hpp" // norm_cdf, norm_ppf, mean_std_pop, skewness, excess_kurtosis

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
//  0.5 (Φ(0)). Degenerate inputs (T < 2, or a pathological skew/kurtosis that
//  drives the variance term ≤ 0) return NaN — this is a public pure function, so
//  the guards are explicit rather than relying on a caller precondition.
// ===========================================================================
[[nodiscard]] inline atx::f64 probabilistic_sharpe(atx::f64 sr, atx::f64 sr_star, atx::usize T,
                                                   atx::f64 skew, atx::f64 exkurt) noexcept {
  // SAFETY: √(T−1) is NaN for T < 2 (and the (T−1) finite-sample correction is
  //         undefined for fewer than two observations). Return NaN explicitly
  //         rather than propagate a silent NaN through the divide below.
  if (T < 2U) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
  // Variance scaling term 1 − γ3·SR + ((κ+2)/4)·SR² (the per-T variance of the
  // Sharpe estimate, times T). Strictly positive for the SR/moment ranges DSR
  // ever sees; std::sqrt of a non-negative argument is well-defined.
  const atx::f64 var_term = 1.0 - skew * sr + ((exkurt + 2.0) / 4.0) * sr * sr;
  // SAFETY: the documented domain has var_term > 0; a pathological skew/kurtosis
  //         can drive it ≤ 0, where √var_term is NaN (or a 0 denominator gives
  //         ±inf). The !(>0) form also rejects a NaN var_term. Fail to NaN.
  if (!(var_term > 0.0)) {
    return std::numeric_limits<atx::f64>::quiet_NaN();
  }
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

// ===========================================================================
//  deflated_sharpe_net_cost — DSR on a net-of-cost return series (S4-4).
//
//  Given a per-period gross PnL series (with pnl[0] == 0 structural zero,
//  matching metrics.hpp §0-F), a matching per-period turnover series, and a
//  round-trip cost in basis points, computes:
//    r_net[t] = pnl[t] - turnover[t] * rt_cost_bps / 1e4
//  then derives net moments over r_net[1..T) (the §0-F exclusion, consistent
//  with metrics.hpp and compute_metrics), forms:
//    SR_net = mean_net / std_net   (per-period, non-annualized)
//  and delegates to the EXISTING deflated_sharpe(SR_net, T, skew_net,
//  exkurt_net, N, var). T = number of observations = pnl.size() - 1
//  (the r_net[1..end] count), matching how the existing tests construct T.
//
//  Moment conventions: population (biased) std, population skewness and
//  excess kurtosis, via stats_ext.hpp helpers — consistent with metrics.hpp
//  and deflated_sharpe.hpp's numeric conventions (see §30 of stats_ext.hpp).
//
//  SCRATCH ALLOCATION: one std::vector<f64> of length pnl.size()-1 is
//  allocated to hold r_net[1..T). This is an analysis helper (not a VM hot
//  path), so a single vector scratch is acceptable; it is documented here.
//
//  PRECONDITION: pnl.size() == turnover.size(). A RELEASE build (NDEBUG) must
//  never read turnover[t] out of bounds, so this guard is ALWAYS-ON (not a
//  debug-only assert, which would both compile out under NDEBUG — leaving the OOB
//  read — and abort a debug-build test that exercises the mismatch path). A size
//  mismatch, or any degenerate input yielding < 1 observation, fails SAFE to a
//  NaN DsrResult, matching this file's "degenerate inputs return NaN" pattern
//  (cf. probabilistic_sharpe T<2 and deflated_sharpe). No UB ever, in any build.
// ===========================================================================
[[nodiscard]] inline DsrResult deflated_sharpe_net_cost(std::span<const atx::f64> pnl,
                                                        std::span<const atx::f64> turnover,
                                                        atx::f64 rt_cost_bps, atx::usize N,
                                                        std::optional<atx::f64> var) noexcept {
  // SAFETY (always-on, both debug and release): a size mismatch would read
  // turnover[t] out of bounds; fewer than 1 return observation (pnl.size() < 2,
  // only the structural zero) is degenerate. Fail safe to a NaN DsrResult rather
  // than risk OOB or feed a 0-observation moment set into deflated_sharpe.
  if (pnl.size() != turnover.size() || pnl.size() < 2U) {
    const atx::f64 nan = std::numeric_limits<atx::f64>::quiet_NaN();
    return DsrResult{nan, nan, nan, nan};
  }

  // Build r_net over r_net[1..T) (the §0-F structural-zero exclusion):
  // index 0 is a structural zero in pnl; skipping it here mirrors the moment
  // exclusion in compute_metrics and deflated_sharpe's existing convention.
  // SCRATCH: single vector allocation, acceptable for an analysis helper.
  const atx::usize T_obs = pnl.size() - 1U; // >= 1 (guarded above)
  std::vector<atx::f64> r_net;
  r_net.reserve(T_obs);
  const atx::f64 cost_per_unit = rt_cost_bps / 1.0e4;
  for (atx::usize t = 1U; t < pnl.size(); ++t) {
    r_net.push_back(pnl[t] - turnover[t] * cost_per_unit);
  }

  // Moments over r_net (population convention, matching metrics.hpp + stats_ext.hpp).
  const MeanStd ms = mean_std_pop(r_net);
  // SR_net = per-period (non-annualized), matching deflated_sharpe's convention.
  // Flat (std == 0) -> SR = 0, same policy as compute_metrics (not NaN/inf).
  const atx::f64 sr_net = (ms.std > 0.0) ? ms.mean / ms.std : 0.0;
  // Population skewness and excess kurtosis via stats_ext.hpp helpers.
  const atx::f64 skew_net = skewness(r_net);
  const atx::f64 exkurt_net = excess_kurtosis(r_net);

  // T passed to deflated_sharpe = number of observations = r_net.size() = T_obs.
  return deflated_sharpe(sr_net, T_obs, skew_net, exkurt_net, N, var);
}

} // namespace atx::engine::eval
