#include "atx/vol/api/risk/strike_coord.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/core/math.hpp" // norm_cdf
#include "atx/vol/api/pricing/pricing_executor.hpp"
#include "risk/american_delta_solve.hpp"
#include "risk/cold_route.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Smile-honouring Forward / Spot delta solve: fixed-point iteration budget and
// stop rule (|dK| / K), then a bisection fallback over k = ln(K / F) in [-3, 3].
constexpr int kMaxFixedPointIterations = 50;
constexpr double kFixedPointRelTol = 1.0e-10;
constexpr double kBisectionHalfWidth = 3.0;
constexpr int kMaxBisectionIterations = 200;
constexpr double kBisectionWidthTol = 1.0e-13;

[[nodiscard]] bool finite_positive(double value) noexcept {
  return std::isfinite(value) && value > 0.0;
}

// Lower-tail standard-normal quantile for p in (0, 0.5]: Acklam's rational seed
// (relative error < 1.15e-9) polished by two Halley steps on the erfc-based CDF,
// which the closed-form round trips need at the 1e-12 level. Exported library
// probits (implied_vol.cpp, contract_projection.cpp, research_validation.cpp) are
// all file-local seeds without the polish, hence this one.
[[nodiscard]] double norm_ppf_lower(double p) noexcept {
  constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,  -2.759285104469687e+02,
                          1.383577518672690e+02,  -3.066479806614716e+01, 2.506628277459239e+00};
  constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                          6.680131188771972e+01, -1.328068155288572e+01};
  constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                          -2.549732539343734e+00, 4.374664141464968e+00,  2.938163982698783e+00};
  constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                          3.754408661907416e+00};
  constexpr double p_low = 0.02425;
  double x = 0.0;
  if (p < p_low) {
    const double q = std::sqrt(-2.0 * std::log(p));
    x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  } else {
    const double q = p - 0.5;
    const double r = q * q;
    x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
        (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
  }
  constexpr double kSqrt2Pi = 2.50662827463100050242;
  for (int step = 0; step < 2; ++step) {
    const double e = atx::core::norm_cdf(x) - p;
    const double u = e * kSqrt2Pi * std::exp(0.5 * x * x);
    x -= u / (1.0 + 0.5 * x * u);
  }
  return x;
}

// Full-range quantile. For p > 1/2, 1 - p is exact (Sterbenz), so the accurate
// lower-tail branch serves both tails.
[[nodiscard]] double norm_ppf(double p) noexcept {
  if (!(p > 0.0 && p < 1.0)) {
    return kNaN;
  }
  return p > 0.5 ? -norm_ppf_lower(1.0 - p) : norm_ppf_lower(p);
}

[[nodiscard]] bool valid_side(Side side) noexcept {
  return side == Side::Call || side == Side::Put;
}

// Signed forward-delta range check: (0,1) call, (-1,0) put.
[[nodiscard]] bool forward_delta_in_range(double signed_delta, Side side) noexcept {
  return side == Side::Call ? (signed_delta > 0.0 && signed_delta < 1.0)
                            : (signed_delta < 0.0 && signed_delta > -1.0);
}

// Closed-form strike for a FORWARD delta at fixed sigma. The put uses
// d1 = -Phi^-1(-delta) (delta_put = -Phi(-d1)), which keeps small put deltas
// accurate instead of forming 1 + delta.
[[nodiscard]] double strike_from_fwd(double F, double T, double sigma, double signed_delta,
                                     Side side) noexcept {
  if (!finite_positive(F) || !finite_positive(T) || !finite_positive(sigma) ||
      !valid_side(side) || !forward_delta_in_range(signed_delta, side)) {
    return kNaN;
  }
  const double d1 = side == Side::Call ? norm_ppf(signed_delta) : -norm_ppf(-signed_delta);
  const double sqrt_t = std::sqrt(T);
  const double K = F * std::exp(0.5 * sigma * sigma * T - sigma * sqrt_t * d1);
  return finite_positive(K) ? K : kNaN;
}

// The closed-form delta under a Forward / Spot convention at (K, sigma).
[[nodiscard]] double closed_form_delta(DeltaConvention conv, double F, double K, double T,
                                       double sigma, double q, Side side) noexcept {
  return conv == DeltaConvention::Spot ? spot_delta(F, K, T, sigma, q, side)
                                       : forward_delta(F, K, T, sigma, side);
}

