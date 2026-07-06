#include "atx/vol/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/american.hpp"        // american_price, american_price_cached, american_greeks
#include "atx/vol/arb.hpp"             // arb_check_calendar (post-refit recheck)
#include "atx/vol/correction.hpp"      // CorrectionCache, AmericanCorrectionCaches
#include "atx/vol/curve_fit.hpp"       // fit_curve_surface (curve-agnostic driver)
#include "atx/vol/data.hpp"           // data_install
#include "atx/vol/dividend.hpp"        // hybrid_forward (representative carry)
#include "atx/vol/essvi_calib.hpp"     // essvi_fit_slice (warm-start refit)
#include "atx/vol/surface_parity.hpp"  // run_surface_parity, SurfaceParityInputs/Report
#include "atx/vol/universe.hpp"        // Universe, Underlying, Uid, Chain
#include "atx/vol/vol_surface.hpp"     // VolSurface

// DESIGN / PARITY NOTES
// ---------------------
// * build() is the ONLY place the pipeline runs. It maps SessionInputs 1:1 onto
//   SurfaceParityInputs, drives run_surface_parity, then MOVES out the fitted
//   surface + per-slice context + per-expiry parity and keeps the pricing inputs
//   so the const queries never refit.
//
// * The queries reproduce run_surface_parity's own coordinates exactly: at a
//   query T equal to a slice's T, interp_forward returns that slice's (F, q_eff)
//   (the between-slices interpolation collapses with alpha == 0), so an on-slice
//   query re-prices on the identical forward/carry the fit was scored on, and the
//   surface's own iv(k, T) serves the vol — no side computation.
//
// * The hot path: when `use_correction_cache` is set, build() builds a per-side
//   Chebyshev correction cache over the chain's (k, T, sigma) box and routes
//   every American inversion (de-Am) and re-pricing (parity + the fair_value /
//   greeks queries) through `american_price_cached`. The same caches price both
//   legs, so the invert/re-price round-trip stays self-consistent; a null cache
//   (disabled, or a build failure) degrades transparently to cold Andersen-Lake.

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// A finite, strictly-positive query coordinate.
[[nodiscard]] bool valid_query(double K, double T) noexcept {
  return std::isfinite(K) && (K > 0.0) && std::isfinite(T) && (T > 0.0);
}

}  // namespace

VolaSession::VolaSession(VolSurface&& surface, std::vector<SliceContext>&& ctx,
                         std::vector<ParityReport>&& parity, SessionInputs in,
                         const SessionDiagnostics& diag,
                         std::optional<CorrectionCache>&& corr_call,
                         std::optional<CorrectionCache>&& corr_put,
                         std::optional<CurveSurface>&& curve_override)
    : surface_{std::move(surface)},
      ctx_{std::move(ctx)},
      parity_{std::move(parity)},
      in_{std::move(in)},
      diag_{diag},
      corr_call_{std::move(corr_call)},
      corr_put_{std::move(corr_put)},
      curve_override_{std::move(curve_override)} {}

