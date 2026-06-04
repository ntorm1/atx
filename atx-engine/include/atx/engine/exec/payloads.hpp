#pragma once

// atx::engine::exec — Signal, Order, Fill payload PODs + typed Event makers (P2-1).
//
// Completes the Phase-1 EventType taxonomy: the EventType tags {Signal, Order,
// Fill} were reserved in event.hpp (P1-1) and the ring slot was sized for them
// (kPayloadBytes = 64). This header provides the three payload PODs that ride
// those slots and the maker functions that wrap them into Event objects.
//
// ------------------------------------------------------------------
//  Ring-slot contract (trivially copyable + size-bounded)
// ------------------------------------------------------------------
//   Every payload here is a trivially-copyable POD. This is the single most
//   load-bearing property: Event::store_payload / payload_as operate via
//   std::memcpy, which is defined for trivially-copyable types (it begins the
//   object's lifetime in the byte buffer). Non-trivially-copyable payloads
//   cannot ride the Disruptor ring (std::variant, std::string, etc. are
//   disallowed as payload members for this reason).
//
//   All three payloads are statically bounded to kPayloadBytes (64 B). The ring
//   slot size is fixed across phases; growing kPayloadBytes would reshape every
//   ring allocation (plan §10 risk). We pay the headroom up front.
//
// ------------------------------------------------------------------
//  Side convention: signed qty is canonical
// ------------------------------------------------------------------
//   Direction is encoded in the sign of `qty` (i64):
//     +qty → buy  (long, increase long / close short)
//     −qty → sell (short, decrease long / open short)
//
//   This matches the LMAX / Disruptor idiom of keeping the ring-slot as
//   minimal as possible. An explicit atx::core::domain::Side field would be
//   redundant and could diverge. Downstream units (Portfolio, ExecutionSimulator)
//   derive direction exclusively from the sign of qty; they do not store Side.
//   NOTE: atx::core::domain::Side {Buy, Sell} exists in atx-core and is NOT
//   redefined here (see plan-adjustment 1 in phase-2-progress.md).
//
// ------------------------------------------------------------------
//  Money fields: exact Decimal, never naked double
// ------------------------------------------------------------------
//   Price and fee fields use atx::core::Decimal (9-decimal-place fixed-point,
//   backed by an i64 mantissa). This prevents the binary-floating-point drift
//   that accumulates across fill/commission/P&L calculations. `impact` is the
//   one deliberate exception: it is a unitless fractional heuristic, not a
//   money value — f64 suffices and an exact type would be misleading.
//
// ------------------------------------------------------------------
//  Bitemporal contract (inherited from Phase-1)
// ------------------------------------------------------------------
//   knowledge_ts ≥ event_ts — a fact cannot be known before it is true.
//   The typed makers ATX_ASSERT this (programmer-error precondition, aborts in
//   debug). make_order_event additionally validates qty != 0 via Result/Err
//   (expected-invalid input, not a programmer error).

#include <string_view> // to_string return type
#include <type_traits> // static_assert guards

#include "atx/core/datetime.hpp" // atx::core::time::Timestamp
#include "atx/core/decimal.hpp"  // atx::core::Decimal
#include "atx/core/error.hpp"    // Result, Err, Ok, ErrorCode
#include "atx/core/macro.hpp"    // ATX_ASSERT
#include "atx/core/types.hpp"    // atx::i64, atx::f64, atx::u8

#include "atx/engine/event/event.hpp" // Event, EventType, kPayloadBytes
#include "atx/engine/loop/types.hpp" // InstrumentId (== atx::core::domain::Symbol); pulls in symbol.hpp