[[nodiscard]] double closed_form_strike(DeltaConvention conv, double F, double T, double sigma,
                                        double q, double signed_delta, Side side) noexcept {
  return conv == DeltaConvention::Spot ? strike_from_spot_delta(F, T, sigma, q, signed_delta, side)
                                       : strike_from_forward_delta(F, T, sigma, signed_delta, side);
}

// The surface-level inputs every conversion at (T) shares.
struct SurfaceAt {
  double T{0.0};
  double sqrt_t{0.0};
  double F{0.0};
  double S{0.0};
  double q{0.0};
  double sigma_atm{0.0};
};

// Forward / Spot delta -> K on the smile: K_{n+1} = inverse(delta; sigma(K_n)),
// seeded at sigma_atm, bounded; a bisection on k in [-3, 3] when the fixed point
// does not settle. NotFound when the target is unreachable on the surface.
[[nodiscard]] Result<double> solve_closed_form_on_smile(const SurfaceRef &surface,
                                                        const SurfaceAt &at, Side side,
                                                        double target, DeltaConvention conv,
                                                        TenorInterp interp) {
  double K = closed_form_strike(conv, at.F, at.T, at.sigma_atm, at.q, target, side);
  if (finite_positive(K)) {
    for (int it = 0; it < kMaxFixedPointIterations; ++it) {
      const double sigma = surface.iv(K, at.T, interp);
      if (!finite_positive(sigma)) {
        break;
      }
      const double next = closed_form_strike(conv, at.F, at.T, sigma, at.q, target, side);
      if (!finite_positive(next)) {
        break;
      }
      if (std::fabs(next - K) <= kFixedPointRelTol * K) {
        return Ok(next);
      }
      K = next;
    }
  }
  // Bisection on g(k) = delta(F e^k, sigma(F e^k)) - target. Both delta
  // conventions are monotone decreasing in K for a fixed side on an arbitrage-free
  // smile; a missing bracket (or a vol hole at an end) is NotFound.
  const auto g = [&](double k) noexcept {
    const double strike = at.F * std::exp(k);
    const double sigma = surface.iv(strike, at.T, interp);
    return closed_form_delta(conv, at.F, strike, at.T, sigma, at.q, side) - target;
  };
  double lo = -kBisectionHalfWidth;
  double hi = kBisectionHalfWidth;
  double g_lo = g(lo);
  const double g_hi = g(hi);
  if (!std::isfinite(g_lo) || !std::isfinite(g_hi) || (g_lo > 0.0) == (g_hi > 0.0)) {
    return Err(ErrorCode::NotFound,
               "convert_strike_coord: delta target unreachable on the surface");
  }
  for (int it = 0; it < kMaxBisectionIterations && (hi - lo) > kBisectionWidthTol; ++it) {
    const double mid = 0.5 * (lo + hi);
    const double g_mid = g(mid);
    if (!std::isfinite(g_mid)) {
      return Err(ErrorCode::NotFound, "convert_strike_coord: vol hole inside the delta bracket");
    }
    if ((g_mid > 0.0) == (g_lo > 0.0)) {
      lo = mid;
      g_lo = g_mid;
    } else {
      hi = mid;
    }
  }
  return Ok(at.F * std::exp(0.5 * (lo + hi)));
}

[[nodiscard]] bool known_convention(DeltaConvention conv) noexcept {
  switch (conv) {
  case DeltaConvention::Forward:
  case DeltaConvention::American:
  case DeltaConvention::Spot:
    return true;
  }
  return false;
}

// Validate the input coordinate's own domain (finite value already checked).
[[nodiscard]] Status validate_coord(const StrikeCoord &from, Side side) {
  switch (from.kind) {
  case StrikeCoordKind::Strike:
  case StrikeCoordKind::StandardMoneyness:
    if (!(from.value > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "convert_strike_coord: strike / K-over-S must be > 0");
    }
    return Ok();
  case StrikeCoordKind::LogMoneyness:
  case StrikeCoordKind::StandardizedMoneyness:
    return Ok();
  case StrikeCoordKind::Delta:
    if (!known_convention(from.delta_conv)) {
      return Err(ErrorCode::InvalidArgument, "convert_strike_coord: unknown delta convention");
    }
    if (!forward_delta_in_range(from.value, side)) {
      return Err(ErrorCode::InvalidArgument,
                 "convert_strike_coord: delta must lie in (0,1) for a call, (-1,0) for a put");
    }
    return Ok();
  }
  return Err(ErrorCode::InvalidArgument, "convert_strike_coord: unknown strike-coordinate kind");
}

