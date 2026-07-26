#include "atx/vol/arb.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/c8.hpp"
#include "atx/vol/curve.hpp"
#include "atx/vol/universe.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kCalendarPairTol = 1.0e-7;

// Wing-residual coefficient width — mirrors the C `ATS_VOL_ESSVI_RESID_N`.
constexpr int kEssviResidN = 16;
static_assert(EssviParams{}.resid_coef.size() == static_cast<std::size_t>(kEssviResidN),
              "resid_coef width must match the C ATS_VOL_ESSVI_RESID_N");

// One nanosecond-scaled second (int64), matching the C's staleness arithmetic.
constexpr std::int64_t kNsPerSecond = 1'000'000'000;

// Per-parametrization slice T lookup (ports the C `slice_T_` helper). Only the
// two evaluatable carriers hold a T; tag-only surfaces report zero slices so
// this is never reached for them.
[[nodiscard]] double slice_T_at(const VolSurface &s, std::size_t i) noexcept {
  switch (s.param()) {
  case Parametrization::Essvi:
    return s.essvi_slices()[i].T;
  case Parametrization::Svi:
  case Parametrization::SviMm:
    return s.svi_slices()[i].T;
  case Parametrization::Wing:
  case Parametrization::C8:
  case Parametrization::CStar16M:
    return kNaN;
  }
  return kNaN;  // unreachable for valid enumerators
}

// Write a full eSSVI slice vector back into `s` (the projection helpers work on
// a local mutable copy so in-pass neighbour reads see the running state, then
// commit here). set_slice_essvi cannot fail on in-range indices of a matching
// surface — the guards up-stack already established both.
[[nodiscard]] Status write_back_essvi(VolSurface &s,
                                      const std::vector<EssviParams> &slices) {
  for (std::size_t i = 0; i < slices.size(); ++i) {
    ATX_TRY_VOID(s.set_slice_essvi(i, slices[i]));
  }
  return Ok();
}

[[nodiscard]] Status write_back_svi(VolSurface &s,
                                    const std::vector<SviParams> &slices) {
  for (std::size_t i = 0; i < slices.size(); ++i) {
    ATX_TRY_VOID(s.set_slice_svi(i, slices[i]));
  }
  return Ok();
}

// Max-over-grid (w_lo - w_hi)_+ on the TOTAL surface for an eSSVI slice pair,
// with `lo`'s residual coefficients temporarily scaled by `alpha`. Zero means
// lo <= hi at every grid point. Ports `total_calendar_deficit_with_alpha`
// (evaluated on a local copy rather than mutate-and-restore).
[[nodiscard]] double total_calendar_deficit_with_alpha(const EssviParams &lo,
                                                       const EssviParams &hi,
                                                       double alpha, double k_min,
                                                       double k_max,
                                                       std::uint32_t n_grid) noexcept {
  if (!(n_grid > 0) || !(k_max > k_min)) {
    return 0.0;
  }
  EssviParams lo_scaled = lo;
  for (std::size_t j = 0; j < lo_scaled.resid_coef.size(); ++j) {
    lo_scaled.resid_coef[j] *= alpha;
  }
  const double dk = (k_max - k_min) / static_cast<double>(n_grid);
  double max_def = 0.0;
  for (std::uint32_t g = 0; g <= n_grid; ++g) {
    const double k = k_min + static_cast<double>(g) * dk;
    const double w_lo = essvi_total_w(lo_scaled, k);
    const double w_hi = essvi_total_w(hi, k);
    if (!std::isfinite(w_lo) || !std::isfinite(w_hi)) {
      continue;
    }
    const double def = w_lo - w_hi;
    if (def > max_def) {
      max_def = def;
    }
  }
  return max_def;
}

// Shared per-slice Durrleman g(k) finite-difference scan. Appends one Butterfly
// `ArbViolation` per interior grid point where the Lee/Roper density
//   g(k) = (1 - k*w'/(2w))^2 - (w'/2)^2*(1/4 + 1/w) + w''/2
// dips below -1e-9. This is the SINGLE implementation of the butterfly FD
// scheme: both the surface-level `arb_check_butterfly` (per slice) and the
// per-slice `arb_check_butterfly_slice` delegate here, so their outputs are
// pointwise identical and the legacy surface pins hold bit-for-bit. The caller
// guarantees `k_max > k_min` and `n_grid >= 4`.
void butterfly_scan_slice(const std::function<double(double)> &w_of_k, double T,
                          double k_min, double k_max, std::uint32_t n_grid,
                          std::vector<ArbViolation> &out) {
  const double dk = (k_max - k_min) / static_cast<double>(n_grid);
  const double inv_2dk = 0.5 / dk;
  const double inv_dksq = 1.0 / (dk * dk);
  for (std::uint32_t g = 1; g < n_grid; ++g) {
    const double k = k_min + static_cast<double>(g) * dk;
    const double w_lo = w_of_k(k - dk);
    const double w_mi = w_of_k(k);
    const double w_hi = w_of_k(k + dk);
    if (!(w_mi > 1.0e-12) || !std::isfinite(w_lo) || !std::isfinite(w_hi)) {
      continue;
    }
    const double w_p = (w_hi - w_lo) * inv_2dk;                 // w'(k)
    const double w_pp = (w_hi - 2.0 * w_mi + w_lo) * inv_dksq;  // w''(k)

    const double term1_inner = 1.0 - 0.5 * k * w_p / w_mi;
    const double term1 = term1_inner * term1_inner;
    const double term2 = 0.25 * w_p * w_p * (0.25 + 1.0 / w_mi);
    const double term3 = 0.5 * w_pp;
    const double g_density = term1 - term2 + term3;

    if (g_density < -1.0e-9) {
      ArbViolation v{};
      v.k_log = k;
      v.T1 = T;
      v.T2 = T;
      v.slack = -g_density;
      v.kind = ArbViolation::Kind::Butterfly;
      out.push_back(v);
    }
  }
}

