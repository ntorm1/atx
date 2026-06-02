#pragma once

// atx::core::concurrent disruptor — LMAX-pattern sequenced ring buffer.
//
// This is the engine event-bus core and a HOT PATH. It is a bounded, lock-free,
// preallocated ring over which one producer (single-producer fast path) or many
// producers (multi-producer CAS path) hand events to one or more consumers, each
// of which sees EVERY event in publication order. The producer is gated by the
// SLOWEST consumer so a live (unconsumed) slot is never overwritten.
//
// ============================================================================
//  Design & the open-source techniques applied (with sources)
// ============================================================================
//  - Single-producer fast path: the claim cursor (`next_value_`) is a
//    PRODUCER-PRIVATE plain integer — no atomic RMW, no CAS, no lock. publish()
//    is a single release-store of the shared cursor. (LMAX SingleProducerSequencer;
//    lmax-exchange.github.io/disruptor/disruptor.html.)
//  - Gating-sequence caching: the producer caches the min consumer sequence
//    (`cached_gate_`) and only re-reads the contended consumer sequences when the
//    cached gate would be overrun. This is the single biggest SP throughput win.
//    (LMAX SingleProducerSequencer.next(); Thompson "mechanical sympathy".)
//  - Multi-producer path: claim does an atomic fetch_add on the cursor to reserve
//    sequences (producers complete out of order), and an AVAILABILITY BUFFER — a
//    per-slot array storing the wrap "round" number, indexed by `seq & mask` —
//    lets a consumer see a slot only once it is TRULY published. The stored value
//    is `seq >> index_shift` (the round), which differs every wrap, so it doubles
//    as ABA protection. (LMAX MultiProducerSequencer setAvailable / isAvailable /
//    getHighestPublishedSequence.)
//  - False-sharing elimination: every independently-written sequence (producer
//    cursor, each consumer sequence, the producer's cached gate) sits alone on a
//    cache line via ATX_CACHE_ALIGNED. (Disruptor padding; Vyukov bounded-queue
//    notes, 1024cores.net.)
//  - Consumer batching: wait_for(seq) returns the HIGHEST available sequence so a
//    consumer drains a whole batch per wait. (LMAX SequenceBarrier.waitFor +
//    BatchEventProcessor.)
//  - Configurable wait strategy: a policy type; default busy-spin with a CPU
//    pause/prefetch hint then yield() after a spin threshold, so a backtest can
//    pick pure busy-spin. (Disruptor BusySpin/Yielding wait strategies;
//    lewissbaker/disruptorplus spin_wait_strategy.)
//  - Power-of-two capacity + mask indexing (`seq & (Capacity-1)`), enforced via
//    atx::core::next_pow2 / is_pow2. (LMAX RingBuffer.)
//
// ============================================================================
//  Concurrency contract — who may call what, concurrently
// ============================================================================
//  Producer side (ProducerKind::Single): claim()/publish()/at(write) MUST be
//    called from exactly ONE thread. claim() reserves the next sequence; the
//    caller writes at(seq); publish(seq) makes it visible.
//  Producer side (ProducerKind::Multi): claim()/publish()/at(write) may be called
//    from MANY threads concurrently. Each claim() returns a distinct sequence to
//    exactly one caller.
//  Consumer side: there are `ConsumerCount` independent consumers, indexed
//    [0, ConsumerCount). Each consumer index MUST be driven by at most one thread.
//    A consumer calls wait_for(seq, c) (blocks until seq is published, returns the
//    highest published sequence >= seq), reads at(s) for s in [seq, highest], then
//    consumed(s, c) to advance its gating sequence. Different consumer indices may
//    run on different threads concurrently; they share the published events.
//
//  The flat API (claim/at/publish/wait_for(seq)/consumed(seq)) targets consumer
//  index 0 and is exactly the Task-21 single-producer/single-consumer contract.
//
// ============================================================================
//  Invariants
// ============================================================================
//  - published-vs-consumed: a slot at index (seq & mask) holds event `seq` and is
//    only reused for `seq + Capacity`. The producer cannot claim `seq + Capacity`
//    until EVERY consumer has called consumed(seq) — this is the wrap-gating
//    guarantee that prevents overwriting a live slot.
//  - sequences are monotonically increasing i64; the initial "nothing yet" value
//    is -1 (so sequence 0 is the first valid event), matching LMAX convention.
//  - no allocation after construction; the ring and all sequences are members.