// The strike for `from` (and the American solve's by-products when it ran).
struct StrikeSolve {
  double K{0.0};
  double american_delta{kNaN}; // achieved American delta when the American solve ran
  std::uint16_t evaluations{0};
};

[[nodiscard]] Result<StrikeSolve> solve_strike(const SurfaceRef &surface, const SurfaceAt &at,
                                               Side side, const StrikeCoord &from,
                                               const ProjectionPolicy &policy) {
  switch (from.kind) {
  case StrikeCoordKind::Strike:
    return Ok(StrikeSolve{from.value, kNaN, 0u});
  case StrikeCoordKind::LogMoneyness:
    return Ok(StrikeSolve{at.F * std::exp(from.value), kNaN, 0u});
  case StrikeCoordKind::StandardMoneyness:
    return Ok(StrikeSolve{from.value * at.S, kNaN, 0u});
  case StrikeCoordKind::StandardizedMoneyness:
    return Ok(StrikeSolve{at.F * std::exp(from.value * at.sigma_atm * at.sqrt_t), kNaN, 0u});
  case StrikeCoordKind::Delta:
    break;
  }
  switch (from.delta_conv) {
  case DeltaConvention::Forward:
  case DeltaConvention::Spot: {
    ATX_TRY(const double K, solve_closed_form_on_smile(surface, at, side, from.value,
                                                       from.delta_conv, policy.interp));
    return Ok(StrikeSolve{K, kNaN, 0u});
  }
  case DeltaConvention::American: {
    ATX_TRY(const risk_detail::AmericanDeltaSolution solved,
            risk_detail::solve_american_delta(surface, at.T, side, from.value,
                                              policy.delta_tolerance, policy.interp));
    return Ok(StrikeSolve{solved.strike, solved.delta, solved.evaluations});
  }
  }
  return Err(ErrorCode::InvalidArgument, "convert_strike_coord: unknown delta convention");
}

[[nodiscard]] std::uint16_t saturating_add(std::uint16_t a, std::uint16_t b) noexcept {
  const unsigned sum = static_cast<unsigned>(a) + static_cast<unsigned>(b);
  return static_cast<std::uint16_t>(
      std::min(sum, static_cast<unsigned>(std::numeric_limits<std::uint16_t>::max())));
}

// The reported delta under the policy's convention at the resolved point.
[[nodiscard]] Result<double> report_delta(const SurfaceRef &surface, const SurfaceAt &at,
                                          Side side, double K, double sigma,
                                          const ProjectionPolicy &policy,
                                          const StrikeSolve &solve, CoordResult &out) {
  switch (policy.delta_conv) {
  case DeltaConvention::Forward:
    return Ok(forward_delta(at.F, K, at.T, sigma, side));
  case DeltaConvention::Spot:
    return Ok(spot_delta(at.F, K, at.T, sigma, at.q, side));
  case DeltaConvention::American: {
    if (std::isfinite(solve.american_delta)) {
      return Ok(solve.american_delta); // the solve's own cold-confirmed evaluation at K
    }
    out.delta_evaluations = saturating_add(out.delta_evaluations, 1u);
    return risk_detail::cold_delta(surface, K, at.T, side, policy.interp);
  }
  }
  return Err(ErrorCode::InvalidArgument, "convert_strike_coord: unknown policy delta convention");
}

[[nodiscard]] CoordResult failed_row(const SurfaceRef &surface, double T,
                                     const ProjectionPolicy &policy) noexcept {
  CoordResult r;
  r.T = T;
  r.forward = kNaN;
  r.spot = kNaN;
  r.K = kNaN;
  r.k_log = kNaN;
  r.standard_moneyness = kNaN;
  r.standardized_moneyness = kNaN;
  r.sigma_atm = kNaN;
  r.delta = kNaN;
  r.sigma = kNaN;
  r.delta_conv = policy.delta_conv;
  if (surface.valid() && std::isfinite(T) && T > 0.0 && surface.extrapolates_tenor(T)) {
    r.flags |= RiskFlag::TenorExtrapolated;
  }
  return r;
}

} // namespace

// ── StrikeCoord factories ────────────────────────────────────────────────────

