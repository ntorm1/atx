#include "atx/vol/api/pricing/adjusted_greeks.hpp"

#include <cmath>
#include <limits>

namespace atx::vol {

namespace {
constexpr double kFdStep = 1e-4;  // central FD half-step on k_log (brief-mandated)
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
}  // namespace

// L5 T4: validate (T, w) BEFORE the sqrt, not after it.
//
// Both slope entries below used to compute `sigma = sqrt(w/T)` ABOVE the guard
// that exists to protect it: `T == 0` divides by zero, and `w < 0` (or `T < 0`)
// takes the square root of a negative, raising FE_INVALID on a path whose whole
// job is a clean early return. The ANSWER was already right -- `!(sigma > 0.0)`
// catches the NaN either way -- so this is a discipline fix, not a numeric one,
// and the reordered screen is exactly equivalent: `sigma` is positive and
// finite iff `T > 0` and `w > 0` and `w/T` neither underflows to zero nor
// overflows, and the two surviving checks after the sqrt still cover that last
// pair (`w = +inf` reaches the sqrt, exactly, and fails `isfinite`).
double curve_skew_slope(const IVolCurve& c, double k_log) noexcept {
  const double T = c.T();
  const double wk = c.w(k_log);
  if (!(T > 0.0) || !(wk > 0.0)) {
    return kNaN;
  }
  const double sigma = std::sqrt(wk / T);
  if (!(sigma > 0.0) || !std::isfinite(sigma)) {
    return kNaN;
  }

  const double w_plus = c.w(k_log + kFdStep);
  const double w_minus = c.w(k_log - kFdStep);
  const double dw_dk = (w_plus - w_minus) / (2.0 * kFdStep);
  if (!std::isfinite(dw_dk)) {
    return kNaN;
  }

  return dw_dk / (2.0 * sigma * T);
}

// Same reordering, same reasoning as `curve_skew_slope` above.
double surface_skew_slope(const VolSurface& s, double k_log, double T) noexcept {
  const double wk = s.w(k_log, T);
  if (!(T > 0.0) || !(wk > 0.0)) {
    return kNaN;
  }
  const double sigma = std::sqrt(wk / T);
  if (!(sigma > 0.0) || !std::isfinite(sigma)) {
    return kNaN;
  }

  const double w_plus = s.w(k_log + kFdStep, T);
  const double w_minus = s.w(k_log - kFdStep, T);
  const double dw_dk = (w_plus - w_minus) / (2.0 * kFdStep);
  if (!std::isfinite(dw_dk)) {
    return kNaN;
  }

  return dw_dk / (2.0 * sigma * T);
}

double vega_slope_from_skew_slope(double skew_slope, double S, const StickyParams& sp) noexcept {
  // Reject non-finite S explicitly: !(S > 0.0) alone catches NaN / <= 0 but
  // would wave S = +inf through (the division then yields 0, not the NaN the
  // header promises for a non-finite spot).
  if (!std::isfinite(S) || !(S > 0.0)) {
    return kNaN;
  }
  if (!std::isfinite(skew_slope)) {
    return kNaN;
  }
  return (1.0 - sp.ref_uprc_weight) * (-skew_slope / S);
}

double vega_slope_per_spot(const IVolCurve& c, double k_log, double S,
                           const StickyParams& sp) noexcept {
  return vega_slope_from_skew_slope(curve_skew_slope(c, k_log), S, sp);
}

Greeks skew_adjusted(const Greeks& g, double vega_slope) noexcept {
  Greeks out = g;
  out.delta = g.delta + vega_slope * g.vega;
  return out;
}

}  // namespace atx::vol
