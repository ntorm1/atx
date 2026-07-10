#include "atx/vol/vol_curve.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/vol/c8_calib.hpp"    // c8_fit_slice_lm
#include "atx/vol/essvi_calib.hpp" // essvi_fit_slice
#include "atx/vol/svi_calib.hpp"   // svi_fit_slice

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
} // namespace

const char *to_string(VolCurveKind kind) noexcept {
  switch (kind) {
  case VolCurveKind::ConvexDense:
    return "convex-dense";
  case VolCurveKind::Essvi:
    return "essvi";
  case VolCurveKind::Svi:
    return "svi";
  case VolCurveKind::LinearVariance:
    return "linear-variance";
  case VolCurveKind::C8:
    return "c8-event";
  }
  return "unknown";
}

// ── IVolCurve ───────────────────────────────────────────────────────────────

double IVolCurve::iv(double k_log) const noexcept {
  const double wk = w(k_log);
  if (!(wk > 0.0) || !(T_ > 0.0)) {
    return kNaN;
  }
  return std::sqrt(wk / T_);
}

// ── ConvexDenseCurve ────────────────────────────────────────────────────────

ConvexDenseCurve::ConvexDenseCurve(ConvexSliceFit fit) noexcept
    : IVolCurve(fit.T, fit.F, fit.df), fit_(std::move(fit)) {}

double ConvexDenseCurve::iv(double k_log) const noexcept { return fit_.iv(k_log); }

double ConvexDenseCurve::w(double k_log) const noexcept {
  const double s = fit_.iv(k_log);
  if (!(s > 0.0) || !(T_ > 0.0)) {
    return kNaN;
  }
  return s * s * T_;
}

// ── EssviCurve / SviCurve ───────────────────────────────────────────────────

EssviCurve::EssviCurve(const EssviParams &slice, double df) noexcept
    : IVolCurve(slice.T, slice.F, df), slice_(slice) {}

SviCurve::SviCurve(const SviParams &slice, double df) noexcept
    : IVolCurve(slice.T, slice.F, df), slice_(slice) {}

C8Curve::C8Curve(const C8Params &slice, double df) noexcept
    : IVolCurve(slice.T, slice.F, df), slice_(slice) {}

// ── LinearVarianceCurve ─────────────────────────────────────────────────────

LinearVarianceCurve::LinearVarianceCurve(double T, double F, double df, std::vector<double> k,
                                         std::vector<double> total_variance) noexcept
    : IVolCurve(T, F, df), k_(std::move(k)), w_(std::move(total_variance)) {}

double LinearVarianceCurve::w(double k_log) const noexcept {
  if (k_.empty() || w_.size() != k_.size() || !std::isfinite(k_log)) {
    return kNaN;
  }
  if (k_log <= k_.front()) {
    return w_.front();
  }
  if (k_log >= k_.back()) {
    return w_.back();
  }
  const auto it = std::lower_bound(k_.begin(), k_.end(), k_log);
  const std::size_t hi = static_cast<std::size_t>(it - k_.begin());
  const std::size_t lo = hi - 1;
  const double span = k_[hi] - k_[lo];
  if (!(span > 0.0)) {
    return w_[lo];
  }
  const double a = (k_log - k_[lo]) / span;
  return w_[lo] + a * (w_[hi] - w_[lo]);
}

// ── CurveSurface ────────────────────────────────────────────────────────────

void CurveSurface::push(std::unique_ptr<IVolCurve> slice) {
  if (slice != nullptr) {
    slices_.push_back(std::move(slice));
  }
}

CurveSurface CurveSurface::clone() const {
  CurveSurface out;
  out.slices_.reserve(slices_.size());
  for (const std::unique_ptr<IVolCurve> &s : slices_) {
    out.slices_.push_back(s->clone());
  }
  return out;
}

