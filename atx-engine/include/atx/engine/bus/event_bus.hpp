#pragma once

// atx::engine::EventBus — a thin engine facade over the atx-core Disruptor (P1-3).
//
// The bus is the engine's event spine: a single producer (the DataHandler)
// claims a ring slot, fills the Event in place, and publishes it; N gated
// consumers each observe EVERY event in publication order. It owns NO ring logic
// of its own — every ordering, wrap-gating, and visibility guarantee is the
// atx-core concurrent::Disruptor's (§3.2). This facade adds exactly three things:
//
//   1. Zero-copy claim-fill-publish ergonomics: claim_slot() returns a reference
//      to the in-ring Event AND the claimed sequence, so the producer mutates the
//      slot in place and never copies an Event (the LMAX pattern).
//   2. A registration table of consumer indices (fixed-capacity, zero-alloc) so
//      the backtest driver knows which consumers to drive and in what order.
//   3. drain_in_order(): the deterministic SINGLE-THREADED backtest driver. It
//      reads the published cursor once and, for each registered consumer in
//      registration order, dispatches every newly-published event to the
//      callback, then advances that consumer's gate. It NEVER blocks (it only
//      drains up to published_sequence()).
//
// ===========================================================================
//  Ownership / lifetime
// ===========================================================================
//  The bus OWNS the ring by value (the Disruptor member). Rule of Zero: the ring
//  member handles construction/destruction; the bus defines no special members.
//  A Consumer handle is a NON-OWNING view (a bare index + a pointer to this bus)
//  and must not outlive the bus.
//
// ===========================================================================
//  Threading contract (inherited from the Disruptor, §3.2)
// ===========================================================================
//  Producer side: with ProducerKind::Single (the default), claim_slot()/publish()
//    MUST be called from exactly ONE thread. With ProducerKind::Multi they may be
//    called concurrently from many threads.
//  Consumer side: each consumer INDEX must be driven by AT MOST ONE thread. The
//    single-threaded backtest path (drain_in_order) drives every registered
//    consumer from the caller's one thread, so it is trivially race-free.
//  Live mode (Phase 2): a caller may instead drive each Consumer on its own
//    thread via Consumer::wait_for()/consumed() (the blocking path). The
//    cross-consumer gating DAG is reserved for live mode and is NOT built here.
//
// ===========================================================================
//  Determinism contract (backtest mode)
// ===========================================================================
//  In single-threaded backtest mode, drain_in_order() calls consumers in
//  REGISTRATION order; each registered consumer sees every event in publication
//  order; all delivery completes before drain_in_order() returns (i.e. before the
//  clock advances). The chosen WaitStrategy is irrelevant on this path because
//  drain_in_order never waits — it drains only what is already published.

#include <type_traits> // std::is_invocable_v — drain_in_order dispatch contract

#include "atx/core/concurrent/disruptor.hpp"
#include "atx/core/container/fixed_vector.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/event/event.hpp"

namespace atx::engine {

// ===========================================================================
//  EventBus
//
//  @tparam Capacity      ring capacity; rounded UP to a power of two by the ring.
//  @tparam ConsumerCount number of independent gated consumers (default 1).
//  @tparam Producer      Single (default) or Multi producer mode.
//
//  The WaitStrategy is fixed to SpinYieldWait (the bus's only consumer-blocking
//  path is live mode; the backtest path never waits).
// ===========================================================================
template <atx::usize Capacity = 1U << 16, atx::usize ConsumerCount = 1,
          atx::core::concurrent::ProducerKind Producer =
              atx::core::concurrent::ProducerKind::Single>
class EventBus {
  using Ring = atx::core::concurrent::Disruptor<event::Event, Capacity, Producer, ConsumerCount,
                                                atx::core::concurrent::SpinYieldWait>;

public:
  // -------------------------------------------------------------------------
  //  Consumer — a small, trivially-copyable, NON-OWNING handle bound to one
  //  consumer index. It carries the index (so a live-mode caller can drive it on
  //  a dedicated thread) plus a pointer to the bus's ring for the blocking
  //  wait_for()/consumed() pair. By the Disruptor contract a given consumer index
  //  must be driven by at most one thread; the handle does not enforce that — the
  //  caller owns that discipline.
  // -------------------------------------------------------------------------
  class Consumer {
  public:
    /// The consumer index this handle drives ([0, ConsumerCount)).
    [[nodiscard]] ATX_FORCE_INLINE atx::usize index() const noexcept { return index_; }

    /// Live-mode blocking drain: wait until `seq` is published and return the
    /// HIGHEST published sequence (>= seq) so the caller can drain a batch.
    /// Not used by the single-threaded backtest path (which never blocks).
    [[nodiscard]] ATX_FORCE_INLINE atx::i64 wait_for(atx::i64 seq) noexcept {
      return ring_->wait_for(seq, index_);
    }

    /// Advance this consumer's gating sequence to `seq` (consumed [.., seq]).
    ATX_FORCE_INLINE void consumed(atx::i64 seq) noexcept { ring_->consumed(seq, index_); }

    /// Highest sequence this consumer has consumed (kInitialSequence if none).
    [[nodiscard]] ATX_FORCE_INLINE atx::i64 sequence() const noexcept {
      return ring_->consumer_sequence(index_);
    }

