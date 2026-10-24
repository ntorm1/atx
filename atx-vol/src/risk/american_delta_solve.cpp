#include "risk/american_delta_solve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "atx/core/error.hpp"
#include "risk/cold_route.hpp"

// The solver bodies below are copied from src/analytics/contract_projection.cpp
// (`inverse_normal_cdf`, `solve_american_delta` -> `solve_american_delta_routed`,
// `add_evaluations`, `solve_american_delta_screened`) with identical arithmetic and
// control flow. The edits: names (the routed solve, the loop's trial abscissa
// `next`, which no longer shadows the outer `candidate`), the error-message prefix,
// the signed entry point at the bottom, and the surface type is a template
// parameter so a non-default TenorInterp can feed the same solve (`InterpSurface`);
// TotalVariance instantiates it with SurfaceRef itself, i.e. the original code.

namespace atx::vol::risk_detail {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

[[nodiscard]] bool finite_positive(double value) noexcept {
  return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] double inverse_normal_cdf(double probability) noexcept {
  // Acklam's rational approximation; max absolute error is below 1.2e-9 over
  // the open unit interval, more than sufficient for a root-solver seed.
  constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,  -2.759285104469687e+02,
                          1.383577518672690e+02,  -3.066479806614716e+01, 2.506628277459239e+00};
  constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                          6.680131188771972e+01, -1.328068155288572e+01};
  constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                          -2.549732539343734e+00, 4.374664141464968e+00,  2.938163982698783e+00};
  constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                          3.754408661907416e+00};
  constexpr double low = 0.02425;
  constexpr double high = 1.0 - low;
  if (!(probability > 0.0 && probability < 1.0)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (probability < low) {
    const double q = std::sqrt(-2.0 * std::log(probability));
    return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
           ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
  }
  if (probability <= high) {
    const double q = probability - 0.5;
    const double r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
  }
  const double q = std::sqrt(-2.0 * std::log(1.0 - probability));
  return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
         ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
}

struct DeltaSolution {
  double strike{0.0};
  double achieved_delta{0.0};
  std::uint16_t evaluations{0};
};

