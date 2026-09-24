// Financing belongs to the elapsed interval held, using its opening marks.
// Continuous trade-date research convention; broker settlement is not modeled.

#include <array>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/datetime.hpp"
#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/cost/borrow.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/data/market.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/backtest_loop.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/rolling_panel.hpp"
#include "atx/engine/loop/signal_source.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/portfolio/portfolio.hpp"

namespace atxtest_backtest_borrow_test {

using atx::f64;
using atx::i64;
using atx::usize;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::time::Duration;
using atx::core::time::Timestamp;
using atx::engine::BacktestLoop;
using atx::engine::BacktestResult;
using atx::engine::Delay;
using atx::engine::EventBus;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::Portfolio;
using atx::engine::RollingPanel;
using atx::engine::Schedule;
using atx::engine::ScriptedSignalSource;
using atx::engine::SimClock;
using atx::engine::Transform;
using atx::engine::Universe;
using atx::engine::WeightPolicy;
using atx::engine::cost::BorrowModel;
using atx::engine::cost::DayCount;
using atx::engine::data::BarRow;
using atx::engine::data::IDataHandler;
using atx::engine::data::InMemoryBarFeed;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;

using Bus = EventBus<>;
constexpr usize kCap = 8;
constexpr i64 kDay = Duration::kNsPerDay;
constexpr i64 kHour = Duration::kNsPerHour;
const InstrumentId kA{1};
constexpr BorrowModel kBorrow{0.05, DayCount::D360};
constexpr f64 kDailyCharge = 100'000.0 * 0.05 / 360.0;

[[nodiscard]] BarRow bar_row(i64 nanos, i64 price) {
  Bar bar{};
  bar.ts = Timestamp::from_unix_nanos(nanos);
  bar.open = Price::from_int(price);
  bar.high = Price::from_int(price);
  bar.low = Price::from_int(price);
  bar.close = Price::from_int(price);
  bar.volume = Quantity::from_int(1'000'000);
  return BarRow{kA, bar, bar.ts, false};
}

// Duplicate slices are delivered separately. Resetting the clock permits a
// malformed backwards feed to reach the loop's guard in Debug, where the usual
// SimClock::advance_to would otherwise abort before the loop receives it.
class BoundaryFeed final : public IDataHandler {
public:
  BoundaryFeed(std::span<const BarRow> rows, SimClock &clock, Bus &bus)
      : rows_{rows}, clock_{&clock}, bus_{&bus} {}

