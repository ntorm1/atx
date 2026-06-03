#pragma once

// atx::engine::data — the no-look-ahead k-way merge feed (P1-5).
//
// Defines:
//   IDataHandler    — the abstract feed: step() advances exactly one coalesced
//                     chronological timestamp slice and returns false at EOF.
//   BarRow          — one source record: a Symbol, its Bar, the knowledge_ts the
//                     bar becomes admissible at, and the survivorship
//                     `delisted_final` flag (a delisted symbol's last bar).
//   InMemoryBarFeed — the in-memory implementation. It k-way-merges N
//                     knowledge_ts-SORTED BarRow sources into one chronological,
//                     coalesced stream, advances the SimClock to each frontier,
//                     and publishes the slice's MarketEvents onto the EventBus.
//
// ===========================================================================
//  The merge contract (plan P1-5 §5)
// ===========================================================================
//  Each source is a cursor over a knowledge_ts-sorted span of BarRow. A min-heap
//  of (knowledge_ts, source_idx) holds one entry per non-empty source (the head
//  of each cursor), ordered ascending by knowledge_ts then source_idx.
//
//  step():
//    * EOF: heap empty → return false.
//    * frontier := heap.top().knowledge_ts.
//    * Advance the clock to the frontier BEFORE publishing the slice, so a
//      consumer draining afterward sees clock.now() == frontier and every event
//      it receives has knowledge_ts == frontier <= now(). This is the
//      no-look-ahead guarantee — the whole reason the clock advances FIRST.
//    * Drain every head whose knowledge_ts == frontier (coalescing): pop it,
//      publish its row as a Market Event via make_market_bar (survivorship flag
//      passed through unchanged), advance that source's cursor, and — if the
//      source has a next row — re-arm the heap with it, asserting source
//      monotonicity (next.knowledge_ts >= frontier; a backward step is a feed bug
//      and fails closed).
//    * Return true.
//
//  Emission order is stable (knowledge_ts, source_idx) — the source_idx tie-break
//  makes equal-timestamp slices deterministic across runs/constructions.
//
//  Survivorship defence: rows are NEVER filtered by present-day existence. A
//  delisted symbol is carried with its final bar (delisted_final=true) so a
//  backtest sees the same universe the strategy saw live (no survivorship bias).
//
// ===========================================================================
//  The caller-drains contract (who delivers the slice)
// ===========================================================================
//  step() PUBLISHES the slice and advances the clock; it does NOT drain. In
//  single-threaded backtest mode the CALLER (the P1-6 replay harness / a backtest
//  driver) calls bus.drain_in_order(dispatch) after each step() to deliver the
//  slice to consumers. Draining BETWEEN steps keeps the ring from wrapping mid-
//  slice and guarantees consumers observe knowledge_ts <= now() (the clock was
//  already advanced to this slice's frontier). The abstract IDataHandler::step()
//  therefore keeps a plain `bool step()` signature (no dispatch argument): the
//  feed owns publication+clock; the driver owns delivery.
//
// ===========================================================================
//  Ownership / lifetime / threading
// ===========================================================================
//  NON-OWNING throughout. The CALLER owns the BarRow storage: `sources` is a
//  span-of-spans into caller memory that MUST outlive the feed (the feed never
//  copies a row — it indexes into the spans on every step). `clock` and `bus` are
//  held by reference and must likewise outlive the feed. Single-threaded backtest
//  use only (the bus is single-producer; the feed is the sole producer + the sole
//  clock advancer).
//
//  Allocation: ONE allocation at construction (the per-source cursor vector and
//  the heap's backing vector, both sized to N = source count). The heap holds at
//  most N entries and never grows past N after construction (each pop is matched
//  by at most one re-arm), so step() performs ZERO heap allocation on the steady-
//  state hot path. A fixed-capacity heap over atx-core's FixedVector is the
//  deferred optimisation residual (plan §5) — priority_queue is the sanctioned
//  baseline here.

#include <queue>  // std::priority_queue — the sanctioned baseline min-heap
#include <span>   // std::span — non-owning source views
#include <vector> // std::vector — cursors + the heap's backing store

#include "atx/core/datetime.hpp"      // atx::core::time::Timestamp
#include "atx/core/domain/domain.hpp" // atx::core::domain::Bar
#include "atx/core/domain/symbol.hpp" // atx::core::domain::Symbol
#include "atx/core/macro.hpp"         // ATX_ASSERT (fail-closed monotonicity guard)
#include "atx/core/types.hpp"         // atx::i64, atx::u32, atx::usize

#include "atx/engine/bus/event_bus.hpp"   // atx::engine::EventBus
#include "atx/engine/clock/sim_clock.hpp" // atx::engine::SimClock
#include "atx/engine/data/market.hpp"     // make_market_bar (typed event maker)
#include "atx/engine/event/event.hpp"     // atx::engine::event::Event

namespace atx::engine::data {

// ===========================================================================
//  IDataHandler — the abstract chronological feed.
// ===========================================================================
class IDataHandler {
public:
  IDataHandler() = default;
  virtual ~IDataHandler() = default;

  // Polymorphic base: suppress slicing copies/moves (agent §1, Rule of Five).
  IDataHandler(const IDataHandler &) = delete;
  IDataHandler &operator=(const IDataHandler &) = delete;
  IDataHandler(IDataHandler &&) = delete;
  IDataHandler &operator=(IDataHandler &&) = delete;

