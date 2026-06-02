#pragma once

// atx::core::concurrent::MpmcQueue<T, Capacity> — Dmitry Vyukov's bounded
// multi-producer / multi-consumer lock-free queue.
//
// Any number of producer threads may try_push and any number of consumer
// threads may try_pop concurrently. It is lock-free (a stalled thread never
// blocks others) though not wait-free: a producer/consumer may retry its CAS
// when it loses a race. Use this for many-to-many fan-in/fan-out (e.g. a shared
// work queue); prefer SpscQueue for a one-to-one handoff and Disruptor for
// single-producer broadcast.
//
// ============================================================================
//  Algorithm (Vyukov bounded MPMC)
// ============================================================================
//  Each of the Capacity cells carries its own sequence counter. A cell is
//  ready-to-enqueue at position p when its sequence == p, and ready-to-dequeue
//  when its sequence == p + 1. Producers claim an enqueue ticket by CAS on
//  enqueue_pos_; consumers claim a dequeue ticket by CAS on dequeue_pos_. After
//  writing/reading the payload, the owner bumps the cell's sequence to hand the
//  cell to the other role (enqueue -> +1 makes it dequeueable; dequeue ->
//  +Capacity makes it enqueueable one lap later).
//
//  The signed difference dif = (i64)seq - (i64)pos drives the decision:
//    dif == 0 : cell is ours to claim — attempt the position CAS.
//    dif <  0 : cell not yet released by the previous lap — queue full/empty.
//    dif >  0 : another thread already claimed this position — reload and retry.
//
// ============================================================================
//  Memory-ordering rationale (agent.md §5 — justify every ordering)
// ============================================================================
//  - cell.sequence load: acquire. Pairs with the release store of the cell's
//    sequence by whoever last handed the cell over, so the payload that thread
//    wrote (producer) or the slot it freed (consumer) is correctly visible.
//  - enqueue_pos_/dequeue_pos_ CAS: relaxed. These counters only allocate a
//    ticket; the actual data publication/visibility is carried by the per-cell
//    acquire/release on sequence, not by the position counters. A relaxed CAS
//    is sufficient because losing the race simply reloads and retries, and
//    winning is followed by the release store that publishes the payload.
//  - cell.sequence store after writing/reading: release. Publishes the payload
//    (enqueue) or the freed slot (dequeue) to the acquiring peer.
//
// ============================================================================
//  False-sharing elimination
// ============================================================================
//  enqueue_pos_ and dequeue_pos_ each occupy their own cache line so producers
//  and consumers do not ping-pong each other's ticket counter. (Per-cell
//  padding is intentionally omitted, matching the reference implementation —
//  the per-cell sequence already decouples adjacent cells in the common case.)
//
// ============================================================================
//  Contract
// ============================================================================
//  - Many producers, many consumers — all concurrent-safe.
//  - No allocation after construction; Capacity fixed at compile time.
//  - T must be move- or copy-constructible (push), move-assignable (pop) and
//    destructible.

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
class MpmcQueue {
    static_assert(Capacity >= 2U, "MpmcQueue: Capacity must be >= 2");
    static_assert(::atx::core::is_pow2(Capacity),
                  "MpmcQueue: Capacity must be a power of two (index is masked)");

    static constexpr u64 kMask = static_cast<u64>(Capacity) - 1U;

    struct Cell {
        std::atomic<u64> sequence;
        // T-aligned payload storage. Alignment is applied to the member (not the
        // whole Cell) so Cell still satisfies std::atomic<u64>'s 8-byte minimum
        // even when alignof(T) is smaller.
        alignas(T) std::array<std::byte, sizeof(T)> storage;
    };

  public:
    using value_type = T;

    MpmcQueue() noexcept {
        // Seed each cell's sequence with its index: cell i is initially ready to
        // be enqueued at position i.
        for (usize i = 0U; i < Capacity; ++i) {
            buffer_[i].sequence.store(static_cast<u64>(i), std::memory_order_relaxed);
        }
    }

    ATX_DISABLE_COPY_MOVE(MpmcQueue);

