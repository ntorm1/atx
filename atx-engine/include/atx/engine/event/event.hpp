#pragma once

// atx::engine::event — the event taxonomy every bus message rides on (P1-1).
//
// Defines:
//   EventType  — a closed, exhaustive tag {Market, Signal, Order, Fill}. Closed
//                so exhaustive switches need no `default`: adding an enumerator
//                later is a compile warning (/W4) at every switch, not a silent
//                fall-through. Phase 1 only populates Market; the other three are
//                reserved now to freeze the vocabulary across phases.
//   Event      — a compact, cache-aligned, trivially-copyable POD carrying two
//                temporal axes (knowledge_ts / event_ts) and a tagged raw-byte
//                payload. It is the LMAX-style "mutate a pre-allocated ring entry
//                in place" object: every Disruptor ring slot is one Event, copied
//                by a flat memcpy with no indirect dispatch and no non-trivial
//                special members.
//
// Why a raw-byte payload + memcpy accessors instead of std::variant or a union:
//   * std::variant has non-trivial special members and an index discriminator —
//     it is neither trivially copyable nor a stable ring-slot size, so it cannot
//     ride a memcpy ring.
//   * A C-style union would require tracking the *active member*; reading the
//     wrong member is undefined behaviour. We instead store the payload as an
//     opaque std::byte buffer and (de)serialise concrete trivially-copyable PODs
//     through std::memcpy. memcpy into/out of a byte buffer is always defined for
//     trivially-copyable types (it begins the object's lifetime), so there is no
//     active-member UB — the discriminator is the `type` tag, and the caller's
//     precondition is `tag == the stored payload's type` (see store_payload).
//
// Ownership/threading: Event is a value type with no owned resources (Rule of
// Zero). It is trivially copyable, so a producer may build one on the stack and
// the bus may memcpy it into a ring slot with no special-member calls. Thread
// safety is the bus's concern (publication/consumption fences), not the POD's.

#include <compare>      // operator<=> on the Timestamp members
#include <cstddef>      // std::byte
#include <cstring>      // std::memcpy
#include <string_view>  // to_string return type
#include <type_traits>  // std::is_trivially_copyable_v, static_assert guards

#include "atx/core/datetime.hpp"  // atx::core::time::Timestamp
#include "atx/core/platform.hpp"  // ATX_CACHE_ALIGNED, atx::core::kCacheLineSize
#include "atx/core/types.hpp"     // atx::u8, atx::usize

namespace atx::engine::event {

// =====================================================================
//  EventType — closed taxonomy of bus messages.
//
//  u8 underlying type keeps the tag to a single byte inside Event. The
//  enumerators default-number from 0, so a value-initialised Event has
//  type == Market (0) — the natural Phase-1 default.
// =====================================================================
enum class EventType : u8 { Market, Signal, Order, Fill };  // closed; no `default` in switches

// Payload buffer size, fixed once across all phases.
//
// Sized to hold the Phase-1 MarketPayload (~56 B: Symbol(4) + tag/flags +
// union{Bar, Tick} where Bar ~= 48 B) with headroom for the Phase-2
// Signal/Order/Fill payloads. Fixing it now fixes the ring-slot size across
// phases: growing kPayloadBytes later reshapes every Disruptor ring (plan §10
// risk), so we pay the headroom up front rather than re-cut the ring later.
inline constexpr atx::usize kPayloadBytes = 64;

// =====================================================================
//  Event — the hot-path POD that rides the Disruptor ring.
//
//  Bitemporal: `knowledge_ts` is the clock-gating axis (when the engine is
//  *allowed* to see the event — sort/visibility key), `event_ts` is the
//  true-at axis (e.g. bar close). The two are independent values; ordering on
//  each axis comes free from Timestamp's defaulted operator<=>.
// =====================================================================
struct ATX_CACHE_ALIGNED Event {
  atx::core::time::Timestamp knowledge_ts{};  // clock-gating axis (sort/visibility)
  atx::core::time::Timestamp event_ts{};      // true-at axis (e.g. bar close)
  EventType                  type{};          // discriminator for the payload bytes

  // SAFETY: the payload bytes are only meaningful when interpreted as the POD
  //         whose EventType matches `type`. Access exclusively via payload_as<P>
  //         with `tag == type` as the caller's precondition. alignas(8) lets any
  //         payload up to 8-byte alignment be memcpy'd in/out without a misaligned
  //         access; the buffer is value-initialised to all-zero bytes.
  alignas(8) std::byte payload[kPayloadBytes]{};

