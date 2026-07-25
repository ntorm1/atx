#include "atx/vol/derivatives.hpp"

#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <utility>

#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/black76.hpp"
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
  const double sigma_atm = surface.iv(0.0, T);
  const double required = strip::required_half_width(sigma_atm, T, strip::kDefaultWidthSigmas);
  if (!span_pinned) {
    const double floor_half = std::fmax(-grid.k_min_log, grid.k_max_log);
    const double kh =
        strip::adaptive_half_width(floor_half, sigma_atm, T, strip::kDefaultWidthSigmas);
    if (kh > 0.0) {
      grid.k_min_log = -kh;
      grid.k_max_log = kh;
    }
  }

  const std::size_t n = grid.n_nodes;
  const double dx = (grid.k_max_log - grid.k_min_log) / static_cast<double>(n - 1);

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
  }
  integral *= (dx / 3.0);

  const double k_var = (2.0 / T) * integral;

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
  out.integration_error_est = kNaN;  // NaN = not estimated (see header)
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

  // Black-76 ATMF call, then the Carr-Lee fair vol
  //   K_vol ~= sqrt(2 pi / T) * C_ATMF / (F * df).
  const double c_atmf = black76_price(F, F, T, sigma_atmf, df, Side::Call);
  const double k_vol =
      std::sqrt(2.0 * std::numbers::pi / T) * c_atmf / (F * df);

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
  // Reserved engines fail clean before any work.
  switch (cfg.engine) {
  case DerivEngine::RvDistributionProxy:
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
  if (contract.kind == DerivKind::VarSwap && contract.cap_dec != 0.0) {
    return Err(ErrorCode::NotImplemented, "cap_dec reserved on var swap");
  }

  switch (contract.kind) {
  case DerivKind::VarSwap:
    return price_var_swap(surface, curves, contract, cfg);
  case DerivKind::VolSwap:
    return price_vol_swap(surface, curves, contract, cfg);
  case DerivKind::CappedVarSwap:
  case DerivKind::CappedVolSwap:
    return Err(ErrorCode::NotImplemented, "capped variants reserved");
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
  // FITTED-RANGE GATE. Between pillars this CurveSet reproduces the surface's own
  // carry; OUTSIDE them it does not, and the disagreement is not benign.
  // `resolve_forward` clamps flat past the end pillars, whereas
  // `PricedSurface::forward_at` keeps extrapolating economically
  // (S·exp((r−q_eff)·T)). The strip prices every node at F·e^x and reads its vol
  // from `ps.iv(F·e^x, T)`, so a forward that is not the surface's own would put
  // k = 0 somewhere other than the surface's ATM and bias K_var — silently.
  // Refuse instead. A caller who genuinely wants an extrapolated tenor supplies
  // its own `CurveSet` through the templated overload and owns that choice.
  if (!(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "deriv: T must be > 0");
  }
  if (T < pillars.front().T || T > pillars.back().T) {
    return Err(ErrorCode::OutOfRange,
               "deriv: T is outside the surface's fitted pillar range; the "
               "PricedSurface overloads do not extrapolate carry");
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
struct PricedSurfaceStripView {
  const PricedSurface* ps;
  const CurveSet* curves;

  [[nodiscard]] double iv(double k_log, double T) const noexcept {
    const double F = resolve_forward(*curves, T);
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
  const PricedSurfaceStripView view{&surface, &curves};
  return var_swap_fair_strike(view, curves, T, cfg);
}

Result<DerivQuote> vol_swap_fair_strike(const PricedSurface& surface, double T,
                                        const DerivConfig& cfg) {
  ATX_TRY(const CurveSet curves, carry_from(surface, T));
  const PricedSurfaceStripView view{&surface, &curves};
  return vol_swap_fair_strike(view, curves, T, cfg);
}

Result<DerivQuote> deriv_price(const PricedSurface& surface, const DerivContract& contract,
                               const DerivConfig& cfg) {
  ATX_TRY(const CurveSet curves, carry_from(surface, contract.maturity_t));
  const PricedSurfaceStripView view{&surface, &curves};
  return deriv_price(view, curves, contract, cfg);
}

}  // namespace atx::vol
