// replay_determinism_test.cpp — the Phase-1 EXIT-CRITERIA EVIDENCE (P1-6).
//
// The proof that the Phase-1 spine replays BYTE-IDENTICALLY. This integration
// test drives the full backtest path — InMemoryBarFeed (N knowledge_ts-sorted
// sources) -> k-way merge + coalescing -> EventBus -> a RecordingConsumer — and
// asserts that two independent replays of identical input fold to an IDENTICAL
// rolling event-hash. It then proves the test is NOT vacuously passing: three
// separate input mutations (reorder a tie, change a bar value, add a late bar)
// each produce a DIFFERENT hash. Determinism without non-vacuity is worthless;
// both halves are the exit-criteria evidence.
//
// Why a SEMANTIC-field fold (not hash_bytes(&e, sizeof e)):
//   Event is a 128 B cache-aligned POD with tail/inter-member padding, and the
//   MarketPayload it carries is a tagged union (the inactive arm's bytes are
//   INDETERMINATE). Hashing the raw struct bytes would fold those indeterminate
//   padding/inactive-union bytes into the digest, which is non-deterministic
//   (UB-adjacent) and could make "identical" runs differ for the wrong reason —
//   or mask a real difference. We instead fold the explicit SEMANTIC identity of
//   each event: its ordinal (running index — makes the fold ORDER-sensitive so a
//   reordered tie changes the hash), kind, both temporal axes, then — for a
//   Market event — the symbol, payload kind, survivorship flag, and the bar's
//   OHLCV+ts via their exact integer representations (Timestamp::unix_nanos and
//   Decimal::raw mantissa). Every folded value is a well-defined integer, so the
//   digest depends only on observable event state, never on padding.
//
// Multi-consumer note: the feed ctor is type-rigid on EventBus<> (the default
// ConsumerCount == 1), so a second gated consumer cannot be registered on the
// SAME bus the feed publishes to. The faithful interpretation of "multi-consumer
// hashes agree on order" is therefore determinism ACROSS consumer instances: two
// independent RecordingConsumers, each fed by its own fresh replay of identical
// input, must agree. That is exactly what MultiConsumer_TwoInstances asserts.

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/hash.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/data/market.hpp"
#include "atx/engine/event/event.hpp"

namespace atxtest_replay_determinism_test {

using atx::i64;
using atx::u32;
using atx::u64;
using atx::u8;
using atx::usize;
using atx::core::hash_combine;
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

// The feed's ctor is frozen to EventBus<>& (Capacity 1<<16, ConsumerCount 1):
// the single bus type the backtest spine uses. A default bus owns a ~8 MB ring
// (1<<16 x 128 B), so it MUST be heap-allocated — a stack instance overflows the
// 1 MB Windows thread stack (the established pattern in event_bus_test.cpp /
// data_handler_test.cpp).
using Bus = EventBus<>;

// Convenience: a nanosecond factory so test timestamps read as plain integers.
constexpr Timestamp ts(i64 ns) noexcept { return Timestamp::from_unix_nanos(ns); }

// ============================================================================
//  RecordingConsumer — test infrastructure (NOT a production API).
//
//  Folds a rolling, ORDER-SENSITIVE hash over the ordered event stream. It is a
//  pure observer: fold() is called once per delivered event, in delivery order.
//  Kept in this test TU because it is the harness's measuring instrument, not an
//  engine type — production code does not depend on it.
// ============================================================================
struct RecordingConsumer {
  u64 digest{}; // running rolling hash; seed 0, folded left-to-right
  u64 count{};  // events folded so far; also the per-event ordinal

