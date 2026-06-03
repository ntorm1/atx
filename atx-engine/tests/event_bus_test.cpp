// event_bus_test.cpp — behavioural tests for atx::engine::EventBus (P1-3).
//
// The bus is a thin engine facade over atx-core's concurrent::Disruptor: one
// producer (DataHandler) claims-fills-publishes Event slots in place; N gated
// consumers each see EVERY event in publication order. The single-threaded
// `drain_in_order` driver is the deterministic backtest spine — these tests pin
// its FIFO, registration-order, batch, idempotence, wrap, boundary, and
// zero-allocation contracts. One optional 2-thread smoke test exercises the
// live-mode wait_for path race-clean (join before assert).
//
// Determinism contract under test: in single-threaded backtest mode
// drain_in_order calls consumers in registration order; each consumer observes
// every published event in publication order; all delivery happens before the
// driver returns (the clock advances).

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/event/event.hpp"

// ---------------------------------------------------------------------------
//  Global heap-allocation counter. Overriding the global operator new/new[] in
//  this translation unit lets the zero-allocation test prove the publish/drain
//  path performs no dynamic allocation: snapshot the counter immediately before
//  and after the measured window and assert it is unchanged. Defined at global
//  scope (replaceable allocation functions must be in the global namespace).
//
//  The override is genuine raw-allocator plumbing (malloc/free, a mutable global
//  counter), so the cppcoreguidelines memory/global-variable checks below are
//  silenced with rationale: this is the ONLY correct way to instrument the global
//  allocator, it is test-only, and it forwards to malloc/free exactly as the
//  default operator new/delete do.
// ---------------------------------------------------------------------------
namespace {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): the global
// allocation counter must be mutable global state to observe the global allocator.
std::atomic<atx::u64> g_alloc_count{0};
} // namespace

// SAFETY: these are the standard replaceable global allocation functions. We
// forward to std::malloc and throw std::bad_alloc on failure exactly as the
// default does; the only added behaviour is the relaxed counter bump (the test
// reads it single-threaded, so relaxed ordering suffices).
// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,clang-analyzer-cplusplus.NewDelete,misc-include-cleaner)
void *operator new(std::size_t n) {
  g_alloc_count.fetch_add(1, std::memory_order_relaxed);
  void *p = std::malloc(n == 0 ? 1 : n);
  if (p == nullptr) {
    throw std::bad_alloc{};
  }
  return p;
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t /*n*/) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t /*n*/) noexcept { std::free(p); }
// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory,clang-analyzer-cplusplus.NewDelete,misc-include-cleaner)

