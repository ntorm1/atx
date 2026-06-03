#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>

#include "atx/core/datetime.hpp"
#include "atx/core/platform.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/event/event.hpp"

namespace {

using atx::engine::event::Event;
using atx::engine::event::EventType;
using atx::engine::event::kPayloadBytes;
using atx::engine::event::to_string;
using Timestamp = atx::core::time::Timestamp;

// A small trivially-copyable POD used to exercise the generic payload accessors
// without depending on the P1-2 MarketPayload type (which does not exist yet).
struct ProbePayload {
  atx::i64 a{};
  double b{};
};
static_assert(std::is_trivially_copyable_v<ProbePayload>);
static_assert(sizeof(ProbePayload) <= kPayloadBytes);

// A payload that exactly fills the buffer — exercises the upper boundary the
// `sizeof(P) <= kPayloadBytes` guard protects (a 1-byte-larger payload would
// fail that static_assert at compile time).
struct FullPayload {
  std::array<std::byte, kPayloadBytes> raw{};
};
static_assert(std::is_trivially_copyable_v<FullPayload>);
static_assert(sizeof(FullPayload) == kPayloadBytes);

// --------------------------------------------------------------------------
//  Type-level guarantees (these duplicate the header's static_asserts, but a
//  failing compile here localises the regression to the event taxonomy).
// --------------------------------------------------------------------------
TEST(EventTaxonomy, Event_IsTriviallyCopyable) { EXPECT_TRUE(std::is_trivially_copyable_v<Event>); }

TEST(EventTaxonomy, Event_IsCacheLineAligned) {
  EXPECT_EQ(alignof(Event), atx::core::kCacheLineSize);
  EXPECT_EQ(alignof(Event), 64U);
}

TEST(EventTaxonomy, EventType_UnderlyingTypeIsU8) {
  EXPECT_TRUE((std::is_same_v<std::underlying_type_t<EventType>, atx::u8>));
}

// --------------------------------------------------------------------------
//  Default-constructed Event is well-formed.
// --------------------------------------------------------------------------
TEST(EventTaxonomy, Event_DefaultConstructed_IsWellFormed) {
  const Event e{};
  EXPECT_EQ(e.knowledge_ts, Timestamp::epoch());
  EXPECT_EQ(e.event_ts, Timestamp::epoch());
  EXPECT_EQ(e.type, EventType::Market); // Market == 0, the zero-init tag
  EXPECT_EQ(e.kind(), EventType::Market);
}

TEST(EventTaxonomy, Event_Kind_ReturnsTypeTag) {
  Event e{};
  e.type = EventType::Order;
  EXPECT_EQ(e.kind(), EventType::Order);
}

// --------------------------------------------------------------------------
//  Payload round-trip through the generic memcpy accessors.
// --------------------------------------------------------------------------
TEST(EventTaxonomy, Payload_StoreThenLoad_RoundTrips) {
  Event e{};
  const ProbePayload in{.a = -42, .b = 3.5};
  e.store_payload(in);
  e.type = EventType::Signal; // caller tags the active payload

  const auto out = e.payload_as<ProbePayload>(); // SAFETY: tag == known
  EXPECT_EQ(out.a, in.a);
  EXPECT_DOUBLE_EQ(out.b, in.b);
  EXPECT_EQ(e.kind(), EventType::Signal);
}

TEST(EventTaxonomy, Payload_DefaultEvent_ReadsZeroBytes) {
  const Event e{};
  const auto out = e.payload_as<ProbePayload>();
  EXPECT_EQ(out.a, 0);
  EXPECT_DOUBLE_EQ(out.b, 0.0);
}

TEST(EventTaxonomy, Payload_FullBufferSize_RoundTrips) {
  // Fill every byte of a kPayloadBytes-sized payload with distinct non-zero
  // values so a partial/truncated copy would be detected.
  FullPayload in{};
  atx::u8 next = 1U; // 1..kPayloadBytes, all non-zero
  for (std::byte &b : in.raw) {
    b = static_cast<std::byte>(next);
    ++next;
  }

  Event e{};
  e.store_payload(in);
  e.type = EventType::Fill;

  const auto out = e.payload_as<FullPayload>(); // SAFETY: tag == known
  EXPECT_EQ(out.raw, in.raw);                   // std::array operator== is element-wise
  EXPECT_EQ(e.kind(), EventType::Fill);
}

// --------------------------------------------------------------------------
//  Timestamp ordering on the two temporal axes.
// --------------------------------------------------------------------------
TEST(EventTaxonomy, Timestamps_Order_KnowledgeAfterEvent) {
  Event e{};
  e.event_ts = Timestamp::from_unix_nanos(1'000);
  e.knowledge_ts = Timestamp::from_unix_nanos(2'000);
  EXPECT_LT(e.event_ts, e.knowledge_ts);
  EXPECT_GT(e.knowledge_ts, e.event_ts);
  EXPECT_NE(e.knowledge_ts, e.event_ts);
}

TEST(EventTaxonomy, Timestamps_Boundary_KnowledgeEqualsEvent) {
  Event e{};
  const Timestamp t = Timestamp::from_unix_nanos(5'000);
  e.event_ts = t;
  e.knowledge_ts = t;
  EXPECT_EQ(e.knowledge_ts, e.event_ts);
  EXPECT_FALSE(e.knowledge_ts < e.event_ts);
  EXPECT_FALSE(e.knowledge_ts > e.event_ts);
}

TEST(EventTaxonomy, Timestamps_Boundary_EpochZero) {
  Event e{};
  e.event_ts = Timestamp::epoch();
  e.knowledge_ts = Timestamp::epoch();
  EXPECT_EQ(e.knowledge_ts.unix_nanos(), 0);
  EXPECT_EQ(e.event_ts.unix_nanos(), 0);
  EXPECT_EQ(e.knowledge_ts, e.event_ts);
}

// --------------------------------------------------------------------------
//  Exhaustive-switch helper covers all four enumerators (no `default`).
// --------------------------------------------------------------------------
TEST(EventTaxonomy, ToString_AllEnumerators_HaveNames) {
  EXPECT_EQ(to_string(EventType::Market), std::string_view{"Market"});
  EXPECT_EQ(to_string(EventType::Signal), std::string_view{"Signal"});
  EXPECT_EQ(to_string(EventType::Order), std::string_view{"Order"});
  EXPECT_EQ(to_string(EventType::Fill), std::string_view{"Fill"});
}

TEST(EventTaxonomy, ToString_IsConstexpr) {
  // Compile-time evaluation proves the helper is usable in constant contexts.
  constexpr std::string_view market = to_string(EventType::Market);
  static_assert(market == std::string_view{"Market"});
  EXPECT_EQ(market, std::string_view{"Market"});
}

} // namespace
