#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/backtest.hpp"                   // Lot, MarketSnapshot, PortfolioState
#include "atx/vol/listed_dispersion_schedule.hpp" // ListedDispersionSchedule
#include "atx/vol/strategy.hpp"                   // IStrategy, HedgeSpec
#include "atx/vol/types.hpp"                      // Result, Status, Side

namespace atx::vol {

// How schedule replay reconciles each leg's live-surface mark against the frozen
// `leg.model_mark`.
//   ExactArchive: current behavior — a bit-exact gate against `leg.model_mark`,
//     replaying the frozen archive; any divergence fails the step.
//   Record: no gate — accept the live seed mark, for repricing the same frozen
//     definitions through a different query route (e.g. an interpolated
//     `QueryExecution::Configured`), recording divergence instead of failing.
enum class ScheduleMarkPolicy { ExactArchive, Record };

// One leg whose live-surface mark diverged from its frozen schedule mark during a
// Record-policy replay step. `schedule_mark` is the frozen `leg.model_mark`;
// `live_mark` is the seed price re-derived from the surface this step.
struct MarkDivergence {
  std::uint32_t uid{0};
  double strike{0.0};
  std::int64_t expiry_ts_ns{0};
  Side side{Side::Call};
  double schedule_mark{0.0};
  double live_mark{0.0};
};

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
  [[nodiscard]] static Result<ListedDispersionStrategy>
  create(ListedDispersionSchedule schedule, double delta_band = 0.0,
         ScheduleMarkPolicy policy = ScheduleMarkPolicy::ExactArchive);

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override;
  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id, const PriceOptions &price_options) override;
  [[nodiscard]] std::span<const FullGreekSeed> entry_risk_seeds() const noexcept override {
    return last_entry_seeds_;
  }
  // Under ScheduleMarkPolicy::Record, the legs whose live mark diverged from the
  // frozen schedule mark on the most recent step (cleared every step; always
  // empty under ExactArchive, which fails the step on the first divergence).
  [[nodiscard]] const std::vector<MarkDivergence> &last_mark_divergences() const noexcept {
    return last_mark_divergences_;
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

private:
  ListedDispersionStrategy(ListedDispersionSchedule schedule, HedgeSpec hedge,
                           ScheduleMarkPolicy policy) noexcept
      : schedule_{std::move(schedule)}, hedge_{hedge}, policy_{policy} {}

  ListedDispersionSchedule schedule_{};
  HedgeSpec hedge_{};
  ScheduleMarkPolicy policy_{ScheduleMarkPolicy::ExactArchive};
  std::size_t next_roll_{0};
  std::vector<FullGreekSeed> last_entry_seeds_{};
  std::vector<MarkDivergence> last_mark_divergences_{};
};

} // namespace atx::vol
