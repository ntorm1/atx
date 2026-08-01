#include "atx/vol/derivatives.hpp"

#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <utility>

#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/detail/rv_lognormal.hpp" // lognormal_call (capped var swap, Task 4)
#include "atx/vol/priced_surface.hpp" // E6: PricedSurface-native entry points
#include "atx/vol/strip_grid.hpp"
#include "atx/vol/surface_parity.hpp" // SliceContext (E6 carry extraction)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Composite-Simpson weight. E2: delegated to the shared strip/grid convention
// (`strip_grid.hpp`) so this TU and analytics_density.cpp quadrature identically.
[[nodiscard]] double simpson_w(std::size_t i, std::size_t n) noexcept {
  return strip::simpson_weight(i, n);
}

// Quality-driven default log-strike grid (strip_quality_defaults). Node counts
// are odd so composite Simpson applies.
struct StripGrid {
  double k_min_log;
  double k_max_log;
  std::size_t n_nodes;
};

[[nodiscard]] StripGrid strip_quality_defaults(DerivQuality q) noexcept {
  switch (q) {
  case DerivQuality::Fast:
    return StripGrid{-1.0, 1.0, 97};
  case DerivQuality::High:
    return StripGrid{-2.0, 2.0, 769};
  case DerivQuality::Audit:
    return StripGrid{-3.0, 3.0, 2049};
  case DerivQuality::Standard:
    return StripGrid{-1.5, 1.5, 257};
  }
  return StripGrid{-1.5, 1.5, 257};  // unreachable; STANDARD fallback
}

// Forward at an arbitrary maturity T from the curve set, with flat (clamped)
// extrapolation outside the pillar range — the atx-curve analogue of the C's
// ats_vol_curve_forward_T under ATS_VOL_EXTRAP_CLAMP_FOR_REPORTING. Falls back
// to the reference spot when no forward curve has been set (F == S).
//
// E2 / AN-P1-2: the INTERIOR blend is now LINEAR IN log(F)
// (`strip::forward_log_blend`), matching `projection.cpp`'s `curve_forward_T`.
// It used to be linear in F, so the same forward curve read at the same T gave
// two different answers depending on which module asked. Clamped extrapolation
// and the pillar values themselves are unchanged, so this moves nothing on a
// flat or single-pillar forward curve.
//
// Precondition (matches the C, documented not checked): forward points are in
// ascending T order.
[[nodiscard]] double resolve_forward(const CurveSet& curves, double T) noexcept {
  const std::span<const ForwardPoint> pts = curves.forward.points();
  if (pts.empty()) {
    return curves.spot;
  }
  if (T <= pts.front().T) {
    return pts.front().F;
  }
  if (T >= pts.back().T) {
    return pts.back().F;
  }
  for (std::size_t i = 1; i < pts.size(); ++i) {
    if (T <= pts[i].T) {
      return strip::forward_log_blend(pts[i - 1].T, pts[i - 1].F, pts[i].T, pts[i].F, T);
    }
  }
  return pts.back().F;
}

// Discount factor at T. T <= 0 is the at-expiry shortcut: df = 1.0 by
// definition, no flag. For T > 0 a non-positive yield lookup substitutes
// df = 1.0 and stamps DfFallback so a mismarked PV is detectable (mirrors the
// C's deriv_df_at_T; here the discount comes from curves.yield).
[[nodiscard]] double deriv_df_at_T(const CurveSet& curves, double T,
                                   DerivFlags& out_flags) noexcept {
  if (T <= 0.0) {
    return 1.0;
  }
  const double df = curves.yield.disc(T);
  if (!(df > 0.0)) {
    out_flags |= DerivFlags::DfFallback;
    return 1.0;
  }
  return df;
}

// Reject any non-zero reserved config field (Sprint 28 enforcement). Returns
// true if the config is clean.
[[nodiscard]] bool reserved_fields_clean(const DerivConfig& cfg) noexcept {
  return cfg.abs_price_tol == 0.0 && cfg.rel_price_tol == 0.0 &&
         cfg.flags_request == 0u;
}

// Reject a negative vol_of_vol (Sprint 29 / Task 3 enforcement): 0 selects
// auto-calibration, > 0 is used as-is, and a negative vol-of-vol has no
// meaning. `!(x >= 0.0)` also catches NaN (comparisons with NaN are false),
// matching this file's existing NaN-safe validation idiom (see the `T > 0.0`
// guards above).
[[nodiscard]] bool vol_of_vol_valid(const DerivConfig& cfg) noexcept {
  return cfg.vol_of_vol >= 0.0;
}

// Aged variance blend (decimal units). Marked total variance over the original
// contract horizon; caller guarantees n_done <= n_total (aged_total_variance_dec).
//   n_total == 0          -> fully unaged (return K_var_future).
//   n_done >= n_total > 0 -> fully aged (return rv_done).
//   n_done == 0           -> fully unaged (return K_var_future).
[[nodiscard]] double aged_total_variance_dec(double rv_done_dec,
                                             double k_var_future_dec,
                                             std::uint32_t n_done,
                                             std::uint32_t n_total) noexcept {
  if (n_total == 0u) {
    return k_var_future_dec;
  }
  if (n_done >= n_total) {
    return rv_done_dec;
  }
  if (n_done == 0u) {
    return k_var_future_dec;
  }
  const double w_done = static_cast<double>(n_done) / static_cast<double>(n_total);
  const double w_future =
      static_cast<double>(n_total - n_done) / static_cast<double>(n_total);
  return w_done * rv_done_dec + w_future * k_var_future_dec;
}

