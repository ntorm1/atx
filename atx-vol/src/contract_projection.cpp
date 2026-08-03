#include "atx/vol/contract_projection.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/vol/detail/parallel_for.hpp"
#include "atx/vol/simd/cpu.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
namespace time = atx::core::time;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

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
    return kNaN;
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

[[nodiscard]] Result<DeltaSolution> solve_american_delta(const SurfaceRef &surface, double T,
                                                         Side side, double target_abs_delta,
                                                         double tolerance,
                                                         QueryExecution query_execution) {
  if (!(target_abs_delta > 0.0 && target_abs_delta < 1.0) ||
      !(std::isfinite(tolerance) && tolerance > 0.0 && tolerance <= 1.0e-3)) {
    return Err(ErrorCode::InvalidArgument, "contract projection: invalid delta target/tolerance");
  }
  const double forward = surface.forward_at(T);
  double sigma = surface.iv(forward, T);
  if (!finite_positive(forward) || !finite_positive(sigma)) {
    return Err(ErrorCode::Unavailable, "contract projection: no surface at maturity");
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
    return Err(ErrorCode::Unavailable, "contract projection: delta seed unavailable");
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
    return Err(ErrorCode::NotFound, "contract projection: delta target not bracketed");
  }

  double root = seed_k;
  Residual root_residual = seed;
  int retained = 0;
  for (int iteration = 0; iteration < 64; ++iteration) {
    double candidate = 0.5 * (lo + hi);
    if (rlo.exact && rhi.exact && rlo.value != rhi.value) {
      const double secant = (rlo.value * hi - rhi.value * lo) / (rlo.value - rhi.value);
      const double margin = 1.0e-6 * (hi - lo);
      if (secant > lo + margin && secant < hi - margin) {
        candidate = secant;
      }
    }
    root = candidate;
    root_residual = evaluate(candidate);
    if (root_residual.exact && std::fabs(root_residual.value) <= tolerance) {
      break;
    }
    if ((root_residual.value <= 0.0) == (rlo.value <= 0.0)) {
      lo = candidate;
      rlo = root_residual;
      if (retained == -1 && rhi.exact) {
        rhi.value *= 0.5;
      }
      retained = -1;
    } else {
      hi = candidate;
      rhi = root_residual;
      if (retained == 1 && rlo.exact) {
        rlo.value *= 0.5;
      }
      retained = 1;
    }
  }
  if (!root_residual.exact || std::fabs(root_residual.value) > tolerance) {
    return Err(ErrorCode::Unavailable, "contract projection: delta solve did not converge");
  }
  return Ok(DeltaSolution{forward * std::exp(root), root_residual.delta, evaluations});
}

[[nodiscard]] std::uint16_t add_evaluations(std::uint16_t left, std::uint16_t right) noexcept {
  const unsigned sum = static_cast<unsigned>(left) + static_cast<unsigned>(right);
  return static_cast<std::uint16_t>(
      std::min(sum, static_cast<unsigned>(std::numeric_limits<std::uint16_t>::max())));
}

