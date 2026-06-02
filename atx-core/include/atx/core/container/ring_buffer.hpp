#pragma once

// atx::core::container RingBuffer — fixed-capacity circular buffer.
//
// Design:
//   RingBuffer<T, Capacity> is a FIFO queue backed by a fixed, inline array of
//   aligned raw storage slots.  Capacity is rounded up to the next power of two
//   so that index masking (& kMask) can replace modulo without any branch.
//
//   Storage:
//     Each slot is an alignas(T) std::array<std::byte, sizeof(T)> — the same
//     pattern used by ObjectPool.  No dynamic allocation; the buffer lives
//     entirely on the stack or inside its parent object.
//
//   Full/empty disambiguation:
//     An explicit `size_` counter is kept alongside `head_` (next-pop index)
//     and `tail_` (next-push index).  This is the simplest, clearest approach:
//     it avoids the extra wasted slot required by the "tail-chase" technique and
//     needs no bitwise tricks, at the cost of one extra usize member.
//
//   Indexing:
//     operator[] uses oldest-relative semantics: [0] is the oldest element,
//     [size-1] is the newest.  This is required for windowed-statistics use.
//
//   Copy / move:
//     Copy and move constructors/assignment are implemented.  They iterate
//     elements in oldest→newest order, copy/move-constructing each into the
//     destination buffer laid out identically (head_==0, contiguous).  This is
//     correct for any T that is copy/move constructible.
//
//   Thread-safety: NONE.  Synchronise externally if shared between threads.
//
// Usage example:
//   RingBuffer<double, 8> rb;
//   rb.push(1.0); rb.push(2.0);
//   auto v = rb.pop();          // v == std::optional<double>{1.0}
//   rb.push_overwrite(3.0);     // never fails; drops oldest when full

#include <array>
#include <cstddef>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include "atx/core/bit.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

namespace atx::core::container {

/// Fixed-capacity circular FIFO queue.
///
/// @tparam T         Element type.  Must be destructible.
/// @tparam Capacity  Requested capacity; rounded up to the next power of two.
///                   The actual capacity is available as `RingBuffer::capacity()`.
template <class T, usize Capacity>
class RingBuffer {
    static_assert(Capacity >= 1U, "RingBuffer: Capacity must be >= 1");

    // Round up to the nearest power-of-two.  next_pow2(1) == 1, so the minimum
    // actual capacity is always 1.
    static constexpr usize kCap  = atx::core::next_pow2(static_cast<usize>(Capacity));
    static constexpr usize kMask = kCap - 1U;

    static_assert(atx::core::is_pow2(kCap), "ring_buffer: kCap must be a power of two");

    // ------------------------------------------------------------------
    // Raw storage — one aligned slot per element position.
    // Using alignas(T) + std::array<std::byte, sizeof(T)> instead of the
    // deprecated std::aligned_storage (deprecated in C++23).
    // ------------------------------------------------------------------
    struct Slot {
        alignas(T) std::array<std::byte, sizeof(T)> storage;
    };

public:
    // ------------------------------------------------------------------
    // Construction / destruction
    // ------------------------------------------------------------------

    /// Default-construct an empty ring buffer.
    RingBuffer() noexcept = default;

    /// Destroy all live elements.
    ~RingBuffer() noexcept { clear(); }

    // ------------------------------------------------------------------
    // Copy constructor — copies elements in oldest→newest order.
    // ------------------------------------------------------------------

    /// Copy-construct from other.  Requires T to be copy-constructible.
    RingBuffer(const RingBuffer& other) {
        copy_from(other);
    }

    /// Copy-assign from other.  Strong exception safety: if a copy throws,
    /// the original state is preserved (we build into a fresh buffer, then swap).
    RingBuffer& operator=(const RingBuffer& other) {
        if (this != &other) {
            RingBuffer tmp{other};
            swap(tmp);
        }
        return *this;
    }

    // ------------------------------------------------------------------
    // Move constructor — moves elements in oldest→newest order.
    // ------------------------------------------------------------------

    /// Move-construct from other.  Leaves other empty after the operation.
    RingBuffer(RingBuffer&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        move_from(std::move(other));
    }

    /// Move-assign from other.  Leaves other empty.
    RingBuffer& operator=(RingBuffer&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &other) {
            clear();
            move_from(std::move(other));
        }
        return *this;
    }