// ── Dispatch helpers (templated on the surface parametrization) ────────────

template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_var_swap(const SurfaceT& surface,
                                                const CurveSet& curves,
                                                const DerivContract& contract,
                                                const DerivConfig& cfg) {
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const double T = contract.maturity_t;

  // Discrete-monitoring FULL_MC is rejected up-front (reserved engine).
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::FullMc) {
    return Err(ErrorCode::NotImplemented, "FULL_MC discrete correction reserved");
  }

  // Future leg: only price the strip when there is residual time.
  double k_var_future_dec = 0.0;
  DerivQuote strip_quote{};
  bool strip_ran = false;
  DerivFlags flags = DerivFlags::None;

  if (rv.n_obs_total == 0u || rv.n_obs_done < rv.n_obs_total) {
    if (!(T > 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "var swap needs T > 0 to price the future leg");
    }
    ATX_TRY(auto sq, var_swap_fair_strike(surface, curves, T, cfg));
    strip_quote = sq;
    k_var_future_dec = strip_quote.fair_strike_dec;
    strip_ran = true;

    // Discrete-monitoring correction (Buhler 2006, leading order in 1/n_total):
    // applies to the future implied-variance leg only.
    if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN &&
        rv.n_obs_total >= 1u) {
      k_var_future_dec *= (1.0 + 1.0 / static_cast<double>(rv.n_obs_total));
      flags |= DerivFlags::DiscreteCorrApplied;
    }
  }

  const double total = aged_total_variance_dec(rv.rv_done_dec, k_var_future_dec,
                                               rv.n_obs_done, rv.n_obs_total);

  const double df = deriv_df_at_T(curves, T, flags);
  const double pv = df * contract.notional * (total - contract.strike_dec);

  // Provenance.
  flags |= strip_quote.flags;
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total) {
    flags |= DerivFlags::FullyAged;
  }

  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }

  DerivQuote out{};
  out.fair_strike_dec = total;  // fair strike that prices the contract to PV = 0
  out.fair_strike_points = 1.0e4 * total;
  out.pv = pv;
  out.undiscounted_expectation_dec = total;
  out.uncapped_var_dec = strip_ran ? strip_quote.uncapped_var_dec : 0.0;
  out.accrued_component_dec = w_done * rv.rv_done_dec;
  out.future_component_dec = w_future * k_var_future_dec;
  out.convexity_adjustment_dec = 0.0;
  out.integration_error_est = strip_quote.integration_error_est;
  out.flags = flags;
  return Ok(out);
}

template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_vol_swap(const SurfaceT& surface,
                                                const CurveSet& curves,
                                                const DerivContract& contract,
                                                const DerivConfig& cfg) {
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const double T = contract.maturity_t;
  const bool fully_aged =
      (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total);
  const bool unaged = (rv.n_obs_done == 0u);

  if (!fully_aged && !unaged) {
    // Mid-life vol-swap MtM needs E[sqrt((A + n_f*RV_f)/n_t)]; the unbiased
    // computation requires a distribution engine this port does not ship.
    return Err(ErrorCode::NotImplemented,
               "mid-life vol-swap dispatch needs a distribution engine");
  }

  if (fully_aged) {
    const double r1 = std::sqrt(std::fmax(rv.rv_done_dec, 0.0));
    DerivFlags flags = DerivFlags::Aged | DerivFlags::FullyAged;
    const double df = deriv_df_at_T(curves, T, flags);
    const double pv = df * contract.notional * (r1 - contract.strike_dec);

    DerivQuote out{};
    out.fair_strike_dec = r1;
    out.fair_strike_points = 1.0e2 * r1;
    out.pv = pv;
    out.undiscounted_expectation_dec = r1;
    out.accrued_component_dec = r1;
    out.future_component_dec = 0.0;
    out.convexity_adjustment_dec = 0.0;
    out.integration_error_est = kNaN;  // NaN = not estimated
    out.flags = flags;
    return Ok(out);
  }

  // Unaged vol-swap pricing.
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "unaged vol swap needs T > 0");
  }

  ATX_TRY(auto vol_q, vol_swap_fair_strike(surface, curves, T, cfg));

  const double k_vol = vol_q.fair_strike_dec;
  DerivFlags flags = DerivFlags::VolCarrLee;
  const double df = deriv_df_at_T(curves, T, flags);
  const double pv = df * contract.notional * (k_vol - contract.strike_dec);

  DerivQuote out{};
  out.fair_strike_dec = k_vol;
  out.fair_strike_points = 1.0e2 * k_vol;
  out.pv = pv;
  out.undiscounted_expectation_dec = k_vol;

  // Best-effort variance strip to populate the convexity diagnostic; do not
  // fail the price call if the strip is unavailable.
  if (const Result<DerivQuote> strip = var_swap_fair_strike(surface, curves, T, cfg);
      strip.has_value()) {
    out.uncapped_var_dec = strip->uncapped_var_dec;
    out.convexity_adjustment_dec =
        std::sqrt(std::fmax(strip->uncapped_var_dec, 0.0)) - k_vol;
  }
  out.accrued_component_dec = 0.0;
  out.future_component_dec = k_vol;
  out.flags = flags;
  return Ok(out);
}