  [[nodiscard]] bool step() override {
    if (next_ == rows_.size()) {
      return false;
    }
    const BarRow &row = rows_[next_++];
    *clock_ = SimClock{};
    clock_->advance_to(row.knowledge_ts);
    i64 sequence = 0;
    auto &slot = bus_->claim_slot(sequence);
    slot = atx::engine::data::make_market_bar(row.symbol, row.bar, row.knowledge_ts);
    bus_->publish(sequence);
    return true;
  }

private:
  std::span<const BarRow> rows_;
  SimClock *clock_;
  Bus *bus_;
  usize next_ = 0;
};

struct Outcome {
  BacktestResult result;
  i64 qty = 0;
};

[[nodiscard]] Outcome run_book(const std::vector<i64> &timestamps,
                               const std::vector<f64> &signals, BorrowModel borrow = {},
                               Delay delay = Delay::Next, const std::vector<i64> &prices = {},
                               bool boundary_feed = false) {
  const std::array<InstrumentId, 1> universe{kA};
  std::vector<BarRow> rows;
  for (usize index = 0; index < timestamps.size(); ++index) {
    rows.push_back(bar_row(timestamps[index], prices.empty() ? 100 : prices.at(index)));
  }
  const std::array<std::span<const BarRow>, 1> sources{std::span<const BarRow>{rows}};
  auto bus = std::make_unique<Bus>();
  SimClock clock;
  std::unique_ptr<IDataHandler> feed;
  if (boundary_feed) {
    feed = std::make_unique<BoundaryFeed>(rows, clock, *bus);
  } else {
    feed = std::make_unique<InMemoryBarFeed>(sources, clock, *bus);
  }
  RollingPanel<kCap> panel{universe, 1};
  std::vector<std::vector<f64>> signal_rows;
  for (const f64 value : signals) {
    signal_rows.push_back({value});
  }
  ScriptedSignalSource signal{signal_rows, 1, 1};
  WeightPolicy policy{};
  policy.transform = Transform::Raw;
  policy.dollar_neutral = false;
  policy.winsorize_limit = 0.0;
  ExecutionSimulator sim{
      FillCfg{}, SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
      ImpactCfg{0.0, 0.5, 0.0},
      CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
      LatencyCfg{}, VolumeCapCfg{1.0}};
  Portfolio portfolio{Decimal::from_int(100'000), universe};
  Market market{universe, std::span<const InstrumentStats>{}};
  BacktestLoop<kCap> loop{*feed, clock, *bus, panel, signal, policy, sim, portfolio,
                          market, Universe{universe}, Schedule{1}, delay, borrow};
  Outcome outcome;
  outcome.result = loop.run();
  outcome.qty = portfolio.holding(kA).qty;
  return outcome;
}

TEST(BacktestBorrow, OpeningAtFinalClose_HasNoElapsedCharge) {
  const Outcome outcome = run_book({kDay, 2 * kDay}, {-1.0}, kBorrow);
  ASSERT_EQ(outcome.result.fills.size(), 1U);
  EXPECT_EQ(outcome.qty, -1000);
  EXPECT_EQ(outcome.result.final_cash, Decimal::from_int(200'000));
  EXPECT_DOUBLE_EQ(outcome.result.final_equity, 100'000.0);
}

TEST(BacktestBorrow, ShortBook_OneElapsedDay_DebitsExactCharge) {
  const Outcome outcome = run_book({kDay, 2 * kDay, 3 * kDay}, {-1.0}, kBorrow);
  const Decimal fee = Decimal::from_double(kDailyCharge).value();
  EXPECT_EQ(outcome.result.final_cash, Decimal::from_int(200'000) - fee);
}

TEST(BacktestBorrow, FridayToMonday_AccruesThreeCalendarDays) {
  const Outcome outcome = run_book({kDay, 2 * kDay, 5 * kDay}, {-1.0}, kBorrow);
  EXPECT_NEAR(outcome.result.final_equity, 100'000.0 - 3.0 * kDailyCharge, 1e-8);
}

TEST(BacktestBorrow, CoverAtMondayClose_StillPaysWeekendHeld) {
  const Outcome outcome = run_book({kDay, 2 * kDay, 5 * kDay}, {-1.0, 0.0}, kBorrow);
  EXPECT_EQ(outcome.qty, 0);
  ASSERT_EQ(outcome.result.fills.size(), 2U);
  EXPECT_EQ(outcome.result.fills.back().qty, 1000);
  EXPECT_NEAR(outcome.result.final_cash.to_double(), 100'000.0 - 3.0 * kDailyCharge, 1e-8);
}

TEST(BacktestBorrow, FlipToLong_ChargesPriorShortIntervalOnly) {
  const Outcome outcome =
      run_book({kDay, 2 * kDay, 5 * kDay, 6 * kDay}, {-1.0, 1.0}, kBorrow);
  EXPECT_EQ(outcome.qty, 1000);
  ASSERT_EQ(outcome.result.fills.size(), 2U);
  EXPECT_EQ(outcome.result.fills.back().qty, 2000);
  EXPECT_NEAR(outcome.result.final_equity, 100'000.0 - 3.0 * kDailyCharge, 1e-8);
}

TEST(BacktestBorrow, NewClose_DoesNotRepriceThePriorHoldingInterval) {
  const Outcome outcome =
      run_book({kDay, 2 * kDay, 3 * kDay}, {-1.0}, kBorrow, Delay::Next, {100, 100, 200});
  EXPECT_NEAR(outcome.result.final_cash.to_double(), 200'000.0 - kDailyCharge, 1e-8);
}

TEST(BacktestBorrow, IntradayPartitioning_PreservesElapsedCharge) {
  const Outcome hourly = run_book({0, kHour}, {-1.0}, kBorrow, Delay::Same);
  std::vector<i64> minutes;
  for (i64 minute = 0; minute <= 60; ++minute) {
    minutes.push_back(minute * Duration::kNsPerMin);
  }
  const Outcome minutely = run_book(minutes, {-1.0}, kBorrow, Delay::Same);
  EXPECT_NEAR(hourly.result.final_equity, 100'000.0 - kDailyCharge / 24.0, 1e-8);
  EXPECT_NEAR(minutely.result.final_equity, hourly.result.final_equity, 60e-9);
}

TEST(BacktestBorrow, RepeatedTimestamp_DoesNotDoubleAccrue) {
  const Outcome outcome = run_book({0, 0, kDay}, {-1.0}, kBorrow, Delay::Same, {}, true);
  ASSERT_EQ(outcome.result.equity_curve.size(), 3U);
  EXPECT_DOUBLE_EQ(outcome.result.equity_curve[0].equity, 100'000.0);
  EXPECT_DOUBLE_EQ(outcome.result.equity_curve[1].equity, 100'000.0);
  EXPECT_NEAR(outcome.result.final_equity, 100'000.0 - kDailyCharge, 1e-8);
}

TEST(BacktestBorrow, BackwardsTimestamp_ThrowsEvenAtZeroRate) {
  EXPECT_THROW((void)run_book({2 * kDay, kDay}, {}, {}, Delay::Next, {}, true),
               std::invalid_argument);
}

TEST(BacktestBorrow, InvalidModel_ThrowsBeforeAnEmptyRun) {
  for (const f64 rate : {-0.01, std::numeric_limits<f64>::quiet_NaN(),
                         std::numeric_limits<f64>::infinity()}) {
    EXPECT_THROW((void)run_book({}, {}, BorrowModel{rate, DayCount::D360}),
                 std::invalid_argument);
  }
  EXPECT_THROW((void)run_book({}, {}, BorrowModel{0.0, DayCount::D252}), std::invalid_argument);
}

TEST(BacktestBorrow, LongOnlyBook_PaysZeroBorrow) {
  const Outcome outcome = run_book({kDay, 2 * kDay, 5 * kDay}, {1.0}, kBorrow);
  EXPECT_DOUBLE_EQ(outcome.result.final_equity, 100'000.0);
}

TEST(BacktestBorrow, ZeroRate_ByteIdenticalToDefault) {
  const Outcome implicit = run_book({kDay, 2 * kDay, 5 * kDay}, {-1.0});
  const Outcome explicit_zero =
      run_book({kDay, 2 * kDay, 5 * kDay}, {-1.0}, BorrowModel{0.0, DayCount::D360});
  EXPECT_EQ(implicit.result.final_cash, explicit_zero.result.final_cash);
  EXPECT_DOUBLE_EQ(implicit.result.final_equity, explicit_zero.result.final_equity);
}

} // namespace atxtest_backtest_borrow_test
