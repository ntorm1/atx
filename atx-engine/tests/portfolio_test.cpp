// atx::engine — Portfolio accounting tests (P2-5).
//
// Portfolio keeps average-cost-basis positions, an exact-Decimal cash ledger,
// realized/unrealized P&L, and f64 exposure/leverage. apply_fill drives the
// open/increase/reduce/close/flip state machine off a signed-qty FillPayload
// (+buy / −sell). Money state (cash/realized/fees/avg_price) is exact Decimal;
// market-derived f64 (mark/market_value/unrealized/equity/gross/net/leverage)
// crosses the boundary only where market data is inherently float.
//
// Covers the plan's Portfolio test list verbatim:
//   * first fill from flat sets avg = fill price;
//   * increase books the weighted-average price (hand-computed exact);
//   * partial reduce books proportional realized (hand-computed);
//   * full close zeroes the position + avg, books realized;
//   * flip long→short books the long leg's P&L, opens short at fill price,
//     correct remainder qty;
//   * cash ledger balances after a sequence (exact Decimal);
//   * equity = cash + Σ market_value after a mark;
//   * gross / net / leverage on a mixed book;
//   * dollar-neutral book → net ≈ 0;
//   * boundaries: reduce-to-zero, over-reduce (flip), first fill from flat;
//   * a freshly-opened position is unpriced (NaN mark) → contributes 0 to equity
//     before the first mark_to_market, then values correctly after;
//   * EXPECT_DEATH on a zero-price, negative-price, zero-qty, and out-of-universe
//     fill (apply_fill's ATX_ASSERT preconditions).
//
// Naming: Subject_Condition_ExpectedResult.

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <span>

#include "atx/core/datetime.hpp" // Timestamp
#include "atx/core/decimal.hpp"  // Decimal
#include "atx/core/types.hpp"    // u32, i64, f64

#include "atx/engine/exec/payloads.hpp"       // FillPayload
#include "atx/engine/loop/market.hpp"         // Market, InstrumentStats
#include "atx/engine/loop/panel_types.hpp"    // MarketSlice, SliceRow
#include "atx/engine/loop/types.hpp"          // InstrumentId
#include "atx/engine/portfolio/portfolio.hpp" // Portfolio, Holding

namespace {

using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::time::Timestamp;
using atx::engine::Holding;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::MarketSlice;
using atx::engine::Portfolio;
using atx::engine::SliceRow;
using atx::engine::exec::FillPayload;

// ---- helpers --------------------------------------------------------------

[[nodiscard]] InstrumentId inst(atx::u32 id) noexcept { return InstrumentId{id}; }
[[nodiscard]] Timestamp ts(atx::i64 nanos) noexcept { return Timestamp::from_unix_nanos(nanos); }
[[nodiscard]] Decimal dec(atx::i64 whole) noexcept { return Decimal::from_int(whole); }

// A fill with whole-unit price and zero fee unless overridden.
[[nodiscard]] FillPayload fill(atx::u32 id, atx::i64 qty, atx::i64 price,
                               atx::i64 fee = 0) noexcept {
  return FillPayload{inst(id), qty, dec(price), dec(fee), 0.0, ts(1)};
}

[[nodiscard]] SliceRow row(atx::u32 id, atx::i64 close) noexcept {
  return SliceRow{inst(id),
                  Bar{ts(1), Price::from_int(close), Price::from_int(close), Price::from_int(close),
                      Price::from_int(close), Quantity::from_int(0)},
                  false};
}

// =====================================================================
//  Open from flat — avg = fill price; cash debited.
// =====================================================================

TEST(Portfolio, ApplyFill_FirstFillFromFlat_SetsAvgToFillPrice) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  pf.apply_fill(fill(10, 100, 50)); // buy 100 @ 50

  const Holding &h = pf.holding(inst(10));
  EXPECT_EQ(h.qty, 100);
  EXPECT_EQ(h.avg_price, dec(50));
  EXPECT_EQ(h.realized, dec(0));
  // cash = 100000 − 100*50 = 95000
  EXPECT_EQ(pf.cash(), dec(95000));
}

// =====================================================================
//  Increase — new avg is the weighted average (hand-computed exact).
//
//  buy 100 @ 50, then buy 100 @ 60 → avg = (100*50 + 100*60)/200 = 55.
// =====================================================================

TEST(Portfolio, ApplyFill_IncreaseSameSide_BooksWeightedAvg) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  pf.apply_fill(fill(10, 100, 50));
  pf.apply_fill(fill(10, 100, 60));

  const Holding &h = pf.holding(inst(10));
  EXPECT_EQ(h.qty, 200);
  EXPECT_EQ(h.avg_price, dec(55));
  EXPECT_EQ(h.realized, dec(0));
  // cash = 100000 − 5000 − 6000 = 89000
  EXPECT_EQ(pf.cash(), dec(89000));
}

