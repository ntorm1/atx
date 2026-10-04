#pragma once

// atx::engine::book -- volatility-managed leverage (platform v8 Y, lane YCOMB, rule vol-target-v1):
// the risk-managed form of a fixed aim leverage. A book that would run at the fixed leverage L
// (the cap) carries
//
//   L_t = clip(L x sigma_ref_t / sigma_hat_t, floor, L),     floor = 1 (registered)
//
//   sigma_hat_t  the ex-ante volatility of the book's current weights at gross 1, annualised
//                (gross_one_vol of risk_target.hpp: the same forecast as risk-target-v1)
//   sigma_ref_t  the mean of the book's estimates so far, sigma_hat_t included (point in time:
//                only estimates at sessions <= t; the first estimate has ratio 1 exactly)
//
// i.e. the volatility target is sigma*_t = L x sigma_ref_t: the risk the book carries at L in a
// state of average forecast risk, the average taken over the past only (the real-time form of
// Moreira and Muir's (2017) normalising constant, Cederburg, O'Doherty, Wang and Yan 2020). The
// rule never levers above L; it de-levers when the forecast is above its running mean and stops
// at the floor. The ratio is formed before the multiplication (L x (sigma_ref / sigma_hat)), so
// equal forecasts give L bit for bit.
//
// Cadence (registered 21 sessions, monthly as Moreira-Muir): the first estimable decision (a
// book that is not flat, a positive variance), then the first estimable decision at least 21
// sessions after the previous estimate; L_t holds between estimates; L before the first. A due
// decision that cannot be estimated leaves the clock and the running mean where they were.
//
// Header-only (inline): the arithmetic is a handful of operations per estimate; the forecast
// itself is risk_target.hpp's gross_one_vol (engine library). Deterministic: no RNG, clock or
// map. The running mean is sum / count in estimate order, so a port reproduces it bit for bit.

#include <cmath>
#include <limits>
#include <span>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/risk_target.hpp"

namespace atx::engine::book {

// ---- registered constants (vol-target-v1) ------------------------------------------------------
inline constexpr atx::f64 vol_target_floor = 1.0;      // L_t >= 1 (the executable's minimum L)
inline constexpr atx::usize vol_target_cadence = 21;   // sessions between estimates (no flag)

// One book's state across decisions (sessions ascending).
struct VolTargetState {
  bool estimated{};
  atx::usize last{};       // session of the latest estimate
  atx::usize estimates{};  // estimates taken so far
  atx::f64 sigma_sum{};    // sum of the estimates' sigma_hat, in estimate order
  atx::f64 sigma_hat{std::numeric_limits<atx::f64>::quiet_NaN()}; // of the latest estimate
  atx::f64 sigma_ref{std::numeric_limits<atx::f64>::quiet_NaN()}; // running mean at that estimate
  RiskTargetLeverage at{}; // raw = L x sigma_ref / sigma_hat; clip Low = the floor, High = L
};

// raw = cap x (sigma_ref / sigma_hat); leverage = clip(raw, lowest, cap) (Low below `lowest`,
// High above the cap; the bounds themselves are inside). Preconditions (vol_target_update checks
// them): sigma_hat, sigma_ref finite > 0, lowest <= cap.
[[nodiscard]] inline RiskTargetLeverage vol_target_leverage(atx::f64 sigma_hat,
                                                            atx::f64 sigma_ref, atx::f64 cap,
                                                            atx::f64 lowest) noexcept {
  RiskTargetLeverage out;
  out.raw = cap * (sigma_ref / sigma_hat);
  if (out.raw < lowest) {
    out.leverage = lowest;
    out.clip = RiskTargetClip::Low;
  } else if (out.raw > cap) {
    out.leverage = cap;
    out.clip = RiskTargetClip::High;
  } else {
    out.leverage = out.raw;
    out.clip = RiskTargetClip::None;
  }
  return out;
}

// No estimate yet, or session at least `cadence` after the latest one.
[[nodiscard]] inline bool vol_target_due(const VolTargetState& s, atx::usize session,
                                         atx::usize cadence = vol_target_cadence) noexcept {
  return !s.estimated || (session >= s.last && session - s.last >= cadence);
}

// L_t in force: the latest estimate's leverage, else the cap (before the first estimate).
[[nodiscard]] inline atx::f64 vol_target_in_force(const VolTargetState& s, atx::f64 cap) noexcept {
  return s.estimated ? s.at.leverage : cap;
}

// One decision of one book. When due and the book can be estimated (gross_one_vol), adds the
// estimate to the running mean, records it at `session` and returns true; otherwise returns false
// with the state untouched (not due, a flat book, no positive variance). InvalidArgument, state
// untouched: cap not finite or below the floor (or above 1e3), or gross_one_vol's refusals.
[[nodiscard]] inline atx::core::Result<bool>
vol_target_update(atx::f64 cap, atx::usize session, const FactorRiskView& m,
                  std::span<const atx::f64> w, atx::f64 gross, VolTargetState& s) {
  if (!std::isfinite(cap) || cap < vol_target_floor || cap > 1e3) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "vol-target-v1: the cap L (--aim-leverage) must be finite in [1, 1e3]");
  }
  if (!vol_target_due(s, session)) {
    return atx::core::Ok(false);
  }
  auto sigma = gross_one_vol(m, w, gross);
  if (!sigma) {
    if (sigma.error().code() == atx::core::ErrorCode::Unavailable) {
      return atx::core::Ok(false); // a flat book or no positive variance: nothing to scale
    }
    return atx::core::Err(std::move(sigma).error());
  }
  const atx::f64 sigma_hat = *sigma;
  const atx::usize count = s.estimates + 1U;
  const atx::f64 sum = s.sigma_sum + sigma_hat;
  const atx::f64 ref = sum / static_cast<atx::f64>(count);
  s.estimated = true;
  s.last = session;
  s.estimates = count;
  s.sigma_sum = sum;
  s.sigma_hat = sigma_hat;
  s.sigma_ref = ref;
  s.at = vol_target_leverage(sigma_hat, ref, cap, vol_target_floor);
  return atx::core::Ok(true);
}

} // namespace atx::engine::book
