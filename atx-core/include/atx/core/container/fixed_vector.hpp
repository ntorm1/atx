#pragma once

// atx::core::container FixedVector — static-capacity contiguous vector.
//
// Design:
//   FixedVector<T, Capacity> is a vector-like container backed by a fixed,
//   inline array of aligned raw storage slots.  No dynamic allocation: the
//   entire buffer lives on the stack (or inside its parent object).
//
//   Storage:
//     Each slot is alignas(T) std::array<std::byte, sizeof(T)> — the same
//     pattern used by RingBuffer and ObjectPool.  Elements are placement-new'd
//     into slots 0..size_-1 in order, giving true contiguous layout so that
//     data() / begin() / end() expose a valid pointer range to the caller.
//
//   at() return type — Result<T*>:
//     Returns a raw (non-owning) T* wrapped in Result rather than
//     Result<std::reference_wrapper<T>>, so the caller can mutate through the
//     pointer and the implementation avoids a std::reference_wrapper
//     construction that would require a live T lvalue anyway.  The pointer is
//     never null on success; it is non-owning and borrows from this vector.
//
//   Copy / move:
//     Both are implemented (FixedVector is a value type).  Copy-constructs or
//     move-constructs each element in index order into the destination buffer.
//     During move, source elements are destroyed after being moved-from so the
//     moved-from vector is left empty and valid.
//
//   Thread-safety: NONE.  Synchronise externally when shared between threads.

#include <array>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "atx/core/error.hpp"  // Result, Err, ErrorCode
#include "atx/core/macro.hpp"  // ATX_ASSERT
#include "atx/core/types.hpp"  // usize

namespace atx::core::container {

/// Fixed-capacity contiguous vector with inline raw-storage.
///
/// @tparam T         Element type.  Must be destructible.
/// @tparam Capacity  Maximum number of elements; must be >= 1.
template <class T, usize Capacity>
class FixedVector {
    static_assert(Capacity >= 1U, "FixedVector: Capacity must be >= 1");

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

    /// Default-construct an empty vector.
    FixedVector() noexcept = default;

    /// Destroy all live elements.
    ~FixedVector() noexcept { clear(); }

    // ------------------------------------------------------------------
    // Copy constructor — copies elements in index order.
    // ------------------------------------------------------------------

    /// Copy-construct from other.  Requires T to be copy-constructible.
    FixedVector(const FixedVector& other) {
        copy_from(other);
    }

    /// Copy-assign.  Strong exception safety: build into a fresh buffer,
    /// then swap — if a copy-ctor throws the original is unmodified.
    FixedVector& operator=(const FixedVector& other) {
        if (this != &other) {
            FixedVector tmp{other};
            swap(tmp);
        }
        return *this;
    }

    // ------------------------------------------------------------------
    // Move constructor — moves elements in index order, leaves src empty.
    // ------------------------------------------------------------------

    /// Move-construct from other.  Leaves other empty after the operation.
    FixedVector(FixedVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        move_from(std::move(other));
    }

    /// Move-assign from other.  Destroys current elements, then moves src.
    FixedVector& operator=(FixedVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &other) {
            clear();
            move_from(std::move(other));
        }
        return *this;
    }

    // ------------------------------------------------------------------
    // push_back — append at tail (asserts capacity)
    // ------------------------------------------------------------------

    /// Append a copy of value.
    ///
    /// @pre size() < Capacity (asserted in debug builds; UB-adjacent in release)
    void push_back(const T& value) {
        ATX_ASSERT(size_ < Capacity);
        emplace_slot(size_, value);
        ++size_;
    }

    /// Append a moved value.
    ///
    /// @pre size() < Capacity
    void push_back(T&& value) {
        ATX_ASSERT(size_ < Capacity);
        emplace_slot(size_, std::move(value));
        ++size_;
    }

    // ------------------------------------------------------------------
    // try_push_back — returns false when full (no assert)
    // ------------------------------------------------------------------

    /// Append a copy of value.
    ///
    /// @return true if inserted; false if the vector is full.
    [[nodiscard]] bool try_push_back(const T& value) {
        if (size_ == Capacity) {
            return false;
        }
        emplace_slot(size_, value);
        ++size_;
        return true;
    }

    /// Append a moved value.
    ///
    /// @return true if inserted; false if the vector is full.
    [[nodiscard]] bool try_push_back(T&& value) {
        if (size_ == Capacity) {
            return false;
        }
        emplace_slot(size_, std::move(value));
        ++size_;
        return true;
    }

