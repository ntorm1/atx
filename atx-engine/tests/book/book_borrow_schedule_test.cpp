#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/borrow_schedule.hpp"
#include "atx/engine/book/replay.hpp"

namespace atx_test_l8_e2e_borrow_schedule {
namespace book = atx::engine::book;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr atx::f64 kInf = std::numeric_limits<atx::f64>::infinity();

inline Panel flat_panel(atx::usize dates, atx::usize instruments) {
  return Panel::create(dates, instruments, {"close"},
                       {std::vector<atx::f64>(dates * instruments, 100.0)}, {})
      .value();
}

inline std::vector<atx::i64> day_axis(atx::usize count) {
  std::vector<atx::i64> times;
  for (atx::usize i = 0; i < count; ++i) times.push_back(static_cast<atx::i64>(i) * kDay);
  return times;
}

inline book::ReplayConfig immediate() {
  book::ReplayConfig cfg;
  cfg.initial_nav = 1000.0;
  cfg.execution_delay_periods = 0;
  cfg.allow_same_close = true; // B-02: same-close fills are an explicit opt-in.
  cfg.borrow_day_basis = book::ReplayDayBasis::D360;
  // B-04: these cases pin the pre-W0 locate rejection (now LocateBreach::AbortV1).
  cfg.locate_breach = book::LocateBreach::AbortV1;
  return cfg;
}

inline book::BorrowSchedule schedule(atx::usize dates, atx::usize instruments) {
  book::BorrowSchedule s;
  s.dates = dates;
  s.instruments = instruments;
  // B-05: these cases pin the pre-W0 fee-and-rebate financing formula.
  s.financing = book::ShortFinancing::FeeAndRebateV1;
  return s;
}


TEST(BookBorrowSchedule, ShortWithoutLocateIsRejected) {
  const auto panel = flat_panel(4, 2);
  auto borrow = schedule(4, 2);
  borrow.locate_dollars.assign(8, kInf);
  borrow.locate_dollars[0 * 2 + 1] = 0.0; // Name 1 is hard to borrow at period 0.
  auto cfg = immediate();
  cfg.borrow_schedule = &borrow;
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{-0.2, -0.2};
  const auto result = book::replay_scheduled_targets(panel, day_axis(4), decisions, targets, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message().find("locate"), std::string::npos);
  EXPECT_NE(result.error().message().find("instrument=1"), std::string::npos);
}

TEST(BookBorrowSchedule, ShortWithinLocateIsAcceptedAndPartialLocateIsBinding) {
  const auto panel = flat_panel(4, 1);
  auto borrow = schedule(4, 1);
  borrow.locate_dollars.assign(4, 150.0);
  auto cfg = immediate();
  cfg.borrow_schedule = &borrow;
  const std::vector<atx::usize> decisions{0};
  EXPECT_TRUE(book::replay_scheduled_targets(panel, day_axis(4), decisions,
                                             std::vector<atx::f64>{-0.15}, cfg)
                  .has_value());
  EXPECT_FALSE(book::replay_scheduled_targets(panel, day_axis(4), decisions,
                                              std::vector<atx::f64>{-0.2}, cfg)
                   .has_value());
}

TEST(BookBorrowSchedule, VanishedLocateNeverForcesACoverOfAnExistingShort) {
  const auto panel = flat_panel(4, 1);
  auto borrow = schedule(4, 1);
  borrow.locate_dollars = {kInf, 0.0, 0.0, 0.0};
  auto cfg = immediate();
  cfg.borrow_schedule = &borrow;
  const std::vector<atx::usize> decisions{0, 1, 2};
  const std::vector<atx::f64> targets{-0.3, -0.3, -0.1}; // Carry, then shrink.
  const auto result = book::replay_scheduled_targets(panel, day_axis(4), decisions, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_DOUBLE_EQ(result->final_tri_units[0], -1.0);
}

TEST(BookBorrowSchedule, PerNameFeesRebateAndCashRateAreChargedOnTheDayBasis) {
  const auto panel = flat_panel(3, 2);
  auto borrow = schedule(3, 2);
  borrow.fee_bps = {360.0, 3600.0, 360.0, 3600.0, 360.0, 3600.0};
  borrow.rebate_bps = 36.0;
  borrow.cash_bps = 72.0;
  auto cfg = immediate();
  cfg.borrow_schedule = &borrow;
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{-0.1, -0.2}; // 100$ and 200$ short.
  const auto result = book::replay_scheduled_targets(panel, day_axis(3), decisions, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  // One day on D360: fee (100*360 + 200*3600) bps, rebate 300*36 on the short
  // proceeds, cash rate only on FREE cash 1300 - 300 = 1000 (no double count).
  const auto expected = (100.0 * 360.0 + 200.0 * 3600.0 - 300.0 * 36.0 - 1000.0 * 72.0) *
                        1.0e-4 / 360.0;
  EXPECT_NEAR(result->intervals[0].borrow_cost, expected, 1.0e-12);
  for (const auto &row : result->intervals) {
    EXPECT_NEAR(row.pretrade_nav + row.gross_pnl - row.trade_cost - row.borrow_cost, row.nav,
                1.0e-12);
  }
}

TEST(BookBorrowSchedule, CashRichBookEarnsNegativeFinancingCost) {
  const auto panel = flat_panel(3, 1);
  auto borrow = schedule(3, 1);
  borrow.cash_bps = 360.0;
  auto cfg = immediate();
  cfg.borrow_schedule = &borrow;
  const std::vector<atx::usize> decisions{0};
  const auto result = book::replay_scheduled_targets(panel, day_axis(3), decisions,
                                                     std::vector<atx::f64>{0.0}, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_NEAR(result->intervals[0].borrow_cost, -1000.0 * 0.036 / 360.0, 1.0e-12);
  EXPECT_GT(result->final_nav, 1000.0);
}

TEST(BookBorrowSchedule, ScheduleValidationAndExclusivity) {
  const auto panel = flat_panel(3, 1);
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{-0.1};
  auto misshaped = schedule(2, 1);
  auto cfg = immediate();
  cfg.borrow_schedule = &misshaped;
  EXPECT_FALSE(book::replay_scheduled_targets(panel, day_axis(3), decisions, targets, cfg));

  auto negative_fee = schedule(3, 1);
  negative_fee.fee_bps = {1.0, -1.0, 1.0};
  cfg.borrow_schedule = &negative_fee;
  EXPECT_FALSE(book::replay_scheduled_targets(panel, day_axis(3), decisions, targets, cfg));

  auto good = schedule(3, 1);
  cfg.borrow_schedule = &good;
  cfg.annual_borrow_bps = 10.0;
  EXPECT_FALSE(book::replay_scheduled_targets(panel, day_axis(3), decisions, targets, cfg));
}

TEST(BookBorrowSchedule, DollarNeutralBookEarnsThePolicyRateOnlyOnceOnNav) {
  // Rebate == cash rate == r and no fees: short proceeds earn the rebate, free
  // cash earns r, so the whole book earns r on NAV (1000), not on NAV + shorts.
  const auto panel = flat_panel(3, 2);
  auto borrow = schedule(3, 2);
  borrow.rebate_bps = 360.0;
  borrow.cash_bps = 360.0;
  auto cfg = immediate();
  cfg.borrow_schedule = &borrow;
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{0.5, -0.5};
  const auto result = book::replay_scheduled_targets(panel, day_axis(3), decisions, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_NEAR(result->intervals[0].borrow_cost, -1000.0 * 0.036 / 360.0, 1.0e-12);
}

TEST(BookBorrowSchedule, NegativeNetRebate_ChargesHardToBorrowShortOnceAcrossWeekend) {
  const auto panel = flat_panel(2, 2);
  auto borrow = schedule(2, 2);
  borrow.financing = book::ShortFinancing::FeeOnceV2;
  borrow.rebate_bps = -720.0;
  borrow.cash_bps = 360.0;
  auto cfg = immediate();
  cfg.borrow_schedule = &borrow;
  const std::vector<atx::i64> times{0, 3 * kDay};
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{0.5, -0.5};
  const auto result = book::replay_scheduled_targets(panel, times, decisions, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(result->intervals.size(), 1U);
  // $500 short pays 7.2%; the remaining $500 free cash earns 3.6%.
  const auto expected = (500.0 * 0.072 - 500.0 * 0.036) * 3.0 / 360.0;
  EXPECT_NEAR(result->intervals.front().borrow_cost, expected, 1e-12);
  EXPECT_NEAR(result->final_cash, 1000.0 - expected, 1e-12);
  EXPECT_NEAR(result->final_nav, 1000.0 - expected, 1e-12);
  borrow.default_fee_bps = 1.0;
  EXPECT_FALSE(book::replay_scheduled_targets(panel, times, decisions, targets, cfg));
}

TEST(BookBorrowSchedule, ExplicitZeroNetRebateWithholdsProceedsInterestAndPreservesLegacyQuotes) {
  const auto panel = flat_panel(2, 2);
  auto borrow = schedule(2, 2);
  borrow.financing = book::ShortFinancing::FeeOnceV2;
  borrow.cash_bps = 360.0;
  auto cfg = immediate();
  cfg.borrow_schedule = &borrow;
  const std::vector<atx::i64> times{0, 3 * kDay};
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{0.5, -0.5};
  const auto run = [&] {
    return book::replay_scheduled_targets(panel, times, decisions, targets, cfg);
  };
  const auto legacy_fee = run();
  borrow.quote_kind = book::BorrowQuoteKind::FeeV2;
  const auto fee = run();
  borrow.quote_kind = book::BorrowQuoteKind::NetRebateV2;
  const auto zero_rebate = run();
  ASSERT_TRUE(legacy_fee.has_value());
  ASSERT_TRUE(fee.has_value());
  ASSERT_TRUE(zero_rebate.has_value()) << zero_rebate.error().message();
  EXPECT_EQ(fee->final_nav, legacy_fee->final_nav);
  EXPECT_NEAR(fee->intervals[0].borrow_cost, -1000.0 * 0.036 * 3.0 / 360.0, 1e-12);
  EXPECT_NEAR(zero_rebate->intervals[0].borrow_cost, -500.0 * 0.036 * 3.0 / 360.0, 1e-12);
  EXPECT_NEAR(zero_rebate->final_nav, 1000.15, 1e-12);

  borrow.rebate_bps = -720.0;
  const auto explicit_negative = run();
  borrow.quote_kind = book::BorrowQuoteKind::LegacyInferV1;
  const auto legacy_negative = run();
  ASSERT_TRUE(explicit_negative.has_value());
  ASSERT_TRUE(legacy_negative.has_value());
  EXPECT_EQ(explicit_negative->final_nav, legacy_negative->final_nav);
  borrow.quote_kind = book::BorrowQuoteKind::FeeV2;
  EXPECT_FALSE(run()); // A net rebate cannot be supplied as a fee quote.
  borrow.rebate_bps = 0.0;
  borrow.quote_kind = book::BorrowQuoteKind::NetRebateV2;
  borrow.default_fee_bps = 1.0;
  EXPECT_FALSE(run()); // Even a zero rebate excludes a separately charged fee.
  borrow.default_fee_bps = 0.0;
  borrow.financing = book::ShortFinancing::FeeAndRebateV1;
  EXPECT_FALSE(run()); // Explicit quote interpretation cannot be silently ignored.
  borrow.quote_kind = static_cast<book::BorrowQuoteKind>(255);
  EXPECT_FALSE(run());
}

} // namespace atx_test_l8_e2e_borrow_schedule