// Carr-Lee ATMF-straddle vol-strike, K_vol ~= sqrt(2 pi / T) * C_ATMF(T) /
// (F * df). Factored out of vol_swap_fair_strike (below) so it and
// resolve_vol_of_vol's auto-calibration path (also below) share ONE
// implementation — the brief that introduced vol-of-vol auto-calibration
// requires the calibrated lognormal to reproduce this number exactly, which
// is only guaranteed if both callers compute it the same way.
//
// @return InvalidArgument for T <= 0; OutOfRange if the forward/discount
//         cannot be resolved or the ATMF implied vol is non-finite/<= 0.
template <class SurfaceT>
[[nodiscard]] Result<double> carr_lee_k_vol(const SurfaceT& surface,
                                            const CurveSet& curves, double T) {
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "carr-lee K_vol needs T > 0");
  }

  const double F = resolve_forward(curves, T);
  const double df = curves.yield.disc(T);
  if (!(F > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::OutOfRange, "forward/discount unavailable at T");
  }

  // ATMF implied vol — log-moneyness 0.
  const double sigma_atmf = surface.iv(0.0, T);
  if (!std::isfinite(sigma_atmf) || sigma_atmf <= 0.0) {
    return Err(ErrorCode::OutOfRange, "ATMF implied vol non-finite or <= 0");
  }

  const double c_atmf = black76_price(F, F, T, sigma_atmf, df, Side::Call);
  return Ok(std::sqrt(2.0 * std::numbers::pi / T) * c_atmf / (F * df));
}

// Vol-of-vol resolution result. Internal only (Tasks 4-6's shared helper,
// same TU) — no header declaration; consumers in later tasks live in this
// same anonymous namespace.
struct VolOfVol {
  double xi;
  bool calibrated;
};

// Resolve the vol-of-vol for a contract: explicit cfg wins; otherwise calibrate
// s.t. the lognormal E[sqrt(W)] reproduces the Carr-Lee K_vol on this surface
// at this tenor: s^2 = -8 ln(k_vol_cl / sqrt(k_var)), xi = s / sqrt(T).
// Returns xi and whether it was calibrated (for the flag). k_vol_cl >= sqrt(k_var)
// (no convexity, or degenerate inputs) yields xi = 0.
//
// `calibrated` means "the auto path ran" — set true whenever cfg.vol_of_vol
// selected auto-calibration, INCLUDING the degenerate xi = 0 outcome (no
// convexity, or a k_var_future too small/non-finite to calibrate against).
// The alternative reading — calibrated only when xi > 0 — would make the flag
// answer "did we compute a value" AND "is that value non-trivial" at once,
// forcing a caller to inspect xi just to know which config path ran. Only an
// EXPLICIT cfg.vol_of_vol (the caller's own number, not ours) gets `false`.
template <class SurfaceT>
[[nodiscard]] Result<VolOfVol> resolve_vol_of_vol(const SurfaceT& surface,
                                                  const CurveSet& curves, double T,
                                                  double k_var_future,
                                                  const DerivConfig& cfg) {
  if (cfg.vol_of_vol > 0.0) {
    return Ok(VolOfVol{cfg.vol_of_vol, false});
  }

  // Auto-calibrate against THIS surface's own Carr-Lee convexity — the same
  // helper vol_swap_fair_strike uses, so a caller pricing off this xi never
  // disagrees with the plain Carr-Lee vol-swap quote by construction.
  ATX_TRY(const double k_vol_cl, carr_lee_k_vol(surface, curves, T));

  if (!std::isfinite(k_vol_cl) || !std::isfinite(k_var_future) ||
      !(k_var_future > 0.0)) {
    return Ok(VolOfVol{0.0, true});  // degenerate input; auto path still "ran"
  }

  const double sqrt_k_var = std::sqrt(k_var_future);
  const double ratio = k_vol_cl / sqrt_k_var;
  // ratio >= 1 (written as !(ratio < 1.0) so a NaN ratio also lands here, not
  // in the log() below): no convexity, or an inverted/degenerate input. s^2
  // would be <= 0, not a valid lognormal log-stdev, so xi = 0 (RV collapses
  // to its own mean).
  if (!(ratio < 1.0) || !(ratio > 0.0)) {
    return Ok(VolOfVol{0.0, true});
  }

  const double s2 = -8.0 * std::log(ratio);
  const double xi = std::sqrt(s2) / std::sqrt(T);
  return Ok(VolOfVol{xi, true});
}

// Assembles a DerivQuote for price_capped_var_swap's three exit paths
// (pinned, fully-aged deterministic, model-based blend) so the shared
// bookkeeping -- PV, points conversion, component breakdown -- lives in one
// place instead of being repeated at each exit.
[[nodiscard]] DerivQuote capped_var_swap_quote(double expectation_dec, double accrued_dec,
                                               double future_dec, double cap_option_dec,
                                               double df, const DerivContract& contract,
                                               DerivFlags flags) noexcept {
  DerivQuote out{};
  out.fair_strike_dec = expectation_dec;  // E[min(V,C)]: the strike pricing to PV = 0
  out.fair_strike_points = 1.0e4 * expectation_dec;
  out.pv = df * contract.notional * (expectation_dec - contract.strike_dec);
  out.undiscounted_expectation_dec = expectation_dec;
  out.accrued_component_dec = accrued_dec;
  out.future_component_dec = future_dec;
  out.cap_option_value_dec = cap_option_dec;
  out.flags = flags;
  return out;
}