struct SharedGridGap {
  double max_deficit{};
  double max_ratio{1.0};
  bool finite{true};
};

template <class Eval>
[[nodiscard]] SharedGridGap shared_grid_gap(
    const std::function<double(double)> &w_prev, Eval &&w_current,
    double k_min, double k_max, std::uint32_t n_grid) noexcept {
  SharedGridGap out;
  const double dk = (k_max - k_min) / static_cast<double>(n_grid);
  for (std::uint32_t gi = 0; gi <= n_grid; ++gi) {
    const double k = k_min + dk * static_cast<double>(gi);
    const double wp = w_prev(k);
    const double wc = w_current(k);
    if (!std::isfinite(wp) || !std::isfinite(wc) || !(wc > 0.0)) {
      out.finite = false;
      continue;
    }
    out.max_deficit = std::max(out.max_deficit, wp - wc);
    if (wp > 0.0) {
      out.max_ratio = std::max(out.max_ratio, wp / wc);
    }
  }
  return out;
}

[[nodiscard]] Result<CalendarPairProjection> validate_pair_projection_inputs(
    const std::function<double(double)> &w_prev, double k_min, double k_max,
    std::uint32_t n_grid) {
  if (!w_prev || n_grid == 0 || !(k_max > k_min)) {
    return Err(ErrorCode::InvalidArgument,
               "calendar pair projection: require previous curve and valid grid");
  }
  return Ok(CalendarPairProjection{});
}

}  // namespace

// ── Calendar / butterfly checks ──────────────────────────────────────────

Result<std::vector<ArbViolation>> arb_check_calendar(const VolSurface &s,
                                                     double k_min, double k_max,
                                                     std::uint32_t n_grid) {
  std::vector<ArbViolation> out;
  const std::size_t n = s.n_slices();
  if (n < 2 || n_grid == 0) {
    return Ok(std::move(out));
  }

  const double dk = (k_max - k_min) / static_cast<double>(n_grid);
  for (std::uint32_t g = 0; g < n_grid; ++g) {
    const double k = k_min + static_cast<double>(g) * dk;
    double w_prev = -std::numeric_limits<double>::infinity();
    double T_prev = 0.0;  // paired with w_prev so the offending slice pair is
                          // recorded distinctly from a butterfly violation.
    for (std::size_t i = 0; i < n; ++i) {
      const double T = slice_T_at(s, i);
      const double w = s.w(k, T);
      if (w + 1.0e-12 < w_prev) {
        ArbViolation v{};
        v.k_log = k;
        v.T1 = T_prev;
        v.T2 = T;
        v.slack = w_prev - w;
        v.kind = ArbViolation::Kind::Calendar;
        out.push_back(v);
      }
      w_prev = w;
      T_prev = T;
    }
  }
  return Ok(std::move(out));
}

Result<std::vector<ArbViolation>> arb_check_calendar(const CurveSurface &s,
                                                     double k_min, double k_max,
                                                     std::uint32_t n_grid) {
  std::vector<ArbViolation> out;
  const auto slices = s.slices();
  if (slices.size() < 2 || n_grid == 0 || !(k_max > k_min)) {
    return Ok(std::move(out));
  }
  constexpr double kCalendarTol = 1.0e-7;  // total-variance units
  const double dk = (k_max - k_min) / static_cast<double>(n_grid);
  for (std::size_t i = 1; i < slices.size(); ++i) {
    const IVolCurve &prev = *slices[i - 1];
    const IVolCurve &curr = *slices[i];
    for (std::uint32_t g = 0; g <= n_grid; ++g) {
      const double k = k_min + dk * static_cast<double>(g);
      // Calendar no-arbitrage is a statement about TRADEABLE quotes: skip any
      // point where either slice is pure extrapolation (a SplineVol flat wing
      // beyond its observed strikes). Default is_extrapolated()==false, so every
      // parametric family (Essvi/Svi/C8/ConvexDense/LinearVariance) is checked on
      // the full grid exactly as before — bit-identical.
      if (prev.is_extrapolated(k) || curr.is_extrapolated(k)) {
        continue;
      }
      const double wp = prev.w(k);
      const double wc = curr.w(k);
      if (!std::isfinite(wp) || !std::isfinite(wc)) {
        continue;  // wing coverage gap on one side — nothing to compare
      }
      const double slack = wp - wc;
      if (slack > kCalendarTol) {
        out.push_back(ArbViolation{k, prev.T(), curr.T(), slack,
                                   ArbViolation::Kind::Calendar});
      }
    }
  }
  return Ok(std::move(out));
}

