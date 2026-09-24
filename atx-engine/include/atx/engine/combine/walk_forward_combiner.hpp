#pragma once

// atx::engine::combine — point-in-time walk-forward weight path (Lane 5).
//
// ===========================================================================
//  PIT contract (load-bearing)
// ===========================================================================
//  fwd(t, ·) in a SignalStore is the h-period forward return from t, REALIZED at
//  t + h. At decision date d (after the close of d) a fit may therefore only use rows
//  t with t + h <= d, i.e. the window [fit_begin, d − h + 1). walk_forward() enforces
//  exactly that embargo; the weights stamped on date d are then applied to the
//  signals of date d (combine_forecast). The PIT test mutates every signal row > d and
//  every forward-return row > d − h and asserts the path up to d is byte-identical.
//
//  Cadence / hysteresis:
//    * a refit is attempted on the first eligible date and then every refit_every
//      dates;
//    * a refit's weights are ADOPTED only if ‖ŵ_new − ŵ_cur‖₁ > hysteresis, where
//      ŵ = w/Σ|w| is the gross-normalized copy (so the threshold is in the same
//      units, [0, 2], for every combiner — FamaMacBethRidge returns raw betas while
//      the others return gross-1 weights). The first successful fit is always
//      adopted; this suppresses turnover from re-estimation noise. The adopted
//      weights themselves are stored un-normalized;
//    * a fit that returns Err keeps the current weights (counted in failed_fits).
//  Dates before the first adoption carry NaN weights (no forecast).

#include <cmath>   // std::abs, std::isfinite
#include <span>    // std::span
#include <utility> // std::move
#include <vector>  // std::vector

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/combine/signal_combiner.hpp" // Combiner, CombineWeights
#include "atx/engine/combine/signal_store.hpp"    // SignalStore, FitWindow, combine_forecast

namespace atx::engine::combine {

struct WalkForwardCfg {
  atx::usize horizon = 1U;     // forward-return horizon h (embargo), >= 1
  atx::usize min_train = 20U;  // minimum fit rows before a fit is attempted, >= 1
  atx::usize lookback = 0U;    // 0 → expanding window; else rolling window of this many rows
  atx::usize refit_every = 1U; // refit cadence in dates, >= 1
  atx::f64 hysteresis = 0.0;   // adopt only when ‖ŵ_new − ŵ_cur‖₁ > hysteresis (ŵ = w/Σ|w|)
};

struct WeightPath {
  atx::usize n_dates = 0U;
  atx::usize n_alphas = 0U;
  std::vector<atx::f64> w;                // row-major n_dates × n_alphas; NaN = no weights
  std::vector<atx::usize> refit_dates;    // dates a fit was attempted
  std::vector<atx::usize> adopted_dates;  // dates whose fit replaced the weights
  atx::usize failed_fits = 0U;

  [[nodiscard]] std::span<const atx::f64> at(atx::usize t) const noexcept {
    return {w.data() + t * n_alphas, n_alphas};
  }
  [[nodiscard]] bool has_weights(atx::usize t) const noexcept {
    return n_alphas > 0U && std::isfinite(w[t * n_alphas]);
  }
};

// Build the PIT weight path over every date of `s`. Err on an invalid cfg or an
// empty store.
template <Combiner C>
[[nodiscard]] atx::core::Result<WeightPath> walk_forward(const SignalStore &s,
                                                         const WalkForwardCfg &cfg, const C &c) {
  if (cfg.horizon == 0U || cfg.min_train == 0U || cfg.refit_every == 0U || !(cfg.hysteresis >= 0.0)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "walk_forward: horizon, min_train, refit_every must be >= 1");
  }
  if (s.n_alphas() == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "walk_forward: empty store");
  }
  WeightPath p;
  p.n_dates = s.n_dates();
  p.n_alphas = s.n_alphas();
  p.w.assign(p.n_dates * p.n_alphas, kSignalNaN);
  std::vector<atx::f64> cur;
  bool first_attempt_seen = false;
  atx::usize first_attempt = 0U;
  for (atx::usize d = 0U; d < p.n_dates; ++d) {
    // Embargo: rows t with t + h <= d → fit_end = d − h + 1.
    const atx::usize fit_end = (d + 1U >= cfg.horizon) ? d + 1U - cfg.horizon : 0U;
    const atx::usize fit_begin =
        (cfg.lookback > 0U && fit_end > cfg.lookback) ? fit_end - cfg.lookback : 0U;
    const bool eligible = fit_end >= fit_begin + cfg.min_train;
    if (eligible && !first_attempt_seen) {
      first_attempt_seen = true;
      first_attempt = d;
    }
    if (eligible && (d - first_attempt) % cfg.refit_every == 0U) {
      p.refit_dates.push_back(d);
      const auto r = c.fit(s, FitWindow{fit_begin, fit_end});
      if (!r.has_value() || r->w.size() != p.n_alphas) {
        ++p.failed_fits;
      } else {
        atx::f64 dist = 0.0;
        if (!cur.empty()) {
          atx::f64 g_new = 0.0;
          atx::f64 g_cur = 0.0;
          for (atx::usize a = 0U; a < p.n_alphas; ++a) {
            g_new += std::abs(r->w[a]);
            g_cur += std::abs(cur[a]);
          }
          const atx::f64 in = (g_new > 0.0) ? 1.0 / g_new : 0.0;
          const atx::f64 ic = (g_cur > 0.0) ? 1.0 / g_cur : 0.0;
          for (atx::usize a = 0U; a < p.n_alphas; ++a) {
            dist += std::abs(r->w[a] * in - cur[a] * ic);
          }
        }
        if (cur.empty() || dist > cfg.hysteresis) {
          cur = r->w;
          p.adopted_dates.push_back(d);
        }
      }
    }
    if (!cur.empty()) {
      for (atx::usize a = 0U; a < p.n_alphas; ++a) {
        p.w[d * p.n_alphas + a] = cur[a];
      }
    }
  }
  return atx::core::Ok(std::move(p));
}

// Per-date realized PnL of the walk-forward forecast: pnl[d] = Σ_i c_i fwd(d,i) / Σ_i |c_i|
// with c = combine_forecast(path.at(d), d) over jointly-finite cells; NaN on dates
// with no weights, a zero forecast, or no finite forward returns. pnl[d] is realized
// at d + h (it is an evaluation series, never a fit input).
[[nodiscard]] inline std::vector<atx::f64> walk_forward_pnl(const SignalStore &s,
                                                           const WeightPath &path) {
  std::vector<atx::f64> out(s.n_dates(), kSignalNaN);
  std::vector<atx::f64> c(s.n_instruments());
  for (atx::usize d = 0U; d < s.n_dates() && d < path.n_dates; ++d) {
    if (!path.has_weights(d)) {
      continue;
    }
    combine_forecast(s, path.at(d), d, c);
    const auto r = s.fwd_row(d);
    atx::f64 gross = 0.0;
    atx::f64 pnl = 0.0;
    for (atx::usize i = 0U; i < c.size(); ++i) {
      if (std::isfinite(r[i])) {
        gross += std::abs(c[i]);
        pnl += c[i] * r[i];
      }
    }
    if (gross > 0.0) {
      out[d] = pnl / gross;
    }
  }
  return out;
}

} // namespace atx::engine::combine