// Capped variance swap: E[min(V,C)] for the blended variance V = a + b*W (see
// file header for the model). Mirrors price_var_swap's structure -- blend
// weights, strip for the future leg, aged-provenance flags -- but the PIN
// check has to run BEFORE the strip and BEFORE any T > 0 requirement: a
// contract whose accrued leg alone already reached the cap is a valid quote
// request at expiry (T == 0), and must not pay for (or fail on) a strip it
// does not need.
//
// Exit paths, in order:
//   1. PIN: a = w_done*rv_done_dec >= cap_dec -> deterministic, no strip.
//   2. FULLY AGED (not pinned): min(V,C) = rv_done_dec exactly (w_future ==
//      0, no future leg to model).
//   3. Otherwise: strip for K_var_future, resolve vol-of-vol, and the
//      displaced-lognormal closed form via detail::lognormal_call.
//
// Precondition (enforced by deriv_price before this is ever called):
// contract.cap_dec > 0.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_capped_var_swap(const SurfaceT& surface,
                                                        const CurveSet& curves,
                                                        const DerivContract& contract,
                                                        const DerivConfig& cfg) {
  assert(contract.cap_dec > 0.0 && "capped var swap: cap_dec validated by the caller");
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const double T = contract.maturity_t;
  const double cap = contract.cap_dec;

  // Discrete-monitoring FULL_MC is rejected up-front (reserved engine),
  // mirroring price_var_swap's guard.
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::FullMc) {
    return Err(ErrorCode::NotImplemented, "FULL_MC discrete correction reserved");
  }

  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }
  const double accrued = w_done * rv.rv_done_dec;

  DerivFlags flags = DerivFlags::None;
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total) {
    flags |= DerivFlags::FullyAged;
  }

  if (accrued >= cap) {
    flags |= DerivFlags::CapPinned | DerivFlags::CapApplied;
    const double df = deriv_df_at_T(curves, T, flags);
    return Ok(capped_var_swap_quote(cap, accrued, 0.0, 0.0, df, contract, flags));
  }

  if (has_flag(flags, DerivFlags::FullyAged)) {
    // rv_done_dec < cap here (the pin check above already handled >= cap):
    // min(V,C) collapses to the realized leg, no model needed.
    const double df = deriv_df_at_T(curves, T, flags);
    return Ok(capped_var_swap_quote(accrued, accrued, 0.0, 0.0, df, contract, flags));
  }

  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "capped var swap needs T > 0 to price the future leg");
  }
  ATX_TRY(auto sq, var_swap_fair_strike(surface, curves, T, cfg));

  // Discrete-monitoring correction (Buhler 2006, leading order in 1/n_total) --
  // same formula and flag as price_var_swap's, applied BEFORE the blend and
  // BEFORE the lognormal model: the corrected mean is both the blend's future
  // leg AND resolve_vol_of_vol's calibration target, so a plain VarSwap and a
  // CappedVarSwap on the same underlying see the same future variance leg
  // under this config (otherwise CapParityHolds silently breaks under it).
  double m = sq.fair_strike_dec;
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN &&
      rv.n_obs_total >= 1u) {
    m *= (1.0 + 1.0 / static_cast<double>(rv.n_obs_total));
    flags |= DerivFlags::DiscreteCorrApplied;
  }

  ATX_TRY(const VolOfVol vv, resolve_vol_of_vol(surface, curves, T, m, cfg));
  const double s = vv.xi * std::sqrt(T);
  const double k_c = (cap - accrued) / w_future;  // w_future > 0: not fully aged (checked above)
  const double cap_option = w_future * detail::lognormal_call(m, s, k_c);
  const double expectation = (accrued + w_future * m) - cap_option;

  flags |= DerivFlags::ModelProxy | DerivFlags::CapApplied | sq.flags;
  if (vv.calibrated) {
    flags |= DerivFlags::VolOfVolCalibrated;
  }
  const double df = deriv_df_at_T(curves, T, flags);

  DerivQuote out =
      capped_var_swap_quote(expectation, accrued, w_future * m, cap_option, df, contract, flags);
  out.uncapped_var_dec = sq.uncapped_var_dec;
  out.integration_error_est = sq.integration_error_est;
  out.vol_of_vol_used = vv.xi;
  return Ok(out);
}

}  // namespace

// ── Variance strip ─────────────────────────────────────────────────────────

