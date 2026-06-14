#include <gtest/gtest.h>

#include <type_traits>

#include "atx/core/datetime.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/engine/data/market.hpp"
#include "atx/engine/event/event.hpp"

namespace atxtest_market_test {

using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Side;
using atx::core::domain::Symbol;
using atx::core::domain::Tick;
using atx::engine::data::make_market_bar;
using atx::engine::data::make_market_tick;
using atx::engine::data::MarketPayload;
using atx::engine::event::Event;
using atx::engine::event::EventType;
using atx::engine::event::kPayloadBytes;
using Timestamp = atx::core::time::Timestamp;

// A canonical, fully-populated bar used across the construction tests.
[[nodiscard]] Bar make_sample_bar(Timestamp ts) {
  return Bar{ts,
             Price::from_int(100),
             Price::from_int(110),
             Price::from_int(95),
             Price::from_int(105),
             Quantity::from_int(1'000)};
}

// --------------------------------------------------------------------------
//  Type-level guarantees — MarketPayload must ride the memcpy ring.
// --------------------------------------------------------------------------
TEST(MarketData, MarketPayload_IsTriviallyCopyable) {
  EXPECT_TRUE(std::is_trivially_copyable_v<MarketPayload>);
}

TEST(MarketData, MarketPayload_FitsPayloadBudget) {
  EXPECT_LE(sizeof(MarketPayload), kPayloadBytes);
}

TEST(MarketData, MarketPayload_DefaultsToBarKind) {
  const MarketPayload mp{};
  EXPECT_EQ(mp.kind, MarketPayload::Kind::Bar);
  EXPECT_FALSE(mp.delisted_final);
}

// --------------------------------------------------------------------------
//  Bar construction: the release-at-close default (knowledge_ts == bar.ts).
// --------------------------------------------------------------------------
TEST(MarketData, MakeBar_ReleaseAtClose_PopulatesEvent) {
  const Symbol sym{42};
  const Timestamp close = Timestamp::from_unix_nanos(1'000'000);
  const Bar bar = make_sample_bar(close);

  const Event e = make_market_bar(sym, bar, close);

  EXPECT_EQ(e.type, EventType::Market);
  EXPECT_EQ(e.kind(), EventType::Market);
  EXPECT_EQ(e.event_ts, close);
  EXPECT_EQ(e.knowledge_ts, close); // release-at-close: known at the bar's ts
}

TEST(MarketData, MakeBar_Payload_RoundTripsSymbolAndBar) {
  const Symbol sym{7};
  const Timestamp close = Timestamp::from_unix_nanos(2'000'000);
  const Bar bar = make_sample_bar(close);

  const Event e = make_market_bar(sym, bar, close);
  const auto mp = e.payload_as<MarketPayload>();

  EXPECT_EQ(mp.symbol, sym);
  EXPECT_EQ(mp.kind, MarketPayload::Kind::Bar);
  EXPECT_FALSE(mp.delisted_final);
  EXPECT_EQ(mp.as_bar(), bar);
  // Spot-check strong-type fields survive the round-trip exactly.
  EXPECT_EQ(mp.as_bar().open, Price::from_int(100));
  EXPECT_EQ(mp.as_bar().close, Price::from_int(105));
  EXPECT_EQ(mp.as_bar().volume, Quantity::from_int(1'000));
  EXPECT_EQ(mp.as_bar().ts, close);
}

TEST(MarketData, MakeBar_TagMatchesAccessor) {
  const Symbol sym{1};
  const Timestamp close = Timestamp::from_unix_nanos(3'000'000);
  const Bar bar = make_sample_bar(close);

  const auto mp = make_market_bar(sym, bar, close).payload_as<MarketPayload>();
  ASSERT_EQ(mp.kind, MarketPayload::Kind::Bar);
  EXPECT_EQ(mp.as_bar(), bar); // accessor keyed by the tag returns the active arm
}

// --------------------------------------------------------------------------
//  delisted_final survivorship flag round-trips both ways.
// --------------------------------------------------------------------------
TEST(MarketData, MakeBar_DelistedFinalTrue_RoundTrips) {
  const Symbol sym{99};
  const Timestamp close = Timestamp::from_unix_nanos(4'000'000);
  const Bar bar = make_sample_bar(close);

  const auto mp =
      make_market_bar(sym, bar, close, /*delisted_final=*/true).payload_as<MarketPayload>();
  EXPECT_TRUE(mp.delisted_final);
}

TEST(MarketData, MakeBar_DelistedFinalFalse_RoundTrips) {
  const Symbol sym{99};
  const Timestamp close = Timestamp::from_unix_nanos(4'000'000);
  const Bar bar = make_sample_bar(close);

  const auto mp =
      make_market_bar(sym, bar, close, /*delisted_final=*/false).payload_as<MarketPayload>();
  EXPECT_FALSE(mp.delisted_final);
}

// --------------------------------------------------------------------------
//  Boundary: zero-volume bar is a valid record (e.g. a no-trade interval).
// --------------------------------------------------------------------------
TEST(MarketData, MakeBar_ZeroVolume_RoundTrips) {
  const Symbol sym{3};
  const Timestamp close = Timestamp::from_unix_nanos(5'000'000);
  const Bar bar{close,
                Price::from_int(50),
                Price::from_int(50),
                Price::from_int(50),
                Price::from_int(50),
                Quantity::from_int(0)};

  const auto mp = make_market_bar(sym, bar, close).payload_as<MarketPayload>();
  EXPECT_EQ(mp.as_bar().volume, Quantity::from_int(0));
  EXPECT_EQ(mp.as_bar(), bar);
}

// --------------------------------------------------------------------------
//  Bitemporal invariant on the two boundaries that are ALLOWED.
// --------------------------------------------------------------------------
TEST(MarketData, MakeBar_KnowledgeEqualsEvent_Allowed) {
  const Symbol sym{8};
  const Timestamp close = Timestamp::from_unix_nanos(6'000'000);
  const Bar bar = make_sample_bar(close);

  const Event e = make_market_bar(sym, bar, close); // knowledge_ts == event_ts
  EXPECT_EQ(e.knowledge_ts, e.event_ts);
}

TEST(MarketData, MakeBar_LateBar_KnowledgeAfterEvent_Allowed) {
  const Symbol sym{8};
  const Timestamp close = Timestamp::from_unix_nanos(6'000'000);
  const Timestamp late = Timestamp::from_unix_nanos(9'000'000); // restated later
  const Bar bar = make_sample_bar(close);

  const Event e = make_market_bar(sym, bar, late);
  EXPECT_EQ(e.event_ts, close);
  EXPECT_EQ(e.knowledge_ts, late);
  EXPECT_GT(e.knowledge_ts, e.event_ts);
}

// --------------------------------------------------------------------------
//  Bitemporal invariant VIOLATION aborts in debug (knowledge before event).
// --------------------------------------------------------------------------
TEST(MarketData, MakeBar_KnowledgeBeforeEvent_Aborts) {
  const Symbol sym{8};
  const Timestamp close = Timestamp::from_unix_nanos(6'000'000);
  const Timestamp earlier = Timestamp::from_unix_nanos(1'000'000);
  const Bar bar = make_sample_bar(close);

  // A fact cannot be known before it is true.
  EXPECT_DEATH({ (void)make_market_bar(sym, bar, earlier); }, "");
}

// --------------------------------------------------------------------------
//  Tick arm: construction, tag, and accessor.
// --------------------------------------------------------------------------
TEST(MarketData, MakeTick_Payload_RoundTripsSymbolAndTick) {
  const Symbol sym{55};
  const Timestamp ts = Timestamp::from_unix_nanos(7'000'000);
  const Tick tick{ts, Price::from_int(200), Quantity::from_int(5), Side::Sell};

  const Event e = make_market_tick(sym, tick, ts);

  EXPECT_EQ(e.type, EventType::Market);
  EXPECT_EQ(e.event_ts, ts);
  EXPECT_EQ(e.knowledge_ts, ts);

  const auto mp = e.payload_as<MarketPayload>();
  EXPECT_EQ(mp.symbol, sym);
  ASSERT_EQ(mp.kind, MarketPayload::Kind::Tick);
  EXPECT_EQ(mp.as_tick(), tick);
  EXPECT_EQ(mp.as_tick().price, Price::from_int(200));
  EXPECT_EQ(mp.as_tick().size, Quantity::from_int(5));
  EXPECT_EQ(mp.as_tick().side, Side::Sell);
}

TEST(MarketData, MakeTick_DelistedFinal_RoundTrips) {
  const Symbol sym{55};
  const Timestamp ts = Timestamp::from_unix_nanos(7'000'000);
  const Tick tick{ts, Price::from_int(200), Quantity::from_int(5), Side::Buy};

  const auto mp =
      make_market_tick(sym, tick, ts, /*delisted_final=*/true).payload_as<MarketPayload>();
  EXPECT_TRUE(mp.delisted_final);
}

TEST(MarketData, MakeTick_KnowledgeBeforeEvent_Aborts) {
  const Symbol sym{55};
  const Timestamp ts = Timestamp::from_unix_nanos(7'000'000);
  const Timestamp earlier = Timestamp::from_unix_nanos(1'000'000);
  const Tick tick{ts, Price::from_int(200), Quantity::from_int(5), Side::Buy};

  EXPECT_DEATH({ (void)make_market_tick(sym, tick, earlier); }, "");
}


}  // namespace atxtest_market_test
