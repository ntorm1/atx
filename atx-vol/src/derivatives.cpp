#include "atx/vol/derivatives.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <concepts>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <utility>

#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/black76.hpp"
#include "atx/vol/detail/deriv_ref_bridge.hpp" // Task 9: SurfaceRef-native entry points
#include "atx/vol/detail/legacy_surface.hpp" // Essvi/SviSurface (demoted, S4-T21)
#include "atx/vol/detail/risk_surface_validation.hpp" // RiskSurfaceValidationConfig (wing-clamp band assert)
#include "atx/vol/detail/rv_lognormal.hpp" // lognormal_call, truncated_expect, norm_cdf (Tasks 4-5)
#include "atx/vol/portfolio_pricer.hpp" // Task 9: SurfaceRef (the borrowed-surface handle)
#include "atx/vol/priced_surface.hpp" // E6: PricedSurface-native entry points
#include "atx/vol/detail/strip_grid.hpp"
#include "atx/vol/surface_parity.hpp" // SliceContext (E6 carry extraction)
#include "atx/vol/surface_policy.hpp" // certified_wing_half_band (FIT-C7 / Task C-6)
#include "atx/vol/vol_surface.hpp" // Tier-A calibration-grade surface container

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace detail {

// See the declaration (derivatives.hpp, "Carr-Lee convexity refinement") for
// the from-paper derivation -- INCLUDING review fix I-1's note that
// `k_vol_naive` is a straddle PROXY for the paper's IV0 (ATM implied vol),
// not IV0 itself, with its own ~sigma^3*T/24 un-corrected residual
// (~1.667 vol bp on this task's fixture, larger than the +0.906 vol bp this
// refinement adds). Pure arithmetic -- no branches, no early exits, so every
// input (including a degenerate k_vol_naive == 0) produces a well-defined
// finite result: the denominator is >= 8 for any T > 0, so there is no
// division-by-zero to guard.
double refine_carr_lee_k_vol(double k_vol_naive, double k_var, double T) noexcept {
  const double naive_sq = k_vol_naive * k_vol_naive;
  const double numerator = T * (k_var - naive_sq);
  const double denominator = 8.0 + 2.0 * T * naive_sq;
  return k_vol_naive * (1.0 + numerator / denominator);
}

}  // namespace detail

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

// Reject a NaN wing-clamp band. 0 selects the certified default and any
// negative value disables the clamp, so NaN is the only unrepresentable input.
[[nodiscard]] bool wing_clamp_valid(const DerivConfig& cfg) noexcept {
  return !std::isnan(cfg.wing_clamp_k);
}

// FIT-C7 / Task C-6: structural (not inheritance-based) detection of a
// surface adapter that carries its own certified wing band -- `PricedSurface`-
// native and `SurfaceRef`-native callers thread one in via
// `PricedSurfaceStripView`/`SurfaceRefStripView` (see the wrapper functions
// below); the templated legacy containers (VolSurface/EssviSurface/
// SviSurface) have no such member and fall through to `std::nullopt`, i.e.
// "no provenance", with no per-type special-casing needed. A bumped-greek
// adapter (RespotView/VolShiftView) also has no such member -- `deriv_greeks`
// pins the CENTER's resolved band into `cfg.wing_clamp_k` for every bump
// instead (`pin_center_scheme`), so those never need to consult this.
template <class SurfaceT>
[[nodiscard]] std::optional<double> surface_certified_wing_band(const SurfaceT& surface) noexcept {
  if constexpr (requires {
                  { surface.certified_wing_band } -> std::convertible_to<std::optional<double>>;
                }) {
    return surface.certified_wing_band;
  } else {
    return std::nullopt;
  }
}

// Resolve the wing trust half-band: 0 -> the surface's own certified band
// when it carries one, else the mode-blind certified validation band; > 0 ->
// the caller's own band; < 0 -> 0.0 (clamp off). The <= 0 encoding of "off"
// lets every consumer test one condition (`band > 0.0`).
[[nodiscard]] double resolve_wing_clamp(const DerivConfig& cfg,
                                        std::optional<double> surface_band) noexcept {
  static_assert(strip::kCertifiedWingHalfBand == RiskSurfaceValidationConfig{}.k_max,
                "the strip's default wing trust band must equal the band the fit "
                "pipeline actually validates (risk_surface_validation.hpp)");
  static_assert(certified_wing_half_band(FitQualityMode::Balanced) == strip::kCertifiedWingHalfBand,
                "surface_policy's mode-keyed certified band must agree with the strip's own "
                "mode-blind default at Balanced quality");
  if (cfg.wing_clamp_k == 0.0) {
    // FIT-C7: a Latency/Accuracy-mode surface certifies a NARROWER/WIDER band
    // than this mode-blind default -- trusting 0.5 for a surface only
    // certified to 0.35 is exactly the defect this branch exists to close.
    if (surface_band.has_value() && *surface_band > 0.0) {
      return *surface_band;
    }
    return strip::kCertifiedWingHalfBand;
  }
  return cfg.wing_clamp_k > 0.0 ? cfg.wing_clamp_k : 0.0;
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

// Carry the strip's resolved grid onto a product quote. Every dispatch path
// that runs a strip owes its caller this, because reproducing a quote's exact
// quadrature (deriv_greeks' bump pinning) is only possible if the grid travels
// with the quote. A default-constructed `strip` carries NaN/0, which is exactly
// the "no strip ran" encoding, so this is safe to call unconditionally.
void carry_strip_grid(DerivQuote& out, const DerivQuote& strip) noexcept {
  out.strip_k_lo_used = strip.strip_k_lo_used;
  out.strip_k_hi_used = strip.strip_k_hi_used;
  out.strip_nodes_used = strip.strip_nodes_used;
  out.resolved_wing_clamp = strip.resolved_wing_clamp;
}

// Broadie-Jain (2008) / Buhler discrete-monitoring diffusion-drift addend for
// the future implied-variance leg. Per-fixing E[r_i^2] = kvar_fut*dt +
// mu^2*dt^2 (mu = r_bar - q_bar - kvar_fut/2, dt = T_resid/n_remaining);
// summing n_remaining fixings and annualizing leaves kvar_fut's own term
// exactly recovered plus this ADDITIVE leading-order piece -- NOT the
// multiplicative (1 + 1/n) the code applied before this task (~100x too
// large at index vols, and keyed off the contract's n_obs_total instead of
// the future leg's own n_remaining; see task-C-1-report.md). The residual
// O(1/n) JUMP term (Broadie-Jain sec 4) is NOT covered here -- LIT-3: jumps
// need the FullMc engine (reserved). Magnitude: a fraction of a variance
// point for a daily-monitored (n ~ 252) contract at typical rate/carry
// differentials.
[[nodiscard]] double discrete_monitoring_addend(double kvar_fut, double T_resid,
                                                std::uint32_t n_remaining,
                                                double r_minus_q) noexcept {
  assert(n_remaining >= 1u && "discrete_monitoring_addend: n_remaining must be >= 1");
  const double mu = r_minus_q - 0.5 * kvar_fut;
  return (T_resid / static_cast<double>(n_remaining)) * mu * mu;
}

// Continuously-compounded carry differential r_bar - q_bar at T: ln(F/S)/T,
// read from the same CurveSet the strip already resolves F from. F itself is
// guaranteed > 0 here -- every call site only reaches this after its own
// var_swap_fair_strike call at the SAME T already validated
// resolve_forward(curves, T) > 0 -- so only curves.spot needs a fresh check.
//
// @return InvalidArgument if curves.spot <= 0.
[[nodiscard]] Result<double> resolve_carry_diff(const CurveSet& curves, double T) {
  if (!(curves.spot > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "discrete correction needs curves.spot > 0");
  }
  const double f = resolve_forward(curves, T);
  return Ok(std::log(f / curves.spot) / T);
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

    // Discrete-monitoring correction (Broadie-Jain 2008 diffusion term,
    // leading order in 1/n_remaining): applies to the future implied-variance
    // leg only, keyed off the FUTURE leg's own remaining fixing count.
    if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN &&
        rv.n_obs_total >= 1u) {
      ATX_TRY(const double r_minus_q, resolve_carry_diff(curves, T));
      const std::uint32_t n_remaining = rv.n_obs_total - rv.n_obs_done;
      k_var_future_dec += discrete_monitoring_addend(k_var_future_dec, T, n_remaining, r_minus_q);
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
  carry_strip_grid(out, strip_quote);
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

  // Task C-5: refined form calibrates against the Remark 6.4/6.5 convexity
  // refinement instead of the naive ATMF-straddle number -- k_var_future is
  // ALREADY the strip's own K_var (every caller of resolve_vol_of_vol runs
  // the strip first), so this costs nothing extra here, unlike the
  // standalone vol_swap_fair_strike entry.
  //
  // DIRECTION (verified against this function's own closed form below, NOT
  // the task brief's paraphrase -- see task-C-5-report.md): xi solves
  // s^2 = -8*ln(ratio), ratio = k_vol/sqrt(k_var_future), which is STRICTLY
  // DECREASING in k_vol for fixed k_var_future. Refined k_vol >= naive k_vol
  // under positive convexity (K_var > k_vol_naive^2), so ratio GROWS and xi
  // SHRINKS under Refined -- less inferred dispersion is needed to explain a
  // SMALLER Jensen gap once the K_vol input is less biased. Cap options are
  // vega-positive, so cap_option_value_dec shrinks too. The brief's "richer
  // caps" framing does not hold against this formula; a caller relying on
  // Refined to CHEAPEN naive's caps (not enrich them) has the right mental
  // model.
  const double k_vol =
      cfg.carr_lee_form == CarrLeeForm::Refined
          ? detail::refine_carr_lee_k_vol(k_vol_cl, k_var_future, T)
          : k_vol_cl;

  const double sqrt_k_var = std::sqrt(k_var_future);
  const double ratio = k_vol / sqrt_k_var;
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

// Mid-life vol-swap distribution model (Task 6): E[sqrt(a + b*W)] for the
// blended variance V = a + b*W (see file header model), W lognormal at the
// strip's own mean m (residual maturity_t, Diffusion1OverN-corrected when
// configured) and log-stdev xi*sqrt(T). sqrt(a+b*w) is SMOOTH in w (a, b >=
// 0), unlike the capped pricers' kinked payoffs above, so plain
// Gauss-Hermite (detail::lognormal_expect) is the right tool -- no
// split-domain quadrature needed.
//
// Two callers share this: the true mid-life blend (a = w_done*rv_done_dec,
// b = w_future) and, when the caller explicitly asks for RvDistributionProxy
// on an unaged contract, the degenerate a = 0 / b = 1 case -- "the
// distribution engine end to end" per the brief is this same formula with no
// accrued leg, not a separate code path.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_vol_swap_distribution(
    const SurfaceT& surface, const CurveSet& curves, const DerivContract& contract,
    const DerivConfig& cfg, double a, double b) {
  const double T = contract.maturity_t;
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "vol swap distribution model needs T > 0");
  }
  ATX_TRY(auto sq, var_swap_fair_strike(surface, curves, T, cfg));
  const RealizedVarianceSpec& rv = contract.rv_spec;

  // xi auto-calibration resolves against the UNCORRECTED strip mean (PV-8):
  // resolve_vol_of_vol's "reproduces Carr-Lee exactly" contract must survive
  // the discrete-monitoring correction mode, so xi is resolved BEFORE the
  // correction below ever touches the mean.
  const double m_uncorrected = sq.fair_strike_dec;
  ATX_TRY(const VolOfVol vv, resolve_vol_of_vol(surface, curves, T, m_uncorrected, cfg));

  // Same Diffusion1OverN correction as price_var_swap / the capped pricers,
  // applied to the mean actually fed to the distribution model below (never
  // to xi's calibration input above) -- see price_capped_var_swap's comment
  // for why this has to match exactly.
  double m = m_uncorrected;
  DerivFlags flags = DerivFlags::None;
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN &&
      rv.n_obs_total >= 1u) {
    ATX_TRY(const double r_minus_q, resolve_carry_diff(curves, T));
    const std::uint32_t n_remaining = rv.n_obs_total - rv.n_obs_done;
    m += discrete_monitoring_addend(m, T, n_remaining, r_minus_q);
    flags |= DerivFlags::DiscreteCorrApplied;
  }

  const double s = vv.xi * std::sqrt(T);
  const double e_sqrt_v =
      detail::lognormal_expect(m, s, [a, b](double w) { return std::sqrt(a + b * w); });

  flags |= DerivFlags::ModelProxy | sq.flags;
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (vv.calibrated) {
    flags |= DerivFlags::VolOfVolCalibrated;
  }
  const double df = deriv_df_at_T(curves, T, flags);

  DerivQuote out{};
  out.fair_strike_dec = e_sqrt_v;
  out.fair_strike_points = 1.0e2 * e_sqrt_v;
  out.pv = df * contract.notional * (e_sqrt_v - contract.strike_dec);
  out.undiscounted_expectation_dec = e_sqrt_v;
  out.uncapped_var_dec = a + b * m;
  out.accrued_component_dec = a;
  out.future_component_dec = b * m;
  out.convexity_adjustment_dec = std::sqrt(a + b * m) - e_sqrt_v;
  out.integration_error_est = sq.integration_error_est;
  carry_strip_grid(out, sq);
  out.vol_of_vol_used = vv.xi;
  out.flags = flags;
  return Ok(out);
}