template <class SurfaceT>
Result<DerivQuote> var_swap_fair_strike(const SurfaceT& surface,
                                        const CurveSet& curves, double T,
                                        const DerivConfig& cfg) {
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "var strip needs T > 0");
  }
  if (!reserved_fields_clean(cfg)) {
    return Err(ErrorCode::NotImplemented, "reserved config field is non-zero");
  }
  if (!vol_of_vol_valid(cfg)) {
    return Err(ErrorCode::InvalidArgument, "vol_of_vol must be >= 0");
  }

  // Grid bounds and node count: quality default, overridden by the config.
  // An explicit [k_min_log, k_max_log] PINS the span — the caller asked for
  // exactly that strip and gets exactly it (with a truncation flag if it does
  // not cover the wings). Otherwise the tier span is a FLOOR that E2 widens to
  // the tenor's own vol scale below, once the ATM vol is known.
  StripGrid grid = strip_quality_defaults(cfg.quality);
  bool span_pinned = false;
  if (cfg.k_min_log != 0.0 || cfg.k_max_log != 0.0) {
    if (!(cfg.k_min_log < cfg.k_max_log)) {
      return Err(ErrorCode::InvalidArgument, "k_min_log must be < k_max_log");
    }
    grid.k_min_log = cfg.k_min_log;
    grid.k_max_log = cfg.k_max_log;
    span_pinned = true;
  }
  if (cfg.strip_nodes != 0u) {
    std::uint32_t n = cfg.strip_nodes;
    if ((n & 1u) == 0u) {
      n += 1u;  // force odd for composite Simpson
    }
    if (n < 5u) {
      n = 5u;
    }
    grid.n_nodes = n;
  }

  // Forward + discount at T.
  const double F = resolve_forward(curves, T);
  const double df = curves.yield.disc(T);
  if (!(F > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::OutOfRange, "forward/discount unavailable at T");
  }

  // ── E2 / AN-P1-2: adaptive wings ────────────────────────────────────────
  //
  // The tier span is fixed in k and knows nothing about σ√T, so a high-vol or
  // long-dated tenor integrated only the middle of its own distribution and
  // reported K_var biased LOW. Widen the (symmetric) span to the shared
  // convention `max(tier_span, 6·σ_atm·√T)` — the same policy
  // `analytics_density.cpp` already used via `RndConfig::width_sigmas`.
  //
  // `required` is also what decides truncation below. A tenor whose ATM vol is
  // unusable yields required == 0, i.e. "coverage not judgeable", and the span
  // stays at the tier default.
  //
  // FIX-E M-6: the width is a CONFIG knob (`DerivConfig::width_sigmas`), as it
  // already was on the density route. 0 keeps the shared 6σ default, so every
  // existing caller is unchanged; a negative value turns vol scaling off, which
  // is the escape hatch for a caller who wants an exactly-specified strip and
  // does not want it flagged short.
  const double width_sigmas =
      cfg.width_sigmas == 0.0 ? strip::kDefaultWidthSigmas : cfg.width_sigmas;
  const double sigma_atm = surface.iv(0.0, T);
  const double required = strip::required_half_width(sigma_atm, T, width_sigmas);
  if (!span_pinned) {
    const double floor_half = std::fmax(-grid.k_min_log, grid.k_max_log);
    const double kh = strip::adaptive_half_width(floor_half, sigma_atm, T, width_sigmas);
    if (kh > 0.0) {
      grid.k_min_log = -kh;
      grid.k_max_log = kh;
    }
    // FIX-E M-7: SCALE THE NODE COUNT WITH THE SPAN. A tier promises a
    // resolution (Δk), not just a node count: Standard is 257 nodes over ±1.5,
    // i.e. Δk ≈ 0.0117. Widening to ±3.6 at 257 nodes silently made Δk 2.4x
    // COARSER (3.6x on Fast) on exactly the tenors E2 widens — trading a
    // truncation bias for a quadrature one. Hold Δk at the tier's own value
    // instead. Only when the caller has not pinned `strip_nodes`: an explicit
    // node count is a request, same as an explicit span.
    if (cfg.strip_nodes == 0u && kh > floor_half && floor_half > 0.0) {
      const double intervals = static_cast<double>(grid.n_nodes - 1) * (kh / floor_half);
      grid.n_nodes = strip::odd_nodes(
          static_cast<std::size_t>(std::ceil(intervals)) + 1u, grid.n_nodes);
      // Round up to 4m+1: the Richardson half-grid error estimate below needs
      // the half grid ((n+1)/2 nodes) to be odd again, which plain odd-forcing
      // does not guarantee (e.g. n=99 halves to 50, even). The tier defaults
      // are already 4m+1 (97/257/769/2049); only this adaptive rescale can
      // land off that lattice, so only it needs the correction.
      if ((grid.n_nodes % 4u) != 1u) {
        grid.n_nodes += 2u;
      }
    }
  }

  const std::size_t n = grid.n_nodes;
  const double dx = (grid.k_max_log - grid.k_min_log) / static_cast<double>(n - 1);

  // Richardson half-grid quadrature error estimate. Valid only when n is
  // 4m+1, so the half grid ((n+1)/2 nodes, every other node of the full grid)
  // is itself an odd count and a valid composite-Simpson grid on its own. The
  // FIX-E M-7 rounding above guarantees this for the adaptive path; the tier
  // defaults are 4m+1 already; a caller-pinned `strip_nodes` is a request and
  // is not rounded, so it may leave `halvable` false.
  const bool halvable = (n % 4u) == 1u;
  const std::size_t n_half = (n + 1) / 2;
  double integral_half = 0.0;

  // Composite Simpson on   integral OTM(K) / (df * F * e^x) dx
  //                      == integral OTM(K) / (df * K) dx   since K = F * e^x.
  // Nodes with a non-finite / non-positive surface IV contribute zero; a bad
  // node touching either integration boundary flips the truncation flag.
  double integral = 0.0;
  bool bad_first = false;
  bool bad_last = false;
  for (std::size_t i = 0; i < n; ++i) {
    const double x = grid.k_min_log + dx * static_cast<double>(i);
    const double K = F * std::exp(x);
    const Side side = (x < 0.0) ? Side::Put : Side::Call;
    const double sigma = surface.iv(x, T);
    const bool bad = !std::isfinite(sigma) || sigma <= 0.0;
    if (i == 0) {
      bad_first = bad;
    }
    if (i == n - 1) {
      bad_last = bad;
    }
    const double price = bad ? 0.0 : black76_price(F, K, T, sigma, df, side);
    const double integrand = price / (df * K);
    integral += simpson_w(i, n) * integrand;
    if (halvable && (i % 2u) == 0u) {
      // Every other node of the full grid, quadratured on its own half-density
      // grid (spacing 2*dx) with that grid's own Simpson weights.
      integral_half += simpson_w(i / 2u, n_half) * integrand;
    }
  }
  integral *= (dx / 3.0);

  const double k_var = (2.0 / T) * integral;

  // Composite-Simpson error is O(h^4): halving h (doubling the node density)
  // shrinks it ~16x, so the difference between the two estimates is ~15/16 of
  // the coarse grid's own error — a self-contained error bound with no
  // external reference. Stays NaN (not 0) when the grid is not 4m+1: that is
  // a caller-pinned exact node count, and NaN says "not estimated" rather
  // than claiming a zero error the code never checked.
  double err_est = kNaN;
  if (halvable) {
    integral_half *= (2.0 * dx / 3.0);
    err_est = std::fabs((2.0 / T) * (integral - integral_half)) / 15.0;
  }

  // E2 / AN-P1-2: truncation is a COVERAGE property, not a NaN property. The
  // old code raised these flags only when the surface returned a non-finite IV
  // at an integration boundary — which a parametric eSSVI/SVI surface never
  // does, so a truncated parametric strip claimed full coverage. Report a wing
  // as truncated when the span does not reach 6·σ_atm·√T on that side, OR when
  // the boundary node was unusable (the original condition, still meaningful
  // for surfaces with genuine NaN wings).
  const strip::WingCoverage cover =
      strip::wing_coverage(grid.k_min_log, grid.k_max_log, required);

  DerivFlags flags = DerivFlags::None;
  if (bad_first || cover.left_short) {
    flags |= DerivFlags::StripTruncatedLeft;
  }
  if (bad_last || cover.right_short) {
    flags |= DerivFlags::StripTruncatedRight;
  }

  DerivQuote out{};
  out.fair_strike_dec = k_var;
  out.fair_strike_points = 1.0e4 * k_var;
  out.pv = 0.0;
  out.undiscounted_expectation_dec = k_var;
  out.uncapped_var_dec = k_var;
  out.accrued_component_dec = 0.0;
  out.future_component_dec = k_var;
  out.convexity_adjustment_dec = 0.0;
  out.integration_error_est = err_est;
  out.flags = flags;
  return Ok(out);
}