namespace atx::engine::exec {

// =====================================================================
//  OrderType — closed taxonomy of order kinds.
//
//  u8 underlying to match the ring-slot discipline (single byte); exhaustive
//  switches need no `default` — adding an enumerator surfaces as a /W4
//  warning at every switch, not a silent fall-through.
// =====================================================================
enum class OrderType : atx::u8 { Market, Limit }; // closed; no `default` in switches

/// Exhaustive-switch helper over OrderType. No `default` so a new enumerator
/// becomes a compiler warning (/W4). Returns a static string_view.
[[nodiscard]] constexpr std::string_view to_string(OrderType t) noexcept {
  switch (t) {
  case OrderType::Market:
    return "Market";
  case OrderType::Limit:
    return "Limit";
  }
  // Unreachable for a valid OrderType. Sentinel on bit-corrupted cast.
  return "Unknown";
}

// =====================================================================
//  Payload PODs — trivially copyable, ride the Disruptor ring.
// =====================================================================

// ------------------------------------------------------------------
//  SignalPayload — alpha VM output per instrument (P2-1).
//
//  Carries one signal score per instrument tick. `value` == NaN means
//  "no opinion": the strategy has no conviction for this instrument on
//  this bar. Downstream WeightPolicy (P2-4) filters NaN scores before
//  constructing the target portfolio.
// ------------------------------------------------------------------
struct SignalPayload {
  InstrumentId id{}; // which instrument this signal is for
  atx::f64 value{};  // alpha score; NaN == no opinion (not-a-number sentinel)
};

// ------------------------------------------------------------------
//  OrderPayload — intent to transact (P2-1).
//
//  Direction is encoded in the sign of `qty` (CANONICAL — see header note):
//    +qty → buy  (long entry / short cover)
//    −qty → sell (long exit / short entry)
//
//  `limit` is the limit price for OrderType::Limit; it is ignored (and
//  conventionally zero) for OrderType::Market. Using Decimal preserves
//  exact price levels across the ring without FP drift.
//
//  `queued_at` is the timestamp the order was placed into the queue. It
//  may be epoch (zero) for simulated t=0 orders — the makers accept it.
// ------------------------------------------------------------------
struct OrderPayload {
  InstrumentId id{};                    // instrument
  atx::i64 qty{};                       // signed shares: +buy / −sell (CANONICAL direction)
  OrderType type{OrderType::Market};    // Market or Limit
  atx::core::Decimal limit;             // limit price (exact money); ignored for Market
  atx::core::time::Timestamp queued_at; // when the order entered the queue
};

// ------------------------------------------------------------------
//  FillPayload — confirmed execution record (P2-1).
//
//  `qty` sign matches OrderPayload convention: +buy fill / −sell fill.
//  `price` and `fee` are exact Decimal (money fields). `impact` is a
//  unitless fractional market-impact estimate — f64 is appropriate here
//  because it is an approximation, not an exact money quantity.
// ------------------------------------------------------------------
struct FillPayload {
  InstrumentId id{};            // instrument that was filled
  atx::i64 qty{};               // signed shares filled: +buy / −sell
  atx::core::Decimal price;     // exact fill price
  atx::core::Decimal fee;       // exact total commission/fee charged
  atx::f64 impact{};            // unitless fractional market-impact (heuristic, not exact money)
  atx::core::time::Timestamp t; // fill timestamp
};

// =====================================================================
//  Regression guards — encode the ring-slot invariants as compile errors.
// =====================================================================

static_assert(std::is_trivially_copyable_v<SignalPayload>,
              "SignalPayload must be trivially copyable to ride the memcpy ring");
static_assert(std::is_trivially_copyable_v<OrderPayload>,
              "OrderPayload must be trivially copyable to ride the memcpy ring");
static_assert(std::is_trivially_copyable_v<FillPayload>,
              "FillPayload must be trivially copyable to ride the memcpy ring");

static_assert(sizeof(SignalPayload) <= atx::engine::event::kPayloadBytes,
              "SignalPayload exceeds kPayloadBytes — would reshape the Disruptor ring");
static_assert(sizeof(OrderPayload) <= atx::engine::event::kPayloadBytes,
              "OrderPayload exceeds kPayloadBytes — would reshape the Disruptor ring");
static_assert(sizeof(FillPayload) <= atx::engine::event::kPayloadBytes,
              "FillPayload exceeds kPayloadBytes — would reshape the Disruptor ring");

// =====================================================================
//  Typed Event makers — mirror make_market_bar (P1-2) pattern.
//
//  Each maker sets the EventType tag, stores the payload via store_payload,
//  and enforces the bitemporal invariant (knowledge_ts ≥ event_ts).
//  make_order_event returns Result<Event> to propagate the qty==0 guard;
//  the others are infallible and return Event directly.
// =====================================================================

/// Wrap a SignalPayload into an Event with EventType::Signal.
///
/// PRECONDITION (bitemporal): knowledge_ts ≥ event_ts (ATX_ASSERT; programmer
/// error — aborts in debug, same discipline as make_market_bar).
[[nodiscard]] inline atx::engine::event::Event
make_signal_event(const SignalPayload &p, atx::core::time::Timestamp knowledge_ts,
                  atx::core::time::Timestamp event_ts) noexcept {
  ATX_ASSERT(knowledge_ts.unix_nanos() >= event_ts.unix_nanos());

  atx::engine::event::Event e{};
  e.type = atx::engine::event::EventType::Signal;
  e.event_ts = event_ts;
  e.knowledge_ts = knowledge_ts;
  e.store_payload(p);
  return e;
}

/// Wrap an OrderPayload into an Event with EventType::Order.
///
/// Returns Err(InvalidArgument) if qty == 0 — a zero-quantity order is an
/// expected-invalid input (not a programmer error), so it travels as an Err
/// rather than triggering ATX_ASSERT.
///
/// PRECONDITION (bitemporal): knowledge_ts ≥ event_ts (ATX_ASSERT; programmer
/// error — aborts in debug).
[[nodiscard]] inline atx::core::Result<atx::engine::event::Event>
make_order_event(const OrderPayload &p, atx::core::time::Timestamp knowledge_ts,
                 atx::core::time::Timestamp event_ts) noexcept {
  ATX_ASSERT(knowledge_ts.unix_nanos() >= event_ts.unix_nanos());

  if (p.qty == 0) {
    return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                          "order qty must be non-zero (sign encodes direction)");
  }

  atx::engine::event::Event e{};
  e.type = atx::engine::event::EventType::Order;
  e.event_ts = event_ts;
  e.knowledge_ts = knowledge_ts;
  e.store_payload(p);
  return atx::core::Ok(e);
}

/// Wrap a FillPayload into an Event with EventType::Fill.
///
/// PRECONDITION (bitemporal): knowledge_ts ≥ event_ts (ATX_ASSERT; programmer
/// error — aborts in debug).
[[nodiscard]] inline atx::engine::event::Event
make_fill_event(const FillPayload &p, atx::core::time::Timestamp knowledge_ts,
                atx::core::time::Timestamp event_ts) noexcept {
  ATX_ASSERT(knowledge_ts.unix_nanos() >= event_ts.unix_nanos());

  atx::engine::event::Event e{};
  e.type = atx::engine::event::EventType::Fill;
  e.event_ts = event_ts;
  e.knowledge_ts = knowledge_ts;
  e.store_payload(p);
  return e;
}

} // namespace atx::engine::exec