// Vol-swap dispatch across the three age regimes.
//   FULLY AGED (n_done >= n_total > 0): exact, sqrt(rv_done_dec), no model --
//     unaffected by cfg.engine (an explicit RvDistributionProxy here keeps
//     this same branch; the model has nothing left to add).
//   UNAGED (n_done == 0): Carr-Lee (engine Auto or explicit VolCarrLee;
//     Marquee pins this at inception) UNLESS the caller explicitly asks for
//     RvDistributionProxy, which runs the distribution model end to end
//     (a = 0, b = 1 -- see price_vol_swap_distribution above).
//   MID-LIFE (0 < n_done < n_total): always the distribution model (Auto or
//     RvDistributionProxy). Carr-Lee has no way to blend an already-accrued
//     leg, so an explicit VolCarrLee here is InvalidArgument rather than
//     silently pricing the wrong thing.
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

  // Discrete-monitoring FULL_MC is rejected up-front (reserved engine),
  // mirroring price_var_swap and the capped pricers -- even though the
  // fully-aged and Carr-Lee-unaged paths below never apply the correction, a
  // reserved-engine request fails the same way regardless of aging state.
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::FullMc) {
    return Err(ErrorCode::NotImplemented, "FULL_MC discrete correction reserved");
  }

  if (!fully_aged && !unaged && cfg.engine == DerivEngine::VolCarrLee) {
    return Err(ErrorCode::InvalidArgument,
               "Carr-Lee cannot blend accrued realized variance");
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

  if (unaged && cfg.engine != DerivEngine::RvDistributionProxy) {
    // Unaged vol-swap pricing (Carr-Lee).
    if (!(T > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "unaged vol swap needs T > 0");
    }

    ATX_TRY(auto vol_q, vol_swap_fair_strike(surface, curves, T, cfg));

    const double k_vol = vol_q.fair_strike_dec;
    DerivFlags flags = DerivFlags::VolCarrLee;
    // Review fix I-2 (C-5): propagate the strip's own provenance flags
    // (StripTruncatedLeft/Right, WingClamped, LowT, InteriorBadNodes) onto
    // this public dispatch quote. Under Naive this is a no-op --
    // vol_q.flags == VolCarrLee exactly, so the OR changes nothing -- but
    // under Refined, vol_swap_fair_strike now runs a strip that FEEDS the
    // price, and its provenance previously vanished at this boundary: a
    // caller gating on StripTruncated*/WingClamped/LowT (the pattern this
    // file establishes everywhere else a strip runs) would pass a quote
    // built on a truncated or wing-clamped strip with no trace of it.
    flags |= vol_q.flags;
    const double df = deriv_df_at_T(curves, T, flags);
    const double pv = df * contract.notional * (k_vol - contract.strike_dec);

    DerivQuote out{};
    out.fair_strike_dec = k_vol;
    out.fair_strike_points = 1.0e2 * k_vol;
    out.pv = pv;
    out.undiscounted_expectation_dec = k_vol;
    out.integration_error_est = kNaN;  // carry-forward fix: NaN, not 0.0, unless the strip below runs

    // Best-effort variance strip to populate the convexity diagnostic; do not
    // fail the price call if the strip is unavailable.
    if (const Result<DerivQuote> strip = var_swap_fair_strike(surface, curves, T, cfg);
        strip.has_value()) {
      out.uncapped_var_dec = strip->uncapped_var_dec;
      out.convexity_adjustment_dec =
          std::sqrt(std::fmax(strip->uncapped_var_dec, 0.0)) - k_vol;
      out.integration_error_est = strip->integration_error_est;
      carry_strip_grid(out, *strip);
    }
    out.accrued_component_dec = 0.0;
    out.future_component_dec = k_vol;
    out.flags = flags;
    return Ok(out);
  }

  // Distribution model (Task 6): the true mid-life blend, or an explicit
  // RvDistributionProxy pricing an unaged contract end to end (a = 0, b = 1).
  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }
  return price_vol_swap_distribution(surface, curves, contract, cfg,
                                     w_done * rv.rv_done_dec, w_future);
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

  // xi auto-calibration resolves against the UNCORRECTED strip mean (PV-8):
  // resolve_vol_of_vol's "reproduces Carr-Lee exactly" contract must survive
  // the discrete-monitoring correction mode. The corrected mean below is
  // still both the blend's future leg AND what the lognormal model actually
  // integrates, so a plain VarSwap and a CappedVarSwap on the same underlying
  // still see the same future variance leg under this config (CapParityHolds)
  // -- it is only xi's calibration input that must stay uncorrected.
  const double m_uncorrected = sq.fair_strike_dec;
  ATX_TRY(const VolOfVol vv, resolve_vol_of_vol(surface, curves, T, m_uncorrected, cfg));

  // Discrete-monitoring correction (Broadie-Jain 2008, leading order in
  // 1/n_remaining) -- same formula and flag as price_var_swap's, applied to
  // the mean fed to the blend and the lognormal model below.
  double m = m_uncorrected;
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN &&
      rv.n_obs_total >= 1u) {
    ATX_TRY(const double r_minus_q, resolve_carry_diff(curves, T));
    const std::uint32_t n_remaining = rv.n_obs_total - rv.n_obs_done;
    m += discrete_monitoring_addend(m, T, n_remaining, r_minus_q);
    flags |= DerivFlags::DiscreteCorrApplied;
  }

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
  carry_strip_grid(out, sq);
  out.vol_of_vol_used = vv.xi;
  return Ok(out);
}