#include <array>
#include <atomic>
#include <thread>

#include "atx/core/bit.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/platform.hpp"
#include "atx/core/types.hpp"

namespace atx::core::concurrent {

// ===========================================================================
//  Producer mode
// ===========================================================================
enum class ProducerKind : u8 {
    Single, // one producer thread; lock-free, CAS-free claim fast path
    Multi,  // many producer threads; CAS (fetch_add) claim + availability buffer
};

// ===========================================================================
//  Sentinel sequence value: "no sequence has been published / consumed yet".
//  Sequence 0 is the first valid event, so the empty cursor reads -1.
// ===========================================================================
inline constexpr i64 kInitialSequence = -1;

// ===========================================================================
//  Wait strategies (policy types)
//
//  A wait strategy decides how a consumer (or a gated producer) waits for a
//  sequence to advance. Each is a stateless type exposing:
//      static void wait_once(u64& spin) noexcept;
//  called once per failed poll; `spin` is a per-wait counter the strategy may
//  use to escalate from busy-spin to yield. Bounded per call (no internal
//  unbounded loop) so the surrounding poll loop owns the exit condition.
// ===========================================================================

// Busy-spin briefly with a CPU pause hint, then yield to the scheduler after a
// threshold. Good general default: low latency under light contention, but does
// not melt a core if a producer/consumer stalls.
struct SpinYieldWait {
    static constexpr u64 kSpinThreshold = 1024U;

    ATX_FORCE_INLINE static void wait_once(u64& spin) noexcept {
        if (spin < kSpinThreshold) {
            // SAFETY: prefetch() is a pure hint (no memory access, no fault); it
            // serves here purely as a cheap CPU pause / pipeline relax on the
            // spin. It has no effect on program semantics.
            ::atx::core::prefetch(&spin);
            ++spin;
        } else {
            std::this_thread::yield();
        }
    }
};

// Pure busy-spin: lowest latency, burns a core. For a dedicated-core backtest
// runner where the consumer is pinned and always hot.
struct BusySpinWait {
    ATX_FORCE_INLINE static void wait_once(u64& spin) noexcept {
        // SAFETY: pure hint; relaxes the pipeline without changing semantics.
        ::atx::core::prefetch(&spin);
        ++spin; // counter kept live so callers can observe spin pressure if they wish
    }
};

// Yield on every poll: friendliest to other threads, highest latency.
struct YieldWait {
    ATX_FORCE_INLINE static void wait_once(u64& spin) noexcept {
        ++spin;
        std::this_thread::yield();
    }
};

namespace detail {

// A single cache-line-isolated atomic<i64> sequence. Padding eliminates false
// sharing between independently-written sequences (producer cursor, each consumer
// gate, etc.). Initialised to kInitialSequence.
//
// SAFETY: the trailing pad bytes are never read; they exist only to push the
// next object onto a different cache line. alignas + sizeof rounding guarantee
// one Sequence per line.
struct ATX_CACHE_ALIGNED Sequence {
    std::atomic<i64> value{kInitialSequence};

    // Pad out the remainder of the cache line after the atomic.
    // (kCacheLineSize is 64; one i64 atomic is 8 bytes.)
    static constexpr usize kPad =
        ::atx::core::kCacheLineSize - sizeof(std::atomic<i64>);
    [[maybe_unused]] std::array<u8, kPad> pad_{};

    Sequence() noexcept = default;
};
static_assert(sizeof(Sequence) == ::atx::core::kCacheLineSize,
              "Sequence must occupy exactly one cache line");
static_assert(std::atomic<i64>::is_always_lock_free,
              "atomic<i64> must be lock-free for the disruptor to be lock-free");

} // namespace detail

// ===========================================================================
//  Disruptor
//
//  @tparam Event         the event payload type (trivially relocatable assumed).
//  @tparam Capacity      ring capacity; rounded UP to a power of two (>= 1).
//  @tparam Producer      Single (default) or Multi producer mode.
//  @tparam ConsumerCount number of independent consumers (default 1). The
//                        producer gates on the slowest of these.
//  @tparam WaitStrategy  how waiters poll (default SpinYieldWait).
// ===========================================================================
template <class Event,
          usize Capacity,
          ProducerKind Producer      = ProducerKind::Single,
          usize ConsumerCount        = 1,
          class WaitStrategy         = SpinYieldWait>
class Disruptor {
    static_assert(Capacity >= 1, "Disruptor capacity must be at least 1");
    static_assert(ConsumerCount >= 1, "Disruptor needs at least one consumer");
    static_assert(std::is_default_constructible_v<Event>,
                  "Event must be default-constructible to preallocate the ring");

