// atx::engine::cost — Borrow accrual test suite (S6-5).
//
// Short-borrow / financing accrual (cost/borrow.hpp) completes the net-cost
// picture: it charges daily interest on the SHORT notional only and debits the
// charge ADDITIVELY from the portfolio's cash via Portfolio::accrue_financing
// (the ONE reviewed engine touch of the S6 sprint — apply_fill asserts qty != 0,
// so a borrow charge cannot ride it; the S6-0 finding).
//
// The three load-bearing behaviours:
//   C4 (hand arithmetic)   the daily charge matches short_notional·rate/denom.
//   §0.7 (additive debit)  accrue_borrow lowers cash by EXACTLY the charge and
//                          leaves the short position untouched (no fill path).
//   dollar-neutral edge    a long-only book has no short notional ⇒ zero borrow.
//
// A short is opened the only legal way (apply_fill of a SELL, qty < 0, price > 0,
// fee = 0); the Market is priced at that mark so mkt.mark(id) returns it. The
// universe array is kept alive for the duration of the non-owning spans.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <span>

#include "atx/core/datetime.hpp" // Timestamp (fill stamp)
#include "atx/core/decimal.hpp"  // Decimal
#include "atx/core/domain/domain.hpp" // Bar, Price, Quantity (slice pricing)
#include "atx/core/types.hpp"    // f64, i64, u32

#include "atx/engine/cost/borrow.hpp"        // BorrowModel, daily_borrow, accrue_borrow, DayCount
#include "atx/engine/exec/payloads.hpp"      // FillPayload
#include "atx/engine/loop/market.hpp"        // Market, InstrumentStats
#include "atx/engine/loop/panel_types.hpp"   // MarketSlice, SliceRow
#include "atx/engine/loop/types.hpp"         // InstrumentId
#include "atx/engine/portfolio/portfolio.hpp" // Portfolio

