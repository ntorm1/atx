// data_handler_test.cpp — behavioural tests for atx::engine::data::InMemoryBarFeed (P1-5).
//
// The DataHandler is the no-look-ahead k-way merge at the heart of the Phase-1
// spine: it merges N knowledge_ts-sorted BarRow sources into one chronological,
// coalesced stream; advances the SimClock to each frontier BEFORE publishing that
// frontier's slice onto the EventBus; and carries delisted symbols' final bars
// (survivorship defence). The CALLER drains the bus between steps — these tests
// register a recording consumer and drain_in_order() after each step() to observe
// the published slice and assert the engine's invariants.
//
// Tests follow the Subject_Condition_Expected convention and pin the P1-5 contract:
//   - Chronological emission: published knowledge_ts is non-decreasing across the
//     whole replay.
//   - Coalescing: two sources at the SAME frontier ts → one step() publishes BOTH,
//     in (ts, source_idx) order, before the caller drains.
//   - No look-ahead: at drain time every published event has
//     knowledge_ts <= clock.now() (the gate the whole engine rests on).
//   - Survivorship: a delisted symbol's final bar (delisted_final=true) is present.
//   - Tie-break stability: equal-ts events emit in ascending source_idx order;
//     deterministic across two independent constructions (identical sequence).
//   - Monotonicity assert: an unsorted source → EXPECT_DEATH on the re-arming step().
//   - Empty feed: step() returns false immediately.
//   - Boundaries: single source, single row, all-same-ts, N interleaved.

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/data/market.hpp"
#include "atx/engine/event/event.hpp"

namespace atxtest_data_handler_test {

using atx::i64;
using atx::u32;
using atx::usize;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::engine::EventBus;
using atx::engine::SimClock;
using atx::engine::data::BarRow;
using atx::engine::data::InMemoryBarFeed;
using atx::engine::data::MarketPayload;
using atx::engine::event::Event;
using atx::engine::event::EventType;
using Timestamp = atx::core::time::Timestamp;

// The feed's ctor is frozen to EventBus<>& — the default-parameter bus type
// (Capacity == 1<<16, ConsumerCount == 1), i.e. the single bus type the backtest
// spine uses. We therefore drive these tests with EventBus<> exclusively. A
// default bus owns a 1<<16 x 128 B ring (~8 MB), which would overflow the default
// 1 MB thread stack, so every bus is HEAP-allocated (matching event_bus_test.cpp).
using Bus = EventBus<>;

// A heap-allocated default bus with consumer 0 already registered (the single
// backtest consumer the recording driver drains).
std::unique_ptr<Bus> make_bus() {
  auto bus = std::make_unique<Bus>();
  (void)bus->add_consumer(0);
  return bus;
}

// Convenience: nanosecond factory so test timestamps read as plain integers.
constexpr Timestamp ts(i64 ns) noexcept { return Timestamp::from_unix_nanos(ns); }

// Build a BarRow whose bar.ts == knowledge_ts (the release-at-close normal case),
// so make_market_bar's bitemporal precondition (knowledge_ts >= bar.ts) holds.
BarRow row(u32 symbol_id, i64 knowledge_ns, bool delisted_final = false) {
  Bar bar{};
  bar.ts = ts(knowledge_ns);
  bar.open = Price::from_int(1);
  bar.high = Price::from_int(2);
  bar.low = Price::from_int(1);
  bar.close = Price::from_int(2);
  bar.volume = Quantity::from_int(100);
  return BarRow{Symbol{symbol_id}, bar, ts(knowledge_ns), delisted_final};
}

// One observed event, captured at drain time by the recording consumer.
struct Seen {
  i64 knowledge_ns{};
  i64 event_ns{};
  u32 symbol_id{};
  bool delisted_final{};
  i64 now_ns_at_drain{}; // clock.now() when this event was delivered
};

// A fixture-free driver: builds the feed over `sources`, then step()-drains until
// EOF, recording every published event AND asserting the no-look-ahead gate at
// drain time. Returns the full recorded stream.
std::vector<Seen> replay(std::span<const std::span<const BarRow>> sources, SimClock &clock,
                         Bus &bus) {
  std::vector<Seen> seen;
  InMemoryBarFeed feed{sources, clock, bus};
  while (feed.step()) {
    bus.drain_in_order([&](usize, const Event &e) {
      ASSERT_EQ(e.type, EventType::Market);
      const auto mp = e.payload_as<MarketPayload>();
      // The no-look-ahead invariant: the clock was advanced to this slice's
      // frontier BEFORE publishing, so every delivered event is already knowable.
      ASSERT_GE(clock.now().unix_nanos(), e.knowledge_ts.unix_nanos())
          << "look-ahead: consumer saw a future event";
      seen.push_back(Seen{e.knowledge_ts.unix_nanos(), e.event_ts.unix_nanos(), mp.symbol.id,
                          mp.delisted_final, clock.now().unix_nanos()});
    });
  }
  return seen;
}

// Helper: a span-of-spans over a vector of row-spans (the caller-owned storage
// pattern the feed documents — the BarRow vectors and the spans outlive `feed`).
std::span<const std::span<const BarRow>>
as_sources(const std::vector<std::span<const BarRow>> &spans) {
  return std::span<const std::span<const BarRow>>{spans};
}

// ============================================================================
//  Empty feed
// ============================================================================
TEST(DataHandler, EmptyFeed_NoSources_StepReturnsFalse) {
  SimClock clock;
  auto bus = make_bus();

  const std::vector<std::span<const BarRow>> spans; // zero sources
  InMemoryBarFeed feed{as_sources(spans), clock, *bus};
  EXPECT_FALSE(feed.step());
}

TEST(DataHandler, EmptyFeed_AllEmptySources_StepReturnsFalse) {
  SimClock clock;
  auto bus = make_bus();

  const std::vector<BarRow> a; // empty source
  const std::vector<BarRow> b; // empty source
  const std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a},
                                                   std::span<const BarRow>{b}};
  InMemoryBarFeed feed{as_sources(spans), clock, *bus};
  EXPECT_FALSE(feed.step());
}

