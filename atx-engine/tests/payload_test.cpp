// atx::engine::exec — payload round-trip, type-level, and maker tests (P2-1).
//
// Covers: trivially-copyable + size static_asserts; round-trip of each payload
// through Event::store_payload / payload_as; make_*_event sets the correct
// EventType and preserves both temporal axes; signed-qty sign convention (+buy
// / −sell); make_order_event rejects qty==0; epoch-zero queued_at accepted;
// Decimal price exactness; OrderType exhaustive to_string compiles.
//
// Naming: Subject_Condition_ExpectedResult.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>

#include "atx/core/datetime.hpp"      // Timestamp
#include "atx/core/decimal.hpp"       // Decimal
#include "atx/core/domain/symbol.hpp" // Symbol
#include "atx/core/error.hpp"         // Result, ErrorCode

#include "atx/engine/event/event.hpp"   // Event, EventType, kPayloadBytes
#include "atx/engine/exec/payloads.hpp" // SignalPayload, OrderPayload, FillPayload, make_*_event

namespace atxtest_payload_test {

using atx::core::Decimal;
using atx::core::ErrorCode;
using atx::core::domain::Symbol;
using atx::core::time::Timestamp;
using atx::engine::event::Event;
using atx::engine::event::EventType;
using atx::engine::event::kPayloadBytes;
using atx::engine::exec::FillPayload;
using atx::engine::exec::make_fill_event;
using atx::engine::exec::make_order_event;
using atx::engine::exec::make_signal_event;
using atx::engine::exec::OrderPayload;
using atx::engine::exec::OrderType;
using atx::engine::exec::SignalPayload;
using atx::engine::exec::to_string;

// =====================================================================
//  Compile-time type-level guarantees (mirror market.hpp pattern).
// =====================================================================

// These duplicate the header's own static_asserts; keeping them in the test
// file localises regressions to the payload taxonomy in ctest output.
static_assert(std::is_trivially_copyable_v<SignalPayload>,
              "SignalPayload must be trivially copyable to ride the memcpy ring");
static_assert(std::is_trivially_copyable_v<OrderPayload>,
              "OrderPayload must be trivially copyable to ride the memcpy ring");
static_assert(std::is_trivially_copyable_v<FillPayload>,
              "FillPayload must be trivially copyable to ride the memcpy ring");

static_assert(sizeof(SignalPayload) <= kPayloadBytes,
              "SignalPayload exceeds kPayloadBytes — ring-slot budget exceeded");
static_assert(sizeof(OrderPayload) <= kPayloadBytes,
              "OrderPayload exceeds kPayloadBytes — ring-slot budget exceeded");
static_assert(sizeof(FillPayload) <= kPayloadBytes,
              "FillPayload exceeds kPayloadBytes — ring-slot budget exceeded");

// =====================================================================
//  Run-time type-level guards (document the invariants as ctest output).
// =====================================================================

TEST(PayloadTypes, SignalPayload_IsTriviallyCopyable) {
  EXPECT_TRUE(std::is_trivially_copyable_v<SignalPayload>);
}
TEST(PayloadTypes, OrderPayload_IsTriviallyCopyable) {
  EXPECT_TRUE(std::is_trivially_copyable_v<OrderPayload>);
}
TEST(PayloadTypes, FillPayload_IsTriviallyCopyable) {
  EXPECT_TRUE(std::is_trivially_copyable_v<FillPayload>);
}

TEST(PayloadTypes, SignalPayload_FitsRingSlotBudget) {
  EXPECT_LE(sizeof(SignalPayload), kPayloadBytes);
}
TEST(PayloadTypes, OrderPayload_FitsRingSlotBudget) {
  EXPECT_LE(sizeof(OrderPayload), kPayloadBytes);
}
TEST(PayloadTypes, FillPayload_FitsRingSlotBudget) {
  EXPECT_LE(sizeof(FillPayload), kPayloadBytes);
}

// =====================================================================
//  SignalPayload — round-trip through Event.
// =====================================================================

TEST(SignalPayloadRoundTrip, StoreThenLoad_PreservesAllFields) {
  const Symbol sym = Symbol{7U};
  const SignalPayload in{sym, 0.42};

  Event e{};
  e.store_payload(in);
  e.type = EventType::Signal;

  const auto out = e.payload_as<SignalPayload>(); // SAFETY: tag == Signal
  EXPECT_EQ(out.id, in.id);
  EXPECT_DOUBLE_EQ(out.value, in.value);
  EXPECT_EQ(e.kind(), EventType::Signal);
}

TEST(SignalPayloadRoundTrip, NaNValue_PreservesNaN) {
  // NaN == "no opinion" per the plan; the field must survive round-trip as NaN.
  const SignalPayload in{Symbol{1U}, std::numeric_limits<double>::quiet_NaN()};

  Event e{};
  e.store_payload(in);
  const auto out = e.payload_as<SignalPayload>();

  EXPECT_TRUE(std::isnan(out.value));
}

// =====================================================================
//  OrderPayload — round-trip through Event.
// =====================================================================

TEST(OrderPayloadRoundTrip, StoreThenLoad_PreservesAllFields) {
  const Symbol sym = Symbol{3U};
  const Timestamp ts = Timestamp::from_unix_nanos(1'000'000);
  const OrderPayload in{sym, 100LL, OrderType::Limit, Decimal::from_int(150), ts};

  Event e{};
  e.store_payload(in);
  e.type = EventType::Order;

  const auto out = e.payload_as<OrderPayload>(); // SAFETY: tag == Order
  EXPECT_EQ(out.id, in.id);
  EXPECT_EQ(out.qty, 100LL);
  EXPECT_EQ(out.type, OrderType::Limit);
  EXPECT_EQ(out.limit, Decimal::from_int(150));
  EXPECT_EQ(out.queued_at, ts);
  EXPECT_EQ(e.kind(), EventType::Order);
}

TEST(OrderPayloadRoundTrip, MarketOrder_PreservesAllFields) {
  const Symbol sym = Symbol{2U};
  const Timestamp ts = Timestamp::epoch();
  const OrderPayload in{sym, -50LL, OrderType::Market, Decimal::from_int(0), ts};

  Event e{};
  e.store_payload(in);
  e.type = EventType::Order;

  const auto out = e.payload_as<OrderPayload>();
  EXPECT_EQ(out.qty, -50LL);
  EXPECT_EQ(out.type, OrderType::Market);
  EXPECT_EQ(out.queued_at, Timestamp::epoch());
}

// =====================================================================
//  FillPayload — round-trip through Event.
// =====================================================================

TEST(FillPayloadRoundTrip, StoreThenLoad_PreservesAllFields) {
  const Symbol sym = Symbol{5U};
  const Decimal price = Decimal::from_raw(150'500'000'000LL); // 150.5
  const Decimal fee = Decimal::from_raw(500'000LL);           // 0.0005
  const Timestamp t = Timestamp::from_unix_nanos(9'000'000);
  const FillPayload in{sym, 200LL, price, fee, 0.001, t};

  Event e{};
  e.store_payload(in);
  e.type = EventType::Fill;

  const auto out = e.payload_as<FillPayload>(); // SAFETY: tag == Fill
  EXPECT_EQ(out.id, in.id);
  EXPECT_EQ(out.qty, 200LL);
  EXPECT_EQ(out.price, price);
  EXPECT_EQ(out.fee, fee);
  EXPECT_DOUBLE_EQ(out.impact, 0.001);
  EXPECT_EQ(out.t, t);
  EXPECT_EQ(e.kind(), EventType::Fill);
}

// =====================================================================
//  Signed-qty sign convention: positive = buy, negative = sell.
//
//  This is the canonical direction signal; downstream units (Portfolio,
//  ExecutionSimulator) read the sign of qty to determine direction.
// =====================================================================

TEST(SignedQtyConvention, PositiveQty_IsLongBuy) {
  // +qty means "buy this many shares long" — verify round-trip preserves sign.
  const OrderPayload buy{Symbol{1U}, +500LL, OrderType::Market, Decimal::from_int(0),
                         Timestamp::epoch()};
  Event e{};
  e.store_payload(buy);
  const auto out = e.payload_as<OrderPayload>();
  EXPECT_GT(out.qty, 0LL) << "positive qty must survive round-trip as positive (buy)";
}

TEST(SignedQtyConvention, NegativeQty_IsShortSell) {
  // −qty means "sell this many shares" — verify round-trip preserves sign.
  const OrderPayload sell{Symbol{1U}, -500LL, OrderType::Market, Decimal::from_int(0),
                          Timestamp::epoch()};
  Event e{};
  e.store_payload(sell);
  const auto out = e.payload_as<OrderPayload>();
  EXPECT_LT(out.qty, 0LL) << "negative qty must survive round-trip as negative (sell)";
}

TEST(SignedQtyConvention, FillQty_PreservesSign) {
  // Fill qty is also signed: +buy fill, −sell fill.
  const FillPayload buy_fill{Symbol{1U},           +100LL, Decimal::from_int(50),
                             Decimal::from_int(0), 0.0,    Timestamp::epoch()};
  const FillPayload sell_fill{Symbol{1U},           -100LL, Decimal::from_int(50),
                              Decimal::from_int(0), 0.0,    Timestamp::epoch()};
  Event e{};
  e.store_payload(buy_fill);
  EXPECT_GT(e.payload_as<FillPayload>().qty, 0LL);

  e.store_payload(sell_fill);
  EXPECT_LT(e.payload_as<FillPayload>().qty, 0LL);
}

// =====================================================================
//  make_signal_event — typed maker.
// =====================================================================

TEST(MakeSignalEvent, ValidPayload_SetsSignalType) {
  const SignalPayload p{Symbol{1U}, 0.8};
  const Timestamp ets = Timestamp::from_unix_nanos(1'000);
  const Timestamp kts = Timestamp::from_unix_nanos(1'000);

  const Event e = make_signal_event(p, kts, ets);
  EXPECT_EQ(e.kind(), EventType::Signal);
}

TEST(MakeSignalEvent, ValidPayload_PreservesTimestamps) {
  const SignalPayload p{Symbol{1U}, 0.5};
  const Timestamp ets = Timestamp::from_unix_nanos(2'000);
  const Timestamp kts = Timestamp::from_unix_nanos(3'000);

  const Event e = make_signal_event(p, kts, ets);
  EXPECT_EQ(e.event_ts, ets);
  EXPECT_EQ(e.knowledge_ts, kts);
}

TEST(MakeSignalEvent, ValidPayload_RoundTripsPayload) {
  const SignalPayload in{Symbol{42U}, -1.23};
  const Timestamp ts = Timestamp::from_unix_nanos(1'000);

  const Event e = make_signal_event(in, ts, ts);
  const auto out = e.payload_as<SignalPayload>(); // SAFETY: kind == Signal
  EXPECT_EQ(out.id, in.id);
  EXPECT_DOUBLE_EQ(out.value, in.value);
}

TEST(MakeSignalEvent, EqualTimestamps_Accepted) {
  // knowledge_ts == event_ts is the release-at-close normal case.
  const Timestamp t = Timestamp::from_unix_nanos(500);
  const Event e = make_signal_event(SignalPayload{Symbol{1U}, 0.0}, t, t);
  EXPECT_EQ(e.knowledge_ts, e.event_ts);
}

// =====================================================================
//  make_order_event — typed maker, returns Result<Event>.
// =====================================================================

TEST(MakeOrderEvent, ValidOrder_ReturnsOk) {
  const OrderPayload p{Symbol{1U}, 100LL, OrderType::Market, Decimal::from_int(0),
                       Timestamp::epoch()};
  const Timestamp ts = Timestamp::from_unix_nanos(1'000);
  const auto result = make_order_event(p, ts, ts);
  EXPECT_TRUE(result.has_value());
}

TEST(MakeOrderEvent, ValidOrder_SetsOrderType) {
  const OrderPayload p{Symbol{1U}, 50LL, OrderType::Limit, Decimal::from_int(200),
                       Timestamp::epoch()};
  const Timestamp ts = Timestamp::from_unix_nanos(1'000);
  const auto result = make_order_event(p, ts, ts);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->kind(), EventType::Order);
}

TEST(MakeOrderEvent, ValidOrder_PreservesTimestamps) {
  const OrderPayload p{Symbol{1U}, 10LL, OrderType::Market, Decimal::from_int(0),
                       Timestamp::epoch()};
  const Timestamp ets = Timestamp::from_unix_nanos(1'000);
  const Timestamp kts = Timestamp::from_unix_nanos(2'000);
  const auto result = make_order_event(p, kts, ets);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->event_ts, ets);
  EXPECT_EQ(result->knowledge_ts, kts);
}

TEST(MakeOrderEvent, ValidOrder_RoundTripsPayload) {
  const Timestamp ts = Timestamp::from_unix_nanos(5'000);
  const OrderPayload in{Symbol{7U}, -25LL, OrderType::Limit, Decimal::from_raw(99'999'000'000LL),
                        ts};
  const auto result = make_order_event(in, ts, ts);
  ASSERT_TRUE(result.has_value());
  const auto out = result->payload_as<OrderPayload>(); // SAFETY: kind == Order
  EXPECT_EQ(out.id, in.id);
  EXPECT_EQ(out.qty, in.qty);
  EXPECT_EQ(out.type, in.type);
  EXPECT_EQ(out.limit, in.limit);
  EXPECT_EQ(out.queued_at, in.queued_at);
}

TEST(MakeOrderEvent, ZeroQty_ReturnsErrInvalidArgument) {
  // qty == 0 is an expected-invalid input that returns Err, not an abort.
  const OrderPayload p{Symbol{1U}, 0LL, OrderType::Market, Decimal::from_int(0),
                       Timestamp::epoch()};
  const Timestamp ts = Timestamp::from_unix_nanos(1'000);
  const auto result = make_order_event(p, ts, ts);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(MakeOrderEvent, EpochZeroQueuedAt_Accepted) {
  // queued_at == epoch (0) is a valid value (e.g. a simulated t=0 order).
  const OrderPayload p{Symbol{1U}, 1LL, OrderType::Market, Decimal::from_int(0),
                       Timestamp::epoch()};
  const Timestamp ts = Timestamp::epoch();
  const auto result = make_order_event(p, ts, ts);
  EXPECT_TRUE(result.has_value());
}

// =====================================================================
//  make_fill_event — typed maker.
// =====================================================================

TEST(MakeFillEvent, ValidFill_SetsFillType) {
  const FillPayload p{Symbol{1U},           100LL, Decimal::from_int(50),
                      Decimal::from_int(0), 0.0,   Timestamp::epoch()};
  const Timestamp ts = Timestamp::from_unix_nanos(1'000);
  const Event e = make_fill_event(p, ts, ts);
  EXPECT_EQ(e.kind(), EventType::Fill);
}

TEST(MakeFillEvent, ValidFill_PreservesTimestamps) {
  const FillPayload p{Symbol{1U},           10LL, Decimal::from_int(100),
                      Decimal::from_int(0), 0.0,  Timestamp::epoch()};
  const Timestamp ets = Timestamp::from_unix_nanos(3'000);
  const Timestamp kts = Timestamp::from_unix_nanos(4'000);
  const Event e = make_fill_event(p, kts, ets);
  EXPECT_EQ(e.event_ts, ets);
  EXPECT_EQ(e.knowledge_ts, kts);
}

TEST(MakeFillEvent, ValidFill_RoundTripsPayload) {
  const Decimal price = Decimal::from_raw(200'000'000'000LL); // exactly 200.0
  const Decimal fee = Decimal::from_raw(1'500'000LL);         // 0.0000015
  const Timestamp t = Timestamp::from_unix_nanos(7'000);
  const FillPayload in{Symbol{9U}, 300LL, price, fee, 0.005, t};
  const Timestamp ts = Timestamp::from_unix_nanos(7'000);

  const Event e = make_fill_event(in, ts, ts);
  const auto out = e.payload_as<FillPayload>(); // SAFETY: kind == Fill
  EXPECT_EQ(out.id, in.id);
  EXPECT_EQ(out.qty, in.qty);
  EXPECT_EQ(out.price, price);
  EXPECT_EQ(out.fee, fee);
  EXPECT_DOUBLE_EQ(out.impact, in.impact);
  EXPECT_EQ(out.t, in.t);
}

// =====================================================================
//  Decimal price exactness — no binary-floating-point drift for money.
// =====================================================================

TEST(DecimalPriceExactness, FillPrice_ExactAfterRoundTrip) {
  // 150.5 expressed exactly as Decimal (raw mantissa: 150_500_000_000).
  const Decimal exact_price = Decimal::from_raw(150'500'000'000LL);
  const Decimal also_150_5 = Decimal::from_raw(150'500'000'000LL);
  EXPECT_EQ(exact_price, also_150_5) << "Decimal equality must hold for the same raw mantissa";

  const FillPayload in{Symbol{1U}, 1LL, exact_price, Decimal::from_int(0), 0.0, Timestamp::epoch()};
  Event e{};
  e.store_payload(in);
  const auto out = e.payload_as<FillPayload>();
  EXPECT_EQ(out.price, exact_price) << "price must be bit-identical after memcpy round-trip";
}

TEST(DecimalPriceExactness, OrderLimit_ExactAfterRoundTrip) {
  const Decimal limit = Decimal::from_raw(99'750'000'000LL); // 99.75
  const OrderPayload in{Symbol{1U}, 1LL, OrderType::Limit, limit, Timestamp::epoch()};
  Event e{};
  e.store_payload(in);
  const auto out = e.payload_as<OrderPayload>();
  EXPECT_EQ(out.limit, limit);
}

// =====================================================================
//  OrderType::to_string — exhaustive-switch compiles (no `default`).
// =====================================================================

TEST(OrderTypeToString, AllEnumerators_HaveNames) {
  EXPECT_EQ(to_string(OrderType::Market), std::string_view{"Market"});
  EXPECT_EQ(to_string(OrderType::Limit), std::string_view{"Limit"});
}

TEST(OrderTypeToString, IsConstexpr) {
  constexpr std::string_view market_sv = to_string(OrderType::Market);
  static_assert(market_sv == std::string_view{"Market"});
  EXPECT_EQ(market_sv, std::string_view{"Market"});
}


}  // namespace atxtest_payload_test