[[nodiscard]] Result<DeltaSolution> solve_american_delta_screened(const SurfaceRef &surface,
                                                                  double T, Side side,
                                                                  double target_abs_delta,
                                                                  double tolerance) {
  const auto cold_fallback = [&](std::uint16_t prior_evaluations) -> Result<DeltaSolution> {
    Result<DeltaSolution> cold = solve_american_delta(surface, T, side, target_abs_delta, tolerance,
                                                      QueryExecution::ColdReference);
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
  Result<DeltaSolution> screen = solve_american_delta(surface, T, side, target_abs_delta,
                                                      screen_tolerance, QueryExecution::Configured);
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
      return Err(ErrorCode::OutOfRange, "contract projection: adaptive strike overflow");
    }
    ATX_TRY(const double delta, surface.delta(strike, T, side, execution));
    if (!std::isfinite(delta)) {
      return Err(ErrorCode::Unavailable, "contract projection: adaptive delta unavailable");
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

// Bounded batch-pass budget for solve_american_delta_batch: pass 0 (seed
// evaluation), pass 1 (Newton), passes 2.. (secant refinement).
constexpr std::size_t kMaxBatchDeltaPasses = 6;

// TASK-4 SEAM: the scalar-fallback tail of solve_american_delta_batch. Rows
// land here when a pass errored, produced a non-finite delta, hit a degenerate
// Newton/secant update, or exhausted the batch pass budget. Task 4 replaces
// this placeholder with the robust per-row cold scalar solver
// (solve_american_delta at QueryExecution::ColdReference) plus the per-row
// failure taxonomy; until then every routed row keeps the status recorded at
// routing time (the propagated pass error or a placeholder Unavailable) and
// NaN strike/achieved-delta outputs.
void solve_batch_fallback_tail([[maybe_unused]] const SurfaceRef &surface,
                               [[maybe_unused]] std::span<const double> T,
                               [[maybe_unused]] std::span<const Side> side,
                               [[maybe_unused]] std::span<const double> target_abs_delta,
                               [[maybe_unused]] double tolerance,
                               [[maybe_unused]] std::span<const std::uint32_t> fallback_rows,
                               [[maybe_unused]] std::span<double> strike_out,
                               [[maybe_unused]] std::span<double> achieved_delta_out,
                               [[maybe_unused]] std::span<std::uint16_t> evaluations_out,
                               [[maybe_unused]] std::span<Status> row_status_out) {}

[[nodiscard]] bool valid_delta_solve_policy(OptionDeltaSolvePolicy policy) noexcept {
  return policy == OptionDeltaSolvePolicy::Direct ||
         policy == OptionDeltaSolvePolicy::FastScreenColdConfirm ||
         policy == OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
}

[[nodiscard]] OptionProjectionStatus status_from_error(const Error &error) noexcept {
  switch (error.code()) {
  case ErrorCode::NotFound:
    return OptionProjectionStatus::DeltaUnreachable;
  case ErrorCode::Unavailable:
    return OptionProjectionStatus::DeltaUnreachable;
  case ErrorCode::OutOfRange:
    return OptionProjectionStatus::MaturityUnavailable;
  default:
    return OptionProjectionStatus::InvalidSpec;
  }
}

[[nodiscard]] bool valid_spec(const OptionProjectionSpec &spec) noexcept {
  return spec.uid != 0u && finite_positive(spec.multiplier) && std::isfinite(spec.strike.value);
}

} // namespace

Result<std::int64_t> resolve_projected_expiry(std::int64_t valuation_ts_ns,
                                              const ProjectedMaturitySpec &spec) {
  if (valuation_ts_ns <= 0) {
    return Err(ErrorCode::InvalidArgument, "contract projection: invalid valuation timestamp");
  }
  switch (spec.kind) {
  case ProjectedMaturityKind::YearFraction: {
    if (!finite_positive(spec.year_fraction)) {
      return Err(ErrorCode::InvalidArgument, "contract projection: year fraction must be positive");
    }
    const long double offset =
        static_cast<long double>(spec.year_fraction) * static_cast<long double>(kNsPerYear);
    const long double available =
        static_cast<long double>(std::numeric_limits<std::int64_t>::max() - valuation_ts_ns);
    if (!(offset >= 1.0L && offset <= available)) {
      return Err(ErrorCode::OutOfRange, "contract projection: maturity overflows timestamp");
    }
    return Ok(valuation_ts_ns + static_cast<std::int64_t>(std::llround(offset)));
  }
  case ProjectedMaturityKind::CalendarDays: {
    if (spec.calendar_count <= 0) {
      return Err(ErrorCode::InvalidArgument, "contract projection: calendar days must be positive");
    }
    constexpr std::int64_t day_ns = 86'400'000'000'000LL;
    if (spec.calendar_count >
        (std::numeric_limits<std::int64_t>::max() - valuation_ts_ns) / day_ns) {
      return Err(ErrorCode::OutOfRange, "contract projection: day maturity overflows");
    }
    return Ok(valuation_ts_ns + static_cast<std::int64_t>(spec.calendar_count) * day_ns);
  }
  case ProjectedMaturityKind::CalendarMonths: {
    if (spec.calendar_count <= 0 || spec.calendar_count > 1200) {
      return Err(ErrorCode::InvalidArgument,
                 "contract projection: calendar months outside (0,1200]");
    }
    const time::CivilTime civil =
        time::to_civil_utc(time::Timestamp::from_unix_nanos(valuation_ts_ns));
    const std::int64_t month_index = static_cast<std::int64_t>(civil.date.year) * 12 +
                                     static_cast<std::int64_t>(civil.date.month - 1u) +
                                     spec.calendar_count;
    const auto year = static_cast<std::int32_t>(month_index / 12);
    const auto month = static_cast<std::uint32_t>(month_index % 12 + 1);
    if (year < 1678 || year > 2261) {
      return Err(ErrorCode::OutOfRange, "contract projection: calendar maturity overflows");
    }
    const std::uint32_t day = std::min(civil.date.day, time::days_in_month(year, month));
    const std::int64_t expiry = time::timestamp_from_utc(year, month, day, civil.hour, civil.minute,
                                                         civil.second, civil.nano)
                                    .unix_nanos();
    return expiry > valuation_ts_ns
               ? Ok(expiry)
               : Err(ErrorCode::OutOfRange, "contract projection: nonpositive calendar tenor");
  }
  case ProjectedMaturityKind::AbsoluteExpiry:
    return spec.expiry_ts_ns > valuation_ts_ns
               ? Ok(spec.expiry_ts_ns)
               : Err(ErrorCode::InvalidArgument,
                     "contract projection: absolute expiry is not after valuation");
  }
  return Err(ErrorCode::InvalidArgument, "contract projection: unknown maturity kind");
}

std::uint64_t projected_definition_fingerprint(const ProjectedOptionDefinition &definition) {
  std::uint64_t words[] = {
      definition.contract.uid,
      std::bit_cast<std::uint64_t>(definition.contract.K),
      std::bit_cast<std::uint64_t>(definition.contract.T),
      static_cast<std::uint64_t>(definition.contract.side),
      static_cast<std::uint64_t>(definition.valuation_ts_ns),
      static_cast<std::uint64_t>(definition.expiry_ts_ns),
      std::bit_cast<std::uint64_t>(definition.multiplier),
  };
  const std::uint64_t hash = atx::core::hash_bytes(words, sizeof words);
  return hash == 0u ? 1u : hash;
}

void AmericanDeltaBatchScratch::resize(std::size_t n) {
  k_log.resize(n);
  strike.resize(n);
  residual.resize(n);
  prev_k.resize(n);
  prev_residual.resize(n);
  forward.resize(n);
  sigma.resize(n);
  signed_d1.resize(n);
  iv.resize(n);
  price.resize(n);
  greeks.resize(n);
  pass_status.resize(n);
  active.clear();
  active.reserve(n);
  active_strike.clear();
  active_strike.reserve(n);
  active_t.clear();
  active_t.reserve(n);
  active_side.clear();
  active_side.reserve(n);
}

Status solve_american_delta_batch(const SurfaceRef &surface, std::span<const double> T,
                                  std::span<const Side> side,
                                  std::span<const double> target_abs_delta, double tolerance,
                                  AmericanDeltaBatchScratch &scratch, std::span<double> strike_out,
                                  std::span<double> achieved_delta_out,
                                  std::span<std::uint16_t> evaluations_out,
                                  std::span<Status> row_status_out) {
  const std::size_t n = T.size();
  if (surface == nullptr || n == 0u || side.size() != n || target_abs_delta.size() != n ||
      strike_out.size() != n || achieved_delta_out.size() != n || evaluations_out.size() != n ||
      row_status_out.size() != n) {
    return Err(ErrorCode::InvalidArgument,
               "contract projection: batch delta solve span/size mismatch");
  }
  if (!(std::isfinite(tolerance) && tolerance > 0.0 && tolerance <= 1.0e-3)) {
    return Err(ErrorCode::InvalidArgument, "contract projection: invalid delta target/tolerance");
  }

  scratch.resize(n);
  // TASK-4 SEAM input: stable-ordered ids of rows the batch passes could not
  // finish; empty (never allocates) on the happy path.
  std::vector<std::uint32_t> fallback_rows;
  const auto route_to_fallback = [&](std::uint32_t row, Status status) {
    strike_out[row] = kNaN;
    achieved_delta_out[row] = kNaN;
    row_status_out[row] = std::move(status);
    fallback_rows.push_back(row);
  };

  // Seed (curve reads only, no boundary solves): the Black-style inverse-delta
  // seed with two smile-refresh iterations, mirroring solve_american_delta.
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(n); ++i) {
    strike_out[i] = kNaN;
    achieved_delta_out[i] = kNaN;
    evaluations_out[i] = 0u;
    row_status_out[i] = Ok();
    const double target = target_abs_delta[i];
    if (!(target > 0.0 && target < 1.0)) {
      row_status_out[i] =
          Err(ErrorCode::InvalidArgument, "contract projection: invalid delta target/tolerance");
      continue;
    }
    const double t = T[i];
    const double forward = surface.forward_at(t);
    double sigma = surface.iv(forward, t);
    if (!finite_positive(forward) || !finite_positive(sigma)) {
      row_status_out[i] =
          Err(ErrorCode::Unavailable, "contract projection: no surface at maturity");
      continue;
    }
    const double carry_discount = std::exp(-surface.q_eff_at(t) * t);
    const double probability = std::clamp(target / carry_discount, 1.0e-8, 1.0 - 1.0e-8);
    const double signed_d1 =
        side[i] == Side::Call ? inverse_normal_cdf(probability) : -inverse_normal_cdf(probability);
    const double sqrt_t = std::sqrt(t);
    double seed_k = 0.0;
    for (int iteration = 0; iteration < 2; ++iteration) {
      seed_k = -signed_d1 * sigma * sqrt_t + 0.5 * sigma * sigma * t;
      const double smile_sigma = surface.iv(forward * std::exp(seed_k), t);
      if (finite_positive(smile_sigma)) {
        sigma = smile_sigma;
      }
    }
    const double seed_strike = forward * std::exp(seed_k);
    if (!finite_positive(seed_strike)) {
      route_to_fallback(
          i, Err(ErrorCode::Unavailable, "contract projection: batch delta seed strike invalid"));
      continue;
    }
    scratch.forward[i] = forward;
    scratch.sigma[i] = sigma;
    scratch.signed_d1[i] = signed_d1;
    scratch.k_log[i] = seed_k;
    scratch.strike[i] = seed_strike;
    scratch.active.push_back(i);
  }

  // One laned cold pass over the active rows: stable compaction (ascending row
  // ids, contiguous bit-identical-T runs preserved), one dense evaluate_batch
  // on the FirstOrder bundle with the reduced GreekNeeds — the exact request
  // shape detail::laned_greek_route_selected admits to the AVX2 laned Greek
  // kernels (no selective Delta/Vega bits) — then freeze / route / retain.
  const auto run_pass = [&]() -> Status {
    const std::size_t m = scratch.active.size();
    scratch.active_strike.clear();
    scratch.active_t.clear();
    scratch.active_side.clear();
    for (const std::uint32_t row : scratch.active) {
      scratch.active_strike.push_back(scratch.strike[row]);
      scratch.active_t.push_back(T[row]);
      scratch.active_side.push_back(side[row]);
    }
    const PricedSurface::EvaluationSoA out{
        .iv = std::span<double>(scratch.iv.data(), m),
        .price = std::span<double>(scratch.price.data(), m),
        .greeks = std::span<AmericanGreeks>(scratch.greeks.data(), m),
        .status = std::span<Status>(scratch.pass_status.data(), m)};
    const Status evaluated = surface.evaluate_batch(
        std::span<const double>(scratch.active_strike.data(), m),
        std::span<const double>(scratch.active_t.data(), m),
        std::span<const Side>(scratch.active_side.data(), m), PricedSurface::EvalField::FirstOrder,
        /*analytic=*/true, out, simd::SimdIsa::Auto, QueryExecution::ColdReference,
        GreekNeeds{.vega = false, .rho = false, .charm = false});
    if (!evaluated) {
      return evaluated;
    }
    std::size_t keep = 0;
    for (std::size_t j = 0; j < m; ++j) {
      const std::uint32_t row = scratch.active[j];
      evaluations_out[row] = add_evaluations(evaluations_out[row], 1u);
      if (!scratch.pass_status[j]) {
        route_to_fallback(row, Err(scratch.pass_status[j].error()));
        continue;
      }
      const double delta = scratch.greeks[j].delta;
      if (!std::isfinite(delta)) {
        route_to_fallback(
            row, Err(ErrorCode::Unavailable, "contract projection: batch delta unavailable"));
        continue;
      }
      const double residual = std::fabs(delta) - target_abs_delta[row];
      // Half-tolerance internal acceptance: the margin absorbs the documented
      // laned-vs-scalar kernel gap so the SCALAR cold oracle holds at the full
      // tolerance. Load-bearing for Task 4's oracle guarantee — never relax.
      if (std::fabs(residual) <= 0.5 * tolerance) {
        strike_out[row] = scratch.strike[row];
        achieved_delta_out[row] = delta;
        row_status_out[row] = Ok();
        continue;
      }
      scratch.residual[row] = residual;
      scratch.active[keep++] = row;
    }
    scratch.active.resize(keep);
    return Ok();
  };

  // Pass 0: evaluate every seeded row.
  if (!scratch.active.empty()) {
    const Status pass = run_pass();
    if (!pass) {
      return pass;
    }
  }

  // Pass 1: one safeguarded Newton step per row off the closed-form Black
  // delta slope (same formula as the scalar solver's bracketing step).
  if (!scratch.active.empty()) {
    constexpr double inv_sqrt_two_pi = 0.39894228040143267794;
    std::size_t keep = 0;
    for (const std::uint32_t row : scratch.active) {
      const double t = T[row];
      const double carry_discount = std::exp(-surface.q_eff_at(t) * t);
      const double signed_d1 = scratch.signed_d1[row];
      const double density = inv_sqrt_two_pi * std::exp(-0.5 * signed_d1 * signed_d1);
      const double slope_magnitude = carry_discount * density / (scratch.sigma[row] * std::sqrt(t));
      const double slope = side[row] == Side::Call ? -slope_magnitude : slope_magnitude;
      const double step = std::clamp(-scratch.residual[row] / slope, -0.50, 0.50);
      const double next_k = scratch.k_log[row] + step;
      const double next_strike = scratch.forward[row] * std::exp(next_k);
      if (!std::isfinite(step) || !finite_positive(next_strike)) {
        route_to_fallback(row, Err(ErrorCode::Unavailable,
                                   "contract projection: batch delta Newton step degenerate"));
        continue;
      }
      scratch.prev_k[row] = scratch.k_log[row];
      scratch.prev_residual[row] = scratch.residual[row];
      scratch.k_log[row] = next_k;
      scratch.strike[row] = next_strike;
      scratch.active[keep++] = row;
    }
    scratch.active.resize(keep);
    if (!scratch.active.empty()) {
      const Status pass = run_pass();
      if (!pass) {
        return pass;
      }
    }
  }

  // Passes 2..kMaxBatchDeltaPasses: secant refinement from (prev_k,
  // prev_residual) and the current point; the active set shrinks and stays
  // stably ordered.
  for (std::size_t pass_index = 2; pass_index < kMaxBatchDeltaPasses && !scratch.active.empty();
       ++pass_index) {
    std::size_t keep = 0;
    for (const std::uint32_t row : scratch.active) {
      const double residual_gap = scratch.residual[row] - scratch.prev_residual[row];
      const double slope = residual_gap / (scratch.k_log[row] - scratch.prev_k[row]);
      const double step = std::clamp(-scratch.residual[row] / slope, -0.25, 0.25);
      const double next_k = scratch.k_log[row] + step;
      const double next_strike = scratch.forward[row] * std::exp(next_k);
      if (!std::isfinite(slope) || std::fabs(residual_gap) < 1.0e-14 || !std::isfinite(step) ||
          !finite_positive(next_strike)) {
        route_to_fallback(row, Err(ErrorCode::Unavailable,
                                   "contract projection: batch delta secant step degenerate"));
        continue;
      }
      scratch.prev_k[row] = scratch.k_log[row];
      scratch.prev_residual[row] = scratch.residual[row];
      scratch.k_log[row] = next_k;
      scratch.strike[row] = next_strike;
      scratch.active[keep++] = row;
    }
    scratch.active.resize(keep);
    if (scratch.active.empty()) {
      break;
    }
    const Status pass = run_pass();
    if (!pass) {
      return pass;
    }
  }

  // Rows still active after the pass budget go to the Task-4 scalar tail.
  for (const std::uint32_t row : scratch.active) {
    route_to_fallback(row, Err(ErrorCode::Unavailable,
                               "contract projection: batch delta solve did not converge"));
  }
  scratch.active.clear();
  solve_batch_fallback_tail(surface, T, side, target_abs_delta, tolerance, fallback_rows,
                            strike_out, achieved_delta_out, evaluations_out, row_status_out);
  return Ok();
}

