#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp"                   // Lot, MarketSnapshot, PortfolioState
#include "atx/vol/listed_dispersion_schedule.hpp" // ListedDispersionSchedule
#include "atx/vol/strategy.hpp"                   // IStrategy, HedgeSpec
#include "atx/vol/types.hpp"                      // Result, Status

namespace atx::vol {

// Convert one validated roll to absolute-expiry lots without mutating strategy
// state. `first_lot_id` is applied in schedule order.
[[nodiscard]] Result<std::vector<Lot>>
materialize_listed_dispersion_roll(const ListedScheduleRoll &roll, std::int64_t valuation_ts_ns,
                                   std::uint64_t first_lot_id);

// Strategy adapter over an immutable, externally persisted listed-contract
// schedule. Roll dates/marks are validated against each loaded archive before
// the existing cohort is replaced; no signal calculation or daily restriking is
// performed here.
class ListedDispersionStrategy final : public IStrategy {
public:
  [[nodiscard]] static Result<ListedDispersionStrategy> create(ListedDispersionSchedule schedule,
                                                               double delta_band = 0.0);

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override;

  [[nodiscard]] HedgeSpec hedge_spec() const override { return hedge_; }
  [[nodiscard]] const ListedDispersionSchedule &schedule() const noexcept { return schedule_; }
  [[nodiscard]] bool all_rolls_consumed() const noexcept {
    return next_roll_ == schedule_.rolls.size();
  }
  [[nodiscard]] std::size_t next_roll_index() const noexcept { return next_roll_; }

private:
  ListedDispersionStrategy(ListedDispersionSchedule schedule, HedgeSpec hedge) noexcept
      : schedule_{std::move(schedule)}, hedge_{hedge} {}

  ListedDispersionSchedule schedule_{};
  HedgeSpec hedge_{};
  std::size_t next_roll_{0};
};

} // namespace atx::vol
