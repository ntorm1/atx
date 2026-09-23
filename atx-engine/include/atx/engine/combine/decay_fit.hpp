#pragma once

// atx::engine::combine — per-alpha IC decay (exponential half-life) fit (Lane 5).
//
//  The decay curve of a forecast is its IC against the ONE-period return k periods
//  ahead: IC(k) = mean_t corr(z(t, ·), r₁(t + k, ·)), k = 0 … K−1. Under the Grinold /
//  Gârleanu-Pedersen AR(1) view IC(k) = IC₀·e^{−φk}, the half-life is ln 2 / φ; it
//  drives horizon matching (down-weight fast decayers) and the trade-rate in the
//  optimizer.
//
//  fit_ic_decay fits log|IC(k)| = log|IC₀| − φk by |IC|-weighted least squares over
//  the leading run of lags that share IC(0)'s sign (a sign flip ends the usable
//  curve). φ <= 0 (no decay) reports an infinite half-life.
//
//  lagged_ic honors the fit/apply firewall: with window [begin, end) only pairs with
//  t >= begin and t + k < end are used (no return row at or past `end` is read).

#include <cmath>   // std::log, std::abs, std::isfinite
#include <limits>  // std::numeric_limits
#include <span>    // std::span
#include <utility> // std::move
#include <vector>  // std::vector

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/combine/signal_store.hpp" // SignalStore, FitWindow, cross_section_corr

namespace atx::engine::combine {

struct DecayFit {
  atx::f64 ic0 = 0.0;       // fitted IC at lag 0 (signed)
  atx::f64 phi = 0.0;       // decay rate per period
  atx::f64 half_life = 0.0; // ln 2 / φ periods; +inf when φ <= 0
  atx::usize n_lags = 0U;   // lags used in the fit
};

// Mean cross-sectional IC of alpha `a` against one-period returns `r1` (date-major
// T×N, the store's shape) at lags 0..max_lag−1 inside window `w`. Lags with no usable
// date are NaN. Err on shape mismatch / a bad window / max_lag == 0.
[[nodiscard]] inline atx::core::Result<std::vector<atx::f64>>
lagged_ic(const SignalStore &s, atx::usize a, std::span<const atx::f64> r1, atx::usize max_lag,
          FitWindow w) {
  const atx::usize n = s.n_instruments();
  if (a >= s.n_alphas() || r1.size() != s.cells() || max_lag == 0U || w.end > s.n_dates() ||
      w.end <= w.begin) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "lagged_ic: bad arguments");
  }
  std::vector<atx::f64> out(max_lag, kSignalNaN);
  for (atx::usize k = 0U; k < max_lag; ++k) {
    atx::f64 sum = 0.0;
    atx::usize cnt = 0U;
    for (atx::usize t = w.begin; t + k < w.end; ++t) {
      const atx::f64 ic = cross_section_corr(s.signal_row(a, t), r1.subspan((t + k) * n, n));
      if (std::isfinite(ic)) {
        sum += ic;
        ++cnt;
      }
    }
    if (cnt > 0U) {
      out[k] = sum / static_cast<atx::f64>(cnt);
    }
  }
  return atx::core::Ok(std::move(out));
}

// Exponential fit of an IC-by-lag curve (see header). Err when fewer than 2 usable
// leading lags (IC(0) must be finite and non-zero).
[[nodiscard]] inline atx::core::Result<DecayFit> fit_ic_decay(std::span<const atx::f64> ic_by_lag) {
  if (ic_by_lag.size() < 2U || !std::isfinite(ic_by_lag[0]) || ic_by_lag[0] == 0.0) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_ic_decay: need >= 2 lags with finite non-zero IC(0)");
  }
  const atx::f64 sign = (ic_by_lag[0] > 0.0) ? 1.0 : -1.0;
  atx::usize m = 0U;
  while (m < ic_by_lag.size() && std::isfinite(ic_by_lag[m]) && sign * ic_by_lag[m] > 0.0) {
    ++m;
  }
  if (m < 2U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "fit_ic_decay: IC flips sign at lag 1 (no decay curve)");
  }
  // Weighted LS of y = log|IC| on x = k with weights |IC| (down-weights the noisy tail).
  atx::f64 sw = 0.0;
  atx::f64 sx = 0.0;
  atx::f64 sy = 0.0;
  for (atx::usize k = 0U; k < m; ++k) {
    const atx::f64 wk = std::abs(ic_by_lag[k]);
    sw += wk;
    sx += wk * static_cast<atx::f64>(k);
    sy += wk * std::log(std::abs(ic_by_lag[k]));
  }
  const atx::f64 mx = sx / sw;
  const atx::f64 my = sy / sw;
  atx::f64 sxx = 0.0;
  atx::f64 sxy = 0.0;
  for (atx::usize k = 0U; k < m; ++k) {
    const atx::f64 wk = std::abs(ic_by_lag[k]);
    const atx::f64 dx = static_cast<atx::f64>(k) - mx;
    sxx += wk * dx * dx;
    sxy += wk * dx * (std::log(std::abs(ic_by_lag[k])) - my);
  }
  const atx::f64 slope = sxy / sxx; // sxx > 0 since m >= 2 distinct x
  DecayFit f;
  f.phi = -slope;
  f.ic0 = sign * std::exp(my - slope * mx);
  f.half_life = (f.phi > 0.0) ? std::log(2.0) / f.phi : std::numeric_limits<atx::f64>::infinity();
  f.n_lags = m;
  return atx::core::Ok(f);
}

} // namespace atx::engine::combine
