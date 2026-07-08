#include "atx/vol/vol_curve.hpp"

#include <cmath>
#include <limits>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/vol/essvi_calib.hpp"  // essvi_fit_slice
#include "atx/vol/svi_calib.hpp"    // svi_fit_slice

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}  // namespace

const char* to_string(VolCurveKind kind) noexcept {
  switch (kind) {
    case VolCurveKind::ConvexDense:
      return "convex-dense";
    case VolCurveKind::Essvi:
      return "essvi";
    case VolCurveKind::Svi:
      return "svi";
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

EssviCurve::EssviCurve(const EssviParams& slice, double df) noexcept
    : IVolCurve(slice.T, slice.F, df), slice_(slice) {}

SviCurve::SviCurve(const SviParams& slice, double df) noexcept
    : IVolCurve(slice.T, slice.F, df), slice_(slice) {}

// ── CurveSurface ────────────────────────────────────────────────────────────

void CurveSurface::push(std::unique_ptr<IVolCurve> slice) {
  if (slice != nullptr) {
    slices_.push_back(std::move(slice));
  }
}

CurveSurface CurveSurface::clone() const {
  CurveSurface out;
  out.slices_.reserve(slices_.size());
  for (const std::unique_ptr<IVolCurve>& s : slices_) {
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
    return wlo;  // long-end: flat total variance beyond the last slice
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

Result<std::unique_ptr<IVolCurve>> fit_slice_curve(
    const CurveConfig& cfg, std::span<const FitObs> obs_eu, double F, double T,
    double df, const std::function<double(double)>& w_prev) {
  if (!(F > 0.0) || !(T > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "fit_slice_curve: F/T/df must be positive");
  }
  if (obs_eu.empty()) {
    return Err(ErrorCode::InvalidArgument, "fit_slice_curve: empty observations");
  }

  switch (cfg.kind) {
    case VolCurveKind::ConvexDense: {
      // `w_prev` (when set) becomes the per-node calendar floor: this slice's
      // total variance cannot dip below the previous expiry's at the fit nodes.
      ATX_TRY(ConvexSliceFit fit,
              fit_convex_slice(obs_eu, F, T, df, cfg.convex, w_prev));
      std::unique_ptr<IVolCurve> curve =
          std::make_unique<ConvexDenseCurve>(std::move(fit));
      return Ok(std::move(curve));
    }
    // Essvi/Svi IGNORE `w_prev`: their calendar handling is unchanged (eSSVI is
    // arb-free by construction through Mingone; raw-SVI relies on the post-fit
    // `arb_project_calendar_svi` repair, not a per-slice floor).
    case VolCurveKind::Essvi: {
      ATX_TRY(EssviParams slice,
              essvi_fit_slice(obs_eu, T, F, cfg.parametric));
      std::unique_ptr<IVolCurve> curve = std::make_unique<EssviCurve>(slice, df);
      return Ok(std::move(curve));
    }
    case VolCurveKind::Svi: {
      ATX_TRY(SviParams slice, svi_fit_slice(obs_eu, T, F, cfg.parametric));
      std::unique_ptr<IVolCurve> curve = std::make_unique<SviCurve>(slice, df);
      return Ok(std::move(curve));
    }
  }
  return Err(ErrorCode::InvalidArgument, "fit_slice_curve: unknown curve kind");
}

}  // namespace atx::vol