Result<std::vector<ArbViolation>> arb_check_price_bounds(const CurveSurface &s,
                                                         double k_min, double k_max,
                                                         std::uint32_t n_grid) {
  std::vector<ArbViolation> out;
  const auto slices = s.slices();
  if (slices.empty() || n_grid == 0 || !(k_max > k_min)) {
    return Ok(std::move(out));
  }
  // Far BELOW the fitting QP's node-level price_epsilon margin
  // (1e-6*max(1,df*F) ~ 1e-4 at F=100, dense_slice.cpp): a fit whose nodes
  // honor that margin clears this tolerance by orders of magnitude, so any
  // trip is a GENUINE breach past the bound itself (e.g. the wing power-tail
  // undershoot of M-7) — while 1e-9 still absorbs bare FP roundoff exactly
  // on the boundary.
  constexpr double kPriceBoundSelfCheckTol = 1.0e-9;
  const double dk = (k_max - k_min) / static_cast<double>(n_grid);
  for (const auto &slice_ptr : slices) {
    const auto *dense = dynamic_cast<const ConvexDenseCurve *>(slice_ptr.get());
    if (dense == nullptr) {
      continue;  // only the convex dense fit clamps a served price; see I-2
    }
    const ConvexSliceFit &fit = dense->fit();
    if (!(fit.F > 0.0) || !(fit.df > 0.0)) {
      continue;
    }
    for (std::uint32_t g = 0; g <= n_grid; ++g) {
      const double k = k_min + dk * static_cast<double>(g);
      const double K = fit.F * std::exp(k);
      const double price = fit.call_price(K);
      if (!std::isfinite(price)) {
        continue;  // NonFinite is already covered by the w-space checks
      }
      const double lower = fit.df * std::max(fit.F - K, 0.0);
      const double upper = fit.df * fit.F;
      const double slack = std::max(lower - price, price - upper);
      if (slack > kPriceBoundSelfCheckTol) {
        out.push_back(ArbViolation{k, fit.T, fit.T, slack, ArbViolation::Kind::PriceBounds});
      }
    }
  }
  return Ok(std::move(out));
}

Result<std::vector<ArbViolation>> arb_check_butterfly(
    const IVolCurve &curve, double k_min, double k_max,
    std::uint32_t n_grid) {
  std::vector<ArbViolation> out;
  if (n_grid < 4) {
    return Ok(std::move(out));
  }
  if (!(k_max > k_min)) {
    return Err(ErrorCode::InvalidArgument,
               "arb_check_butterfly(curve): require k_max > k_min");
  }
  const double dk = (k_max - k_min) / static_cast<double>(n_grid);
  const double inv_2dk = 0.5 / dk;
  const double inv_dksq = 1.0 / (dk * dk);
  for (std::uint32_t gi = 1; gi < n_grid; ++gi) {
    const double k = k_min + static_cast<double>(gi) * dk;
    const double w_lo = curve.w(k - dk);
    const double w_mi = curve.w(k);
    const double w_hi = curve.w(k + dk);
    if (!(w_mi > 1.0e-12) || !std::isfinite(w_lo) ||
        !std::isfinite(w_mi) || !std::isfinite(w_hi)) {
      out.push_back(ArbViolation{k, curve.T(), curve.T(),
                                 std::numeric_limits<double>::infinity(),
                                 ArbViolation::Kind::Butterfly});
      continue;
    }
    const double w_p = (w_hi - w_lo) * inv_2dk;
    const double w_pp = (w_hi - 2.0 * w_mi + w_lo) * inv_dksq;
    const double a = 1.0 - 0.5 * k * w_p / w_mi;
    const double density = a * a -
                           0.25 * w_p * w_p * (0.25 + 1.0 / w_mi) +
                           0.5 * w_pp;
    if (density < -1.0e-9) {
      out.push_back(ArbViolation{k, curve.T(), curve.T(), -density,
                                 ArbViolation::Kind::Butterfly});
    }
  }
  return Ok(std::move(out));
}

Result<std::vector<ArbViolation>> arb_check_butterfly(const VolSurface &s,
                                                      double k_min, double k_max,
                                                      std::uint32_t n_grid) {
  std::vector<ArbViolation> out;
  const std::size_t n = s.n_slices();
  if (n == 0 || n_grid < 4) {
    return Ok(std::move(out));
  }
  if (!(k_max > k_min)) {
    return Err(ErrorCode::InvalidArgument,
               "arb_check_butterfly: require k_max > k_min");
  }

  for (std::size_t i = 0; i < n; ++i) {
    const double T = slice_T_at(s, i);
    if (!(T > 0.0)) {
      continue;
    }
    // Delegate to the shared FD scan; the per-slice evaluator samples the
    // total surface variance at this slice's maturity (unchanged arithmetic).
    butterfly_scan_slice([&s, T](double k) { return s.w(k, T); }, T, k_min,
                         k_max, n_grid, out);
  }
  return Ok(std::move(out));
}

Result<std::vector<ArbViolation>>
arb_check_butterfly_slice(const std::function<double(double)> &w_of_k, double T,
                          double k_min, double k_max, std::uint32_t n_grid) {
  std::vector<ArbViolation> out;
  if (n_grid < 4) {
    return Ok(std::move(out));
  }
  if (!(k_max > k_min)) {
    return Err(ErrorCode::InvalidArgument,
               "arb_check_butterfly_slice: require k_max > k_min");
  }
  butterfly_scan_slice(w_of_k, T, k_min, k_max, n_grid, out);
  return Ok(std::move(out));
}