    ~MpmcQueue() {
        // No concurrent access during destruction: every position in
        // [dequeue_pos_, enqueue_pos_) holds a live element — destroy them.
        const u64 deq = dequeue_pos_.load(std::memory_order_relaxed);
        const u64 enq = enqueue_pos_.load(std::memory_order_relaxed);
        for (u64 pos = deq; pos != enq; ++pos) {
            cell_value(buffer_[static_cast<usize>(pos & kMask)])->~T();
        }
    }

    // -------------------------------------------------------------------------
    //  try_push — returns false if the queue is full.
    // -------------------------------------------------------------------------
    [[nodiscard]] bool try_push(const T& value) { return emplace(value); }
    [[nodiscard]] bool try_push(T&& value) { return emplace(std::move(value)); }

    template <class... Args>
    [[nodiscard]] bool emplace(Args&&... args) {
        Cell* cell = nullptr;
        // SAFETY: relaxed — only allocating a ticket; payload visibility is via
        // the per-cell release store below.
        u64 pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[static_cast<usize>(pos & kMask)];
            // SAFETY: acquire — pairs with a consumer's release store that freed
            // this cell, so reusing its storage is safe.
            const u64 seq = cell->sequence.load(std::memory_order_acquire);
            const i64 dif = static_cast<i64>(seq) - static_cast<i64>(pos);
            if (dif == 0) {
                // SAFETY: relaxed CAS — claims the enqueue ticket; correctness
                // of the data handoff is provided by the release store later.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1U,
                                                       std::memory_order_relaxed)) {
                    break; // claimed position `pos`
                }
                // CAS failed: `pos` was reloaded with the current value; retry.
            } else if (dif < 0) {
                return false; // full: previous lap has not released this cell
            } else {
                // Another producer claimed this position; reload and retry.
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }

        // SAFETY: storage is alignas(T), sizeof(T) bytes, and free (we hold the
        // ticket for `pos`). Placement-new begins the element's lifetime.
        ::new (static_cast<void*>(cell->storage.data())) T(std::forward<Args>(args)...);

        // SAFETY: release — publishes the element; a consumer that acquire-loads
        // sequence == pos+1 sees the fully-written payload.
        cell->sequence.store(pos + 1U, std::memory_order_release);
        return true;
    }

    // -------------------------------------------------------------------------
    //  try_pop — returns false if the queue is empty.
    // -------------------------------------------------------------------------
    [[nodiscard]] bool try_pop(T& out) {
        Cell* cell = nullptr;
        // SAFETY: relaxed — ticket allocation only (see emplace).
        u64 pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &buffer_[static_cast<usize>(pos & kMask)];
            // SAFETY: acquire — pairs with the producer's release store of
            // sequence == pos+1, making the published payload visible.
            const u64 seq = cell->sequence.load(std::memory_order_acquire);
            const i64 dif = static_cast<i64>(seq) - static_cast<i64>(pos + 1U);
            if (dif == 0) {
                // SAFETY: relaxed CAS — claims the dequeue ticket.
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1U,
                                                       std::memory_order_relaxed)) {
                    break; // claimed position `pos`
                }
            } else if (dif < 0) {
                return false; // empty: producer has not published this cell yet
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }

        T* const elem = cell_value(*cell);
        out           = std::move(*elem);
        elem->~T();

        // SAFETY: release — frees the cell for the producer one lap ahead; a
        // producer acquire-loading sequence == pos+Capacity may reuse it.
        cell->sequence.store(pos + static_cast<u64>(Capacity), std::memory_order_release);
        return true;
    }

    [[nodiscard]] static constexpr usize capacity() noexcept { return Capacity; }

  private:
    [[nodiscard]] static T* cell_value(Cell& c) noexcept {
        // SAFETY: storage holds a live T whenever the caller has established the
        // cell is occupied; launder ties the pointer to that object's lifetime.
        return std::launder(reinterpret_cast<T*>(c.storage.data()));
    }

    // Producer ticket counter and consumer ticket counter on separate lines.
    ATX_CACHE_ALIGNED std::atomic<u64> enqueue_pos_{0};
    ATX_CACHE_ALIGNED std::atomic<u64> dequeue_pos_{0};

    // Cell array on its own cache line.
    ATX_CACHE_ALIGNED std::array<Cell, Capacity> buffer_{};
};

static_assert(std::atomic<u64>::is_always_lock_free,
              "MpmcQueue requires std::atomic<u64> to be lock-free");

} // namespace atx::core::concurrent
