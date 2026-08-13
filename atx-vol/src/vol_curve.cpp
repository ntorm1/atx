#include "atx/vol/vol_curve.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/vol/arb.hpp" // butterfly gates + shared-k pair projection + independent shape check
#include "atx/vol/black76.hpp"     // black76_price -- price-space calendar floor scan (P-5)
#include "atx/vol/c8_calib.hpp"    // c8_fit_slice_lm
#include "atx/vol/detail/counters.hpp" // ConvexDense wing-anchor observability
#include "atx/vol/detail/dense_slice_price.hpp" // safe_call_price (shared w/ dense_slice.cpp, P-5 I-1)
#include "atx/vol/essvi_calib.hpp" // essvi_fit_slice
#include "atx/vol/svi_calib.hpp"   // svi_fit_slice, svi_project_mm

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kRiskCalendarMin = -0.60;
constexpr double kRiskCalendarMax = 0.60;
constexpr std::uint32_t kRiskCalendarIntervals = 64;
constexpr std::uint32_t kRiskShapeIntervals = 256;

// The tradeable pair band for a parametric calendar projection: the risk band
// intersected with BOTH slices' data-supported k ranges (the current slice's
// from its own observations, the previous slice's from `prev_data_k_range`) —
// the same overlap rule SplineVolCurve::project_calendar already applies. A
// crossing outside this band has no traded witness on either slice: it lives
// in the closed forms' extrapolated wings, where the fit parameters are
// unidentified by data, and projecting the slice's LEVEL over it converts that
// extrapolation noise into ATM error (the sp100-2026 XOM/CVX defect: worst-case
// wing deficits at k=+-0.6, on slices quoted to |k|<~0.1, were added to `a`
// slice after slice, serving 53-vol ATMs against 29-vol quotes).
struct PairBand {
  double lo{0.0};
  double hi{0.0};
  bool usable{false};
};
[[nodiscard]] PairBand tradeable_pair_band(std::span<const FitObs> obs,
                                           std::pair<double, double> prev_range) noexcept {
  double obs_lo = std::numeric_limits<double>::infinity();
  double obs_hi = -std::numeric_limits<double>::infinity();
  for (const FitObs &o : obs) {
    if (std::isfinite(o.k)) {
      obs_lo = std::min(obs_lo, o.k);
      obs_hi = std::max(obs_hi, o.k);
    }
  }
  PairBand band;
  band.lo = std::max({kRiskCalendarMin, obs_lo, prev_range.first});
  band.hi = std::min({kRiskCalendarMax, obs_hi, prev_range.second});
  band.usable =
      std::isfinite(band.lo) && std::isfinite(band.hi) && band.hi - band.lo > 1.0e-9;
  return band;
}

[[nodiscard]] Status validate_parametric_risk_shape(const IVolCurve &curve,
                                                    double k_min, double k_max) {
  ATX_TRY(const std::vector<ArbViolation> violations,
          arb_check_butterfly(curve, k_min, k_max, kRiskShapeIntervals));
  if (!violations.empty()) {
    return Err(ErrorCode::Unavailable,
               "fit_slice_curve: post-calendar strike-shape admission failed");
  }
  return Ok();
}

[[nodiscard]] Status validate_parametric_risk_shape(const IVolCurve &curve) {
  return validate_parametric_risk_shape(curve, kRiskCalendarMin, kRiskCalendarMax);
}

// FT-C2/FT-C7: the strike-shape density scan for a closed-form parametric (SVI)
// or a pinned SplineVol slice must cover the FULL quoted range padded by 0.5 in
// log-moneyness (the C8/CStar policy), NOT just the fixed tradeable [-0.6, 0.6]
// band — a slice that passes the necessary-conditions gate and is clean on
// |k|<=0.6 can still carry Durrleman g<0 in a wing the closed form extrapolates
// past the band. The scan window is the UNION of the padded quoted range and the
// historical band, so coverage is never reduced for a tightly-quoted slice.
[[nodiscard]] Status validate_served_shape_over_quotes(const IVolCurve &curve,
                                                       std::span<const FitObs> obs) {
  double k_lo = kRiskCalendarMin;
  double k_hi = kRiskCalendarMax;
  for (const FitObs &o : obs) {
    if (!std::isfinite(o.k)) {
      continue;
    }
    k_lo = std::min(k_lo, o.k - 0.5);
    k_hi = std::max(k_hi, o.k + 0.5);
  }
  return validate_parametric_risk_shape(curve, k_lo, k_hi);
}
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
  case VolCurveKind::SplineVol:
    return "spline-vol";
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

double ConvexDenseCurve::iv(double k_log) const noexcept {
  const double wk = w(k_log);
  return wk > 0.0 && T_ > 0.0 ? std::sqrt(wk / T_) : kNaN;
}