Result<std::vector<ArbViolation>> arb_check_all(const VolSurface &s,
                                                double k_min, double k_max,
                                                std::uint32_t n_grid) {
  ATX_TRY(auto out, arb_check_calendar(s, k_min, k_max, n_grid));
  ATX_TRY(auto butterfly, arb_check_butterfly(s, k_min, k_max, n_grid));
  out.insert(out.end(), butterfly.begin(), butterfly.end());
  return Ok(std::move(out));
}

Result<TotalSurfaceArbCounts> arb_check_total_surface_all(const VolSurface &s,
                                                          double k_min,
                                                          double k_max,
                                                          std::uint32_t n_grid) {
  TotalSurfaceArbCounts counts{};
  const std::size_t n = s.n_slices();

  // Calendar: same sampling as arb_check_calendar, count-only.
  if (n >= 2 && n_grid > 0 && k_max > k_min) {
    const double dk = (k_max - k_min) / static_cast<double>(n_grid);
    for (std::uint32_t g = 0; g < n_grid; ++g) {
      const double k = k_min + static_cast<double>(g) * dk;
      double w_prev = -std::numeric_limits<double>::infinity();
      for (std::size_t i = 0; i < n; ++i) {
        const double T = slice_T_at(s, i);
        const double w = s.w(k, T);
        if (w + 1.0e-12 < w_prev) {
          ++counts.n_calendar;
        }
        w_prev = w;
      }
    }
  }

  // Butterfly: FD on total surface w, mirroring arb_check_butterfly.
  if (n > 0 && n_grid >= 4 && k_max > k_min) {
    const double dk = (k_max - k_min) / static_cast<double>(n_grid);
    const double inv_2dk = 0.5 / dk;
    const double inv_dksq = 1.0 / (dk * dk);
    for (std::size_t i = 0; i < n; ++i) {
      const double T = slice_T_at(s, i);
      if (!(T > 0.0)) {
        continue;
      }
      for (std::uint32_t g = 1; g < n_grid; ++g) {
        const double k = k_min + static_cast<double>(g) * dk;
        const double w_lo = s.w(k - dk, T);
        const double w_mi = s.w(k, T);
        const double w_hi = s.w(k + dk, T);
        if (!(w_mi > 1.0e-12) || !std::isfinite(w_lo) || !std::isfinite(w_hi)) {
          continue;
        }
        const double w_p = (w_hi - w_lo) * inv_2dk;
        const double w_pp = (w_hi - 2.0 * w_mi + w_lo) * inv_dksq;
        const double a = 1.0 - 0.5 * k * w_p / w_mi;
        const double t1 = a * a;
        const double t2 = 0.25 * w_p * w_p * (0.25 + 1.0 / w_mi);
        const double t3 = 0.5 * w_pp;
        if (t1 - t2 + t3 < -1.0e-9) {
          ++counts.n_butterfly;
        }
      }
    }
  }
  return Ok(counts);
}

// ── SVI-MM admissibility ─────────────────────────────────────────────────

SviMmAdmissibility arb_check_butterfly_svi_mm(const SviParams &slice,
                                              double T) noexcept {
  constexpr double kTol = 1.0e-12;
  std::uint32_t n_viol = 0;
  double worst = 0.0;

  // (1) b > 0
  if (!(slice.b > kTol)) {
    ++n_viol;
    const double slack = kTol - slice.b;
    if (slack > worst) {
      worst = slack;
    }
  }
  // (2) sigma > 0
  if (!(slice.sigma > kTol)) {
    ++n_viol;
    const double slack = kTol - slice.sigma;
    if (slack > worst) {
      worst = slack;
    }
  }
  // (3) |rho| < 1
  if (!(std::fabs(slice.rho) < 1.0 - kTol)) {
    ++n_viol;
    const double slack = std::fabs(slice.rho) - (1.0 - kTol);
    if (slack > worst) {
      worst = slack;
    }
  }
  // (4) a + b*sigma*sqrt(1 - rho^2) >= 0 (skipped when (3) fired: radical unreal)
  if (std::fabs(slice.rho) < 1.0) {
    const double w_min =
        slice.a + slice.b * slice.sigma * std::sqrt(1.0 - slice.rho * slice.rho);
    if (!(w_min >= -kTol)) {
      ++n_viol;
      const double slack = -w_min;
      if (slack > worst) {
        worst = slack;
      }
    }
  }
  // (5) Lee wing-slope bound. FT-C3: with w = TOTAL variance this bound is T-FREE
  // (b*(1+|rho|) <= 4), matching the eSSVI/Mingone-cube convention and the
  // svi_project_mm projector. The previous 4/T form was over-tight for T>1 and
  // vacuous for short T (moment-exploding short-dated wings passed the gate).
  if (T > 0.0) {
    const double lee_lhs = slice.b * (1.0 + std::fabs(slice.rho));
    const double lee_rhs = 4.0;
    if (!(lee_lhs <= lee_rhs + kTol)) {
      ++n_viol;
      const double slack = lee_lhs - lee_rhs;
      if (slack > worst) {
        worst = slack;
      }
    }
  }

  return {n_viol, worst};
}

