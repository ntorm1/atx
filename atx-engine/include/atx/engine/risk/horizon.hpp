#pragma once

// atx::engine::risk — PIT multi-horizon forecast trajectory (P2-S1-3).
//
// A signal carries a forecast decay horizon. At the current as-of period the
// forward trajectory is the horizon-decayed PROJECTION of the CURRENT cross-
// section α_t. Multi-horizon = a superposition over signal sources that decay at
// different rates.
//
// ===========================================================================
//  D8 — PURE numeric kernel (recorded amendment; implemented EXACTLY)
// ===========================================================================
//  forecast_trajectory does NOT take ISignalSource / PanelView. The plan §4.3
//  ISignalSource* form is reconciled away: ISignalSource::evaluate needs a full
//  PanelView and returns a borrowed view, and coupling this header to the alpha-VM
//  stack is undesirable. Instead the S1-4 driver evaluates the sources AT the PIT
//  panel and passes the resulting α_t spans here. The trajectory is then a pure
//  deterministic function of the current α_t only.
//
//  No-look-ahead (R2) therefore holds STRUCTURALLY: there is no future read and no
//  panel coupling — alpha[h] depends solely on the as-of cross-sections.
//
// ===========================================================================
//  Decay model
// ===========================================================================
//  decay(h) = 2^{-h / halflife_periods} — the per-step forecast persistence:
//  the forecast halves every halflife_periods periods. Boundary cases:
//    halflife = +inf ⇒ decay(h) = 1 ∀h   (IDENTITY — the §0.5 boundary-pin case;
//                                          the trajectory is exactly constant)
//    halflife = 0    ⇒ decay(0) = 1, decay(h>0) = 0   (instant decay)
//    halflife < 0    ⇒ INVALID (forecast_trajectory returns Err before calling)
//
// ===========================================================================
//  NaN / no-opinion semantics (load-bearing for the boundary pin)
// ===========================================================================
//  α_t,s[i] == NaN means source s has NO opinion on name i. A no-opinion cell is
//  NOT added to the sum. If NO source has an opinion on name i (all NaN), the
//  output stays NaN — it is NEVER coerced to 0 — so the downstream optimizer's
//  NaN→0/excluded handling stays correct (the boundary pin depends on this).

#include <cmath>   // std::exp2, std::isinf, std::isnan
#include <span>    // std::span (current cross-section views)
#include <utility> // std::pair (source = {α_t span, SignalHorizon})
#include <vector>  // std::vector (the (H+1)×M trajectory)

#include <limits>  // std::numeric_limits (identity = +inf halflife)

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/types.hpp" // f64, usize

namespace atx::engine::risk {

// ---------------------------------------------------------------------------
//  SignalHorizon — a signal's forecast decay, in PERIOD units.
// ---------------------------------------------------------------------------
struct SignalHorizon {
  atx::f64 halflife_periods; // forecast halves every halflife_periods periods

  // decay(h) = 2^{-h / halflife_periods}, with the +inf / 0 edges pinned. PRE-
  // CONDITION: halflife_periods >= 0 (forecast_trajectory validates this before
  // calling, so decay itself is noexcept and assumes a validated horizon).
  [[nodiscard]] atx::f64 decay(atx::usize h) const noexcept {
    // +inf halflife ⇒ no decay (the constant / identity trajectory).
    if (std::isinf(halflife_periods) && halflife_periods > 0.0) {
      return 1.0;
    }
    // Instant decay: only h == 0 survives (the as-of anchor).
    if (halflife_periods == 0.0) {
      return h == 0U ? 1.0 : 0.0;
    }
    // Geometric persistence. Explicit usize→f64 cast (no narrowing into exp2).
    return std::exp2(-static_cast<atx::f64>(h) / halflife_periods);
  }

  // The identity horizon: +inf halflife ⇒ decay(h) == 1 ∀h ⇒ a constant trajectory
  // (the boundary-pin precondition §0.5).
  [[nodiscard]] static SignalHorizon identity() noexcept {
    return SignalHorizon{std::numeric_limits<atx::f64>::infinity()};
  }
};

// ---------------------------------------------------------------------------
//  HorizonForecast — the forward trajectory, (H+1) rows × M names.
// ---------------------------------------------------------------------------
struct HorizonForecast {
  std::vector<std::vector<atx::f64>> alpha; // alpha[h][i] = forecast at horizon h, name i
  atx::usize H;                             // max forward horizon (alpha has H+1 rows)
};

// ---------------------------------------------------------------------------
//  forecast_trajectory — build the forward trajectory from {(α_t, horizon)} pairs.
//
//  sources_now[s].first is source s's CURRENT cross-section α_t,s (length M; a NaN
//  cell == no opinion for that name). For each horizon h in [0, H] and name i:
//      alpha[h][i] = Σ_{s : α_t,s[i] non-NaN} decay_s(h) · α_t,s[i]
//    - NO source has an opinion on name i (all NaN) ⇒ alpha[h][i] = NaN (PRESERVED,
//      never coerced to 0 — the downstream optimizer relies on this).
//    - ≥1 source has an opinion ⇒ the finite decay-weighted sum.
//  alpha[0] uses decay(0)=1, so alpha[0][i] == Σ_s α_t,s[i] over non-NaN sources
//  (single source ⇒ == α_t exactly, NaN preserved) — the no-look-ahead anchor.
//
//  Order-fixed (R1): outer h ascending, then name i ascending, then source s
//  ascending in the inner sum — a fixed reduction order for bitwise determinism.
//
//  Returns Err(InvalidArgument) on: empty sources_now; any source span length != M;
//  any halflife < 0. H may be 0 (single row); M must be >= 1.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::core::Result<HorizonForecast>
forecast_trajectory(std::span<const std::pair<std::span<const atx::f64>, SignalHorizon>> sources_now,
                    atx::usize M, atx::usize H) {
  if (sources_now.empty()) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "forecast_trajectory: sources_now must be non-empty");
  }
  if (M == 0U) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "forecast_trajectory: M must be >= 1");
  }
  // Validate every source up front: matching width and a non-negative halflife.
  for (const auto &[alpha_t, horizon] : sources_now) {
    if (alpha_t.size() != M) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "forecast_trajectory: every source span must have length M");
    }
    // NaN/negative halflife is invalid; +inf (identity) and 0 (instant) are valid.
    if (!(horizon.halflife_periods >= 0.0)) {
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "forecast_trajectory: halflife_periods must be >= 0");
    }
  }

  HorizonForecast out;
  out.H = H;
  out.alpha.assign(H + 1U, std::vector<atx::f64>(M, 0.0));

  // Order-fixed reduction: h ascending, then name i ascending, then source s ascending.
  for (atx::usize h = 0; h <= H; ++h) {
    std::vector<atx::f64> &row = out.alpha[h];
    for (atx::usize i = 0; i < M; ++i) {
      atx::f64 acc = 0.0;
      bool any = false; // did ANY source contribute an opinion on name i?
      for (const auto &[alpha_t, horizon] : sources_now) {
        const atx::f64 a = alpha_t[i];
        if (std::isnan(a)) {
          continue; // no opinion from this source — simply not added
        }
        acc += horizon.decay(h) * a;
        any = true;
      }
      // PRESERVE no-opinion: all-NaN name stays NaN (never coerced to 0).
      row[i] = any ? acc : std::numeric_limits<atx::f64>::quiet_NaN();
    }
  }

  return atx::core::Ok(std::move(out));
}

} // namespace atx::engine::risk