template <class Surface>
[[nodiscard]] Result<DeltaSolution> solve_american_delta_routed(const Surface &surface,
                                                                double T, Side side,
                                                                double target_abs_delta,
                                                                double tolerance,
                                                                QueryExecution query_execution) {
  if (!(target_abs_delta > 0.0 && target_abs_delta < 1.0) ||
      !(std::isfinite(tolerance) && tolerance > 0.0 && tolerance <= 1.0e-3)) {
    return Err(ErrorCode::InvalidArgument, "american delta solve: invalid delta target/tolerance");
  }
  const double forward = surface.forward_at(T);
  double sigma = surface.iv(forward, T);
  if (!finite_positive(forward) || !finite_positive(sigma)) {
    return Err(ErrorCode::Unavailable, "american delta solve: no surface at maturity");
  }

  const double carry_discount = std::exp(-surface.q_eff_at(T) * T);
  const double probability = std::clamp(target_abs_delta / carry_discount, 1.0e-8, 1.0 - 1.0e-8);
  const double signed_d1 =
      side == Side::Call ? inverse_normal_cdf(probability) : -inverse_normal_cdf(probability);
  const double sqrt_t = std::sqrt(T);
  double seed_k = 0.0;
  for (int iteration = 0; iteration < 2; ++iteration) {
    seed_k = -signed_d1 * sigma * sqrt_t + 0.5 * sigma * sigma * T;
    const double smile_sigma = surface.iv(forward * std::exp(seed_k), T);
    if (finite_positive(smile_sigma)) {
      sigma = smile_sigma;
    }
  }

  struct Residual {
    double value{0.0};
    double delta{0.0};
    bool exact{false};
  };
  std::uint16_t evaluations = 0;
  const auto evaluate = [&](double k) -> Residual {
    ++evaluations;
    const Result<double> delta = surface.delta(forward * std::exp(k), T, side, query_execution);
    if (delta && std::isfinite(*delta)) {
      return Residual{std::fabs(*delta) - target_abs_delta, *delta, true};
    }
    const bool itm = side == Side::Call ? k < 0.0 : k > 0.0;
    return Residual{(itm ? 1.0 : 0.0) - target_abs_delta, 0.0, false};
  };

  const Residual seed = evaluate(seed_k);
  if (!seed.exact) {
    return Err(ErrorCode::Unavailable, "american delta solve: delta seed unavailable");
  }
  if (std::fabs(seed.value) <= tolerance) {
    return Ok(DeltaSolution{forward * std::exp(seed_k), seed.delta, evaluations});
  }

  // One safeguarded Newton step using the Black delta slope. The residual is
  // still the exact American delta; the closed-form slope is only a cheap
  // direction/scale estimate used to obtain a tight initial bracket.
  constexpr double inv_sqrt_two_pi = 0.39894228040143267794;
  const double density = inv_sqrt_two_pi * std::exp(-0.5 * signed_d1 * signed_d1);
  const double slope_magnitude = carry_discount * density / (sigma * sqrt_t);
  const double slope = side == Side::Call ? -slope_magnitude : slope_magnitude;
  const double newton_step = std::clamp(-seed.value / slope, -0.50, 0.50);
  double candidate_k = seed_k + newton_step;
  Residual candidate = evaluate(candidate_k);
  if (candidate.exact && std::fabs(candidate.value) <= tolerance) {
    return Ok(DeltaSolution{forward * std::exp(candidate_k), candidate.delta, evaluations});
  }

  double lo = std::min(seed_k, candidate_k);
  double hi = std::max(seed_k, candidate_k);
  Residual rlo = seed_k <= candidate_k ? seed : candidate;
  Residual rhi = seed_k <= candidate_k ? candidate : seed;
  bool bracketed = (rlo.value <= 0.0) != (rhi.value <= 0.0);
  if (!bracketed) {
    const double direction = newton_step >= 0.0 ? 1.0 : -1.0;
    const double initial_width = std::max(0.05, 2.0 * std::fabs(newton_step));
    for (const double scale : {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0}) {
      candidate_k = seed_k + direction * std::min(5.0, initial_width * scale);
      candidate = evaluate(candidate_k);
      if (candidate_k < seed_k) {
        lo = candidate_k;
        rlo = candidate;
        hi = seed_k;
        rhi = seed;
      } else {
        lo = seed_k;
        rlo = seed;
        hi = candidate_k;
        rhi = candidate;
      }
      if ((rlo.value <= 0.0) != (rhi.value <= 0.0)) {
        bracketed = true;
        break;
      }
      if (std::fabs(candidate_k - seed_k) >= 5.0) {
        break;
      }
    }
  }
  if (!bracketed) {
    return Err(ErrorCode::NotFound, "american delta solve: delta target not bracketed");
  }

  double root = seed_k;
  Residual root_residual = seed;
  int retained = 0;
  for (int iteration = 0; iteration < 64; ++iteration) {
    double next = 0.5 * (lo + hi);
    if (rlo.exact && rhi.exact && rlo.value != rhi.value) {
      const double secant = (rlo.value * hi - rhi.value * lo) / (rlo.value - rhi.value);
      const double margin = 1.0e-6 * (hi - lo);
      if (secant > lo + margin && secant < hi - margin) {
        next = secant;
      }
    }
    root = next;
    root_residual = evaluate(next);
    if (root_residual.exact && std::fabs(root_residual.value) <= tolerance) {
      break;
    }
    if ((root_residual.value <= 0.0) == (rlo.value <= 0.0)) {
      lo = next;
      rlo = root_residual;
      if (retained == -1 && rhi.exact) {
        rhi.value *= 0.5;
      }
      retained = -1;
    } else {
      hi = next;
      rhi = root_residual;
      if (retained == 1 && rlo.exact) {
        rlo.value *= 0.5;
      }
      retained = 1;
    }
  }
  if (!root_residual.exact || std::fabs(root_residual.value) > tolerance) {
    return Err(ErrorCode::Unavailable, "american delta solve: did not converge");
  }
  return Ok(DeltaSolution{forward * std::exp(root), root_residual.delta, evaluations});
}

[[nodiscard]] std::uint16_t add_evaluations(std::uint16_t left, std::uint16_t right) noexcept {
  const unsigned sum = static_cast<unsigned>(left) + static_cast<unsigned>(right);
  return static_cast<std::uint16_t>(
      std::min(sum, static_cast<unsigned>(std::numeric_limits<std::uint16_t>::max())));
}

