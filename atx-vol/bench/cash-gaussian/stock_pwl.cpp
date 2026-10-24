#include "stock_pwl.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace atx::vol::cash_gaussian {
namespace {
using atx::core::Err;
using atx::core::Ok;

Status node_room(std::size_t count, const Budget &budget) {
  if (budget.limits.max_knots == 0) return Err(ErrorCode::InvalidArgument, "PWL limits");
  if (count > budget.limits.max_knots ||
      budget.created_knots > budget.limits.max_created_knots ||
      count > budget.limits.max_created_knots - budget.created_knots) {
    return Err(ErrorCode::Unavailable, "PWL node budget");
  }
  return Ok();
}

// Same-tail erfc differences avoid subtracting two CDF values near one. Across
// zero, erf differences retain small intervals without 1 - .5 - .5 cancellation.
double normal_mass(double lo, double hi) noexcept {
  constexpr double inv_sqrt_two = 0.707106781186547524400844362104849039;
  if (lo >= 0.) return .5 * (std::erfc(lo * inv_sqrt_two) - std::erfc(hi * inv_sqrt_two));
  if (hi <= 0.) return .5 * (std::erfc(-hi * inv_sqrt_two) - std::erfc(-lo * inv_sqrt_two));
  return .5 * (std::erf(hi * inv_sqrt_two) - std::erf(lo * inv_sqrt_two));
}

bool finite_nonnegative(double value) noexcept {
  return std::isfinite(value) && value >= 0.;
}
} // namespace

Result<StockPwl> StockPwl::create(std::span<const double> stock, std::span<const double> value,
                                 double right_slope, Budget &budget) {
  if (stock.empty() || stock.size() != value.size() || stock.front() != 0. ||
      !std::isfinite(right_slope)) return Err(ErrorCode::InvalidArgument, "PWL input");
  if (const auto room = node_room(stock.size(), budget); !room) return Err(room.error());
  for (std::size_t i = 0; i < stock.size(); ++i) {
    if (!finite_nonnegative(stock[i]) || !std::isfinite(value[i]) ||
        (i != 0 && !(stock[i] > stock[i-1]))) {
      return Err(ErrorCode::InvalidArgument, "PWL knots");
    }
  }
  try {
    StockPwl out;
    out.stock_.assign(stock.begin(), stock.end());
    out.value_.assign(value.begin(), value.end());
    out.right_slope_ = right_slope;
    out.slope_.resize(stock.size());
    out.intercept_.resize(stock.size());
    out.log_stock_.resize(stock.size());
    for (std::size_t i = 0; i < stock.size(); ++i) {
      const double slope = i + 1 == stock.size() ? right_slope :
          (value[i+1] - value[i]) / (stock[i+1] - stock[i]);
      const double intercept = std::fma(-slope, stock[i], value[i]);
      if (!std::isfinite(slope) || !std::isfinite(intercept))
        return Err(ErrorCode::OutOfRange, "PWL arithmetic");
      out.slope_[i] = slope;
      out.intercept_[i] = intercept;
      out.log_stock_[i] = stock[i] == 0. ? -std::numeric_limits<double>::infinity() :
          std::log(stock[i]);
    }
    budget.created_knots += stock.size();
    return out;
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "PWL allocation");
  }
}

Result<double> StockPwl::value(double stock) const noexcept {
  if (!populated() || !finite_nonnegative(stock))
    return Err(ErrorCode::InvalidArgument, "PWL evaluation");
  const auto upper = std::upper_bound(stock_.begin(), stock_.end(), stock);
  const auto index = static_cast<std::size_t>(upper - stock_.begin() - 1);
  const double result = std::fma(slope_[index], stock - stock_[index], value_[index]);
  if (!std::isfinite(result)) return Err(ErrorCode::OutOfRange, "PWL evaluation");
  return result;
}