// ============================================================================
//  Boundaries: single source / single row
// ============================================================================
TEST(DataHandler, SingleSource_SingleRow_PublishedOnce) {
  SimClock clock;
  auto bus = make_bus();

  const std::vector<BarRow> a{row(7, 1'000)};
  const std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a}};
  const auto seen = replay(as_sources(spans), clock, *bus);

  ASSERT_EQ(seen.size(), 1U);
  EXPECT_EQ(seen[0].symbol_id, 7U);
  EXPECT_EQ(seen[0].knowledge_ns, 1'000);
  EXPECT_EQ(seen[0].now_ns_at_drain, 1'000); // clock advanced to the frontier
}

TEST(DataHandler, SingleSource_ManyRows_EmittedInOrder) {
  SimClock clock;
  auto bus = make_bus();

  const std::vector<BarRow> a{row(1, 100), row(1, 200), row(1, 300), row(1, 400)};
  const std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a}};
  const auto seen = replay(as_sources(spans), clock, *bus);

  ASSERT_EQ(seen.size(), 4U);
  for (usize i = 0; i < seen.size(); ++i) {
    EXPECT_EQ(seen[i].knowledge_ns, static_cast<i64>((i + 1) * 100));
  }
}

// ============================================================================
//  Chronological emission across N interleaved sources
// ============================================================================
TEST(DataHandler, NSourcesInterleaved_KnowledgeTsNonDecreasing) {
  SimClock clock;
  auto bus = make_bus();

  // Three sources, each sorted, interleaving across the merged timeline.
  const std::vector<BarRow> a{row(1, 100), row(1, 400), row(1, 700)};
  const std::vector<BarRow> b{row(2, 200), row(2, 500), row(2, 800)};
  const std::vector<BarRow> c{row(3, 300), row(3, 600), row(3, 900)};
  const std::vector<std::span<const BarRow>> spans{
      std::span<const BarRow>{a}, std::span<const BarRow>{b}, std::span<const BarRow>{c}};
  const auto seen = replay(as_sources(spans), clock, *bus);

  ASSERT_EQ(seen.size(), 9U);
  for (usize i = 1; i < seen.size(); ++i) {
    EXPECT_LE(seen[i - 1].knowledge_ns, seen[i].knowledge_ns)
        << "knowledge_ts must be non-decreasing across the merged stream";
  }
  // Exact merged order: 100,200,...,900.
  for (usize i = 0; i < seen.size(); ++i) {
    EXPECT_EQ(seen[i].knowledge_ns, static_cast<i64>((i + 1) * 100));
  }
}

