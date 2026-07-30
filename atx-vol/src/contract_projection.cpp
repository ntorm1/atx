#include "atx/vol/contract_projection.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

#include "atx/core/datetime.hpp"
#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "atx/vol/detail/parallel_for.hpp"

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

[[nodiscard]] Result<std::int64_t> resolve_expiry(std::int64_t valuation_ts_ns,
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

[[nodiscard]] std::uint64_t definition_fingerprint(const ProjectedOptionDefinition &definition) {
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
        config.delta_tolerance <= 1.0e-3)) {
    return Err(ErrorCode::InvalidArgument, "contract projection: invalid spec/config/surface uid");
  }
  const std::int64_t valuation = surface.pricing().now_ts_ns;
  ATX_TRY(const std::int64_t expiry, resolve_expiry(valuation, spec.maturity));
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
    ATX_TRY(const DeltaSolution solution,
            solve_american_delta(surface, residual_t, spec.side, spec.strike.value,
                                 config.delta_tolerance, config.query_execution));
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
  out.definition.fingerprint = definition_fingerprint(out.definition);
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
        config.delta_tolerance <= 1.0e-3)) {
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
