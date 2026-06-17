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


}  // namespace atxtest_borrow_test
