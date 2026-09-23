#pragma once

// atx::engine::eval — Harvey & Liu (2015) multiple-testing Sharpe haircut.
//
// "Backtesting", Journal of Portfolio Management 42(1). A backtested Sharpe is
// a t-statistic in disguise: t = SR·√T (per-period SR, T periods). Its single-
// test two-sided p-value p_S = 2·(1 − Φ(t)) ignores the other N − 1 strategies
// tried. The multiple-testing p-value p_M adjusts for them:
//    Bonferroni : p_M = min(1, N·p_S)
//    Sidak      : p_M = 1 − (1 − p_S)^N     (exact FWER under independence)
// and inverting p_M gives the haircut Sharpe
//    SR_adj = Φ⁻¹(1 − p_M/2) / √T,   haircut = 1 − SR_adj / SR.
//
// N is REAL-valued so the registry's N_eff (effective independent trials) can
// be fed straight in; the TrialSummary overload does exactly that. HL's
// Holm/BHY variants need the full cross-section of p-values — use
// multiple_testing.hpp for those. Autocorrelation adjustment of SR (Lo 2002) is
// the caller's job before calling. Pure; no allocation.

#include <algorithm> // std::max, std::min
#include <cmath>     // std::sqrt, std::pow, std::log1p, std::expm1

#include "atx/core/types.hpp"                 // atx::f64, atx::u8, atx::usize
#include "atx/engine/eval/stats_ext.hpp"      // norm_cdf, norm_ppf
#include "atx/engine/eval/trial_registry.hpp" // TrialSummary

namespace atx::engine::eval {

enum class HaircutMethod : atx::u8 { Bonferroni = 0, Sidak = 1 };

struct HaircutResult {
  atx::f64 p_single{};    // two-sided single-test p-value of t = SR·√T
  atx::f64 p_multiple{};  // multiple-testing adjusted p-value
  atx::f64 sr_adjusted{}; // haircut (per-period) Sharpe, >= 0 for SR > 0
  atx::f64 haircut{};     // 1 − SR_adj/SR in [0, 1]; 0 when SR <= 0
};

// `n_tests` < 1 is treated as 1 (no multiplicity). SR <= 0: nothing to haircut
// (sr_adjusted = SR, haircut = 0). T == 0 yields p_single = 1 and a full cut.
[[nodiscard]] inline HaircutResult haircut_sharpe(atx::f64 sr, atx::usize T, atx::f64 n_tests,
                                                  HaircutMethod method) noexcept {
  const atx::f64 n = n_tests > 1.0 ? n_tests : 1.0;
  const atx::f64 sqrt_t = std::sqrt(static_cast<atx::f64>(T));
  const atx::f64 t = sr * sqrt_t;
  // Upper tail via Φ(−|t|) keeps precision for large t (1 − Φ(t) would cancel).
  const atx::f64 p_s = std::min(1.0, 2.0 * norm_cdf(-std::abs(t)));
  atx::f64 p_m = 1.0;
  switch (method) {
  case HaircutMethod::Bonferroni:
    p_m = std::min(1.0, n * p_s);
    break;
  case HaircutMethod::Sidak:
    // 1 − (1 − p)^N computed as −expm1(N·log1p(−p)) for small-p accuracy.
    p_m = p_s >= 1.0 ? 1.0 : std::min(1.0, -std::expm1(n * std::log1p(-p_s)));
    break;
  }
  HaircutResult r{p_s, p_m, sr, 0.0};
  if (!(sr > 0.0)) {
    return r;
  }
  if (p_m >= 1.0 || T == 0U) {
    r.sr_adjusted = 0.0;
    r.haircut = 1.0;
    return r;
  }
  const atx::f64 t_adj = norm_ppf(1.0 - p_m / 2.0);
  r.sr_adjusted = std::max(0.0, std::min(sr, t_adj / sqrt_t));
  r.haircut = 1.0 - r.sr_adjusted / sr;
  return r;
}

// Registry-fed: N = trials.n_eff (effective independent trials).
[[nodiscard]] inline HaircutResult haircut_sharpe(atx::f64 sr, atx::usize T,
                                                  const TrialSummary &trials,
                                                  HaircutMethod method) noexcept {
  return haircut_sharpe(sr, T, trials.n_eff, method);
}

} // namespace atx::engine::eval
