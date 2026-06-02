#pragma once

// atx::core::concurrent::SeqLock<T> — single-writer / many-reader lock-free
// snapshot of a small trivially-copyable value (e.g. a top-of-book Quote).
//
// A reader never blocks a writer and a writer never blocks a reader: readers
// optimistically copy the payload and validate it against an even/odd sequence
// counter, retrying if a write overlapped. Ideal for a hot value that is
// updated frequently and read on the critical path (market data snapshots,
// configuration, clocks) where readers must never stall a producer.
//
// ============================================================================
//  Protocol (even/odd sequence counter)
// ============================================================================
//  seq_ is even when the payload is stable and odd while a write is in flight.
//
//  Writer (store):  s = seq;  seq = s+1 (odd);  write payload;  seq = s+2 (even).
//  Reader (load):   spin: s0 = seq; copy payload; s1 = seq;
//                   accept iff s0 == s1 AND s0 is even; else retry.
//
//  If a writer overlapped the reader's copy, either s0 was odd, or seq advanced
//  between s0 and s1 — both rejected, so a torn payload is never returned.
//
// ============================================================================
//  Memory-ordering rationale (agent.md §5 — justify every ordering)
// ============================================================================
//  store():
//    seq=s+1 is relaxed: it only needs to become odd *before* the payload
//    writes become visible, which the following release fence guarantees.
//    The release fence orders the payload writes after the odd publish and
//    before the final seq=s+2 release-store, so any reader that observes the
//    even value (acquire) also observes the complete payload.
//  load():
//    s0 is an acquire load — it pairs with the writer's release-store of the
//    even sequence so the payload writes that happened-before that store are
//    visible to this reader. The acquire fence after copying the payload
//    prevents the s1 re-read from being hoisted above the copy, so s1 truly
//    reflects "did a write touch seq while I was copying".
//
// ============================================================================
//  SAFETY — the seqlock payload race (documented deviation, agent.md §0)
// ============================================================================
//  A reader may memcpy `value_` while a writer is mid-write. In the strict C++
//  abstract machine that overlap is a data race on a non-atomic object. This is
//  the defining, deliberate property of a seqlock (same as the Linux kernel and
//  Rigtorp's reference implementation): the value produced by a torn read is
//  *discarded* by the sequence check and never observed by the caller, and the
//  acquire/release fences give the accepted (untorn) read correct visibility.
//  We confine the deviation by (a) requiring T to be trivially copyable so the
//  copy is a bytewise memcpy with no user code on the racing path, and (b)
//  validating every read before returning it. No std::atomic<T> is used because
//  for T larger than the platform's lock-free width it would silently fall back
//  to a mutex, defeating the wait-free reader guarantee.
//
// ============================================================================
//  Contract
// ============================================================================
//  - Exactly ONE writer thread may call store() at a time. Concurrent writers
//    are unsupported (use a MpmcQueue / external lock to serialise producers).
//  - Any number of reader threads may call load() concurrently with the writer.
//  - T must be trivially copyable and default constructible.

#include <atomic>
#include <cstring> // std::memcpy
#include <type_traits>

#include "atx/core/macro.hpp"
#include "atx/core/platform.hpp"
#include "atx/core/types.hpp"

namespace atx::core::concurrent {

template <class T>
class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SeqLock<T>: T must be trivially copyable (payload is memcpy'd)");
    static_assert(std::is_default_constructible_v<T>,
                  "SeqLock<T>: T must be default constructible");

  public:
    SeqLock() noexcept = default;
    ~SeqLock()         = default;

    // The lock embeds mutable shared state with an identity (the sequence
    // counter); copying or moving it would alias that state.
    ATX_DISABLE_COPY_MOVE(SeqLock);

    /// Seed the initial value before any reader/writer races begin. Not safe to
    /// call concurrently with load()/store(); intended for single-threaded init.
    explicit SeqLock(const T& initial) noexcept : value_{initial} {}

    // -------------------------------------------------------------------------
    //  store() — publish a new value. Single-writer only.
    // -------------------------------------------------------------------------
    void store(const T& desired) noexcept {
        // SAFETY: relaxed — single writer, so this load observes our own last
        // even value with no contention; ordering is supplied by the fence and
        // the release-store below, not by this load.
        const u32 s = seq_.load(std::memory_order_relaxed);

        // SAFETY: relaxed — make the counter odd ("write in progress"). The
        // release fence that follows guarantees this odd value is ordered
        // before the payload writes from any acquiring reader's viewpoint.
        seq_.store(s + 1U, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);

        // Payload write. Trivially-copyable T → bytewise copy. A concurrent
        // reader may observe a torn intermediate; it is rejected by the seq
        // check (see file-header SAFETY note).
        value_ = desired;

        // SAFETY: release — publishes the completed payload. A reader that
        // acquire-loads this even value is guaranteed to see all payload bytes.
        seq_.store(s + 2U, std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    //  load() — wait-free-ish optimistic read with validation.
    //
    //  Bounded in practice by writer frequency; under a continuously-writing
    //  producer it may retry but cannot livelock for an unbounded time on real
    //  hardware (each retry costs one writer interval).
    // -------------------------------------------------------------------------
    [[nodiscard]] T load() const noexcept {
        T out;
        for (;;) {
            // SAFETY: acquire — pairs with the writer's release-store of the
            // even sequence so the payload bytes published before it are
            // visible to this thread before we copy them.
            const u32 s0 = seq_.load(std::memory_order_acquire);

            // Bytewise copy of the payload (see file-header SAFETY note on the
            // deliberate benign race). memcpy avoids assuming T has a usable
            // copy assignment on the racing path.
            std::memcpy(&out, &value_, sizeof(T));

            // SAFETY: acquire fence — stops the s1 re-read from being reordered
            // before the copy above, so s1 genuinely brackets the copy window.
            std::atomic_thread_fence(std::memory_order_acquire);
            const u32 s1 = seq_.load(std::memory_order_relaxed);

            // Accept only a stable, non-overlapped read: counter unchanged and
            // even (no write was in progress at either sample).
            if (ATX_LIKELY(s0 == s1 && (s0 & 1U) == 0U)) {
                return out;
            }
            // Otherwise a write overlapped — discard `out` and retry.
        }
    }

  private:
    // Sequence counter on its own cache line: it is the contended atomic and
    // must not false-share with the payload (which the writer also touches, but
    // separating them keeps a reader's seq polling off the payload line).
    ATX_CACHE_ALIGNED std::atomic<u32> seq_{0};

    // Payload, default-initialised so an early reader (before the first store)
    // sees a valid value rather than indeterminate bytes.
    T value_{};
};

// The sequence counter must be genuinely lock-free or the wait-free reader
// guarantee is void.
static_assert(std::atomic<u32>::is_always_lock_free,
              "SeqLock requires std::atomic<u32> to be lock-free");

} // namespace atx::core::concurrent
