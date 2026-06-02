#pragma once

// atx::core::concurrent::SpscQueue<T, Capacity> — wait-free single-producer /
// single-consumer bounded ring buffer.
//
// Exactly one thread calls try_push and exactly one (other) thread calls
// try_pop. Both are wait-free (no loops, no locks): each op does O(1) work and
// returns immediately with success/failure. This is the lowest-latency queue in
// the library and the right default for a one-to-one handoff (feed handler ->
// strategy, strategy -> order gateway).
//
// ============================================================================
//  Design
// ============================================================================
//  Monotonic 64-bit head_ (consumer) and tail_ (producer) counters index a
//  power-of-two slot array via a bit mask. Element count == tail_ - head_;
//  the queue is full when that equals Capacity and empty when it equals 0.
//  64-bit counters never realistically wrap, so no special wrap handling is
//  needed beyond masking the index.
//
//  Each side caches the other's counter (head_cache_ / tail_cache_) so the
//  common case touches only its own cache line: the producer re-reads the real
//  head_ only when its cache says "full", the consumer re-reads tail_ only when
//  its cache says "empty". This removes a cross-core load from the hot path.
//
// ============================================================================
//  Memory-ordering rationale (agent.md §5 — justify every ordering)
// ============================================================================
//  try_push:
//    - load tail_ relaxed: the producer is the only writer of tail_, so it
//      always reads its own latest value; no synchronisation needed.
//    - load head_ acquire (only on the "looks full" path): pairs with the
//      consumer's release store of head_ in try_pop, so the slot the consumer
//      just freed is safe for us to overwrite (its read of the old element
//      happens-before our construction of the new one).
//    - store tail_ release: publishes the newly-constructed element; the
//      consumer's acquire load of tail_ that observes this value also observes
//      the element bytes we wrote before it.
//  try_pop: symmetric, with the roles of head_/tail_ swapped.
//
// ============================================================================
//  False-sharing elimination
// ============================================================================
//  tail_ (+ its producer-private cache) and head_ (+ its consumer-private
//  cache) live on separate cache lines via ATX_CACHE_ALIGNED, so the producer
//  and consumer never invalidate each other's index line. The slot array starts
//  on its own line as well.
//
// ============================================================================
//  Contract
// ============================================================================
//  - One producer thread, one consumer thread. More of either is UB.
//  - No allocation after construction; capacity is fixed at compile time.
//  - T must be move- or copy-constructible (for push) and destructible.

#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

#include "atx/core/bit.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/platform.hpp"
#include "atx/core/types.hpp"

namespace atx::core::concurrent {

template <class T, usize Capacity>
class SpscQueue {
    static_assert(Capacity >= 2U, "SpscQueue: Capacity must be >= 2");
    static_assert(::atx::core::is_pow2(Capacity),
                  "SpscQueue: Capacity must be a power of two (index is masked)");

    static constexpr u64 kMask = static_cast<u64>(Capacity) - 1U;

    // Raw, correctly-aligned storage for one T; lifetime managed by placement
    // new / explicit destructor call.
    struct alignas(T) Slot {
        std::array<std::byte, sizeof(T)> storage;
    };

  public:
    using value_type = T;

    SpscQueue() noexcept = default;

    // Single-owner concurrency primitive; copying/moving would alias the live
    // head/tail state across two objects.
    ATX_DISABLE_COPY_MOVE(SpscQueue);

    ~SpscQueue() {
        // No concurrent access during destruction: drain whatever remains.
        const u64 head = head_.load(std::memory_order_relaxed);
        const u64 tail = tail_.load(std::memory_order_relaxed);
        for (u64 i = head; i != tail; ++i) {
            slot_ptr(i)->~T();
        }
    }

    // -------------------------------------------------------------------------
    //  try_push — producer side. Returns false if the queue is full.
    // -------------------------------------------------------------------------
    [[nodiscard]] bool try_push(const T& value) { return emplace(value); }
    [[nodiscard]] bool try_push(T&& value) { return emplace(std::move(value)); }

    /// Construct an element in place from args. Producer side.
    template <class... Args>
    [[nodiscard]] bool emplace(Args&&... args) {
        // SAFETY: relaxed — producer is the sole writer of tail_.
        const u64 t = tail_.load(std::memory_order_relaxed);

        if (t - head_cache_ == static_cast<u64>(Capacity)) {
            // Cache says full; refresh from the real head before giving up.
            // SAFETY: acquire — pairs with the consumer's release store of
            // head_; a slot it has freed becomes safe to reuse.
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ == static_cast<u64>(Capacity)) {
                return false; // genuinely full
            }
        }

        // SAFETY: slot storage is alignas(T), sizeof(T) bytes, and currently
        // vacant (index t is beyond head_, so no live element occupies it).
        ::new (static_cast<void*>(slot_ptr_raw(t))) T(std::forward<Args>(args)...);

        // SAFETY: release — publishes the element bytes to a consumer that
        // acquire-loads this new tail_ value.
        tail_.store(t + 1U, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    //  try_pop — consumer side. Returns false if the queue is empty.
    // -------------------------------------------------------------------------
    [[nodiscard]] bool try_pop(T& out) {
        // SAFETY: relaxed — consumer is the sole writer of head_.
        const u64 h = head_.load(std::memory_order_relaxed);

        if (h == tail_cache_) {
            // Cache says empty; refresh from the real tail before giving up.
            // SAFETY: acquire — pairs with the producer's release store of
            // tail_; the element it just published becomes visible to us.
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) {
                return false; // genuinely empty
            }
        }

        T* const elem = slot_ptr(h);
        out           = std::move(*elem);
        elem->~T();

        // SAFETY: release — frees the slot for the producer; a producer that
        // acquire-loads this new head_ may now reuse the slot.
        head_.store(h + 1U, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    //  Observers (approximate under concurrency; exact when quiescent).
    // -------------------------------------------------------------------------
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] usize size() const noexcept {
        const u64 t = tail_.load(std::memory_order_acquire);
        const u64 h = head_.load(std::memory_order_acquire);
        return static_cast<usize>(t - h);
    }

    [[nodiscard]] static constexpr usize capacity() noexcept { return Capacity; }

  private:
    [[nodiscard]] std::byte* slot_ptr_raw(u64 index) noexcept {
        return slots_[static_cast<usize>(index & kMask)].storage.data();
    }

    [[nodiscard]] T* slot_ptr(u64 index) noexcept {
        // SAFETY: index & kMask is in [0, Capacity); the slot holds a live T
        // (caller checked occupancy). std::launder ties the pointer to the
        // object whose lifetime began with placement-new.
        return std::launder(reinterpret_cast<T*>(slot_ptr_raw(index)));
    }

    // Producer line: tail_ (written by producer) + its private cache of head_.
    ATX_CACHE_ALIGNED std::atomic<u64> tail_{0};
    u64 head_cache_{0};

    // Consumer line: head_ (written by consumer) + its private cache of tail_.
    ATX_CACHE_ALIGNED std::atomic<u64> head_{0};
    u64 tail_cache_{0};

    // Storage on its own cache line.
    ATX_CACHE_ALIGNED std::array<Slot, Capacity> slots_{};
};

static_assert(std::atomic<u64>::is_always_lock_free,
              "SpscQueue requires std::atomic<u64> to be lock-free");

} // namespace atx::core::concurrent