// ── Carr-Lee volatility strike ─────────────────────────────────────────────

template <class SurfaceT>
Result<DerivQuote> vol_swap_fair_strike(const SurfaceT& surface,
                                        const CurveSet& curves, double T,
                                        const DerivConfig& cfg) {
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "vol strip needs T > 0");
  }
  if (!reserved_fields_clean(cfg)) {
    return Err(ErrorCode::NotImplemented, "reserved config field is non-zero");
  }
  if (!vol_of_vol_valid(cfg)) {
    return Err(ErrorCode::InvalidArgument, "vol_of_vol must be >= 0");
  }

  // K_vol ~= sqrt(2 pi / T) * C_ATMF / (F * df) — shared with
  // resolve_vol_of_vol's auto-calibration path so the two never drift.
  ATX_TRY(const double k_vol, carr_lee_k_vol(surface, curves, T));

  DerivQuote out{};
  out.fair_strike_dec = k_vol;
  out.fair_strike_points = 1.0e2 * k_vol;
  out.pv = 0.0;
  out.undiscounted_expectation_dec = k_vol;
  out.uncapped_var_dec = 0.0;  // not computed in this entry
  out.convexity_adjustment_dec = 0.0;
  out.flags = DerivFlags::VolCarrLee;
  return Ok(out);
}

// ── Unified dispatch ───────────────────────────────────────────────────────