  // Fold one event's SEMANTIC identity into the digest. The ordinal (count) is
  // folded FIRST so the hash is position-sensitive: a reordered tie — the same
  // events in a different order — yields a different digest. See the file header
  // for why semantic fields, not raw struct bytes, are hashed.
  void fold(const Event &e) {
    const u64 ordinal = count;
    digest = hash_combine(digest, ordinal, static_cast<u8>(e.kind()), e.knowledge_ts.unix_nanos(),
                          e.event_ts.unix_nanos());

    // Phase 1 only populates Market events; decode and fold the payload identity.
    // (A non-Market event would carry indeterminate payload bytes, so we only
    // touch the union arm named by the tag — exactly the as_bar() precondition.)
    if (e.kind() == EventType::Market) {
      const auto mp = e.payload_as<MarketPayload>();
      digest = hash_combine(digest, mp.symbol.id, static_cast<u8>(mp.kind),
                            static_cast<u8>(mp.delisted_final));
      const Bar &b = mp.as_bar();
      // OHLCV + bar.ts via EXACT integer representations (Decimal mantissa /
      // unix nanos) — no float, no padding, fully reproducible across runs.
      digest = hash_combine(digest, b.ts.unix_nanos(), b.open.to_decimal().raw(),
                            b.high.to_decimal().raw(), b.low.to_decimal().raw(),
                            b.close.to_decimal().raw(), b.volume.to_decimal().raw());
    }
    ++count;
  }
};

// ============================================================================
//  The replay harness — runs the full feed -> bus -> consumer path ONCE.
//
//  Heap-allocates a fresh bus, constructs a fresh clock + feed + consumer,
//  registers ONE consumer, and step()-drains to EOF, folding every delivered
//  event into the digest. Returns the final digest. Each call is fully
//  independent, so two calls on identical `sources` must return equal digests
//  iff the spine is deterministic — which is the property under test.
// ============================================================================
u64 replay(std::span<const std::span<const BarRow>> sources) {
  auto bus = std::make_unique<Bus>();
  (void)bus->add_consumer(0);

  SimClock clock;
  InMemoryBarFeed feed{sources, clock, *bus};

  RecordingConsumer record;
  const auto dispatch = [&record](usize, const Event &e) { record.fold(e); };
  while (feed.step()) {
    bus->drain_in_order(dispatch);
  }
  return record.digest;
}

// ---------------------------------------------------------------------------
//  Fixture builders.
// ---------------------------------------------------------------------------

// Build a BarRow with distinct OHLCV derived from (symbol_id, knowledge_ns) so
// every row has a unique payload fingerprint (a value change is observable).
// bar.ts == knowledge_ts (release-at-close), so make_market_bar's bitemporal
// precondition (knowledge_ts >= bar.ts) holds.
BarRow row(u32 symbol_id, i64 knowledge_ns, i64 close_units = 0, bool delisted_final = false) {
  Bar bar{};
  bar.ts = ts(knowledge_ns);
  bar.open = Price::from_int(static_cast<i64>(symbol_id));
  bar.high = Price::from_int(static_cast<i64>(symbol_id) + 1);
  bar.low = Price::from_int(static_cast<i64>(symbol_id) - 1);
  // Close defaults to a ts-derived value so distinct rows differ; an explicit
  // close_units overrides it (used by the value-mutation test).
  bar.close = Price::from_int(close_units != 0 ? close_units : knowledge_ns / 100);
  bar.volume = Quantity::from_int(100 + static_cast<i64>(symbol_id));
  return BarRow{Symbol{symbol_id}, bar, ts(knowledge_ns), delisted_final};
}

// Span-of-spans over caller-owned row-spans (the storage pattern the feed
// documents: the BarRow vectors and the spans outlive the feed inside replay()).
std::span<const std::span<const BarRow>>
as_sources(const std::vector<std::span<const BarRow>> &spans) {
  return std::span<const std::span<const BarRow>>{spans};
}

// The fixed multi-source fixture: >=2 sources, multiple timestamps, at least one
// COALESCED tie (two sources share knowledge_ts=300) and one delisted-final bar
// (symbol 20's last row), so the digest exercises ordering + coalescing +
// survivorship. The two source vectors are returned by reference-binding into the
// caller's storage; the caller owns lifetime.
struct Fixture {
  std::vector<BarRow> a;
  std::vector<BarRow> b;
  std::vector<std::span<const BarRow>> spans;

  Fixture()
      : a{row(10, 100), row(10, 300), row(10, 500)},
        b{row(20, 200), row(20, 300), row(20, 400, /*close*/ 0, /*delisted_final=*/true)} {
    spans = {std::span<const BarRow>{a}, std::span<const BarRow>{b}};
  }
};

// ============================================================================
//  Identical input -> identical hash (the determinism half).
// ============================================================================
TEST(ReplayDeterminism, IdenticalInput_TwoReplays_IdenticalHash) {
  const Fixture fx;
  const u64 first = replay(as_sources(fx.spans));
  const u64 second = replay(as_sources(fx.spans));

  EXPECT_EQ(first, second) << "the spine must replay byte-identically (P1 exit criterion)";
  // A zero digest would mean nothing was folded — guard against a vacuous pass
  // where the feed emitted no events.
  EXPECT_NE(first, 0U) << "fixture must emit events (else the test is vacuous)";
}

// ============================================================================
//  Mutated input -> different hash (the non-vacuity half). Three mutations,
//  each isolated and each asserting the digest DIFFERS from the baseline.
// ============================================================================

// (a) Reorder a tie. The feed tie-breaks the equal-knowledge_ts (300) slice by
// SOURCE index, so swapping which source carries symbol 10 vs symbol 20 at the
// tie changes the EMISSION ORDER of that coalesced slice — and, because the fold
// is ordinal-sensitive, the digest.
TEST(ReplayDeterminism, MutatedInput_ReorderedTie_DifferentHash) {
  const Fixture base;
  const u64 baseline = replay(as_sources(base.spans));

  // Swap the symbols that sit at the tie ts=300: source A now carries symbol 20's
  // value there and source B carries symbol 10's, flipping the coalesced order.
  std::vector<BarRow> a{row(20, 100), row(20, 300), row(20, 500)};
  std::vector<BarRow> b{row(10, 200), row(10, 300), row(10, 400, 0, true)};
  const std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a},
                                                   std::span<const BarRow>{b}};
  const u64 mutated = replay(as_sources(spans));

  EXPECT_NE(baseline, mutated) << "a reordered tie must change the order-sensitive digest";
}