Result<SviMmAdmissibility> arb_check_butterfly_svi_mm_surface(const VolSurface &s) {
  SviMmAdmissibility out{};
  // Only meaningful for SVI-MM; legacy SVI does not promise the polytope.
  if (s.param() != Parametrization::SviMm) {
    return Ok(out);
  }
  std::uint32_t total = 0;
  double max_slack = 0.0;
  for (const SviParams &slice : s.svi_slices()) {
    const SviMmAdmissibility a = arb_check_butterfly_svi_mm(slice, slice.T);
    total += a.n_violations;
    if (a.max_slack > max_slack) {
      max_slack = a.max_slack;
    }
  }
  out.n_violations = total;
  out.max_slack = max_slack;
  return Ok(out);
}

// ── Calendar-spread projection / repair ──────────────────────────────────

Result<CalendarPairProjection> arb_project_calendar_essvi_pair(
    EssviParams &current, const std::function<double(double)> &w_prev,
    double k_min, double k_max, std::uint32_t n_grid) {
  ATX_TRY(CalendarPairProjection out,
          validate_pair_projection_inputs(w_prev, k_min, k_max, n_grid));
  const SharedGridGap before = shared_grid_gap(
      w_prev, [&](double k) { return essvi_total_w(current, k); },
      k_min, k_max, n_grid);
  if (!before.finite) {
    return Err(ErrorCode::Unavailable,
               "eSSVI calendar pair projection: non-finite candidate");
  }
  out.max_deficit_before = before.max_deficit;
  for (std::uint32_t pass = 0; pass < 6; ++pass) {
    const SharedGridGap gap = shared_grid_gap(
        w_prev, [&](double k) { return essvi_total_w(current, k); },
        k_min, k_max, n_grid);
    if (gap.finite && gap.max_deficit <= kCalendarPairTol) {
      return Ok(out);
    }
    if (!gap.finite || !std::isfinite(gap.max_ratio) ||
        !(gap.max_ratio > 1.0)) {
      return Err(ErrorCode::Unavailable,
                 "eSSVI calendar pair projection: invalid scale");
    }
    const double scale = gap.max_ratio * (1.0 + 1.0e-9);
    current.theta *= scale;
    for (double &coef : current.resid_coef) {
      coef *= scale;
    }
    current.phi = std::min(current.phi,
                           essvi_phi_max(current.theta, current.rho));
    out.scale *= scale;
    ++out.passes;
  }
  const SharedGridGap final_gap = shared_grid_gap(
      w_prev, [&](double k) { return essvi_total_w(current, k); },
      k_min, k_max, n_grid);
  if (!final_gap.finite || final_gap.max_deficit > kCalendarPairTol) {
    return Err(ErrorCode::Unavailable,
               "eSSVI calendar pair projection did not converge");
  }
  return Ok(out);
}

Result<CalendarPairProjection> arb_project_calendar_svi_pair(
    SviParams &current, const std::function<double(double)> &w_prev,
    double k_min, double k_max, std::uint32_t n_grid) {
  ATX_TRY(CalendarPairProjection out,
          validate_pair_projection_inputs(w_prev, k_min, k_max, n_grid));
  const SharedGridGap before = shared_grid_gap(
      w_prev, [&](double k) { return svi_total_w(current, k); },
      k_min, k_max, n_grid);
  if (!before.finite) {
    return Err(ErrorCode::Unavailable,
               "SVI calendar pair projection: non-finite candidate");
  }
  out.max_deficit_before = before.max_deficit;
  if (before.max_deficit > kCalendarPairTol) {
    current.a += before.max_deficit + 1.0e-9;
    out.passes = 1;
  }
  const SharedGridGap final_gap = shared_grid_gap(
      w_prev, [&](double k) { return svi_total_w(current, k); },
      k_min, k_max, n_grid);
  if (!final_gap.finite || final_gap.max_deficit > kCalendarPairTol) {
    return Err(ErrorCode::Unavailable,
               "SVI calendar pair projection did not converge");
  }
  return Ok(out);
}

Result<CalendarPairProjection> arb_project_calendar_c8_pair(
    C8Params &current, const std::function<double(double)> &w_prev,
    double k_min, double k_max, std::uint32_t n_grid) {
  ATX_TRY(CalendarPairProjection out,
          validate_pair_projection_inputs(w_prev, k_min, k_max, n_grid));
  const SharedGridGap before = shared_grid_gap(
      w_prev, [&](double k) { return c8_slice_w(current, k); },
      k_min, k_max, n_grid);
  if (!before.finite) {
    return Err(ErrorCode::Unavailable,
               "C8 calendar pair projection: non-finite candidate");
  }
  out.max_deficit_before = before.max_deficit;
  for (std::uint32_t pass = 0; pass < 6; ++pass) {
    const SharedGridGap gap = shared_grid_gap(
        w_prev, [&](double k) { return c8_slice_w(current, k); },
        k_min, k_max, n_grid);
    if (gap.finite && gap.max_deficit <= kCalendarPairTol) {
      return Ok(out);
    }
    if (!gap.finite || !std::isfinite(gap.max_deficit) ||
        !(gap.max_deficit > 0.0)) {
      return Err(ErrorCode::Unavailable,
                 "C8 calendar pair projection: invalid level shift");
    }
    // In JW coordinates, adding the same constant to v and v_min is exactly
    // a parallel shift of raw-SVI `a`: slopes, minimum location, and all compact
    // bump coefficients stay unchanged. This is the minimum shape-preserving
    // move, analogous to the raw-SVI `a` projection above.
    const double shift = gap.max_deficit + 1.0e-9;
    current.v += shift;
    current.v_min += shift;
    c8_arb_project(current);
    ++out.passes;
  }
  const SharedGridGap final_gap = shared_grid_gap(
      w_prev, [&](double k) { return c8_slice_w(current, k); },
      k_min, k_max, n_grid);
  if (!final_gap.finite || final_gap.max_deficit > kCalendarPairTol) {
    return Err(ErrorCode::Unavailable,
               "C8 calendar pair projection did not converge");
  }
  return Ok(out);
}

