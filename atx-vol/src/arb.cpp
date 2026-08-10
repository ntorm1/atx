#include "atx/vol/arb.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string> // budget-refusal diagnostics (std::to_string)
#include <vector>

#include "atx/core/error.hpp"
#include "atx/vol/c8.hpp"
#include "atx/vol/rates_curve.hpp"
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

struct DensePricePoint {
  double strike{};
  double call{};
};

[[nodiscard]] std::vector<ArbViolation>
butterfly_check_convex_dense(const ConvexDenseCurve &curve, double k_min,
                             double k_max) {
  std::vector<ArbViolation> out;
  const ConvexSliceFit &fit = curve.fit();
  const auto reject_non_finite = [&](double k) {
    out.push_back(ArbViolation{k, curve.T(), curve.T(),
                               std::numeric_limits<double>::infinity(),
                               ArbViolation::Kind::Butterfly});
  };
  if (!(fit.F > 0.0) || !(fit.T > 0.0) || !(fit.df > 0.0) ||
      fit.u.size() < 2u || fit.C.size() != fit.u.size()) {
    reject_non_finite(k_min);
    return out;
  }
  for (std::size_t i = 0; i < fit.u.size(); ++i) {
    if (!(fit.u[i] > 0.0) || !std::isfinite(fit.u[i]) ||
        !std::isfinite(fit.C[i]) ||
        (i > 0u && !(fit.u[i] > fit.u[i - 1u]))) {
      reject_non_finite(k_min);
      return out;
    }
  }

  const double strike_min = fit.F * std::exp(k_min);
  const double strike_max = fit.F * std::exp(k_max);
  if (!(strike_min > 0.0) || !(strike_max > strike_min) ||
      !std::isfinite(strike_min) || !std::isfinite(strike_max)) {
    reject_non_finite(k_min);
    return out;
  }

  std::vector<DensePricePoint> points;
  points.reserve(fit.u.size() + 2u);
  points.push_back(DensePricePoint{strike_min, fit.call_price(strike_min)});
  const double endpoint_tol =
      32.0 * std::numeric_limits<double>::epsilon() *
      std::max({1.0, std::fabs(strike_min), std::fabs(strike_max)});
  for (const double strike : fit.u) {
    if (strike > strike_min + endpoint_tol &&
        strike < strike_max - endpoint_tol) {
      points.push_back(DensePricePoint{strike, fit.call_price(strike)});
    }
  }
  points.push_back(DensePricePoint{strike_max, fit.call_price(strike_max)});

  for (const DensePricePoint &point : points) {
    if (!std::isfinite(point.call)) {
      reject_non_finite(std::log(point.strike / fit.F));
      return out;
    }
  }
  if (points.size() < 3u) {
    return out;
  }

  double previous_slope =
      (points[1].call - points[0].call) /
      (points[1].strike - points[0].strike);
  if (!std::isfinite(previous_slope)) {
    reject_non_finite(std::log(points[1].strike / fit.F));
    return out;
  }
  for (std::size_t i = 1u; i + 1u < points.size(); ++i) {
    const double right_width = points[i + 1u].strike - points[i].strike;
    const double left_width = points[i].strike - points[i - 1u].strike;
    const double slope =
        (points[i + 1u].call - points[i].call) / right_width;
    if (!(left_width > 0.0) || !(right_width > 0.0) ||
        !std::isfinite(slope)) {
      reject_non_finite(std::log(points[i].strike / fit.F));
      return out;
    }

    // The first term admits the dense QP's 1e-8 scaled feasibility floor; the
    // epsilon term scales subtraction roundoff by the local price/strike
    // resolution. Honest fits therefore clear their own certificate tolerance,
    // while an economically meaningful fall in the slope sequence remains a
    // positive convexity breach.
    const double slope_scale =
        std::max({1.0, std::fabs(fit.df), std::fabs(previous_slope),
                  std::fabs(slope)});
    const double price_scale =
        std::max({1.0, std::fabs(points[i - 1u].call),
                  std::fabs(points[i].call), std::fabs(points[i + 1u].call)});
    const double roundoff_tol =
        64.0 * std::numeric_limits<double>::epsilon() * price_scale /
        std::min(left_width, right_width);
    const double tolerance = 1.0e-8 * slope_scale + roundoff_tol;
    const double slack = previous_slope - slope;
    if (slack > tolerance) {
      out.push_back(ArbViolation{std::log(points[i].strike / fit.F), curve.T(),
                                 curve.T(), slack,
                                 ArbViolation::Kind::Butterfly});
    }
    previous_slope = slope;
  }
  return out;
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

// ── Real roots of a polynomial of degree <= 4 ────────────────────────────
//
// Coefficients ASCENDING: p(x) = sum_{i=0}^{deg} c[i]*x^i.
//
// Isolation by critical points rather than Ferrari's closed form. The roots of
// p' (one degree lower, found by the same routine) cut R into intervals on
// which p is strictly monotone, so each holds at most one root and any sign
// change across one brackets it; bisection inside a bracket then converges
// unconditionally. That costs a few hundred flops rather than ~50 and buys the
// property that matters here: no branch of it can silently return the wrong
// root COUNT. Ferrari's resolvent cubic cancels worst exactly where this
// quartic is worst conditioned — at a near-tangency, which is precisely the
// narrow crossing a grid scan already misses.
//
// An exact double root is the one case not enumerated (p touches zero without
// changing sign and the node value is not bit-exactly 0). For the calendar
// question that is harmless: a tangency of w_lo - w_hi does not separate two
// sign regimes, so the interval decomposition below is unaffected.

constexpr std::size_t kMaxPolyDegree = 4;
using PolyCoefs = std::array<double, kMaxPolyDegree + 1>;

struct PolyRoots {
  std::array<double, kMaxPolyDegree> r{};
  std::uint8_t n{};
  void push(double x) noexcept {
    // A degree-<=4 polynomial has at most 4 distinct real roots, so the cap is
    // unreachable; dropping rather than growing keeps the type allocation-free.
    if (n < kMaxPolyDegree) {
      r[n] = x;
      ++n;
    }
  }
};

[[nodiscard]] double poly_eval(const PolyCoefs &c, std::size_t deg,
                               double x) noexcept {
  double v = c[deg];
  for (std::size_t i = deg; i-- > 0;) {
    v = v * x + c[i];
  }
  return v;
}

// Cauchy's bound: every real root satisfies |x| <= 1 + max_{i<deg}|c_i|/|c_deg|.
[[nodiscard]] double poly_root_bound(const PolyCoefs &c, std::size_t deg) noexcept {
  const double lead = std::fabs(c[deg]);
  if (!(lead > 0.0)) {
    return 0.0;
  }
  double worst = 0.0;
  for (std::size_t i = 0; i < deg; ++i) {
    worst = std::max(worst, std::fabs(c[i]));
  }
  const double bound = 1.0 + worst / lead;
  return std::isfinite(bound) ? bound : 0.0;
}

[[nodiscard]] double poly_bisect(const PolyCoefs &c, std::size_t deg, double lo,
                                 double hi, double f_lo) noexcept {
  // Bounded: each pass halves the bracket and the adjacency test exits once
  // `mid` can no longer sit strictly inside it (~60 passes for doubles).
  for (int it = 0; it < 200; ++it) {
    const double mid = 0.5 * (lo + hi);
    if (!(mid > lo) || !(mid < hi)) {
      break;
    }
    const double f_mid = poly_eval(c, deg, mid);
    if (f_mid == 0.0) {
      return mid;
    }
    if ((f_mid > 0.0) == (f_lo > 0.0)) {
      lo = mid;
      f_lo = f_mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

// Recursion depth is statically bounded by kMaxPolyDegree — each level takes
// one derivative — so this satisfies the bounded-recursion rule.
void poly_real_roots(const PolyCoefs &c, std::size_t deg, PolyRoots &out) noexcept {
  if (deg == 0) {
    return;  // constant: no root, or identically zero — the caller's problem
  }
  if (deg == 1) {
    if (std::fabs(c[1]) > 0.0) {
      const double x = -c[0] / c[1];
      if (std::isfinite(x)) {
        out.push(x);
      }
    }
    return;
  }

  PolyCoefs d{};
  for (std::size_t i = 0; i < deg; ++i) {
    d[i] = static_cast<double>(i + 1) * c[i + 1];
  }
  PolyRoots crit{};
  poly_real_roots(d, deg - 1, crit);
  std::sort(crit.r.begin(), crit.r.begin() + crit.n);

  const double bound = poly_root_bound(c, deg);
  if (!(bound > 0.0)) {
    return;
  }

  std::array<double, kMaxPolyDegree + 2> node{};
  std::size_t n_node = 0;
  node[n_node++] = -bound;
  for (std::uint8_t i = 0; i < crit.n; ++i) {
    if (crit.r[i] > -bound && crit.r[i] < bound) {
      node[n_node++] = crit.r[i];
    }
  }
  node[n_node++] = bound;

  double f_prev = poly_eval(c, deg, node[0]);
  if (f_prev == 0.0) {
    out.push(node[0]);
  }
  for (std::size_t i = 1; i < n_node; ++i) {
    const double f_cur = poly_eval(c, deg, node[i]);
    if (f_cur == 0.0) {
      out.push(node[i]);
    } else if (f_prev != 0.0 && ((f_prev > 0.0) != (f_cur > 0.0))) {
      out.push(poly_bisect(c, deg, node[i - 1], node[i], f_prev));
    }
    f_prev = f_cur;
  }
}

// Relative gate on |w_lo - w_hi| for accepting a quartic root as a genuine
// crossing. The three spurious factors put D at 2*B_1, -2*B_2 or 2*(B_1 - B_2)
// there; the first two are bounded below by 2*b_i*sigma_i and the third is
// small only where L is small too, i.e. only where the pair really does nearly
// meet. Orders of magnitude of separation, so the exact threshold is not
// delicate — but it must exist, because a Newton-polished genuine root lands at
// ~1e-16 relative and a spurious one does not move at all.
constexpr double kCrossingResidualRelTol = 1.0e-7;

}  // namespace

std::optional<SviCrossingSlice>
SviCrossingSlice::from_svi(const SviParams &p) noexcept {
  if (!std::isfinite(p.a) || !std::isfinite(p.b) || !std::isfinite(p.rho) ||
      !std::isfinite(p.m) || !std::isfinite(p.sigma)) {
    return std::nullopt;
  }
  if (!(p.b >= 0.0) || !(p.sigma >= 0.0) || !(std::fabs(p.rho) < 1.0)) {
    return std::nullopt;
  }
  SviCrossingSlice out;
  out.a_ = p.a;
  out.b_ = p.b;
  out.rho_ = p.rho;
  out.m_ = p.m;
  out.sigma_ = p.sigma;
  out.T_ = p.T;
  return out;
}

std::optional<SviCrossingSlice>
SviCrossingSlice::from_essvi_backbone(const EssviParams &p) noexcept {
  if (essvi_rho_blend_armed(p) || p.resid_scale > 0.0) {
    return std::nullopt;
  }
  if (!std::isfinite(p.theta) || !std::isfinite(p.phi) || !std::isfinite(p.rho)) {
    return std::nullopt;
  }
  if (!(p.theta > 0.0) || !(p.phi > 0.0) || !(std::fabs(p.rho) < 1.0)) {
    return std::nullopt;
  }
  const double one_minus_rho2 = 1.0 - p.rho * p.rho;
  SviCrossingSlice out;
  out.a_ = 0.5 * p.theta * one_minus_rho2;
  out.b_ = 0.5 * p.theta * p.phi;
  out.rho_ = p.rho;
  out.m_ = -p.rho / p.phi;
  out.sigma_ = std::sqrt(one_minus_rho2) / p.phi;
  out.T_ = p.T;
  return out;
}

double SviCrossingSlice::w(double k) const noexcept {
  const double dk = k - m_;
  return a_ + b_ * (rho_ * dk + std::sqrt(dk * dk + sigma_ * sigma_));
}

double SviCrossingSlice::dw_dk(double k) const noexcept {
  const double dk = k - m_;
  const double r = std::sqrt(dk * dk + sigma_ * sigma_);
  // sigma_ == 0 gives a kink at k == m_ where the derivative does not exist;
  // report the right-hand slope there rather than a NaN, which is all the
  // Newton polish below needs (it is safeguarded on the residual).
  const double u = (r > 0.0) ? (dk / r) : 1.0;
  return b_ * (rho_ + u);
}

namespace {

[[nodiscard]] double crossing_residual(const SviCrossingSlice &lo,
                                       const SviCrossingSlice &hi,
                                       double k) noexcept {
  return lo.w(k) - hi.w(k);
}

// Newton on D itself. The quartic LOCATED the root; D is far better conditioned
// than P near a near-tangency and is the function the answer is about, so the
// last few digits are earned here rather than there. Safeguarded: a step that
// does not reduce |D| is rejected and the iteration stops.
[[nodiscard]] double polish_crossing(const SviCrossingSlice &lo,
                                     const SviCrossingSlice &hi,
                                     double k) noexcept {
  double f = crossing_residual(lo, hi, k);
  for (int it = 0; it < 8; ++it) {
    const double g = lo.dw_dk(k) - hi.dw_dk(k);
    if (!(std::fabs(g) > 1.0e-300)) {
      break;
    }
    const double step = f / g;
    const double k_next = k - step;
    if (!std::isfinite(k_next)) {
      break;
    }
    const double f_next = crossing_residual(lo, hi, k_next);
    if (!(std::fabs(f_next) < std::fabs(f))) {
      break;
    }
    k = k_next;
    f = f_next;
    if (std::fabs(step) <= 1.0e-16 * (1.0 + std::fabs(k))) {
      break;
    }
  }
  return k;
}

}  // namespace

SliceCrossings svi_pair_crossings(const SviCrossingSlice &lo,
                                  const SviCrossingSlice &hi) noexcept {
  SliceCrossings out{};

  // L(k) = p*k + q, the non-radical part of w_lo - w_hi.
  const double p = lo.b() * lo.rho() - hi.b() * hi.rho();
  const double q = (lo.a() - hi.a()) - lo.b() * lo.rho() * lo.m() +
                   hi.b() * hi.rho() * hi.m();

  // B_i^2 = u2*k^2 + u1*k + u0 (and v* for the second slice).
  const double b1sq = lo.b() * lo.b();
  const double b2sq = hi.b() * hi.b();
  const double u2 = b1sq;
  const double u1 = -2.0 * b1sq * lo.m();
  const double u0 = b1sq * (lo.m() * lo.m() + lo.sigma() * lo.sigma());
  const double v2 = b2sq;
  const double v1 = -2.0 * b2sq * hi.m();
  const double v0 = b2sq * (hi.m() * hi.m() + hi.sigma() * hi.sigma());

  // Q = L^2 - B_1^2 - B_2^2.
  const double Q2 = p * p - u2 - v2;
  const double Q1 = 2.0 * p * q - u1 - v1;
  const double Q0 = q * q - u0 - v0;

  // P = Q^2 - 4*B_1^2*B_2^2.
  PolyCoefs c{};
  c[4] = Q2 * Q2 - 4.0 * u2 * v2;
  c[3] = 2.0 * Q2 * Q1 - 4.0 * (u2 * v1 + u1 * v2);
  c[2] = Q1 * Q1 + 2.0 * Q2 * Q0 - 4.0 * (u2 * v0 + u1 * v1 + u0 * v2);
  c[1] = 2.0 * Q1 * Q0 - 4.0 * (u1 * v0 + u0 * v1);
  c[0] = Q0 * Q0 - 4.0 * u0 * v0;

  double scale = 0.0;
  for (const double ci : c) {
    if (!std::isfinite(ci)) {
      return out;  // the pair is not exactly decidable; caller falls back
    }
    scale = std::max(scale, std::fabs(ci));
  }
  if (!(scale > 0.0)) {
    return out;  // P is identically zero: the two slices coincide, no crossing
  }
  for (double &ci : c) {
    ci /= scale;
  }
  // Strip a negligible leading coefficient: a root lost that way sits at
  // |k| ~ 1/eps, past any log-moneyness a surface is defined on, and the
  // back-substitution below would reject it in any case.
  std::size_t deg = kMaxPolyDegree;
  while (deg > 0 && std::fabs(c[deg]) <= 1.0e-14) {
    --deg;
  }

  PolyRoots roots{};
  poly_real_roots(c, deg, roots);
  std::sort(roots.r.begin(), roots.r.begin() + roots.n);

  // Accept into scratch first, then sort and dedupe GLOBALLY. Newton polish
  // moves a root, and a SPURIOUS root can migrate onto a genuine crossing
  // somewhere else entirely — measured on a randomised benchmark pair, the
  // spurious root at k = -0.406 converged to the genuine crossing at k = +2.170
  // and was (correctly) accepted there. So the accepted set is neither in the
  // quartic's order nor duplicate-free, and comparing only against the previous
  // entry both admits duplicates and leaves `out.k` unsorted — which the
  // interval decomposition below reads as ascending and silently mis-partitions.
  std::array<double, kMaxPolyDegree> accepted{};
  std::uint8_t n_accepted = 0;
  for (std::uint8_t i = 0; i < roots.n; ++i) {
    const double k = polish_crossing(lo, hi, roots.r[i]);
    if (!std::isfinite(k)) {
      continue;
    }
    const double w_lo = lo.w(k);
    const double w_hi = hi.w(k);
    if (!std::isfinite(w_lo) || !std::isfinite(w_hi)) {
      continue;
    }
    const double w_scale = std::max({1.0e-6, std::fabs(w_lo), std::fabs(w_hi)});
    if (std::fabs(w_lo - w_hi) > kCrossingResidualRelTol * w_scale) {
      continue;  // spurious: introduced by one of the three other sign factors
    }
    accepted[n_accepted] = k;
    ++n_accepted;
  }
  std::sort(accepted.begin(), accepted.begin() + n_accepted);
  for (std::uint8_t i = 0; i < n_accepted; ++i) {
    const double k = accepted[i];
    if (out.n > 0 &&
        std::fabs(k - out.k[out.n - 1u]) <= 1.0e-9 * (1.0 + std::fabs(k))) {
      continue;
    }
    out.k[out.n] = k;
    ++out.n;
  }
  return out;
}

std::vector<CalendarInterval>
svi_pair_calendar_intervals(const SviCrossingSlice &lo,
                            const SviCrossingSlice &hi, double tol) {
  std::vector<CalendarInterval> out;
  const SliceCrossings x = svi_pair_crossings(lo, hi);
  constexpr double kInf = std::numeric_limits<double>::infinity();

  // One decisive probe per sign region: D has no zero strictly between two
  // consecutive crossings (nor beyond the outermost ones), so its sign there is
  // settled by a single evaluation. That is what makes the tails answerable at
  // all — no finite grid can certify them.
  const double span =
      (x.n >= 2) ? std::max(1.0, x.k[x.n - 1u] - x.k[0]) : 1.0;

  for (std::uint8_t i = 0; i <= x.n; ++i) {
    const double k_lo = (i == 0) ? -kInf : x.k[i - 1u];
    const double k_hi = (i == x.n) ? kInf : x.k[i];
    double probe = 0.0;
    if (i == 0 && i == x.n) {
      probe = 0.0;  // no crossing anywhere: any point decides all of R
    } else if (i == 0) {
      probe = k_hi - span;
    } else if (i == x.n) {
      probe = k_lo + span;
    } else {
      probe = 0.5 * (k_lo + k_hi);
    }
    if (!(crossing_residual(lo, hi, probe) > tol)) {
      continue;
    }
    // Sharpen the witness inside a BOUNDED interval — `slack` is documented as
    // a lower bound, and a bounded sweep makes it a useful one. The tails keep
    // their single decisive probe: the deficit there can grow without bound and
    // "the worst point" is not a meaningful thing to report.
    CalendarInterval iv{};
    iv.k_lo = k_lo;
    iv.k_hi = k_hi;
    iv.k_witness = probe;
    iv.slack = crossing_residual(lo, hi, probe);
    if (std::isfinite(k_lo) && std::isfinite(k_hi)) {
      constexpr int kWitnessSamples = 15;
      for (int s = 1; s <= kWitnessSamples; ++s) {
        const double t = static_cast<double>(s) /
                         static_cast<double>(kWitnessSamples + 1);
        const double ks = k_lo + t * (k_hi - k_lo);
        const double d = crossing_residual(lo, hi, ks);
        if (d > iv.slack) {
          iv.slack = d;
          iv.k_witness = ks;
        }
      }
    }
    out.push_back(iv);
  }
  return out;
}

namespace {

// Every slice of `s` as an exactly-decidable crossing slice, or nullopt if any
// slice refuses or the maturities are not strictly increasing. Decided ONCE per
// surface so a single `arb_check_calendar` call never mixes exact and sampled
// semantics across its pairs.
[[nodiscard]] std::optional<std::vector<SviCrossingSlice>>
exact_crossing_slices(const VolSurface &s) {
  const std::size_t n = s.n_slices();
  if (n < 2) {
    return std::nullopt;
  }
  std::vector<SviCrossingSlice> out;
  out.reserve(n);
  switch (s.param()) {
  case Parametrization::Svi:
  case Parametrization::SviMm:
    for (const SviParams &sl : s.svi_slices()) {
      const std::optional<SviCrossingSlice> cs = SviCrossingSlice::from_svi(sl);
      if (!cs.has_value()) {
        return std::nullopt;
      }
      out.push_back(*cs);
    }
    break;
  case Parametrization::Essvi:
    for (const EssviParams &sl : s.essvi_slices()) {
      const std::optional<SviCrossingSlice> cs =
          SviCrossingSlice::from_essvi_backbone(sl);
      if (!cs.has_value()) {
        return std::nullopt;
      }
      out.push_back(*cs);
    }
    break;
  case Parametrization::Wing:
  case Parametrization::C8:
  case Parametrization::CStar16M:
    return std::nullopt;
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (!(out[i].T() > 0.0) || !std::isfinite(out[i].T())) {
      return std::nullopt;
    }
    if (i > 0 && !(out[i].T() > out[i - 1].T())) {
      return std::nullopt;  // duplicate / unsorted T: pair order is not defined
    }
  }
  return out;
}

// Absolute deficit below which a crossing is FP noise rather than arbitrage.
// Same number the sampled regime has always used, so the two regimes agree on
// what counts as a violation and only differ in how they look for one.
constexpr double kCalendarExactTol = 1.0e-12;

[[nodiscard]] std::vector<ArbViolation>
exact_calendar_violations(const std::vector<SviCrossingSlice> &slices,
                          double k_min, double k_max) {
  std::vector<ArbViolation> out;
  for (std::size_t i = 1; i < slices.size(); ++i) {
    const SviCrossingSlice &prev = slices[i - 1];
    const SviCrossingSlice &curr = slices[i];
    for (const CalendarInterval &iv :
         svi_pair_calendar_intervals(prev, curr, kCalendarExactTol)) {
      const double lo = std::max(iv.k_lo, k_min);
      const double hi = std::min(iv.k_hi, k_max);
      if (!(hi >= lo)) {
        continue;  // the violating interval lies wholly outside the band
      }
      // Re-witness inside the CLIPPED interval: the unclipped witness can sit
      // outside the band, and a violation record whose k is not in the band the
      // caller asked about is a lie about where the surface is broken.
      double k_best = std::clamp(iv.k_witness, lo, hi);
      double slack = prev.w(k_best) - curr.w(k_best);
      constexpr int kClipSamples = 16;
      for (int sidx = 0; sidx <= kClipSamples; ++sidx) {
        const double t =
            static_cast<double>(sidx) / static_cast<double>(kClipSamples);
        // Clamped, not just computed: `lo + 1.0*(hi - lo)` rounds ABOVE `hi`,
        // which would hand the caller a violation stamped at a k outside the
        // band it asked about.
        const double ks = std::clamp(lo + t * (hi - lo), lo, hi);
        const double d = prev.w(ks) - curr.w(ks);
        if (d > slack) {
          slack = d;
          k_best = ks;
        }
      }
      if (!(slack > kCalendarExactTol)) {
        continue;  // clipped away to nothing
      }
      out.push_back(ArbViolation{k_best, prev.T(), curr.T(), slack,
                                 ArbViolation::Kind::Calendar});
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

  // EXACT regime when the surface admits it — see arb.hpp for the two regimes
  // and what each records. Applicability is a property of the whole surface, so
  // one call is never half exact and half sampled.
  if (k_max > k_min) {
    if (const std::optional<std::vector<SviCrossingSlice>> exact =
            exact_crossing_slices(s);
        exact.has_value()) {
      return Ok(exact_calendar_violations(*exact, k_min, k_max));
    }
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
      // A non-finite w is UNCOMPARABLE: not a violation, and — the defect this
      // guards — not a BASELINE either. Assigning it to w_prev DISCARDED the
      // last usable comparison point: the next slice's `w + 1e-12 < NaN` is
      // false (NaN compares unordered), so the crossing that SPANS the unusable
      // slice was never tested and the surface came back clean. Skipping keeps
      // the last FINITE (w, T) as the baseline, so that crossing is reported
      // and carries the maturities of the two finite slices it actually spans.
      // Reporting nothing for the offending slice itself matches the
      // CurveSurface overload below, which already continues past a non-finite
      // w on either side ("wing coverage gap — nothing to compare").
      if (!std::isfinite(w)) {
        continue;
      }
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

DeltaBand delta_band_from_atm_w(double w_atm, double z) noexcept {
  if (!std::isfinite(w_atm) || !(w_atm > 0.0) || !std::isfinite(z) ||
      !(z > 0.0)) {
    return DeltaBand{};
  }
  const double half = z * std::sqrt(w_atm);
  return DeltaBand{0.5 * w_atm - half, 0.5 * w_atm + half};
}

namespace {

// The band a slice PAIR is certified on: the intersection of the two slices'
// delta bands. Empty when either slice has no usable ATM variance.
[[nodiscard]] DeltaBand pair_band(double w_atm_lo, double w_atm_hi,
                                  double z) noexcept {
  const DeltaBand a = delta_band_from_atm_w(w_atm_lo, z);
  const DeltaBand b = delta_band_from_atm_w(w_atm_hi, z);
  if (!a.usable() || !b.usable()) {
    return DeltaBand{};
  }
  return DeltaBand{std::max(a.k_lo, b.k_lo), std::min(a.k_hi, b.k_hi)};
}

// Worst deficit over a FINITE sub-range of a violating interval, with the k it
// occurs at. The whole interval violates, so any probe is a valid witness; the
// sweep only sharpens the reported number (documented as a lower bound).
[[nodiscard]] ArbViolation witness_over(const SviCrossingSlice &lo,
                                        const SviCrossingSlice &hi, double k_lo,
                                        double k_hi) noexcept {
  constexpr int kSamples = 16;
  double k_best = k_lo;
  double slack = lo.w(k_lo) - hi.w(k_lo);
  for (int i = 1; i <= kSamples; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(kSamples);
    const double k = std::clamp(k_lo + t * (k_hi - k_lo), k_lo, k_hi);
    const double d = lo.w(k) - hi.w(k);
    if (d > slack) {
      slack = d;
      k_best = k;
    }
  }
  return ArbViolation{k_best, lo.T(), hi.T(), slack,
                      ArbViolation::Kind::Calendar};
}

struct FiniteRange {
  double lo{};
  double hi{};
};

// A finite stand-in for a possibly-unbounded violating interval. D has constant
// sign across the whole interval, so any finite sub-range of it is a valid
// witness range — this only decides where the record points, never whether one
// is emitted. Both ends infinite (no crossing anywhere, the pair is inverted on
// all of R) collapses to a symmetric window rather than to -inf/-inf, which
// would evaluate w at infinity and produce NaN.
[[nodiscard]] FiniteRange finite_range(double k_lo, double k_hi,
                                       double span) noexcept {
  const bool lo_inf = !std::isfinite(k_lo);
  const bool hi_inf = !std::isfinite(k_hi);
  if (lo_inf && hi_inf) {
    return FiniteRange{-span, span};
  }
  if (lo_inf) {
    return FiniteRange{k_hi - span, k_hi};
  }
  if (hi_inf) {
    return FiniteRange{k_lo, k_lo + span};
  }
  return FiniteRange{k_lo, k_hi};
}

}  // namespace

Result<CalendarBandReport> arb_check_calendar_banded(const VolSurface &s,
                                                     double z,
                                                     std::uint32_t n_grid) {
  CalendarBandReport out{};
  const std::size_t n = s.n_slices();
  if (n < 2) {
    return Ok(std::move(out));
  }

  if (const std::optional<std::vector<SviCrossingSlice>> exact =
          exact_crossing_slices(s);
      exact.has_value()) {
    const std::vector<SviCrossingSlice> &slices = *exact;
    for (std::size_t i = 1; i < slices.size(); ++i) {
      const SviCrossingSlice &lo = slices[i - 1];
      const SviCrossingSlice &hi = slices[i];
      ++out.n_pairs_exact;
      const DeltaBand band = pair_band(lo.w(0.0), hi.w(0.0), z);
      for (const CalendarInterval &iv :
           svi_pair_calendar_intervals(lo, hi, kCalendarExactTol)) {
        const double span = std::max(1.0, band.k_hi - band.k_lo);
        if (band.usable()) {
          const double a = std::max(iv.k_lo, band.k_lo);
          const double b = std::min(iv.k_hi, band.k_hi);
          if (b >= a) {
            out.in_band.push_back(witness_over(lo, hi, a, b));
          }
        }
        // Left and right out-of-band pieces. With NO usable band the whole
        // interval is out of band: refusing to certify anything is the only
        // honest reading of "this slice has no tradeable window".
        if (!band.usable()) {
          const FiniteRange r = finite_range(iv.k_lo, iv.k_hi, span);
          out.out_of_band.push_back(witness_over(lo, hi, r.lo, r.hi));
          continue;
        }
        const double left_hi = std::min(iv.k_hi, band.k_lo);
        if (left_hi > iv.k_lo) {
          const FiniteRange r = finite_range(iv.k_lo, left_hi, span);
          out.out_of_band.push_back(witness_over(lo, hi, r.lo, r.hi));
        }
        const double right_lo = std::max(iv.k_lo, band.k_hi);
        if (right_lo < iv.k_hi) {
          const FiniteRange r = finite_range(right_lo, iv.k_hi, span);
          out.out_of_band.push_back(witness_over(lo, hi, r.lo, r.hi));
        }
      }
    }
    return Ok(std::move(out));
  }

  // SAMPLED fallback: the same outer window for both lanes, split by the same
  // band. `certified_over_r()` goes false so the caller cannot mistake this for
  // the unbounded statement.
  out.n_pairs_sampled = static_cast<std::uint32_t>(n - 1);
  ATX_TRY(const std::vector<ArbViolation> viols,
          arb_check_calendar(s, -kSampledOuterK, kSampledOuterK, n_grid));
  for (const ArbViolation &v : viols) {
    const DeltaBand band = pair_band(s.w(0.0, v.T1), s.w(0.0, v.T2), z);
    if (band.usable() && band.contains(v.k_log)) {
      out.in_band.push_back(v);
    } else {
      out.out_of_band.push_back(v);
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
  if (const auto *dense = dynamic_cast<const ConvexDenseCurve *>(&curve);
      dense != nullptr) {
    return Ok(butterfly_check_convex_dense(*dense, k_min, k_max));
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

  // Calendar: count-only, and DELIBERATELY still sampled. This is the C's
  // `ats_arb_check_total_surface_all` tally, whose contract is "violations per
  // grid point" — callers compare it across runs at a fixed n_grid. T4 moved
  // `arb_check_calendar` to the exact regime because its result feeds a
  // BOOLEAN; this one feeds a number nothing gates on, so changing its units
  // would only break the comparability it exists for.
  if (n >= 2 && n_grid > 0 && k_max > k_min) {
    const double dk = (k_max - k_min) / static_cast<double>(n_grid);
    for (std::uint32_t g = 0; g < n_grid; ++g) {
      const double k = k_min + static_cast<double>(g) * dk;
      double w_prev = -std::numeric_limits<double>::infinity();
      for (std::size_t i = 0; i < n; ++i) {
        const double T = slice_T_at(s, i);
        const double w = s.w(k, T);
        // Uncomparable, not a baseline — see arb_check_calendar. Without it a
        // NaN slice cost the count every crossing that spans it.
        if (!std::isfinite(w)) {
          continue;
        }
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
  // TRANSACTIONAL: iterate on a private copy; `current` is committed only on
  // the Ok paths, so a budget refusal (or any other failure) leaves the
  // caller's slice exactly as passed in.
  EssviParams trial = current;
  const SharedGridGap before = shared_grid_gap(
      w_prev, [&](double k) { return essvi_total_w(trial, k); },
      k_min, k_max, n_grid);
  if (!before.finite) {
    return Err(ErrorCode::Unavailable,
               "eSSVI calendar pair projection: non-finite candidate");
  }
  out.max_deficit_before = before.max_deficit;
  // Fidelity budget: the scale multiplies the slice's ATM total variance
  // exactly (theta and the residual coefficients scale together), so the
  // cumulative scale is capped at 1 + budget/w_atm — see
  // kCalendarRepairMaxAtmShiftFrac (arb.hpp) for why exceeding it is
  // fabrication, not repair.
  const double w_atm = essvi_total_w(trial, 0.0);
  const double budget_w = std::max(kCalendarRepairMaxAtmShiftFrac * w_atm,
                                   kCalendarRepairMinBudgetW);
  const double max_scale =
      (w_atm > 0.0) ? 1.0 + budget_w / w_atm : 1.0 + kCalendarRepairMaxAtmShiftFrac;
  for (std::uint32_t pass = 0; pass < 6; ++pass) {
    const SharedGridGap gap = shared_grid_gap(
        w_prev, [&](double k) { return essvi_total_w(trial, k); },
        k_min, k_max, n_grid);
    if (gap.finite && gap.max_deficit <= kCalendarPairTol) {
      current = trial;
      return Ok(out);
    }
    if (!gap.finite || !std::isfinite(gap.max_ratio) ||
        !(gap.max_ratio > 1.0)) {
      return Err(ErrorCode::Unavailable,
                 "eSSVI calendar pair projection: invalid scale");
    }
    const double scale = gap.max_ratio * (1.0 + 1.0e-9);
    if (out.scale * scale > max_scale) {
      return Err(ErrorCode::Unavailable,
                 "eSSVI calendar pair projection: required ATM level scale " +
                     std::to_string(out.scale * scale) +
                     " exceeds the fidelity budget " + std::to_string(max_scale) +
                     " (the crossing is not repairable by a level shift)");
    }
    trial.theta *= scale;
    for (double &coef : trial.resid_coef) {
      coef *= scale;
    }
    trial.phi = std::min(trial.phi, essvi_phi_max(trial.theta, trial.rho));
    out.scale *= scale;
    ++out.passes;
  }
  const SharedGridGap final_gap = shared_grid_gap(
      w_prev, [&](double k) { return essvi_total_w(trial, k); },
      k_min, k_max, n_grid);
  if (!final_gap.finite || final_gap.max_deficit > kCalendarPairTol) {
    return Err(ErrorCode::Unavailable,
               "eSSVI calendar pair projection did not converge");
  }
  current = trial;
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
    // Fidelity budget: the parallel `a` shift moves the slice's ATM total
    // variance by exactly the deficit. A deficit beyond the budget is not a
    // repair — refuse and leave the slice untouched (transactional; nothing
    // was mutated yet on this path). See kCalendarRepairMaxAtmShiftFrac.
    const double w_atm = svi_total_w(current, 0.0);
    const double budget_w = std::max(kCalendarRepairMaxAtmShiftFrac * w_atm,
                                     kCalendarRepairMinBudgetW);
    if (before.max_deficit > budget_w) {
      return Err(ErrorCode::Unavailable,
                 "SVI calendar pair projection: required ATM level shift " +
                     std::to_string(before.max_deficit) +
                     " exceeds the fidelity budget " + std::to_string(budget_w) +
                     " (the crossing is not repairable by a level shift)");
    }
    // Trial-shift so the non-convergence path below is transactional too.
    SviParams trial = current;
    trial.a += before.max_deficit + 1.0e-9;
    const SharedGridGap shifted = shared_grid_gap(
        w_prev, [&](double k) { return svi_total_w(trial, k); },
        k_min, k_max, n_grid);
    if (!shifted.finite || shifted.max_deficit > kCalendarPairTol) {
      return Err(ErrorCode::Unavailable,
                 "SVI calendar pair projection did not converge");
    }
    current = trial;
    out.passes = 1;
    return Ok(out);
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
  // TRANSACTIONAL: iterate on a private copy; committed only on success.
  C8Params trial = current;
  const SharedGridGap before = shared_grid_gap(
      w_prev, [&](double k) { return c8_slice_w(trial, k); },
      k_min, k_max, n_grid);
  if (!before.finite) {
    return Err(ErrorCode::Unavailable,
               "C8 calendar pair projection: non-finite candidate");
  }
  out.max_deficit_before = before.max_deficit;
  // Fidelity budget on the CUMULATIVE parallel level shift — see
  // kCalendarRepairMaxAtmShiftFrac (arb.hpp).
  const double w_atm = c8_slice_w(trial, 0.0);
  const double budget_w = std::max(kCalendarRepairMaxAtmShiftFrac * w_atm,
                                   kCalendarRepairMinBudgetW);
  double total_shift = 0.0;
  for (std::uint32_t pass = 0; pass < 6; ++pass) {
    const SharedGridGap gap = shared_grid_gap(
        w_prev, [&](double k) { return c8_slice_w(trial, k); },
        k_min, k_max, n_grid);
    if (gap.finite && gap.max_deficit <= kCalendarPairTol) {
      current = trial;
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
    if (total_shift + shift > budget_w) {
      return Err(ErrorCode::Unavailable,
                 "C8 calendar pair projection: required ATM level shift " +
                     std::to_string(total_shift + shift) +
                     " exceeds the fidelity budget " + std::to_string(budget_w) +
                     " (the crossing is not repairable by a level shift)");
    }
    trial.v += shift;
    trial.v_min += shift;
    c8_arb_project(trial);
    total_shift += shift;
    ++out.passes;
  }
  const SharedGridGap final_gap = shared_grid_gap(
      w_prev, [&](double k) { return c8_slice_w(trial, k); },
      k_min, k_max, n_grid);
  if (!final_gap.finite || final_gap.max_deficit > kCalendarPairTol) {
    return Err(ErrorCode::Unavailable,
               "C8 calendar pair projection did not converge");
  }
  current = trial;
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

  // Per-slice cumulative-shift fidelity budget, sized off the slice's
  // PRE-REPAIR ATM total variance — see kCalendarRepairMaxAtmShiftFrac.
  std::vector<double> budget_w(n);
  std::vector<double> shifted_w(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    budget_w[i] = std::max(kCalendarRepairMaxAtmShiftFrac * svi_total_w(slices[i], 0.0),
                           kCalendarRepairMinBudgetW);
  }

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
        if (shifted_w[i] + max_def + kSafety > budget_w[i]) {
          return Err(ErrorCode::Unavailable,
                     "arb_project_calendar_svi: slice " + std::to_string(i) +
                         " (T=" + std::to_string(curr.T) +
                         ") needs a cumulative ATM level shift of " +
                         std::to_string(shifted_w[i] + max_def) +
                         ", beyond the fidelity budget " +
                         std::to_string(budget_w[i]) +
                         " (not repairable by a level shift)");
        }
        curr.a += max_def + kSafety;
        shifted_w[i] += max_def + kSafety;
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

  // Per-slice cumulative-scale fidelity budget, sized off the slice's
  // PRE-REPAIR theta — see kCalendarRepairMaxAtmShiftFrac.
  std::vector<double> max_scale(n);
  std::vector<double> applied_scale(n, 1.0);
  for (std::size_t i = 0; i < n; ++i) {
    const double th = slices[i].theta;
    max_scale[i] =
        (th > 0.0) ? 1.0 + std::max(kCalendarRepairMaxAtmShiftFrac,
                                    kCalendarRepairMinBudgetW / th)
                   : 1.0 + kCalendarRepairMaxAtmShiftFrac;
  }

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
        const double step = max_ratio * kSafety;
        if (applied_scale[i] * step > max_scale[i]) {
          return Err(ErrorCode::Unavailable,
                     "arb_project_calendar_essvi: slice " + std::to_string(i) +
                         " (T=" + std::to_string(curr.T) +
                         ") needs a cumulative ATM level scale of " +
                         std::to_string(applied_scale[i] * step) +
                         ", beyond the fidelity budget " +
                         std::to_string(max_scale[i]) +
                         " (not repairable by a level scale)");
        }
        curr.theta *= step;
        applied_scale[i] *= step;
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
      // collapses `lo` to its backbone; if that still sits above `hi` the crossing
      // is not repairable by damping `lo`, yet the bisection below would converge
      // on a_lo = 0, commit it (erasing `lo`'s whole residual layer) and report Ok
      // on a surface that is still calendar-arbitrageable.
      //
      // Running `arb_project_calendar_essvi` first is NECESSARY BUT NOT SUFFICIENT
      // to rule this out, and the difference is not academic: that projection
      // enforces BACKBONE-vs-BACKBONE monotonicity (it evaluates
      // `essvi_backbone_w` on both slices — see its k-loop), while the test here
      // is `lo`'s backbone against `hi`'s TOTAL, `essvi_total_w(hi)`. Residual
      // coefficients are unconstrained in sign, so wherever `hi` carries a
      // NEGATIVE residual, `w_total(hi) < w_backbone(hi)` and this deficit can be
      // positive even though the projection ran and succeeded. That makes the
      // status below reachable on the shipped `run_surface_parity` ordering
      // (project, then repair) — see arb.hpp and
      // ArbRepairCalendarResidual.ProjectedBackbonesStillTripTheGuardOnANegativeUpperResidual.
      if (total_calendar_deficit_with_alpha(lo, hi, 0.0, k_min, k_max, n_grid) > 0.0) {
        // TRANSACTIONAL, like every repair in this file: the sweeps mutate the
        // local `slices` copy and only `write_back_essvi` commits, so returning
        // here leaves the caller's surface exactly as it was passed in.
        return Err(ErrorCode::Unavailable,
                   "arb_repair_calendar_residual: lower slice's backbone still crosses the "
                   "next slice's total variance at alpha=0 (residual damping cannot repair "
                   "this pair)");
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
