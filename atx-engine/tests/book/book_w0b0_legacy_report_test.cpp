// W0-B0 (B-03): the legacy weight report. LegacyReportRule::HoldingIntervalV2
// (the default for stage_report) charges each rebalance's book the TRI return
// compounded over its whole holding window, prices a held name that stops
// printing at its last print times (1 + Shumway fallback, flagged) instead of
// 0, and ReportBorrowAccrual::AnnualBySessionsV2 accrues an ANNUAL borrow rate
// over the sessions held instead of once per rebalance. OnePeriodV1 and
// FlatPerRebalanceV1 reproduce the pre-W0 arithmetic exactly.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/report.hpp"
#include "atx/engine/combine/gate.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/multi_period.hpp"

namespace atx_test_w0_b0_legacy_report {
namespace book = atx::engine::book;
namespace lib = atx::engine::library;
using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::alpha::Panel;
using atx::engine::risk::FactorModel;
using atx::engine::risk::MultiPeriodResult;
using atx::engine::risk::RebalanceSchedule;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// 11 sessions x 2 names. Name 0 compounds +1 %/session; name 1 is set per test.
std::vector<f64> closes(const std::vector<f64> &name1) {
  std::vector<f64> close;
  f64 p = 100.0;
  for (usize t = 0; t < name1.size(); ++t) {
    close.push_back(p);
    close.push_back(name1[t]);
    p *= 1.01;
  }
  return close;
}

const std::vector<f64> kSteady{50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60};

std::vector<std::vector<f64>> books_of(usize n, std::vector<f64> book) {
  return std::vector<std::vector<f64>>(n, std::move(book));
}

TEST(BookLegacyReport, HoldingSessionsFollowTheScheduleAndCapAtThePanelEnd) {
  const std::vector<usize> weekly{0, 5, 10};
  EXPECT_EQ(book::report_holding_sessions(weekly, 11), (std::vector<usize>{5, 5, 0}));
  const std::vector<usize> weekly_early{0, 5};
  EXPECT_EQ(book::report_holding_sessions(weekly_early, 11), (std::vector<usize>{5, 5}));
  const std::vector<usize> late{0, 5};
  EXPECT_EQ(book::report_holding_sessions(late, 8), (std::vector<usize>{5, 2}));
  const std::vector<usize> single{3};
  EXPECT_EQ(book::report_holding_sessions(single, 10), (std::vector<usize>{1}));
  const std::vector<usize> terminal{9};
  EXPECT_EQ(book::report_holding_sessions(terminal, 10), (std::vector<usize>{0}));
  const std::vector<usize> daily{0, 1, 2};
  EXPECT_EQ(book::report_holding_sessions(daily, 4), (std::vector<usize>{1, 1, 1}));
}

TEST(BookLegacyReport, WeeklyBookEarnsTheCompoundedWeekNotOneSession) {
  const auto close = closes(kSteady);
  const std::vector<usize> periods{0, 5};
  const auto books = books_of(2, {1.0, 0.0});
  const auto v2 = book::holding_interval_returns(close, 11, 2, periods, books,
                                                 book::LegacyReportRule::HoldingIntervalV2);
  ASSERT_TRUE(v2.has_value()) << v2.error().message();
  const auto v1 = book::holding_interval_returns(close, 11, 2, periods, books,
                                                 book::LegacyReportRule::OnePeriodV1);
  ASSERT_TRUE(v1.has_value()) << v1.error().message();
  const f64 week = close[5 * 2] / close[0] - 1.0; // 1.01^5 - 1
  EXPECT_NEAR(week, std::pow(1.01, 5) - 1.0, 1.0e-12);
  EXPECT_DOUBLE_EQ(v2->returns[0], week);
  EXPECT_DOUBLE_EQ(v2->returns[2], close[10 * 2] / close[5 * 2] - 1.0);
  EXPECT_NEAR(v1->returns[0], 0.01, 1.0e-12); // Pre-W0: one session of a five-session hold.
  EXPECT_EQ(v2->holding_sessions, (std::vector<usize>{5, 5}));
  EXPECT_EQ(v1->holding_sessions, (std::vector<usize>{1, 1}));
  std::printf("[measured] weekly book, name 0: V2 %.6f vs V1 %.6f per rebalance\n",
              v2->returns[0], v1->returns[0]);
}

TEST(BookLegacyReport, DelistedHeldNameTakesTheShumwayReturnNotZero) {
  // Name 1 prints through session 5 and never again.
  std::vector<f64> name1{50, 51, 52, 53, 54, 55, kNaN, kNaN, kNaN, kNaN, kNaN};
  const auto close = closes(name1);
  const std::vector<usize> periods{0, 5};
  for (const f64 side : {0.4, -0.4}) {
    const auto books = books_of(2, {0.6, side});
    const auto v2 = book::holding_interval_returns(close, 11, 2, periods, books,
                                                   book::LegacyReportRule::HoldingIntervalV2);
    ASSERT_TRUE(v2.has_value()) << v2.error().message();
    const f64 expected = side > 0.0 ? -0.55 : -0.30; // Unknown venue: adverse for the side.
    EXPECT_NEAR(v2->returns[1 * 2 + 1], expected, 1.0e-15);
    ASSERT_EQ(v2->terminals.size(), 1U);
    EXPECT_EQ(v2->terminals[0].schedule_index, 1U);
    EXPECT_EQ(v2->terminals[0].instrument, 1U);
    EXPECT_EQ(v2->terminals[0].last_valid_date, 5U);
    EXPECT_EQ(v2->terminals[0].source, book::TerminalReturnSource::ShumwayUnknownAdverse);
    // Pre-W0: the missing forward close is NaN, which the report reads as 0.
    const auto v1 = book::holding_interval_returns(close, 11, 2, periods, books,
                                                   book::LegacyReportRule::OnePeriodV1);
    ASSERT_TRUE(v1.has_value());
    EXPECT_TRUE(std::isnan(v1->returns[1 * 2 + 1]));
    EXPECT_TRUE(v1->terminals.empty());
  }
  // A known venue picks its own Shumway number whatever the side.
  const std::vector<book::ListingExchange> venues{book::ListingExchange::NyseAmex,
                                                  book::ListingExchange::Nasdaq};
  const auto nasdaq = book::holding_interval_returns(
      close, 11, 2, periods, books_of(2, {0.6, -0.4}), book::LegacyReportRule::HoldingIntervalV2,
      venues);
  ASSERT_TRUE(nasdaq.has_value());
  EXPECT_NEAR(nasdaq->returns[3], -0.55, 1.0e-15);
  EXPECT_EQ(nasdaq->terminals.at(0).source, book::TerminalReturnSource::ShumwayNasdaq);
}

TEST(BookLegacyReport, DelistingInsideTheWindowCompoundsToTheLastPrint) {
  // Name 1 prints 55 at session 5 and 60.5 at session 7, then never again.
  std::vector<f64> name1{50, 51, 52, 53, 54, 55, 58, 60.5, kNaN, kNaN, kNaN};
  const auto close = closes(name1);
  const std::vector<usize> periods{0, 5};
  const auto v2 = book::holding_interval_returns(close, 11, 2, periods, books_of(2, {0.5, 0.5}),
                                                 book::LegacyReportRule::HoldingIntervalV2);
  ASSERT_TRUE(v2.has_value());
  EXPECT_NEAR(v2->returns[3], (60.5 / 55.0) * (1.0 - 0.55) - 1.0, 1.0e-15);
  EXPECT_EQ(v2->terminals.at(0).last_valid_date, 7U);
}

TEST(BookLegacyReport, InteriorGapMarksAtTheLastPrintAndIsNotADelisting) {
  // 12 sessions: name 1 misses sessions 9-10 but prints again at 11.
  std::vector<f64> name1{50, 51, 52, 53, 54, 55, 56, 57, 58, kNaN, kNaN, 61};
  const auto close = closes(name1);
  const std::vector<usize> periods{0, 5};
  const auto v2 = book::holding_interval_returns(close, 12, 2, periods, books_of(2, {0.5, 0.5}),
                                                 book::LegacyReportRule::HoldingIntervalV2);
  ASSERT_TRUE(v2.has_value());
  EXPECT_NEAR(v2->returns[3], 58.0 / 55.0 - 1.0, 1.0e-15);
  EXPECT_TRUE(v2->terminals.empty());
  EXPECT_EQ(v2->gap_marks, 1U);
}

TEST(BookLegacyReport, OnePeriodV1ReproducesThePreW0ArithmeticBitForBit) {
  std::vector<f64> name1{50, 0.0, 52, kNaN, 54, -1.0, 56, 57, 58, 59, 60};
  const auto close = closes(name1);
  const std::vector<usize> periods{0, 1, 2, 3, 4, 5, 9, 10};
  const auto v1 = book::holding_interval_returns(close, 11, 2, periods,
                                                 books_of(periods.size(), {0.5, 0.5}),
                                                 book::LegacyReportRule::OnePeriodV1);
  ASSERT_TRUE(v1.has_value());
  for (usize s = 0; s < periods.size(); ++s) {
    for (usize i = 0; i < 2; ++i) {
      const usize d = periods[s];
      f64 old = kNaN; // The pre-W0 stage_report loop, verbatim.
      if (d + 1 < 11) {
        const f64 p0 = close[d * 2 + i];
        const f64 p1 = close[(d + 1) * 2 + i];
        if (!std::isnan(p0) && !std::isnan(p1) && p0 != 0.0) old = p1 / p0 - 1.0;
      }
      const f64 now = v1->returns[s * 2 + i];
      EXPECT_TRUE((std::isnan(old) && std::isnan(now)) || old == now) << s << "," << i;
    }
  }
}

TEST(BookLegacyReport, InputsAreValidated) {
  const auto close = closes(kSteady);
  const std::vector<usize> periods{0, 5};
  const auto books = books_of(2, {0.5, 0.5});
  const auto rule = book::LegacyReportRule::HoldingIntervalV2;
  EXPECT_FALSE(book::holding_interval_returns(close, 10, 2, periods, books, rule));
  EXPECT_FALSE(book::holding_interval_returns(close, 11, 2, periods, books_of(1, {0.5, 0.5}),
                                              rule));
  EXPECT_FALSE(book::holding_interval_returns(close, 11, 2, periods, books_of(2, {0.5}), rule));
  const std::vector<usize> late{0, 11};
  EXPECT_FALSE(book::holding_interval_returns(close, 11, 2, late, books, rule));
  EXPECT_FALSE(book::holding_interval_returns(close, 11, 2, periods, books,
                                              static_cast<book::LegacyReportRule>(0)));
  const std::vector<book::ListingExchange> one{book::ListingExchange::Nasdaq};
  EXPECT_FALSE(book::holding_interval_returns(close, 11, 2, periods, books, rule, one));
}

// ---------------------------------------------------------------------------
// accumulate_report: borrow accrual and the delisted return reaching pnl.
// ---------------------------------------------------------------------------
std::string tmpdir(const std::string &tag) {
  const auto dir = std::filesystem::temp_directory_path() / "atx_w0b0_legacy_report" / tag;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}

FactorModel model_2x1() {
  MatX x(2, 1);
  x << 1.0, 1.0;
  MatX f(1, 1);
  f << 0.04;
  VecX d(2);
  d << 0.10, 0.10;
  auto created = FactorModel::create(x, f, d, 0U, 2U);
  EXPECT_TRUE(created.has_value());
  return std::move(*created);
}

TEST(BookLegacyReport, BorrowAccruesAnAnnualRateOverTheSessionsHeld) {
  // Zero returns isolate the borrow term. Weekly schedule {0, 5} on 11 sessions.
  const auto panel = Panel::create(11, 2, {"ret"}, {std::vector<f64>(22, 0.0)}, {}).value();
  const auto ret = panel.field_id("ret").value();
  MultiPeriodResult books;
  books.books = {{0.5, -0.5}, {0.5, -0.5}};
  books.turnover = {0.0, 0.0};
  books.cost_bps = {0.0, 0.0};
  const RebalanceSchedule sched{std::vector<usize>{0U, 5U}};
  const auto V = model_2x1();
  auto library = lib::Library::open(tmpdir("borrow"), atx::engine::combine::GateConfig{},
                                    std::vector<atx::u64>{7ULL});
  book::ReportAccrual annual;
  annual.borrow_bps = 252.0; // 2.52 % a year on the short notional.
  const auto v2 = book::accumulate_report(books, panel, ret, sched, V, 1.0, library, 0U, annual);
  ASSERT_TRUE(v2.has_value()) << v2.error().message();
  book::ReportAccrual flat = annual;
  flat.borrow_rule = book::ReportBorrowAccrual::FlatPerRebalanceV1;
  const auto v1 = book::accumulate_report(books, panel, ret, sched, V, 1.0, library, 0U, flat);
  ASSERT_TRUE(v1.has_value()) << v1.error().message();
  // V2: 0.5 * 2.52 % * 5 / 252 per week; V1 charged a full year per rebalance.
  for (usize s = 0; s < 2; ++s) {
    EXPECT_NEAR(v2->pnl_borrow[s], 0.5 * 252.0e-4 * 5.0 / 252.0, 1.0e-15);
    EXPECT_NEAR(v1->pnl_borrow[s], 0.5 * 252.0e-4, 1.0e-15);
  }
  EXPECT_EQ(v2->holding_sessions, (std::vector<usize>{5, 5}));
  // The scalar-rate overload is the corrected default.
  const auto scalar = book::accumulate_report(books, panel, ret, sched, V, 1.0, library, 0U, 252.0);
  ASSERT_TRUE(scalar.has_value());
  EXPECT_EQ(scalar->pnl_borrow, v2->pnl_borrow);
  std::printf("[measured] weekly 0.5 short @252bps/yr: V2 %.3e vs V1 %.3e per rebalance\n",
              v2->pnl_borrow[0], v1->pnl_borrow[0]);
  // Invalid accruals are rejected.
  auto bad = annual;
  bad.sessions_per_year = 0.0;
  EXPECT_FALSE(book::accumulate_report(books, panel, ret, sched, V, 1.0, library, 0U, bad));
  bad = annual;
  bad.borrow_bps = -1.0;
  EXPECT_FALSE(book::accumulate_report(books, panel, ret, sched, V, 1.0, library, 0U, bad));
  bad = annual;
  const std::vector<usize> wrong{5};
  bad.holding_sessions = wrong;
  EXPECT_FALSE(book::accumulate_report(books, panel, ret, sched, V, 1.0, library, 0U, bad));
  bad = annual;
  bad.borrow_rule = static_cast<book::ReportBorrowAccrual>(0);
  EXPECT_FALSE(book::accumulate_report(books, panel, ret, sched, V, 1.0, library, 0U, bad));
}

TEST(BookLegacyReport, DelistedReturnReachesThePnlThroughARowPanel) {
  std::vector<f64> name1{50, 51, 52, 53, 54, 55, kNaN, kNaN, kNaN, kNaN, kNaN};
  const auto close = closes(name1);
  const std::vector<usize> periods{0, 5};
  MultiPeriodResult books;
  books.books = {{0.6, 0.4}, {0.6, 0.4}};
  books.turnover = {0.0, 0.0};
  books.cost_bps = {0.0, 0.0};
  const auto held = book::holding_interval_returns(close, 11, 2, periods, books.books,
                                                   book::LegacyReportRule::HoldingIntervalV2);
  ASSERT_TRUE(held.has_value());
  const auto rows = Panel::create(2, 2, {"ret"}, {held->returns}, {}).value();
  const RebalanceSchedule row_sched{std::vector<usize>{0U, 1U}};
  auto library = lib::Library::open(tmpdir("pnl"), atx::engine::combine::GateConfig{},
                                    std::vector<atx::u64>{7ULL});
  book::ReportAccrual accrual;
  accrual.holding_sessions = held->holding_sessions;
  const auto rep = book::accumulate_report(books, rows, rows.field_id("ret").value(), row_sched,
                                           model_2x1(), 1.0, library, 0U, accrual);
  ASSERT_TRUE(rep.has_value()) << rep.error().message();
  const f64 name0_week = close[10 * 2] / close[5 * 2] - 1.0;
  EXPECT_NEAR(rep->pnl_gross[1], 0.6 * name0_week + 0.4 * -0.55, 1.0e-15);
  EXPECT_LT(rep->pnl_gross[1], 0.6 * name0_week); // Pre-W0 would have added 0.
  std::printf("[measured] delisting week pnl_gross: V2 %.6f vs survivor-only %.6f\n",
              rep->pnl_gross[1], 0.6 * name0_week);
}

} // namespace atx_test_w0_b0_legacy_report