// (b) Change a bar value. One row's close price differs; everything else is the
// baseline fixture. The payload fold must propagate that into the digest.
TEST(ReplayDeterminism, MutatedInput_ChangedBarValue_DifferentHash) {
  const Fixture base;
  const u64 baseline = replay(as_sources(base.spans));

  std::vector<BarRow> a{row(10, 100), row(10, 300, /*close*/ 999), row(10, 500)};
  std::vector<BarRow> b{row(20, 200), row(20, 300), row(20, 400, 0, true)};
  const std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a},
                                                   std::span<const BarRow>{b}};
  const u64 mutated = replay(as_sources(spans));

  EXPECT_NE(baseline, mutated) << "a changed bar value must change the payload-sensitive digest";
}

// (c) Add a late bar. An extra row at a strictly later knowledge_ts (600) appends
// one more event to the stream, so the digest must differ (more folds + later ts).
TEST(ReplayDeterminism, MutatedInput_AddedLateBar_DifferentHash) {
  const Fixture base;
  const u64 baseline = replay(as_sources(base.spans));

  std::vector<BarRow> a{row(10, 100), row(10, 300), row(10, 500), row(10, 600)};
  std::vector<BarRow> b{row(20, 200), row(20, 300), row(20, 400, 0, true)};
  const std::vector<std::span<const BarRow>> spans{std::span<const BarRow>{a},
                                                   std::span<const BarRow>{b}};
  const u64 mutated = replay(as_sources(spans));

  EXPECT_NE(baseline, mutated) << "an added late bar must change the digest";
}

// ============================================================================
//  Multi-consumer agreement on order. The feed ctor is type-rigid on EventBus<>
//  (ConsumerCount == 1), so two gated consumers cannot share the publishing bus;
//  the faithful test is determinism ACROSS consumer instances — two independent
//  RecordingConsumers, each over its own fresh identical replay, must agree.
// ============================================================================
TEST(ReplayDeterminism, MultiConsumer_TwoInstances_AgreeOnOrder) {
  const Fixture fx;
  const u64 consumer_one = replay(as_sources(fx.spans));
  const u64 consumer_two = replay(as_sources(fx.spans));

  EXPECT_EQ(consumer_one, consumer_two)
      << "two independent consumer instances must agree on the ordered event stream";
}

// ============================================================================
//  Coalesced-slice order stability. The coalesced tie (ts=300) must emit in the
//  SAME (source_idx) order across runs. Covered implicitly by identical-digest,
//  but pinned explicitly here: capture the per-event symbol-id sequence on two
//  independent replays and assert the sequences are element-wise identical.
// ============================================================================
TEST(ReplayDeterminism, CoalescedSlice_SymbolOrder_StableAcrossRuns) {
  const Fixture fx;

  // A local capturing replay that records the ordered symbol-id sequence (the
  // observable order of emission, including the coalesced ts=300 slice).
  const auto symbol_order = [](std::span<const std::span<const BarRow>> sources) {
    auto bus = std::make_unique<Bus>();
    (void)bus->add_consumer(0);
    SimClock clock;
    InMemoryBarFeed feed{sources, clock, *bus};
    std::vector<u32> ids;
    const auto dispatch = [&ids](usize, const Event &e) {
      ids.push_back(e.payload_as<MarketPayload>().symbol.id);
    };
    while (feed.step()) {
      bus->drain_in_order(dispatch);
    }
    return ids;
  };

  const std::vector<u32> run_a = symbol_order(as_sources(fx.spans));
  const std::vector<u32> run_b = symbol_order(as_sources(fx.spans));

  ASSERT_EQ(run_a.size(), run_b.size());
  EXPECT_EQ(run_a, run_b) << "coalesced emission order must be stable across runs";

  // Pin the coalesced tie explicitly: at ts=300 source 0 (symbol 10) precedes
  // source 1 (symbol 20) — the (knowledge_ts, source_idx) tie-break. The fixture
  // emits 10@100, 20@200, then the tie 10@300, 20@300, ...
  ASSERT_GE(run_a.size(), 4U);
  EXPECT_EQ(run_a[2], 10U) << "tie slice: lower source_idx (symbol 10) first";
  EXPECT_EQ(run_a[3], 20U) << "tie slice: higher source_idx (symbol 20) second";
}


}  // namespace atxtest_replay_determinism_test