namespace {

// Per-side correction caches built for a session (empty => cold fallback).
struct BuiltCaches {
  std::optional<CorrectionCache> call;
  std::optional<CorrectionCache> put;
};

// Build both per-side Chebyshev correction caches over the underlying's
// (k_log, T, sigma) box. A side whose build fails is left empty, so the pipeline
// transparently falls back to the cold Andersen-Lake path for that side.
[[nodiscard]] BuiltCaches build_session_caches(const Underlying& under,
                                               const SessionInputs& in) {
  BuiltCaches out;
  if (under.chains.empty() || !(in.S > 0.0)) {
    return out;
  }
  const double S = in.S;

  // (k_log, T) box from every strike / expiry, using spot as the forward proxy.
  double k_min = std::numeric_limits<double>::infinity();
  double k_max = -std::numeric_limits<double>::infinity();
  double T_lo = std::numeric_limits<double>::infinity();
  double T_hi = -std::numeric_limits<double>::infinity();
  for (const Chain& c : under.chains) {
    if (!(c.T > 0.0)) {
      continue;
    }
    T_lo = std::min(T_lo, c.T);
    T_hi = std::max(T_hi, c.T);
    for (const double K : c.strikes) {
      if (!(K > 0.0)) {
        continue;
      }
      const double k = std::log(K / S);
      k_min = std::min(k_min, k);
      k_max = std::max(k_max, k);
    }
  }
  if (!std::isfinite(k_min) || !std::isfinite(k_max) || !(k_max > k_min) ||
      !std::isfinite(T_lo) || !(T_lo > 0.0)) {
    return out;  // degenerate box -> cold path everywhere
  }

  // Pad the box and keep T strictly ordered even for a single expiry.
  k_min -= 0.05;
  k_max += 0.05;
  const double T_min = 0.9 * T_lo;
  const double T_max = (T_hi > T_lo) ? (1.1 * T_hi) : (1.5 * T_lo);
  constexpr double kSigMin = 0.05;
  constexpr double kSigMax = 1.5;

  // Representative carry q_rep from the mid expiry's zero-borrow hybrid forward
  // (F = S*e^{(r-q)T}). The correction is baked at this single carry; the
  // Black-76 leg always uses the real per-quote q_eff, and the small carry
  // mismatch cancels in the self-consistent invert/re-price round-trip.
  const Chain& mid = under.chains[under.chains.size() / 2];
  double q_rep = in.r;
  if (mid.T > 0.0) {
    const double F_rep = hybrid_forward(S, in.r, 0.0, mid.T, in.cash_divs,
                                        mid.expiry_ns, in.now_ts_ns, in.deam.hyb);
    if (F_rep > 0.0 && std::isfinite(F_rep)) {
      q_rep = in.r - std::log(F_rep / S) / mid.T;
    }
  }

  constexpr std::uint16_t kNK = 16;
  constexpr std::uint16_t kNT = 8;
  constexpr std::uint16_t kNS = 12;
  Result<CorrectionCache> cc =
      CorrectionCache::build(kNK, kNT, kNS, in.r, q_rep, k_min, k_max, T_min,
                             T_max, kSigMin, kSigMax, Side::Call, in.deam.al_opts);
  if (cc) {
    out.call = std::move(*cc);
  }
  Result<CorrectionCache> pp =
      CorrectionCache::build(kNK, kNT, kNS, in.r, q_rep, k_min, k_max, T_min,
                             T_max, kSigMin, kSigMax, Side::Put, in.deam.al_opts);
  if (pp) {
    out.put = std::move(*pp);
  }
  return out;
}

}  // namespace

void apply_fit_preset(SessionInputs& in, FitPreset preset) noexcept {
  // Shared across every preset: route the American inversions / re-pricing
  // through the correction-cache hot path.
  in.use_correction_cache = true;
  switch (preset) {
    case FitPreset::Fast:
    case FitPreset::Hft:
      // Fast surface-fit path: the fast Andersen-Lake preset with the inversion
      // tol matched to its ~1e-4 accuracy floor (a tighter tol collapses
      // safeguarded Newton into bisection and slows the fit), and a single ATM
      // borrow pair (the term borrow the smile then absorbs into log-moneyness).
      in.deam.al_opts = al_fast_opts();
      in.deam.iv_tol = 1.0e-5;
      in.deam.n_atm = 1;
      // Hft additionally repairs calendar arb (near-money, held quality); Fast
      // leaves the raw surface. Both feed the cheap cached query path.
      in.calendar_repair =
          (preset == FitPreset::Hft) ? CalendarRepair::MonotoneFit
                                     : CalendarRepair::None;
      break;
    case FitPreset::Accurate:
    case FitPreset::Robust:
      // Reference fidelity: the ACCURATE Andersen-Lake preset (pinned explicitly
      // so build() does not substitute the fast preset), a tight inversion tol,
      // and three ATM borrow pairs.
      in.deam.al_opts = al_default_opts();
      in.deam.iv_tol = 1.0e-7;
      in.deam.n_atm = 3;
      // NOTE on the wing-residual layer: measured OFF here deliberately. On real
      // SPY OPRA the eSSVI backbone alone already fits the tradeable smile to
      // ~1.0 vol pt vega-weighted; enabling the additive HingeQuad residual moves
      // that headline by ~0 (it only reshapes the low-vega deep wings, which the
      // vega weighting discounts) and OVER-FITS sparse event wings (a lone deep
      // put can swing ~50 vol pts) — matching the existing profile.cpp finding.
      // So it stays at its default (disabled); accuracy comes from the backbone.
      // Robust makes the surface calendar-arb-free near-money at held quality;
      // Accurate reports the raw calendar status without altering the fit.
      in.calendar_repair =
          (preset == FitPreset::Robust) ? CalendarRepair::MonotoneFit
                                        : CalendarRepair::None;
      break;
  }
}