template <class Surface>
[[nodiscard]] Result<DeltaSolution> solve_american_delta_screened(const Surface &surface,
                                                                  double T, Side side,
                                                                  double target_abs_delta,
                                                                  double tolerance) {
  const auto cold_fallback = [&](std::uint16_t prior_evaluations) -> Result<DeltaSolution> {
    Result<DeltaSolution> cold = solve_american_delta_routed(
        surface, T, side, target_abs_delta, tolerance, QueryExecution::ColdReference);
    if (cold) {
      cold->evaluations = add_evaluations(cold->evaluations, prior_evaluations);
    }
    return cold;
  };
  const QueryPricingTier tier = surface.query_pricing_tier();
  if (tier != QueryPricingTier::RepresentativeFast && tier != QueryPricingTier::CarryBank) {
    return cold_fallback(0u);
  }

  // The prepared route is only a cheap proposal. Its own root need not meet the
  // cold target; a loose screen tolerance avoids spending work polishing an
  // approximation that is always cold-confirmed below.
  constexpr double screen_tolerance = 1.0e-4;
  Result<DeltaSolution> screen = solve_american_delta_routed(
      surface, T, side, target_abs_delta, screen_tolerance, QueryExecution::Configured);
  if (!screen) {
    return cold_fallback(0u);
  }

  const double forward = surface.forward_at(T);
  if (!finite_positive(forward) || !finite_positive(screen->strike)) {
    return cold_fallback(screen->evaluations);
  }
  struct DeltaPoint {
    double k{0.0};
    double residual{0.0};
    double delta{0.0};
  };
  std::uint16_t evaluations = screen->evaluations;
  const auto point = [&](double k, QueryExecution execution) -> Result<DeltaPoint> {
    evaluations = add_evaluations(evaluations, 1u);
    const double strike = forward * std::exp(k);
    if (!finite_positive(strike)) {
      return Err(ErrorCode::OutOfRange, "american delta solve: adaptive strike overflow");
    }
    ATX_TRY(const double delta, surface.delta(strike, T, side, execution));
    if (!std::isfinite(delta)) {
      return Err(ErrorCode::Unavailable, "american delta solve: adaptive delta unavailable");
    }
    return Ok(DeltaPoint{k, std::fabs(delta) - target_abs_delta, delta});
  };

  const double screen_k = std::log(screen->strike / forward);
  Result<DeltaPoint> current = point(screen_k, QueryExecution::ColdReference);
  if (!current) {
    return cold_fallback(evaluations);
  }
  if (std::fabs(current->residual) <= tolerance) {
    return Ok(DeltaSolution{screen->strike, current->delta, evaluations});
  }

  DeltaPoint previous{};
  bool have_previous = false;
  constexpr double max_log_strike_step = 0.05;
  constexpr unsigned max_refine_iterations = 8u;
  for (unsigned iteration = 0u; iteration < max_refine_iterations; ++iteration) {
    double slope = 0.0;
    if (have_previous && current->k != previous.k) {
      slope = (current->residual - previous.residual) / (current->k - previous.k);
    } else {
      constexpr double h = 1.0e-3;
      const Result<DeltaPoint> left = point(current->k - h, QueryExecution::Configured);
      const Result<DeltaPoint> right = point(current->k + h, QueryExecution::Configured);
      if (!left || !right) {
        break;
      }
      slope = (right->residual - left->residual) / (2.0 * h);
    }
    if (!(std::isfinite(slope) && std::fabs(slope) > 1.0e-12)) {
      break;
    }
    const double step =
        std::clamp(-current->residual / slope, -max_log_strike_step, max_log_strike_step);
    if (!(std::isfinite(step) && std::fabs(step) > 1.0e-12)) {
      break;
    }
    Result<DeltaPoint> next = point(current->k + step, QueryExecution::ColdReference);
    if (!next) {
      break;
    }
    if (std::fabs(next->residual) <= tolerance) {
      return Ok(DeltaSolution{forward * std::exp(next->k), next->delta, evaluations});
    }
    previous = *current;
    have_previous = true;
    current = std::move(next);
  }
  return cold_fallback(evaluations);
}

// The solver's surface interface (forward_at / iv / q_eff_at / delta /
// query_pricing_tier) over a SurfaceRef whose vol is read under a non-default
// TenorInterp. Always cold: it reports a cold tier, so the screened wrapper goes
// straight to the cold solve, and `delta` is cold_route's ColdReference-equivalent.
class InterpSurface {
public:
  InterpSurface(const SurfaceRef &surface, TenorInterp interp) noexcept
      : surface_{surface}, interp_{interp} {}
  [[nodiscard]] double forward_at(double T) const noexcept { return surface_.forward_at(T); }
  [[nodiscard]] double iv(double K, double T) const noexcept { return surface_.iv(K, T, interp_); }
  [[nodiscard]] double q_eff_at(double T) const noexcept { return surface_.q_eff_at(T); }
  [[nodiscard]] Result<double> delta(double K, double T, Side side, QueryExecution) const {
    return cold_delta(surface_, K, T, side, interp_);
  }
  [[nodiscard]] QueryPricingTier query_pricing_tier() const noexcept {
    return QueryPricingTier::ColdReference;
  }

private:
  SurfaceRef surface_; // a 16-byte borrowed handle, copied
  TenorInterp interp_;
};

} // namespace

Result<AmericanDeltaSolution> solve_american_delta(const SurfaceRef &surface, double T, Side side,
                                                   double signed_target, double tolerance,
                                                   TenorInterp interp) {
  const bool call = side == Side::Call;
  const bool sign_ok = call ? (signed_target > 0.0 && signed_target < 1.0)
                            : (signed_target < 0.0 && signed_target > -1.0);
  if (!sign_ok || (side != Side::Call && side != Side::Put)) {
    return Err(ErrorCode::InvalidArgument,
               "american delta solve: target must lie in (0,1) for a call, (-1,0) for a put");
  }
  const double target_abs = std::fabs(signed_target);
  Result<DeltaSolution> solved =
      interp == TenorInterp::TotalVariance
          ? solve_american_delta_screened(surface, T, side, target_abs, tolerance)
          : solve_american_delta_screened(InterpSurface{surface, interp}, T, side, target_abs,
                                          tolerance);
  if (!solved) {
    return Err(std::move(solved).error());
  }
  return Ok(AmericanDeltaSolution{solved->strike, solved->achieved_delta, solved->evaluations});
}

} // namespace atx::vol::risk_detail