Status arb_project_calendar_svi(VolSurface &s, double k_min, double k_max,
                                std::uint32_t n_grid) {
  // SVI_MM shares the (a, b, rho, m, sigma) carrier; the parallel `a`-bump is
  // the smallest shape-preserving perturbation in either tag.
  if (s.param() != Parametrization::Svi && s.param() != Parametrization::SviMm) {
    return Ok();
  }
  const std::size_t n = s.n_slices();
  if (n < 2 || n_grid == 0) {
    return Ok();
  }
  if (!(k_max > k_min)) {
    return Err(ErrorCode::InvalidArgument,
               "arb_project_calendar_svi: require k_max > k_min");
  }

  const auto src = s.svi_slices();
  std::vector<SviParams> slices(src.begin(), src.end());

  constexpr double kSafety = 1.0e-9;  // absorbed in w-space against FP noise
  std::uint32_t ng = n_grid;
  for (int pass = 0; pass < 6; ++pass) {
    const double dk = (k_max - k_min) / static_cast<double>(ng);
    bool touched = false;
    for (std::size_t i = 1; i < n; ++i) {
      const SviParams &prev = slices[i - 1];
      SviParams &curr = slices[i];
      double max_def = 0.0;
      for (std::uint32_t g = 0; g <= ng; ++g) {
        const double k = k_min + static_cast<double>(g) * dk;
        const double def = svi_total_w(prev, k) - svi_total_w(curr, k);
        if (def > max_def) {
          max_def = def;
        }
      }
      if (max_def > 0.0) {
        curr.a += max_def + kSafety;
        touched = true;
      }
    }
    if (!touched && pass > 0) {
      break;  // idempotent on this grid
    }
    ng *= 4;
    if (ng > 65536) {
      break;  // hard cap
    }
  }
  return write_back_svi(s, slices);
}

Status arb_project_calendar_essvi(VolSurface &s, double k_min, double k_max,
                                  std::uint32_t n_grid) {
  if (s.param() != Parametrization::Essvi) {
    return Ok();
  }
  const std::size_t n = s.n_slices();
  if (n < 2 || n_grid == 0) {
    return Ok();
  }
  if (!(k_max > k_min)) {
    return Err(ErrorCode::InvalidArgument,
               "arb_project_calendar_essvi: require k_max > k_min");
  }

  const auto src = s.essvi_slices();
  std::vector<EssviParams> slices(src.begin(), src.end());

  constexpr double kSafety = 1.0 + 1.0e-9;  // multiplicative pad
  std::uint32_t ng = n_grid;
  for (int pass = 0; pass < 6; ++pass) {
    const double dk = (k_max - k_min) / static_cast<double>(ng);
    bool touched = false;
    for (std::size_t i = 1; i < n; ++i) {
      const EssviParams &prev = slices[i - 1];
      EssviParams &curr = slices[i];
      double max_ratio = 1.0;
      for (std::uint32_t gi = 0; gi <= ng; ++gi) {
        const double k = k_min + static_cast<double>(gi) * dk;
        // Backbone-only: the wing residual is a separate, smaller correction;
        // scaling theta to cover a neighbour's residual would lift the whole
        // curr smile (including ATM, where curr has no residual).
        const double w_prev = essvi_backbone_w(prev, k);
        const double w_curr = essvi_backbone_w(curr, k);
        if (!(w_curr > 1.0e-15) || !std::isfinite(w_prev)) {
          continue;
        }
        const double r = w_prev / w_curr;
        if (r > max_ratio) {
          max_ratio = r;
        }
      }
      if (max_ratio > 1.0) {
        curr.theta *= max_ratio * kSafety;
        // Re-clamp phi to the butterfly ceiling at the new theta.
        const double phi_hi = essvi_phi_max(curr.theta, curr.rho);
        if (curr.phi > phi_hi) {
          curr.phi = phi_hi;
        }
        touched = true;
      }
    }
    if (!touched && pass > 0) {
      break;
    }
    ng *= 4;
    if (ng > 65536) {
      break;
    }
  }
  return write_back_essvi(s, slices);
}