template <class SurfaceT>
Result<DerivQuote> deriv_price(const SurfaceT& surface, const CurveSet& curves,
                               const DerivContract& contract,
                               const DerivConfig& cfg) {
  // Reserved engines fail clean before any work. RvDistributionProxy is the
  // one exception: Task 4 wires it up (alongside Auto) as the distribution
  // model's entry point, but ONLY for CappedVarSwap -- every other kind still
  // sees it as reserved.
  switch (cfg.engine) {
  case DerivEngine::RvDistributionProxy:
    if (contract.kind != DerivKind::CappedVarSwap) {
      return Err(ErrorCode::NotImplemented, "reserved pricing engine");
    }
    break;
  case DerivEngine::RvDistributionAffine:
  case DerivEngine::McQe:
    return Err(ErrorCode::NotImplemented, "reserved pricing engine");
  case DerivEngine::Auto:
  case DerivEngine::StripLogContract:
  case DerivEngine::VolCarrLee:
    break;
  }

  // Reject any non-zero reserved field before dispatch.
  if (!reserved_fields_clean(cfg)) {
    return Err(ErrorCode::NotImplemented, "reserved config field is non-zero");
  }
  if (!vol_of_vol_valid(cfg)) {
    return Err(ErrorCode::InvalidArgument, "vol_of_vol must be >= 0");
  }

  // cap_dec validation applies uniformly to both capped kinds (even though
  // CappedVolSwap still dead-ends in NotImplemented below): a malformed
  // contract should fail the same way regardless of which capped product it
  // names. Uncapped kinds (VarSwap/VolSwap) must leave cap_dec at 0.
  const bool is_capped_kind = contract.kind == DerivKind::CappedVarSwap ||
                              contract.kind == DerivKind::CappedVolSwap;
  if (is_capped_kind) {
    if (!(contract.cap_dec > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "capped kind needs cap_dec > 0");
    }
  } else if (contract.cap_dec != 0.0) {
    return Err(ErrorCode::InvalidArgument, "cap_dec is only valid on capped kinds");
  }

  switch (contract.kind) {
  case DerivKind::VarSwap:
    return price_var_swap(surface, curves, contract, cfg);
  case DerivKind::VolSwap:
    return price_vol_swap(surface, curves, contract, cfg);
  case DerivKind::CappedVarSwap:
    if (cfg.engine == DerivEngine::StripLogContract || cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price capped kinds");
    }
    return price_capped_var_swap(surface, curves, contract, cfg);
  case DerivKind::CappedVolSwap:
    return Err(ErrorCode::NotImplemented, "capped volatility swap reserved");
  }
  // Defends against an out-of-enum kind (matches the C default's ERR_INVALID).
  return Err(ErrorCode::InvalidArgument, "unknown derivative kind");
}

// ── RealizedTracker ────────────────────────────────────────────────────────

Result<RealizedTracker> RealizedTracker::create(double annualization,
                                                std::uint32_t n_obs_total) {
  if (!(annualization > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "annualization must be > 0");
  }
  if (n_obs_total == 0u) {
    return Err(ErrorCode::InvalidArgument, "n_obs_total must be > 0");
  }
  RealizedTracker t;
  t.rv_.annualization = annualization;
  t.rv_.n_obs_total = n_obs_total;
  return Ok(std::move(t));
}

Status RealizedTracker::observe(double spot) {
  if (!(spot > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "spot must be > 0");
  }
  if (!(rv_.annualization > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "tracker not initialized");
  }

  if (!have_prev_) {
    // First observation seeds the previous spot. No return yet.
    prev_spot_ = spot;
    have_prev_ = true;
    return Ok();
  }

  // Refuse once all n_obs_total returns have been observed.
  if (rv_.n_obs_done >= rv_.n_obs_total) {
    return Err(ErrorCode::InvalidArgument, "all observations already recorded");
  }

  const double r = std::log(spot / prev_spot_);
  rv_.sum_sq_log_returns_done += r * r;
  rv_.n_obs_done += 1u;
  prev_spot_ = spot;

  const double n = static_cast<double>(rv_.n_obs_done);
  rv_.rv_done_dec = rv_.annualization * rv_.sum_sq_log_returns_done / n;
  return Ok();
}

Status RealizedTracker::observe_batch(std::span<const double> spots) {
  for (const double spot : spots) {
    ATX_TRY_VOID(observe(spot));
  }
  return Ok();
}

// ── Explicit instantiations (mirrors surface.cpp) ──────────────────────────