SessionInputs make_session_inputs(FitPreset preset, double S, double r,
                                  std::int64_t now_ts_ns) {
  SessionInputs in;
  in.S = S;
  in.r = r;
  in.now_ts_ns = now_ts_ns;
  apply_fit_preset(in, preset);
  return in;
}

Result<VolaSession> VolaSession::build(const Underlying& under,
                                       const SessionInputs& in) {
  // The session is the fast production fit path: de-Americanize and sample the
  // correction cache with the fast ALO preset unless the caller pinned an
  // explicit accuracy. IV inversion / cache sampling only need ~1e-4 price
  // accuracy (surface RMSE is ~1e-2), so the high-precision (nullopt) preset is
  // wasted cost here. `eff` carries this default onto BOTH the cache build and
  // the parity run, and is the copy stored for the const queries so the cold
  // fair_value/greeks fallback prices on the same scheme it was fit with.
  SessionInputs eff = in;
  if (!eff.deam.al_opts) {
    eff.deam.al_opts = al_fast_opts();
    // Match the inversion tol to the fast pricer's ~1e-4 accuracy floor. 1e-5 is
    // still 3 orders below the ~1e-2 surface RMSE, so quality is unaffected, but
    // it lets the American-IV Newton converge instead of stalling into bisection
    // (each bisection step is a full American solve). Only applied when the fast
    // preset is auto-selected; a caller pinning al_opts keeps the tight default.
    eff.deam.iv_tol = 1.0e-5;
    // Borrow from the single closest-ATM co-terminal pair. Each extra pair runs
    // its own borrow fixed-point (both legs re-inverted per iteration) for a
    // borrow the smile then absorbs into log-moneyness; one well-chosen pair is
    // the dominant term-structure driver at a fraction of the cost.
    eff.deam.n_atm = 1;
  }

  // SessionInputs -> SurfaceParityInputs (1:1; run_surface_parity validates S/r).
  SurfaceParityInputs sp;
  sp.S = eff.S;
  sp.r = eff.r;
  sp.cash_divs = eff.cash_divs;
  sp.now_ts_ns = eff.now_ts_ns;
  sp.deam = eff.deam;
  sp.calib = eff.calib;
  sp.band_k = eff.band_k;
  sp.repair = eff.calendar_repair;

  // SOTA hot path: build per-side correction caches and route every American
  // inversion (de-Am) + re-pricing (parity) through the cached pricer. The
  // caches are locals whose pointers feed run_surface_parity, then are MOVED
  // into the session for the const queries. Empty (build failed / disabled) =>
  // the cold Andersen-Lake path, transparently.
  BuiltCaches caches;
  if (eff.use_correction_cache) {
    caches = build_session_caches(under, eff);
    sp.deam.caches = AmericanCorrectionCaches{
        caches.call ? &*caches.call : nullptr,
        caches.put ? &*caches.put : nullptr};
  }

  // ── Curve-family dispatch ──────────────────────────────────────────────────
  // Default (Essvi) keeps the byte-identical run_surface_parity path below.
  // ConvexDense / Svi fit through the curve-agnostic driver and are SERVED via
  // the polymorphic-surface override — this is how PricerFitter reaches the
  // 99.5%-in-band convex dense fit (previously bench-only).
  if (eff.curve.kind != VolCurveKind::Essvi) {
    ATX_TRY(CurveSurfaceReport crep, fit_curve_surface(under, sp, eff.curve));

    SessionDiagnostics cdiag{};
    cdiag.n_slices = crep.n_slices;
    // Calendar no-arb across slices is not (yet) checked for the dense/SVI
    // override; report it unverified rather than asserting an unproven property.
    // (Each convex slice is butterfly-arb-free by construction of the QP.)
    cdiag.calendar_arb_free = false;
    cdiag.n_calendar_viol_pre = 0;
    {
      double worst = std::numeric_limits<double>::infinity();
      double sum_frac = 0.0, sum_chi2 = 0.0, sum_rmse = 0.0;
      std::size_t np_scored = 0;
      for (const ParityReport& p : crep.per_expiry) {
        if (p.n == 0) {
          continue;
        }
        worst = std::min(worst, p.frac_fv_within_bidask);
        sum_frac += p.frac_fv_within_bidask;
        sum_chi2 += p.chi2_reduced;
        sum_rmse += p.rmse_mid_vol;
        ++np_scored;
      }
      if (np_scored > 0) {
        const double dn = static_cast<double>(np_scored);
        cdiag.worst_frac_within_bidask = worst;
        cdiag.mean_frac_within_bidask = sum_frac / dn;
        cdiag.mean_chi2_reduced = sum_chi2 / dn;
        cdiag.mean_rmse_vol = sum_rmse / dn;
      }
      std::size_t nq = 0;
      for (const SliceContext& c : crep.context) {
        nq += c.n_used;
      }
      cdiag.n_quotes = nq;
    }

    // Placeholder eSSVI VolSurface: queries read the override, so surface_ is
    // unused, but VolaSession holds one by value. Cap >= 1 for create().
    ATX_TRY(VolSurface placeholder,
            VolSurface::create(under.uid, Parametrization::Essvi,
                               std::max<std::size_t>(std::size_t{1},
                                                     under.chains.size())));
    return Ok(VolaSession{std::move(placeholder), std::move(crep.context),
                          std::move(crep.per_expiry), std::move(eff), cdiag,
                          std::move(caches.call), std::move(caches.put),
                          std::optional<CurveSurface>{std::move(crep.surface)}});
  }

  ATX_TRY(SurfaceParityReport rep, run_surface_parity(under, sp));

  // Aggregate diagnostics from the per-expiry parity + per-slice context BEFORE
  // moving those vectors into the session.
  SessionDiagnostics diag{};
  diag.n_slices = rep.n_slices;
  diag.calendar_arb_free = rep.calendar_arb_free;
  diag.n_calendar_viol_pre = rep.n_calendar_viol_pre;

  double worst = std::numeric_limits<double>::infinity();
  double sum_frac = 0.0;
  double sum_chi2 = 0.0;
  double sum_rmse = 0.0;
  for (const ParityReport& p : rep.per_expiry) {
    worst = std::min(worst, p.frac_fv_within_bidask);
    sum_frac += p.frac_fv_within_bidask;
    sum_chi2 += p.chi2_reduced;
    sum_rmse += p.rmse_mid_vol;
  }
  const std::size_t np = rep.per_expiry.size();
  if (np > 0) {
    const double dnp = static_cast<double>(np);
    diag.worst_frac_within_bidask = worst;
    diag.mean_frac_within_bidask = sum_frac / dnp;
    diag.mean_chi2_reduced = sum_chi2 / dnp;
    diag.mean_rmse_vol = sum_rmse / dnp;
  }

  std::size_t n_quotes = 0;
  for (const SliceContext& c : rep.context) {
    n_quotes += c.n_used;
  }
  diag.n_quotes = n_quotes;

  return Ok(VolaSession{std::move(rep.surface), std::move(rep.context),
                        std::move(rep.per_expiry), std::move(eff), diag,
                        std::move(caches.call), std::move(caches.put),
                        std::optional<CurveSurface>{}});
}