  // The discriminator. Callers switch on this to pick the payload type.
  [[nodiscard]] constexpr EventType kind() const noexcept { return type; }

  // Serialise a concrete payload POD into the byte buffer.
  //
  // Precondition (caller): after store_payload, set `type` to the EventType that
  // names P so a later payload_as<P> is valid. store_payload does NOT touch
  // `type` — the maker functions (P1-2) own the tag/payload pairing.
  template <class P>
  void store_payload(const P& p) noexcept {
    static_assert(std::is_trivially_copyable_v<P>,
                  "payload must be trivially copyable to ride the memcpy ring");
    static_assert(sizeof(P) <= kPayloadBytes,
                  "payload exceeds kPayloadBytes — grow kPayloadBytes (reshapes the ring)");
    // SAFETY: P is trivially copyable and fits; memcpy into the byte buffer
    //         begins P's lifetime there. Caller sets `type` to match P.
    std::memcpy(payload, &p, sizeof(P));
  }

  // Deserialise the byte buffer back into a copy of payload POD P.
  template <class P>
  [[nodiscard]] P payload_as() const noexcept {
    static_assert(std::is_trivially_copyable_v<P>,
                  "payload must be trivially copyable to ride the memcpy ring");
    static_assert(sizeof(P) <= kPayloadBytes,
                  "payload exceeds kPayloadBytes — grow kPayloadBytes (reshapes the ring)");
    P out{};  // value-init then overwrite; P is trivially copyable
    // SAFETY: `type == the EventType that names P` is the caller's precondition
    //         (see kind()). memcpy out of the byte buffer is defined for any
    //         trivially-copyable P; reading bytes never stored as P is logically
    //         meaningless but not UB (it is a copy of well-defined byte storage).
    std::memcpy(&out, payload, sizeof(P));
    return out;
  }
};

// =====================================================================
//  Regression guards — these encode the hot-path invariants the Disruptor
//  ring relies on. A change that breaks one is a compile error here.
// =====================================================================

// Trivially copyable: the bus memcpy's Event into ring slots with no special
// members. This is the single most load-bearing property of the type.
static_assert(std::is_trivially_copyable_v<Event>,
              "Event must be trivially copyable to ride the Disruptor ring by memcpy");

// Standard layout: holds (no virtuals, no mixed-access non-static members), so
// the byte layout is predictable and ABI-stable across translation units.
static_assert(std::is_standard_layout_v<Event>,
              "Event must be standard-layout for a stable, memcpy-able byte layout");

// Cache-line aligned: one Event per ring slot, no false sharing across slots.
static_assert(alignof(Event) == atx::core::kCacheLineSize,
              "Event must be cache-line aligned (one event per ring slot)");

// Size guard. Layout: two Timestamp (8B each = 16B) + EventType (1B) ... then
// the alignas(8) payload buffer forces 7B of tail-of-tag padding to reach an
// 8-byte boundary (16 + 1 + 7 = 24), + 64B payload = 88B; rounded up to the
// 64B alignment of the struct gives 128B. Fixing the size here documents the
// ring-slot footprint; the plan permits a >64B Event (it is the payload, not
// the struct, that is bounded at kPayloadBytes).
static_assert(sizeof(Event) == 128,
              "Event size changed — ring-slot footprint is load-bearing; update intentionally");

// =====================================================================
//  to_string — exhaustive-switch helper over EventType.
//
//  Demonstrates (and forces) the closed-taxonomy discipline: the switch has NO
//  `default`, so adding an enumerator makes the compiler flag this function
//  under /W4. constexpr so it is usable in constant contexts and for cheap
//  logging/diagnostics. Returns a static string_view (no allocation).
// =====================================================================
[[nodiscard]] constexpr std::string_view to_string(EventType t) noexcept {
  switch (t) {
    case EventType::Market:
      return "Market";
    case EventType::Signal:
      return "Signal";
    case EventType::Order:
      return "Order";
    case EventType::Fill:
      return "Fill";
  }
  // Unreachable for a valid EventType. A value outside the enumerators can only
  // arise from a bit-corrupted cast (UB at the cast site); we return a sentinel
  // rather than std::unreachable() so a corrupted tag fails loud, not silently.
  return "Unknown";
}

}  // namespace atx::engine::event
