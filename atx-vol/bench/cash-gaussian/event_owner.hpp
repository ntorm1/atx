#pragma once

#include "stock_pwl.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace atx::vol::cash_gaussian {

enum class CashModel { Liquidator, Escrowed, Survivor };
struct Event { double time = 0., cash = 0.; };
struct Model {
  double spot = 100., strike = 100., maturity = 1., sigma = .2;
  double rate = .04, yield = 0.;
  Side side = Side::Put;
  ExerciseStyle exercise = ExerciseStyle::American;
  CashModel cash_model = CashModel::Liquidator;
};
struct Options {
  std::size_t positive_intervals = 256;
  std::size_t time_base = 128;
  unsigned time_level = 0;
  std::size_t max_events = 64, max_steps = 4096;
  Limits limits{8192, 1U << 24, 1U << 28, 1U << 20};
};
struct Step { double from = 0., to = 0., cash_at_from = 0.; };
struct Work {
  Budget integral{};
  std::size_t planned_steps = 0, completed_steps = 0, grid_nodes = 0;
  std::size_t jumps = 0, exercise_projections = 0, intersections = 0;
  std::size_t rounded_intersections = 0, deterministic_rollbacks = 0;
};

// Borrowed only during the synchronous callback. A diagnostic must copy any
// spans it retains; its callback cannot throw or modify the source function.
struct RoundedCrossing {
  std::span<const double> stock, value;
  double right_slope = 0., strike = 0.;
  Side side = Side::Put;
  bool tail = false;
  double stock_lo = 0., stock_hi = 0., difference_lo = 0., difference_hi = 0.;
  double slope_difference = 0., computed_stock = 0.;
};
struct CrossingObserver {
  using Callback = void (*)(const RoundedCrossing &, void *) noexcept;
  Callback callback = nullptr;
  void *context = nullptr;
};

// These private experiment operators own their output and retain all completed
// construction/work counts on refusal. They do not call the PDE implementation.
[[nodiscard]] Result<StockPwl> exercise_max(const StockPwl &, double strike, Side,
                                           Work &, const CrossingObserver * = nullptr);
[[nodiscard]] Result<StockPwl> deterministic_rollback(const StockPwl &, double dt,
    double rate, double yield, Work &);

// Owns the validated raw events, normalized events, grid and descending clock.
// No borrowed caller storage or reference prices. Concurrent const price calls
// are safe with distinct Work objects. price resets its caller-owned diagnostics.
class EventOwner {
 public:
  [[nodiscard]] static Result<EventOwner> prepare(const Model &, std::span<const Event>,
                                                  const Options & = {});
  [[nodiscard]] Result<double> price(Work &, const CrossingObserver * = nullptr) const;
  [[nodiscard]] const Model &model() const noexcept { return model_; }
  [[nodiscard]] const Options &options() const noexcept { return options_; }
  [[nodiscard]] std::span<const Event> raw_events() const noexcept { return raw_; }
  [[nodiscard]] std::span<const Event> events() const noexcept { return events_; }
  [[nodiscard]] std::span<const Step> steps() const noexcept { return steps_; }
  [[nodiscard]] std::span<const double> stock_grid() const noexcept { return stock_; }
 private:
  bool prepared_ = false;
  Model model_{};
  Options options_{};
  std::vector<Event> raw_, events_;
  std::vector<Step> steps_;
  std::vector<double> stock_;
};
} // namespace atx::vol::cash_gaussian