    // ------------------------------------------------------------------
    // Push — append at tail
    // ------------------------------------------------------------------

    /// Append a copy of value at the tail.
    ///
    /// @return true if the element was inserted; false if the buffer is full.
    [[nodiscard]] bool push(const T& value) {
        if (full()) {
            return false;
        }
        emplace_at_tail(value);
        return true;
    }

    /// Append a moved value at the tail.
    ///
    /// @return true if the element was inserted; false if the buffer is full.
    [[nodiscard]] bool push(T&& value) {
        if (full()) {
            return false;
        }
        emplace_at_tail(std::move(value));
        return true;
    }

    // ------------------------------------------------------------------
    // Push-overwrite — append, dropping oldest on overflow
    // ------------------------------------------------------------------

    /// Append a copy of value.  If the buffer is full, the oldest element is
    /// destroyed and dropped to make room.
    void push_overwrite(const T& value) {
        if (full()) {
            drop_oldest();
        }
        emplace_at_tail(value);
    }

    /// Append a moved value.  If the buffer is full, the oldest element is
    /// destroyed and dropped to make room.
    void push_overwrite(T&& value) {
        if (full()) {
            drop_oldest();
        }
        emplace_at_tail(std::move(value));
    }

    // ------------------------------------------------------------------
    // Pop — remove and return oldest element
    // ------------------------------------------------------------------

    /// Remove and return the oldest element (FIFO order).
    ///
    /// @return The element wrapped in std::optional, or std::nullopt if empty.
    [[nodiscard]] std::optional<T> pop() {
        if (empty()) {
            return std::nullopt;
        }

        T* const p = slot_ptr(head_);

        // SAFETY: head_ slot was constructed via placement-new in emplace_at_tail
        //   and has not been destroyed.  We move-construct a value to return, then
        //   explicitly destroy the element to end its lifetime before advancing.
        std::optional<T> result{std::move(*p)};
        p->~T();

        head_ = (head_ + 1U) & kMask;
        --size_;
        return result;
    }

    // ------------------------------------------------------------------
    // Indexed window access — oldest-relative
    // ------------------------------------------------------------------

    /// Return a const reference to the i-th oldest element.  [0] is oldest.
    ///
    /// @pre i < size()
    [[nodiscard]] const T& operator[](usize i) const noexcept {
        ATX_ASSERT(i < size_);
        return *slot_ptr((head_ + i) & kMask);
    }

    /// Return a mutable reference to the i-th oldest element.  [0] is oldest.
    ///
    /// @pre i < size()
    [[nodiscard]] T& operator[](usize i) noexcept {
        ATX_ASSERT(i < size_);
        return *slot_ptr((head_ + i) & kMask);
    }

    // ------------------------------------------------------------------
    // Observers
    // ------------------------------------------------------------------

    /// Number of live elements currently in the buffer.
    [[nodiscard]] usize size() const noexcept { return size_; }

    /// True if the buffer holds no elements.
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }

    /// True if the buffer holds kCap elements (no more can be pushed).
    [[nodiscard]] bool full() const noexcept { return size_ == kCap; }

    /// Maximum number of elements the buffer can hold (compile-time constant).
    [[nodiscard]] static constexpr usize capacity() noexcept { return kCap; }

    // ------------------------------------------------------------------
    // Clear
    // ------------------------------------------------------------------

    /// Destroy all live elements and reset the buffer to empty.
    void clear() noexcept {
        // Destroy in oldest→newest order so that elements with order-dependent
        // destructors (e.g., reference-counted chains) behave predictably.
        for (usize i = 0U; i < size_; ++i) {
            slot_ptr((head_ + i) & kMask)->~T();
        }
        head_ = 0U;
        tail_ = 0U;
        size_ = 0U;
    }

    // ------------------------------------------------------------------
    // Swap
    // ------------------------------------------------------------------

    /// Swap two ring buffers.  Used internally by copy-assignment.
    void swap(RingBuffer& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                          std::is_nothrow_destructible_v<T>) {
        // There is no cheap way to swap raw storage in-place when heads differ,
        // so we rebuild each side via temporary.  For a ring buffer this is fine:
        // it holds at most kCap elements and is not expected to be swapped on a
        // hot path.
        RingBuffer tmp{std::move(*this)};
        move_from(std::move(other));
        other.move_from(std::move(tmp));
    }