template Result<DerivQuote> var_swap_fair_strike<EssviSurface>(
    const EssviSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> var_swap_fair_strike<SviSurface>(
    const SviSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> vol_swap_fair_strike<EssviSurface>(
    const EssviSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> vol_swap_fair_strike<SviSurface>(
    const SviSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> deriv_price<EssviSurface>(
    const EssviSurface&, const CurveSet&, const DerivContract&, const DerivConfig&);
template Result<DerivQuote> deriv_price<SviSurface>(
    const SviSurface&, const CurveSet&, const DerivContract&, const DerivConfig&);

// ── E6 / AN-W: PricedSurface-native entry points ───────────────────────────

namespace {

// The fitted surface's OWN carry, expressed as a CurveSet so the strip resolves
// forward and discount exactly as it does on the templated path. Pillars come
// straight from the surface's fitted `context()`; between them `resolve_forward`
// applies the shared log-F convention (strip_grid.hpp, E2) and `YieldCurve`
// interpolates the per-expiry rates `rate_at` decodes from each slice's own
// discount factor.
[[nodiscard]] Result<CurveSet> carry_from(const PricedSurface& ps, double T) {
  const std::span<const SliceContext> pillars = ps.context();
  if (pillars.empty()) {
    return Err(ErrorCode::InvalidArgument, "deriv: surface carries no fitted pillar");
  }
  // FITTED-RANGE GATE (applied below, once the usable pillar set is known).
  // Between pillars this CurveSet reproduces the surface's own carry; OUTSIDE
  // them it does not, and the disagreement is not benign. `resolve_forward`
  // clamps flat past the end pillars, whereas `PricedSurface::forward_at` keeps
  // extrapolating economically (S·exp((r−q_eff)·T)). The strip prices every node
  // at F·e^x and reads its vol from `ps.iv(F·e^x, T)`, so a forward that is not
  // the surface's own would put k = 0 somewhere other than the surface's ATM and
  // bias K_var — silently. Refuse instead. A caller who genuinely wants an
  // extrapolated tenor supplies its own `CurveSet` through the templated
  // overload and owns that choice.
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "deriv: T must be > 0");
  }
  CurveSet cs;
  cs.spot = ps.pricing().S;

  std::vector<double> ts;
  std::vector<double> rates;
  std::vector<ForwardPoint> fwd;
  ts.reserve(pillars.size());
  rates.reserve(pillars.size());
  fwd.reserve(pillars.size());
  for (const SliceContext& p : pillars) {
    if (!(p.T > 0.0) || !(p.forward > 0.0)) {
      continue; // a degenerate pillar contributes no carry
    }
    ts.push_back(p.T);
    rates.push_back(ps.rate_at(p.T));
    ForwardPoint fp;
    fp.T = p.T;
    fp.F = p.forward;
    fp.q_eff = p.q_eff;
    fwd.push_back(fp);
  }
  if (ts.empty()) {
    return Err(ErrorCode::InvalidArgument, "deriv: surface carries no usable fitted pillar");
  }
  // FIX-E I-5. The gate runs AFTER the filter and on the SURVIVING pillars.
  // Gating on `pillars.front()/back()` while building the CurveSet from the
  // filtered list is two different pillar sets in one function: if the first or
  // last pillar is degenerate (T <= 0 or forward <= 0) it is dropped from the
  // curve but still widens the admitted range, so an admitted T could land
  // outside the surviving forward curve — exactly the flat-clamp-vs-extrapolate
  // disagreement the gate exists to prevent, reopened on the degenerate-pillar
  // path. `ts` is the correct set: it is the one the curve is built from, so
  // "admitted" and "interpolated rather than clamped" become the same
  // condition by construction. (`ps.context()` is ascending in T, and the
  // filter preserves order, so front/back of `ts` are its min/max.)
  if (T < ts.front() || T > ts.back()) {
    return Err(ErrorCode::OutOfRange,
               "deriv: T is outside the surface's usable fitted pillar range; the "
               "PricedSurface overloads do not extrapolate carry");
  }
  ATX_TRY_VOID(cs.set_yield(ts, rates));
  cs.forward.set(fwd);
  return Ok(std::move(cs));
}

// Presents a PricedSurface through the LOG-MONEYNESS `iv(k_log, T)` contract the
// strip templates require. `PricedSurface::iv` is STRIKE-based, so the
// conversion has to happen somewhere, and it MUST use the same forward the strip
// itself uses — otherwise the vol would be read at one strike while the price is
// computed at another. Hence `resolve_forward(*curves, T)` here rather than
// `ps->forward_at(T)`: inside the fitted pillar range (the only range
// `carry_from` admits) the two agree, and using the strip's own forward is what
// keeps the two reads on the same strike by construction.
// FIX-E M-8: the forward is CONSTANT across a strip but `resolve_forward` is a
// linear scan over the pillars, and the strip calls `iv` once per node (97-2049
// times). Resolve it once for the strip's own tenor at construction and reuse it
// whenever the query T matches; a query at any other T (nothing does today, but
// the templates are free to) falls back to the full resolve, so the cache is an
// optimisation and never a behaviour change.
struct PricedSurfaceStripView {
  const PricedSurface* ps;
  const CurveSet* curves;
  double T_cached;
  double F_cached;

  PricedSurfaceStripView(const PricedSurface* surface, const CurveSet* cs, double T) noexcept
      : ps{surface}, curves{cs}, T_cached{T}, F_cached{resolve_forward(*cs, T)} {}

  [[nodiscard]] double iv(double k_log, double T) const noexcept {
    const double F = (T == T_cached) ? F_cached : resolve_forward(*curves, T);
    if (!(F > 0.0) || !std::isfinite(k_log)) {
      return kNaN;
    }
    return ps->iv(F * std::exp(k_log), T);
  }
};

} // namespace

Result<DerivQuote> var_swap_fair_strike(const PricedSurface& surface, double T,
                                        const DerivConfig& cfg) {
  ATX_TRY(const CurveSet curves, carry_from(surface, T));
  const PricedSurfaceStripView view{&surface, &curves, T};
  return var_swap_fair_strike(view, curves, T, cfg);
}

Result<DerivQuote> vol_swap_fair_strike(const PricedSurface& surface, double T,
                                        const DerivConfig& cfg) {
  ATX_TRY(const CurveSet curves, carry_from(surface, T));
  const PricedSurfaceStripView view{&surface, &curves, T};
  return vol_swap_fair_strike(view, curves, T, cfg);
}

Result<DerivQuote> deriv_price(const PricedSurface& surface, const DerivContract& contract,
                               const DerivConfig& cfg) {
  ATX_TRY(const CurveSet curves, carry_from(surface, contract.maturity_t));
  const PricedSurfaceStripView view{&surface, &curves, contract.maturity_t};
  return deriv_price(view, curves, contract, cfg);
}

}  // namespace atx::vol