  /// Advance one coalesced chronological timestamp slice: advance the clock to
  /// the next frontier, publish every event at that frontier onto the bus, and
  /// return true. Returns false at EOF (no more rows). The CALLER drains the bus
  /// after each true return (see the caller-drains contract above).
  [[nodiscard]] virtual bool step() = 0;
};

// ===========================================================================
//  BarRow — one knowledge_ts-stamped source record.
//
//  `knowledge_ts` is the merge/visibility key (when the bar becomes admissible);
//  `bar.ts` is the true-at instant (bar close). `delisted_final` marks a delisted
//  symbol's last bar (survivorship). PRECONDITION (per make_market_bar): the
//  bitemporal invariant knowledge_ts >= bar.ts must hold for every row.
// ===========================================================================
struct BarRow {
  atx::core::domain::Symbol symbol{};
  atx::core::domain::Bar bar{};
  // No `{}`: Timestamp's defaulted ctor already value-inits to epoch (a `{}` here
  // is flagged redundant; the aggregate members above DO need it).
  atx::core::time::Timestamp knowledge_ts;
  bool delisted_final{false};
};

// ===========================================================================
//  InMemoryBarFeed — the in-memory k-way merge.
// ===========================================================================
class InMemoryBarFeed final : public IDataHandler {
public:
  /// Build a feed over `sources` (each a knowledge_ts-sorted span of BarRow),
  /// publishing onto `bus` and advancing `clock`. Seeds the heap with the head of
  /// every non-empty source. NON-OWNING: `sources` (and the row storage it views),
  /// `clock`, and `bus` must outlive this feed (see the header lifetime note).
  ///
  // bugprone-exception-escape suppressed: the ctor is frozen `noexcept` by the
  // P1-5 plan. Its only throwing operations are the two construction-time
  // allocations (cursors_.assign / heap_.push, each bounded by N). A std::bad_alloc
  // here is unrecoverable feed-setup failure, so terminating (the noexcept effect)
  // is the intended fail-closed posture — there is no half-built feed to leak.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  InMemoryBarFeed(std::span<const std::span<const BarRow>> sources, SimClock &clock,
                  EventBus<> &bus) noexcept
      : sources_{sources}, clock_{&clock}, bus_{&bus} {
    // One allocation each, sized to N: the cursors and the heap's backing store.
    cursors_.assign(sources_.size(), 0);
    for (atx::usize s = 0; s < sources_.size(); ++s) {
      if (!sources_[s].empty()) {
        heap_.push(
            HeapEntry{sources_[s].front().knowledge_ts.unix_nanos(), static_cast<atx::u32>(s)});
      }
    }
  }

  // step() is NOT noexcept: std::priority_queue::push may throw std::bad_alloc.
  // In steady state the heap never grows past its N-sized backing store (every
  // pop is matched by at most one re-arm), so no allocation — and no throw —
  // occurs on the hot path; the signature simply matches the worst-case reality.
  [[nodiscard]] bool step() override {
    if (heap_.empty()) {
      return false; // EOF: every source's cursor is past its last row.
    }
    const atx::i64 frontier = heap_.top().knowledge_ts;

    // Advance the clock BEFORE publishing this slice, so a consumer draining
    // afterward sees now() == frontier and every event's knowledge_ts <= now().
    clock_->advance_to(atx::core::time::Timestamp::from_unix_nanos(frontier));

    drain_frontier(frontier);
    return true;
  }

private:
  // A min-heap entry: the head knowledge_ts of a source and its index. Ordered
  // ascending by (knowledge_ts, source_idx) so equal timestamps tie-break by
  // source for deterministic emission. A small trivially-copyable POD.
  struct HeapEntry {
    atx::i64 knowledge_ts{};
    atx::u32 source_idx{};
  };

  // Comparator that turns std::priority_queue (a MAX-heap by default) into a
  // MIN-heap on (knowledge_ts, source_idx): `top()` is the smallest entry.
  struct Greater {
    [[nodiscard]] bool operator()(const HeapEntry &a, const HeapEntry &b) const noexcept {
      if (a.knowledge_ts != b.knowledge_ts) {
        return a.knowledge_ts > b.knowledge_ts;
      }
      return a.source_idx > b.source_idx; // tie-break: lower source_idx first
    }
  };

  // Drain every head whose knowledge_ts == frontier into the bus (coalescing the
  // slice), advancing each drained source's cursor and re-arming the heap with its
  // next row. Extracted from step() to keep both functions small.
  void drain_frontier(atx::i64 frontier) {
    while (!heap_.empty() && heap_.top().knowledge_ts == frontier) {
      const atx::u32 idx = heap_.top().source_idx;
      heap_.pop();

      const BarRow &r = sources_[idx][cursors_[idx]];
      atx::i64 seq = 0;
      event::Event &slot = bus_->claim_slot(seq);
      slot = make_market_bar(r.symbol, r.bar, r.knowledge_ts, r.delisted_final);
      bus_->publish(seq);

      cursors_[idx] += 1;
      if (cursors_[idx] < sources_[idx].size()) {
        const BarRow &next = sources_[idx][cursors_[idx]];
        // Fail-closed source monotonicity: a backward knowledge_ts is a feed bug
        // (the merge's correctness rests on each source being sorted). Aborts in
        // debug; the heap re-arm below would otherwise silently reorder the stream.
        ATX_ASSERT(next.knowledge_ts.unix_nanos() >= frontier);
        heap_.push(HeapEntry{next.knowledge_ts.unix_nanos(), idx});
      }
    }
  }

  std::span<const std::span<const BarRow>> sources_; // non-owning; caller-owned rows
  SimClock *clock_;                                  // non-owning; outlives the feed
  EventBus<> *bus_;                                  // non-owning; outlives the feed

  // Per-source cursor (row index). Sized N at construction; one entry per source.
  std::vector<atx::usize> cursors_;

  // Min-heap of source heads. Bounded by N (one entry per source); never grows
  // past its construction-time backing store, so step() does not allocate.
  std::priority_queue<HeapEntry, std::vector<HeapEntry>, Greater> heap_;
};

} // namespace atx::engine::data