// Assembles a DerivQuote for price_capped_vol_swap's three exit paths
// (pinned, fully-aged deterministic, model-based split-domain quadrature).
// Mirrors capped_var_swap_quote's bookkeeping but in VOL units:
// fair_strike_points uses the 1e2 vol-points scale (not 1e4 var-points), and
// accrued_component_dec / future_component_dec deliberately stay
// VARIANCE-space diagnostics (a and b*m, not their square roots) -- the brief
// specifies these as the blended-variance decomposition even though the
// strike itself is vol-space.
[[nodiscard]] DerivQuote capped_vol_swap_quote(double expectation_dec, double accrued_dec,
                                               double future_dec, double cap_option_dec,
                                               double df, const DerivContract& contract,
                                               DerivFlags flags) noexcept {
  DerivQuote out{};
  out.fair_strike_dec = expectation_dec;  // E[min(sqrt V,c)]: strike pricing to PV = 0
  out.fair_strike_points = 1.0e2 * expectation_dec;  // vol points, NOT var points
  out.pv = df * contract.notional * (expectation_dec - contract.strike_dec);
  out.undiscounted_expectation_dec = expectation_dec;
  out.accrued_component_dec = accrued_dec;
  out.future_component_dec = future_dec;
  out.cap_option_value_dec = cap_option_dec;
  out.flags = flags;
  return out;
}

