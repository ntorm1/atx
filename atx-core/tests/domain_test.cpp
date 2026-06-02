// Tests for atx::core::domain — market vocabulary (Price/Quantity/Notional,
// Side, Symbol/SymbolTable, Bar/Tick).
//
// Coverage strategy (agent profile §7):
//   - Typed arithmetic: Price x Quantity -> Notional; Notional add/sub;
//     Quantity add/sub. Result exactness via Decimal's canonical to_string.
//   - from_string: success round-trips to_string; bad input -> Err.
//   - Strong-type discipline: comparison ordering on each unit.
//   - Side enum: distinct enumerators; opposite().
//   - Symbol interning: idempotent round-trip, distinct ids, size().
//   - name() precondition (out-of-range Symbol) -> documented ABORT (death).
//   - POD records: Bar / Tick construction and field access.
//
// Decimal to_string policy (from decimal.hpp): trailing zeros trimmed, at least
// one fractional digit kept — so 31.5 renders as "31.5" (NOT "31.500000000").

#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "atx/core/datetime.hpp" // atx::core::time::Timestamp
#include "atx/core/error.hpp"    // ErrorCode

using namespace atx;              // NOLINT(google-build-using-namespace) — i64 etc.
using namespace atx::core;        // NOLINT(google-build-using-namespace) — ErrorCode.
using namespace atx::core::domain; // NOLINT(google-build-using-namespace) — domain types.

using atx::core::time::Timestamp;

// ============================================================
//  Typed arithmetic
// ============================================================

TEST(Domain, PriceTimesQuantityIsNotional) {
  const auto price_result = Price::from_string("10.50");
  ASSERT_TRUE(price_result.has_value()) << price_result.error().to_string();
  const Price p = price_result.value();
  const Quantity q = Quantity::from_int(3);

  const Notional n = p * q; // 10.50 * 3 == 31.5

  // Decimal trims trailing zeros but keeps one fractional digit -> "31.5".
  EXPECT_EQ(n.to_string(), "31.5");

  const auto expected = Notional::from_string("31.5");
  ASSERT_TRUE(expected.has_value()) << expected.error().to_string();
  EXPECT_EQ(n, expected.value());
}

TEST(Domain, QuantityTimesPriceIsCommutative) {
  const Price p = Price::from_int(7);
  const Quantity q = Quantity::from_int(6);
  EXPECT_EQ(p * q, q * p);
  EXPECT_EQ((p * q).to_string(), "42.0");
}

TEST(Domain, NotionalAddition) {
  const auto a = Notional::from_string("100.25");
  const auto b = Notional::from_string("50.75");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const Notional sum = a.value() + b.value();
  EXPECT_EQ(sum.to_string(), "151.0");
}

TEST(Domain, NotionalSubtraction) {
  const auto a = Notional::from_string("151.0");
  const auto b = Notional::from_string("50.75");
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const Notional diff = a.value() - b.value();
  EXPECT_EQ(diff.to_string(), "100.25");
}

TEST(Domain, QuantityAddSub) {
  const Quantity a = Quantity::from_int(10);
  const Quantity b = Quantity::from_int(4);
  EXPECT_EQ((a + b).to_string(), "14.0");
  EXPECT_EQ((a - b).to_string(), "6.0");
}

// ============================================================
//  Construction & parsing
// ============================================================

TEST(Domain, PriceFromIntRoundTrip) {
  const Price p = Price::from_int(42);
  EXPECT_EQ(p.to_string(), "42.0");
  EXPECT_EQ(p.to_decimal(), Decimal::from_int(42));
}

TEST(Domain, PriceFromStringBadInputIsErr) {
  const auto bad = Price::from_string("not-a-number");
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), ErrorCode::ParseError);
}

TEST(Domain, PriceFromStringEmptyIsErr) {
  EXPECT_FALSE(Price::from_string("").has_value());
}

// ============================================================
//  Ordering / comparison
// ============================================================

TEST(Domain, PriceComparisonOrdering) {
  const Price lo = Price::from_int(5);
  const Price hi = Price::from_int(9);
  EXPECT_LT(lo, hi);
  EXPECT_GT(hi, lo);
  EXPECT_EQ(lo, Price::from_int(5));
  EXPECT_NE(lo, hi);
}

