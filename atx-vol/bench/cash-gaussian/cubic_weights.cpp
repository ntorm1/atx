#include "cubic_weights.hpp"

#include <array>
#include <cmath>
#include <exception>
#include <ios>
#include <limits>
#include <new>
#include <utility>

#include <boost/math/policies/policy.hpp>
#include <boost/math/special_functions/erf.hpp>
#include <boost/multiprecision/cpp_bin_float.hpp>

namespace atx::vol::cash_gaussian {
namespace {
using atx::core::Err;
using SpecialPolicy = boost::math::policies::policy<
    boost::math::policies::max_series_iterations<4096>,
    boost::math::policies::max_root_iterations<256>>;

template <unsigned Digits>
using Real = boost::multiprecision::number<boost::multiprecision::cpp_bin_float<Digits>>;

bool valid(const WeightRequest &r) noexcept {
  return std::isfinite(r.lower) && std::isfinite(r.upper) && r.lower >= 0. &&
      r.upper > r.lower && std::isfinite(r.stock) && r.stock > 0. &&
      std::isfinite(r.dt) && r.dt > 0. && std::isfinite(r.sigma) && r.sigma > 0. &&
      std::isfinite(r.rate) && std::isfinite(r.yield);
}

template <class R>
bool finite(const R &x) {
  return x == x && x != std::numeric_limits<R>::infinity() &&
      x != -std::numeric_limits<R>::infinity();
}

template <class R>
R normal_mass(const R &lo, const R &hi, WeightWork &work) {
  const R root_two = sqrt(R(2));
  R first, second;
  if (lo >= 0) {
    ++work.normal_functions;
    first = boost::math::erfc(R(lo / root_two), SpecialPolicy{});
    ++work.normal_functions;
    second = boost::math::erfc(R(hi / root_two), SpecialPolicy{});
  } else if (hi <= 0) {
    ++work.normal_functions;
    first = boost::math::erfc(R(-hi / root_two), SpecialPolicy{});
    ++work.normal_functions;
    second = boost::math::erfc(R(-lo / root_two), SpecialPolicy{});
  } else {
    ++work.normal_functions;
    first = boost::math::erf(R(hi / root_two), SpecialPolicy{});
    ++work.normal_functions;
    second = boost::math::erf(R(lo / root_two), SpecialPolicy{});
  }
  return (first - second) / 2;
}

// Coefficients of (Y-A)^j (1-Y)^(degree-j), before Bernstein normalization.
template <class R>
std::array<R, 4> basis(unsigned degree, unsigned j, const R &a) {
  std::array<R, 4> result{R(1), R(0), R(0), R(0)};
  for (unsigned k = 0; k < degree; ++k) {
    const R constant = k < j ? -a : R(1);
    const R slope = k < j ? R(1) : R(-1);
    std::array<R, 4> next{};
    for (unsigned n = 0; n <= k; ++n) {
      next[n] += constant * result[n];
      next[n+1] += slope * result[n];
    }
    result = std::move(next);
  }
  return result;
}

template <unsigned Digits>
Result<CubicWeights> calculate(const WeightRequest &r, const WeightBudget &budget,
                               WeightWork &work) {
  using R = Real<Digits>;
  work.decimal_digits = Digits;
  const R lower(r.lower), upper(r.upper), stock(r.stock), dt(r.dt), sigma(r.sigma);
  const R rate(r.rate), yield(r.yield);
  const R a = lower / upper, h = (upper - lower) / upper;
  const R variance = sigma * sigma * dt, sd = sqrt(variance);
  const R mu = log(stock / upper) + (rate - yield) * dt - variance / 2;
  const R log_a = lower == 0 ? -std::numeric_limits<R>::infinity() : log(a);
  std::array<R, 4> zlo{}, zhi{}, prefactor{}, moments{};
  // Validate the entire declared numerical domain before any special function.
  // Unsupported tails are refused; they are never silently discarded.
  for (unsigned k = 0; k < 4; ++k) {
    zlo[k] = (log_a - mu - R(k) * variance) / sd;
    zhi[k] = (-mu - R(k) * variance) / sd;
    prefactor[k] = -rate * dt + R(k) * mu + R(k * k) * variance / 2;
    if (!finite(zhi[k]) || abs(zhi[k]) > 256 ||
        (lower != 0 && (!finite(zlo[k]) || abs(zlo[k]) > 256)) ||
        !finite(prefactor[k]) || abs(prefactor[k]) > 32768) {
      return Err(ErrorCode::OutOfRange, "cubic weight numerical domain");
    }
  }
  for (unsigned k = 0; k < 4; ++k) {
    if (work.moment_attempts >= budget.max_moments ||
        work.normal_functions > budget.max_normal_functions ||
        budget.max_normal_functions - work.normal_functions < 2) {
      return Err(ErrorCode::Unavailable, "cubic weight work budget");
    }
    ++work.moment_attempts;
    const R mass = normal_mass(zlo[k], zhi[k], work);
    moments[k] = exp(prefactor[k]) * mass;
    if (!finite(moments[k]) || !(moments[k] > 0))
      return Err(ErrorCode::OutOfRange, "cubic weight moment arithmetic");
  }

  R maximum_condition = 1;
  const R condition_limit = pow(R(10), Digits - 30);
  const auto combine = [&](const std::array<R, 4> &coefficients) -> Result<R> {
    R sum = 0, absolute_sum = 0;
    for (unsigned k = 0; k < 4; ++k) {
      ++work.polynomial_terms;
      const R term = coefficients[k] * moments[k];
      sum += term;
      absolute_sum += abs(term);
    }
    if (!finite(sum) || !(sum > 0))
      return Err(ErrorCode::OutOfRange, "cubic weight signed cancellation");
    const R condition = absolute_sum / sum;
    if (!finite(condition) || condition > condition_limit)
      return Err(ErrorCode::OutOfRange, "cubic weight conditioning");
    if (condition > maximum_condition) maximum_condition = condition;
    return sum;
  };

  std::array<R, 4> weights{};
  std::array<R, 3> derivative{};
  const R h2 = h * h, h3 = h2 * h;
  for (unsigned j = 0; j < 4; ++j) {
    auto coefficients = basis<R>(3, j, a);
    const R scale = R(j == 0 || j == 3 ? 1 : 3) / h3;
    for (auto &coefficient : coefficients) coefficient *= scale;
    auto value = combine(coefficients);
    if (!value) return Err(value.error());
    weights[j] = *value;
  }
  for (unsigned j = 0; j < 3; ++j) {
    const auto polynomial = basis<R>(2, j, a);
    std::array<R, 4> coefficients{};
    const R scale = R(j == 1 ? 2 : 1) * (upper / stock) / h2;
    for (unsigned k = 0; k < 3; ++k) coefficients[k+1] = scale * polynomial[k];
    auto value = combine(coefficients);
    if (!value) return Err(value.error());
    derivative[j] = *value;
  }
  R weight_sum = 0, derivative_sum = 0;
  for (const auto &w : weights) weight_sum += w;
  for (const auto &d : derivative) derivative_sum += d;
  const R stock_ratio = (upper / stock) * moments[1];
  const R relative_guard("1e-25");
  if (abs(weight_sum - moments[0]) > relative_guard * moments[0] ||
      abs(derivative_sum - stock_ratio) > relative_guard * stock_ratio) {
    return Err(ErrorCode::OutOfRange, "cubic weight conservation");
  }

  const auto encode = [](const R &value) { return value.str(Digits, std::ios_base::scientific); };
  CubicWeights out;
  for (unsigned k = 0; k < 4; ++k) {
    out.value[k] = encode(weights[k]);
    out.scaled_moments[k] = encode(moments[k]);
  }
  for (unsigned k = 0; k < 3; ++k) out.derivative[k] = encode(derivative[k]);
  out.mass = encode(moments[0]);
  out.discounted_stock_ratio = encode(stock_ratio);
  out.maximum_condition = encode(maximum_condition);
  return out;
}
} // namespace

Result<CubicWeights> cubic_weight_control(const WeightRequest &request, WeightPrecision precision,
    const WeightBudget &budget, WeightWork &work) {
  work = {};
  if (!valid(request)) return Err(ErrorCode::InvalidArgument, "cubic weight Gaussian input");
  try {
    switch (precision) {
      case WeightPrecision::Decimal100: return calculate<100>(request, budget, work);
      case WeightPrecision::Decimal200: return calculate<200>(request, budget, work);
    }
    return Err(ErrorCode::InvalidArgument, "cubic weight precision");
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "cubic weight allocation");
  } catch (const std::exception &) {
    return Err(ErrorCode::OutOfRange, "cubic weight special-function arithmetic");
  }
}
} // namespace atx::vol::cash_gaussian