StrikeCoord StrikeCoord::strike(double K) noexcept {
  return StrikeCoord{StrikeCoordKind::Strike, K, DeltaConvention::Forward};
}
StrikeCoord StrikeCoord::log_moneyness(double k) noexcept {
  return StrikeCoord{StrikeCoordKind::LogMoneyness, k, DeltaConvention::Forward};
}
StrikeCoord StrikeCoord::standard_moneyness(double m) noexcept {
  return StrikeCoord{StrikeCoordKind::StandardMoneyness, m, DeltaConvention::Forward};
}
StrikeCoord StrikeCoord::standardized_moneyness(double z) noexcept {
  return StrikeCoord{StrikeCoordKind::StandardizedMoneyness, z, DeltaConvention::Forward};
}
StrikeCoord StrikeCoord::delta(double signed_delta, DeltaConvention conv) noexcept {
  return StrikeCoord{StrikeCoordKind::Delta, signed_delta, conv};
}

// ── Closed forms ─────────────────────────────────────────────────────────────

double forward_delta(double F, double K, double T, double sigma, Side side) noexcept {
  if (!finite_positive(F) || !finite_positive(K) || !finite_positive(T) ||
      !finite_positive(sigma) || !valid_side(side)) {
    return kNaN;
  }
  const double sqrt_t = std::sqrt(T);
  const double d1 = (std::log(F / K) + 0.5 * sigma * sigma * T) / (sigma * sqrt_t);
  // Put: N(d1) - 1 evaluated as -N(-d1), its cancellation-free form.
  return side == Side::Call ? atx::core::norm_cdf(d1) : -atx::core::norm_cdf(-d1);
}

double spot_delta(double F, double K, double T, double sigma, double q, Side side) noexcept {
  if (!std::isfinite(q)) {
    return kNaN;
  }
  return std::exp(-q * T) * forward_delta(F, K, T, sigma, side);
}

double strike_from_forward_delta(double F, double T, double sigma, double signed_delta,
                                 Side side) noexcept {
  return strike_from_fwd(F, T, sigma, signed_delta, side);
}

double strike_from_spot_delta(double F, double T, double sigma, double q, double signed_delta,
                              Side side) noexcept {
  if (!std::isfinite(q) || !finite_positive(T)) {
    return kNaN;
  }
  // delta_spot = e^{-qT} delta_fwd: the forward target leaves (0,1) / (-1,0) — and
  // strike_from_fwd returns NaN — when the carry makes the spot target unreachable.
  return strike_from_fwd(F, T, sigma, signed_delta / std::exp(-q * T), side);
}

// ── Conversions on a surface ─────────────────────────────────────────────────

