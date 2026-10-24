#include "cubic_weights.hpp"
#include "cubic_local_weights.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <ios>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <utility>

#include <boost/math/policies/policy.hpp>
#include <boost/math/constants/constants.hpp>
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

template <class R>
struct RawWeightMoments {
  R upper, stock, a, h;
  std::array<R, 4> moments;
};

template <unsigned Digits>
Result<RawWeightMoments<Real<Digits>>> raw_weight_moments(
    const WeightRequest &r, const WeightBudget &budget, WeightWork &work) {
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

  return RawWeightMoments<R>{upper, stock, a, h, moments};
}

template <unsigned Digits>
Result<CubicWeights> calculate(const WeightRequest &r, const WeightBudget &budget,
                               WeightWork &work) {
  using R = Real<Digits>;
  auto raw = raw_weight_moments<Digits>(r, budget, work);
  if (!raw) return Err(raw.error());
  const auto &upper = raw->upper;
  const auto &stock = raw->stock;
  const auto &a = raw->a;
  const auto &h = raw->h;
  const auto &moments = raw->moments;
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

struct LocalWorkLimit { const char *message; };
struct LocalNumericFailure { const char *message; };

// Bounds follow cubic-local-series-plan.md. The small rational is assembled
// exactly before any multiprecision division. All integer products are checked.
Result<BernsteinMonomialIntegral> bernstein_integral(unsigned degree, unsigned j,
                                                   unsigned power) {
  if ((degree != 2 && degree != 3) || j > degree || power > 513)
    return Err(ErrorCode::InvalidArgument, "local Bernstein integral domain");
  constexpr std::array<std::array<int, 4>, 3> quadratic{{
      {{1, -2, 1, 0}}, {{1, 0, -1, 0}}, {{1, 2, 1, 0}}}};
  constexpr std::array<std::array<int, 4>, 4> cubic{{
      {{1, -3, 3, -1}}, {{1, -1, -1, 1}},
      {{1, 1, -1, -1}}, {{1, 3, 3, 1}}}};
  const auto &p = degree == 2 ? quadratic[j] : cubic[j];
  std::int64_t numerator = 0, denominator = 1;
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  for (unsigned l = 0; l <= degree; ++l) {
    if ((power+l) % 2 != 0 || p[l] == 0) continue;
    const auto next = static_cast<std::int64_t>(power+l+1);
    if (denominator > maximum/next || std::abs(numerator) > maximum/next ||
        denominator > maximum/std::abs(p[l]))
      return Err(ErrorCode::OutOfRange, "local Bernstein rational product");
    const auto first = numerator*next;
    const auto second = static_cast<std::int64_t>(p[l])*denominator;
    if ((second > 0 && first > maximum-second) ||
        (second < 0 && first < -maximum-second))
      return Err(ErrorCode::OutOfRange, "local Bernstein rational sum");
    numerator = first+second;
    denominator *= next;
  }
  const std::int64_t choose = j == 0 || j == degree ? 1 : degree;
  const std::int64_t scale = std::int64_t{1} << degree;
  if (std::abs(numerator) > maximum/choose || denominator > maximum/scale)
    return Err(ErrorCode::OutOfRange, "local Bernstein rational scale");
  numerator *= choose;
  denominator *= scale;
  const auto divisor = std::gcd(numerator, denominator);
  return BernsteinMonomialIntegral{numerator/divisor,
      static_cast<std::uint64_t>(denominator/divisor)};
}

struct LocalCounter {
  const LocalWeightBudget &budget;
  LocalWeightWork &work;

  template <class F>
  auto function(F &&f) {
    if (work.local_functions >= budget.max_local_functions)
      throw LocalWorkLimit{"local cubic function budget"};
    ++work.local_functions;
    return f();
  }
  template <class R>
  R product(const R &a, const R &b) {
    if (work.coefficient_products >= budget.max_coefficient_products)
      throw LocalWorkLimit{"local cubic coefficient budget"};
    ++work.coefficient_products;
    return a*b;
  }
  void selection() {
    if (work.selection_checks >= budget.max_selection_checks)
      throw LocalWorkLimit{"local cubic selection budget"};
    ++work.selection_checks;
  }
};

template <class R>
struct LocalWorkspace {
  std::array<R, 514> exponent{}, term{}, next{}, sum{};
};

template <unsigned Digits>
Result<LocalCubicWeights> calculate_local(const WeightRequest &request,
    const LocalWeightBudget &budget, LocalWeightWork &work) {
  using R = Real<Digits>;
  LocalCounter counter{budget, work};
  const auto original = [&](LocalWeightReason reason) -> Result<LocalCubicWeights> {
    work.route = LocalWeightRoute::OriginalMoments;
    work.reason = reason;
    auto value = calculate<Digits>(request, budget.moments, work.moments);
    if (!value) return Err(value.error());
    LocalCubicWeights out;
    out.weights = std::move(*value);
    return out;
  };
  if (request.lower == 0.) return original(LocalWeightReason::ZeroLower);
  const R lower(request.lower), upper(request.upper), stock(request.stock);
  const R h = upper-lower, center = lower+h/2;
  const R epsilon = h/(2*center);
  if (epsilon > R(1)/16) return original(LocalWeightReason::WideCell);
  const R dt(request.dt), sigma(request.sigma), rate(request.rate), yield(request.yield);
  const R variance = sigma*sigma*dt;
  const R sd = counter.function([&] { return R(sqrt(variance)); });
  const R ratio_delta = (center-stock)/stock;
  work.used_log1p = abs(ratio_delta) <= R(1)/2;
  const R log_ratio = counter.function([&] {
    return R(work.used_log1p ? R(log1p(ratio_delta)) : R(log(center/stock)));
  });
  const R drift = (rate-yield-sigma*sigma/2)*dt;
  const R z = (log_ratio-drift)/sd;
  const R a = 1+z/sd, b = 1/(2*variance);
  const R ell = counter.function([&] { return R(-log1p(-epsilon)); });
  const R c = abs(a)*ell+b*ell*ell;
  if (!finite(c) || !finite(z) || !(epsilon > 0))
    return Err(ErrorCode::OutOfRange, "local cubic density arithmetic");
  if (c > R(1)/8) return original(LocalWeightReason::DensityVariation);
  const R exp_c = counter.function([&] { return R(exp(c)); });
  const R tau = Digits == 100 ? R("1e-80") : R("1e-180");
  R epsilon_power = epsilon, remainder = 0, relative_value = 0, relative_derivative = 0;
  unsigned selected_j = 0, selected_n = 0;
  for (unsigned j = 1; j <= 16 && selected_j == 0; ++j) {
    epsilon_power *= epsilon;
    const R log_remainder = epsilon_power/(R(j+1)*(1-epsilon));
    const R exponent_error = log_remainder*(abs(a)+b*(2*ell+log_remainder));
    const R bound = c+exponent_error;
    const R exp_bound = counter.function([&] { return R(exp(bound)); });
    R power = bound*bound, factorial = 2;
    for (unsigned n = 1; n <= 16; ++n) {
      counter.selection();
      const R candidate = exp_bound*(exponent_error+power/factorial);
      const R rv = candidate*exp_c;
      const R rd = rv*(1+epsilon)/(1-epsilon);
      if (finite(candidate) && candidate > 0 && rv <= tau && rd <= tau) {
        selected_j = j;
        selected_n = n;
        remainder = candidate;
        relative_value = rv;
        relative_derivative = rd;
        break;
      }
      power *= bound;
      factorial *= R(n+2);
    }
  }
  if (selected_j == 0) return original(LocalWeightReason::OrderCap);
  work.route = LocalWeightRoute::LocalSeries;
  work.log_order = selected_j;
  work.exponential_order = selected_n;
  // Conservative owned workspace reservation includes fixed scalar/short-array
  // temporaries in this route, beyond the four large coefficient buffers.
  constexpr std::size_t workspace_bytes = sizeof(LocalWorkspace<R>)+256*sizeof(R);
  static_assert(workspace_bytes <= 1048576);
  if (workspace_bytes > budget.max_workspace_bytes)
    return Err(ErrorCode::Unavailable, "local cubic workspace budget");
  work.workspace_bytes = workspace_bytes;
  auto workspace = std::make_unique<LocalWorkspace<R>>();
  auto raw = raw_weight_moments<Digits>(request, budget.moments, work.moments);
  if (!raw) return Err(raw.error());
  const R exponent = -rate*dt-z*z/2;
  if (!finite(exponent) || abs(exponent) > 32768)
    return Err(ErrorCode::OutOfRange, "local cubic prefactor domain");
  const R numerator = counter.function([&] { return R(exp(exponent)); });
  const R root_two_pi = counter.function([&] {
    return R(sqrt(2*boost::math::constants::pi<R>()));
  });
  const R prefactor = numerator*h/(sd*center*root_two_pi);
  if (!finite(prefactor) || !(prefactor > 0))
    return Err(ErrorCode::OutOfRange, "local cubic prefactor arithmetic");

  std::array<R, 17> logarithm{};
  R coefficient_power = 1;
  for (unsigned k = 1; k <= selected_j; ++k) {
    coefficient_power = counter.product(coefficient_power, epsilon);
    logarithm[k] = (k % 2 ? coefficient_power : -coefficient_power)/R(k);
    workspace->exponent[k] = counter.product(R(-a), logarithm[k]);
  }
  for (unsigned i = 1; i <= selected_j; ++i)
    for (unsigned j = 1; j <= selected_j; ++j)
      workspace->exponent[i+j] -= counter.product(b,
          counter.product(logarithm[i], logarithm[j]));
  workspace->term[0] = 1;
  workspace->sum[0] = 1;
  const unsigned exponent_degree = 2*selected_j;
  for (unsigned n = 1; n <= selected_n; ++n) {
    workspace->next.fill(R(0));
    const unsigned old_degree = exponent_degree*(n-1);
    for (unsigned i = 0; i <= old_degree; ++i)
      for (unsigned j = 0; j <= exponent_degree; ++j)
        workspace->next[i+j] += counter.product(workspace->term[i], workspace->exponent[j]);
    for (unsigned k = 0; k <= exponent_degree*n; ++k) {
      workspace->next[k] /= R(n);
      workspace->sum[k] += workspace->next[k];
    }
    workspace->term.swap(workspace->next);
  }
  const unsigned degree = exponent_degree*selected_n;
  R maximum_condition = 1;
  const auto integrate = [&](const std::array<R, 514> &coefficients, unsigned count,
                             unsigned basis_degree, unsigned j) -> R {
    R value = 0, absolute_sum = 0;
    for (unsigned k = 0; k <= count; ++k) {
      const auto rational = bernstein_integral(basis_degree, j, k);
      if (!rational) throw LocalNumericFailure{"local cubic rational integral"};
      const R integral = R(rational->numerator)/R(rational->denominator);
      const R term = counter.product(coefficients[k], integral);
      value += term;
      absolute_sum += abs(term);
    }
    if (!finite(value) || !(value > 0))
      throw LocalNumericFailure{"local cubic integral arithmetic"};
    const R condition = absolute_sum/value;
    if (!finite(condition) || condition > 4)
      throw LocalNumericFailure{"local cubic integral conditioning"};
    if (condition > maximum_condition) maximum_condition = condition;
    return value;
  };
  std::array<R, 4> weights{};
  std::array<R, 3> derivative{};
  for (unsigned j = 0; j < 4; ++j)
    weights[j] = counter.product(prefactor, integrate(workspace->sum, degree, 3, j));
  workspace->term.fill(R(0));
  for (unsigned k = 0; k <= degree; ++k) {
    workspace->term[k] += workspace->sum[k];
    workspace->term[k+1] += counter.product(epsilon, workspace->sum[k]);
  }
  const R derivative_prefactor = prefactor*(center/stock);
  for (unsigned j = 0; j < 3; ++j)
    derivative[j] = counter.product(derivative_prefactor,
        integrate(workspace->term, degree+1, 2, j));
  R weight_sum = 0, derivative_sum = 0;
  for (const auto &w : weights) weight_sum += w;
  for (const auto &d : derivative) derivative_sum += d;
  const R stock_ratio = (raw->upper/raw->stock)*raw->moments[1];
  if (abs(weight_sum-raw->moments[0]) > R("1e-25")*raw->moments[0] ||
      abs(derivative_sum-stock_ratio) > R("1e-25")*stock_ratio)
    return Err(ErrorCode::OutOfRange, "local cubic conservation");
  const auto encode = [](const R &value) { return value.str(Digits, std::ios_base::scientific); };
  const R value_bound = prefactor*remainder/4;
  const R derivative_bound = derivative_prefactor*(1+epsilon)*remainder/3;
  for (const auto &w : weights)
    if (!finite(w) || !(w > 0))
      return Err(ErrorCode::OutOfRange, "local cubic value arithmetic");
  for (const auto &d : derivative)
    if (!finite(d) || !(d > 0))
      return Err(ErrorCode::OutOfRange, "local cubic derivative arithmetic");
  if (!finite(value_bound) || !(value_bound > 0) ||
      !finite(derivative_bound) || !(derivative_bound > 0))
    return Err(ErrorCode::OutOfRange, "local cubic remainder arithmetic");
  LocalCubicWeights out;
  for (unsigned k = 0; k < 4; ++k) {
    out.weights.value[k] = encode(weights[k]);
    out.weights.scaled_moments[k] = encode(raw->moments[k]);
    out.value_remainders[k] = encode(value_bound);
  }
  for (unsigned k = 0; k < 3; ++k) {
    out.weights.derivative[k] = encode(derivative[k]);
    out.derivative_remainders[k] = encode(derivative_bound);
  }
  out.weights.mass = encode(raw->moments[0]);
  out.weights.discounted_stock_ratio = encode(stock_ratio);
  out.weights.maximum_condition = encode(maximum_condition);
  out.density_remainder = encode(remainder);
  out.relative_value_remainder = encode(relative_value);
  out.relative_derivative_remainder = encode(relative_derivative);
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

Result<BernsteinMonomialIntegral> local_bernstein_integral(unsigned degree, unsigned j,
                                                        unsigned power) {
  return bernstein_integral(degree, j, power);
}

Result<LocalCubicWeights> cubic_local_weight_control(const WeightRequest &request,
    WeightPrecision precision, const LocalWeightBudget &budget, LocalWeightWork &work) {
  work = {};
  if (!valid(request)) return Err(ErrorCode::InvalidArgument, "local cubic Gaussian input");
  try {
    switch (precision) {
      case WeightPrecision::Decimal100: return calculate_local<100>(request, budget, work);
      case WeightPrecision::Decimal200: return calculate_local<200>(request, budget, work);
    }
    return Err(ErrorCode::InvalidArgument, "local cubic precision");
  } catch (const LocalWorkLimit &error) {
    return Err(ErrorCode::Unavailable, error.message);
  } catch (const LocalNumericFailure &error) {
    return Err(ErrorCode::OutOfRange, error.message);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "local cubic allocation");
  } catch (const std::exception &) {
    return Err(ErrorCode::OutOfRange, "local cubic special-function arithmetic");
  }
}
} // namespace atx::vol::cash_gaussian