Result<VolaSession> VolaSession::from_frame(const QuoteFrame& frame,
                                            const SessionInputs& in) {
  Universe u;
  ATX_TRY(const Uid uid, data_install(u, frame));
  ATX_TRY(Underlying* under, u.get_underlying(uid));
  return build(*under, in);
}

VolaSession::ForwardCarry VolaSession::interp_forward(double T) const noexcept {
  // Precondition: ctx_ is non-empty and ascending in T (build guarantees it).
  const SliceContext& first = ctx_.front();
  const SliceContext& last = ctx_.back();
  if (T <= first.T) {
    return ForwardCarry{first.forward, first.q_eff};
  }
  if (T >= last.T) {
    return ForwardCarry{last.forward, last.q_eff};
  }

  // Strictly between the endpoints: find the first slice whose T exceeds the
  // query, then linearly interpolate the bracketing pair. `hi >= 1` because
  // T > first.T; `hi < size` because T < last.T.
  std::size_t hi = 0;
  while (hi < ctx_.size() && ctx_[hi].T <= T) {
    ++hi;
  }
  const std::size_t lo = hi - 1;
  const SliceContext& a = ctx_[lo];
  const SliceContext& b = ctx_[hi];
  const double span = b.T - a.T;
  const double alpha = (span > 0.0) ? (T - a.T) / span : 0.0;
  return ForwardCarry{a.forward + alpha * (b.forward - a.forward),
                      a.q_eff + alpha * (b.q_eff - a.q_eff)};
}