CurveSurface::Bracket CurveSurface::locate(double T) const noexcept {
  const std::size_t n = slices_.size();
  // Clamp below the first / at-or-above the last; linear-interp between.
  if (T <= slices_.front()->T()) {
    return Bracket{0, 0, 0.0};
  }
  if (T >= slices_.back()->T()) {
    return Bracket{n - 1, n - 1, 0.0};
  }
  std::size_t hi = 1;
  while (hi < n && slices_[hi]->T() < T) {
    ++hi;
  }
  const std::size_t lo = hi - 1;
  const double Tlo = slices_[lo]->T();
  const double Thi = slices_[hi]->T();
  const double span = Thi - Tlo;
  const double frac = span > 0.0 ? (T - Tlo) / span : 0.0;
  return Bracket{lo, hi, frac};
}

double CurveSurface::w(double k_log, double T) const noexcept {
  if (slices_.empty() || !(T > 0.0)) {
    return kNaN;
  }
  // Short-end model: below the front slice, hold implied vol FLAT at the front
  // curve's iv (total variance scales linearly, w = w_front * T/T_front). This is
  // bounded and positive as T -> 0, so options aged to near-expiry (the hold-to-
  // expiry backtest path) price cleanly instead of hitting the old NaN cliff.
  const double T_front = slices_.front()->T();
  if (T < T_front) {
    const double w_front = slices_.front()->w(k_log);
    return std::isfinite(w_front) ? w_front * (T / T_front) : kNaN;
  }
  const Bracket b = locate(T);
  const double wlo = slices_[b.lo]->w(k_log);
  if (b.lo == b.hi) {
    return wlo; // long-end: flat total variance beyond the last slice
  }
  const double whi = slices_[b.hi]->w(k_log);
  if (!std::isfinite(wlo) || !std::isfinite(whi)) {
    return kNaN;
  }
  return (1.0 - b.frac) * wlo + b.frac * whi;
}

double CurveSurface::iv(double k_log, double T) const noexcept {
  const double wk = w(k_log, T);
  if (!(wk > 0.0) || !(T > 0.0)) {
    return kNaN;
  }
  return std::sqrt(wk / T);
}

double CurveSurface::forward_at(double T) const noexcept {
  if (slices_.empty()) {
    return 0.0;
  }
  const Bracket b = locate(T);
  const double flo = slices_[b.lo]->F();
  if (b.lo == b.hi) {
    return flo;
  }
  return (1.0 - b.frac) * flo + b.frac * slices_[b.hi]->F();
}

// ── fit_slice_curve ─────────────────────────────────────────────────────────