namespace atxtest_borrow_test {

using atx::f64;
using atx::i64;
using atx::u32;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::time::Timestamp;
using atx::core::time::Duration;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::MarketSlice;
using atx::engine::Portfolio;
using atx::engine::SliceRow;
using atx::engine::exec::FillPayload;
namespace cost = atx::engine::cost;

[[nodiscard]] InstrumentId inst(u32 id) noexcept { return InstrumentId{id}; }

// One sealed bar at `close` price with ample volume (volume is irrelevant to
// borrow — only the mark is read). Integer price so the mark is exact.
[[nodiscard]] Bar bar(i64 t, i64 close) noexcept {
  return Bar{Timestamp::from_unix_nanos(t), Price::from_int(close), Price::from_int(close),
             Price::from_int(close),        Price::from_int(close), Quantity::from_int(1'000'000)};
}

// Price a one-name Market at `mark`, so mkt.mark(id) == mark.
void price_at(Market& mkt, std::span<const InstrumentId> uni, u32 id, i64 mark) {
  const std::array<SliceRow, 1> rows{SliceRow{inst(id), bar(/*t=*/1, mark), false}};
  (void)uni;
  mkt.update_prices(MarketSlice{Timestamp::from_unix_nanos(1), std::span<const SliceRow>{rows}});
}

// f64→Decimal at the ledger grid — the SAME conversion daily_borrow performs, so
// the hand-computed expected matches the function's own rounding exactly.
[[nodiscard]] Decimal money(f64 v) { return Decimal::from_double(v).value_or(Decimal{}); }

// =============================================================================
//  Suite: Borrow
// =============================================================================

// C4 — the daily charge matches the hand arithmetic. A short of |notional|
// 2,000,000 (short 20,000 sh @ mark 100) at 5%/yr on a 360 basis costs
// 2,000,000 · 0.05 / 360 ≈ 277.78 / day.
TEST(Borrow, DailyAccrual_MatchesHandArithmetic) {
  const std::array<InstrumentId, 1> uni{inst(1)};
  const std::array<InstrumentStats, 1> stats{InstrumentStats{}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{stats}};
  price_at(mkt, uni, /*id=*/1, /*mark=*/100);

  Portfolio pf{Decimal::from_int(10'000'000), std::span<const InstrumentId>{uni}};
  pf.apply_fill(FillPayload{inst(1), /*qty=*/-20'000, Decimal::from_int(100), Decimal{}, 0.0,
                            Timestamp::from_unix_nanos(1)}); // open a 20k short @ 100

  const cost::BorrowModel b{/*annual_rate=*/0.05, cost::DayCount::D360};
  const Decimal fee = cost::daily_borrow(b, pf, mkt, std::span<const InstrumentId>{uni});
  EXPECT_TRUE(fee == money(2'000'000.0 * 0.05 / 360.0)); // ~277.78 / day
}

// §0.7 — accrue_borrow debits cash by EXACTLY the daily charge and changes no
// position (it rides accrue_financing, not the fill path).
TEST(Borrow, AccrueDebitsCashByBorrow_NoPositionChange) {
  const std::array<InstrumentId, 1> uni{inst(1)};
  const std::array<InstrumentStats, 1> stats{InstrumentStats{}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{stats}};
  price_at(mkt, uni, /*id=*/1, /*mark=*/100);

  Portfolio pf{Decimal::from_int(10'000'000), std::span<const InstrumentId>{uni}};
  pf.apply_fill(FillPayload{inst(1), /*qty=*/-20'000, Decimal::from_int(100), Decimal{}, 0.0,
                            Timestamp::from_unix_nanos(1)});

  const cost::BorrowModel b{/*annual_rate=*/0.05, cost::DayCount::D360};
  const Decimal cash0 = pf.cash();
  const Decimal expected = cost::daily_borrow(b, pf, mkt, std::span<const InstrumentId>{uni});

  cost::accrue_borrow(b, pf, mkt, std::span<const InstrumentId>{uni});

  EXPECT_TRUE(cash0 - pf.cash() == expected);          // exact additive debit
  EXPECT_EQ(pf.holding(inst(1)).qty, -20'000);         // short unchanged (no position change)
}

// Dollar-neutral edge — a long-only book has no short notional, so it pays no
// borrow regardless of rate or day count.
TEST(Borrow, LongOnlyBook_NoBorrowCharge) {
  const std::array<InstrumentId, 1> uni{inst(1)};
  const std::array<InstrumentStats, 1> stats{InstrumentStats{}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{stats}};
  price_at(mkt, uni, /*id=*/1, /*mark=*/100);

  Portfolio pf{Decimal::from_int(10'000'000), std::span<const InstrumentId>{uni}};
  pf.apply_fill(FillPayload{inst(1), /*qty=*/+20'000, Decimal::from_int(100), Decimal{}, 0.0,
                            Timestamp::from_unix_nanos(1)}); // a LONG, not a short

  const cost::BorrowModel b{/*annual_rate=*/0.05, cost::DayCount::D360};
  EXPECT_TRUE(cost::daily_borrow(b, pf, mkt, std::span<const InstrumentId>{uni}) == Decimal{});
}


namespace {

struct ElapsedBook {
  const std::array<InstrumentId, 1> universe{inst(1)};
  const std::array<InstrumentStats, 1> stats{InstrumentStats{}};
  Market market{universe, stats};
  Portfolio portfolio;

  explicit ElapsedBook(i64 qty = -1000,
                        Decimal starting_cash = Decimal::from_int(10'000'000))
      : portfolio{starting_cash, universe} {
    price_at(market, universe, 1, 100);
    if (qty != 0) {
      portfolio.apply_fill(FillPayload{inst(1), qty, Decimal::from_int(100), Decimal{}, 0.0,
                                        Timestamp::from_unix_nanos(1)});
    }
  }
};

TEST(BorrowElapsed, OneHourAndWeekendUseActualCalendarTime) {
  const ElapsedBook book;
  const cost::BorrowModel model{0.05, cost::DayCount::D360};
  const auto hour = cost::borrow_for_elapsed(model, book.portfolio, book.market,
                                             book.universe, Duration::hours(1));
  const auto weekend = cost::borrow_for_elapsed(model, book.portfolio, book.market,
                                                book.universe, Duration::hours(72));
  ASSERT_TRUE(hour.has_value());
  ASSERT_TRUE(weekend.has_value());
  EXPECT_EQ(hour->raw(), 578'703'704);
  EXPECT_EQ(weekend->raw(), 41'666'666'667);
}

TEST(BorrowElapsed, CalendarBasesMatchDailyConventionAtOneDay) {
  const ElapsedBook book;
  for (const auto basis : {cost::DayCount::D360, cost::DayCount::D365}) {
    const cost::BorrowModel model{0.05, basis};
    const auto elapsed = cost::borrow_for_elapsed(model, book.portfolio, book.market,
                                                  book.universe, Duration::days(1));
    ASSERT_TRUE(elapsed.has_value());
    EXPECT_EQ(*elapsed, cost::daily_borrow(model, book.portfolio, book.market, book.universe));
    EXPECT_EQ(elapsed->raw(), basis == cost::DayCount::D360 ? 13'888'888'889 : 13'698'630'137);
  }
}

TEST(BorrowElapsed, SubdayPartitionChangesOnlyPerDebitRounding) {
  const ElapsedBook book;
  const cost::BorrowModel model{0.05, cost::DayCount::D360};
  const auto hour = cost::borrow_for_elapsed(model, book.portfolio, book.market,
                                             book.universe, Duration::hours(1));
  const auto minute = cost::borrow_for_elapsed(model, book.portfolio, book.market,
                                               book.universe, Duration::minutes(1));
  ASSERT_TRUE(hour.has_value());
  ASSERT_TRUE(minute.has_value());
  // Sixty separately rounded debits differ by at most thirty nanos plus the
  // rounding of the single hourly debit. Exact equality would promise too much.
  EXPECT_LE(std::fabs(static_cast<f64>(60 * minute->raw() - hour->raw())), 31.0);
}

TEST(BorrowElapsed, FractionalDurationPrecedesMonetaryRounding) {
  ElapsedBook book{-1};
  book.market.shift_mark(inst(1), -99.0); // one dollar of short notional
  const cost::BorrowModel model{0.00000024, cost::DayCount::D360};
  const auto half_day = cost::borrow_for_elapsed(model, book.portfolio, book.market,
                                                 book.universe, Duration::hours(12));
  ASSERT_TRUE(half_day.has_value());
  // Daily fee is 2/3 nano and rounds to one nano; half a day is 1/3 nano,
  // which rounds to zero. Rounding the daily amount first would yield one nano.
  EXPECT_EQ(half_day->raw(), 0);
}

TEST(BorrowElapsed, ZeroRateOrDurationSkipsUnavailableMarks) {
  ElapsedBook book;
  Market unpriced{book.universe, book.stats};
  const auto zero_rate = cost::borrow_for_elapsed(cost::BorrowModel{}, book.portfolio,
                                                  unpriced, book.universe, Duration::days(1));
  const auto zero_time = cost::borrow_for_elapsed({0.05, cost::DayCount::D360}, book.portfolio,
                                                  unpriced, book.universe, Duration::zero());
  ASSERT_TRUE(zero_rate.has_value());
  ASSERT_TRUE(zero_time.has_value());
  EXPECT_EQ(*zero_rate, Decimal{});
  EXPECT_EQ(*zero_time, Decimal{});
}

TEST(BorrowElapsed, LongAndFlatBooksHaveNoCharge) {
  for (const i64 qty : {i64{0}, i64{1000}}) {
    const ElapsedBook book{qty};
    const auto charge = cost::borrow_for_elapsed({0.05, cost::DayCount::D360}, book.portfolio,
                                                 book.market, book.universe, Duration::days(3));
    ASSERT_TRUE(charge.has_value());
    EXPECT_EQ(*charge, Decimal{});
  }
}

TEST(BorrowElapsed, InvalidRateAndBasisAreRejectedEvenForZeroTime) {
  const ElapsedBook book;
  for (const f64 rate : {-0.1, std::numeric_limits<f64>::infinity(),
                         std::numeric_limits<f64>::quiet_NaN()}) {
    EXPECT_FALSE(cost::borrow_for_elapsed({rate, cost::DayCount::D360}, book.portfolio,
                                           book.market, book.universe, Duration::zero()));
  }
  for (const auto basis : {cost::DayCount::D252, static_cast<cost::DayCount>(255)}) {
    EXPECT_FALSE(cost::validate_elapsed_borrow_model({0.0, basis}));
    EXPECT_FALSE(cost::borrow_for_elapsed({0.0, basis}, book.portfolio, book.market,
                                           book.universe, Duration::zero()));
  }
  // D252 remains explicitly available in the legacy one-day API.
  EXPECT_EQ(cost::daily_borrow({0.05, cost::DayCount::D252}, book.portfolio, book.market,
                               book.universe), money(5000.0 / 252.0));
}

TEST(BorrowElapsed, NegativeAndMaximumDurationHaveCheckedBehavior) {
  const ElapsedBook book;
  const cost::BorrowModel model{0.05, cost::DayCount::D360};
  EXPECT_FALSE(cost::borrow_for_elapsed(model, book.portfolio, book.market, book.universe,
                                         Duration::nanoseconds(-1)));
  EXPECT_FALSE(cost::borrow_for_elapsed(cost::BorrowModel{}, book.portfolio, book.market,
      book.universe, Duration::nanoseconds(std::numeric_limits<i64>::min())));
  const auto maximum = cost::borrow_for_elapsed(model, book.portfolio, book.market,
      book.universe, Duration::nanoseconds(std::numeric_limits<i64>::max()));
  ASSERT_TRUE(maximum.has_value());
  EXPECT_NEAR(maximum->to_double(), 1'482'666.5439902868, 1e-8);
}

TEST(BorrowElapsed, InvalidShortMarksAreRejectedWithoutChangingCash) {
  for (const f64 delta : {-100.0, -101.0, std::numeric_limits<f64>::infinity(),
                          std::numeric_limits<f64>::quiet_NaN()}) {
    ElapsedBook book;
    book.market.shift_mark(inst(1), delta);
    const Decimal before = book.portfolio.cash();
    EXPECT_FALSE(cost::accrue_borrow_for_elapsed({0.05, cost::DayCount::D360}, book.portfolio,
                                                 book.market, book.universe, Duration::days(1)));
    EXPECT_EQ(book.portfolio.cash(), before);
  }
}

TEST(BorrowElapsed, UnrepresentableChargeIsAnErrorInsteadOfZero) {
  ElapsedBook book;
  const Decimal before = book.portfolio.cash();
  for (const f64 rate : {1e12, std::numeric_limits<f64>::max()}) {
    EXPECT_FALSE(cost::accrue_borrow_for_elapsed({rate, cost::DayCount::D360}, book.portfolio,
                                                 book.market, book.universe, Duration::days(1)));
    EXPECT_EQ(book.portfolio.cash(), before);
  }
}

TEST(BorrowElapsed, UnknownShortMarkAndNotionalOverflowFailClosed) {
  ElapsedBook book;
  Market unpriced{book.universe, book.stats};
  const cost::BorrowModel model{0.05, cost::DayCount::D360};
  EXPECT_FALSE(cost::borrow_for_elapsed(model, book.portfolio, unpriced,
                                         book.universe, Duration::days(1)));
  book.market.shift_mark(inst(1), std::numeric_limits<f64>::max());
  EXPECT_FALSE(cost::borrow_for_elapsed(model, book.portfolio, book.market,
                                         book.universe, Duration::days(1)));
}

TEST(BorrowElapsed, CashOverflowIsRejectedBeforeMutation) {
  ElapsedBook book{-1, Decimal::from_raw(std::numeric_limits<i64>::min())};
  const Decimal before = book.portfolio.cash();
  const auto status = cost::accrue_borrow_for_elapsed({720.0, cost::DayCount::D360},
      book.portfolio, book.market, book.universe, Duration::days(1));
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), atx::core::ErrorCode::OutOfRange);
  EXPECT_EQ(book.portfolio.cash(), before);
  EXPECT_EQ(book.portfolio.holding(inst(1)).qty, -1);
}

TEST(BorrowElapsed, SuccessfulAccrualOnlyDebitsExactCash) {
  ElapsedBook book;
  const Decimal before = book.portfolio.cash();
  const auto status = cost::accrue_borrow_for_elapsed({0.05, cost::DayCount::D360},
      book.portfolio, book.market, book.universe, Duration::hours(1));
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ((before - book.portfolio.cash()).raw(), 578'703'704);
  EXPECT_EQ(book.portfolio.holding(inst(1)).qty, -1000);
}

} // namespace
}  // namespace atxtest_borrow_test
