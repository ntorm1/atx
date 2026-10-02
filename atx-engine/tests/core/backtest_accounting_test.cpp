// Portfolio valuation must see the permanent mark shift from the current fills.

#include <array>
#include <memory>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/datetime.hpp"
#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/backtest_loop.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/rolling_panel.hpp"
#include "atx/engine/loop/signal_source.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/portfolio/portfolio.hpp"

namespace atxtest_backtest_accounting_test {

using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::time::Timestamp;
using atx::engine::Delay;

struct Outcome {
  atx::engine::BacktestResult result;
  atx::i64 qty = 0;
  atx::f64 holding_mark = 0.0;
  atx::f64 market_mark = 0.0;
};

[[nodiscard]] Outcome run_impact(Delay delay, int bars, bool repeat_signal = false,
                                atx::f64 leverage = 0.1, atx::f64 gamma = 2.0) {
  using namespace atx::engine;
  using namespace atx::engine::exec;
  const std::array<InstrumentId, 1> universe{InstrumentId{1}};
  const std::array<InstrumentStats, 1> stats{InstrumentStats{1000.0, 0.1, 0.0}};
  std::vector<data::BarRow> rows;
  for (int index = 1; index <= bars; ++index) {
    Bar bar{};
    bar.ts = Timestamp::from_unix_nanos(index);
    bar.open = Price::from_int(100);
    bar.high = Price::from_int(100);
    bar.low = Price::from_int(100);
    bar.close = Price::from_int(100);
    bar.volume = Quantity::from_int(1'000'000);
    rows.push_back(data::BarRow{universe[0], bar, bar.ts, false});
  }
  const std::array<std::span<const data::BarRow>, 1> sources{
      std::span<const data::BarRow>{rows}};
  auto bus = std::make_unique<EventBus<>>();
  SimClock clock;
  data::InMemoryBarFeed feed{sources, clock, *bus};
  RollingPanel<8> panel{universe, 1};
  const atx::usize signal_count = repeat_signal ? static_cast<atx::usize>(bars) : 1U;
  ScriptedSignalSource signal{
      std::vector<std::vector<atx::f64>>(signal_count, std::vector<atx::f64>{1.0}), 1, 1};
  WeightPolicy policy{};
  policy.transform = Transform::Raw;
  policy.dollar_neutral = false;
  policy.winsorize_limit = 0.0;
  policy.gross_leverage = leverage;
  ExecutionSimulator sim{
      FillCfg{}, SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
      ImpactCfg{0.0, 0.5, gamma},
      CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
      LatencyCfg{}, VolumeCapCfg{1.0}};
  Portfolio portfolio{Decimal::from_int(100'000), universe};
  Market market{universe, stats};
  BacktestLoop<8> loop{feed, clock, *bus, panel, signal, policy, sim, portfolio,
                       market, Universe{universe}, Schedule{1}, delay};
  Outcome outcome;
  outcome.result = loop.run();
  outcome.qty = portfolio.holding(universe[0]).qty;
  outcome.holding_mark = portfolio.holding(universe[0]).mark;
  outcome.market_mark = market.mark(universe[0]);
  return outcome;
}

void expect_current_marks(const Outcome &outcome, atx::i64 fill_time) {
  ASSERT_EQ(outcome.result.fills.size(), 1U);
  EXPECT_EQ(outcome.result.execution_diagnostics.filled_orders, 1U);
  EXPECT_EQ(outcome.result.execution_diagnostics.invalid_economics, 0U);
  EXPECT_EQ(outcome.result.fills.front().t.unix_nanos(), fill_time);
  EXPECT_EQ(outcome.qty, 100);
  EXPECT_DOUBLE_EQ(outcome.market_mark, 101.0);
  EXPECT_DOUBLE_EQ(outcome.holding_mark, outcome.market_mark);
  EXPECT_EQ(outcome.result.final_cash, Decimal::from_int(90'000));
  EXPECT_DOUBLE_EQ(outcome.result.final_equity, 100'100.0);
  ASSERT_FALSE(outcome.result.equity_curve.empty());
  EXPECT_DOUBLE_EQ(outcome.result.equity_curve.back().equity, 100'100.0);
  EXPECT_DOUBLE_EQ(outcome.result.equity_curve.back().gross, 10'100.0);
  EXPECT_DOUBLE_EQ(outcome.result.equity_curve.back().net, 10'100.0);
}

TEST(BacktestAccounting, NextFill_FinalSliceReflectsPermanentImpact) {
  expect_current_marks(run_impact(Delay::Next, 2), 2);
}

TEST(BacktestAccounting, SameFill_FinalSliceReflectsPermanentImpact) {
  expect_current_marks(run_impact(Delay::Same, 1), 1);
}

TEST(BacktestAccounting, Rebalance_AfterImpact_UsesCurrentEquityAndPrice) {
  for (const Delay delay : {Delay::Next, Delay::Same}) {
    const Outcome outcome = run_impact(delay, 3, true, 1.0);
    // All-in 1000 shares at $100 shift the mark to $110. Equity is $110k,
    // so the unchanged 100% target stays at 1000 shares, without a phantom sale.
    ASSERT_EQ(outcome.result.fills.size(), 1U);
    EXPECT_EQ(outcome.qty, 1000);
    EXPECT_NEAR(outcome.market_mark, 110.0, 1e-12);
    EXPECT_DOUBLE_EQ(outcome.holding_mark, outcome.market_mark);
    EXPECT_NEAR(outcome.result.final_equity, 110'000.0, 1e-8);
  }
}

TEST(BacktestAccounting, ZeroPermanentImpact_PreservesFrictionlessEquity) {
  for (const Delay delay : {Delay::Next, Delay::Same}) {
    const Outcome outcome = run_impact(delay, 3, false, 0.1, 0.0);
    EXPECT_DOUBLE_EQ(outcome.holding_mark, 100.0);
    EXPECT_DOUBLE_EQ(outcome.market_mark, 100.0);
    EXPECT_DOUBLE_EQ(outcome.result.final_equity, 100'000.0);
  }
}

TEST(BacktestAccounting, InvalidExecutionConfigurationRefusesTheRun) {
  EXPECT_THROW((void)run_impact(Delay::Next, 2, false, 0.1,
      std::numeric_limits<atx::f64>::quiet_NaN()), std::invalid_argument);
}

} // namespace atxtest_backtest_accounting_test