  private:
    friend class EventBus;
    Consumer(Ring *ring, atx::usize index) noexcept : ring_{ring}, index_{index} {}

    Ring *ring_{nullptr}; // non-owning; the bus outlives the handle by contract
    atx::usize index_{0};
  };

  // -------------------------------------------------------------------------
  //  Compile-time shape.
  // -------------------------------------------------------------------------

  /// Usable ring capacity (power of two >= requested Capacity).
  [[nodiscard]] static constexpr atx::usize capacity() noexcept { return Ring::capacity(); }

  /// Number of independent consumers the ring gates on.
  [[nodiscard]] static constexpr atx::usize consumer_count() noexcept {
    return Ring::consumer_count();
  }

  // -------------------------------------------------------------------------
  //  Producer side — zero-copy claim-fill-publish.
  // -------------------------------------------------------------------------

  /// Reserve the next ring slot and return a mutable reference to its Event,
  /// writing the claimed sequence to `out_seq`. The caller fills the slot IN
  /// PLACE (no Event copy) and then calls publish(out_seq). Blocks per the wait
  /// strategy only if claiming would lap the slowest consumer (wrap-gating).
  [[nodiscard]] ATX_FORCE_INLINE event::Event &claim_slot(atx::i64 &out_seq) noexcept {
    out_seq = ring_.claim();
    return ring_.at(out_seq);
  }

  /// Publish a previously claimed sequence, making the fully-written slot visible
  /// to consumers. Write-then-publish discipline: fill the slot completely before
  /// calling this.
  ATX_FORCE_INLINE void publish(atx::i64 seq) noexcept { ring_.publish(seq); }

  // -------------------------------------------------------------------------
  //  Consumer registration + the single-threaded backtest driver.
  // -------------------------------------------------------------------------

  /// Register consumer `index` (`index < ConsumerCount`) and return its handle.
  /// Registration order is the drain_in_order() dispatch order. Each index should
  /// be registered at most once; double-registration is a caller error (asserted
  /// in debug). Zero-allocation: the registry is a fixed-capacity vector.
  [[nodiscard]] Consumer add_consumer(atx::usize index) noexcept {
    ATX_ASSERT(index < ConsumerCount);
    ATX_ASSERT(registered_.size() < ConsumerCount);
    registered_.push_back(index);
    return Consumer{&ring_, index};
  }

  /// Single-threaded backtest driver. Reads the published cursor ONCE, then for
  /// each registered consumer (in registration order) dispatches every event in
  /// (consumer_sequence, published] to `dispatch(consumer_index, event)` and
  /// advances that consumer's gate. Never blocks (drains only what is already
  /// published). Zero allocation. Deterministic: see the determinism contract.
  ///
  /// @param dispatch  callable of signature void(atx::usize, const event::Event&).
  ///                  Taken by const-ref (NOT a forwarding reference): it is
  ///                  invoked once per event, so it must remain a stable lvalue
  ///                  across the batch — forwarding it would move-from on the
  ///                  first call. noexcept is the determinism/fail-loud contract:
  ///                  the dispatch callback MUST NOT throw (a throwing dispatch is
  ///                  a precondition violation that std::terminates, by design —
  ///                  it must not silently corrupt the in-order delivery state).
  // bugprone-exception-escape suppressed below: noexcept is the intentional
  // fail-loud contract — a throwing dispatch callback is a caller precondition
  // violation; terminating beats half-draining and corrupting consumer gates.
  template <class Fn>
  // NOLINTNEXTLINE(bugprone-exception-escape)
  void drain_in_order(const Fn &dispatch) noexcept {
    static_assert(std::is_invocable_v<const Fn &, atx::usize, const event::Event &>,
                  "drain_in_order dispatch must be callable as "
                  "void(atx::usize, const event::Event&)");
    // Snapshot the cursor once so every consumer drains the SAME publication
    // window this call — the determinism guarantee (all consumers see the same
    // events before the clock advances).
    const atx::i64 published = ring_.published_sequence();
    // Bounded loop: registered_.size() <= ConsumerCount (compile-time constant).
    for (const atx::usize c : registered_) {
      const atx::i64 from = ring_.consumer_sequence(c) + 1;
      if (published < from) {
        continue; // nothing new for this consumer (idempotent empty re-drain)
      }
      // Batch drain: one pass over the contiguous published slice.
      for (atx::i64 s = from; s <= published; ++s) {
        dispatch(c, ring_.at(s));
      }
      ring_.consumed(published, c); // advance the gate exactly once per drain
    }
  }

  /// Read-only access to the Event at sequence `seq`. For the live-mode consumer
  /// loop (Consumer::wait_for returns a sequence range; the caller reads slots
  /// via this accessor). Valid for a consumer between wait_for() returning >= seq
  /// and its consumed() call.
  [[nodiscard]] ATX_FORCE_INLINE const event::Event &at(atx::i64 seq) const noexcept {
    return ring_.at(seq);
  }

private:
  Ring ring_{}; // owns the ring + all sequences; Rule of Zero handles lifetime

  // Registered consumer indices in registration order — drives drain_in_order.
  // Fixed-capacity (sized ConsumerCount), so registration is zero-allocation.
  atx::core::container::FixedVector<atx::usize, ConsumerCount> registered_{};
};

} // namespace atx::engine
