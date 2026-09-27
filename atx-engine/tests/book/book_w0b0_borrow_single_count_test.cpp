// W0-B0 (B-05): the borrow fee is counted once. A quoted rebate is the policy
// rate LESS the borrow fee, so charging the fee and also crediting the rebate
// (ShortFinancing::FeeAndRebateV1, pre-W0) charges the fee twice. FeeOnceV2
// (the default) takes a schedule quoted either as fees (the proceeds then earn
// the cash rate like any cash) or as a rebate (no separate fee), and rejects a
// schedule that quotes both.

#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/borrow_schedule.hpp"
#include "atx/engine/book/replay.hpp"

namespace atx_test_w0_b0_borrow_single_count {
namespace book = atx::engine::book;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;

Panel flat(atx::usize dates, atx::usize names) {
  return Panel::create(dates, names, {"close"},
                       {std::vector<atx::f64>(dates * names, 100.0)}, {})
      .value();
}

std::vector<atx::i64> day_axis(atx::usize count) {
  std::vector<atx::i64> keys;
  for (atx::usize i = 0; i < count; ++i) keys.push_back(static_cast<atx::i64>(i) * kDay);
  return keys;
}

book::BorrowSchedule schedule(atx::usize dates, atx::usize names) {
  book::BorrowSchedule s;
  s.dates = dates;
  s.instruments = names;
  return s;
}

// NAV ~1000 on D360; delay 1: execution at period 1, one day of financing on
// the interval 1 -> 2. Book: 10 % of NAV short of name 0 and 20 % of name 1, so
// settled cash after the trade is ~1300$ and the short dollars are ~300$.
atx::core::Result<book::ReplayResult> run(const book::BorrowSchedule &borrow) {
  book::ReplayConfig cfg;
  cfg.initial_nav = 1000.0;
  cfg.borrow_day_basis = book::ReplayDayBasis::D360;
  cfg.borrow_schedule = &borrow;
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{-0.1, -0.2};
  return book::replay_scheduled_targets(flat(3, 2), day_axis(3), decisions, targets, cfg);
}

constexpr atx::f64 kPerDay = 1.0e-4 / 360.0; // bps-dollars -> dollars for one D360 day.

// The financed book of interval 1, from its own start: interval 0 (all cash)
// accrues the cash rate, so the execution NAV is not exactly 1000.
struct Financed {
  atx::f64 s0, s1, shorts, cash;
};
Financed financed(const book::ReplayInterval &row) {
  const atx::f64 s0 = 0.1 * row.pretrade_nav;
  const atx::f64 s1 = 0.2 * row.pretrade_nav;
  return {s0, s1, s0 + s1, row.pretrade_nav + s0 + s1}; // cash = NAV + short proceeds.
}

TEST(BookBorrowSingleCount, DefaultIsFeeOnce) {
  const book::BorrowSchedule s;
  EXPECT_EQ(s.financing, book::ShortFinancing::FeeOnceV2);
}

TEST(BookBorrowSingleCount, FeeQuotedScheduleChargesTheFeeOnceAndProceedsEarnCash) {
  auto borrow = schedule(3, 2);
  borrow.fee_bps.assign(6, 0.0);
  for (atx::usize t = 0; t < 3; ++t) {
    borrow.fee_bps[t * 2 + 0] = 360.0;
    borrow.fee_bps[t * 2 + 1] = 3600.0;
  }
  borrow.cash_bps = 72.0;
  const auto result = run(borrow);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  // Fees on both shorts; ALL settled cash (proceeds included) earns 72.
  const auto b = financed(result->intervals[1]);
  const auto expected = (b.s0 * 360.0 + b.s1 * 3600.0 - b.cash * 72.0) * kPerDay;
  EXPECT_NEAR(result->intervals[1].borrow_cost, expected, 1.0e-12);
  // Pre-W0 withheld the cash rate from the ~300$ proceeds (no rebate quoted).
  borrow.financing = book::ShortFinancing::FeeAndRebateV1;
  const auto legacy = run(borrow);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().message();
  EXPECT_NEAR(legacy->intervals[1].borrow_cost - result->intervals[1].borrow_cost,
              b.shorts * 72.0 * kPerDay, 1.0e-12);
  std::printf("[measured] fee-quoted 1-day financing: V2=%.10f V1=%.10f\n",
              result->intervals[1].borrow_cost, legacy->intervals[1].borrow_cost);
}

TEST(BookBorrowSingleCount, RebateQuotedScheduleNetsTheFeeOnceAndMatchesV1) {
  auto borrow = schedule(3, 2);
  borrow.rebate_bps = 36.0; // policy 72 less a 36 bps fee, already netted.
  borrow.cash_bps = 72.0;
  const auto result = run(borrow);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  const auto b = financed(result->intervals[1]);
  const auto expected = (-b.shorts * 36.0 - (b.cash - b.shorts) * 72.0) * kPerDay;
  EXPECT_NEAR(result->intervals[1].borrow_cost, expected, 1.0e-12);
  borrow.financing = book::ShortFinancing::FeeAndRebateV1;
  const auto legacy = run(borrow);
  ASSERT_TRUE(legacy.has_value());
  EXPECT_EQ(legacy->intervals[1].borrow_cost, result->intervals[1].borrow_cost);
}

TEST(BookBorrowSingleCount, FeeAndRebateTogetherDoubleCountUnderV1AndAreRejectedUnderV2) {
  // Policy rate 72, borrow fee 36 on both names, so the matching rebate is
  // 72 - 36 = 36. Economically the short leg costs its 36 bps fee exactly once.
  auto both = schedule(3, 2);
  both.default_fee_bps = 36.0;
  both.rebate_bps = 36.0;
  both.cash_bps = 72.0;
  const auto rejected = run(both);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_NE(rejected.error().message().find("rebate"), std::string::npos);

  both.financing = book::ShortFinancing::FeeAndRebateV1;
  const auto doubled = run(both);
  ASSERT_TRUE(doubled.has_value()) << doubled.error().message();
  auto fee_once = schedule(3, 2); // The same economics quoted as a fee (V2).
  fee_once.default_fee_bps = 36.0;
  fee_once.cash_bps = 72.0;
  const auto once = run(fee_once);
  ASSERT_TRUE(once.has_value()) << once.error().message();
  auto rebate_once = schedule(3, 2); // ... and quoted as a rebate (V2).
  rebate_once.rebate_bps = 36.0;
  rebate_once.cash_bps = 72.0;
  const auto net = run(rebate_once);
  ASSERT_TRUE(net.has_value()) << net.error().message();
  // Both single-count quotes agree to the cent-fraction...
  EXPECT_NEAR(once->intervals[1].borrow_cost, net->intervals[1].borrow_cost, 1.0e-12);
  // ...and V1 charges exactly one extra fee: short dollars * fee.
  EXPECT_NEAR(doubled->intervals[1].borrow_cost - once->intervals[1].borrow_cost,
              financed(once->intervals[1]).shorts * 36.0 * kPerDay, 1.0e-12);
  std::printf("[measured] 1-day financing: V1 fee+rebate=%.10f V2 fee=%.10f V2 rebate=%.10f\n",
              doubled->intervals[1].borrow_cost, once->intervals[1].borrow_cost,
              net->intervals[1].borrow_cost);
}

TEST(BookBorrowSingleCount, FinancingRuleIsValidated) {
  auto bad = schedule(3, 2);
  bad.financing = static_cast<book::ShortFinancing>(7);
  EXPECT_FALSE(run(bad).has_value());
  // A zero-fee, zero-rebate schedule is valid under both rules.
  auto none = schedule(3, 2);
  EXPECT_TRUE(run(none).has_value());
  none.fee_bps.assign(6, 0.0);
  none.rebate_bps = 10.0;
  EXPECT_TRUE(run(none).has_value()); // An all-zero grid quotes no fee.
}

} // namespace atx_test_w0_b0_borrow_single_count