  public:
    // Capacity rounded up to a power of two so `seq & mask` indexes the ring.
    static constexpr usize kCapacity = ::atx::core::next_pow2(Capacity);
    static constexpr i64   kMask     = static_cast<i64>(kCapacity) - 1;

    static_assert(::atx::core::is_pow2(kCapacity), "internal: capacity not pow2");

    // The MP availability buffer stores `seq >> index_shift` (the wrap round).
    // index_shift = log2(kCapacity).
    static constexpr u32 kIndexShift = ::atx::core::ctz(kCapacity);

    Disruptor() noexcept {
        // Seed the MP availability buffer to round -1. A real sequence's round is
        // always >= 0, so no slot reads as "available" until its first publish.
        // (Default-constructed atomic<i32> is 0, which would alias round 0.)
        if constexpr (Producer == ProducerKind::Multi) {
            for (auto& slot : availability_) {
                slot.store(-1, std::memory_order_relaxed);
            }
        }
    }

    // Owns cursors and a ring; copying/moving would alias the live sequences and
    // is meaningless. (agent.md §1 Rule of Five — explicit delete.)
    ATX_DISABLE_COPY_MOVE(Disruptor);

    ~Disruptor() = default;

    /// Usable ring capacity (power of two >= requested Capacity).
    [[nodiscard]] static constexpr usize capacity() noexcept { return kCapacity; }

    /// Number of independent consumers gating this disruptor.
    [[nodiscard]] static constexpr usize consumer_count() noexcept {
        return ConsumerCount;
    }

    // -----------------------------------------------------------------------
    //  Producer side
    // -----------------------------------------------------------------------

    /// Reserve the next sequence. Blocks (per WaitStrategy) only if claiming
    /// would lap the slowest consumer (wrap-gating). Single-producer fast path
    /// is CAS-free; multi-producer uses an atomic fetch_add.
    [[nodiscard]] ATX_FORCE_INLINE i64 claim() noexcept {
        if constexpr (Producer == ProducerKind::Single) {
            return claim_single();
        } else {
            return claim_multi();
        }
    }

    /// Reference to the slot owning `seq`. Valid for the producer between claim()
    /// and publish(), and for a consumer between wait_for() returning >= seq and
    /// consumed(). Mask-indexed; no bounds branch on the hot path.
    [[nodiscard]] ATX_FORCE_INLINE Event& at(i64 seq) noexcept {
        return ring_[static_cast<usize>(seq & kMask)];
    }
    [[nodiscard]] ATX_FORCE_INLINE const Event& at(i64 seq) const noexcept {
        return ring_[static_cast<usize>(seq & kMask)];
    }

    /// Publish `seq`, making the slot visible to consumers.
    ATX_FORCE_INLINE void publish(i64 seq) noexcept {
        if constexpr (Producer == ProducerKind::Single) {
            // SAFETY: release store pairs with the consumer's acquire load of the
            // cursor in wait_for(). Everything the producer wrote to at(seq) before
            // this store happens-before the consumer's read after its acquire. SP
            // sequences are published strictly in order, so the cursor alone is the
            // availability signal — no per-slot buffer needed.
            cursor_.value.store(seq, std::memory_order_release);
        } else {
            // MP: publishers finish out of order, so the cursor (claim high-water)
            // is NOT a safe availability signal. Mark the slot's round in the
            // availability buffer; the consumer scans it to find the contiguous
            // published run.
            set_available(seq);
        }
    }

    /// Non-blocking: highest sequence currently published (kInitialSequence if
    /// none). For SP this is just the cursor; for MP it is the contiguous run.
    [[nodiscard]] i64 published_sequence() const noexcept {
        if constexpr (Producer == ProducerKind::Single) {
            // SAFETY: acquire so a caller that subsequently reads at(seq) sees the
            // producer's writes.
            return cursor_.value.load(std::memory_order_acquire);
        } else {
            // SAFETY: acquire load of the claim high-water; get_highest_published
            // then re-checks each slot's availability flag with acquire, so a
            // half-published tail is excluded.
            const i64 claimed = cursor_.value.load(std::memory_order_acquire);
            return get_highest_published(kInitialSequence + 1, claimed);
        }
    }