Result<CoordResult> convert_strike_coord(const SurfaceRef &surface, double T, Side side,
                                         const StrikeCoord &from,
                                         const ProjectionPolicy &policy) {
  if (!surface.valid()) {
    return Err(ErrorCode::InvalidArgument, "convert_strike_coord: null surface");
  }
  if (!finite_positive(T)) {
    return Err(ErrorCode::InvalidArgument, "convert_strike_coord: T must be finite and > 0");
  }
  if (!valid_side(side) || !std::isfinite(from.value)) {
    return Err(ErrorCode::InvalidArgument, "convert_strike_coord: bad side or non-finite value");
  }
  const bool modes_known =
      known_convention(policy.delta_conv) &&
      (policy.interp == TenorInterp::TotalVariance || policy.interp == TenorInterp::ShapeBlend) &&
      (policy.extrap == TenorExtrapPolicy::Flag || policy.extrap == TenorExtrapPolicy::Forbid);
  if (!modes_known) {
    return Err(ErrorCode::InvalidArgument, "convert_strike_coord: unknown policy mode");
  }
  ATX_TRY_VOID(validate_coord(from, side));

  CoordResult out;
  out.T = T;
  out.delta_conv = policy.delta_conv;
  if (surface.extrapolates_tenor(T)) {
    switch (policy.extrap) {
    case TenorExtrapPolicy::Flag:
      out.flags |= RiskFlag::TenorExtrapolated;
      break;
    case TenorExtrapPolicy::Forbid:
      return Err(ErrorCode::OutOfRange,
                 "convert_strike_coord: T outside the surface's fitted tenor range (Forbid)");
    }
  }

  SurfaceAt at;
  at.T = T;
  at.sqrt_t = std::sqrt(T);
  at.F = surface.forward_at(T);
  at.S = surface.pricing().S;
  at.q = surface.q_eff_at(T);
  at.sigma_atm = finite_positive(at.F) ? surface.iv(at.F, T, policy.interp) : kNaN;
  if (!finite_positive(at.F) || !finite_positive(at.S) || !finite_positive(at.sigma_atm)) {
    return Err(ErrorCode::Unavailable,
               "convert_strike_coord: surface has no forward / ATM vol at T");
  }

  ATX_TRY(const StrikeSolve solve, solve_strike(surface, at, side, from, policy));
  out.delta_evaluations = solve.evaluations;
  const double K = solve.K;
  if (!finite_positive(K)) {
    return Err(ErrorCode::NotFound, "convert_strike_coord: resolved strike is not finite");
  }
  const double sigma = surface.iv(K, T, policy.interp);
  if (!finite_positive(sigma)) {
    return Err(ErrorCode::Unavailable, "convert_strike_coord: surface has no vol at the strike");
  }

  // The input coordinate is echoed verbatim; the others are derived from K.
  const double k_log = from.kind == StrikeCoordKind::LogMoneyness ? from.value
                       : from.kind == StrikeCoordKind::StandardizedMoneyness
                           ? from.value * at.sigma_atm * at.sqrt_t
                           : std::log(K / at.F);
  out.forward = at.F;
  out.spot = at.S;
  out.K = K;
  out.k_log = k_log;
  out.standard_moneyness = from.kind == StrikeCoordKind::StandardMoneyness ? from.value : K / at.S;
  out.standardized_moneyness = from.kind == StrikeCoordKind::StandardizedMoneyness
                                   ? from.value
                                   : k_log / (at.sigma_atm * at.sqrt_t);
  out.sigma_atm = at.sigma_atm;
  out.sigma = sigma;
  ATX_TRY(const double delta, report_delta(surface, at, side, K, sigma, policy, solve, out));
  if (!std::isfinite(delta)) {
    return Err(ErrorCode::Unavailable, "convert_strike_coord: delta not finite at the strike");
  }
  out.delta = delta;
  if (surface.moneyness_extrapolated(K, T)) {
    out.flags |= RiskFlag::MoneynessExtrapolated;
  }
  return Ok(out);
}

Status convert_strike_coord_batch(const SurfaceRef &surface, std::span<const double> T,
                                  std::span<const Side> side, std::span<const StrikeCoord> from,
                                  const ProjectionPolicy &policy, std::span<CoordResult> out,
                                  std::span<Status> row_status) {
  if (!surface.valid()) {
    return Err(ErrorCode::InvalidArgument, "convert_strike_coord_batch: null surface");
  }
  const std::size_t n = T.size();
  if (side.size() != n || from.size() != n || out.size() != n || row_status.size() != n) {
    return Err(ErrorCode::InvalidArgument, "convert_strike_coord_batch: span length mismatch");
  }
  // Rows are independent and each writes only its own slots, so the result is
  // identical for any worker count (pricing_executor's disjoint-block contract).
  pricing_executor().run_blocks(n, 0u, [&](std::size_t i) {
    Result<CoordResult> row = convert_strike_coord(surface, T[i], side[i], from[i], policy);
    if (row) {
      out[i] = *row;
      row_status[i] = Ok();
    } else {
      out[i] = failed_row(surface, T[i], policy);
      row_status[i] = Err(std::move(row).error());
    }
  });
  return Ok();
}

// ── Names ────────────────────────────────────────────────────────────────────

const char *to_string(StrikeCoordKind value) noexcept {
  switch (value) {
  case StrikeCoordKind::Strike:
    return "strike";
  case StrikeCoordKind::LogMoneyness:
    return "log-moneyness";
  case StrikeCoordKind::StandardMoneyness:
    return "standard-moneyness";
  case StrikeCoordKind::Delta:
    return "delta";
  case StrikeCoordKind::StandardizedMoneyness:
    return "standardized-moneyness";
  }
  return "unknown";
}

const char *to_string(TenorInterp value) noexcept {
  switch (value) {
  case TenorInterp::TotalVariance:
    return "total-variance";
  case TenorInterp::ShapeBlend:
    return "shape-blend";
  }
  return "unknown";
}

const char *to_string(TenorExtrapPolicy value) noexcept {
  switch (value) {
  case TenorExtrapPolicy::Flag:
    return "flag";
  case TenorExtrapPolicy::Forbid:
    return "forbid";
  }
  return "unknown";
}

} // namespace atx::vol
