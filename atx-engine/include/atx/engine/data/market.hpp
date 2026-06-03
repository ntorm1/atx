#pragma once

// atx::engine::data — the bitemporal market payload that rides the event bus (P1-2).
//
// Defines:
//   MarketPayload — the Phase-1 market record: a Symbol, the survivorship
//                   `delisted_final` flag, and a tagged union over atx-core's
//                   `domain::Bar` / `domain::Tick`. It is the payload POD stored
//                   inside an Event's raw-byte buffer (see event.hpp): trivially
//                   copyable and ≤ kPayloadBytes, so it memcpy's into a ring slot
//                   with no special members.
//   make_market_bar / make_market_tick — the typed makers that wrap a Bar/Tick
//                   into an Event, enforcing the bitemporal invariant
//                   (knowledge_ts ≥ event_ts) and applying the release-at-close
//                   default (a normal historical bar is known at its own ts).
//
// Why a tagged C union (not std::variant, not two inline members):
//   * std::variant is neither trivially copyable nor a fixed size — it cannot
//     ride the memcpy ring (same rationale as event.hpp's raw-byte payload).
//   * Two inline `Bar bar; Tick tick;` members would sum past the kPayloadBytes=64
//     ring-slot budget. A union overlays the two arms so the payload stays ~56 B.
//   * The active arm is named by `kind`; reading the other arm is undefined
//     behaviour, so raw union access is private and reads go through the
//     tag-guarded as_bar()/as_tick() accessors (ATX_ASSERT on the tag in debug).
//   Both arms are trivially copyable, so the union — and MarketPayload — stay
//   trivially copyable (verified by the static_assert below).
//
// Bitemporal contract (plan P1-2): `knowledge_ts ≥ event_ts` — a fact cannot be
// known before it is true. The makers ATX_ASSERT this (aborts in debug). The
// release-at-close rule is expressed by the caller passing `bar.ts` as the
// knowledge_ts for the normal case; later data (restatements, survivorship-bias
// corrections) passes a strictly later knowledge_ts.
//
// Ownership/threading: MarketPayload is a Rule-of-Zero value type with no owned
// resources. Thread safety is the bus's concern, not the POD's.

#include <type_traits> // std::is_trivially_copyable_v static_assert guards

#include "atx/core/datetime.hpp"      // atx::core::time::Timestamp
#include "atx/core/domain/domain.hpp" // atx::core::domain::Bar, Tick
#include "atx/core/domain/symbol.hpp" // atx::core::domain::Symbol
#include "atx/core/macro.hpp"         // ATX_ASSERT (aborts in debug)
#include "atx/core/types.hpp"         // atx::u8

#include "atx/engine/event/event.hpp" // Event, EventType, kPayloadBytes

namespace atx::engine::data {

// =====================================================================
//  MarketPayload — Symbol + survivorship flag + tagged union{Bar, Tick}.
//
//  Layout leaves room for the Phase-3/5 point-in-time fields to be appended
//  without a breaking change (the ring slot is fixed at kPayloadBytes, and this
//  payload only spends ~56 B of it).
// =====================================================================
struct MarketPayload {
  // The two payload arms this record can carry.
  enum class Kind : atx::u8 { Bar, Tick };

  atx::core::domain::Symbol symbol{}; // which instrument
  Kind kind{Kind::Bar};               // names the active union arm
  bool delisted_final{false};         // survivorship: last record before delisting

  // SAFETY: tagged union — read ONLY the arm named by `kind` (via as_bar/as_tick).
  //         Both arms are trivially copyable, so MarketPayload stays trivially
  //         copyable and rides the memcpy ring. A union (not two inline members)
  //         is REQUIRED to fit kPayloadBytes=64. The default member initializer
  //         `bar{}` makes `kind==Bar` consistent with a value-initialised union
  //         and keeps the type trivially copyable + default-constructible.
  union {
    atx::core::domain::Bar bar{};
    atx::core::domain::Tick tick;
  };