    // -----------------------------------------------------------------------
    //  Consumer side — flat API (consumer 0) + indexed API (consumer c)
    // -----------------------------------------------------------------------

    /// Block until `seq` is published; return the HIGHEST published sequence
    /// (>= seq) so the caller can drain a batch. Flat form targets consumer 0.
    ATX_FORCE_INLINE i64 wait_for(i64 seq) noexcept { return wait_for(seq, 0); }

    /// Indexed form: wait on behalf of consumer `c`.
    i64 wait_for(i64 seq, usize c) noexcept {
        ATX_ASSERT(c < ConsumerCount);
        u64 spin = 0;
        for (;;) {
            const i64 available = highest_published_for(seq);
            if (ATX_LIKELY(available >= seq)) {
                return available;
            }
            // Bounded per call; the loop exit is the availability condition above.
            WaitStrategy::wait_once(spin);
        }
    }

    /// Non-blocking variant: returns the highest published sequence if it is
    /// >= seq, else a value < seq (kInitialSequence-relative) meaning "not ready".
    [[nodiscard]] i64 try_wait_for(i64 seq) const noexcept {
        const i64 available = highest_published_for(seq);
        return available;
    }

    /// Advance consumer 0's gating sequence to `seq` (it has consumed [.., seq]).
    ATX_FORCE_INLINE void consumed(i64 seq) noexcept { consumed(seq, 0); }

    /// Indexed form: advance consumer `c`'s gating sequence.
    void consumed(i64 seq, usize c) noexcept {
        ATX_ASSERT(c < ConsumerCount);
        // Consumption must be monotonic: a consumer advances its gate forward only.
        // Moving a gate BACKWARD would falsely tell the producer that an
        // already-reused slot is still live, so this catches caller misuse loudly
        // in debug (agent.md §0). The gate is producer-private to nobody but this
        // consumer index (driven by at most one thread), so a relaxed load of our
        // own prior value is sufficient for the check.
        ATX_ASSERT(seq >= consumer_gates_[c].value.load(std::memory_order_relaxed));
        // SAFETY: release so the producer's acquire load of this gate (in the
        // wrap-gating check) sees that the consumer is done reading at(seq) before
        // the producer may reuse the slot. This store is the wrap-gating handshake.
        consumer_gates_[c].value.store(seq, std::memory_order_release);
    }

    /// Current gating sequence of consumer `c` (highest consumed).
    [[nodiscard]] i64 consumer_sequence(usize c) const noexcept {
        ATX_ASSERT(c < ConsumerCount);
        return consumer_gates_[c].value.load(std::memory_order_acquire);
    }

    // -----------------------------------------------------------------------
    //  Consumer barrier handle — a lightweight, copyable view bound to one
    //  consumer index. Lets the engine pass a consumer's wait/consumed pair
    //  around without exposing the whole disruptor or the raw index everywhere.
    // -----------------------------------------------------------------------
    class ConsumerBarrier {
      public:
        ATX_FORCE_INLINE i64 wait_for(i64 seq) noexcept {
            return owner_->wait_for(seq, index_);
        }
        ATX_FORCE_INLINE void consumed(i64 seq) noexcept {
            owner_->consumed(seq, index_);
        }
        [[nodiscard]] ATX_FORCE_INLINE i64 sequence() const noexcept {
            return owner_->consumer_sequence(index_);
        }
        [[nodiscard]] ATX_FORCE_INLINE usize index() const noexcept {
            return index_;
        }

      private:
        friend class Disruptor;
        ConsumerBarrier(Disruptor* owner, usize index) noexcept
            : owner_{owner}, index_{index} {}

        Disruptor* owner_{nullptr}; // non-owning; outlives the handle by contract
        usize      index_{0};
    };

    /// Obtain the barrier handle for consumer `c`.
    [[nodiscard]] ConsumerBarrier consumer(usize c) noexcept {
        ATX_ASSERT(c < ConsumerCount);
        return ConsumerBarrier{this, c};
    }