Status arb_project_calendar_essvi_total(VolSurface &s, double k_min,
                                        double k_max, std::uint32_t n_grid,
                                        double max_theta_bump) {
  (void)max_theta_bump;  // reserved — partition-of-unity path uses no theta bump
  if (s.param() != Parametrization::Essvi) {
    return Ok();
  }
  const std::size_t n = s.n_slices();
  if (n < 2 || n_grid == 0) {
    return Ok();
  }
  if (!(k_max > k_min)) {
    return Err(ErrorCode::InvalidArgument,
               "arb_project_calendar_essvi_total: require k_max > k_min");
  }

  const auto src = s.essvi_slices();
  std::vector<EssviParams> slices(src.begin(), src.end());

  // Absolute pad on the level shift; the downstream check uses 1e-12 tolerance.
  constexpr double kSafetyC = 1.0e-9;
  std::uint32_t ng = n_grid;
  for (int pass = 0; pass < 6; ++pass) {
    const double dk = (k_max - k_min) / static_cast<double>(ng);
    bool touched = false;
    for (std::size_t i = 1; i < n; ++i) {
      const EssviParams &prev = slices[i - 1];
      EssviParams &curr = slices[i];
      // The constant-shift trick requires a partition-of-unity basis.
      if (curr.resid_basis_kind != ResidualBasisKind::C2Bspline) {
        continue;
      }
      if (!(curr.resid_scale > 0.0)) {
        continue;
      }
      if (curr.resid_n_basis == 0) {
        continue;
      }

      double max_def = 0.0;
      for (std::uint32_t gi = 0; gi <= ng; ++gi) {
        const double k = k_min + static_cast<double>(gi) * dk;
        const double w_total_prev = essvi_total_w(prev, k);
        const double w_total_curr = essvi_total_w(curr, k);
        if (!std::isfinite(w_total_prev) || !std::isfinite(w_total_curr)) {
          continue;
        }
        const double def = w_total_prev - w_total_curr;
        if (def > max_def) {
          max_def = def;
        }
      }
      if (max_def > 0.0) {
        const double c_shift = max_def + kSafetyC;
        const int n_b = static_cast<int>(curr.resid_n_basis);
        const int n_b_clamped = (n_b > kEssviResidN) ? kEssviResidN : n_b;
        for (int j = 0; j < n_b_clamped; ++j) {
          curr.resid_coef[static_cast<std::size_t>(j)] += c_shift;
        }
        touched = true;
      }
    }
    if (!touched && pass > 0) {
      break;
    }
    ng *= 4;
    if (ng > 65536) {
      break;
    }
  }
  return write_back_essvi(s, slices);
}

Status arb_repair_calendar_residual(VolSurface &s, double k_min, double k_max,
                                    std::uint32_t n_grid) {
  if (s.param() != Parametrization::Essvi) {
    return Ok();
  }
  const std::size_t n = s.n_slices();
  if (n < 2 || n_grid == 0) {
    return Ok();
  }
  if (!(k_max > k_min)) {
    return Err(ErrorCode::InvalidArgument,
               "arb_repair_calendar_residual: require k_max > k_min");
  }

  const auto src = s.essvi_slices();
  std::vector<EssviParams> slices(src.begin(), src.end());

  for (int outer = 0; outer < 5; ++outer) {
    bool touched = false;
    for (std::size_t i = 0; i + 1 < n; ++i) {
      EssviParams &lo = slices[i];
      const EssviParams &hi = slices[i + 1];
      if (!(lo.resid_scale > 0.0)) {
        continue;  // nothing to damp
      }
      // Quick feasibility: if alpha = 1 is already monotone, skip.
      if (total_calendar_deficit_with_alpha(lo, hi, 1.0, k_min, k_max, n_grid) <=
          0.0) {
        continue;
      }
      // VERIFY the bisection's lower endpoint instead of assuming it. alpha = 0
      // collapses `lo` to its backbone, which is monotone against `hi` only once
      // arb_project_calendar_essvi has run (the documented precondition). When it
      // has not — or the projection could not close the pair — alpha = 0 does not
      // repair the crossing, and the bisection below would converge on a_lo = 0,
      // commit it (erasing `lo`'s whole residual layer) and report Ok on a surface
      // that is still calendar-arbitrageable. Damping can only pull `lo` DOWN
      // toward that backbone, so no larger alpha helps either: the pair is
      // unrepairable by this operator. Fail before touching anything.
      if (total_calendar_deficit_with_alpha(lo, hi, 0.0, k_min, k_max, n_grid) > 0.0) {
        // TRANSACTIONAL, like every repair in this file: the sweeps mutate the
        // local `slices` copy and only `write_back_essvi` commits, so returning
        // here leaves the caller's surface exactly as it was passed in.
        return Err(ErrorCode::Unavailable,
                   "arb_repair_calendar_residual: backbone crossing survives full "
                   "residual damping (run arb_project_calendar_essvi first)");
      }
      double a_lo = 0.0;  // feasible (verified above)
      double a_hi = 1.0;  // infeasible
      for (int it = 0; it < 20; ++it) {
        const double a_mid = 0.5 * (a_lo + a_hi);
        const double def =
            total_calendar_deficit_with_alpha(lo, hi, a_mid, k_min, k_max, n_grid);
        if (def <= 0.0) {
          a_lo = a_mid;
        } else {
          a_hi = a_mid;
        }
        if (a_hi - a_lo < 1.0e-4) {
          break;
        }
      }
      // Commit the largest feasible alpha.
      for (int j = 0; j < kEssviResidN; ++j) {
        lo.resid_coef[static_cast<std::size_t>(j)] *= a_lo;
      }
      if (a_lo <= 1.0e-12) {
        lo.resid_scale = 0.0;
        lo.resid_basis_kind = ResidualBasisKind::None;
      }
      touched = true;
    }
    if (!touched) {
      break;
    }
  }
  return write_back_essvi(s, slices);
}

// ── Quote-level pre-fit filters ──────────────────────────────────────────

FilterOpts filter_default_opts() noexcept { return FilterOpts{}; }