    // ------------------------------------------------------------------
    // emplace_back — in-place construct, returns reference
    // ------------------------------------------------------------------

    /// Construct a T in-place at the tail and return a reference to it.
    ///
    /// @pre size() < Capacity
    template <class... Args>
    T& emplace_back(Args&&... args) {
        ATX_ASSERT(size_ < Capacity);
        void* const mem = slots_[size_].storage.data();

        // SAFETY: mem points to alignas(T) storage of exactly sizeof(T) bytes.
        //   Placement-new begins the lifetime of a T object at mem.
        ::new (mem) T(std::forward<Args>(args)...);
        ++size_;
        return *slot_ptr(size_ - 1U);
    }

    // ------------------------------------------------------------------
    // pop_back — remove last element
    // ------------------------------------------------------------------

    /// Destroy the last element and decrement size.
    ///
    /// @pre !empty()
    void pop_back() noexcept {
        ATX_ASSERT(size_ > 0U);
        --size_;
        slot_ptr(size_)->~T();
    }

    // ------------------------------------------------------------------
    // Indexed access — operator[]
    // ------------------------------------------------------------------

    /// Mutable indexed access.
    ///
    /// @pre i < size()
    [[nodiscard]] T& operator[](usize i) noexcept {
        ATX_ASSERT(i < size_);
        return *slot_ptr(i);
    }

    /// Const indexed access.
    ///
    /// @pre i < size()
    [[nodiscard]] const T& operator[](usize i) const noexcept {
        ATX_ASSERT(i < size_);
        return *slot_ptr(i);
    }

    // ------------------------------------------------------------------
    // Bounds-checked access — at()
    //
    // Returns Result<T*>: a non-owning pointer on success, or
    // Err(ErrorCode::OutOfRange) when i >= size().  The pointer is never
    // null on the success path.
    // ------------------------------------------------------------------

    /// Bounds-checked mutable access.
    ///
    /// @return Ok(T*) — non-owning pointer to element, or Err(OutOfRange).
    [[nodiscard]] atx::core::Result<T*> at(usize i) noexcept {
        if (i >= size_) {
            return atx::core::Err(atx::core::ErrorCode::OutOfRange);
        }
        return atx::core::Ok(slot_ptr(i));
    }

    /// Bounds-checked const access.
    ///
    /// @return Ok(const T*) — non-owning pointer to element, or Err(OutOfRange).
    [[nodiscard]] atx::core::Result<const T*> at(usize i) const noexcept {
        if (i >= size_) {
            return atx::core::Err(atx::core::ErrorCode::OutOfRange);
        }
        return atx::core::Ok(slot_ptr(i));
    }

    // ------------------------------------------------------------------
    // front() / back()
    // ------------------------------------------------------------------

    /// Reference to the first element.
    ///
    /// @pre !empty()
    [[nodiscard]] T& front() noexcept {
        ATX_ASSERT(size_ > 0U);
        return *slot_ptr(0U);
    }

    [[nodiscard]] const T& front() const noexcept {
        ATX_ASSERT(size_ > 0U);
        return *slot_ptr(0U);
    }

    /// Reference to the last element.
    ///
    /// @pre !empty()
    [[nodiscard]] T& back() noexcept {
        ATX_ASSERT(size_ > 0U);
        return *slot_ptr(size_ - 1U);
    }

    [[nodiscard]] const T& back() const noexcept {
        ATX_ASSERT(size_ > 0U);
        return *slot_ptr(size_ - 1U);
    }

    // ------------------------------------------------------------------
    // Contiguous data pointer + iterators
    //
    // Elements are stored consecutively in slots_[0..size_-1], so a raw
    // T* into slot 0 is a valid pointer-range [data(), data()+size()).
    // ------------------------------------------------------------------

    /// Pointer to the first element (or one-past-end if empty).
    [[nodiscard]] T* data() noexcept {
        return slot_ptr(0U);
    }

    [[nodiscard]] const T* data() const noexcept {
        return slot_ptr(0U);
    }

    /// Iterator to the first element.
    [[nodiscard]] T* begin() noexcept { return data(); }
    [[nodiscard]] const T* begin() const noexcept { return data(); }