Result<std::unique_ptr<IVolCurve>> fit_slice_curve(const CurveConfig &cfg,
                                                   std::span<const FitObs> obs_eu, double F,
                                                   double T, double df,
                                                   const std::function<double(double)> &w_prev) {
  if (!(F > 0.0) || !(T > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "fit_slice_curve: F/T/df must be positive");
  }
  if (obs_eu.empty()) {
    return Err(ErrorCode::InvalidArgument, "fit_slice_curve: empty observations");
  }

  switch (cfg.kind) {
  case VolCurveKind::ConvexDense: {
    // `w_prev` (when set) becomes the per-node calendar floor: this slice's
    // total variance cannot dip below the previous expiry's at the fit nodes.
    ATX_TRY(ConvexSliceFit fit, fit_convex_slice(obs_eu, F, T, df, cfg.convex, w_prev));
    std::unique_ptr<IVolCurve> curve = std::make_unique<ConvexDenseCurve>(std::move(fit));
    return Ok(std::move(curve));
  }
  // Essvi/Svi IGNORE `w_prev`: their calendar handling is unchanged (eSSVI is
  // arb-free by construction through Mingone; raw-SVI relies on the post-fit
  // `arb_project_calendar_svi` repair, not a per-slice floor).
  case VolCurveKind::Essvi: {
    ATX_TRY(EssviParams slice, essvi_fit_slice(obs_eu, T, F, cfg.parametric));
    std::unique_ptr<IVolCurve> curve = std::make_unique<EssviCurve>(slice, df);
    return Ok(std::move(curve));
  }
  case VolCurveKind::Svi: {
    ATX_TRY(SviParams slice, svi_fit_slice(obs_eu, T, F, cfg.parametric));
    std::unique_ptr<IVolCurve> curve = std::make_unique<SviCurve>(slice, df);
    return Ok(std::move(curve));
  }
  case VolCurveKind::LinearVariance: {
    std::vector<FitObs> sorted(obs_eu.begin(), obs_eu.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const FitObs &a, const FitObs &b) { return a.k < b.k; });
    std::vector<double> k;
    std::vector<double> w;
    std::vector<double> weight;
    k.reserve(sorted.size());
    w.reserve(sorted.size());
    weight.reserve(sorted.size());
    for (const FitObs &o : sorted) {
      if (!std::isfinite(o.k) || !(o.w_mkt > 0.0) || !std::isfinite(o.w_mkt)) {
        continue;
      }
      if (!k.empty() && o.k == k.back()) {
        // One preferred OTM leg per strike is the normal case. If a caller
        // supplied a duplicate, retain the higher-active-weight observation.
        if (o.active_weight_w > weight.back()) {
          w.back() = o.w_mkt;
          weight.back() = o.active_weight_w;
        }
        continue;
      }
      k.push_back(o.k);
      w.push_back(o.w_mkt);
      weight.push_back(o.active_weight_w);
    }
    if (k.size() < 2) {
      return Err(ErrorCode::NotFound, "fit_slice_curve: fewer than 2 linear-variance nodes");
    }
    std::unique_ptr<IVolCurve> curve =
        std::make_unique<LinearVarianceCurve>(T, F, df, std::move(k), std::move(w));
    return Ok(std::move(curve));
  }
  case VolCurveKind::C8: {
    // Warm-start the event-specific bump family from the robust eSSVI
    // backbone, then fit in total-variance space.  FitObs::noise_sigma is the
    // price-spread uncertainty converted to vol units, so 2*sigma*T*noise is
    // the first-order full-spread uncertainty in w.
    ATX_TRY(EssviParams essvi, essvi_fit_slice(obs_eu, T, F, cfg.parametric));
    const auto seed_res = c8_seed_from_essvi(essvi);
    if (!seed_res.has_value()) {
      return Err(ErrorCode::Unavailable, "fit_slice_curve: C8 seed failed");
    }
    C8Params seed = *seed_res;
    seed.T = T;
    seed.F = F;
    C8Params fitted = seed;

    std::vector<double> k;
    std::vector<double> target_w;
    std::vector<double> spread_w;
    k.reserve(obs_eu.size());
    target_w.reserve(obs_eu.size());
    spread_w.reserve(obs_eu.size());
    for (const FitObs &o : obs_eu) {
      if (!std::isfinite(o.k) || !(o.w_mkt > 0.0)) {
        continue;
      }
      k.push_back(o.k);
      target_w.push_back(o.w_mkt);
      const double dw_dsigma = 2.0 * std::max(o.sigma_mkt, 0.005) * T;
      spread_w.push_back(std::max(dw_dsigma * std::max(o.noise_sigma, 1.0e-7), 1.0e-9));
    }
    if (k.size() < 8) {
      // A C8 fit is under-identified on a thin board. Return its eSSVI-equivalent
      // zero-bump seed; the selector's eight-DoF penalty will prefer eSSVI.
      fitted.bumps_active = false;
    } else {
      const double seed_sse = c8_residual_sse(seed, k, target_w, spread_w, 1.0e-9);
      const int max_inner = std::max<int>(4, cfg.parametric.max_inner_iter);
      const Status status = c8_fit_slice_lm(fitted, k, target_w, spread_w, max_inner, 1.0e-9);
      const double fit_sse = status ? c8_residual_sse(fitted, k, target_w, spread_w, 1.0e-9)
                                    : std::numeric_limits<double>::infinity();
      if (!std::isfinite(fit_sse) || fit_sse > seed_sse * 1.05) {
        fitted = seed;
        fitted.bumps_active = false;
      }
    }
    std::unique_ptr<IVolCurve> curve = std::make_unique<C8Curve>(fitted, df);
    return Ok(std::move(curve));
  }
  }
  return Err(ErrorCode::InvalidArgument, "fit_slice_curve: unknown curve kind");
}

} // namespace atx::vol
