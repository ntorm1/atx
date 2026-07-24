// Aggregators — the full single-surface analytic bundle, the session/fitted/
// fitter convenience overloads, the earnings implied-move solver, and the
// two-surface change bundle. Orchestrates the primitives and density TUs
// (see analytics.hpp). Implemented last (depends on the other TUs' public fns).

#include "atx/vol/analytics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/event_vol.hpp"      // count_events_at, implied_emove_joint
#include "atx/vol/priced_surface.hpp" // PricedSurface, PricingContext
#include "atx/vol/pricer_fitter.hpp"  // PricerFitter, FittedSurface
#include "atx/vol/session.hpp"        // VolaSession, SessionInputs, SessionDiagnostics
#include "atx/vol/surface_parity.hpp" // SliceContext

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Unwrap a `Result<double>` to its value or NaN — lets a per-tenor bundle absorb
// an unreachable wing / degenerate strip without failing the whole aggregate, and
// lets downstream arithmetic propagate the NaN into the derived field.
[[nodiscard]] double value_or_nan(const Result<double> &r) noexcept {
  return r.has_value() ? *r : kNaN;
}
} // namespace

Result<EmoveSolution> earnings_implied_move_ex(const PricedSurface &ps, const EventContext &ctx) {
  if (ctx.schedule == nullptr) {
    return Err(ErrorCode::InvalidArgument, "no event schedule");
  }
  const std::int64_t now = ps.pricing().now_ts_ns;
  const std::span<const SliceContext> pillars = ps.context();

  // E3a / AN-P1-3. This used to scan for the FIRST adjacent pair whose event
  // count rose and hand that single bracket to `implied_emove` — a two-pillar
  // solve that forces one flat censored variance across the bracket, so the
  // censored term structure inside it aliased straight into eMove² (the sweep's
  // AAPL case: +173% when no near expiry spans the event). Now EVERY fitted
  // pillar is offered to `implied_emove_joint`, which runs the identified
  // {eMove, st, lt, decay} fit when the pillar set supports it and falls back to
  // exactly the old two-pillar bracket when it does not.
  std::vector<CensorObsInput> obs;
  obs.reserve(pillars.size());
  for (const SliceContext &p : pillars) {
    CensorObsInput o;
    o.T = p.T;
    o.w_dirty = ps.total_variance(ps.forward_at(p.T), p.T);
    o.n = count_events_at(*ctx.schedule, now, p.T);
    obs.push_back(o);
  }
  return implied_emove_joint(obs);
}

Result<double> earnings_implied_move(const PricedSurface &ps, const EventContext &ctx) {
  ATX_TRY(const EmoveSolution sol, earnings_implied_move_ex(ps, ctx));
  return Ok(sol.emove);
}

