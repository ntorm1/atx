#pragma once

// Private independent reference experiment. No production dispatch or PDE helpers.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol::cash_gaussian {

struct Limits {
  std::size_t max_knots = 4096;
  std::uint64_t max_created_knots = 1U << 24;
  std::uint64_t max_interval_evaluations = 1U << 28;
  std::uint64_t max_expectations = 1U << 20;
};

// Caller-owned for one complete experiment. Completed work remains counted on
// numerical failure; an exhausted limit never yields a partial price.
// Do not mutate a live budget concurrently. There is no global or retained cache.
struct Budget {
  Limits limits{};
  std::uint64_t created_knots = 0;
  std::uint64_t interval_evaluations = 0;
  std::uint64_t expectations = 0;
  std::uint64_t deterministic_evaluations = 0;
  std::uint64_t zero_state_evaluations = 0;
};

class StockPwl {
 public:
  // Continuous stock-price function on [0,infinity). The first knot is exactly
  // zero; knots strictly increase. The final value extends with right_slope.
  // All finite slopes are admitted; liquidator puts need not be globally convex.
  [[nodiscard]] static Result<StockPwl> create(std::span<const double> stock,
      std::span<const double> value, double right_slope, Budget &budget);
  [[nodiscard]] Result<double> value(double stock) const noexcept;
  [[nodiscard]] std::span<const double> knots() const noexcept { return stock_; }
  [[nodiscard]] std::span<const double> values() const noexcept { return value_; }
  [[nodiscard]] double right_slope() const noexcept { return right_slope_; }

 private:
  [[nodiscard]] bool populated() const noexcept {
    const auto n = stock_.size();
    return n > 0 && value_.size() == n && slope_.size() == n &&
        intercept_.size() == n && log_stock_.size() == n;
  }
  friend Result<double> expectation(const StockPwl &, double, double, double,
                                    double, double, Budget &);
  std::vector<double> stock_, value_, slope_, intercept_, log_stock_;
  double right_slope_ = 0.0;
};

// Discounted expectation under a GBM interval. Every finite interval and the
// infinite tail contribute; no probability/moneyness cutoff. Handles S=0,
// dt=0 and sigma=0 explicitly. Rates/yields may be negative. Finite arithmetic
// overflow/underflow that destroys the transition is a typed OutOfRange.
[[nodiscard]] Result<double> expectation(const StockPwl &function, double stock,
    double dt, double sigma, double rate, double yield, Budget &budget);

// Exact composition f(max(S-cash,0)) in stock-price PWL algebra. Independent of
// reference-PDE jump_at, interpolation and event adapters. No exercise inserted.
[[nodiscard]] Result<StockPwl> liquidator_jump(const StockPwl &after, double cash,
                                             Budget &budget);

} // namespace atx::vol::cash_gaussian