namespace {

using atx::i64;
using atx::u64;
using atx::usize;
using atx::engine::EventBus;
using atx::engine::event::Event;
using atx::engine::event::EventType;
using Timestamp = atx::core::time::Timestamp;

// A trivially-copyable probe payload so we can stamp a distinct tag/checksum per
// event and detect any reorder, drop, or slot overwrite without depending on the
// (not-yet-landed) P1-2 MarketPayload.
struct ProbePayload {
  i64 tag{};      // monotone publication index
  i64 checksum{}; // derived value; must travel with its tag
};
static_assert(std::is_trivially_copyable_v<ProbePayload>);

// Fill slot `e` for publication index `i` with a distinct, verifiable payload.
void stamp(Event &e, i64 i) {
  e.event_ts = Timestamp::from_unix_nanos(1'000 + i);
  e.knowledge_ts = Timestamp::from_unix_nanos(2'000 + i);
  e.type = EventType::Market;
  const ProbePayload p{.tag = i, .checksum = i * 7 + 3};
  e.store_payload(p);
}

// Publish `count` distinct events starting at tag `first` through the
// single-producer claim/publish path.
template <class Bus> void publish_range(Bus &bus, i64 first, i64 count) {
  for (i64 i = 0; i < count; ++i) {
    i64 seq = 0;
    Event &slot = bus.claim_slot(seq);
    stamp(slot, first + i);
    bus.publish(seq);
  }
}

// Publish `n` distinct events tagged 0..n-1.
template <class Bus> void publish_n(Bus &bus, i64 n) { publish_range(bus, 0, n); }

// Assert an event carries the expected tag and a matching checksum (the wrap
// tests use this to prove no slot was overwritten and no payload corrupted).
void expect_event(const Event &e, i64 expected_tag) {
  const auto p = e.payload_as<ProbePayload>();
  EXPECT_EQ(p.tag, expected_tag) << "wrap reorder/overwrite at " << expected_tag;
  EXPECT_EQ(p.checksum, p.tag * 7 + 3) << "wrap corrupted payload";
}

// --------------------------------------------------------------------------
//  SP -> SC FIFO: every event arrives at consumer 0 exactly once, in order.
// --------------------------------------------------------------------------
TEST(EventBus, SingleConsumer_DrainInOrder_DeliversAllInPublicationOrder) {
  EventBus<1U << 12, 1> bus;
  (void)bus.add_consumer(0);

  constexpr i64 kN = 3'000;
  publish_n(bus, kN);

  std::vector<i64> seen;
  seen.reserve(static_cast<usize>(kN));
  bus.drain_in_order([&](usize c, const Event &e) {
    EXPECT_EQ(c, 0U);
    const auto p = e.payload_as<ProbePayload>();
    EXPECT_EQ(p.checksum, p.tag * 7 + 3); // payload integrity
    seen.push_back(p.tag);
  });

  ASSERT_EQ(seen.size(), static_cast<usize>(kN));
  for (i64 i = 0; i < kN; ++i) {
    EXPECT_EQ(seen[static_cast<usize>(i)], i) << "FIFO broken at index " << i;
  }
}

// --------------------------------------------------------------------------
//  SP -> multi-consumer: every consumer sees every event; drain_in_order visits
//  consumers in REGISTRATION order (all of consumer 0's events, then 1, then 2),
//  each in sequence order.
// --------------------------------------------------------------------------
TEST(EventBus, MultiConsumer_DrainInOrder_VisitsConsumersInRegistrationOrder) {
  EventBus<1U << 12, 3> bus;
  (void)bus.add_consumer(0);
  (void)bus.add_consumer(1);
  (void)bus.add_consumer(2);

  constexpr i64 kN = 500;
  publish_n(bus, kN);

  std::vector<std::pair<usize, i64>> calls; // (consumer_index, tag) in call order
  calls.reserve(static_cast<usize>(kN) * 3);
  bus.drain_in_order(
      [&](usize c, const Event &e) { calls.emplace_back(c, e.payload_as<ProbePayload>().tag); });

  ASSERT_EQ(calls.size(), static_cast<usize>(kN) * 3);
  // Expect block [c0: 0..N-1][c1: 0..N-1][c2: 0..N-1].
  usize idx = 0;
  for (usize c = 0; c < 3; ++c) {
    for (i64 i = 0; i < kN; ++i) {
      EXPECT_EQ(calls[idx].first, c) << "consumer order broken at " << idx;
      EXPECT_EQ(calls[idx].second, i) << "per-consumer FIFO broken at " << idx;
      ++idx;
    }
  }
}

// --------------------------------------------------------------------------
//  Batch drain: K events published then a single drain_in_order delivers all K
//  in one pass (one contiguous slice).
// --------------------------------------------------------------------------
TEST(EventBus, BatchDrain_SingleDrainCall_DeliversWholeSlice) {
  EventBus<1U << 8, 1> bus;
  (void)bus.add_consumer(0);

  constexpr i64 kK = 64;
  publish_n(bus, kK);

  i64 count = 0;
  i64 expect = 0;
  bus.drain_in_order([&](usize, const Event &e) {
    EXPECT_EQ(e.payload_as<ProbePayload>().tag, expect);
    ++expect;
    ++count;
  });
  EXPECT_EQ(count, kK);
}

// --------------------------------------------------------------------------
//  Idempotent re-drain: a second drain with nothing newly published delivers
//  nothing (consumer_sequence already at the published cursor).
// --------------------------------------------------------------------------
TEST(EventBus, ReDrain_NoNewPublications_DeliversNothing) {
  EventBus<1U << 8, 2> bus;
  (void)bus.add_consumer(0);
  (void)bus.add_consumer(1);

  publish_n(bus, 10);
  i64 first = 0;
  bus.drain_in_order([&](usize, const Event &) { ++first; });
  EXPECT_EQ(first, 20); // 10 events x 2 consumers

  i64 second = 0;
  bus.drain_in_order([&](usize, const Event &) { ++second; });
  EXPECT_EQ(second, 0) << "re-drain with no delta must be a no-op";

  // A fresh publication after an empty re-drain is picked up correctly.
  publish_range(bus, 10, 5);
  i64 third = 0;
  bus.drain_in_order([&](usize, const Event &) { ++third; });
  EXPECT_EQ(third, 10); // 5 new events x 2 consumers
}

// --------------------------------------------------------------------------
//  Backpressure / wrap: a small ring drained slice-by-slice must never overwrite
//  a live slot, and every event's tag/checksum must survive the wrap. Drive
//  single-threaded: publish slice -> drain -> next slice (the backtest cadence).
// --------------------------------------------------------------------------
TEST(EventBus, SmallRingWrap_SliceBySlice_NoOverwriteValuesIntact) {
  constexpr usize kCap = 4; // tiny ring forces repeated wraps
  EventBus<kCap, 1> bus;
  ASSERT_EQ(bus.capacity(), 4U);
  (void)bus.add_consumer(0);

  constexpr i64 kSlices = 50;
  constexpr i64 kPerSlice = 3; // < capacity so a slice fits unconsumed
  i64 expect = 0;
  for (i64 s = 0; s < kSlices; ++s) {
    // Publish a fresh slice (tags advance globally so wrapped slots get new tags).
    publish_range(bus, s * kPerSlice, kPerSlice);
    bus.drain_in_order([&](usize, const Event &e) { expect_event(e, expect++); });
  }
  EXPECT_EQ(expect, kSlices * kPerSlice);
}

// --------------------------------------------------------------------------
//  Boundaries: capacity rounds up to a power of two; smallest ring works.
// --------------------------------------------------------------------------
TEST(EventBus, Capacity_RoundsUpToPowerOfTwo) {
  const EventBus<5, 1> bus; // 5 -> 8
  EXPECT_EQ(bus.capacity(), 8U);
  EXPECT_EQ(decltype(bus)::capacity(), 8U); // static form too
}

TEST(EventBus, Capacity_One_SmallestRing_Works) {
  EventBus<1, 1> bus;
  EXPECT_EQ(bus.capacity(), 1U);
  (void)bus.add_consumer(0);

  // Capacity-1 ring: must drain each event before the next can be claimed.
  i64 expect = 0;
  for (i64 i = 0; i < 16; ++i) {
    i64 seq = 0;
    Event &slot = bus.claim_slot(seq);
    stamp(slot, i);
    bus.publish(seq);
    bus.drain_in_order([&](usize, const Event &e) {
      EXPECT_EQ(e.payload_as<ProbePayload>().tag, expect);
      ++expect;
    });
  }
  EXPECT_EQ(expect, 16);
}

TEST(EventBus, ConsumerCount_DefaultIsOne) {
  const EventBus<1U << 8> bus; // default ConsumerCount == 1
  EXPECT_EQ(decltype(bus)::consumer_count(), 1U);
}

// --------------------------------------------------------------------------
//  Zero-allocation on the publish/drain path. We override global operator
//  new/new[] in this TU to bump an atomic counter; snapshot it immediately
//  around a publish+drain cycle and assert it is unchanged. (GoogleTest and the
//  std::vector setup allocate outside the measured window.)
// --------------------------------------------------------------------------
TEST(EventBus, PublishDrainCycle_PerformsNoHeapAllocation) {
  EventBus<1U << 12, 2> bus;
  (void)bus.add_consumer(0);
  (void)bus.add_consumer(1);

  // Warm: publish once and drain once so any lazy first-touch is outside window.
  publish_n(bus, 1);
  bus.drain_in_order([](usize, const Event &) {});

  const u64 before = g_alloc_count.load(std::memory_order_relaxed);
  // The measured window: a publish batch + a full drain, no STL growth inside.
  for (i64 i = 1; i < 257; ++i) {
    i64 seq = 0;
    Event &slot = bus.claim_slot(seq);
    stamp(slot, i);
    bus.publish(seq);
  }
  i64 sink = 0;
  bus.drain_in_order([&](usize, const Event &e) { sink += e.payload_as<ProbePayload>().tag; });
  const u64 after = g_alloc_count.load(std::memory_order_relaxed);

  EXPECT_EQ(after, before) << "publish/drain path allocated " << (after - before) << " times";
  EXPECT_GT(sink, 0); // keep the drain from being optimised away
}

// --------------------------------------------------------------------------
//  Optional 2-thread live-mode smoke test. Producer thread publishes; the
//  consumer drains via the bus's blocking wait_for path (live mode). Join the
//  producer BEFORE asserting so the test is race-clean by construction (no TSan
//  on clang-cl/Windows; this is the residual-tracked posture).
// --------------------------------------------------------------------------
TEST(EventBus, TwoThreadSmoke_ProducerConsumer_DeliversAllInOrder) {
  // Heap-allocate the bus: an Event is 128 B, so even a 1<<10 ring is ~128 KB,
  // and a larger ring would overflow the default 1 MB thread stack. N >> capacity
  // exercises the wrap-gating handshake regardless.
  auto bus = std::make_unique<EventBus<1U << 10, 1>>();
  auto consumer = bus->add_consumer(0);

  constexpr i64 kN = 20'000;
  std::thread prod([&] { publish_n(*bus, kN); });

  std::vector<i64> seen;
  seen.reserve(static_cast<usize>(kN));
  i64 next = 0;
  while (next < kN) {
    const i64 hi = consumer.wait_for(next);
    for (i64 s = next; s <= hi; ++s) {
      seen.push_back(bus->at(s).payload_as<ProbePayload>().tag);
    }
    consumer.consumed(hi);
    next = hi + 1;
  }
  prod.join();

  ASSERT_EQ(seen.size(), static_cast<usize>(kN));
  for (i64 i = 0; i < kN; ++i) {
    EXPECT_EQ(seen[static_cast<usize>(i)], i);
  }
}

} // namespace
