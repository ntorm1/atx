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
//   Misuse detection (debug-only):
//     A per-slot liveness flag array `live_[]` is compiled in only when
//     NDEBUG is not defined.  acquire() marks a slot live; release() asserts
//     the slot is currently live BEFORE freeing it and then marks it dead.
//     This catches double-release and release-of-never-acquired in debug —
//     a bare free_top_ < Capacity check cannot (when other slots are live a
//     double-release silently pushes the same index twice, aliasing storage
//     on the next two acquires).  The flag array and its logic carry zero
//     cost in release builds (entirely #ifndef NDEBUG'd out).
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
#include <limits>
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
    static_assert(Capacity <= static_cast<usize>(std::numeric_limits<u32>::max()),
                  "ObjectPool: Capacity must fit in u32 (free-stack uses u32 indices)");

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
#ifndef NDEBUG
        live_[idx] = true; // debug-only misuse tracker (see release())
#endif
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

        // SAFETY: slot_index() computes a byte offset over Slot::storage from
        //   the pointer value alone; it performs no access through p.  It is
        //   called BEFORE ~T() so the index is in hand for the debug liveness
        //   check.  (Pointer arithmetic on the std::byte storage address would
        //   remain well-defined even after the object's lifetime ends — see
        //   the post-lifetime SAFETY note below.)
        const u32 idx = slot_index(p);

        // Debug-only misuse detection: the slot MUST currently be live.
        // Asserting here catches double-release (the index is already free)
        // and release-of-never-acquired.  A bare free_top_ < Capacity check
        // cannot — with other slots live, a second release would silently
        // push the same index twice and alias storage on the next acquires.
#ifndef NDEBUG
        ATX_ASSERT(live_[idx]); // not live ⇒ double-release or never acquired
#endif

        // SAFETY: p was constructed via placement-new in acquire().
        //   Calling the destructor explicitly ends the object's lifetime
        //   without freeing memory (the storage belongs to the pool).
        //   This is the documented correct pattern for placement-new paired
        //   with explicit destructor call (C++ standard [basic.life]).
        p->~T();

        // SAFETY: ~T() has ended the T object's lifetime, but the underlying
        //   Slot::storage bytes are still alive (the Slot, and thus its
        //   std::byte array, outlives the contained T).  Recording the freed
        //   index and marking the slot dead operates purely on the pool's own
        //   bookkeeping and on the still-valid storage address — no access
        //   through the dead T occurs.  Pointer arithmetic over the byte
        //   storage remains well-defined post-lifetime ([basic.life]).
        ATX_ASSERT(free_top_ < static_cast<u32>(Capacity)); // pool must not be empty
        free_stack_[free_top_++] = idx;
        --live_count_;
#ifndef NDEBUG
        live_[idx] = false; // slot is now free
#endif
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

#ifndef NDEBUG
    /// Debug-only per-slot liveness flags for misuse detection (double-release
    /// / release-of-never-acquired).  Zero cost in release builds — the entire
    /// member is #ifndef NDEBUG'd out.  Value-initialised to all-false.
    std::array<bool, Capacity> live_{};
#endif
};

} // namespace atx::core