// =====================================================================
//  Partial reduce — books proportional realized; avg unchanged.
//
//  long 200 @ 55, sell 50 @ 70 → realized = 50*(70−55) = 750; qty 150; avg 55.
// =====================================================================

TEST(Portfolio, ApplyFill_PartialReduceLong_BooksProportionalRealized) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  pf.apply_fill(fill(10, 100, 50));
  pf.apply_fill(fill(10, 100, 60)); // avg 55, qty 200
  pf.apply_fill(fill(10, -50, 70)); // sell 50 @ 70

  const Holding &h = pf.holding(inst(10));
  EXPECT_EQ(h.qty, 150);
  EXPECT_EQ(h.avg_price, dec(55)); // unchanged on reduce
  EXPECT_EQ(h.realized, dec(750)); // 50*(70−55)
  // cash = 89000 − (−50)*70 = 89000 + 3500 = 92500
  EXPECT_EQ(pf.cash(), dec(92500));
}

// =====================================================================
//  Full close — qty → 0, avg → 0, realized booked.
//
//  long 100 @ 55, sell 100 @ 70 → realized = 100*(70−55) = 1500; qty 0; avg 0.
// =====================================================================

TEST(Portfolio, ApplyFill_FullCloseLong_ZeroesPositionAndAvg) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  pf.apply_fill(fill(10, 100, 55));  // long 100 @ 55
  pf.apply_fill(fill(10, -100, 70)); // close @ 70

  const Holding &h = pf.holding(inst(10));
  EXPECT_EQ(h.qty, 0);
  EXPECT_EQ(h.avg_price, dec(0)); // zeroed on full close
  EXPECT_EQ(h.realized, dec(1500));
  // cash = 100000 − 100*55 + 100*70 = 100000 − 5500 + 7000 = 101500
  EXPECT_EQ(pf.cash(), dec(101500));
}

// =====================================================================
//  Reduce short — symmetric realized sign convention.
//
//  short 100 @ 70 (sell), buy 40 @ 60 → realized = 40*(70−60) = 400; qty −60.
//  realized for a short closed at a LOWER price is a gain (sold high, bought
//  low). sign(old) = −1, (p−avg) = (60−70) = −10, 40*(−10)*(−1) = +400.
// =====================================================================

TEST(Portfolio, ApplyFill_PartialReduceShort_BooksGainWhenBoughtLower) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  pf.apply_fill(fill(10, -100, 70)); // short 100 @ 70
  pf.apply_fill(fill(10, 40, 60));   // cover 40 @ 60

  const Holding &h = pf.holding(inst(10));
  EXPECT_EQ(h.qty, -60);
  EXPECT_EQ(h.avg_price, dec(70));
  EXPECT_EQ(h.realized, dec(400));
  // cash = 100000 − (−100)*70 − 40*60 = 100000 + 7000 − 2400 = 104600
  EXPECT_EQ(pf.cash(), dec(104600));
}

// =====================================================================
//  Flip long→short — close long leg, open short at fill price.
//
//  long 100 @ 50, sell 150 @ 60:
//    close leg: realized += 100*(60−50) = 1000.
//    remainder = 100 + (−150) = −50; avg reset to fill price 60.
//  cash = 100000 − 100*50 − (−150)*60 = 100000 − 5000 + 9000 = 104000.
// =====================================================================

TEST(Portfolio, ApplyFill_FlipLongToShort_BooksLongLegOpensShortAtFill) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  pf.apply_fill(fill(10, 100, 50));  // long 100 @ 50
  pf.apply_fill(fill(10, -150, 60)); // sell 150 @ 60 → flip to short 50

  const Holding &h = pf.holding(inst(10));
  EXPECT_EQ(h.qty, -50);
  EXPECT_EQ(h.avg_price, dec(60));  // remainder opened at fill price
  EXPECT_EQ(h.realized, dec(1000)); // long-leg P&L
  EXPECT_EQ(pf.cash(), dec(104000));
}

// =====================================================================
//  Flip short→long — symmetric.
//
//  short 100 @ 70, buy 150 @ 60:
//    close leg: realized += 100*(60−70)*sign(−1) = 100*(−10)*(−1) = +1000.
//    remainder = −100 + 150 = +50; avg reset to 60.
//  cash = 100000 − (−100)*70 − 150*60 = 100000 + 7000 − 9000 = 98000.
// =====================================================================