ProjectedMaturitySpec ProjectedMaturitySpec::years(double value) noexcept {
  ProjectedMaturitySpec spec;
  spec.kind = ProjectedMaturityKind::YearFraction;
  spec.year_fraction = value;
  return spec;
}

ProjectedMaturitySpec ProjectedMaturitySpec::days(std::int32_t value) noexcept {
  ProjectedMaturitySpec spec;
  spec.kind = ProjectedMaturityKind::CalendarDays;
  spec.calendar_count = value;
  return spec;
}

ProjectedMaturitySpec ProjectedMaturitySpec::months(std::int32_t value) noexcept {
  ProjectedMaturitySpec spec;
  spec.kind = ProjectedMaturityKind::CalendarMonths;
  spec.calendar_count = value;
  return spec;
}

ProjectedMaturitySpec ProjectedMaturitySpec::absolute(std::int64_t expiry) noexcept {
  ProjectedMaturitySpec spec;
  spec.kind = ProjectedMaturityKind::AbsoluteExpiry;
  spec.expiry_ts_ns = expiry;
  return spec;
}

ProjectedStrikeSpec ProjectedStrikeSpec::atm_forward() noexcept { return {}; }

ProjectedStrikeSpec ProjectedStrikeSpec::delta(double target_abs_delta) noexcept {
  return ProjectedStrikeSpec{ProjectedStrikeKind::Delta, target_abs_delta};
}