Result<std::uint32_t> arb_filter_quotes_ex(const QuoteBatch &batch,
                                           const FilterOpts &opts,
                                           std::span<const double> vegas,
                                           std::span<std::uint8_t> flags_out) {
  const std::size_t count = batch.bids.size();
  if (batch.asks.size() != count) {
    return Err(ErrorCode::InvalidArgument,
               "arb_filter_quotes_ex: bids/asks length mismatch");
  }
  if (flags_out.size() < count) {
    return Err(ErrorCode::InvalidArgument,
               "arb_filter_quotes_ex: flags_out shorter than batch");
  }
  if (!batch.flags.empty() && batch.flags.size() < count) {
    return Err(ErrorCode::InvalidArgument,
               "arb_filter_quotes_ex: flags column shorter than batch");
  }
  if (!batch.ts_ns.empty() && batch.ts_ns.size() < count) {
    return Err(ErrorCode::InvalidArgument,
               "arb_filter_quotes_ex: ts_ns column shorter than batch");
  }
  if (!vegas.empty() && vegas.size() < count) {
    return Err(ErrorCode::InvalidArgument,
               "arb_filter_quotes_ex: vegas column shorter than batch");
  }

  const std::int64_t stale_cutoff_ns =
      (opts.now_ts_ns > 0 && opts.stale_seconds > 0)
          ? opts.now_ts_ns - opts.stale_seconds * kNsPerSecond
          : 0;

  std::uint32_t flagged = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const QuoteFlag f0 =
        batch.flags.empty() ? QuoteFlag::None
                            : static_cast<QuoteFlag>(batch.flags[i]);
    QuoteFlag f = f0;

    const double bid = batch.bids[i];
    const double ask = batch.asks[i];
    const double mid = 0.5 * (bid + ask);

    // Locked / crossed.
    if (bid > ask) {
      f |= QuoteFlag::Crossed;
    } else if (bid >= ask) {
      f |= QuoteFlag::Locked;
    }

    // Wide spread — meaningless on locked/crossed quotes, so skip those.
    if (bid > 0.0 && ask > bid && opts.wide_spread_pct > 0.0) {
      const double denom = (mid > opts.wide_min_mid) ? mid : opts.wide_min_mid;
      const double rel = (ask - bid) / denom;
      if (rel > opts.wide_spread_pct) {
        f |= QuoteFlag::WideSpread;
      }
    }

    // Penny — an empty (bid == 0) quote intentionally trips PENNY.
    if (bid < opts.penny_floor) {
      f |= QuoteFlag::Penny;
    }

    // Stale — only if a reference instant and a per-quote timestamp exist.
    if (stale_cutoff_ns != 0 && !batch.ts_ns.empty() && batch.ts_ns[i] != 0 &&
        batch.ts_ns[i] < stale_cutoff_ns) {
      f |= QuoteFlag::Stale;
    }

    // Low vega — only when the caller supplied a vega column.
    if (!vegas.empty() && opts.min_vega_filter > 0.0 &&
        vegas[i] < opts.min_vega_filter) {
      f |= QuoteFlag::LowVega;
    }

    if (f != f0) {
      ++flagged;
    }
    flags_out[i] = to_u8(f);
  }
  return Ok(flagged);
}

Result<std::uint32_t> prefit_filter_underlier(Underlying &under,
                                              const CurveSet &curves,
                                              const FilterOpts &opts,
                                              std::int64_t now_ns) {
  (void)curves;  // reserved: predicates need no curve context (matches C's cs)

  const std::int64_t ref_ts = (now_ns != 0) ? now_ns : opts.now_ts_ns;
  const std::int64_t stale_cutoff_ns =
      (ref_ts > 0 && opts.stale_seconds > 0)
          ? ref_ts - opts.stale_seconds * kNsPerSecond
          : 0;

  std::uint32_t flagged = 0;
  for (Chain &c : under.chains) {
    const std::size_t n_strikes = c.n_strikes();
    for (std::size_t si = 0; si < n_strikes; ++si) {
      for (int side = 0; side < 2; ++side) {
        const Side sd = (side == 0) ? Side::Call : Side::Put;
        const std::size_t idx = chain_index(static_cast<std::uint16_t>(si), sd);

        const QuoteFlag f0 = static_cast<QuoteFlag>(c.flags[idx]);
        QuoteFlag f = f0;

        const double bid = c.bids[idx];
        const double ask = c.asks[idx];
        const double mid = 0.5 * (bid + ask);

        if (bid > ask) {
          f |= QuoteFlag::Crossed;
        } else if (bid >= ask) {
          f |= QuoteFlag::Locked;
        }

        if (bid > 0.0 && ask > bid && opts.wide_spread_pct > 0.0) {
          const double denom = (mid > opts.wide_min_mid) ? mid : opts.wide_min_mid;
          const double rel = (ask - bid) / denom;
          if (rel > opts.wide_spread_pct) {
            f |= QuoteFlag::WideSpread;
          }
        }

        if (bid < opts.penny_floor) {
          f |= QuoteFlag::Penny;
        }

        if (stale_cutoff_ns != 0 && c.ts_ns[idx] != 0 &&
            c.ts_ns[idx] < stale_cutoff_ns) {
          f |= QuoteFlag::Stale;
        }

        if (f != f0) {
          ++flagged;
        }
        c.flags[idx] = to_u8(f);
      }
    }
  }
  return Ok(flagged);
}

}  // namespace atx::vol