TEST(Portfolio, ApplyFill_FlipShortToLong_BooksShortLegOpensLongAtFill) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  pf.apply_fill(fill(10, -100, 70));
  pf.apply_fill(fill(10, 150, 60));

  const Holding &h = pf.holding(inst(10));
  EXPECT_EQ(h.qty, 50);
  EXPECT_EQ(h.avg_price, dec(60));
  EXPECT_EQ(h.realized, dec(1000));
  EXPECT_EQ(pf.cash(), dec(98000));
}

// =====================================================================
//  Reduce-to-zero boundary — an exact full close via reduce path.
// =====================================================================

TEST(Portfolio, ApplyFill_ReduceExactlyToZero_ClosesPosition) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  pf.apply_fill(fill(10, 30, 100));
  pf.apply_fill(fill(10, -30, 100)); // exact close at entry → realized 0

  const Holding &h = pf.holding(inst(10));
  EXPECT_EQ(h.qty, 0);
  EXPECT_EQ(h.avg_price, dec(0));
  EXPECT_EQ(h.realized, dec(0));
}

// =====================================================================
//  Fees — accumulated as positive magnitude; cash debited by qty*p + fee.
// =====================================================================

TEST(Portfolio, ApplyFill_WithFee_AccumulatesFeeAndDebitsCash) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  pf.apply_fill(fill(10, 100, 50, /*fee=*/5)); // buy 100 @ 50, fee 5

  const Holding &h = pf.holding(inst(10));
  EXPECT_EQ(h.fees, dec(5)); // positive magnitude
  // cash = 100000 − (100*50 + 5) = 94995
  EXPECT_EQ(pf.cash(), dec(94995));
}

// =====================================================================
//  Unrealized & market_value — qty·(mark − avg) and qty·mark.
// =====================================================================

TEST(Portfolio, MarkToMarket_LongPosition_SetsMarkAndUnrealized) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  pf.apply_fill(fill(10, 100, 50)); // long 100 @ 50

  std::array<SliceRow, 1> rows{row(10, 60)};
  mkt.update_prices(MarketSlice{ts(1), std::span<const SliceRow>{rows}});
  pf.mark_to_market(mkt);

  const Holding &h = pf.holding(inst(10));
  EXPECT_DOUBLE_EQ(h.mark, 60.0);
  EXPECT_DOUBLE_EQ(h.market_value(), 6000.0); // 100 * 60
  EXPECT_DOUBLE_EQ(h.unrealized(), 1000.0);   // 100 * (60 − 50)
}

TEST(Portfolio, Unrealized_FlatPosition_IsZero) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  const Holding &h = pf.holding(inst(10));
  EXPECT_DOUBLE_EQ(h.unrealized(), 0.0);
  EXPECT_DOUBLE_EQ(h.market_value(), 0.0);
}

// =====================================================================
//  Unpriced sentinel — a freshly-opened position has a NaN mark and
//  contributes 0 to equity/market_value/unrealized BEFORE the first
//  mark_to_market (equity == cash), then values correctly AFTER it.
// =====================================================================

TEST(Portfolio, Equity_OpenedPositionBeforeMark_ContributesZero) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  pf.apply_fill(fill(10, 100, 50)); // cash → 95000; mark still NaN (unpriced)

  const Holding &h = pf.holding(inst(10));
  EXPECT_TRUE(std::isnan(h.mark));         // not yet priced
  EXPECT_DOUBLE_EQ(h.market_value(), 0.0); // NaN mark ⇒ 0 contribution
  EXPECT_DOUBLE_EQ(h.unrealized(), 0.0);   // NaN mark ⇒ 0 (no phantom −5000)
  EXPECT_DOUBLE_EQ(pf.equity(), 95000.0);  // equity == cash, well-defined
  EXPECT_DOUBLE_EQ(pf.gross(), 0.0);
  EXPECT_DOUBLE_EQ(pf.net(), 0.0);

  // After the first mark the position values correctly.
  std::array<SliceRow, 1> rows{row(10, 60)};
  mkt.update_prices(MarketSlice{ts(1), std::span<const SliceRow>{rows}});
  pf.mark_to_market(mkt);

  EXPECT_DOUBLE_EQ(pf.holding(inst(10)).market_value(), 6000.0);
  EXPECT_DOUBLE_EQ(pf.equity(), 101000.0); // 95000 + 6000
}

// =====================================================================
//  Equity = cash + Σ market_value after a mark.
// =====================================================================

TEST(Portfolio, Equity_AfterMark_EqualsCashPlusMarketValue) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  pf.apply_fill(fill(10, 100, 50)); // cash → 95000, position 100 @ 50

  std::array<SliceRow, 1> rows{row(10, 60)};
  mkt.update_prices(MarketSlice{ts(1), std::span<const SliceRow>{rows}});
  pf.mark_to_market(mkt);

  // equity = 95000 + 100*60 = 101000
  EXPECT_DOUBLE_EQ(pf.equity(), 101000.0);
}