ProjectedStrikeSpec ProjectedStrikeSpec::log_moneyness(double k) noexcept {
  return ProjectedStrikeSpec{ProjectedStrikeKind::LogMoneyness, k};
}

ProjectedStrikeSpec ProjectedStrikeSpec::absolute(double strike) noexcept {
  return ProjectedStrikeSpec{ProjectedStrikeKind::AbsoluteStrike, strike};
}

const char *to_string(OptionProjectionStatus status) noexcept {
  switch (status) {
  case OptionProjectionStatus::Ok:
    return "Ok";
  case OptionProjectionStatus::InvalidSpec:
    return "InvalidSpec";
  case OptionProjectionStatus::SurfaceUnavailable:
    return "SurfaceUnavailable";
  case OptionProjectionStatus::MaturityUnavailable:
    return "MaturityUnavailable";
  case OptionProjectionStatus::DeltaUnreachable:
    return "DeltaUnreachable";
  case OptionProjectionStatus::PricingError:
    return "PricingError";
  }
  return "Unknown";
}

Result<ProjectedOption> project_option_contract(const SurfaceRef &surface,
                                                const OptionProjectionSpec &spec,
                                                const OptionProjectionConfig &config) {
  if (!valid_spec(spec) || surface.uid() != spec.uid ||
      !(std::isfinite(config.delta_tolerance) && config.delta_tolerance > 0.0 &&
        config.delta_tolerance <= 1.0e-3) ||
      !valid_delta_solve_policy(config.delta_solve_policy)) {
    return Err(ErrorCode::InvalidArgument, "contract projection: invalid spec/config/surface uid");
  }
  const std::int64_t valuation = surface.pricing().now_ts_ns;
  ATX_TRY(const std::int64_t expiry, resolve_projected_expiry(valuation, spec.maturity));
  const double residual_t = static_cast<double>(expiry - valuation) / kNsPerYear;
  if (!finite_positive(residual_t)) {
    return Err(ErrorCode::OutOfRange, "contract projection: nonpositive residual maturity");
  }
  const double forward = surface.forward_at(residual_t);
  if (!finite_positive(forward)) {
    return Err(ErrorCode::Unavailable, "contract projection: forward unavailable");
  }

  double strike = 0.0;
  double achieved_delta = 0.0;
  std::uint16_t delta_evaluations = 0;
  switch (spec.strike.kind) {
  case ProjectedStrikeKind::AtmForward:
    strike = forward;
    break;
  case ProjectedStrikeKind::Delta: {
    const bool use_screened_solver =
        config.delta_solve_policy == OptionDeltaSolvePolicy::FastScreenColdConfirm ||
        config.delta_solve_policy == OptionDeltaSolvePolicy::CrossSectionalColdConfirm;
    Result<DeltaSolution> solved =
        use_screened_solver
            ? solve_american_delta_screened(surface, residual_t, spec.side, spec.strike.value,
                                            config.delta_tolerance)
            : solve_american_delta(surface, residual_t, spec.side, spec.strike.value,
                                   config.delta_tolerance, config.query_execution);
    if (!solved) {
      return Err(solved.error());
    }
    const DeltaSolution &solution = *solved;
    strike = solution.strike;
    achieved_delta = solution.achieved_delta;
    delta_evaluations = solution.evaluations;
    break;
  }
  case ProjectedStrikeKind::LogMoneyness:
    strike = forward * std::exp(spec.strike.value);
    break;
  case ProjectedStrikeKind::AbsoluteStrike:
    strike = spec.strike.value;
    break;
  }
  if (!finite_positive(strike)) {
    return Err(ErrorCode::InvalidArgument, "contract projection: resolved strike invalid");
  }
  const PricedSurface::ResolvedSurfacePoint point = surface.resolve(strike, residual_t);
  if (!point.valid || !finite_positive(point.sigma)) {
    return Err(ErrorCode::Unavailable, "contract projection: resolved surface point unavailable");
  }

  ProjectedOption out;
  out.definition.contract = OptionContract{spec.uid, strike, residual_t, spec.side};
  out.definition.valuation_ts_ns = valuation;
  out.definition.expiry_ts_ns = expiry;
  out.definition.multiplier = spec.multiplier;
  out.definition.fingerprint = projected_definition_fingerprint(out.definition);
  out.forward = forward;
  out.implied_vol = point.sigma;
  out.requested_abs_delta =
      spec.strike.kind == ProjectedStrikeKind::Delta ? spec.strike.value : 0.0;
  out.achieved_delta = achieved_delta;
  out.delta_evaluations = delta_evaluations;
  out.model_mark = kNaN;

  if (config.output == OptionProjectionOutput::DefinitionOnly) {
    return Ok(std::move(out));
  }
  if (config.output == OptionProjectionOutput::Mark) {
    const auto evaluated =
        surface.evaluate(strike, residual_t, spec.side, PricedSurface::EvalField::Price, false,
                         config.query_execution);
    if (!evaluated.status || !std::isfinite(evaluated.price)) {
      return Err(evaluated.status ? ErrorCode::Unavailable : evaluated.status.error().code(),
                 "contract projection: mark failed");
    }
    out.model_mark = evaluated.price;
    return Ok(std::move(out));
  }

  const auto fields = PricedSurface::EvalField::Price | PricedSurface::EvalField::FirstOrder |
                      PricedSurface::EvalField::SecondOrder;
  const auto evaluated = surface.evaluate(strike, residual_t, spec.side, fields,
                                          config.analytic_greeks, config.query_execution);
  if (!evaluated.status || !std::isfinite(evaluated.price)) {
    return Err(evaluated.status ? ErrorCode::Unavailable : evaluated.status.error().code(),
               "contract projection: full-risk evaluation failed");
  }
  out.model_mark = evaluated.price;
  out.greeks = evaluated.greeks;
  if (spec.strike.kind == ProjectedStrikeKind::Delta) {
    out.achieved_delta = out.greeks.delta;
    if (std::fabs(std::fabs(out.achieved_delta) - spec.strike.value) > config.delta_tolerance) {
      return Err(ErrorCode::Unavailable, "contract projection: terminal Greek delta misses target");
    }
  }
  return Ok(std::move(out));
}