Result<SurfaceAnalytics> compute_surface_analytics(const PricedSurface &ps,
                                                   const AnalyticsConfig &cfg,
                                                   const EventContext *ctx) {
  SurfaceAnalytics out;
  out.uid = ps.uid();
  out.as_of_ts_ns = ps.pricing().now_ts_ns;
  out.spot = ps.pricing().S;
  out.implied_emove = (ctx != nullptr && ctx->schedule != nullptr) ? ctx->implied_emove : 0.0;

  const std::vector<double> &tenors = cfg.tenors.tenors_years;
  const std::vector<std::string> &labels = cfg.tenors.labels;

  // Fitted pillar range: the served CurveSurface flat-extrapolates a parametric
  // smile beyond [Tmin, Tmax], so atmf_vol is finite but FABRICATED there. Gate
  // each tenor to the fitted range and mark out-of-range tenors extrapolated /
  // invalid (see analytics.hpp "Error / NaN semantics").
  const std::span<const SliceContext> pillars = ps.context();
  const bool have_range = !pillars.empty();
  const double Tmin = have_range ? pillars.front().T : kNaN;
  const double Tmax = have_range ? pillars.back().T : kNaN;

  // Risk-neutral densities computed ONCE up front over a shared strike grid; their
  // BKM moments and var-swap vol are copied back onto any tenor that lines up
  // (within ~1 day) so no tenor rebuilds a density grid it already has.
  if (cfg.compute_rnd) {
    for (const double Tr : cfg.rnd_tenors_years) {
      auto d = risk_neutral_density(ps, Tr, cfg.rnd);
      if (d.has_value()) {
        out.densities.push_back(std::move(*d));
      }
    }
  }

  const double k = cfg.skew_k_ref;

  for (std::size_t i = 0; i < tenors.size(); ++i) {
    const double T = tenors[i];
    TenorAnalytics t;
    t.tenor_years = T;
    if (i < labels.size()) {
      t.label = labels[i];
    }

    if (!have_range || !(T >= Tmin && T <= Tmax)) {
      t.extrapolated = true; // flat-extrapolated smile: finite but not a fitted value
      t.valid = false;
      out.tenors.push_back(std::move(t));
      continue;
    }

    // In-range: resolve the forward and ATM vol ONCE and reuse them everywhere.
    const double F = ps.forward_at(T);
    const double atm = ps.iv(F, T);
    t.forward = F;
    t.df = std::exp(-ps.rate_at(T) * T);
    t.atm_vol = atm;

    // Earnings-stripped ATM + event-variance share.
    std::size_t n_ev = 0;
    double emove = 0.0;
    if (ctx != nullptr && ctx->schedule != nullptr) {
      n_ev = count_events_at(*ctx->schedule, ps.pricing().now_ts_ns, T);
      emove = ctx->implied_emove;
      t.n_earnings = static_cast<int>(n_ev);
      t.atm_vol_ex_earn = cfg.ex_earnings ? atmf_vol_ex_earnings(ps, T, *ctx) : kNaN;
    } else {
      t.atm_vol_ex_earn = kNaN;
      t.n_earnings = 0;
    }
    t.event_var_share = (ctx != nullptr && ctx->schedule != nullptr && emove > 0.0 && atm > 0.0)
                            ? (static_cast<double>(n_ev) * emove * emove) / (atm * atm * T)
                            : 0.0;

    // Delta wings: a wing strike can be unreachable in the far tail — store NaN
    // for that entry rather than failing the whole bundle.
    for (const double d : cfg.delta_points) {
      const double pv = value_or_nan(vol_at_delta(ps, T, Side::Put, d));
      const double cv = value_or_nan(vol_at_delta(ps, T, Side::Call, d));
      t.put_delta_vol.push_back(pv);
      t.call_delta_vol.push_back(cv);
      t.risk_reversal.push_back(pv - cv);
      t.butterfly.push_back(0.5 * (pv + cv) - atm);
    }

    // Inline skew/curvature reusing F and atm (avoids skew_curvature's redundant
    // forward_at + iv(F,T)). Leave slope/curvature 0 if a pivot is non-finite.
    const double sp = ps.iv(F * std::exp(k), T);
    const double sm = ps.iv(F * std::exp(-k), T);
    if (std::isfinite(sp) && std::isfinite(sm)) {
      t.skew_slope = (sp - sm) / (2.0 * k);
      t.curvature = (sp + sm - 2.0 * atm) / (k * k);
    }
    t.skew_slope_sqrt_t = t.skew_slope * std::sqrt(T);
    t.skew_slope_norm = (atm > 0.0) ? t.skew_slope / atm : kNaN;

    // Fixed-moneyness vols (K = F·m), reusing F. Capture the 0.90 / 1.10 entries
    // for skew_90_110 if the config carries them; else compute them directly.
    double v90 = kNaN;
    double v110 = kNaN;
    bool have90 = false;
    bool have110 = false;
    for (const double m : cfg.moneyness_points) {
      const double mv = ps.iv(F * m, T);
      t.moneyness_vol.push_back(mv);
      if (m == 0.90) {
        v90 = mv;
        have90 = true;
      } else if (m == 1.10) {
        v110 = mv;
        have110 = true;
      }
    }
    t.skew_90_110 = (have90 && have110)
                        ? (v90 - v110)
                        : (vol_at_moneyness(ps, T, 0.90) - vol_at_moneyness(ps, T, 1.10));

    // Var-swap vol / density copy-back: reuse a matching precomputed density's
    // var_swap_vol + BKM moments (no second grid build); else compute var-swap.
    const RiskNeutralDensity *match = nullptr;
    for (const RiskNeutralDensity &d : out.densities) {
      if (std::fabs(t.tenor_years - d.T) < 1.5 / 365.25) {
        match = &d;
        break;
      }
    }
    if (match != nullptr) {
      t.var_swap_vol = match->var_swap_vol;
      t.rnd_skewness = match->bkm_skew;
      t.rnd_kurtosis = match->bkm_kurt;
      t.prob_below_forward = match->prob_below_forward;
    } else {
      t.var_swap_vol = cfg.compute_varswap ? value_or_nan(var_swap_vol(ps, T, cfg.rnd)) : kNaN;
    }
    t.convexity_premium = t.var_swap_vol - atm;
    t.expected_move = cfg.straddle_move_multiplier * atm * std::sqrt(T);
    t.valid = true;
    out.tenors.push_back(std::move(t));
  }

  // Forward (calendar) vol between CONSECUTIVE in-range valid tenors (ascending),
  // one segment per adjacent pair.
  std::vector<double> in_range_T;
  in_range_T.reserve(out.tenors.size());
  for (const TenorAnalytics &t : out.tenors) {
    if (t.valid) {
      in_range_T.push_back(t.tenor_years);
    }
  }
  for (std::size_t i = 0; i + 1 < in_range_T.size(); ++i) {
    out.forward_vol_segments.push_back(forward_vol(ps, in_range_T[i], in_range_T[i + 1]));
  }

  // Term-structure summary (constant-maturity ATMF read straight off the surface,
  // same ACT/365.25 basis as the tenor grid). A leg outside the fitted range is
  // NaN; leave the slope fields 0 on a NaN, the ratio NaN.
  const auto ts_vol = [&](double X) {
    return (have_range && X >= Tmin && X <= Tmax) ? atmf_vol(ps, X) : kNaN;
  };
  const double sig1m = ts_vol(30.0 / 365.25);
  const double sig3m = ts_vol(91.0 / 365.25);
  const double sig1y = ts_vol(365.0 / 365.25);
  const double slope_1m_3m = sig3m - sig1m;
  const double slope_3m_1y = sig1y - sig3m;
  if (std::isfinite(slope_1m_3m)) {
    out.ts_slope_1m_3m = slope_1m_3m;
  }
  if (std::isfinite(slope_3m_1y)) {
    out.ts_slope_3m_1y = slope_3m_1y;
  }
  out.ts_ratio_1m_3m =
      (std::isfinite(sig1m) && std::isfinite(sig3m) && sig3m != 0.0) ? sig1m / sig3m : kNaN;

  const TenorAnalytics *first_valid = nullptr;
  const TenorAnalytics *last_valid = nullptr;
  for (const TenorAnalytics &t : out.tenors) {
    if (t.valid) {
      if (first_valid == nullptr) {
        first_valid = &t;
      }
      last_valid = &t;
    }
  }
  if (first_valid != nullptr && last_valid != nullptr) {
    out.backwardation = first_valid->atm_vol > last_valid->atm_vol;
  }

  out.valid = first_valid != nullptr;
  return Ok(std::move(out));
}