Result<double> expectation(const StockPwl &f, double stock, double dt, double sigma,
                           double rate, double yield, Budget &budget) {
  if (!f.populated() || !finite_nonnegative(stock) || !finite_nonnegative(dt) ||
      !finite_nonnegative(sigma) || !std::isfinite(rate) || !std::isfinite(yield)) {
    return Err(ErrorCode::InvalidArgument, "GBM input");
  }
  if (budget.expectations >= budget.limits.max_expectations)
    return Err(ErrorCode::Unavailable, "GBM query budget");
  const bool gaussian = stock > 0. && dt > 0. && sigma > 0.;
  if (gaussian && (budget.interval_evaluations > budget.limits.max_interval_evaluations ||
      f.stock_.size() > budget.limits.max_interval_evaluations - budget.interval_evaluations)) {
    return Err(ErrorCode::Unavailable, "GBM work budget");
  }
  ++budget.expectations;
  const double discount = std::exp(-rate * dt);
  if (!std::isfinite(discount) || !(discount > 0.))
    return Err(ErrorCode::OutOfRange, "GBM discount");
  if (stock == 0.) {
    ++budget.zero_state_evaluations;
    const double result = discount * f.value_.front();
    if (!std::isfinite(result)) return Err(ErrorCode::OutOfRange, "GBM zero state");
    return result;
  }
  if (!gaussian) {
    ++budget.deterministic_evaluations;
    if (dt == 0.) return f.value(stock);
    const double growth = std::exp((rate - yield) * dt);
    const double terminal = stock * growth;
    if (!std::isfinite(terminal) || !(terminal > 0.))
      return Err(ErrorCode::OutOfRange, "GBM transition");
    const auto value = f.value(terminal);
    if (!value) return Err(value.error());
    const double result = discount * *value;
    if (!std::isfinite(result)) return Err(ErrorCode::OutOfRange, "GBM value");
    return result;
  }
  const double sd = sigma * std::sqrt(dt), variance = sd * sd;
  const double log_mean = std::log(stock) + (rate - yield) * dt;
  const double location = log_mean - .5 * variance;
  const double discounted_mean = std::exp(std::log(stock) - yield * dt);
  if (!(sd > 0.) || !std::isfinite(variance) || !std::isfinite(location) ||
      !std::isfinite(discounted_mean) || !(discounted_mean > 0.)) {
    return Err(ErrorCode::OutOfRange, "GBM transition");
  }
  double sum = 0., correction = 0.;
  for (std::size_t i = 0; i < f.stock_.size(); ++i) {
    ++budget.interval_evaluations;
    const double lo = (f.log_stock_[i] - location) / sd;
    const double hi = i + 1 == f.stock_.size() ? std::numeric_limits<double>::infinity() :
        (f.log_stock_[i+1] - location) / sd;
    const double mass = normal_mass(lo, hi), moment = normal_mass(lo-sd, hi-sd);
    const double term = std::fma(f.intercept_[i], discount * mass,
                                f.slope_[i] * (discounted_mean * moment));
    if (!finite_nonnegative(mass) || !finite_nonnegative(moment) || !std::isfinite(term))
      return Err(ErrorCode::OutOfRange, "GBM interval");
    const double adjusted = term - correction;
    const double next = sum + adjusted;
    correction = (next - sum) - adjusted;
    sum = next;
  }
  if (!std::isfinite(sum)) return Err(ErrorCode::OutOfRange, "GBM sum");
  return sum;
}

Result<StockPwl> liquidator_jump(const StockPwl &after, double cash, Budget &budget) {
  if (after.knots().empty() || !finite_nonnegative(cash))
    return Err(ErrorCode::InvalidArgument, "cash jump input");
  const auto extra = cash > 0. ? 1u : 0u;
  if (after.knots().size() > std::numeric_limits<std::size_t>::max() - extra)
    return Err(ErrorCode::Unavailable, "cash node budget");
  const auto count = after.knots().size() + extra;
  if (const auto room = node_room(count, budget); !room) return Err(room.error());
  try {
    std::vector<double> stock, values;
    stock.reserve(count);
    values.reserve(count);
    if (cash > 0.) { stock.push_back(0.); values.push_back(after.values().front()); }
    for (std::size_t i = 0; i < after.knots().size(); ++i) {
      const double shifted = after.knots()[i] + cash;
      if (!std::isfinite(shifted) || (!stock.empty() && !(shifted > stock.back())))
        return Err(ErrorCode::OutOfRange, "cash knot range");
      stock.push_back(shifted);
      values.push_back(after.values()[i]);
    }
    return StockPwl::create(stock, values, after.right_slope(), budget);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "cash allocation");
  }
}
} // namespace atx::vol::cash_gaussian