Result<PreparedOptionProjection>
PreparedOptionProjection::create(std::span<const OptionProjectionSpec> specs) {
  if (specs.empty()) {
    return Err(ErrorCode::InvalidArgument, "contract projection: empty prepared plan");
  }
  PreparedOptionProjection prepared;
  prepared.specs_.assign(specs.begin(), specs.end());
  prepared.execution_order_.resize(specs.size());
  std::string fingerprint_material;
  fingerprint_material.reserve(specs.size() * 96u);
  for (std::size_t i = 0; i < specs.size(); ++i) {
    if (!valid_spec(specs[i])) {
      return Err(ErrorCode::InvalidArgument, "contract projection: invalid prepared spec");
    }
    prepared.execution_order_[i] = static_cast<std::uint32_t>(i);
    const std::uint64_t words[] = {
        specs[i].uid,
        static_cast<std::uint64_t>(specs[i].maturity.kind),
        std::bit_cast<std::uint64_t>(specs[i].maturity.year_fraction),
        static_cast<std::uint64_t>(specs[i].maturity.calendar_count),
        static_cast<std::uint64_t>(specs[i].maturity.expiry_ts_ns),
        static_cast<std::uint64_t>(specs[i].strike.kind),
        std::bit_cast<std::uint64_t>(specs[i].strike.value),
        static_cast<std::uint64_t>(specs[i].side),
        std::bit_cast<std::uint64_t>(specs[i].multiplier),
    };
    fingerprint_material.append(reinterpret_cast<const char *>(words), sizeof words);
  }
  std::stable_sort(prepared.execution_order_.begin(), prepared.execution_order_.end(),
                   [&](std::uint32_t left, std::uint32_t right) {
                     return std::tie(prepared.specs_[left].uid, left) <
                            std::tie(prepared.specs_[right].uid, right);
                   });
  prepared.fingerprint_ =
      atx::core::hash_bytes(fingerprint_material.data(), fingerprint_material.size());
  if (prepared.fingerprint_ == 0u) {
    prepared.fingerprint_ = 1u;
  }
  return Ok(std::move(prepared));
}

