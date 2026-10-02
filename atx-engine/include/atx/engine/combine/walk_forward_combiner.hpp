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

#include <algorithm>
#include <limits>
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
  atx::u64 ic_cache_max_bytes = 256ULL * 1024ULL * 1024ULL; // 0 disables exact IC row reuse
};

struct WeightPath {
  atx::usize n_dates = 0U;
  atx::usize n_alphas = 0U;
  std::vector<atx::f64> w;                // row-major n_dates × n_alphas; NaN = no weights
  std::vector<atx::usize> refit_dates;    // dates a fit was attempted
  std::vector<atx::usize> adopted_dates;  // dates whose fit replaced the weights
  atx::usize failed_fits = 0U;
  atx::usize ic_rows_computed = 0U; // distinct rows, not rows revisited by refits
  bool used_ic_cache = false;

  [[nodiscard]] std::span<const atx::f64> at(atx::usize t) const noexcept {
    return {w.data() + t * n_alphas, n_alphas};
  }
  [[nodiscard]] bool has_weights(atx::usize t) const noexcept {
    return n_alphas > 0U && std::isfinite(w[t * n_alphas]);
  }
};

namespace walk_detail {
template <class Fit>
[[nodiscard]] atx::core::Result<WeightPath> run_path(
    atx::usize n_dates, atx::usize n_alphas, const WalkForwardCfg& cfg,
    atx::usize delay, atx::usize maturity_end, atx::usize data_begin,
    atx::usize data_end, Fit&& fit) {
  if (cfg.horizon == 0U || cfg.min_train == 0U || cfg.refit_every == 0U || !(cfg.hysteresis >= 0.0)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "walk_forward: horizon, min_train, refit_every must be >= 1");
  }
  if (n_alphas == 0U || n_dates == 0U ||
      n_dates > std::numeric_limits<atx::usize>::max() / n_alphas ||
      cfg.horizon > std::numeric_limits<atx::usize>::max() - delay) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "walk_forward: empty store");
  }
  WeightPath p;
  p.n_dates = n_dates;
  p.n_alphas = n_alphas;
  p.w.assign(p.n_dates * p.n_alphas, kSignalNaN);
  std::vector<atx::f64> cur;
  bool first_attempt_seen = false;
  atx::usize first_attempt = 0U;
  const atx::usize lag = cfg.horizon + delay;
  for (atx::usize d = 0U; d < p.n_dates; ++d) {
    // Embargo: rows t with t + h <= d → fit_end = d − h + 1.
    const atx::usize observed_end = std::min(d + 1U, maturity_end);
    const atx::usize fit_end = std::min(data_end, observed_end >= lag ? observed_end - lag : 0U);
    const atx::usize fit_begin =
        std::max(data_begin, (cfg.lookback > 0U && fit_end > cfg.lookback) ? fit_end - cfg.lookback : 0U);
    const bool eligible = fit_end >= fit_begin && fit_end - fit_begin >= cfg.min_train;
    if (eligible && !first_attempt_seen) {
      first_attempt_seen = true;
      first_attempt = d;
    }
    if (eligible && (d - first_attempt) % cfg.refit_every == 0U) {
      p.refit_dates.push_back(d);
      const auto r = fit(FitWindow{fit_begin, fit_end}, observed_end);
      if (!r.has_value() || r->w.size() != p.n_alphas ||
          !std::all_of(r->w.begin(), r->w.end(), [](atx::f64 value) { return std::isfinite(value); })) {
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
} // namespace walk_detail

template <class C>
concept CachedIcCombiner = requires(const C& c, const SignalIcView& s, FitWindow w) {
  { c.fit(s, w) } -> std::same_as<atx::core::Result<CombineWeights>>;
  { c.inference } -> std::same_as<const SignalInferenceConfig&>;
};

// Existing store callers use exact incremental IC rows when the declared horizon
// agrees with the combiner. Other combiners, old mismatched declarations and a
// cache exceeding the caller's budget retain the original store path.
// Each row is constructed only when its label matures. Unobserved future labels
// are never read to fill the cache; refits borrow the already immutable prefix.
template <Combiner C>
[[nodiscard]] atx::core::Result<WeightPath> walk_forward(
    const SignalStore& s, const WalkForwardCfg& cfg, const C& c) {
  std::vector<atx::f64> rows;
  std::vector<atx::f64> clipped;
  bool cached = false;
  if constexpr (CachedIcCombiner<C>) {
    const atx::usize capacity = s.n_dates() > cfg.horizon ? s.n_dates() - cfg.horizon : 0U;
    const bool treatment_ok = c.inference.return_treatment == IcReturnTreatment::RawV1 ||
                             c.inference.return_treatment == IcReturnTreatment::WinsorizedV2;
    if (cfg.horizon > 0U && cfg.horizon == c.inference.label_horizon && treatment_ok &&
        cfg.ic_cache_max_bytes > 0U && s.n_alphas() > 0U && capacity > 0U &&
        capacity <= std::numeric_limits<atx::usize>::max() / s.n_alphas()) {
      const atx::usize cells = capacity * s.n_alphas();
      const atx::usize scratch = c.inference.return_treatment == IcReturnTreatment::WinsorizedV2
                                    ? s.n_instruments() : 0U;
      if (cells <= cfg.ic_cache_max_bytes / sizeof(atx::f64) &&
          scratch <= (cfg.ic_cache_max_bytes - cells * sizeof(atx::f64)) / sizeof(atx::f64)) {
        rows.resize(cells, kSignalNaN);
        clipped.resize(scratch);
        cached = true;
      }
    }
  }
  atx::usize ready = 0U;
  const auto fit = [&](FitWindow w, atx::usize observed_end) {
    if constexpr (CachedIcCombiner<C>) {
      if (cached) {
        if (w.end > ready) {
          const FitWindow fresh{ready, w.end};
          auto output = std::span{rows}.subspan(ready * s.n_alphas(), fresh.size() * s.n_alphas());
          signal_detail::ic_rows_into(s, fresh, output, c.inference.return_treatment, 3.0, clipped);
          ready = w.end;
        }
        const SignalIcView view{std::span<const atx::f64>{rows}.first(ready * s.n_alphas()),
            s.n_dates(), s.n_alphas(), {0U, ready},
            {cfg.horizon, 0U, observed_end, c.inference.return_treatment, 3.0,
             SignalIcPrecision::ExactF64V1}};
        return c.fit(view, w);
      }
    }
    return c.fit(s, w);
  };
  ATX_TRY(auto path, walk_detail::run_path(s.n_dates(), s.n_alphas(), cfg, 0U,
                                          s.n_dates(), 0U, s.n_dates(), fit));
  path.used_ic_cache = cached;
  path.ic_rows_computed = ready;
  return atx::core::Ok(std::move(path));
}

// A cube/stat-cache fit requires an exact matching declared horizon. The cube's
// execution delay is added to the embargo, and its immutable maturity cutoff
// bounds the observed prefix even on later decision dates.
template <CachedIcCombiner C>
[[nodiscard]] atx::core::Result<WeightPath> walk_forward(
    const SignalIcCache& cache, const WalkForwardCfg& cfg, const C& c) {
  const SignalIcView view = cache.view();
  ATX_TRY_VOID(validate_window(view, view.stored_window, 1U));
  if (cfg.horizon != view.config.label_horizon ||
      c.inference.label_horizon != view.config.label_horizon ||
      c.inference.return_treatment != view.config.return_treatment ||
      (view.config.return_treatment == IcReturnTreatment::WinsorizedV2 && view.config.winsor != 3.0)) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "walk_forward: IC cache horizon/return recipe mismatch");
  }
  const auto fit = [&](FitWindow w, atx::usize observed_end) {
    SignalIcView visible = view;
    visible.stored_window.end = w.end;
    visible.values = view.values.first(visible.stored_window.size() * view.n_alphas);
    visible.config.maturity_end = observed_end;
    return c.fit(visible, w); // future cached rows are absent from the fit's view
  };
  ATX_TRY(auto path, walk_detail::run_path(view.n_dates, view.n_alphas, cfg,
      view.config.execution_delay, view.config.maturity_end, view.stored_window.begin,
      view.stored_window.end, fit));
  path.used_ic_cache = true;
  return atx::core::Ok(std::move(path));
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