private:
    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------

    /// Return a typed pointer to slot at physical index idx.
    ///
    /// SAFETY: idx is always masked before use (& kMask), so it is in [0, kCap).
    ///   The Slot array has exactly kCap elements, so the access is in-bounds.
    ///   The caller is responsible for ensuring the slot is live before
    ///   dereferencing the returned pointer.
    [[nodiscard]] T* slot_ptr(usize idx) noexcept {
        // SAFETY: slots_[idx].storage is alignas(T), sizeof(T) bytes.
        //   std::launder is required after placement-new to avoid pointer
        //   provenance issues (C++17/20 [ptr.launder]).
        return std::launder(reinterpret_cast<T*>(slots_[idx].storage.data()));
    }

    [[nodiscard]] const T* slot_ptr(usize idx) const noexcept {
        // SAFETY: same as the non-const overload above.
        return std::launder(reinterpret_cast<const T*>(slots_[idx].storage.data()));
    }

    /// Construct a T in-place at the current tail slot and advance tail/size.
    template <class U>
    void emplace_at_tail(U&& value) {
        void* const mem = slots_[tail_].storage.data();

        // SAFETY: mem points to alignas(T) storage of exactly sizeof(T) bytes.
        //   Placement-new begins the lifetime of a T object at mem.
        ::new (mem) T(std::forward<U>(value));

        tail_ = (tail_ + 1U) & kMask;
        ++size_;
    }

    /// Destroy the oldest element and advance head.  Called only when full.
    void drop_oldest() noexcept {
        ATX_ASSERT(!empty());
        slot_ptr(head_)->~T();
        head_ = (head_ + 1U) & kMask;
        --size_;
    }

    /// Copy all elements from src into this buffer (assumed empty).
    /// Elements are copied in oldest→newest order; head_ is set to 0.
    void copy_from(const RingBuffer& src) {
        // Precondition: the destination must hold no live elements. We construct
        // into slots_[0..) and overwrite head_/tail_/size_, so any pre-existing
        // element would be leaked (never destroyed) and the indices corrupted.
        ATX_ASSERT(size_ == 0U);
        for (usize i = 0U; i < src.size_; ++i) {
            const T* const elem = src.slot_ptr((src.head_ + i) & kMask);
            void* const mem = slots_[i].storage.data();

            // SAFETY: mem is alignas(T) storage of sizeof(T) bytes.
            //   Placement-new copy-constructs T.
            ::new (mem) T(*elem);
        }
        head_ = 0U;
        tail_ = src.size_ & kMask; // tail wraps if size == kCap
        size_ = src.size_;
    }

    /// Move all elements from src into this buffer (assumed empty), then clear src.
    void move_from(RingBuffer&& src) noexcept(std::is_nothrow_move_constructible_v<T>) {
        // Precondition: same as copy_from — the destination must be empty, since
        // we construct into slots_[0..) and overwrite the indices wholesale.
        ATX_ASSERT(size_ == 0U);
        for (usize i = 0U; i < src.size_; ++i) {
            T* const elem = src.slot_ptr((src.head_ + i) & kMask);
            void* const mem = slots_[i].storage.data();

            // SAFETY: mem is alignas(T) storage of sizeof(T) bytes.
            //   Placement-new move-constructs T.
            ::new (mem) T(std::move(*elem));

            // Destroy the source element now that it has been moved-from.
            elem->~T();
        }
        head_ = 0U;
        tail_ = src.size_ & kMask;
        size_ = src.size_;

        // Leave src empty and consistent.
        src.head_ = 0U;
        src.tail_ = 0U;
        src.size_ = 0U;
    }

    // ------------------------------------------------------------------
    // Data members
    // ------------------------------------------------------------------

    /// Inline storage for kCap element slots — no heap allocation.
    std::array<Slot, kCap> slots_{};

    /// Physical index of the oldest live element (next to be popped).
    usize head_{0U};

    /// Physical index of the next empty slot (next push writes here).
    usize tail_{0U};

    /// Number of currently live elements.
    usize size_{0U};
};

} // namespace atx::core::container