double VolaSession::forward_at(double T) const noexcept {
  if (!(T > 0.0) || ctx_.empty()) {
    return 0.0;
  }
  return interp_forward(T).forward;
}

double VolaSession::q_eff_at(double T) const noexcept {
  if (!(T > 0.0) || ctx_.empty()) {
    return 0.0;
  }
  return interp_forward(T).q_eff;
}

Result<PricedSurface> VolaSession::to_priced_surface() const {
  // Resolved cold-repricing scalars. `in_` carries the effective (post-build)
  // pricer method + Andersen-Lake preset; build() always engages al_opts (either
  // caller-pinned or the fast default), so value_or is a belt-and-braces fallback.
  PricingContext pc;
  pc.S = in_.S;
  pc.r = in_.r;
  pc.now_ts_ns = in_.now_ts_ns;
  pc.method = in_.deam.method;
  pc.al_opts = in_.deam.al_opts.value_or(al_fast_opts());
  pc.uid = surface_.uid();

  CurveSurface cs;
  if (curve_override_.has_value()) {
    // ConvexDense / Svi: the fitted curves already live in the override. A deep
    // copy leaves the live session's surface intact for continued serving.
    cs = curve_override_->clone();
  } else {
    // eSSVI default: the fitted slices live in the VolSurface, not a CurveSurface.
    // Rebuild them into a uniform CurveSurface (df_i = exp(-r*T_i), ascending T,
    // parallel to ctx_) so the snapshot serves through the SAME polymorphic path.
    const std::span<const EssviParams> sl = surface_.essvi_slices();
    for (const EssviParams& e : sl) {
      const double df = std::exp(-in_.r * e.T);
      cs.push(std::make_unique<EssviCurve>(e, df));
    }
  }

  std::vector<SliceContext> ctx_copy(ctx_.begin(), ctx_.end());
  return PricedSurface::create(std::move(cs), std::move(ctx_copy), pc);
}

double VolaSession::iv(double K, double T) const {
  if (!valid_query(K, T)) {
    return kNaN;
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  return model_iv(k, T);
}

double VolaSession::total_variance(double K, double T) const {
  if (!valid_query(K, T)) {
    return kNaN;
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  return model_w(k, T);
}

Result<double> VolaSession::fair_value(double K, double T, Side side) const {
  if (!valid_query(K, T)) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::fair_value: non-finite or non-positive K/T");
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  const double sigma = model_iv(k, T);

  // Cached hot path for the eSSVI default; cold (accurate) Andersen-Lake for the
  // high-accuracy override surface (see served_cache).
  const CorrectionCache* const cc = served_cache(side);
  if (cc != nullptr) {
    const double fv =
        american_price_cached(in_.S, K, T, sigma, in_.r, fc.q_eff, side, cc);
    if (!std::isfinite(fv)) {
      return Err(ErrorCode::Internal,
                 "VolaSession::fair_value: cached pricer produced a non-finite price");
    }
    return Ok(fv);
  }
  return american_price(in_.S, K, T, sigma, in_.r, fc.q_eff, side,
                        in_.deam.method, in_.deam.al_opts);
}

Result<AmericanGreeks> VolaSession::greeks(double K, double T, Side side) const {
  if (!valid_query(K, T)) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::greeks: non-finite or non-positive K/T");
  }
  const ForwardCarry fc = interp_forward(T);
  const double k = std::log(K / fc.forward);
  const double sigma = model_iv(k, T);

  // Cached hot path for the eSSVI default; a null cache (override surface, or a
  // side on the cold path) degrades american_greeks to the accurate leg.
  const CorrectionCache* const use = served_cache(side);
  return american_greeks(in_.S, K, T, sigma, in_.r, fc.q_eff, side, use);
}

Status VolaSession::fair_value_ladder(double T, std::span<const double> strikes,
                                      std::span<const Side> sides,
                                      std::span<double> out) const {
  if (!std::isfinite(T) || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::fair_value_ladder: non-finite or non-positive T");
  }
  if (strikes.size() != sides.size() || strikes.size() != out.size()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::fair_value_ladder: strikes/sides/out length mismatch");
  }
  // Resolve the per-expiry context ONCE and reuse it across the whole ladder:
  // the T-bracket forward/carry interpolation and this session's per-side cache
  // pointers do not vary with strike.
  const ForwardCarry fc = interp_forward(T);
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double K = strikes[i];
    if (!std::isfinite(K) || !(K > 0.0)) {
      out[i] = kNaN;  // a bad strike must not sink the rest of the reprice
      continue;
    }
    const Side side = sides[i];
    const double k = std::log(K / fc.forward);
    const double sigma = model_iv(k, T);
    const CorrectionCache* const cc = served_cache(side);
    if (cc != nullptr) {
      out[i] =
          american_price_cached(in_.S, K, T, sigma, in_.r, fc.q_eff, side, cc);
    } else {
      const auto p = american_price(in_.S, K, T, sigma, in_.r, fc.q_eff, side,
                                    in_.deam.method, in_.deam.al_opts);
      out[i] = p.has_value() ? *p : kNaN;
    }
  }
  return Ok();
}

