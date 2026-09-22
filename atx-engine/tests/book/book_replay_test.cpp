#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/replay.hpp"

namespace {
namespace book = atx::engine::book;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

Panel prices(atx::usize dates, atx::usize instruments, std::vector<atx::f64> close,
             std::vector<atx::u8> universe = {}) {
  return Panel::create(dates, instruments, {"close"}, {std::move(close)},
                        std::move(universe)).value();
}

std::vector<atx::i64> days(atx::usize count) {
  std::vector<atx::i64> times;
  for (atx::usize i = 0; i < count; ++i) times.push_back(static_cast<atx::i64>(i) * kDay);
  return times;
}

book::ReplayConfig immediate() {
  book::ReplayConfig cfg;
  cfg.initial_nav = 100.0;
  cfg.execution_delay_periods = 0;
  return cfg;
}

void expect_self_financing(const book::ReplayResult &result) {
  for (const auto &row : result.intervals) {
    EXPECT_NEAR(row.cash + row.assets, row.nav, 1.0e-12);
    EXPECT_NEAR(row.pretrade_nav + row.gross_pnl - row.trade_cost - row.borrow_cost,
                row.nav, 1.0e-12);
    EXPECT_NEAR((row.nav - row.pretrade_nav) / row.pretrade_nav, row.net_return, 1.0e-14);
  }
}
} // namespace

TEST(BookReplay, WeeklyHoldingsDriftAcrossEveryIntervalAndTerminalTargetDoesNotTrade) {
  const auto panel = prices(6, 1, {100, 110, 121, 121, 121, 121});
  const std::vector<atx::usize> schedule{0, 5};
  const std::vector<atx::f64> targets{0.5, -1.0};
  const auto result = book::replay_scheduled_targets(
      panel, days(6), schedule, targets, immediate());
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(result->intervals.size(), 5U);
  ASSERT_EQ(result->trades.size(), 1U);
  EXPECT_DOUBLE_EQ(result->trades[0].dollar_delta, 50.0);
  EXPECT_DOUBLE_EQ(result->final_tri_units[0], 0.5);
  EXPECT_DOUBLE_EQ(result->final_cash, 50.0);
  EXPECT_DOUBLE_EQ(result->final_assets, 60.5);
  EXPECT_DOUBLE_EQ(result->final_nav, 110.5); // 0.5 units * 121 + 50 cash.
  EXPECT_DOUBLE_EQ(result->intervals[0].gross_pnl, 5.0);
  EXPECT_DOUBLE_EQ(result->intervals[1].gross_pnl, 5.5);
  EXPECT_EQ(result->effective_rebalances, 1U);
  EXPECT_EQ(result->unexecuted_decisions, 1U);
  expect_self_financing(*result);

  // A future observation makes the former terminal target executable, but never
  // changes any already-completed valuation interval.
  const auto extended = prices(7, 1, {100, 110, 121, 121, 121, 121, 133.1});
  const auto later = book::replay_scheduled_targets(
      extended, days(7), schedule, targets, immediate());
  ASSERT_TRUE(later.has_value()) << later.error().message();
  EXPECT_EQ(later->effective_rebalances, 2U);
  for (atx::usize i = 0; i < result->intervals.size(); ++i) {
    EXPECT_DOUBLE_EQ(later->intervals[i].pretrade_nav, result->intervals[i].pretrade_nav);
    EXPECT_DOUBLE_EQ(later->intervals[i].nav, result->intervals[i].nav);
    EXPECT_DOUBLE_EQ(later->intervals[i].gross_pnl, result->intervals[i].gross_pnl);
  }
}