const ConvexDenseCurve::FiniteAnchors *ConvexDenseCurve::finite_anchors() const noexcept {
  if (const FiniteAnchors *anchors =
          finite_anchors_published_.load(std::memory_order_acquire)) {
    return anchors;
  }
  try {
    std::call_once(finite_anchors_once_, [this] {
      auto anchors = std::make_unique<FiniteAnchors>();
      anchors->k.reserve(fit_.u.size() * 2u);
      anchors->w.reserve(fit_.u.size() * 2u);
      const auto append_anchor = [this, &anchors](double k) {
        ATX_VOL_COUNT(ConvexDenseWingAnchorIvEvaluations);
        const double sigma = fit_.iv(k);
        if (std::isfinite(sigma) && sigma > 0.0) {
          anchors->k.push_back(k);
          anchors->w.push_back(sigma * sigma * T_);
        }
      };
      for (std::size_t i = 0; i < fit_.u.size(); ++i) {
        append_anchor(std::log(fit_.u[i] / F_));
        if (i + 1u < fit_.u.size()) {
          append_anchor(std::log(std::sqrt(fit_.u[i] * fit_.u[i + 1u]) / F_));
        }
      }
      const FiniteAnchors *const published = anchors.get();
      finite_anchors_owner_ = std::move(anchors);
      ATX_VOL_COUNT(ConvexDenseWingAnchorBuilds);
      finite_anchors_published_.store(published, std::memory_order_release);
    });
  } catch (...) {
    // w() is noexcept. A transient allocation/call_once failure therefore
    // fails this evaluation as NaN; call_once leaves the flag unset so a later
    // fallback can retry instead of terminating or observing partial state.
    return nullptr;
  }
  return finite_anchors_published_.load(std::memory_order_acquire);
}

double ConvexDenseCurve::w(double k_log) const noexcept {
  const double s = fit_.iv(k_log);
  if (s > 0.0 && std::isfinite(s) && T_ > 0.0) {
    return s * s * T_;
  }
  ATX_VOL_COUNT(ConvexDenseWingFallbackEntries);
  const FiniteAnchors *const anchors = finite_anchors();
  if (anchors == nullptr || anchors->k.empty()) {
    return kNaN;
  }
  if (anchors->k.size() == 1u) {
    return anchors->w.front();
  }
  const auto it = std::lower_bound(anchors->k.begin(), anchors->k.end(), k_log);
  std::size_t lo = 0u;
  std::size_t hi = 1u;
  if (it == anchors->k.begin()) {
    lo = 0u;
    hi = 1u;
  } else if (it == anchors->k.end()) {
    hi = anchors->k.size() - 1u;
    lo = hi - 1u;
  } else {
    hi = static_cast<std::size_t>(it - anchors->k.begin());
    lo = hi - 1u;
  }
  const double span = anchors->k[hi] - anchors->k[lo];
  if (!(span > 0.0)) {
    return anchors->w[lo];
  }
  // Roger Lee's moment bound is |dw/dk| <= 2. Stay just inside it so a
  // numerical wing cannot manufacture an infinite-moment surface.
  const double slope =
      std::clamp((anchors->w[hi] - anchors->w[lo]) / span, -1.999, 1.999);
  return std::max(1.0e-12,
                  anchors->w[lo] + slope * (k_log - anchors->k[lo]));
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
    const double maturity = slice->T();
    maturities_.push_back(maturity);
    try {
      slices_.push_back(std::move(slice));
    } catch (...) {
      maturities_.pop_back();
      throw;
    }
  }
}

Status CurveSurface::replace(std::size_t index, std::unique_ptr<IVolCurve> slice) {
  if (index >= slices_.size() || slice == nullptr) {
    return Err(ErrorCode::InvalidArgument, "CurveSurface::replace: invalid index or null slice");
  }
  const double lower_T = index > 0 ? slices_[index - 1]->T() : 0.0;
  const double upper_T = index + 1 < slices_.size() ? slices_[index + 1]->T()
                                                    : std::numeric_limits<double>::infinity();
  if (!(slice->T() > lower_T) || !(slice->T() < upper_T)) {
    return Err(ErrorCode::InvalidArgument,
               "CurveSurface::replace: replacement breaks maturity order");
  }
  slices_[index] = std::move(slice);
  maturities_[index] = slices_[index]->T();
  return Ok();
}

CurveSurface CurveSurface::clone() const {
  CurveSurface out;
  out.slices_.reserve(slices_.size());
  out.maturities_.reserve(maturities_.size());
  for (const std::unique_ptr<IVolCurve> &s : slices_) {
    out.push(s->clone());
  }
  return out;
}