Status VolaSession::greeks_ladder(double T, std::span<const double> strikes,
                                  std::span<const Side> sides,
                                  std::span<AmericanGreeks> out) const {
  if (!std::isfinite(T) || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::greeks_ladder: non-finite or non-positive T");
  }
  if (strikes.size() != sides.size() || strikes.size() != out.size()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::greeks_ladder: strikes/sides/out length mismatch");
  }
  const ForwardCarry fc = interp_forward(T);
  for (std::size_t i = 0; i < strikes.size(); ++i) {
    const double K = strikes[i];
    if (!std::isfinite(K) || !(K > 0.0)) {
      out[i] = AmericanGreeks{};
      out[i].price = kNaN;
      continue;
    }
    const Side side = sides[i];
    const double k = std::log(K / fc.forward);
    const double sigma = model_iv(k, T);
    const CorrectionCache* const use = served_cache(side);
    const auto g = american_greeks(in_.S, K, T, sigma, in_.r, fc.q_eff, side, use);
    if (g.has_value()) {
      out[i] = *g;
    } else {
      out[i] = AmericanGreeks{};
      out[i].price = kNaN;
    }
  }
  return Ok();
}

Result<FitDiag> VolaSession::refit_slice(std::size_t slice_idx,
                                         std::span<const FitObs> new_obs) {
  if (slice_idx >= ctx_.size()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::refit_slice: slice_idx out of range");
  }
  if (new_obs.empty()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::refit_slice: empty observation set");
  }
  const std::span<const EssviParams> slices = surface_.essvi_slices();
  if (slice_idx >= slices.size()) {
    return Err(ErrorCode::InvalidArgument,
               "VolaSession::refit_slice: no eSSVI slice at that index");
  }
  // Copy the current slice: it is BOTH the warm-start seed and the source of the
  // expiry identity we must preserve across the swap.
  const EssviParams warm = slices[slice_idx];
  const SliceContext& sc = ctx_[slice_idx];

  // Keep the term structure calendar-monotone through the update by flooring the
  // ATM level at the previous slice's theta (a no-op for the first slice, and
  // only binds where the refit would otherwise invert against its neighbour).
  const double theta_floor =
      (slice_idx > 0) ? slices[slice_idx - 1].theta : 0.0;

  FitDiag diag{};
  Result<EssviParams> refit = essvi_fit_slice(new_obs, sc.T, sc.forward,
                                              in_.calib, &diag, theta_floor,
                                              &warm);
  if (!refit.has_value()) {
    return Err(std::move(refit).error());  // surface untouched on failure
  }
  refit->expiry_id = warm.expiry_id;   // preserve identity across the swap
  refit->expiry_ns = warm.expiry_ns;

  if (Status st = surface_.set_slice_essvi(slice_idx, *refit); !st.has_value()) {
    return Err(std::move(st).error());
  }
  ctx_[slice_idx].n_used = new_obs.size();

  // Re-evaluate the surface-level calendar no-arb flag over the standard window
  // so diagnostics() stays truthful about the mutated surface (the same window
  // run_surface_parity checks). A check failure leaves the flag conservatively
  // false rather than asserting no-arb it could not verify.
  constexpr double kArbKMin = -3.0;
  constexpr double kArbKMax = 3.0;
  constexpr std::uint32_t kArbNGrid = 25;
  auto cal = arb_check_calendar(surface_, kArbKMin, kArbKMax, kArbNGrid);
  diag_.calendar_arb_free = cal.has_value() && cal->empty();

  return Ok(diag);
}

}  // namespace atx::vol
