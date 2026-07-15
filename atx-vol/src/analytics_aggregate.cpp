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
#include "atx/vol/event_vol.hpp"      // count_events_at, implied_emove
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
} // namespace

Result<double> earnings_implied_move(const PricedSurface &ps, const EventContext &ctx) {
  if (ctx.schedule == nullptr) {
    return Err(ErrorCode::InvalidArgument, "no event schedule");
  }
  const std::int64_t now = ps.pricing().now_ts_ns;
  const std::span<const SliceContext> pillars = ps.context();

  // Walk ascending-T pillars; the first adjacent pair whose event count rises
  // (n_{i+1} > n_i) brackets the next earnings date between those two expiries.
  for (std::size_t i = 0; i + 1 < pillars.size(); ++i) {
    const double T1 = pillars[i].T;
    const double T2 = pillars[i + 1].T;
    const std::size_t n1 = count_events_at(*ctx.schedule, now, T1);
    const std::size_t n2 = count_events_at(*ctx.schedule, now, T2);
    if (n2 > n1) {
      const double w1 = ps.total_variance(ps.forward_at(T1), T1);
      const double w2 = ps.total_variance(ps.forward_at(T2), T2);
      return implied_emove(w1, T1, n1, w2, T2, n2);
    }
  }
  return Err(ErrorCode::NotFound, "no earnings bracket in fitted expiries");
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

  for (std::size_t i = 0; i < tenors.size(); ++i) {
    const double T = tenors[i];
    TenorAnalytics t;
    t.tenor_years = T;
    if (i < labels.size()) {
      t.label = labels[i];
    }

    const double F = atmf_forward(ps, T);
    const double atm = atmf_vol(ps, T);
    if (!std::isfinite(F) || !std::isfinite(atm)) {
      t.valid = false; // tenor outside the surface's no-extrapolation domain
      out.tenors.push_back(std::move(t));
      continue;
    }

    t.forward = F;
    t.df = std::exp(-ps.rate_at(T) * T);
    t.atm_vol = atm;

    if (cfg.ex_earnings && ctx != nullptr && ctx->schedule != nullptr) {
      t.atm_vol_ex_earn = atmf_vol_ex_earnings(ps, T, *ctx);
      t.n_earnings = static_cast<int>(count_events_at(*ctx->schedule, ps.pricing().now_ts_ns, T));
    } else {
      t.atm_vol_ex_earn = kNaN;
      t.n_earnings = 0;
    }

    // Delta wings: a wing strike can be unreachable in the far tail — store NaN
    // for that entry rather than failing the whole bundle.
    for (const double d : cfg.delta_points) {
      const auto put = vol_at_delta(ps, T, Side::Put, d);
      const auto call = vol_at_delta(ps, T, Side::Call, d);
      const double pv = put.has_value() ? *put : kNaN;
      const double cv = call.has_value() ? *call : kNaN;
      t.put_delta_vol.push_back(pv);
      t.call_delta_vol.push_back(cv);
      t.risk_reversal.push_back(pv - cv);
      t.butterfly.push_back(0.5 * (pv + cv) - atm);
    }

    const SkewCurvature sc = skew_curvature(ps, T, cfg.skew_k_ref);
    t.skew_slope = sc.skew_slope;
    t.curvature = sc.curvature;

    for (const double m : cfg.moneyness_points) {
      t.moneyness_vol.push_back(vol_at_moneyness(ps, T, m));
    }
    t.skew_90_110 = vol_at_moneyness(ps, T, 0.90) - vol_at_moneyness(ps, T, 1.10);

    if (cfg.compute_varswap) {
      const auto vs = var_swap_vol(ps, T, cfg.rnd);
      t.var_swap_vol = vs.has_value() ? *vs : kNaN;
      t.convexity_premium = t.var_swap_vol - atm;
    } else {
      t.var_swap_vol = kNaN;
      t.convexity_premium = kNaN;
    }

    t.expected_move = cfg.straddle_move_multiplier * atm * std::sqrt(T);
    t.valid = true;
    out.tenors.push_back(std::move(t));
  }

  // Risk-neutral density: one per selected tenor; copy its BKM shape moments back
  // onto any matching per-tenor bundle.
  if (cfg.compute_rnd) {
    for (const double Tr : cfg.rnd_tenors_years) {
      auto d = risk_neutral_density(ps, Tr, cfg.rnd);
      if (!d.has_value()) {
        continue;
      }
      for (auto &t : out.tenors) {
        if (std::fabs(t.tenor_years - Tr) < 1e-9) {
          t.rnd_skewness = d->bkm_skew;
          t.rnd_kurtosis = d->bkm_kurt;
          t.prob_below_forward = d->prob_below_forward;
        }
      }
      out.densities.push_back(std::move(*d));
    }
  }

  // Term-structure summary (constant-maturity ATMF read straight off the surface,
  // same ACT/365.25 basis as the tenor grid). Leave the slope fields 0 on a NaN.
  const double sig1m = atmf_vol(ps, 30.0 / 365.25);
  const double sig3m = atmf_vol(ps, 91.0 / 365.25);
  const double sig1y = atmf_vol(ps, 365.0 / 365.25);
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

  double t_first_valid = kNaN;
  bool any_valid = false;

  for (std::size_t i = 0; i < tenors.size(); ++i) {
    const double T = tenors[i];
    TenorDiff td;
    td.tenor_years = T;
    if (i < labels.size()) {
      td.label = labels[i];
    }

    const double va = atmf_vol(a, T);
    const double vb = atmf_vol(b, T);
    if (!std::isfinite(va) || !std::isfinite(vb)) {
      td.valid = false; // one of the surfaces is out of domain at this tenor
      out.tenors.push_back(std::move(td));
      continue;
    }

    td.d_forward = atmf_forward(b, T) - atmf_forward(a, T);
    td.d_atm_vol = vb - va;

    const double K0 = atmf_forward(a, T); // t1's ATM strike (sticky-strike)
    td.d_vol_fixed_strike = b.iv(K0, T) - a.iv(K0, T);

    const auto pa = vol_at_delta(a, T, Side::Put, 0.25);
    const auto pb = vol_at_delta(b, T, Side::Put, 0.25);
    td.d_vol_fixed_delta = (pa.has_value() && pb.has_value()) ? (*pb - *pa) : kNaN;

    td.d_skew_slope = skew_curvature(b, T, cfg.skew_k_ref).skew_slope -
                      skew_curvature(a, T, cfg.skew_k_ref).skew_slope;

    const auto rra = risk_reversal(a, T, 0.25);
    const auto rrb = risk_reversal(b, T, 0.25);
    td.d_risk_reversal_25 = (rra.has_value() && rrb.has_value()) ? (*rrb - *rra) : kNaN;

    const auto bfa = butterfly(a, T, 0.25);
    const auto bfb = butterfly(b, T, 0.25);
    td.d_butterfly_25 = (bfa.has_value() && bfb.has_value()) ? (*bfb - *bfa) : kNaN;

    td.valid = true;
    if (!any_valid) {
      t_first_valid = T;
      any_valid = true;
    }
    out.tenors.push_back(std::move(td));
  }

  // Sticky decomposition off the FIRST valid tenor: predicted ATM move under a
  // sticky-strike regime is 𝒮·R, and the residual is the observed move minus it.
  if (any_valid && std::isfinite(t_first_valid)) {
    const double skew = skew_curvature(a, t_first_valid, cfg.skew_k_ref).skew_slope;
    const double R = out.log_return;
    out.sticky_strike_atm_pred = (std::isfinite(skew) && std::isfinite(R)) ? skew * R : 0.0;
    out.sticky_delta_atm_pred = 0.0;
    out.residual_atm_move =
        (atmf_vol(b, t_first_valid) - atmf_vol(a, t_first_valid)) - out.sticky_strike_atm_pred;
  }

  out.valid = any_valid;
  return Ok(std::move(out));
}

} // namespace atx::vol