CurveSurface::Bracket CurveSurface::bracket(double T) const noexcept {
  const std::size_t n = maturities_.size();
  if (n == 0 || !(T > maturities_.front())) {
    return Bracket{0, 0, 0.0};
  }
  if (T >= maturities_.back()) {
    return Bracket{n - 1, n - 1, 0.0};
  }
  const auto upper = std::lower_bound(maturities_.begin() + 1, maturities_.end(), T);
  const std::size_t hi = static_cast<std::size_t>(upper - maturities_.begin());
  // Exact-node queries evaluate exactly that slice. Besides avoiding one
  // virtual call, this prevents an invalid neighbouring slice from poisoning a
  // valid node through the otherwise-zero interpolation weight.
  if (*upper == T) {
    return Bracket{hi, hi, 0.0};
  }
  const std::size_t lo = hi - 1;
  const double Tlo = maturities_[lo];
  const double Thi = maturities_[hi];
  const double span = Thi - Tlo;
  const double upper_weight = span > 0.0 ? (T - Tlo) / span : 0.0;
  return Bracket{lo, hi, upper_weight};
}

double CurveSurface::w(double k_log, double T) const noexcept {
  if (slices_.empty() || !(T > 0.0)) {
    return kNaN;
  }
  return w(k_log, T, bracket(T));
}