// Capped volatility swap: E[min(sqrt(V), c)] for the blended variance
// V = a + b*W (see file header / Task 4 for the a/b/W model), c =
// contract.cap_dec a decimal VOL cap, C = c^2 its variance-units image.
//
// Raw Gauss-Hermite on min(sqrt V, c) is NOT used: the payoff is kinked in W
// (established by Task 2's rv_lognormal.hpp header note -- GH loses spectral
// accuracy past a kink), so the domain is split instead at the kink's
// standard-normal coordinate z* solving a + b*w* = C:
//   z* = (ln(w*/m) + s^2/2) / s
// Below the kink sqrt(a+b*W) is smooth and integrated by
// lognormal_truncated_expect (GL-64); above it the payoff is the constant c
// and the tail probability closes analytically via 1 - Phi(z*). The
// degenerate s == 0 case (W collapses to a point mass at m) is handled before
// the quadrature call -- lognormal_truncated_expect asserts s > 0.
//
// Exit paths, in order (mirrors price_capped_var_swap):
//   1. PIN: a = w_done*rv_done_dec >= C -> deterministic df*N*(c-K).
//   2. FULLY AGED (not pinned): min(sqrt(V),C) collapses to sqrt(rv_done_dec)
//      exactly (w_future == 0, no future leg to model).
//   3. Otherwise: strip for K_var_future, resolve vol-of-vol, split-domain
//      quadrature.
//
// Precondition (enforced by deriv_price before this is ever called):
// contract.cap_dec > 0.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote> price_capped_vol_swap(const SurfaceT& surface,
                                                        const CurveSet& curves,
                                                        const DerivContract& contract,
                                                        const DerivConfig& cfg) {
  assert(contract.cap_dec > 0.0 && "capped vol swap: cap_dec validated by the caller");
  const RealizedVarianceSpec& rv = contract.rv_spec;
  const double T = contract.maturity_t;
  const double c = contract.cap_dec;
  const double cap_var = c * c;  // C: the vol cap's variance-units image

  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::FullMc) {
    return Err(ErrorCode::NotImplemented, "FULL_MC discrete correction reserved");
  }

  double w_done = 0.0;
  double w_future = 1.0;
  if (rv.n_obs_total > 0u) {
    w_done = static_cast<double>(rv.n_obs_done) / static_cast<double>(rv.n_obs_total);
    w_future = 1.0 - w_done;
  }
  const double a = w_done * rv.rv_done_dec;

  DerivFlags flags = DerivFlags::None;
  if (rv.n_obs_done > 0u) {
    flags |= DerivFlags::Aged;
  }
  if (rv.n_obs_total > 0u && rv.n_obs_done >= rv.n_obs_total) {
    flags |= DerivFlags::FullyAged;
  }

  if (a >= cap_var) {
    flags |= DerivFlags::CapPinned | DerivFlags::CapApplied;
    const double df = deriv_df_at_T(curves, T, flags);
    return Ok(capped_vol_swap_quote(c, a, 0.0, 0.0, df, contract, flags));
  }

  if (has_flag(flags, DerivFlags::FullyAged)) {
    // a < C here (the pin check above already handled a >= C): min(V,C)
    // collapses to the realized leg, no model needed.
    const double r1 = std::sqrt(std::fmax(a, 0.0));
    const double df = deriv_df_at_T(curves, T, flags);
    return Ok(capped_vol_swap_quote(r1, a, 0.0, 0.0, df, contract, flags));
  }

  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "capped vol swap needs T > 0 to price the future leg");
  }
  ATX_TRY(auto sq, var_swap_fair_strike(surface, curves, T, cfg));

  // Same xi-before-correction ordering (PV-8) and the same Diffusion1OverN
  // correction as price_capped_var_swap / price_var_swap -- see
  // price_capped_var_swap's comment for why both have to match exactly.
  const double m_uncorrected = sq.fair_strike_dec;
  ATX_TRY(const VolOfVol vv, resolve_vol_of_vol(surface, curves, T, m_uncorrected, cfg));

  double m = m_uncorrected;
  if (cfg.discrete_correction_mode == DerivDiscreteCorrection::Diffusion1OverN &&
      rv.n_obs_total >= 1u) {
    ATX_TRY(const double r_minus_q, resolve_carry_diff(curves, T));
    const std::uint32_t n_remaining = rv.n_obs_total - rv.n_obs_done;
    m += discrete_monitoring_addend(m, T, n_remaining, r_minus_q);
    flags |= DerivFlags::DiscreteCorrApplied;
  }

  const double s = vv.xi * std::sqrt(T);
  const double b = w_future;  // > 0: not fully aged (checked above)
  const auto sqrt_v = [a, b](double w) noexcept { return std::sqrt(a + b * w); };

  // {E[min(sqrt V,c)], E[sqrt V] - E[min(sqrt V,c)]}. s <= 0: W collapses to
  // a point mass at m, no quadrature, no kink to split. s > 0: split-domain
  // quadrature at the kink z* solving a + b*w* = C.
  const auto [expectation, cap_option] = [&]() -> std::pair<double, double> {
    if (s <= 0.0) {
      const double sqrt_v_mean = sqrt_v(m);
      const double capped = std::fmin(sqrt_v_mean, c);
      return {capped, sqrt_v_mean - capped};
    }
    const double w_star = (cap_var - a) / b;  // > 0: a < C (checked above)
    const double z_star = (std::log(w_star / m) + 0.5 * s * s) / s;
    const double lower = detail::lognormal_truncated_expect(m, s, -8.0, z_star, sqrt_v);
    const double tail_prob = 1.0 - detail::norm_cdf(z_star);
    const double capped = lower + c * tail_prob;
    const double uncapped_sqrt = detail::lognormal_truncated_expect(m, s, -8.0, 8.0, sqrt_v);
    return {capped, uncapped_sqrt - capped};
  }();

  flags |= DerivFlags::ModelProxy | DerivFlags::CapApplied | sq.flags;
  if (vv.calibrated) {
    flags |= DerivFlags::VolOfVolCalibrated;
  }
  const double df = deriv_df_at_T(curves, T, flags);

  DerivQuote out =
      capped_vol_swap_quote(expectation, a, b * m, cap_option, df, contract, flags);
  out.uncapped_var_dec = sq.uncapped_var_dec;
  out.integration_error_est = sq.integration_error_est;
  carry_strip_grid(out, sq);
  out.vol_of_vol_used = vv.xi;
  out.convexity_adjustment_dec = sqrt_v(m) - expectation;
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
  if (!wing_clamp_valid(cfg)) {
    return Err(ErrorCode::InvalidArgument, "wing_clamp_k must not be NaN");
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

  // Wing trust band for the surface READS (see DerivConfig::wing_clamp_k): a
  // node beyond the band prices at its true strike under the BAND-EDGE vol —
  // flat-vol tails over the uncertified extrapolation region, never a
  // truncated span. band <= 0 means the clamp is off. Resolved BEFORE the
  // resolution floor below, which has to know how many panels the C-3 split
  // will cut — and that depends on where this band falls inside the span.
  const double wing_band = resolve_wing_clamp(cfg, surface_certified_wing_band(surface));
  const bool wing_clamped =
      wing_band > 0.0 && (grid.k_min_log < -wing_band || grid.k_max_log > wing_band);

  // C-2 / PV-2: the MIRROR rule. The rescale above only widens the span for a
  // high-vol/long-dated tenor; a short-tenor/low-vol quote can still resolve
  // too coarsely even at (or below) the tier's own floor span, because the
  // tier grids are sized for a roughly-1Y reference vol scale, not a 1-day
  // one. Enforce dk <= sigma_atm*sqrt(T)/4 by raising the node count, same
  // 4m+1 rounding as above (`strip::dk_floor_nodes`). `sigma_atm` is the same
  // ATM vol read the span rescale above already resolved -- no second read.
  // `cfg.strip_nodes` pinned is never overridden here, same as the span
  // rescale: a pinned node count is a caller request and gets flagged
  // (LowT) instead of silently changed.
  //
  // The ceiling binds the spacing the strip ACTUALLY integrates on, which
  // after C-3 is per-panel, not one uniform dk — hence the panel count.
  const double dk_max = strip::dk_ceiling(sigma_atm, T);
  const double resolved_span = grid.k_max_log - grid.k_min_log;
  const std::size_t n_panels =
      strip::strip_panel_count(grid.k_min_log, grid.k_max_log, wing_band);
  bool low_t = false;
  if (cfg.strip_nodes == 0u) {
    const std::size_t raised =
        strip::dk_floor_nodes(resolved_span, grid.n_nodes, dk_max, n_panels);
    if (raised != grid.n_nodes) {
      grid.n_nodes = raised;
      low_t = true;
    }
  }

  const std::size_t n = grid.n_nodes;

  // C-3 / LIT-10: the integrand is piecewise smooth, not smooth — it kinks at
  // k = 0 (put-call parity) and at ±wing_band when the clamp binds. Split the
  // composite Simpson at every interior kink so each one is a PANEL BOUNDARY
  // for any span, symmetric or not, and the O(h⁴) law (and with it the
  // Richardson estimate below) holds on every panel. See `plan_strip_split`
  // for the budget apportionment and its degradation ladder; the total node
  // count and the reported span are unchanged by the split.
  assert(n >= 3u && "composite Simpson needs at least one panel of 3 nodes");
  const strip::StripSplit split =
      strip::plan_strip_split(grid.k_min_log, grid.k_max_log, n, wing_band);

  // LowT, decided on the grid actually integrated. For the unpinned path the
  // floor above has already provisioned every panel under the ceiling, so this
  // only ever confirms it; for a caller-pinned node count — never overridden —
  // it is the whole job of the flag, and checking the widest PANEL rather than
  // the nominal span/(n-1) is what makes the verdict honest after C-3.
  if (dk_max > 0.0 && strip::max_panel_spacing(split) > dk_max) {
    low_t = true;
  }

  // Richardson half-grid quadrature error estimate. Valid only when EVERY
  // panel is 4m+1, so each panel's half grid ((n+1)/2 nodes, every other node)
  // is itself an odd count, a valid composite-Simpson grid, and — decisively —
  // still has the kinks on its own boundaries. The FIX-E M-7 rounding, the C-2
  // resolution floor and the tier defaults all keep n on the 4m+1 lattice, and
  // the split keeps every panel there; a caller-pinned `strip_nodes` is a
  // request and is not rounded, so it may leave `halvable` false.
  const bool halvable = split.richardson_ok;

  // Composite Simpson on   integral OTM(K) / (df * F * e^x) dx
  //                      == integral OTM(K) / (df * K) dx   since K = F * e^x.
  // Nodes with a non-finite / non-positive surface IV contribute zero; a bad
  // node touching either integration boundary flips the truncation flag.
  //
  // The strip's one surface read. Returns the normalized integrand at log-
  // moneyness x, and whether the surface's IV there was unusable.
  const auto integrand_at = [&](double x) {
    const double K = F * std::exp(x);
    const Side side = (x < 0.0) ? Side::Put : Side::Call;
    const double x_read = wing_band > 0.0 ? std::clamp(x, -wing_band, wing_band) : x;
    const double sigma = surface.iv(x_read, T);
    const bool bad = !std::isfinite(sigma) || sigma <= 0.0;
    const double price = bad ? 0.0 : black76_price(F, K, T, sigma, df, side);
    return std::pair<double, bool>{price / (df * K), bad};
  };

  double integral = 0.0;
  double integral_half = 0.0;
  bool bad_first = false;
  bool bad_last = false;
  // PV-4: nodes strictly inside the grid whose surface read was non-finite/
  // non-positive. bad_first/bad_last (the two grid ENDPOINTS) are tracked
  // separately, unchanged, below -- they drive StripTruncatedLeft/Right, a
  // COVERAGE signal. An interior bad node is a different failure (a hole in
  // the middle of an otherwise-usable surface), counted here and consumed
  // after the loop.
  std::size_t interior_bad_count = 0;
  double shared = 0.0;  // integrand at the node the previous panel ended on
  for (std::size_t p = 0; p < split.count; ++p) {
    const strip::StripPanel& panel = split.panels[p];
    const std::size_t np = panel.n_nodes;
    const std::size_t np_half = (np + 1) / 2;
    const double dx = (panel.k_hi - panel.k_lo) / static_cast<double>(np - 1);
    double sum = 0.0;
    double sum_half = 0.0;
    for (std::size_t i = 0; i < np; ++i) {
      // A panel's first node IS the previous panel's last node, and carries
      // the same value: reusing it is what keeps the split at exactly one
      // iv() read per DISTINCT node, as the un-split single pass was.
      double integrand = shared;
      if (p == 0 || i != 0) {
        // Panel ends are the kink abscissae verbatim rather than k_lo + i*dx,
        // so no rounding step can drift a kink off the node it must sit on.
        const double x = (i == 0)        ? panel.k_lo
                         : (i + 1 == np) ? panel.k_hi
                                         : panel.k_lo + dx * static_cast<double>(i);
        const auto [value, bad] = integrand_at(x);
        integrand = value;
        // A panel-boundary kink (e.g. k = 0) is a node strictly inside the
        // WHOLE grid even though it sits at the edge of ITS panel -- only
        // the true ends of the entire split (p == 0's first node, the last
        // panel's last node) are the grid's own boundary.
        const bool is_grid_first = (p == 0 && i == 0);
        const bool is_grid_last = (p + 1 == split.count && i + 1 == np);
        if (is_grid_first) {
          bad_first = bad;
        } else if (is_grid_last) {
          bad_last = bad;
        } else if (bad) {
          ++interior_bad_count;
        }
      }
      sum += simpson_w(i, np) * integrand;
      if (halvable && (i % 2u) == 0u) {
        // Every other node of this panel, quadratured on its own half-density
        // grid (spacing 2*dx) with that grid's own Simpson weights. The panel
        // boundaries — and so the kinks — are boundaries of THAT grid too,
        // which is what makes the /15 difference an error estimate.
        sum_half += simpson_w(i / 2u, np_half) * integrand;
      }
      if (i + 1 == np) {
        shared = integrand;
      }
    }
    integral += sum * (dx / 3.0);
    integral_half += sum_half * (2.0 * dx / 3.0);
  }

  // Review fix round 1 (Critical): interior-bad-node accounting is gated on
  // the strip's own ENDPOINTS, not on a fresh ATM read. `sigma_atm` above
  // reads the SAME (k_log=0.0, T) point the k = 0 panel-boundary kink node
  // reads inside the loop just run -- k = 0 is a forced, distinct grid node
  // whenever k_lo < 0 < k_hi (`strip_panel_bounds`), true of virtually
  // every real call. Gating on sigma_atm's finiteness could therefore never
  // tell "the surface is unusable everywhere" (T under the legacy_surface
  // short-T extrapolation guard, `T < 0.5*T0`, where EVERY node including
  // both true endpoints reads non-finite) apart from "the one bad interior
  // node happens to sit at ATM" -- exactly the case PV-4's finding names
  // explicitly ("including the k = 0 put-call-parity kink"), silently
  // reproducing the pre-fix bug for it. `bad_first`/`bad_last` (the two
  // TRUE grid endpoints, already computed by the loop above) carry the
  // right signal instead: both true only in the wholesale-unusable case --
  // an interior node's badness, wherever it sits, cannot set either.
  const bool strip_wholly_unusable = bad_first && bad_last;

  // PV-4: a strip whose middle is mostly holes is broken, not merely sparse
  // -- refuse before spending any more work computing a number that would be
  // built mostly from the bad-node zero substitution. `max(2, n/100)` gives
  // small grids a fixed floor (two isolated gaps stays a quote) and scales
  // with node count on large ones, matching the brief's own budget. Skipped
  // entirely when the strip is wholly unusable (see above) -- that is the
  // pre-existing, deliberately-tolerated degenerate corner, not this task's
  // target.
  if (!strip_wholly_unusable && interior_bad_count > std::max<std::size_t>(2, n / 100u)) {
    return Err(ErrorCode::Internal, "variance strip has too many interior bad nodes");
  }

  const double k_var = (2.0 / T) * integral;

  // Composite-Simpson error is O(h^4): halving h (doubling the node density)
  // shrinks it ~16x, so the difference between the two estimates is ~15/16 of
  // the coarse grid's own error — a self-contained error bound with no
  // external reference. Summing the SIGNED per-panel differences (rather than
  // their magnitudes) keeps this exact: in the h⁴ limit the sum equals the
  // total error, cancellation between panels included. Stays NaN (not 0) when
  // a panel is not 4m+1: that is a caller-pinned exact node count, and NaN
  // says "not estimated" rather than claiming a zero error nothing checked.
  double err_est = kNaN;
  if (halvable) {
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
  if (wing_clamped) {
    flags |= DerivFlags::WingClamped;
  }
  if (low_t) {
    flags |= DerivFlags::LowT;
  }
  if (!strip_wholly_unusable && interior_bad_count > 0u) {
    flags |= DerivFlags::InteriorBadNodes;
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
  // The grid this quote was actually integrated on, so a caller (deriv_greeks)
  // can pin it back and reproduce this exact quadrature.
  out.strip_k_lo_used = grid.k_min_log;
  out.strip_k_hi_used = grid.k_max_log;
  out.strip_nodes_used = static_cast<std::uint32_t>(n);
  // The band actually resolved above (FIT-C7 / Task C-6) -- carried the same
  // way as the grid fields it sits beside, so a caller can inspect exactly
  // which trust band this quote's reads were clamped to.
  out.resolved_wing_clamp = wing_band;
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
  if (!wing_clamp_valid(cfg)) {
    return Err(ErrorCode::InvalidArgument, "wing_clamp_k must not be NaN");
  }

  // K_vol ~= sqrt(2 pi / T) * C_ATMF / (F * df) — shared with
  // resolve_vol_of_vol's auto-calibration path so the two never drift.
  ATX_TRY(const double k_vol_naive, carr_lee_k_vol(surface, curves, T));

  DerivQuote out{};
  out.pv = 0.0;
  out.flags = DerivFlags::VolCarrLee;

  // Task C-5: Refined form needs the strip's own K_var (Remark 6.4/6.5), so
  // unlike Naive this branch pays for one var_swap_fair_strike evaluation --
  // an opt-in cost, never paid by a Naive (default) caller. A strip failure
  // here propagates (ATX_TRY): the caller explicitly asked for the
  // strip-dependent form, so a surface the strip cannot integrate is this
  // call's failure too, not a silent fall-back to the naive number.
  if (cfg.carr_lee_form == CarrLeeForm::Refined) {
    ATX_TRY(auto strip, var_swap_fair_strike(surface, curves, T, cfg));
    const double k_vol =
        detail::refine_carr_lee_k_vol(k_vol_naive, strip.fair_strike_dec, T);
    out.fair_strike_dec = k_vol;
    out.fair_strike_points = 1.0e2 * k_vol;
    out.undiscounted_expectation_dec = k_vol;
    out.uncapped_var_dec = strip.uncapped_var_dec;
    out.convexity_adjustment_dec =
        std::sqrt(std::fmax(strip.uncapped_var_dec, 0.0)) - k_vol;
    out.integration_error_est = strip.integration_error_est;
    carry_strip_grid(out, strip);
    out.flags |= strip.flags;
    return Ok(out);
  }

  out.fair_strike_dec = k_vol_naive;
  out.fair_strike_points = 1.0e2 * k_vol_naive;
  out.undiscounted_expectation_dec = k_vol_naive;
  out.uncapped_var_dec = 0.0;  // not computed in this entry
  out.convexity_adjustment_dec = 0.0;
  out.integration_error_est = kNaN;  // no strip runs here; NaN = not estimated
  return Ok(out);
}

// ── Unified dispatch ───────────────────────────────────────────────────────

template <class SurfaceT>
Result<DerivQuote> deriv_price(const SurfaceT& surface, const CurveSet& curves,
                               const DerivContract& contract,
                               const DerivConfig& cfg) {
  // Reserved engines fail clean before any work. RvDistributionProxy is the
  // one exception: Task 4 wires it up (alongside Auto) as the distribution
  // model's entry point for CappedVarSwap, Task 5 adds CappedVolSwap, and
  // Task 6 adds plain VolSwap (mid-life, and an unaged contract priced end to
  // end through the model instead of Carr-Lee) -- every other kind still sees
  // it as reserved.
  switch (cfg.engine) {
  case DerivEngine::RvDistributionProxy:
    if (contract.kind != DerivKind::CappedVarSwap && contract.kind != DerivKind::CappedVolSwap &&
        contract.kind != DerivKind::VolSwap) {
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

  // cap_dec validation applies uniformly to both capped kinds: a malformed
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

  // Kind x engine dispatch matrix (PV-5), enforced in two stages: the
  // reserved-engine switch above (RvDistributionAffine/McQe always
  // NotImplemented; RvDistributionProxy NotImplemented except on the kinds
  // it is wired up for) narrows cfg.engine to what each kind's own case below
  // can still misuse, and each case rejects the one engine value that
  // survives narrowing but still names no pricing formula for that kind.
  // Full matrix: VarSwap -> {Auto, StripLogContract}; VolSwap -> {Auto,
  // VolCarrLee (unaged only -- price_vol_swap itself checks that), Rv
  // DistributionProxy}; CappedVarSwap/CappedVolSwap -> {Auto,
  // RvDistributionProxy}. Everything else is InvalidArgument.
  switch (contract.kind) {
  case DerivKind::VarSwap:
    // Kind x engine matrix (PV-5): VarSwap only ever runs the strip --
    // price_var_swap never reads cfg.engine at all, so an explicit
    // VolCarrLee here used to silently price the strip anyway (VolCarrLee
    // has no variance-swap formula of its own to run instead). RvDistribution
    // Proxy/RvDistributionAffine/McQe on VarSwap are already NotImplemented
    // from the reserved-engine switch above; VolCarrLee is the one gap.
    if (cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price a var swap");
    }
    return price_var_swap(surface, curves, contract, cfg);
  case DerivKind::VolSwap:
    // Kind x engine matrix (PV-5): an explicit StripLogContract here used to
    // silently fall through to price_vol_swap's unaged Carr-Lee branch --
    // the same branch Auto/VolCarrLee take -- because that branch only tests
    // `cfg.engine != RvDistributionProxy`, not which engine it actually is.
    // StripLogContract has no vol-swap formula of its own to run instead.
    if (cfg.engine == DerivEngine::StripLogContract) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price a vol swap");
    }
    return price_vol_swap(surface, curves, contract, cfg);
  case DerivKind::CappedVarSwap:
    if (cfg.engine == DerivEngine::StripLogContract || cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price capped kinds");
    }
    return price_capped_var_swap(surface, curves, contract, cfg);
  case DerivKind::CappedVolSwap:
    if (cfg.engine == DerivEngine::StripLogContract || cfg.engine == DerivEngine::VolCarrLee) {
      return Err(ErrorCode::InvalidArgument, "engine cannot price capped kinds");
    }
    return price_capped_vol_swap(surface, curves, contract, cfg);
  }
  // Defends against an out-of-enum kind (matches the C default's ERR_INVALID).
  return Err(ErrorCode::InvalidArgument, "unknown derivative kind");
}

// ── Finite-difference greeks ───────────────────────────────────────────────

namespace {

// Sticky-strike respot view. The bumped curves move the forward by e^{k_shift},
// so reading the base surface at k + k_shift keeps the vol tied to the SAME
// absolute strike the bumped strip prices at.
template <class SurfaceT>
struct RespotView {
  const SurfaceT* base;  // non-owning, non-null
  double k_shift;

  [[nodiscard]] double iv(double k_log, double T) const noexcept {
    return base->iv(k_log + k_shift, T);
  }
};

// Parallel additive vol shift. Composed OVER a RespotView for the cross bumps,
// which is why it is a separate view rather than another field on that one.
template <class SurfaceT>
struct VolShiftView {
  const SurfaceT* base;  // non-owning, non-null
  double vol_shift;

  [[nodiscard]] double iv(double k_log, double T) const noexcept {
    return base->iv(k_log, T) + vol_shift;
  }
};

// Spot bump: the reference spot and every fitted forward scale together, so the
// bumped world is the same carry seen from a different spot. Yield and
// dividends are untouched.
[[nodiscard]] CurveSet respot_curves(const CurveSet& base, double scale) {
  CurveSet out = base;
  out.spot = base.spot * scale;
  // `out` is a fresh copy from the line above (ForwardCurve has no shared
  // storage -- Rule of Zero over its own std::vector), so this exclusively-
  // owned instance is the only reference in play: the non-const `points()`
  // in-place write handle (see its doc in rates_curve.hpp) cannot alias
  // `base` or any concurrent reader, satisfying the many-readers-or-one-
  // writer contract trivially.
  for (ForwardPoint& p : out.forward.points()) {
    p.F *= scale;
  }
  return out;
}

// Rate bump: rebuild the yield curve with every zero rate shifted by dr.
// Sampling at the forward pillars' Ts plus the contract's own T guarantees the
// contract tenor is an exact pillar of the rebuilt curve, so its discount
// factor is e^{-(r(T)+dr)T} exactly rather than an interpolant of one.
[[nodiscard]] Result<CurveSet> rate_shift_curves(const CurveSet& base, double dr, double T) {
  std::vector<double> ts;
  ts.reserve(base.forward.points().size() + 1u);
  for (const ForwardPoint& p : base.forward.points()) {
    if (p.T > 0.0) {
      ts.push_back(p.T);
    }
  }
  if (T > 0.0) {
    ts.push_back(T);
  }
  std::sort(ts.begin(), ts.end());
  ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
  if (ts.empty()) {
    // No positive tenor anywhere: there is no rate exposure to shift (the only
    // way here is T <= 0 with no forward pillars, where df == 1 by definition).
    return Ok(base);
  }

  std::vector<double> rates;
  rates.reserve(ts.size());
  for (const double t : ts) {
    rates.push_back(base.yield.zero(t) + dr);
  }
  CurveSet out = base;
  ATX_TRY_VOID(out.set_yield(ts, rates));
  return Ok(std::move(out));
}

// One bumped repricing, through the SAME deriv_price every mark goes through.
template <class SurfaceT>
[[nodiscard]] Result<double> bumped_pv(const SurfaceT& surface, const CurveSet& curves,
                                       const DerivContract& contract, const DerivConfig& cfg,
                                       double k_shift, double vol_shift) {
  const RespotView<SurfaceT> respot{&surface, k_shift};
  const VolShiftView<RespotView<SurfaceT>> view{&respot, vol_shift};
  ATX_TRY(const DerivQuote q, deriv_price(view, curves, contract, cfg));
  return Ok(q.pv);
}

// The repricings the stencils below difference. Members left at NaN are ones
// this bump set did not evaluate, and NaN then propagates into exactly the
// greeks that depend on them -- which is the "NaN = not computed" contract.
struct BumpPvs {
  double c = kNaN;                      // center
  double s_up = kNaN, s_dn = kNaN;      // S(1 +/- h)
  double v_up = kNaN, v_dn = kNaN;      // sigma +/- dv
  double r_up = kNaN;                   // r + dr
  double t_dn = kNaN;                   // T - dt
  double sv_pp = kNaN, sv_pm = kNaN;    // (S+, sigma+), (S+, sigma-)
  double sv_mp = kNaN, sv_mm = kNaN;    // (S-, sigma+), (S-, sigma-)
  double t_s_up = kNaN, t_s_dn = kNaN;  // S(1 +/- h) at T - dt
};

// Up to 8 evaluations, 14 with second_order (one fewer / three fewer when the
// contract cannot roll). Every failure propagates: a bumped contract that will
// not price is a real failure, not a missing greek.
//
// The center is repriced HERE, under the same pinned config as the bumps,
// rather than reusing the caller's center quote: a stencil must difference
// values from one consistent configuration, and the caller's center was priced
// before the grid and xi were pinned.
template <class SurfaceT>
[[nodiscard]] Result<BumpPvs> eval_bump_table(const SurfaceT& surface, const CurveSet& curves,
                                              const DerivContract& contract,
                                              const DerivConfig& cfg,
                                              const DerivGreekBumps& bumps) {
  const double h = bumps.spot_rel;
  const double dv = bumps.vol_abs;
  const double ks_up = std::log1p(h);
  const double ks_dn = std::log1p(-h);
  const CurveSet cs_up = respot_curves(curves, 1.0 + h);
  const CurveSet cs_dn = respot_curves(curves, 1.0 - h);

  // A roll landing at or past expiry has no future leg to price; theta/charm
  // stay NaN rather than failing the whole block (see the header).
  const bool can_roll = contract.maturity_t > bumps.time_years;
  DerivContract rolled = contract;
  rolled.maturity_t = contract.maturity_t - bumps.time_years;

  BumpPvs pv{};
  ATX_TRY(pv.c, bumped_pv(surface, curves, contract, cfg, 0.0, 0.0));
  ATX_TRY(pv.s_up, bumped_pv(surface, cs_up, contract, cfg, ks_up, 0.0));
  ATX_TRY(pv.s_dn, bumped_pv(surface, cs_dn, contract, cfg, ks_dn, 0.0));
  ATX_TRY(pv.v_up, bumped_pv(surface, curves, contract, cfg, 0.0, dv));
  ATX_TRY(pv.v_dn, bumped_pv(surface, curves, contract, cfg, 0.0, -dv));
  ATX_TRY(const CurveSet cs_r,
          rate_shift_curves(curves, bumps.rate_abs, contract.maturity_t));
  ATX_TRY(pv.r_up, bumped_pv(surface, cs_r, contract, cfg, 0.0, 0.0));
  if (can_roll) {
    ATX_TRY(pv.t_dn, bumped_pv(surface, curves, rolled, cfg, 0.0, 0.0));
  }

  if (bumps.second_order) {
    ATX_TRY(pv.sv_pp, bumped_pv(surface, cs_up, contract, cfg, ks_up, dv));
    ATX_TRY(pv.sv_pm, bumped_pv(surface, cs_up, contract, cfg, ks_up, -dv));
    ATX_TRY(pv.sv_mp, bumped_pv(surface, cs_dn, contract, cfg, ks_dn, dv));
    ATX_TRY(pv.sv_mm, bumped_pv(surface, cs_dn, contract, cfg, ks_dn, -dv));
    if (can_roll) {
      ATX_TRY(pv.t_s_up, bumped_pv(surface, cs_up, rolled, cfg, ks_up, 0.0));
      ATX_TRY(pv.t_s_dn, bumped_pv(surface, cs_dn, rolled, cfg, ks_dn, 0.0));
    }
  }
  return Ok(pv);
}

// Bump sizes are a caller input: validate once, at the boundary.
[[nodiscard]] bool bumps_valid(const DerivGreekBumps& b) noexcept {
  return b.spot_rel > 0.0 && b.spot_rel < 1.0 && b.vol_abs > 0.0 && b.rate_abs > 0.0 &&
         b.time_years > 0.0;
}

// Pin everything about the center quote that a bumped evaluation would
// otherwise re-derive for itself: the strip's grid, the resolved wing clamp,
// and the vol-of-vol. All three are resolved from the SURFACE, so all three
// drift when the surface is bumped, and would then contaminate the
// differences with a change in the numerical scheme rather than a change in
// the price.
[[nodiscard]] DerivConfig pin_center_scheme(const DerivConfig& cfg, const DerivQuote& center) noexcept {
  DerivConfig out = cfg;

  // Grid: a bumped surface can cross the adaptive rescale's ceil() boundary and
  // integrate on a different node count than the center, making the stencils
  // straddle a step discontinuity.
  if (center.strip_nodes_used > 0u && std::isfinite(center.strip_k_lo_used) &&
      std::isfinite(center.strip_k_hi_used) &&
      center.strip_k_lo_used < center.strip_k_hi_used) {
    out.k_min_log = center.strip_k_lo_used;
    out.k_max_log = center.strip_k_hi_used;
    out.strip_nodes = center.strip_nodes_used;
  }

  // Wing clamp (FIT-C7 / Task C-6): a bumped evaluation prices through
  // RespotView/VolShiftView, an adapter that carries no surface provenance of
  // its own -- left alone, `resolve_wing_clamp` would silently fall back to
  // the mode-blind default for every bump while the center resolved a
  // surface-carried band, straddling a DIFFERENT clamp than the value it is
  // differenced against. Only pin when the center itself consulted surface
  // provenance (`cfg.wing_clamp_k == 0.0`, the default-resolution branch) and
  // actually resolved a positive band (a strip ran); an explicit >0/<0
  // override on `cfg` already resolves identically for every bump with no
  // surface read at all, and pinning 0.0 here would wrongly turn "clamp
  // resolved off" into "explicit request for the certified band".
  if (cfg.wing_clamp_k == 0.0 && center.resolved_wing_clamp > 0.0) {
    out.wing_clamp_k = center.resolved_wing_clamp;
  }

  // Vol-of-vol: pin the calibrated xi so vega measures the model's response to
  // the vol shift, not the calibration re-fitting itself. A calibrated xi of
  // exactly 0 cannot be written back as 0 (that is the config's
  // "auto-calibrate" selector), so it is pinned as the smallest positive double
  // instead. Every consumer of xi reaches the same limit at a denormal as at
  // zero: `lognormal_expect`'s nodes collapse onto the mean, `lognormal_call`
  // resolves to max(m-k,0) through Phi(+-inf), and the capped-vol-swap's kink
  // coordinate z* overflows to +-inf, which `lognormal_truncated_expect` clamps
  // into its own [-8,8] domain. So this pins the VALUE without selecting the
  // auto path, which is exactly what is needed.
  if (std::isfinite(center.vol_of_vol_used)) {
    out.vol_of_vol = center.vol_of_vol_used > 0.0
                         ? center.vol_of_vol_used
                         : std::numeric_limits<double>::denorm_min();
  }
  return out;
}

}  // namespace

template <class SurfaceT>
Result<DerivGreeks> deriv_greeks(const SurfaceT& surface, const CurveSet& curves,
                                 const DerivContract& contract, const DerivConfig& cfg,
                                 const DerivGreekBumps& bumps) {
  if (!bumps_valid(bumps)) {
    return Err(ErrorCode::InvalidArgument, "greek bump sizes must be > 0 (spot_rel < 1)");
  }
  if (!(curves.spot > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "greeks need curves.spot > 0 (delta's divisor)");
  }

  ATX_TRY(const DerivQuote center, deriv_price(surface, curves, contract, cfg));
  DerivGreeks g{};
  g.pv = center.pv;
  g.quote = center;

  // Fully aged: nothing is left to realize, so PV is a fixed settlement amount
  // under a pure discount, PV(t) = e^{-r(T-t)}*X. Both time greeks are then
  // analytic and must agree with each other -- dPV/dr = -(T-t)*PV and
  // dPV/dt = +r*PV are the same statement differentiated two ways. No bumping.
  // At T == 0 the discount is gone and both collapse to 0 (YieldCurve::zero
  // returns 0 for T <= 0, so theta lands there without a special case).
  if (has_flag(center.flags, DerivFlags::FullyAged)) {
    g.rho = -contract.maturity_t * center.pv;
    g.theta = curves.yield.zero(contract.maturity_t) * center.pv;
    return Ok(g);
  }

  // Pin the center's numerical scheme (strip grid + calibrated xi) into every
  // bumped evaluation; see pin_center_scheme and the header.
  const DerivConfig cfg_pinned = pin_center_scheme(cfg, center);

  ATX_TRY(const BumpPvs p, eval_bump_table(surface, curves, contract, cfg_pinned, bumps));

  const double ds = bumps.spot_rel * curves.spot;  // absolute spot bump
  const double dv = bumps.vol_abs;
  g.delta = (p.s_up - p.s_dn) / (2.0 * ds);
  g.gamma = (p.s_up - 2.0 * p.c + p.s_dn) / (ds * ds);
  g.vega = (p.v_up - p.v_dn) / (2.0 * dv);
  g.volga = (p.v_up - 2.0 * p.c + p.v_dn) / (dv * dv);
  g.vanna = (p.sv_pp - p.sv_pm - p.sv_mp + p.sv_mm) / (4.0 * ds * dv);
  g.theta = (p.t_dn - p.c) / bumps.time_years;
  g.rho = (p.r_up - p.c) / bumps.rate_abs;
  // charm = d(delta)/dt on the SAME calendar-time convention as theta above:
  // one day of calendar time is one day of maturity gone.
  const double delta_rolled = (p.t_s_up - p.t_s_dn) / (2.0 * ds);
  g.charm = (delta_rolled - g.delta) / bumps.time_years;
  return Ok(g);
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

Status RealizedTracker::observe_dated(std::int64_t ts_ns, double spot) {
  // Ordering validated FIRST and unconditionally: a stale/replayed ts_ns
  // mutates nothing, even when observe(spot) would itself have rejected the
  // spot (e.g. non-positive) -- the caller learns "not ascending", not a
  // spot-validation error that implies the timestamp was otherwise fine.
  if (ts_ns <= last_fixing_ts_ns_) {
    return Err(ErrorCode::AlreadyExists, "fixing timestamp not ascending");
  }
  ATX_TRY_VOID(observe(spot));
  last_fixing_ts_ns_ = ts_ns;
  return Ok();
}

// ── Explicit instantiations (mirrors surface.cpp) ──────────────────────────
//
// SUPPORTED SET (v1 ruling, closeout item 1.2). `SurfaceT`'s whole requirement
// is `iv(k_log, T)`, so the set is a linkage decision, not a modelling one.
// Three entries, in the order a caller should reach for them:
//
//   1. `VolSurface` — the TIER-A calibration-grade surface container, and the
//      only entry a Tier-A caller can name without reaching into `detail/`.
//      This is what makes the templated overloads usable from the frozen API
//      at all: before it, every instantiation was on a demoted type, so the
//      declarations in `derivatives.hpp` were reachable only by including a
//      `detail/` header — a Tier-A signature you could not link against.
//   2. The two per-family containers demoted to `detail/legacy_surface.hpp` by
//      S4-T21. Kept ONLY for source compatibility with callers that predate
//      the demotion (in-tree: the deriv unit tests and their shared fixture).
//      Not un-demoted by appearing here — a `detail/` type reached through a
//      Tier-A function template is still a `detail/` type, and the tier
//      manifest is what says so.
//   3. Neither of the above, for the modern fitted pipeline: it produces a
//      `PricedSurface` / `SurfaceRef`, which do NOT go through this list.
//      Their entry points are the non-templated `PricedSurface`-native
//      overloads below and `detail::deriv_price_on_ref`; both instantiate
//      their own file-local log-moneyness adapters inside THIS translation
//      unit, so they need nothing here.
//
// A caller with some other `SurfaceT` adds an instantiation beside these. New
// code should not need to: it should hold a `PricedSurface` or a `SurfaceRef`.

template Result<DerivQuote> var_swap_fair_strike<VolSurface>(
    const VolSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> vol_swap_fair_strike<VolSurface>(
    const VolSurface&, const CurveSet&, double, const DerivConfig&);
template Result<DerivQuote> deriv_price<VolSurface>(
    const VolSurface&, const CurveSet&, const DerivContract&, const DerivConfig&);
template Result<DerivGreeks> deriv_greeks<VolSurface>(
    const VolSurface&, const CurveSet&, const DerivContract&, const DerivConfig&,
    const DerivGreekBumps&);

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
template Result<DerivGreeks> deriv_greeks<EssviSurface>(
    const EssviSurface&, const CurveSet&, const DerivContract&, const DerivConfig&,
    const DerivGreekBumps&);
template Result<DerivGreeks> deriv_greeks<SviSurface>(
    const SviSurface&, const CurveSet&, const DerivContract&, const DerivConfig&,
    const DerivGreekBumps&);

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
  // FIT-C7 / Task C-6: the caller-supplied certified band, if any --
  // `resolve_wing_clamp` reads this via `surface_certified_wing_band`'s
  // structural detection. `std::nullopt` (no band supplied) is
  // indistinguishable from "this adapter carries no provenance", which is
  // exactly the mode-blind-default behaviour a caller who does not know (or
  // does not care about) the surface's build quality mode should keep.
  std::optional<double> certified_wing_band;

  PricedSurfaceStripView(const PricedSurface* surface, const CurveSet* cs, double T,
                         std::optional<double> band = std::nullopt) noexcept
      : ps{surface}, curves{cs}, T_cached{T}, F_cached{resolve_forward(*cs, T)},
        certified_wing_band{band} {}

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
                                        const DerivConfig& cfg,
                                        std::optional<double> surface_certified_wing_band) {
  ATX_TRY(const CurveSet curves, carry_from(surface, T));
  const PricedSurfaceStripView view{&surface, &curves, T, surface_certified_wing_band};
  return var_swap_fair_strike(view, curves, T, cfg);
}

Result<DerivQuote> vol_swap_fair_strike(const PricedSurface& surface, double T,
                                        const DerivConfig& cfg,
                                        std::optional<double> surface_certified_wing_band) {
  ATX_TRY(const CurveSet curves, carry_from(surface, T));
  const PricedSurfaceStripView view{&surface, &curves, T, surface_certified_wing_band};
  return vol_swap_fair_strike(view, curves, T, cfg);
}

Result<DerivQuote> deriv_price(const PricedSurface& surface, const DerivContract& contract,
                               const DerivConfig& cfg,
                               std::optional<double> surface_certified_wing_band) {
  ATX_TRY(const CurveSet curves, carry_from(surface, contract.maturity_t));
  const PricedSurfaceStripView view{&surface, &curves, contract.maturity_t,
                                    surface_certified_wing_band};
  return deriv_price(view, curves, contract, cfg);
}

Result<DerivGreeks> deriv_greeks(const PricedSurface& surface, const DerivContract& contract,
                                 const DerivConfig& cfg, const DerivGreekBumps& bumps,
                                 std::optional<double> surface_certified_wing_band) {
  // The fitted-range gate is paid ONCE here, on the contract's own maturity;
  // the theta roll below reuses this same carry with a shorter contract T (see
  // the header) rather than re-deriving it at T - dt.
  ATX_TRY(const CurveSet curves, carry_from(surface, contract.maturity_t));
  const PricedSurfaceStripView view{&surface, &curves, contract.maturity_t,
                                    surface_certified_wing_band};
  return deriv_greeks(view, curves, contract, cfg, bumps);
}

// ── Task 9 / DerivBook: SurfaceRef-native bridge ───────────────────────────
//
// See include/atx/vol/detail/deriv_ref_bridge.hpp for why these two functions
// live in THIS translation unit: they instantiate the strip templates over a
// third surface adapter, and the template bodies are here.

namespace detail {
namespace {

// Lower yield pillar as a fraction of the contract tenor. TWO pillars carrying
// the SAME zero rate make the curve genuinely flat in RATE: with exactly two
// pillars the Fritsch-Carlson tangents both equal the secant, so the Hermite
// interpolant of log(df) is exactly the straight line -r*t between them and
// `disc(t) == e^{-r*t}` for every t in [frac*T, T].
//
// A SINGLE pillar would not do this. `YieldCurve` extrapolates log(df) FLAT
// outside its pillar range, so a one-pillar curve returns the SAME discount
// factor e^{-r*T} at every tenor -- a flat DISCOUNT, not a flat yield. The mark
// at T is identical either way (T is a pillar in both), but `deriv_greeks`'
// theta stencil reprices at T - dt, and under a frozen discount that repricing
// silently drops theta's r*PV discount-roll term. The second pillar costs one
// vector element and makes the roll exact.
//
// 1e-3 puts the floor pillar far enough below any roll the greek stencil takes
// that `disc` interpolates rather than clamps. The one exception is a contract
// with T in (dt, ~1.001*dt) -- i.e. within a tenth of a percent of the roll size
// itself -- where T - dt falls under the floor and df clamps at e^{-r*1e-3*T}.
// On a contract expiring in about a day that differs from the exact
// e^{-r*(T-dt)} by well under 1e-6: theta is then microscopically off, never
// dropped, and the term it exists to capture is itself ~0 there.
constexpr double kFlatYieldFloorFrac = 1.0e-3;

// The borrowed surface's OWN carry, expressed as a CurveSet so the strip
// resolves forward and discount exactly as it does on every other path.
//
// `roll_dt` is the theta roll the caller is about to take (0 when it takes
// none). It exists because the curve must carry a forward pillar at the ROLLED
// tenor as well. `resolve_forward` clamps outside the pillar range, so with a
// lone pillar at T a repricing at T - dt would read F(T) while the surface's
// smile stays anchored at its own F(T - dt): the strip's k = 0 would then land
// at k = ln(F(T)/F(T-dt)) = (r - q)*dt ON THE SMILE instead of at its ATM point.
// On a skewed name that MIS-CENTERING biases K_var by about
// 2*sigma*(dsigma/dk)*(r-q)*dt, which theta promptly divides by dt -- a
// first-order error in theta, comparable to and opposing the discount-roll term.
// Two pillars make the rolled repricing read the surface's own forward at its
// own residual tenor, which is exactly what the multi-pillar E6 `carry_from`
// gives the PricedSurface path.
//
// (An earlier revision justified the lone pillar by claiming it was needed to
// keep the adapter's vol read and the strip's strike on the same K. That was
// WRONG: both resolve F through this same CurveSet, so they agree at ANY pillar
// count -- which is precisely why the E6 path has always been free to carry
// every fitted pillar.)
//
// UNLIKE the E6 `carry_from`, no fitted-range gate is applied. Not because it is
// impossible -- an owned handle could reach `owned()->context()` -- but because a
// view-backed `PricedSurfaceView` exposes no pillar list, so gating would make
// the two `SurfaceRef` forms behave DIFFERENTLY on the same surface. Uniform
// behaviour is chosen instead, and the resulting tenor-hygiene obligation is
// documented on the caller-facing API (deriv_book.hpp).
[[nodiscard]] Result<CurveSet> carry_from_ref(const SurfaceRef& ref, double T, double roll_dt) {
  CurveSet cs;
  cs.spot = ref.pricing().S;

  // Ascending in T -- `resolve_forward`'s documented precondition. A roll that
  // would land at or before the valuation date contributes no pillar; the
  // greek stencil skips theta/charm on exactly that condition (`can_roll`), and
  // the at-expiry path never reads a forward at all.
  ForwardPoint pts[2];
  std::size_t n_pts = 0;
  const double t_rolled = T - roll_dt;
  if (roll_dt > 0.0 && t_rolled > 0.0) {
    pts[n_pts].T = t_rolled;
    pts[n_pts].F = ref.forward_at(t_rolled);
    pts[n_pts].q_eff = ref.q_eff_at(t_rolled);
    ++n_pts;
  }
  pts[n_pts].T = T;
  pts[n_pts].F = ref.forward_at(T);
  pts[n_pts].q_eff = ref.q_eff_at(T);
  ++n_pts;
  cs.forward.set(std::span<const ForwardPoint>{pts, n_pts});

  // T <= 0 is the at-expiry case: `deriv_df_at_T` short-circuits df = 1 there
  // and never consults the curve, so a default (empty) YieldCurve -- which
  // itself returns 1.0 -- is the correct and only representable answer. Feeding
  // a non-positive pillar to `set_yield` would be a fabricated rate.
  if (T > 0.0) {
    const double r = ref.rate_at(T);
    const double ts[] = {T * kFlatYieldFloorFrac, T};
    const double rates[] = {r, r};
    ATX_TRY_VOID(cs.set_yield(ts, rates));
  }
  return Ok(std::move(cs));
}

// Presents a SurfaceRef through the LOG-MONEYNESS `iv(k_log, T)` contract the
// strip templates require. Mirrors `PricedSurfaceStripView` above -- including
// the reason it resolves F from the CurveSet rather than from the surface: the
// vol must be read at the strike the strip is pricing, and the strip's own
// forward is what makes that true by construction. Same one-tenor cache, for
// the same reason (the strip calls `iv` once per node, 97-2049 times).
struct SurfaceRefStripView {
  const SurfaceRef* ref; // non-owning, non-null, valid()
  const CurveSet* curves;
  double T_cached;
  double F_cached;
  // FIT-C7 / Task C-6: mirrors `PricedSurfaceStripView::certified_wing_band`
  // above -- see that member's comment.
  std::optional<double> certified_wing_band;

  SurfaceRefStripView(const SurfaceRef* handle, const CurveSet* cs, double T,
                      std::optional<double> band = std::nullopt) noexcept
      : ref{handle}, curves{cs}, T_cached{T}, F_cached{resolve_forward(*cs, T)},
        certified_wing_band{band} {}

  [[nodiscard]] double iv(double k_log, double T) const noexcept {
    const double F = (T == T_cached) ? F_cached : resolve_forward(*curves, T);
    if (!(F > 0.0) || !std::isfinite(k_log)) {
      return kNaN;
    }
    return ref->iv(F * std::exp(k_log), T);
  }
};

}  // namespace

Result<DerivQuote> deriv_price_on_ref(const SurfaceRef& ref, const DerivContract& contract,
                                      const DerivConfig& cfg,
                                      std::optional<double> surface_certified_wing_band) {
  if (!ref.valid()) {
    return Err(ErrorCode::InvalidArgument, "deriv: null surface handle");
  }
  // No roll happens in pricing, so the carry needs no rolled forward pillar.
  ATX_TRY(const CurveSet curves, carry_from_ref(ref, contract.maturity_t, 0.0));
  const SurfaceRefStripView view{&ref, &curves, contract.maturity_t, surface_certified_wing_band};
  return deriv_price(view, curves, contract, cfg);
}

Result<DerivGreeks> deriv_greeks_on_ref(const SurfaceRef& ref, const DerivContract& contract,
                                        const DerivConfig& cfg, const DerivGreekBumps& bumps,
                                        std::optional<double> surface_certified_wing_band) {
  if (!ref.valid()) {
    return Err(ErrorCode::InvalidArgument, "deriv: null surface handle");
  }
  // The carry snapshot is taken ONCE, at the contract's own maturity, and every
  // bumped evaluation reuses it, so a stencil never differences two
  // differently-derived carries. It is told the roll up front so it can carry a
  // forward pillar at the rolled tenor too -- see `carry_from_ref`.
  //
  // NOTE for a future cross rate x time stencil: `rate_shift_curves` rebuilds
  // the yield curve from the FORWARD pillars' tenors plus the contract's own, so
  // it inherits this curve's rolled pillar and stays flat in rate. When there is
  // no roll (T <= dt) it rebuilds a single pillar -- a flat-DISCOUNT curve -- which
  // is harmless today because the rate bump prices the UNROLLED T only. A stencil
  // that ever reprices at both r + dr and T - dt must re-establish the second
  // pillar there.
  ATX_TRY(const CurveSet curves, carry_from_ref(ref, contract.maturity_t, bumps.time_years));
  const SurfaceRefStripView view{&ref, &curves, contract.maturity_t, surface_certified_wing_band};
  return deriv_greeks(view, curves, contract, cfg, bumps);
}

}  // namespace detail

}  // namespace atx::vol