    /// One-past-the-end iterator.
    [[nodiscard]] T* end() noexcept { return data() + size_; }
    [[nodiscard]] const T* end() const noexcept { return data() + size_; }

    // ------------------------------------------------------------------
    // Observers
    // ------------------------------------------------------------------

    /// Number of live elements.
    [[nodiscard]] usize size() const noexcept { return size_; }

    /// Compile-time maximum capacity.
    [[nodiscard]] static constexpr usize capacity() noexcept { return Capacity; }

    /// True when the vector holds no elements.
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }

    /// True when the vector has reached its static capacity.
    [[nodiscard]] bool full() const noexcept { return size_ == Capacity; }

    // ------------------------------------------------------------------
    // clear — destroy all live elements
    // ------------------------------------------------------------------

    /// Destroy all live elements and reset size to zero.
    void clear() noexcept {
        // Destroy in reverse order so that LIFO-dependent destructors behave
        // predictably (matches std::vector conventions).
        for (usize i = size_; i > 0U; --i) {
            slot_ptr(i - 1U)->~T();
        }
        size_ = 0U;
    }

    // ------------------------------------------------------------------
    // swap — used by copy-assignment (copy-and-swap idiom)
    // ------------------------------------------------------------------

    void swap(FixedVector& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                           std::is_nothrow_destructible_v<T>) {
        // No cheap in-place swap for raw storage when sizes differ, so rebuild
        // via temporaries.  Capacity is fixed, so this is bounded-cost.
        FixedVector tmp{std::move(*this)};
        move_from(std::move(other));
        other.move_from(std::move(tmp));
    }

private:
    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------

    /// Return a typed pointer to the element at physical index idx.
    ///
    /// SAFETY: idx is always checked against size_ before calling (or the
    ///   caller has just placement-new'd at idx), so the slot is in-bounds.
    ///   std::launder is required after placement-new to establish the correct
    ///   pointer provenance for the newly-created T object (C++17/20 [ptr.launder]).
    [[nodiscard]] T* slot_ptr(usize idx) noexcept {
        // SAFETY: slots_[idx].storage is alignas(T), sizeof(T) bytes; idx in [0, Capacity).
        return std::launder(reinterpret_cast<T*>(slots_[idx].storage.data()));
    }

    [[nodiscard]] const T* slot_ptr(usize idx) const noexcept {
        // SAFETY: same as the non-const overload above.
        return std::launder(reinterpret_cast<const T*>(slots_[idx].storage.data()));
    }

    /// Placement-new a T into slot at idx, forwarding args.
    template <class... Args>
    void emplace_slot(usize idx, Args&&... args) {
        void* const mem = slots_[idx].storage.data();

        // SAFETY: mem is alignas(T) storage of sizeof(T) bytes.
        //   Placement-new begins the lifetime of a T object at mem.
        ::new (mem) T(std::forward<Args>(args)...);
    }

    /// Copy all elements from src into this buffer (assumed empty).
    void copy_from(const FixedVector& src) {
        ATX_ASSERT(size_ == 0U);
        for (usize i = 0U; i < src.size_; ++i) {
            void* const mem = slots_[i].storage.data();

            // SAFETY: mem is alignas(T) storage of sizeof(T) bytes.
            //   Placement-new copy-constructs T from src element.
            ::new (mem) T(*src.slot_ptr(i));
        }
        size_ = src.size_;
    }

    /// Move all elements from src into this buffer (assumed empty), clear src.
    void move_from(FixedVector&& src) noexcept(std::is_nothrow_move_constructible_v<T>) {
        ATX_ASSERT(size_ == 0U);
        for (usize i = 0U; i < src.size_; ++i) {
            T* const elem = src.slot_ptr(i);
            void* const mem = slots_[i].storage.data();

            // SAFETY: mem is alignas(T) storage of sizeof(T) bytes.
            //   Placement-new move-constructs T from src element.
            ::new (mem) T(std::move(*elem));

            // Destroy the source element now that it has been moved-from.
            elem->~T();
        }
        size_ = src.size_;

        // Leave src empty and consistent.
        src.size_ = 0U;
    }

    // ------------------------------------------------------------------
    // Data members
    // ------------------------------------------------------------------

    /// Inline storage for Capacity element slots — zero heap allocation.
    std::array<Slot, Capacity> slots_{};

    /// Number of currently live elements in [0, Capacity].
    usize size_{0U};
};

} // namespace atx::core::container