Status PreparedOptionProjection::project_into(const SurfaceSet &surfaces,
                                              std::span<ProjectedOption> output,
                                              const OptionProjectionConfig &config) const {
  if (output.size() != specs_.size() ||
      !(std::isfinite(config.delta_tolerance) && config.delta_tolerance > 0.0 &&
        config.delta_tolerance <= 1.0e-3) ||
      !valid_delta_solve_policy(config.delta_solve_policy)) {
    return Err(ErrorCode::InvalidArgument, "contract projection: invalid output/config");
  }
  const auto project_index = [&](std::size_t execution_index) {
    const std::size_t input_index = execution_order_[execution_index];
    const OptionProjectionSpec &spec = specs_[input_index];
    const SurfaceRef surface = surfaces.find(spec.uid);
    if (surface == nullptr) {
      ProjectedOption missing;
      missing.status = OptionProjectionStatus::SurfaceUnavailable;
      output[input_index] = missing;
      return;
    }
    Result<ProjectedOption> projected = project_option_contract(*surface, spec, config);
    if (projected) {
      output[input_index] = std::move(*projected);
      return;
    }
    ProjectedOption failed;
    failed.status = status_from_error(projected.error());
    output[input_index] = failed;
  };
  parallel_for_dynamic(execution_order_.size(), config.n_threads, project_index);
  return Ok();
}

} // namespace atx::vol