TEST(Domain, QuantityComparisonOrdering) {
  const auto small = Quantity::from_string("1.5");
  const auto large = Quantity::from_string("2.0");
  ASSERT_TRUE(small.has_value());
  ASSERT_TRUE(large.has_value());
  EXPECT_LT(small.value(), large.value());
}

// ============================================================
//  Side enum
// ============================================================

TEST(Domain, SideEnum) {
  EXPECT_NE(Side::Buy, Side::Sell);
  EXPECT_EQ(opposite(Side::Buy), Side::Sell);
  EXPECT_EQ(opposite(Side::Sell), Side::Buy);
}

// ============================================================
//  Symbol / SymbolTable
// ============================================================

TEST(Symbol, InternRoundTrip) {
  SymbolTable tbl;
  const Symbol a = tbl.intern("AAPL");
  const Symbol b = tbl.intern("AAPL");
  EXPECT_EQ(a, b);
  EXPECT_EQ(tbl.name(a), "AAPL");
  EXPECT_NE(a, tbl.intern("MSFT"));
}

TEST(Symbol, DistinctSymbolsDistinctIds) {
  SymbolTable tbl;
  const Symbol a = tbl.intern("AAPL");
  const Symbol b = tbl.intern("MSFT");
  const Symbol c = tbl.intern("GOOG");
  EXPECT_EQ(tbl.size(), 3U);
  EXPECT_NE(a, b);
  EXPECT_NE(b, c);
  EXPECT_NE(a, c);
  EXPECT_EQ(tbl.name(a), "AAPL");
  EXPECT_EQ(tbl.name(b), "MSFT");
  EXPECT_EQ(tbl.name(c), "GOOG");
}

TEST(Symbol, NameStableAcrossGrowth) {
  // A view returned by name() must survive subsequent interning (which can grow
  // and reallocate the internal vector). Documents the lifetime guarantee.
  SymbolTable tbl;
  const Symbol first = tbl.intern("AAPL");
  const std::string_view view = tbl.name(first);
  for (int i = 0; i < 100; ++i) {
    (void)tbl.intern("SYM" + std::to_string(i));
  }
  EXPECT_EQ(view, "AAPL");
}

TEST(Symbol, EmptyTableSizeZero) {
  const SymbolTable tbl;
  EXPECT_EQ(tbl.size(), 0U);
}

TEST(Symbol, NameOutOfRangeAborts) {
  // name() documents a precondition (sym.id < size()); a violation ABORTS.
  SymbolTable tbl;
  const Symbol bogus{42};
  EXPECT_DEATH({ (void)tbl.name(bogus); }, ".*");
}

// ============================================================
//  POD records
// ============================================================

TEST(Domain, BarConstruction) {
  const Timestamp ts = Timestamp::from_unix_seconds(1'700'000'000);
  const Bar bar{ts,
                Price::from_int(100),
                Price::from_int(110),
                Price::from_int(95),
                Price::from_int(105),
                Quantity::from_int(2500)};
  EXPECT_EQ(bar.close, Price::from_int(105));
  EXPECT_EQ(bar.open, Price::from_int(100));
  EXPECT_EQ(bar.high, Price::from_int(110));
  EXPECT_EQ(bar.low, Price::from_int(95));
  EXPECT_EQ(bar.volume, Quantity::from_int(2500));
  EXPECT_EQ(bar.ts, ts);
}

TEST(Domain, OhlcvIsBarAlias) {
  const OHLCV bar{};
  EXPECT_EQ(bar.close, Price{});
}

TEST(Domain, TickConstruction) {
  const Timestamp ts = Timestamp::from_unix_nanos(1'700'000'000'123'456'789LL);
  const auto px = Price::from_string("250.75");
  ASSERT_TRUE(px.has_value());
  const Tick tick{ts, px.value(), Quantity::from_int(50), Side::Sell};
  EXPECT_EQ(tick.price, px.value());
  EXPECT_EQ(tick.size, Quantity::from_int(50));
  EXPECT_EQ(tick.side, Side::Sell);
  EXPECT_EQ(tick.ts, ts);
}