double CurveSurface::w(double k_log, double T, Bracket resolved) const noexcept {
  if (slices_.empty() || !(T > 0.0) || resolved.lo >= slices_.size() ||
      resolved.hi >= slices_.size() || resolved.lo > resolved.hi ||
      !(resolved.upper_weight >= 0.0 && resolved.upper_weight <= 1.0)) {
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
  const double wlo = slices_[resolved.lo]->w(k_log);
  if (resolved.is_single_slice()) {
    return wlo; // long-end: flat total variance beyond the last slice
  }
  const double whi = slices_[resolved.hi]->w(k_log);
  if (!std::isfinite(wlo) || !std::isfinite(whi)) {
    return kNaN;
  }
  return (1.0 - resolved.upper_weight) * wlo + resolved.upper_weight * whi;
}

double CurveSurface::iv(double k_log, double T) const noexcept {
  const double wk = w(k_log, T);
  if (!(wk > 0.0) || !(T > 0.0)) {
    return kNaN;
  }
  return std::sqrt(wk / T);
}

double CurveSurface::iv(double k_log, double T, Bracket resolved) const noexcept {
  const double wk = w(k_log, T, resolved);
  if (!(wk > 0.0) || !(T > 0.0)) {
    return kNaN;
  }
  return std::sqrt(wk / T);
}

double CurveSurface::forward_at(double T) const noexcept {
  if (slices_.empty()) {
    return 0.0;
  }
  if (std::isnan(T)) {
    return kNaN;
  }
  const Bracket b = bracket(T);
  const double flo = slices_[b.lo]->F();
  if (b.is_single_slice()) {
    return flo;
  }
  return (1.0 - b.upper_weight) * flo + b.upper_weight * slices_[b.hi]->F();
}

// ── fit_slice_curve ─────────────────────────────────────────────────────────

Result<std::unique_ptr<IVolCurve>> fit_slice_curve(const CurveConfig &cfg,
                                                   std::span<const FitObs> obs_eu, double F,
                                                   double T, double df,
                                                   const std::function<double(double)> &w_prev,
                                                   std::span<const double> calendar_floor_knots,
                                                   std::pair<double, double> prev_data_k_range) {
  if (!(F > 0.0) || !(T > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "fit_slice_curve: F/T/df must be positive");
  }
  if (obs_eu.empty()) {
    return Err(ErrorCode::InvalidArgument, "fit_slice_curve: empty observations");
  }
  const Status option_status = validate_calib_options(cfg.parametric);
  if (!option_status.has_value()) {
    return Err(option_status.error());
  }

  switch (cfg.kind) {
  case VolCurveKind::ConvexDense: {
    // The first pass is the noise-aware, constrained market fit. If a previous
    // expiry exists, admission is evaluated on a SHARED k lattice. Any residual
    // crossing is promoted to an exact QP node and the slice is re-fit against
    // the same observations. This is an alternating projection onto the slice
    // price-shape cone and the calendar cone: every iterate remains bounded,
    // monotone and convex because calendar repair happens inside the QP, never
    // by mutating total variance after the fit.
    // Task F-4: THE calendar tolerance (arb.hpp), not a local literal.
    constexpr double kCalendarTol = kCalendarTotalVarianceTol;
    constexpr int kMaxCalendarRefits = 4;

    const ConvexRepairSpec* repair =
        cfg.convex_repair.has_value() ? &*cfg.convex_repair : nullptr;
    if (repair != nullptr &&
        !(std::isfinite(repair->k_min) && std::isfinite(repair->k_max) &&
          repair->k_max > repair->k_min && repair->grid_points >= 2u &&
          std::isfinite(repair->tolerance) && repair->tolerance >= 0.0)) {
      return Err(ErrorCode::InvalidArgument,
                 "fit_slice_curve: invalid ConvexRepairSpec");
    }
    const double calendar_tol = repair != nullptr ? repair->tolerance : kCalendarTol;

    std::vector<double> required_k(calendar_floor_knots.begin(),
                                   calendar_floor_knots.end());
    if (repair != nullptr && !repair->extra_node_ks.empty()) {
      for (const double k : repair->extra_node_ks) {
        if (std::isfinite(k)) {
          required_k.push_back(k);
        }
      }
      std::sort(required_k.begin(), required_k.end());
      required_k.erase(std::unique(required_k.begin(), required_k.end()),
                       required_k.end());
    }
    ConvexFitOpts risk_opts = cfg.convex;
    risk_opts.bound_slope_below = true;
    for (int pass = 0; pass <= kMaxCalendarRefits; ++pass) {
      ConvexFitContext context;
      context.required_k = std::span<const double>{required_k};
      context.noise_aware_regularization = true;
      ATX_TRY(ConvexSliceFit fit,
              fit_convex_slice(obs_eu, F, T, df, risk_opts, w_prev, context));

      if (!w_prev) {
        std::unique_ptr<IVolCurve> curve =
            std::make_unique<ConvexDenseCurve>(std::move(fit));
        return Ok(std::move(curve));
      }

      std::vector<double> violations;
      violations.reserve(8);
      // FIT-P1 (Task P-5): the floor this scan is checking is ITSELF enforced
      // in price space (fit_convex_slice's cfloor rows, dense_slice.cpp), so
      // inverting the fitted node back to an implied vol here just to square
      // it into a total variance was pure waste -- up to 64 Black-76 calls
      // per scanned k via fit.iv(), on top of the fit itself. Compare prices
      // directly instead: fold calendar_tol into the FLOOR side (shift the
      // required total variance down by the tolerance before pricing it),
      // so "floored price >= current price" is exactly the old "wp - w_curr >
      // calendar_tol" decision under Black-76's monotone-in-sigma price (same
      // equivalence dense_slice.cpp's own cfloor construction relies on).
      // The current-side price MUST go through the identical safe_price
      // projection ConvexSliceFit::iv() applies before it would invert --
      // in a wing the raw node price can sit within float noise of the
      // intrinsic/forward no-arb bound, where comparing it unprojected
      // disagrees with what iv()-then-square used to compare (found during
      // characterization: comparing the raw node price flagged spurious wing
      // violations a slack floor never triggered against the pinned
      // pre-change baseline -- see VolCurve.
      // CalendarScanPriceSpaceSelectsIdenticalFloorsAsPreP5Baseline and
      // task-P-5-report.md Sec.2). Not a bit-for-bit-identical arithmetic
      // path (one black76_price call instead of bisecting fit.iv() ~64
      // times), but the same SOURCE decision, so the set of flagged k's is
      // unchanged. Task P-5 review I-1: the projection itself now lives in
      // ONE place (detail::safe_call_price, shared with ConvexSliceFit::iv())
      // instead of being copy-pasted here -- the prior copy is exactly the
      // kind of drift-prone duplication that produced the wing bug above.
      const auto scan_k = [&](double k) {
        const double wp = w_prev(k);
        if (!std::isfinite(wp)) {
          return;
        }
        const double w_floor = wp - calendar_tol;
        if (!(w_floor > 0.0)) {
          return; // current total variance is always >= 0, so no floor to violate
        }
        const double K = F * std::exp(k);
        const double c = fit.call_price(K);
        const double safe_price = detail::safe_call_price(F, K, T, df, c);
        if (!std::isfinite(safe_price)) {
          return; // iv() would have returned NaN here too -- no comparable current vol
        }
        const double floor_price =
            black76_price(F, K, T, std::sqrt(w_floor / T), df, Side::Call);
        if (std::isfinite(floor_price) && safe_price < floor_price) {
          violations.push_back(k);
        }
      };
      if (repair == nullptr) {
        const double dk = (kRiskCalendarMax - kRiskCalendarMin) /
                          static_cast<double>(kRiskCalendarIntervals);
        for (std::uint32_t gi = 0; gi <= kRiskCalendarIntervals; ++gi) {
          scan_k(kRiskCalendarMin + dk * static_cast<double>(gi));
        }
      } else {
        // The oracle's inclusive sample formula verbatim (fraction first):
        // repair evaluates the same SOURCE expression, in the same order, that
        // admission does. Not a bit-for-bit guarantee across TUs/compiler flags
        // (FMA contraction can move an individual sample by an ulp) — the
        // strict caller's 10x-tighter `tolerance` and exact-node promotion
        // (`extra_node_ks`, ConvexRepairSpec) absorb any such drift.
        for (std::uint32_t gi = 0; gi < repair->grid_points; ++gi) {
          const double fraction = static_cast<double>(gi) /
                                  static_cast<double>(repair->grid_points - 1u);
          scan_k(repair->k_min + fraction * (repair->k_max - repair->k_min));
        }
      }
      if (violations.empty()) {
        std::unique_ptr<IVolCurve> curve =
            std::make_unique<ConvexDenseCurve>(std::move(fit));
        return Ok(std::move(curve));
      }
      if (pass == kMaxCalendarRefits) {
        return Err(ErrorCode::Unavailable,
                   "fit_slice_curve: shared-k calendar admission did not converge");
      }

      const std::size_t before = required_k.size();
      required_k.insert(required_k.end(), violations.begin(), violations.end());
      std::sort(required_k.begin(), required_k.end());
      required_k.erase(std::unique(required_k.begin(), required_k.end()),
                       required_k.end());
      if (required_k.size() == before) {
        return Err(ErrorCode::Unavailable,
                   "fit_slice_curve: shared-k calendar projection stalled");
      }
    }
    return Err(ErrorCode::Internal,
               "fit_slice_curve: unreachable shared-k calendar state");
  }
  // Parametric candidates use their native shape-preserving level projection
  // on the same shared-k lattice, then pass an independent served-value Roper
  // check. A model's own successful projection never certifies itself.
  case VolCurveKind::Essvi: {
    ATX_TRY(EssviParams slice, essvi_fit_slice(obs_eu, T, F, cfg.parametric));
    if (w_prev) {
      const PairBand band = tradeable_pair_band(obs_eu, prev_data_k_range);
      if (band.usable) {
        ATX_TRY(const CalendarPairProjection projection,
                arb_project_calendar_essvi_pair(slice, w_prev, band.lo, band.hi,
                                                kRiskCalendarIntervals));
        (void)projection;
      }
    }
    std::unique_ptr<IVolCurve> curve = std::make_unique<EssviCurve>(slice, df);
    // FIT-C5: the eSSVI backbone is butterfly-arb-free EVERYWHERE by
    // construction (the Mingone cube-space fit enforces the Lee/Gatheral-
    // Jacquier bound), so the fixed risk-band scan is sufficient whenever no
    // wing residual was fit — this branch is BIT-IDENTICAL to before C-8 on
    // that (default, residual_disable == true) path. The optional HINGE_QUAD
    // wing-residual layer is NOT projected onto the admissible cone (the
    // per-slice Roper projector is out of port scope; see the PORT NOTE on
    // `fit_wing_residual`, essvi_calib.cpp), so a served residual slice needs
    // the SAME full-quoted-range scan the SVI branch uses (FT-C2/FT-C5): the
    // residual's hinge-quadratic wing term can carry Durrleman g < 0 outside
    // the fixed [-0.6, 0.6] band that a narrower scan never sees.
    if (slice.resid_scale > 0.0) {
      ATX_TRY_VOID(validate_served_shape_over_quotes(*curve, obs_eu));
    } else {
      ATX_TRY_VOID(validate_parametric_risk_shape(*curve));
    }
    return Ok(std::move(curve));
  }
  case VolCurveKind::Svi: {
    ATX_TRY(SviParams slice, svi_fit_slice(obs_eu, T, F, cfg.parametric));
    // Butterfly serving gate: the quasi-explicit raw-SVI fit does NOT promise
    // the Mingone polytope, so validate the fitted slice with the closed-form
    // Martini-Mingone admissibility tally. On a violation, project onto the
    // polytope (the same repair svi_mm_fit_slice applies to every iterate) and
    // re-check; if it STILL violates, refuse to serve an arbitrageable slice.
    if (arb_check_butterfly_svi_mm(slice, T).n_violations > 0) {
      (void)svi_project_mm(slice, T);
      const auto adm = arb_check_butterfly_svi_mm(slice, T);
      if (adm.n_violations > 0) {
        return Err(ErrorCode::Unavailable,
                   "fit_slice_curve: raw-SVI slice butterfly-inadmissible after "
                   "Mingone projection (worst slack " +
                       std::to_string(adm.max_slack) + ")");
      }
    }
    if (w_prev) {
      const PairBand band = tradeable_pair_band(obs_eu, prev_data_k_range);
      if (band.usable) {
        ATX_TRY(const CalendarPairProjection projection,
                arb_project_calendar_svi_pair(slice, w_prev, band.lo, band.hi,
                                              kRiskCalendarIntervals));
        (void)projection;
      }
    }
    std::unique_ptr<IVolCurve> curve = std::make_unique<SviCurve>(slice, df);
    // FT-C2: scan the full quoted range +/- 0.5, not the fixed [-0.6, 0.6] band —
    // the raw-SVI closed form can extrapolate wing butterfly arb past the band
    // that the necessary-conditions Mingone gate above does not see.
    ATX_TRY_VOID(validate_served_shape_over_quotes(*curve, obs_eu));
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
    if (w_prev) {
      const std::vector<double> market_k = k;
      const std::vector<double> market_w = w;
      const auto interpolate_market = [&](double x) noexcept {
        if (x <= market_k.front()) {
          return market_w.front();
        }
        if (x >= market_k.back()) {
          return market_w.back();
        }
        const auto it = std::lower_bound(market_k.begin(), market_k.end(), x);
        const std::size_t hi = static_cast<std::size_t>(it - market_k.begin());
        const std::size_t lo = hi - 1u;
        const double a = (x - market_k[lo]) / (market_k[hi] - market_k[lo]);
        return market_w[lo] + a * (market_w[hi] - market_w[lo]);
      };

      k.insert(k.end(), calendar_floor_knots.begin(), calendar_floor_knots.end());
      std::sort(k.begin(), k.end());
      k.erase(std::unique(k.begin(), k.end()), k.end());
      w.clear();
      w.reserve(k.size());
      for (const double x : k) {
        const double market = interpolate_market(x);
        const double floor = w_prev(x);
        w.push_back(std::isfinite(floor) ? std::max(market, floor) : market);
      }
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
      return Err(ErrorCode::Unavailable, "fit_slice_curve: fewer than 8 observations for C8");
    }
    const double seed_sse = c8_residual_sse(seed, k, target_w, spread_w, 1.0e-9);
    const int max_inner = std::max<int>(4, cfg.parametric.max_inner_iter);
    const Status status = c8_fit_slice_lm(fitted, k, target_w, spread_w, max_inner, 1.0e-9);
    const double fit_sse = status ? c8_residual_sse(fitted, k, target_w, spread_w, 1.0e-9)
                                  : std::numeric_limits<double>::infinity();
    // Admissibility gate (mirrors c8_apply_quality_gate's slice_ok plus the
    // JW-domain v_min range): guards against degenerate fits the SSE-only gate
    // would accept — e.g. an ill-conditioned LM run that improves SSE while
    // collapsing a wing slope to exactly 0 (exposed when the FD Jacobian's
    // accidental all-gradients-fail rejection of v_min == v seeds went away
    // with the analytic JW->raw Jacobian).
    const bool fit_admissible =
        std::isfinite(fitted.v) && fitted.v > 0.0 && std::isfinite(fitted.psi) &&
        std::isfinite(fitted.p) && fitted.p > 0.0 &&
        std::isfinite(fitted.c) && fitted.c > 0.0 &&
        std::isfinite(fitted.v_min) && fitted.v_min >= 0.0 &&
        fitted.v_min <= fitted.v + 1.0e-12 && std::isfinite(fitted.kappa) &&
        std::isfinite(fitted.q_L) && std::isfinite(fitted.q_R);
    // Butterfly accept gate: a grid Durrleman g(k) >= 0 density check of the
    // fitted slice — accept-time only (one 64-point grid eval per served slice,
    // NOT inside the LM loop). Grid spans the observed strikes padded by 0.5 in
    // log-moneyness. A violation folds into the existing revert-to-seed path.
    const auto k_range = std::minmax_element(k.begin(), k.end());
    const double bf_k_min = *k_range.first - 0.5;
    const double bf_k_max = *k_range.second + 0.5;
    const auto fit_bf = arb_check_butterfly_slice(
        [&fitted](double kk) { return c8_slice_w(fitted, kk); }, T, bf_k_min,
        bf_k_max, 64u);
    const bool fit_butterfly_ok = fit_bf.has_value() && fit_bf->empty();
    if (!fit_admissible || !std::isfinite(fit_sse) || fit_sse > seed_sse * 1.05 ||
        !fit_butterfly_ok) {
      fitted = seed;
      fitted.bumps_active = false;
      // The reverted seed is the backbone-only SVI-JW smile; if even it trips
      // the grid g-check, refuse to serve an arbitrageable C8 slice.
      const auto seed_bf = arb_check_butterfly_slice(
          [&fitted](double kk) { return c8_slice_w(fitted, kk); }, T, bf_k_min,
          bf_k_max, 64u);
      if (!(seed_bf.has_value() && seed_bf->empty())) {
        return Err(ErrorCode::Unavailable,
                   "fit_slice_curve: C8 seed slice butterfly-inadmissible");
      }
    }
    if (w_prev) {
      const PairBand band = tradeable_pair_band(obs_eu, prev_data_k_range);
      if (band.usable) {
        ATX_TRY(const CalendarPairProjection projection,
                arb_project_calendar_c8_pair(fitted, w_prev, band.lo, band.hi,
                                             kRiskCalendarIntervals));
        (void)projection;
      }
    }
    std::unique_ptr<IVolCurve> curve = std::make_unique<C8Curve>(fitted, df);
    ATX_TRY_VOID(validate_parametric_risk_shape(*curve));
    return Ok(std::move(curve));
  }
  case VolCurveKind::SplineVol: {
    ATX_TRY(std::unique_ptr<IVolCurve> curve,
            fit_spline_vol_slice(obs_eu, F, T, df, cfg.spline));
    if (w_prev) {
      // Project onto the calendar cone above the previous expiry, matching the
      // Essvi/Svi/C8 branches above. `curve` is a SplineVolCurve by construction
      // (fit_spline_vol_slice always returns one; kind() == SplineVol makes the
      // downcast safe). `calendar_floor_knots` remains unused by the v1 spline.
      auto *spline = static_cast<SplineVolCurve *>(curve.get());
      ATX_TRY_VOID(spline->project_calendar(w_prev, kRiskCalendarMin,
                                            kRiskCalendarMax, kRiskCalendarIntervals,
                                            prev_data_k_range.first,
                                            prev_data_k_range.second));
    }
    // FT-C7: a caller-pinned SplineVol bypasses the selector's butterfly
    // disqualification, and fit_spline_vol_slice records n_butterfly_viol as a
    // DIAGNOSTIC only (never rejects). Turn that same count — computed over the
    // spline's own data range, the exact quantity the selector disqualifies on —
    // into a hard admission rejection so a pinned butterfly-arbitrageable spline
    // is dropped (the fallback ladder handles it) instead of served.
    if (static_cast<const SplineVolCurve *>(curve.get())->params().n_butterfly_viol > 0) {
      return Err(ErrorCode::Unavailable,
                 "fit_slice_curve: pinned SplineVol slice butterfly-inadmissible");
    }
    return Ok(std::move(curve));
  }
  }
  return Err(ErrorCode::InvalidArgument, "fit_slice_curve: unknown curve kind");
}

Result<std::unique_ptr<IVolCurve>> refit_slice_curve(
    const CurveConfig &cfg, const IVolCurve &current,
    std::span<const FitObs> obs_eu, double F, double T, double df,
    const std::function<double(double)> &w_prev, FitDiag *diag) {
  if (current.kind() != cfg.kind || obs_eu.empty() || !(F > 0.0) ||
      !(T > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "refit_slice_curve: incompatible curve or invalid inputs");
  }
  if (diag != nullptr) {
    *diag = FitDiag{};
  }

  switch (cfg.kind) {
  case VolCurveKind::ConvexDense: {
    const auto *warm = dynamic_cast<const ConvexDenseCurve *>(&current);
    if (warm == nullptr) {
      return Err(ErrorCode::InvalidArgument,
                 "refit_slice_curve: missing ConvexDense warm state");
    }
    std::vector<double> warm_k;
    warm_k.reserve(warm->fit().u.size());
    for (const double K : warm->fit().u) {
      if (K > 0.0) {
        warm_k.push_back(std::log(K / F));
      }
    }
    ATX_TRY(std::unique_ptr<IVolCurve> curve,
            fit_slice_curve(cfg, obs_eu, F, T, df, w_prev, warm_k));
    if (diag != nullptr) {
      diag->n_quotes_used = static_cast<std::uint32_t>(obs_eu.size());
    }
    return Ok(std::move(curve));
  }
  case VolCurveKind::Essvi: {
    const auto *warm = dynamic_cast<const EssviCurve *>(&current);
    if (warm == nullptr) {
      return Err(ErrorCode::InvalidArgument,
                 "refit_slice_curve: missing eSSVI warm state");
    }
    ATX_TRY(EssviParams slice,
            essvi_fit_slice(obs_eu, T, F, cfg.parametric, diag, 0.0,
                            &warm->slice()));
    slice.expiry_id = warm->slice().expiry_id;
    slice.expiry_ns = warm->slice().expiry_ns;
    if (w_prev) {
      // The refit seam carries no previous-slice data range; restrict to the
      // CURRENT slice's own quoted range (still a strict improvement over the
      // fixed band — see tradeable_pair_band).
      const PairBand band = tradeable_pair_band(
          obs_eu, {-std::numeric_limits<double>::infinity(),
                   std::numeric_limits<double>::infinity()});
      if (band.usable) {
        ATX_TRY(const CalendarPairProjection projection,
                arb_project_calendar_essvi_pair(slice, w_prev, band.lo, band.hi,
                                                kRiskCalendarIntervals));
        (void)projection;
      }
    }
    std::unique_ptr<IVolCurve> curve = std::make_unique<EssviCurve>(slice, df);
    ATX_TRY_VOID(validate_parametric_risk_shape(*curve));
    return Ok(std::move(curve));
  }
  case VolCurveKind::Svi: {
    ATX_TRY(SviParams slice,
            svi_fit_slice(obs_eu, T, F, cfg.parametric, diag));
    if (w_prev) {
      const PairBand band = tradeable_pair_band(
          obs_eu, {-std::numeric_limits<double>::infinity(),
                   std::numeric_limits<double>::infinity()});
      if (band.usable) {
        ATX_TRY(const CalendarPairProjection projection,
                arb_project_calendar_svi_pair(slice, w_prev, band.lo, band.hi,
                                              kRiskCalendarIntervals));
        (void)projection;
      }
    }
    std::unique_ptr<IVolCurve> curve = std::make_unique<SviCurve>(slice, df);
    ATX_TRY_VOID(validate_parametric_risk_shape(*curve));
    return Ok(std::move(curve));
  }
  case VolCurveKind::C8: {
    const auto *warm = dynamic_cast<const C8Curve *>(&current);
    if (warm == nullptr) {
      return Err(ErrorCode::InvalidArgument,
                 "refit_slice_curve: missing C8 warm state");
    }
    C8Params fitted = warm->slice();
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
      spread_w.push_back(
          std::max(dw_dsigma * std::max(o.noise_sigma, 1.0e-7), 1.0e-9));
    }
    if (k.size() < 8) {
      return Err(ErrorCode::Unavailable,
                 "refit_slice_curve: fewer than 8 observations for C8");
    }
    const double warm_sse =
        c8_residual_sse(fitted, k, target_w, spread_w, 1.0e-9);
    const int max_inner = std::max<int>(4, cfg.parametric.max_inner_iter);
    ATX_TRY_VOID(c8_fit_slice_lm(fitted, k, target_w, spread_w, max_inner,
                                 1.0e-9));
    const double fit_sse =
        c8_residual_sse(fitted, k, target_w, spread_w, 1.0e-9);
    const double quality_ceiling =
        std::max(warm_sse * 1.05, warm_sse + 1.0e-10);
    if (!std::isfinite(fit_sse) || fit_sse > quality_ceiling) {
      return Err(ErrorCode::Unavailable,
                 "refit_slice_curve: C8 warm update failed quality gate");
    }
    fitted.expiry_id = warm->slice().expiry_id;
    fitted.expiry_ns = warm->slice().expiry_ns;
    c8_arb_project(fitted);
    if (w_prev) {
      const PairBand band = tradeable_pair_band(
          obs_eu, {-std::numeric_limits<double>::infinity(),
                   std::numeric_limits<double>::infinity()});
      if (band.usable) {
        ATX_TRY(const CalendarPairProjection projection,
                arb_project_calendar_c8_pair(fitted, w_prev, band.lo, band.hi,
                                             kRiskCalendarIntervals));
        (void)projection;
      }
    }
    std::unique_ptr<IVolCurve> curve = std::make_unique<C8Curve>(fitted, df);
    ATX_TRY_VOID(validate_parametric_risk_shape(*curve));
    if (diag != nullptr) {
      diag->n_quotes_used = static_cast<std::uint32_t>(k.size());
      diag->inner_iters_total = static_cast<std::uint16_t>(
          std::clamp(fitted.n_lm_iters, 0, 65535));
    }
    return Ok(std::move(curve));
  }
  case VolCurveKind::LinearVariance:
    return Err(ErrorCode::InvalidArgument,
               "refit_slice_curve: LinearVariance is not an admitted risk curve");
  case VolCurveKind::SplineVol:
    // SplineVol has no local warm-refit path in v1 (no calendar projection /
    // shared-k admission support either — see fit_slice_curve). Fail closed
    // rather than serve an unvalidated local update.
    return Err(ErrorCode::NotImplemented,
               "refit_slice_curve: SplineVol local refit is not implemented");
  }
  return Err(ErrorCode::InvalidArgument,
             "refit_slice_curve: unknown curve kind");
}

} // namespace atx::vol
