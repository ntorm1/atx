#pragma once

// atx::core ObjectPool — fixed-capacity O(1) pool allocator.
//
// Design:
//   ObjectPool<T, Capacity> manages a fixed array of Capacity slots of aligned
//   raw storage for T objects.  Acquisition and release are both O(1) via an
//   index stack (a separate std::array<u32, Capacity> used as a LIFO free-list).
//
//   Free-list strategy — index stack (LIFO):
//     A top-of-stack index `free_top_` tracks how many free slots remain.
//     `free_stack_[0 .. free_top_-1]` holds the indices of available slots.
//     acquire() pops the top; release() pushes the returned slot back.
//     This is slightly larger in memory than an intrusive list (one extra
//     u32 array) but simpler, safer, and avoids any aliasing between the
//     T-storage and the list linkage.
//
//   Storage:
//     Each slot is a struct containing an alignas(T) std::byte array of
//     sizeof(T) bytes.  std::aligned_storage is deprecated in C++23; this
//     pattern is the recommended replacement.
//
//   Dtor policy:
//     The pool requires all live objects to be released before the pool is
//     destroyed.  In debug builds the destructor asserts size() == 0.
//     In release the destructor is a no-op (no implicit destruction of live
//     objects — that would require tracking which slots are live, adding
//     complexity and memory; instead we enforce the contract via the assert).
//     Rationale: silently destroying live objects in the dtor would hide
//     lifetime bugs; asserting makes the violation loud and immediately
//     actionable.
//
//   Copy/move:
//     Deleted via ATX_DISABLE_COPY_MOVE.  Rationale: callers hold raw T*
//     pointers into the pool's inline storage.  If the pool were moved or
//     copied, all outstanding pointers would dangle.  Making the pool
//     non-copyable and non-movable prevents this class of error at compile
//     time.
//
//   Thread-safety: NONE.  Synchronise externally if multiple threads
//     share an ObjectPool.
//
// Usage example:
//   ObjectPool<Widget, 64> pool;
//   Widget* w = pool.acquire(ctor_arg1, ctor_arg2);
//   if (w == nullptr) { /* pool full */ }
//   // … use w …
//   pool.release(w);   // calls ~Widget()

#include <array>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

namespace atx::core {

/// Fixed-capacity O(1) object pool.
///
/// @tparam T         The type of objects managed by this pool.
/// @tparam Capacity  Maximum number of simultaneously live objects.
///
/// @note  All objects must be released before the pool is destroyed.
///        In debug builds the destructor asserts size() == 0.
template <class T, usize Capacity>
class ObjectPool {
    static_assert(Capacity > 0U, "ObjectPool: Capacity must be > 0");

public:
    // ------------------------------------------------------------------
    // Construction / destruction
    // ------------------------------------------------------------------

    ObjectPool() noexcept {
        // Populate the free stack with every slot index (0 .. Capacity-1).
        // After construction all slots are free; free_top_ == Capacity.
        for (u32 i = 0U; i < static_cast<u32>(Capacity); ++i) {
            free_stack_[i] = i;
        }
    }

    ~ObjectPool() noexcept {
        // SAFETY: Callers are required to release all live objects before
        //   destroying the pool.  Asserting here (debug-only) surfaces the
        //   violation immediately.  In release we leave it as a no-op —
        //   implicitly destroying live objects would require liveness
        //   tracking and would hide caller bugs rather than expose them.
        ATX_ASSERT(live_count_ == 0U);
    }

    // Pool owns inline storage; copy/move would dangle all outstanding T*.
    ATX_DISABLE_COPY_MOVE(ObjectPool);

    // ------------------------------------------------------------------
    // Acquire — construct a T in a free slot
    // ------------------------------------------------------------------

    /// Construct a T in a free slot and return a pointer to it.
    ///
    /// @param args  Arguments forwarded to T's constructor.
    /// @return      Pointer to the constructed object, or nullptr if full.
    ///
    /// @note  The returned pointer is valid until release() is called on it.
    template <class... Args>
    [[nodiscard]] T* acquire(Args&&... args) {
        if (free_top_ == 0U) {
            return nullptr;
        }

        const u32 idx = free_stack_[--free_top_];
        void* const mem = slots_[idx].storage.data();

        // SAFETY: mem points to alignas(T) storage of exactly sizeof(T) bytes
        //   (enforced by the Slot struct below).  Placement-new begins the
        //   lifetime of a T object at mem.  The pointer is returned to the
        //   caller who owns the object until release() is called.
        T* const obj = ::new (mem) T(std::forward<Args>(args)...);
        ++live_count_;
        return obj;
    }

