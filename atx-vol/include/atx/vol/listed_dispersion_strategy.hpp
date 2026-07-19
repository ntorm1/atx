#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
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
// performed here. Current schedules are cold-authored and carry no pricing-route
// metadata, so fast schedules require a future versioned route field plus an
// explicit economic-error gate before they can safely relax ColdReference.
class ListedDispersionStrategy final : public IStrategy {
public:
  [[nodiscard]] static Result<ListedDispersionStrategy> create(ListedDispersionSchedule schedule,
                                                               double delta_band = 0.0);

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override;
  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id, const PriceOptions &price_options) override;
  [[nodiscard]] std::span<const FullGreekSeed> entry_risk_seeds() const noexcept override {
    return last_entry_seeds_;
  }
  [[nodiscard]] QueryExecution required_economic_execution() const noexcept override {
    return QueryExecution::ColdReference;
  }

  [[nodiscard]] HedgeSpec hedge_spec() const override { return hedge_; }
  [[nodiscard]] const ListedDispersionSchedule &schedule() const noexcept { return schedule_; }
  [[nodiscard]] bool all_rolls_consumed() const noexcept {
    return next_roll_ == schedule_.rolls.size();
  }
  [[nodiscard]] std::size_t next_roll_index() const noexcept { return next_roll_; }

  // Relative tolerance for the entry-mark cross-check against the loaded archive.
  // Defaults to the SHARED kListedEntryMarkTolerance so this guard and the
  // reconciliation guard agree by construction; 0.0 restores a bit-exact compare.
  [[nodiscard]] double entry_mark_tolerance() const noexcept { return entry_mark_tolerance_; }
  void set_entry_mark_tolerance(double tol) noexcept { entry_mark_tolerance_ = tol; }

private:
  ListedDispersionStrategy(ListedDispersionSchedule schedule, HedgeSpec hedge) noexcept
      : schedule_{std::move(schedule)}, hedge_{hedge} {}

  ListedDispersionSchedule schedule_{};
  HedgeSpec hedge_{};
  std::size_t next_roll_{0};
  double entry_mark_tolerance_{kListedEntryMarkTolerance};
  std::vector<FullGreekSeed> last_entry_seeds_{};
};

} // namespace atx::vol