TEST(BookReplay, TradeFeesUseActualDollarsAndLaterTargetsUseNavAfterEarlierFees) {
  const auto panel = prices(4, 1, {100, 100, 100, 100});
  auto cfg = immediate();
  cfg.trade_bps = 10.0;
  const std::vector<atx::usize> schedule{0, 2, 3};
  const std::vector<atx::f64> targets{1.0, 0.5, 0.0};
  const auto result = book::replay_scheduled_targets(panel, days(4), schedule, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(result->trades.size(), 2U);
  EXPECT_DOUBLE_EQ(result->trades[0].dollar_delta, 100.0);
  EXPECT_NEAR(result->intervals[0].trade_cost, 0.1, 1.0e-15);
  EXPECT_NEAR(result->intervals[0].cash, -0.1, 1.0e-15);
  EXPECT_NEAR(result->intervals[0].nav, 99.9, 1.0e-12);
  EXPECT_DOUBLE_EQ(result->intervals[1].traded_dollars, 0.0);
  EXPECT_DOUBLE_EQ(result->intervals[1].trade_cost, 0.0);
  EXPECT_NEAR(result->trades[1].dollar_delta, -50.05, 1.0e-12);
  EXPECT_NEAR(result->intervals[2].trade_cost, 0.05005, 1.0e-14);
  EXPECT_NEAR(result->final_assets, 49.95, 1.0e-12);
  EXPECT_NEAR(result->final_cash, 49.89995, 1.0e-12);
  EXPECT_NEAR(result->final_nav, 99.84995, 1.0e-12);
  EXPECT_EQ(result->unexecuted_decisions, 1U);
  expect_self_financing(*result);
}

TEST(BookReplay, WeekendBorrowUsesStartShortNotionalAndCoverStopsFurtherAccrual) {
  const auto panel = prices(4, 1, {100, 100, 100, 100});
  const std::vector<atx::i64> times{0, 3 * kDay, 4 * kDay, 5 * kDay};
  const std::vector<atx::usize> schedule{0, 2};
  const std::vector<atx::f64> targets{-0.5, 0.0};
  for (const auto basis : {book::ReplayDayBasis::D365, book::ReplayDayBasis::D360}) {
    auto cfg = immediate();
    cfg.borrow_day_basis = basis;
    cfg.annual_borrow_bps = basis == book::ReplayDayBasis::D365 ? 365.0 : 360.0;
    const auto result = book::replay_scheduled_targets(panel, times, schedule, targets, cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result->intervals.size(), 3U);
    EXPECT_NEAR(result->intervals[0].borrow_cost, 0.015, 1.0e-15);
    EXPECT_NEAR(result->intervals[1].borrow_cost, 0.005, 1.0e-15);
    EXPECT_DOUBLE_EQ(result->intervals[2].borrow_cost, 0.0);
    EXPECT_NEAR(result->final_nav, 99.98, 1.0e-12);
    EXPECT_DOUBLE_EQ(result->final_assets, 0.0);
    EXPECT_DOUBLE_EQ(result->final_tri_units[0], 0.0);
    ASSERT_EQ(result->trades.size(), 2U);
    EXPECT_DOUBLE_EQ(result->trades[1].dollar_delta, 50.0);
    expect_self_financing(*result);
  }
}

TEST(BookReplay, DefaultDelayExcludesEarlierMoveAndPreservesOriginWhileHolding) {
  // Decision is eligible at row zero. Subsequent mask loss neither cancels the
  // already-decided target nor erases its carried value under this declared seam.
  const auto panel = prices(4, 1, {100, 200, 220, 242}, {1, 0, 0, 0});
  book::ReplayConfig cfg;
  cfg.initial_nav = 100.0;
  const std::vector<atx::usize> schedule{0, 2};
  const std::vector<atx::f64> targets{1.0, 0.0};
  const auto result = book::replay_scheduled_targets(panel, days(4), schedule, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(result->intervals.size(), 3U);
  EXPECT_FALSE(result->intervals[0].decision_period.has_value());
  EXPECT_DOUBLE_EQ(result->intervals[0].nav, 100.0);
  EXPECT_EQ(result->intervals[1].decision_period, 0U);
  EXPECT_EQ(result->intervals[2].decision_period, 0U);
  EXPECT_DOUBLE_EQ(result->intervals[1].gross_pnl, 10.0);
  EXPECT_DOUBLE_EQ(result->intervals[2].gross_pnl, 11.0);
  EXPECT_DOUBLE_EQ(result->final_nav, 121.0);
  ASSERT_EQ(result->trades.size(), 1U);
  EXPECT_EQ(result->trades[0].period, 1U);
  EXPECT_EQ(result->trades[0].decision_period, 0U);
  EXPECT_EQ(result->unexecuted_decisions, 1U);
  expect_self_financing(*result);
}

TEST(BookReplay, EligibilityExitStillLosesMoneyAndOnlyUnusedMissingMarksAreIgnored) {
  const std::vector<atx::u8> mask{1, 0, 0, 0, 0, 0};
  const auto panel = prices(3, 2, {100, kNaN, 80, kNaN, 88, kNaN}, mask);
  const std::vector<atx::usize> schedule{0};
  const std::vector<atx::f64> targets{1.0, 0.0};
  const auto result = book::replay_scheduled_targets(
      panel, days(3), schedule, targets, immediate());
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_DOUBLE_EQ(result->intervals[0].gross_pnl, -20.0);
  EXPECT_DOUBLE_EQ(result->intervals[1].gross_pnl, 8.0);
  EXPECT_DOUBLE_EQ(result->final_nav, 88.0);
  expect_self_financing(*result);
  for (const auto bad : {kNaN, 0.0, -1.0, std::numeric_limits<atx::f64>::infinity()}) {
    const auto missing = prices(3, 2, {100, kNaN, bad, kNaN, 88, kNaN}, mask);
    const auto rejected = book::replay_scheduled_targets(
        missing, days(3), schedule, targets, immediate());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_NE(rejected.error().message().find("period=1 instrument=0"), std::string::npos);
  }
}

TEST(BookReplay, ReversalTradesFullDollarDeltaAndUnchangedInventoryDoesNotTrade) {
  const auto panel = prices(4, 1, {100, 100, 100, 100});
  const std::vector<atx::usize> schedule{0, 2};
  const std::vector<atx::f64> targets{0.5, -0.5};
  const auto result = book::replay_scheduled_targets(
      panel, days(4), schedule, targets, immediate());
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(result->trades.size(), 2U);
  EXPECT_DOUBLE_EQ(result->trades[0].dollar_delta, 50.0);
  EXPECT_DOUBLE_EQ(result->trades[1].dollar_delta, -100.0);
  EXPECT_DOUBLE_EQ(result->intervals[1].traded_dollars, 0.0);
  EXPECT_DOUBLE_EQ(result->intervals[1].start_gross, 50.0);
  EXPECT_DOUBLE_EQ(result->intervals[2].traded_dollars, 100.0);
  EXPECT_DOUBLE_EQ(result->final_cash, 150.0);
  EXPECT_DOUBLE_EQ(result->final_assets, -50.0);
  EXPECT_DOUBLE_EQ(result->final_nav, 100.0);
  expect_self_financing(*result);
}

TEST(BookReplay, SmallHoldingsSurviveAndChangingTriScaleDoesNotChangeEconomics) {
  const std::vector<atx::usize> schedule{0};
  const std::vector<atx::f64> half{0.5};
  for (const auto scale : {1.0, 1.0e12, 1.0e-100}) {
    const auto panel = prices(3, 1, {100 * scale, 110 * scale, 121 * scale});
    const auto result = book::replay_scheduled_targets(panel, days(3), schedule, half, immediate());
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_NEAR(result->final_nav, 110.5, 1.0e-12);
    EXPECT_NE(result->final_tri_units[0], 0.0);
    expect_self_financing(*result);
  }
  const auto panel = prices(2, 1, {100, 200});
  const std::vector<atx::f64> tiny{1.0e-15};
  const auto result = book::replay_scheduled_targets(panel, days(2), schedule, tiny, immediate());
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(result->trades.size(), 1U);
  EXPECT_GT(result->trades[0].dollar_delta, 0.0);
  EXPECT_GT(result->final_tri_units[0], 0.0);
  EXPECT_GT(result->final_nav, 100.0);
}

TEST(BookReplay, ShapesSchedulesTimesAndTargetsAreValidatedBeforeReplay) {
  const auto panel = prices(3, 1, {100, 100, 100});
  const auto times = days(3);
  const std::vector<atx::usize> schedule{0};
  const std::vector<atx::f64> target{0.5};
  EXPECT_FALSE(book::replay_scheduled_targets(panel, days(2), schedule, target, immediate()));
  EXPECT_FALSE(book::replay_scheduled_targets(panel, times, schedule, {}, immediate()));
  for (const auto &bad_schedule : {std::vector<atx::usize>{0, 0},
                                   std::vector<atx::usize>{2, 1},
                                   std::vector<atx::usize>{0, 3}}) {
    const std::vector<atx::f64> two_targets{0.0, 0.0};
    EXPECT_FALSE(book::replay_scheduled_targets(
        panel, times, bad_schedule, two_targets, immediate()));
  }
  for (const auto &bad_times : {std::vector<atx::i64>{0, 0, kDay},
                                std::vector<atx::i64>{0, -kDay, kDay},
                                std::vector<atx::i64>{std::numeric_limits<atx::i64>::min(),
                                                     0, kDay}}) {
    EXPECT_FALSE(book::replay_scheduled_targets(panel, bad_times, schedule, target, immediate()));
  }
  const std::vector<atx::i64> crossing_epoch{-kDay, 0, kDay};
  EXPECT_TRUE(book::replay_scheduled_targets(panel, crossing_epoch, schedule, target, immediate()));
  for (const auto invalid : {kNaN, std::numeric_limits<atx::f64>::infinity()}) {
    const std::vector<atx::f64> invalid_target{invalid};
    EXPECT_FALSE(book::replay_scheduled_targets(
        panel, times, schedule, invalid_target, immediate()));
  }
  const auto ineligible = prices(3, 1, {100, 100, 100}, {0, 1, 1});
  EXPECT_FALSE(book::replay_scheduled_targets(ineligible, times, schedule, target, immediate()));
  auto delayed = immediate();
  delayed.execution_delay_periods = std::numeric_limits<atx::usize>::max();
  const std::vector<atx::usize> later_decision{1};
  EXPECT_FALSE(book::replay_scheduled_targets(panel, times, later_decision, target, delayed));
}

TEST(BookReplay, InvalidCostsInsolvencyAndUnrepresentablePositionsFailClosed) {
  const auto flat = prices(2, 1, {100, 100});
  const std::vector<atx::usize> schedule{0};
  const std::vector<atx::f64> target{1.0};
  for (const auto bad : {-1.0, kNaN, std::numeric_limits<atx::f64>::infinity()}) {
    auto cfg = immediate();
    cfg.trade_bps = bad;
    EXPECT_FALSE(book::replay_scheduled_targets(flat, days(2), schedule, target, cfg));
    cfg = immediate();
    cfg.annual_borrow_bps = bad;
    EXPECT_FALSE(book::replay_scheduled_targets(flat, days(2), schedule, target, cfg));
    cfg = immediate();
    cfg.initial_nav = bad;
    EXPECT_FALSE(book::replay_scheduled_targets(flat, days(2), schedule, target, cfg));
  }
  auto cfg = immediate();
  cfg.borrow_day_basis = static_cast<book::ReplayDayBasis>(252);
  EXPECT_FALSE(book::replay_scheduled_targets(flat, days(2), {}, {}, cfg));
  cfg = immediate();
  cfg.initial_nav = 0.0;
  EXPECT_FALSE(book::replay_scheduled_targets(flat, days(2), schedule, target, cfg));
  cfg = immediate();
  cfg.trade_bps = 10'000.0;
  EXPECT_FALSE(book::replay_scheduled_targets(flat, days(2), schedule, target, cfg));
  const auto rally = prices(2, 1, {100, 300});
  const std::vector<atx::f64> short_target{-0.5};
  EXPECT_FALSE(book::replay_scheduled_targets(rally, days(2), schedule, short_target, immediate()));
  const std::vector<atx::f64> huge{std::numeric_limits<atx::f64>::max()};
  EXPECT_FALSE(book::replay_scheduled_targets(flat, days(2), schedule, huge, immediate()));
  const auto tiny_price = prices(2, 1, {std::numeric_limits<atx::f64>::denorm_min(), 100});
  EXPECT_FALSE(book::replay_scheduled_targets(tiny_price, days(2), schedule, target, immediate()));
  // Finite marks alone are insufficient: 3 dollars of initial equity round to
  // 4 when cash and a 1.2e16-dollar short are naively added/subtracted.
  cfg = immediate();
  cfg.initial_nav = 3.0;
  const std::vector<atx::f64> ill_conditioned{-4.0e15};
  const auto lost_cash = book::replay_scheduled_targets(
      flat, days(2), schedule, ill_conditioned, cfg);
  ASSERT_FALSE(lost_cash.has_value());
  EXPECT_NE(lost_cash.error().message().find("do not reconcile"), std::string::npos);
}

TEST(BookReplay, FinalObservationIsValuationOnlyAndEmptyScheduleRemainsCash) {
  const auto terminal = prices(1, 1, {kNaN});
  auto cfg = immediate();
  cfg.trade_bps = 10.0;
  cfg.annual_borrow_bps = 365.0;
  const std::vector<atx::usize> schedule{0};
  const std::vector<atx::f64> targets{-1.0};
  const auto result = book::replay_scheduled_targets(terminal, days(1), schedule, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_TRUE(result->intervals.empty());
  EXPECT_TRUE(result->trades.empty());
  EXPECT_EQ(result->effective_rebalances, 0U);
  EXPECT_EQ(result->unexecuted_decisions, 1U);
  EXPECT_DOUBLE_EQ(result->final_cash, 100.0);
  EXPECT_DOUBLE_EQ(result->final_assets, 0.0);
  EXPECT_DOUBLE_EQ(result->final_nav, 100.0);
  const auto unused = prices(3, 1, {kNaN, kNaN, kNaN});
  const auto cash = book::replay_scheduled_targets(unused, days(3), {}, {}, cfg);
  ASSERT_TRUE(cash.has_value()) << cash.error().message();
  EXPECT_EQ(cash->intervals.size(), 2U);
  EXPECT_TRUE(cash->trades.empty());
  EXPECT_DOUBLE_EQ(cash->final_nav, 100.0);
}

TEST(BookReplay, PolicySeesMarkedDriftAndRecordsActualAcceptedAllocation) {
  const auto panel = prices(4, 2, {100, kNaN, 120, kNaN, 120, kNaN, 120, kNaN});
  const std::vector<atx::usize> schedule{0, 1};
  const std::vector<atx::f64> preferences{0.5, 0.0, 0.5, 0.0};
  auto cfg = immediate();
  cfg.trade_bps = 10.0;
  atx::usize calls = 0;
  const book::ReplayAllocationPolicy policy = [&](const book::ReplayAllocationState &state) {
    EXPECT_EQ(state.schedule_index, calls);
    EXPECT_EQ(state.execution_period, calls);
    EXPECT_EQ(state.tri_units.size(), 2U);
    EXPECT_EQ(state.marked_dollars.size(), 2U);
    EXPECT_TRUE(std::isnan(state.current_marks[1]));
    EXPECT_DOUBLE_EQ(state.marked_dollars[1], 0.0);
    if (calls == 0) {
      EXPECT_DOUBLE_EQ(state.cash, 100.0);
      EXPECT_DOUBLE_EQ(state.pretrade_nav, 100.0);
      EXPECT_DOUBLE_EQ(state.tri_units[0], 0.0);
    } else {
      EXPECT_DOUBLE_EQ(state.tri_units[0], 0.5);
      EXPECT_DOUBLE_EQ(state.marked_dollars[0], 60.0);
      EXPECT_NEAR(state.cash, 49.95, 1.0e-12);
      EXPECT_NEAR(state.pretrade_nav, 109.95, 1.0e-12);
    }
    ++calls;
    return atx::core::Ok(std::vector<atx::f64>(
        state.preference_weights.begin(), state.preference_weights.end()));
  };
  const auto result = book::replay_scheduled_targets(
      panel, days(4), schedule, preferences, policy, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(calls, 2U);
  ASSERT_EQ(result->allocations.size(), 2U);
  const auto &second = result->allocations[1];
  EXPECT_EQ(second.decision_period, 1U);
  EXPECT_EQ(second.execution_period, 1U);
  EXPECT_EQ(second.decision_session_key, kDay);
  EXPECT_EQ(second.execution_session_key, kDay);
  EXPECT_EQ(second.target_weights, (std::vector<atx::f64>{0.5, 0.0}));
  EXPECT_EQ(second.pretrade_marked_dollars, (std::vector<atx::f64>{60.0, 0.0}));
  EXPECT_NEAR(second.posttrade_marked_dollars[0], 54.975, 1.0e-12);
  EXPECT_NEAR(second.traded_dollars, 5.025, 1.0e-12);
  EXPECT_NEAR(second.trade_cost, 0.005025, 1.0e-12);
  EXPECT_NEAR(second.posttrade_nav, second.pretrade_nav - second.trade_cost, 1.0e-12);
  EXPECT_NEAR(second.posttrade_cash + second.posttrade_marked_dollars[0],
              second.posttrade_nav, 1.0e-12);
  EXPECT_NEAR(result->replay.trades[1].dollar_delta, -5.025, 1.0e-12);
  expect_self_financing(result->replay);
}

TEST(BookReplay, PolicyReceivesOriginalDecisionRowsAndNeverRunsAtFinalValuation) {
  const std::vector<atx::usize> schedule{0, 1, 3};
  const std::vector<atx::f64> preferences{0.5, 0.0, 0.0, -0.25, 0.0, 0.0};
  const std::vector<atx::u8> mask{1, 0, 0, 1, 1, 0, 1, 1, 0, 0};
  auto cfg = immediate();
  cfg.execution_delay_periods = 1;
  // Changing later marks changes sizing/P&L, but never substitutes those rows'
  // eligibility or the next scheduled preference for the original decision.
  for (const auto execution_price : {200.0, 250.0}) {
    const auto panel = prices(5, 2,
        {100, 100, execution_price, 100, 150, 100, 180, 100, 190, 100}, mask);
    atx::usize calls = 0;
    const book::ReplayAllocationPolicy policy = [&](const book::ReplayAllocationState &state) {
      EXPECT_EQ(state.schedule_index, calls);
      EXPECT_EQ(state.decision_period, calls);
      EXPECT_EQ(state.execution_period, calls + 1);
      EXPECT_EQ(state.decision_session_key, static_cast<atx::i64>(calls) * kDay);
      EXPECT_EQ(state.execution_session_key, static_cast<atx::i64>(calls + 1) * kDay);
      EXPECT_EQ(state.decision_eligibility[0], calls == 0 ? 1U : 0U);
      EXPECT_EQ(state.decision_eligibility[1], calls == 0 ? 0U : 1U);
      EXPECT_EQ(state.preference_weights[0], calls == 0 ? 0.5 : 0.0);
      EXPECT_EQ(state.preference_weights[1], calls == 0 ? 0.0 : -0.25);
      if (calls == 0) {
        EXPECT_DOUBLE_EQ(state.current_marks[0], execution_price);
      } else {
        EXPECT_DOUBLE_EQ(state.tri_units[0], 50.0 / execution_price);
        EXPECT_DOUBLE_EQ(state.marked_dollars[0], (50.0 / execution_price) * 150.0);
      }
      ++calls;
      return atx::core::Ok(std::vector<atx::f64>(
          state.preference_weights.begin(), state.preference_weights.end()));
    };
    const auto result = book::replay_scheduled_targets(
        panel, days(5), schedule, preferences, policy, cfg);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(calls, 2U);
    EXPECT_EQ(result->allocations.size(), 2U);
    EXPECT_EQ(result->replay.unexecuted_decisions, 1U);
    EXPECT_EQ(result->replay.final_tri_units[0], 0.0);
    EXPECT_LT(result->replay.final_tri_units[1], 0.0);
    expect_self_financing(result->replay);
  }
}

TEST(BookReplay, PolicyErrorsAndInvalidCandidatesReturnNoPartialResult) {
  const auto panel = prices(3, 2, {100, kNaN, 100, kNaN, 100, kNaN}, {1, 0, 1, 0, 1, 0});
  const std::vector<atx::usize> schedule{0, 1};
  const std::vector<atx::f64> preferences{0.5, 0.0, 0.5, 0.0};
  const book::ReplayAllocationPolicy empty;
  EXPECT_FALSE(book::replay_scheduled_targets(
      panel, days(3), schedule, preferences, empty, immediate()));
  const std::vector<std::vector<atx::f64>> bad_targets{
      {}, {0.5}, {kNaN, 0.0}, {0.0, 0.1},
      {std::numeric_limits<atx::f64>::max(), 0.0}};
  for (const auto &bad_target : bad_targets) {
    atx::usize calls = 0;
    const book::ReplayAllocationPolicy policy = [&](const book::ReplayAllocationState &) {
      return atx::core::Ok(calls++ == 0 ? std::vector<atx::f64>{0.5, 0.0} : bad_target);
    };
    const auto rejected = book::replay_scheduled_targets(
        panel, days(3), schedule, preferences, policy, immediate());
    EXPECT_FALSE(rejected.has_value());
    EXPECT_EQ(calls, 2U); // The earlier accepted allocation is never published.
  }
  const book::ReplayAllocationPolicy error = [](const book::ReplayAllocationState &)
      -> atx::core::Result<std::vector<atx::f64>> {
    return atx::core::Err(atx::core::ErrorCode::Unavailable, "declared policy failure");
  };
  const auto rejected = book::replay_scheduled_targets(
      panel, days(3), schedule, preferences, error, immediate());
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_EQ(rejected.error().message(), "declared policy failure");

  atx::usize calls = 0;
  const book::ReplayAllocationPolicy exit = [&](const book::ReplayAllocationState &state) {
    ++calls;
    return atx::core::Ok(std::vector<atx::f64>(
        state.preference_weights.begin(), state.preference_weights.end()));
  };
  const auto missing_held_mark = prices(3, 1, {100, kNaN, 100});
  const std::vector<atx::f64> enter_then_exit{0.5, 0.0};
  const auto missing = book::replay_scheduled_targets(
      missing_held_mark, days(3), schedule, enter_then_exit, exit, immediate());
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(calls, 1U); // An intended exit cannot hide a missing held price.
  EXPECT_NE(missing.error().message().find("period=1 instrument=0"), std::string::npos);

  const auto unused_missing_mark = prices(2, 1, {kNaN, 100});
  const std::vector<atx::usize> enter{0};
  const std::vector<atx::f64> zero_preference{0.0};
  const book::ReplayAllocationPolicy buy = [](const book::ReplayAllocationState &) {
    return atx::core::Ok(std::vector<atx::f64>{0.5});
  };
  EXPECT_FALSE(book::replay_scheduled_targets(
      unused_missing_mark, days(2), enter, zero_preference, buy, immediate()));
}

TEST(BookReplay, IdentityPolicyExactlyMatchesFixedReplayWithDriftFeesAndBorrow) {
  const auto panel = prices(6, 2, {100, 200, 101, 190, 103, 191,
                                  102, 185, 108, 186, 110, 180});
  const std::vector<atx::usize> schedule{0, 2, 4};
  const std::vector<atx::f64> preferences{0.4, -0.4, 0.1, -0.1, 0.0, 0.0};
  const std::vector<atx::i64> times{0, kDay, 4 * kDay, 5 * kDay, 6 * kDay, 7 * kDay};
  const book::ReplayAllocationPolicy identity = [](const book::ReplayAllocationState &state) {
    return atx::core::Ok(std::vector<atx::f64>(
        state.preference_weights.begin(), state.preference_weights.end()));
  };
  auto cfg = immediate();
  cfg.execution_delay_periods = 1;
  cfg.trade_bps = 5.0;
  cfg.annual_borrow_bps = 365.0;
  const auto fixed = book::replay_scheduled_targets(panel, times, schedule, preferences, cfg);
  const auto allocated = book::replay_scheduled_targets(
      panel, times, schedule, preferences, identity, cfg);
  ASSERT_TRUE(fixed.has_value()) << fixed.error().message();
  ASSERT_TRUE(allocated.has_value()) << allocated.error().message();
  const auto &actual = allocated->replay;
  ASSERT_EQ(actual.intervals.size(), fixed->intervals.size());
  for (atx::usize i = 0; i < actual.intervals.size(); ++i) {
    const auto &a = actual.intervals[i];
    const auto &b = fixed->intervals[i];
    EXPECT_EQ(a.start_period, b.start_period);
    EXPECT_EQ(a.end_period, b.end_period);
    EXPECT_EQ(a.decision_period, b.decision_period);
    EXPECT_EQ(a.pretrade_nav, b.pretrade_nav);
    EXPECT_EQ(a.cash, b.cash);
    EXPECT_EQ(a.assets, b.assets);
    EXPECT_EQ(a.nav, b.nav);
    EXPECT_EQ(a.gross_pnl, b.gross_pnl);
    EXPECT_EQ(a.trade_cost, b.trade_cost);
    EXPECT_EQ(a.borrow_cost, b.borrow_cost);
    EXPECT_EQ(a.net_return, b.net_return);
    EXPECT_EQ(a.traded_dollars, b.traded_dollars);
    EXPECT_EQ(a.start_gross, b.start_gross);
    EXPECT_EQ(a.end_gross, b.end_gross);
  }
  ASSERT_EQ(actual.trades.size(), fixed->trades.size());
  for (atx::usize i = 0; i < actual.trades.size(); ++i) {
    EXPECT_EQ(actual.trades[i].period, fixed->trades[i].period);
    EXPECT_EQ(actual.trades[i].decision_period, fixed->trades[i].decision_period);
    EXPECT_EQ(actual.trades[i].instrument, fixed->trades[i].instrument);
    EXPECT_EQ(actual.trades[i].dollar_delta, fixed->trades[i].dollar_delta);
  }
  EXPECT_EQ(actual.final_tri_units, fixed->final_tri_units);
  EXPECT_EQ(actual.initial_nav, fixed->initial_nav);
  EXPECT_EQ(actual.final_cash, fixed->final_cash);
  EXPECT_EQ(actual.final_assets, fixed->final_assets);
  EXPECT_EQ(actual.final_nav, fixed->final_nav);
  EXPECT_EQ(actual.effective_rebalances, fixed->effective_rebalances);
  EXPECT_EQ(actual.unexecuted_decisions, fixed->unexecuted_decisions);
  EXPECT_EQ(allocated->allocations.size(), actual.effective_rebalances);
}

TEST(BookReplay, ExplicitHoldPreservesUnitsWithoutWeightRoundTripAndCloseIsExact) {
  const auto panel = prices(5, 2,
      {3, kNaN, 11, kNaN, 11, kNaN, 11, kNaN, kNaN, kNaN});
  const std::vector<atx::usize> schedule{0, 1, 3, 4};
  const std::vector<atx::f64> preferences{0.3, 0, 0, 0, 0, 0, 0, 0};
  auto cfg = immediate();
  cfg.trade_bps = 10.0;
  atx::usize calls = 0;
  using Action = book::ReplayTargetAction;
  const book::ReplayIntentPolicy policy = [&](const book::ReplayAllocationState &state) {
    std::vector<book::ReplayTargetIntent> intents{{Action::Close, 0}, {Action::HoldCurrent, 0}};
    if (calls == 0) intents[0] = {Action::TargetWeight, 0.3};
    if (calls == 1) {
      const auto round_trip = book::resolve_replay_target(
          {Action::TargetWeight, state.marked_dollars[0] / state.pretrade_nav},
          state.tri_units[0], state.marked_dollars[0], state.current_marks[0],
          state.pretrade_nav, true);
      EXPECT_TRUE(round_trip.has_value());
      if (round_trip) {
        EXPECT_NE(round_trip->tri_units, state.tri_units[0]);
        EXPECT_NE(round_trip->dollar_delta, 0.0); // Weight reconstruction creates a ghost trade.
      }
      intents[0] = {Action::HoldCurrent, 0};
    }
    if (calls == 2) EXPECT_EQ(std::bit_cast<atx::u64>(state.tri_units[0]),
                              std::bit_cast<atx::u64>(10.0));
    ++calls;
    return atx::core::Ok(std::move(intents));
  };
  const auto result = book::replay_scheduled_intents(
      panel, days(5), schedule, preferences, policy, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(calls, 3U); // Terminal decision never invokes the policy.
  ASSERT_EQ(result->allocations.size(), 3U);
  const auto &held = result->allocations[1];
  EXPECT_EQ(held.target_intents[0].action, Action::HoldCurrent);
  EXPECT_EQ(held.target_weights[0], 110.0 / held.pretrade_nav);
  EXPECT_EQ(std::bit_cast<atx::u64>(held.pretrade_marked_dollars[0]),
            std::bit_cast<atx::u64>(held.posttrade_marked_dollars[0]));
  EXPECT_EQ(held.traded_dollars, 0.0);
  EXPECT_EQ(held.trade_cost, 0.0);
  EXPECT_EQ(held.pretrade_cash, held.posttrade_cash);
  EXPECT_EQ(held.pretrade_nav, held.posttrade_nav);
  const auto &closed = result->allocations[2];
  EXPECT_EQ(closed.target_intents[0].action, Action::Close);
  EXPECT_EQ(std::bit_cast<atx::u64>(closed.posttrade_marked_dollars[0]), 0U);
  EXPECT_EQ(closed.target_weights[0], 0.0);
  ASSERT_EQ(result->replay.trades.size(), 2U);
  EXPECT_EQ(result->replay.trades[1].dollar_delta, -110.0);
  EXPECT_EQ(std::bit_cast<atx::u64>(result->replay.final_tri_units[0]), 0U);
  EXPECT_EQ(result->replay.unexecuted_decisions, 1U);
  expect_self_financing(result->replay);
}

TEST(BookReplay, InvalidIntentsCannotBypassHeldMarksEligibilityOrPublishEarlierAllocations) {
  using Action = book::ReplayTargetAction;
  const auto panel = prices(3, 1, {3, 11, 11}, {1, 0, 1});
  const std::vector<atx::usize> schedule{0, 1};
  const std::vector<atx::f64> preferences{0.3, 0};
  const book::ReplayIntentPolicy empty;
  EXPECT_FALSE(book::replay_scheduled_intents(
      panel, days(3), schedule, preferences, empty, immediate()));
  const std::vector<std::vector<book::ReplayTargetIntent>> bad_candidates{
      {}, {{Action::HoldCurrent, 0}}, {{Action::TargetWeight, 0.1}},
      {{Action::HoldCurrent, 1}}, {{Action::Close, 1}}, {{Action::Close, kNaN}},
      {{Action::TargetWeight, kNaN}}, {{static_cast<Action>(255), 0}}};
  for (const auto &bad : bad_candidates) {
    atx::usize calls = 0;
    const book::ReplayIntentPolicy policy = [&](const book::ReplayAllocationState &) {
      return atx::core::Ok(calls++ == 0
          ? std::vector<book::ReplayTargetIntent>{{Action::TargetWeight, 0.3}} : bad);
    };
    const auto rejected = book::replay_scheduled_intents(
        panel, days(3), schedule, preferences, policy, immediate());
    EXPECT_FALSE(rejected.has_value());
    EXPECT_EQ(calls, 2U);
  }
  atx::usize calls = 0;
  const book::ReplayIntentPolicy close = [&](const book::ReplayAllocationState &) {
    return atx::core::Ok(std::vector<book::ReplayTargetIntent>{calls++ == 0
        ? book::ReplayTargetIntent{Action::TargetWeight, 0.3}
        : book::ReplayTargetIntent{Action::Close, 0}});
  };
  const auto allowed = book::replay_scheduled_intents(
      panel, days(3), schedule, preferences, close, immediate());
  ASSERT_TRUE(allowed.has_value()) << allowed.error().message();
  EXPECT_EQ(allowed->replay.final_tri_units[0], 0.0); // Ineligible exit is mandatory/allowed.
  calls = 0;
  const auto missing = prices(3, 1, {3, kNaN, 11}, {1, 0, 1});
  const auto rejected = book::replay_scheduled_intents(
      missing, days(3), schedule, preferences, close, immediate());
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(calls, 1U);
  EXPECT_NE(rejected.error().message().find("period=1 instrument=0"), std::string::npos);
  const book::ReplayIntentPolicy failure = [](const book::ReplayAllocationState &)
      -> atx::core::Result<std::vector<book::ReplayTargetIntent>> {
    return atx::core::Err(atx::core::ErrorCode::Unavailable, "intent failure");
  };
  const auto failed = book::replay_scheduled_intents(
      panel, days(3), schedule, preferences, failure, immediate());
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().code(), atx::core::ErrorCode::Unavailable);
  EXPECT_EQ(failed.error().message(), "intent failure");
  EXPECT_FALSE(book::resolve_replay_target({Action::Close, 0}, 1, 11, kNaN, 100, false));
  EXPECT_FALSE(book::resolve_replay_target({Action::HoldCurrent, 0}, 1, 10, 11, 100, true));
  for (const auto action : {Action::HoldCurrent, Action::Close, Action::TargetWeight}) {
    const auto zero = book::resolve_replay_target({action, 0}, 0, 0, kNaN, 100, false);
    ASSERT_TRUE(zero.has_value()) << zero.error().message();
    EXPECT_EQ(zero->tri_units, 0.0);
    EXPECT_EQ(zero->dollar_delta, 0.0);
  }
}

TEST(BookReplay, TargetWeightIntentsExactlyMatchDelayedFixedAndLegacyPolicyReplay) {
  const auto panel = prices(6, 2, {100, 200, 101, 190, 103, 191,
                                  102, 185, 108, 186, 110, 180});
  const std::vector<atx::usize> schedule{0, 2, 4};
  const std::vector<atx::f64> preferences{0.4, -0.4, 0.1, -0.1, 0, 0};
  const std::vector<atx::i64> times{0, kDay, 4 * kDay, 5 * kDay, 6 * kDay, 7 * kDay};
  auto cfg = immediate();
  cfg.execution_delay_periods = 1;
  cfg.trade_bps = 5;
  cfg.annual_borrow_bps = 365;
  const book::ReplayAllocationPolicy legacy = [](const book::ReplayAllocationState &state) {
    return atx::core::Ok(std::vector<atx::f64>(
        state.preference_weights.begin(), state.preference_weights.end()));
  };
  atx::usize calls = 0;
  const book::ReplayIntentPolicy policy = [&](const book::ReplayAllocationState &state) {
    EXPECT_EQ(state.schedule_index, calls);
    EXPECT_EQ(state.decision_period, schedule[calls]);
    EXPECT_EQ(state.execution_period, schedule[calls] + 1);
    EXPECT_EQ(state.decision_session_key, times[schedule[calls]]);
    EXPECT_EQ(state.execution_session_key, times[schedule[calls] + 1]);
    std::vector<book::ReplayTargetIntent> intents;
    for (atx::usize i = 0; i < state.preference_weights.size(); ++i) {
      EXPECT_EQ(state.preference_weights[i], preferences[calls * 2 + i]);
      intents.push_back({book::ReplayTargetAction::TargetWeight, state.preference_weights[i]});
    }
    ++calls;
    return atx::core::Ok(std::move(intents));
  };
  const auto fixed = book::replay_scheduled_targets(panel, times, schedule, preferences, cfg);
  const auto old = book::replay_scheduled_targets(panel, times, schedule, preferences, legacy, cfg);
  const auto intents = book::replay_scheduled_intents(
      panel, times, schedule, preferences, policy, cfg);
  ASSERT_TRUE(fixed.has_value()) << fixed.error().message();
  ASSERT_TRUE(old.has_value()) << old.error().message();
  ASSERT_TRUE(intents.has_value()) << intents.error().message();
  EXPECT_EQ(calls, 2U);
  const auto fields = [](const book::ReplayInterval &r) {
    return std::tuple(r.start_period, r.end_period, r.decision_period, r.pretrade_nav,
        r.cash, r.assets, r.nav, r.gross_pnl, r.trade_cost, r.borrow_cost, r.net_return,
        r.traded_dollars, r.start_gross, r.end_gross);
  };
  for (const auto *expected : {&*fixed, &old->replay}) {
    const auto &actual = intents->replay;
    ASSERT_EQ(actual.intervals.size(), expected->intervals.size());
    for (atx::usize i = 0; i < actual.intervals.size(); ++i) {
      EXPECT_EQ(fields(actual.intervals[i]), fields(expected->intervals[i]));
    }
    ASSERT_EQ(actual.trades.size(), expected->trades.size());
    for (atx::usize i = 0; i < actual.trades.size(); ++i) {
      const auto &a = actual.trades[i];
      const auto &b = expected->trades[i];
      EXPECT_EQ(std::tie(a.period, a.decision_period, a.instrument, a.dollar_delta),
                std::tie(b.period, b.decision_period, b.instrument, b.dollar_delta));
    }
    EXPECT_EQ(actual.final_tri_units, expected->final_tri_units);
    EXPECT_EQ(actual.initial_nav, expected->initial_nav);
    EXPECT_EQ(actual.final_cash, expected->final_cash);
    EXPECT_EQ(actual.final_assets, expected->final_assets);
    EXPECT_EQ(actual.final_nav, expected->final_nav);
    EXPECT_EQ(actual.effective_rebalances, expected->effective_rebalances);
    EXPECT_EQ(actual.unexecuted_decisions, expected->unexecuted_decisions);
  }
  ASSERT_EQ(old->allocations.size(), intents->allocations.size());
  for (atx::usize i = 0; i < old->allocations.size(); ++i) {
    EXPECT_TRUE(old->allocations[i].target_intents.empty());
    EXPECT_EQ(intents->allocations[i].target_intents.size(), 2U);
    EXPECT_EQ(intents->allocations[i].target_weights, old->allocations[i].target_weights);
    EXPECT_EQ(intents->allocations[i].posttrade_marked_dollars,
              old->allocations[i].posttrade_marked_dollars);
  }
}