// ============================================================================
//  Coalescing: two sources sharing a frontier ts publish together in one step()
// ============================================================================
TEST(DataHandler, EqualFrontier_OneStep_PublishesBothBeforeDrain) {
  SimClock clock;
  auto bus = make_bus();

  // Source 0 and source 1 both have a row at ts=500.
  const std::vector<BarRow> a{row(10, 500)};
  const std::vector<BarRow> b{row(20, 500)};
  const std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a},
                                                   std::span<const BarRow>{b}};

  InMemoryBarFeed feed{as_sources(spans), clock, *bus};

  // First step coalesces the equal frontier: drain sees BOTH events.
  ASSERT_TRUE(feed.step());
  std::vector<u32> ids;
  bus->drain_in_order(
      [&](usize, const Event &e) { ids.push_back(e.payload_as<MarketPayload>().symbol.id); });
  ASSERT_EQ(ids.size(), 2U) << "equal-frontier rows must coalesce into one slice";
  // Tie-break by source_idx: source 0 (symbol 10) before source 1 (symbol 20).
  EXPECT_EQ(ids[0], 10U);
  EXPECT_EQ(ids[1], 20U);

  // Feed is now exhausted.
  EXPECT_FALSE(feed.step());
}

TEST(DataHandler, AllRowsSameTimestamp_OneCoalescedSlice) {
  SimClock clock;
  auto bus = make_bus();

  const std::vector<BarRow> a{row(1, 1'000)};
  const std::vector<BarRow> b{row(2, 1'000)};
  const std::vector<BarRow> c{row(3, 1'000)};
  const std::vector<std::span<const BarRow>> spans{
      std::span<const BarRow>{a}, std::span<const BarRow>{b}, std::span<const BarRow>{c}};

  InMemoryBarFeed feed{as_sources(spans), clock, *bus};

  // A single step() drains the whole coalesced slice (all three sources).
  ASSERT_TRUE(feed.step());
  i64 count = 0;
  bus->drain_in_order([&](usize, const Event &) { ++count; });
  EXPECT_EQ(count, 3) << "all-same-ts must collapse into ONE step's slice";
  EXPECT_FALSE(feed.step());
}

// ============================================================================
//  No look-ahead: explicit assertion that the consumer never sees the future
// ============================================================================
TEST(DataHandler, NoLookAhead_ConsumerNeverSeesFutureKnowledgeTs) {
  SimClock clock;
  auto bus = make_bus();

  const std::vector<BarRow> a{row(1, 100), row(1, 300), row(1, 500)};
  const std::vector<BarRow> b{row(2, 200), row(2, 400), row(2, 600)};
  const std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a},
                                                   std::span<const BarRow>{b}};
  const auto seen = replay(as_sources(spans), clock, *bus);

  ASSERT_EQ(seen.size(), 6U);
  // The replay() helper already ASSERTs now >= knowledge at drain; reinforce the
  // exact invariant here: at delivery the clock equals the slice's frontier, so
  // each delivered knowledge_ts <= now and no future event is ever seen.
  for (const auto &s : seen) {
    EXPECT_LE(s.knowledge_ns, s.now_ns_at_drain) << "look-ahead leaked to consumer";
  }
}

// ============================================================================
//  Survivorship: a delisted symbol's final bar is carried through the stream
// ============================================================================
TEST(DataHandler, Survivorship_DelistedFinalBar_PresentInStream) {
  SimClock clock;
  auto bus = make_bus();

  // Symbol 42 delists: its last row carries delisted_final=true.
  const std::vector<BarRow> alive{row(1, 100), row(1, 200), row(1, 300)};
  const std::vector<BarRow> delisting{row(42, 150), row(42, 250, /*delisted_final=*/true)};
  const std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{alive},
                                                   std::span<const BarRow>{delisting}};
  const auto seen = replay(as_sources(spans), clock, *bus);

  ASSERT_EQ(seen.size(), 5U);
  // Count the delisted-final events and pin them to the expected (symbol, ts).
  // Exactly one event carries the flag, and it is symbol 42's final bar at ts=250.
  i64 final_flags = 0;
  bool found_expected_final = false;
  for (const auto &s : seen) {
    final_flags += s.delisted_final ? 1 : 0;
    found_expected_final =
        found_expected_final || (s.delisted_final && s.symbol_id == 42U && s.knowledge_ns == 250);
  }
  EXPECT_EQ(final_flags, 1) << "exactly the one final bar carries delisted_final";
  EXPECT_TRUE(found_expected_final)
      << "delisted final bar (sym 42 @ 250) must be present, flagged, and not filtered out";
}

// ============================================================================
//  Tie-break stability + determinism across two independent constructions
// ============================================================================
TEST(DataHandler, TieBreak_EqualTs_AscendingSourceIdx_Deterministic) {
  // Four sources, all with a row at the same ts; the emission order at that ts
  // must be ascending source_idx (0,1,2,3), identically across two constructions.
  const std::vector<BarRow> s0{row(100, 500)};
  const std::vector<BarRow> s1{row(101, 500)};
  const std::vector<BarRow> s2{row(102, 500)};
  const std::vector<BarRow> s3{row(103, 500)};
  const std::vector<std::span<const BarRow>> spans{
      std::span<const BarRow>{s0}, std::span<const BarRow>{s1}, std::span<const BarRow>{s2},
      std::span<const BarRow>{s3}};

  SimClock clock_a;
  auto bus_a = make_bus();
  const auto run_a = replay(as_sources(spans), clock_a, *bus_a);

  SimClock clock_b;
  auto bus_b = make_bus();
  const auto run_b = replay(as_sources(spans), clock_b, *bus_b);

  ASSERT_EQ(run_a.size(), 4U);
  ASSERT_EQ(run_b.size(), 4U);
  // Ascending source_idx == ascending symbol id (100,101,102,103) at equal ts.
  for (usize i = 0; i < run_a.size(); ++i) {
    EXPECT_EQ(run_a[i].symbol_id, static_cast<u32>(100 + i)) << "tie-break not source-idx order";
    EXPECT_EQ(run_a[i].symbol_id, run_b[i].symbol_id) << "non-deterministic across constructions";
  }
}

// ============================================================================
//  Monotonicity assert: an unsorted source aborts on the re-arming step()
// ============================================================================
TEST(DataHandler, UnsortedSource_BackwardKnowledgeTs_Aborts) {
  // Source 0's second row goes BACKWARD in knowledge_ts (1'000 -> 500). The merge
  // asserts source monotonicity when it re-arms the heap with the next row.
  SimClock clock;
  auto bus = make_bus();

  const std::vector<BarRow> bad{row(1, 1'000), row(1, 500)}; // NOT sorted
  const std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{bad}};
  InMemoryBarFeed feed{as_sources(spans), clock, *bus};

  // First step() pops the ts=1'000 head and re-arms with the backward ts=500 row,
  // tripping the ATX_ASSERT(next.knowledge_ts >= frontier). EXPECT_DEATH captures
  // the std::abort(); ".*" matches whatever (if anything) reaches stderr.
  EXPECT_DEATH({ (void)feed.step(); }, ".*");
}


}  // namespace atxtest_data_handler_test