Result<SurfaceAnalytics> compute_surface_analytics(const VolaSession &session,
                                                   const AnalyticsConfig &cfg) {
  ATX_TRY(auto ps, session.to_priced_surface());
  EventContext ec;
  ec.schedule = session.inputs().events.get();
  ec.implied_emove = session.diagnostics().implied_emove;
  // The schedule shared_ptr lives on `session`, which outlives this call.
  return compute_surface_analytics(ps, cfg, ec.schedule != nullptr ? &ec : nullptr);
}

Result<SurfaceAnalytics> compute_surface_analytics(const FittedSurface &fitted,
                                                   const AnalyticsConfig &cfg) {
  return compute_surface_analytics(fitted.session(), cfg);
}

Result<SurfaceAnalytics> compute_surface_analytics(const PricerFitter &fitter,
                                                   const AnalyticsConfig &cfg) {
  const FittedSurface *fs = fitter.surface();
  if (fs == nullptr) {
    return Err(ErrorCode::InvalidArgument, "fitter has no fitted surface");
  }
  return compute_surface_analytics(*fs, cfg);
}

Result<SurfaceDiff> compute_surface_diff(const PricedSurface &a, const PricedSurface &b,
                                         const AnalyticsConfig &cfg) {
  if (a.uid() != b.uid()) {
    return Err(ErrorCode::InvalidArgument, "surface uid mismatch");
  }

  SurfaceDiff out;
  out.ts1_ns = a.pricing().now_ts_ns;
  out.ts2_ns = b.pricing().now_ts_ns;
  out.spot1 = a.pricing().S;
  out.spot2 = b.pricing().S;
  out.d_spot = out.spot2 - out.spot1;
  out.log_return = (out.spot1 > 0.0 && out.spot2 > 0.0) ? std::log(out.spot2 / out.spot1) : kNaN;

  const std::vector<double> &tenors = cfg.tenors.tenors_years;
  const std::vector<std::string> &labels = cfg.tenors.labels;

  // Gate tenors to the INTERSECTION of both surfaces' fitted pillar ranges — a
  // change is only a fitted number where BOTH sides are in-range (neither
  // flat-extrapolates).
  const std::span<const SliceContext> pa_ctx = a.context();
  const std::span<const SliceContext> pb_ctx = b.context();
  const bool have_range = !pa_ctx.empty() && !pb_ctx.empty();
  const double Tmin = have_range ? std::max(pa_ctx.front().T, pb_ctx.front().T) : kNaN;
  const double Tmax = have_range ? std::min(pa_ctx.back().T, pb_ctx.back().T) : kNaN;

  double t_first_valid = kNaN;
  double skew_a_first = kNaN; // stashed at the first valid tenor (no recompute)
  double d_atm_first = kNaN;
  bool any_valid = false;

  for (std::size_t i = 0; i < tenors.size(); ++i) {
    const double T = tenors[i];
    TenorDiff td;
    td.tenor_years = T;
    if (i < labels.size()) {
      td.label = labels[i];
    }

    if (!have_range || !(T >= Tmin && T <= Tmax)) {
      td.valid = false; // out of at least one surface's fitted range
      out.tenors.push_back(std::move(td));
      continue;
    }

    const double va = atmf_vol(a, T);
    const double vb = atmf_vol(b, T);
    td.d_forward = atmf_forward(b, T) - atmf_forward(a, T);
    td.d_atm_vol = vb - va;

    const double K0 = atmf_forward(a, T); // t1's ATM strike (sticky-strike)
    td.d_vol_fixed_strike = b.iv(K0, T) - a.iv(K0, T);

    // Resolve each 25Δ wing ONCE (4 root-finds, not 10) and derive the fixed-delta
    // change, risk-reversal change, and butterfly change from those four vols.
    // Any wing NaN propagates through the arithmetic into the derived field.
    const double pa = value_or_nan(vol_at_delta(a, T, Side::Put, 0.25));
    const double ca = value_or_nan(vol_at_delta(a, T, Side::Call, 0.25));
    const double pb = value_or_nan(vol_at_delta(b, T, Side::Put, 0.25));
    const double cb = value_or_nan(vol_at_delta(b, T, Side::Call, 0.25));
    td.d_vol_fixed_delta = pb - pa;
    td.d_risk_reversal_25 = (pb - cb) - (pa - ca);
    td.d_butterfly_25 = (0.5 * (pb + cb) - vb) - (0.5 * (pa + ca) - va);

    const double skew_a = skew_curvature(a, T, cfg.skew_k_ref).skew_slope;
    const double skew_b = skew_curvature(b, T, cfg.skew_k_ref).skew_slope;
    td.d_skew_slope = skew_b - skew_a;

    td.valid = true;
    if (!any_valid) {
      any_valid = true;
      t_first_valid = T;
      skew_a_first = skew_a;
      d_atm_first = td.d_atm_vol;
    }
    out.tenors.push_back(std::move(td));
  }

  // Sticky decomposition off the FIRST valid tenor. The sticky-STRIKE regime
  // tracks the FORWARD (not spot), so the predicted ATM move is 𝒮·ln(F2/F1); the
  // residual is the observed ATM move minus that prediction.
  if (any_valid && std::isfinite(t_first_valid)) {
    const double fwd_ret = std::log(b.forward_at(t_first_valid) / a.forward_at(t_first_valid));
    out.sticky_strike_atm_pred =
        (std::isfinite(skew_a_first) && std::isfinite(fwd_ret)) ? skew_a_first * fwd_ret : 0.0;
    out.sticky_delta_atm_pred = 0.0;
    out.residual_atm_move = d_atm_first - out.sticky_strike_atm_pred;
  }

  out.valid = any_valid;
  return Ok(std::move(out));
}

} // namespace atx::vol