// =====================================================================
//  gross / net / leverage on a mixed (long + short) book.
//
//  long 100 @ 50 of inst 10 (mark 60 → mv +6000),
//  short 100 @ 50 of inst 20 (mark 40 → mv −4000).
//  gross = 6000 + 4000 = 10000; net = 6000 − 4000 = 2000.
//  cash = 100000 − 100*50 (buy) + 100*50 (sell) = 100000.
//  equity = 100000 + 2000 = 102000; leverage = 10000/102000.
// =====================================================================

TEST(Portfolio, GrossNetLeverage_MixedBook_ComputedFromMarketValues) {
  std::array<InstrumentId, 2> uni{inst(10), inst(20)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  pf.apply_fill(fill(10, 100, 50));  // long 100
  pf.apply_fill(fill(20, -100, 50)); // short 100

  std::array<SliceRow, 2> rows{row(10, 60), row(20, 40)};
  mkt.update_prices(MarketSlice{ts(1), std::span<const SliceRow>{rows}});
  pf.mark_to_market(mkt);

  EXPECT_DOUBLE_EQ(pf.gross(), 10000.0);
  EXPECT_DOUBLE_EQ(pf.net(), 2000.0);
  EXPECT_DOUBLE_EQ(pf.equity(), 102000.0);
  EXPECT_DOUBLE_EQ(pf.leverage(), 10000.0 / 102000.0);
}

// =====================================================================
//  Dollar-neutral book → net ≈ 0.
//
//  long 100 @ 50 of inst 10, short 100 @ 50 of inst 20, both mark 50.
//  mv +5000 and −5000 → net = 0; gross = 10000.
// =====================================================================

TEST(Portfolio, Net_DollarNeutralBook_IsApproximatelyZero) {
  std::array<InstrumentId, 2> uni{inst(10), inst(20)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{}};

  pf.apply_fill(fill(10, 100, 50));
  pf.apply_fill(fill(20, -100, 50));

  std::array<SliceRow, 2> rows{row(10, 50), row(20, 50)};
  mkt.update_prices(MarketSlice{ts(1), std::span<const SliceRow>{rows}});
  pf.mark_to_market(mkt);

  EXPECT_NEAR(pf.net(), 0.0, 1e-9);
  EXPECT_DOUBLE_EQ(pf.gross(), 10000.0);
}

// =====================================================================
//  Leverage guard — zero equity returns 0 (no div-by-zero).
// =====================================================================

TEST(Portfolio, Leverage_ZeroEquity_ReturnsZero) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(0), std::span<const InstrumentId>{uni}}; // no cash, flat book

  EXPECT_DOUBLE_EQ(pf.equity(), 0.0);
  EXPECT_DOUBLE_EQ(pf.leverage(), 0.0);
}

// =====================================================================
//  Cash ledger balances over a multi-instrument sequence (exact Decimal).
// =====================================================================

TEST(Portfolio, Cash_AfterMixedSequence_BalancesExactly) {
  std::array<InstrumentId, 2> uni{inst(10), inst(20)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  pf.apply_fill(fill(10, 100, 50, 5));  // −5000 − 5  = −5005
  pf.apply_fill(fill(20, -200, 30, 7)); // +6000 − 7  = +5993
  pf.apply_fill(fill(10, -40, 55, 2));  // +2200 − 2  = +2198

  // cash = 100000 − 5005 + 5993 + 2198 = 103186
  EXPECT_EQ(pf.cash(), dec(103186));
}

// =====================================================================
//  Death tests — invalid input is a programmer error (ATX_ASSERT).
// =====================================================================

TEST(PortfolioDeathTest, ApplyFill_ZeroPrice_Aborts) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  EXPECT_DEATH({ pf.apply_fill(fill(10, 100, 0)); }, "");
}

TEST(PortfolioDeathTest, ApplyFill_NegativePrice_Aborts) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  EXPECT_DEATH({ pf.apply_fill(fill(10, 100, -5)); }, "");
}

TEST(PortfolioDeathTest, ApplyFill_IdNotInUniverse_Aborts) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  EXPECT_DEATH({ pf.apply_fill(fill(77, 100, 50)); }, "");
}

TEST(PortfolioDeathTest, ApplyFill_ZeroQty_Aborts) {
  std::array<InstrumentId, 1> uni{inst(10)};
  Portfolio pf{dec(100000), std::span<const InstrumentId>{uni}};

  // A zero-quantity fill is not a transaction (sign encodes direction).
  EXPECT_DEATH({ pf.apply_fill(fill(10, 0, 50)); }, "");
}

} // namespace