    // ------------------------------------------------------------------
    // Release — destroy a T and return its slot to the free list
    // ------------------------------------------------------------------

    /// Destroy the T at p and return its slot to the pool.
    ///
    /// @param p  Pointer previously returned by acquire().
    ///           Must be non-null, must point into this pool, and the slot
    ///           must currently be live (debug assertions enforce all three).
    ///
    /// @pre  p != nullptr
    /// @pre  p was returned by acquire() on *this and has not been released.
    void release(T* const p) noexcept {
        ATX_ASSERT(p != nullptr);
        ATX_ASSERT(owns(p)); // pointer must be inside this pool's storage

        // SAFETY: p was constructed via placement-new in acquire().
        //   Calling the destructor explicitly ends the object's lifetime
        //   without freeing memory (the storage belongs to the pool).
        //   This is the documented correct pattern for placement-new paired
        //   with explicit destructor call (C++ standard [basic.life]).
        p->~T();

        const u32 idx = slot_index(p);
        ATX_ASSERT(free_top_ < static_cast<u32>(Capacity)); // pool must not be empty
        free_stack_[free_top_++] = idx;
        --live_count_;
    }

    // ------------------------------------------------------------------
    // Observers
    // ------------------------------------------------------------------

    /// Number of live (acquired, not yet released) objects.
    [[nodiscard]] usize size() const noexcept { return live_count_; }

    /// Maximum objects the pool can hold (compile-time constant).
    [[nodiscard]] constexpr usize capacity() const noexcept { return Capacity; }

    /// Number of slots available for acquisition.
    [[nodiscard]] usize available() const noexcept { return free_top_; }

private:
    // ------------------------------------------------------------------
    // Internal types
    // ------------------------------------------------------------------

    /// One slot of aligned raw storage for a T.
    ///
    /// Using alignas(T) + std::array<std::byte, sizeof(T)> instead of the
    /// deprecated std::aligned_storage (deprecated in C++23).
    struct Slot {
        alignas(T) std::array<std::byte, sizeof(T)> storage;
    };

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    /// Convert a T* (within this pool) to its slot index.
    [[nodiscard]] u32 slot_index(const T* const p) const noexcept {
        // SAFETY: p is within slots_ (verified by owns() in debug before this
        //   call).  Casting to const std::byte* and computing the byte offset
        //   is well-defined: std::byte is a character type and pointer
        //   arithmetic within the same array object is defined.
        const auto* const base =
            reinterpret_cast<const std::byte*>(slots_.data()); // SAFETY: char-type alias
        const auto* const ptr  =
            reinterpret_cast<const std::byte*>(p);             // SAFETY: char-type alias
        const usize byte_offset = static_cast<usize>(ptr - base);
        return static_cast<u32>(byte_offset / sizeof(Slot));
    }

    /// Return true if p points to the beginning of a slot in this pool.
    [[nodiscard]] bool owns(const T* const p) const noexcept {
        const auto* const base = reinterpret_cast<const std::byte*>(slots_.data());
        const auto* const ptr  = reinterpret_cast<const std::byte*>(p);
        const auto* const end  = base + sizeof(Slot) * Capacity;
        if (ptr < base || ptr >= end) {
            return false;
        }
        // Must be exactly on a slot boundary.
        const usize byte_offset = static_cast<usize>(ptr - base);
        return (byte_offset % sizeof(Slot)) == 0U;
    }

    // ------------------------------------------------------------------
    // Data members
    // ------------------------------------------------------------------

    /// Inline storage for all slots — no heap allocation.
    std::array<Slot, Capacity> slots_{};

    /// LIFO index stack of free slot indices.
    /// free_stack_[0 .. free_top_-1] are valid free indices.
    std::array<u32, Capacity> free_stack_{};

    /// Top-of-stack for free_stack_; equals number of available slots.
    u32 free_top_{static_cast<u32>(Capacity)};

    /// Number of currently live (acquired) objects.
    usize live_count_{0U};
};

} // namespace atx::core