  private:
    // -----------------------------------------------------------------------
    //  Single-producer claim with gating-sequence caching.
    // -----------------------------------------------------------------------
    i64 claim_single() noexcept {
        const i64 next = sp_scratch_.next_value + 1; // producer-private; no atomics
        const i64 wrap_point = next - static_cast<i64>(kCapacity);

        // Fast path: the cached gate proves the slot is free; no consumer read.
        // SAFETY: sp_scratch_.cached_gate is producer-private (only this thread
        // touches it), so a plain read is correct.
        if (ATX_UNLIKELY(wrap_point > sp_scratch_.cached_gate)) {
            wait_for_gate(wrap_point);
        }
        sp_scratch_.next_value = next;
        return next;
    }

    // Re-read the (contended) slowest-consumer sequence and spin until the slot
    // at `wrap_point` has been consumed; refresh the cache. Only reached when the
    // cached gate would be overrun — the rare slow path.
    void wait_for_gate(i64 wrap_point) noexcept {
        u64 spin = 0;
        i64 gate = min_consumer_sequence();
        while (wrap_point > gate) {
            WaitStrategy::wait_once(spin);
            gate = min_consumer_sequence();
        }
        sp_scratch_.cached_gate = gate; // cache freshest min for next claims
    }

    // -----------------------------------------------------------------------
    //  Multi-producer claim: atomically reserve a sequence, then wrap-gate.
    // -----------------------------------------------------------------------
    i64 claim_multi() noexcept {
        // SAFETY: acq_rel here is a deliberately CONSERVATIVE choice, NOT a
        // happens-before this code relies on. All inter-thread publication ordering
        // is carried solely by the availability buffer's release/acquire pair
        // (set_available / is_available): a consumer never reads at(seq) on the
        // strength of the cursor alone, only after is_available(seq) succeeds with
        // acquire. The cursor's fetch_add only needs to be atomic to hand each
        // producer a unique `next` — relaxed would suffice for that and for the
        // relaxed gate-cache refresh below (the gate cache is always re-validated
        // against the true consumer sequences, so its ordering is immaterial).
        // We retain acq_rel rather than relaxed because it cannot be wrong, the
        // claim path is not the measured bottleneck, and a future reader extending
        // this code should not have to re-derive the relaxed proof to stay safe.
        // This is an explicit, not-proven-reducible deviation toward stronger
        // ordering (agent.md §5).
        const i64 next       = cursor_.value.fetch_add(1, std::memory_order_acq_rel) + 1;
        const i64 wrap_point = next - static_cast<i64>(kCapacity);

        // MP gate cache is shared, so read it atomically (relaxed: a stale value
        // only costs an extra slow-path check, never correctness).
        const i64 cached = mp_cached_gate_.value.load(std::memory_order_relaxed);
        if (ATX_UNLIKELY(wrap_point > cached)) {
            u64 spin = 0;
            i64 gate = min_consumer_sequence();
            while (wrap_point > gate) {
                WaitStrategy::wait_once(spin);
                gate = min_consumer_sequence();
            }
            // Publish the freshest min; monotone CAS-free store is fine because a
            // smaller stale value only triggers a redundant recheck later.
            // SAFETY: relaxed — the gate value's only use is the wrap check above,
            // which is re-validated against the true consumer sequences; an
            // out-of-date cache is self-correcting and never admits an overwrite.
            mp_cached_gate_.value.store(gate, std::memory_order_relaxed);
        }
        return next;
    }

    // -----------------------------------------------------------------------
    //  Availability buffer (multi-producer publication signalling)
    // -----------------------------------------------------------------------

    // Mark slot for `seq` as published by stamping its wrap round.
    void set_available(i64 seq) noexcept {
        const usize idx  = static_cast<usize>(seq & kMask);
        const i32   flag = availability_flag(seq);
        // SAFETY: release so a consumer's acquire load in is_available() that sees
        // this flag also sees the producer's prior write to at(seq). The flag value
        // (the round number) changes every wrap, so a consumer can never mistake a
        // stale older round for the current one (ABA-safe).
        availability_[idx].store(flag, std::memory_order_release);
    }

    [[nodiscard]] bool is_available(i64 seq) const noexcept {
        const usize idx  = static_cast<usize>(seq & kMask);
        const i32   flag = availability_flag(seq);
        // SAFETY: acquire pairs with set_available()'s release.
        return availability_[idx].load(std::memory_order_acquire) == flag;
    }