  // Tag-guarded read of the Bar arm. PRECONDITION: kind == Kind::Bar (ABORTS in
  // debug); reading the wrong arm would be undefined behaviour.
  [[nodiscard]] const atx::core::domain::Bar &as_bar() const noexcept {
    ATX_ASSERT(kind == Kind::Bar);
    // SAFETY: tag-guarded read of the active member (defined behaviour); the
    //         union is required for the ring-slot size budget (see header note).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
    return bar;
  }

  // Tag-guarded read of the Tick arm. PRECONDITION: kind == Kind::Tick (ABORTS in
  // debug); reading the wrong arm would be undefined behaviour.
  [[nodiscard]] const atx::core::domain::Tick &as_tick() const noexcept {
    ATX_ASSERT(kind == Kind::Tick);
    // SAFETY: tag-guarded read of the active member (defined behaviour); the
    //         union is required for the ring-slot size budget (see header note).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
    return tick;
  }
};

// Trivially copyable: MarketPayload is stored into Event's byte buffer by memcpy
// (store_payload/payload_as). The union of two trivially-copyable arms keeps it so.
static_assert(std::is_trivially_copyable_v<MarketPayload>,
              "MarketPayload must be trivially copyable to ride the memcpy ring");

// Fits the fixed ring-slot payload budget (56 ≤ 64) with headroom for the
// Phase-3/5 PIT fields. Growing past kPayloadBytes would reshape every ring.
static_assert(sizeof(MarketPayload) <= atx::engine::event::kPayloadBytes,
              "MarketPayload exceeds kPayloadBytes — would reshape the Disruptor ring");
// Pin the actual size so the "headroom" narrative above stays honest: a new
// field that silently eats the budget trips this before it reaches the ring.
static_assert(
    sizeof(MarketPayload) <= 56,
    "MarketPayload grew past 56 B — update the headroom note and re-check the ring budget");

// =====================================================================
//  Typed makers — wrap a Bar/Tick into an Event with the bitemporal invariant.
// =====================================================================

/// Wrap `bar` for `symbol` into a Market Event.
///
/// PRECONDITION (bitemporal): knowledge_ts ≥ bar.ts — a fact cannot be known
/// before it is true (ATX_ASSERT; aborts in debug). Release-at-close: pass
/// `bar.ts` as knowledge_ts for a normal historical bar; pass a strictly later
/// knowledge_ts for late/restated data. `event_ts` is set to `bar.ts`.
[[nodiscard]] inline event::Event make_market_bar(atx::core::domain::Symbol symbol,
                                                  const atx::core::domain::Bar &bar,
                                                  atx::core::time::Timestamp knowledge_ts,
                                                  bool delisted_final = false) noexcept {
  ATX_ASSERT(knowledge_ts.unix_nanos() >= bar.ts.unix_nanos());

  // Aggregate-init sets the Bar arm (the union's first member) active at
  // construction — no post-construction union-member write to flag.
  const MarketPayload mp{symbol, MarketPayload::Kind::Bar, delisted_final, {bar}};

  event::Event e{};
  e.type = event::EventType::Market;
  e.event_ts = bar.ts;
  e.knowledge_ts = knowledge_ts;
  e.store_payload(mp);
  return e;
}

/// Wrap `tick` for `symbol` into a Market Event.
///
/// PRECONDITION (bitemporal): knowledge_ts ≥ tick.ts (ATX_ASSERT; aborts in
/// debug). Release-at-close analogue: pass `tick.ts` for the normal case.
/// `event_ts` is set to `tick.ts`.
[[nodiscard]] inline event::Event make_market_tick(atx::core::domain::Symbol symbol,
                                                   const atx::core::domain::Tick &tick,
                                                   atx::core::time::Timestamp knowledge_ts,
                                                   bool delisted_final = false) noexcept {
  ATX_ASSERT(knowledge_ts.unix_nanos() >= tick.ts.unix_nanos());

  // Designated-init {.tick = …} makes the Tick arm the active union member at
  // construction — no post-construction union-member write to flag.
  const MarketPayload mp{symbol, MarketPayload::Kind::Tick, delisted_final, {.tick = tick}};

  event::Event e{};
  e.type = event::EventType::Market;
  e.event_ts = tick.ts;
  e.knowledge_ts = knowledge_ts;
  e.store_payload(mp);
  return e;
}

} // namespace atx::engine::data