    // The round number stored for `seq`: how many times the ring has wrapped.
    [[nodiscard]] static constexpr i32 availability_flag(i64 seq) noexcept {
        // SAFETY: the round (seq >> kIndexShift) is truncated to i32, so two
        // sequences whose rounds differ by a multiple of 2^32 stamp the SAME
        // flag. That can never cause a stale round to alias a live one: at any
        // instant the wrap-gate guarantees at most ONE live round per slot
        // (the producer cannot claim seq + kCapacity until every consumer has
        // consumed seq). For a stale flag to collide with the live one, two
        // sequences mapping to the same slot would have to differ by
        // 2^32 * kCapacity AND both be in flight simultaneously — impossible,
        // because only kCapacity sequences are ever live at once. The wrap value
        // therefore needs only to be unique across consecutive rounds, which a
        // 32-bit window provides with an enormous margin; the truncation is safe.
        return static_cast<i32>(static_cast<u64>(seq) >> kIndexShift);
    }

    // Scan [lower, available] for the highest CONTIGUOUS published sequence.
    // For MP only; SP publishes in order so this is unused on its fast path.
    [[nodiscard]] i64 get_highest_published(i64 lower, i64 available) const noexcept {
        for (i64 seq = lower; seq <= available; ++seq) {
            if (!is_available(seq)) {
                return seq - 1;
            }
        }
        return available;
    }

    // Highest published sequence >= caller's `seq` request, for either mode.
    [[nodiscard]] i64 highest_published_for(i64 seq) const noexcept {
        if constexpr (Producer == ProducerKind::Single) {
            // SAFETY: acquire pairs with publish()'s release store of the cursor.
            return cursor_.value.load(std::memory_order_acquire);
        } else {
            const i64 claimed = cursor_.value.load(std::memory_order_acquire);
            if (claimed < seq) {
                return claimed; // nothing as far as `seq` yet
            }
            return get_highest_published(seq, claimed);
        }
    }

    // -----------------------------------------------------------------------
    //  Slowest-consumer gate
    // -----------------------------------------------------------------------
    [[nodiscard]] i64 min_consumer_sequence() const noexcept {
        i64 mn = consumer_gates_[0].value.load(std::memory_order_acquire);
        // Bounded loop: ConsumerCount is a compile-time constant.
        for (usize c = 1; c < ConsumerCount; ++c) {
            // SAFETY: acquire so the producer's wrap decision sees the consumer's
            // released consumed() store — the consumer is provably done with the
            // slot before we reuse it.
            const i64 s = consumer_gates_[c].value.load(std::memory_order_acquire);
            if (s < mn) {
                mn = s;
            }
        }
        return mn;
    }

    // -----------------------------------------------------------------------
    //  State. Cache-line layout (each padded Sequence owns its own line):
    //    cursor_           — producer claim/publish cursor (shared)
    //    consumer_gates_[] — one gate per consumer (each shared with producer)
    //  Producer-private SP scratch (next_value_/cached_gate_) gets its own line.
    //  The ring and availability buffer are read-mostly bulk arrays.
    // -----------------------------------------------------------------------

    // Producer cursor: SP -> publication high-water; MP -> claim high-water.
    detail::Sequence cursor_{};

    // One gating sequence per consumer (the producer gates on their min).
    std::array<detail::Sequence, ConsumerCount> consumer_gates_{};

    // Single-producer private scratch, isolated on its own cache line so the
    // producer's frequent writes never invalidate a consumer's line. These are
    // touched only by the single producer thread on the SP path — never atomic.
    struct ATX_CACHE_ALIGNED ProducerScratch {
        i64 next_value{kInitialSequence};  // last claimed sequence (private)
        i64 cached_gate{kInitialSequence}; // cached min consumer seq (private)
    };
    ProducerScratch sp_scratch_{};

    // Multi-producer shared gate cache, isolated on its own cache line.
    detail::Sequence mp_cached_gate_{};

    // The ring of events — preallocated, no allocation after construction.
    std::array<Event, kCapacity> ring_{};

    // Multi-producer availability buffer: per-slot published round number.
    // std::atomic<i32> is neither copyable nor movable, so the array is default-
    // constructed in place and seeded to -1 in the constructor body (a round of
    // -1 can never equal a real round, which is >= 0, so nothing reads as
    // available before its first publish). Sized for every mode (cheap; keeps the
    // type uniform). Only read/written on the MP path.
    std::array<std::atomic<i32>, kCapacity> availability_{};
};

} // namespace atx::core::concurrent
