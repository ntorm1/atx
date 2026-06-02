#pragma once

// atx::core::container SmallVector — inline-first, heap-spill vector.
//
// Design:
//   SmallVector<T, N> holds up to N elements in inline raw storage (no heap).
//   When size_ would exceed capacity_, it spills to a heap buffer allocated via
//   aligned_alloc_bytes, and grows by 2x on each subsequent reallocation.
//
//   Inline storage:
//     N slots of alignas(T) std::array<std::byte, sizeof(T)> — same pattern as
//     FixedVector.  Elements are placement-new'd; the raw slot array is never
//     directly accessed as T[] to preserve object lifetimes.
//
//   Heap storage:
//     When spilled, data_ points to an aligned heap buffer obtained from
//     aligned_alloc_bytes(cap * sizeof(T), alignof(T)).  The buffer is freed
//     with aligned_free on destruction / re-grow / clear-while-spilled.
//
//   Move semantics (the critical case):
//     - Spilled source: steal heap pointer, set source back to inline state
//       (size 0, capacity N, data_ → inline storage).  O(1), no element moves.
//     - Inline source: element-wise move-construct into dest inline storage,
//       destroy source elements.  Can't steal the source's inline buffer
//       address because it belongs to the source object's storage.
//
//   Copy semantics:
//     Deep copy — copy-construct each element in order.  If src is spilled,
//     allocate a fresh heap buffer of the same capacity.
//
//   Thread-safety: NONE. Synchronise externally.

#include <array>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "atx/core/aligned.hpp"   // aligned_alloc_bytes, aligned_free
#include "atx/core/error.hpp"     // Result, Err, ErrorCode
#include "atx/core/macro.hpp"     // ATX_ASSERT
#include "atx/core/types.hpp"     // usize

namespace atx::core::container {

/// Inline-first vector: stores up to N elements without heap allocation, then
/// spills to a heap-allocated buffer that grows by 2x on each reallocation.
///
/// @tparam T  Element type.  Must be destructible and move-constructible.
/// @tparam N  Inline capacity; must be >= 1.
template <class T, usize N>
class SmallVector {
    static_assert(N >= 1U, "SmallVector: N must be >= 1");

    // ------------------------------------------------------------------
    // Inline raw storage — N slots, each aligned for T.
    // ------------------------------------------------------------------
    struct Slot {
        alignas(T) std::array<std::byte, sizeof(T)> storage;
    };

public:
    // ---- Type aliases ----
    using value_type      = T;
    using size_type       = usize;
    using reference       = T&;
    using const_reference = const T&;
    using pointer         = T*;
    using const_pointer   = const T*;
    using iterator        = T*;
    using const_iterator  = const T*;

    // ------------------------------------------------------------------
    // Default constructor — all elements inline, size 0.
    // ------------------------------------------------------------------

    SmallVector() noexcept
        : data_{inline_ptr()}, size_{0U}, capacity_{N} {}

    // ------------------------------------------------------------------
    // Destructor — destroy live elements, free heap if spilled.
    // ------------------------------------------------------------------

    ~SmallVector() noexcept { destroy_and_free(); }

    // ------------------------------------------------------------------
    // Copy constructor — deep copy.
    // ------------------------------------------------------------------

    SmallVector(const SmallVector& other) : SmallVector() {
        copy_from(other);
    }

    // ------------------------------------------------------------------
    // Copy assignment — copy-and-swap for strong exception safety.
    // ------------------------------------------------------------------

    SmallVector& operator=(const SmallVector& other) {
        if (this != &other) {
            SmallVector tmp{other};
            swap_with(tmp);
        }
        return *this;
    }

    // ------------------------------------------------------------------
    // Move constructor.
    //
    // SAFETY: Two distinct strategies based on whether src is spilled.
    //   Spilled:  data_ pointer is heap-allocated and portable — we steal it.
    //   Inline:   data_ points into src's inline_slots_, which is part of the
    //             src object on the stack.  We CANNOT take that address; we
    //             must element-wise move-construct into our own inline_slots_.
    // ------------------------------------------------------------------

    SmallVector(SmallVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : SmallVector()
    {
        steal_from(std::move(other));
    }

    // ------------------------------------------------------------------
    // Move assignment.
    // ------------------------------------------------------------------

    SmallVector& operator=(SmallVector&& other)
        noexcept(std::is_nothrow_move_constructible_v<T>)
    {
        if (this != &other) {
            destroy_and_free();
            // Re-initialise to inline state before stealing.
            data_     = inline_ptr();
            size_     = 0U;
            capacity_ = N;
            steal_from(std::move(other));
        }
        return *this;
    }

    // ------------------------------------------------------------------
    // push_back (copy)
    // ------------------------------------------------------------------

    void push_back(const T& value) {
        grow_if_full();
        // SAFETY: data_ is either inline_slots_ or a heap buffer of capacity_
        //   elements.  size_ < capacity_ is guaranteed by grow_if_full().
        //   Placement-new begins lifetime of T at data_[size_].
        ::new (static_cast<void*>(data_ + size_)) T(value);
        ++size_;
    }

    // ------------------------------------------------------------------
    // push_back (move)
    // ------------------------------------------------------------------

    void push_back(T&& value) {
        grow_if_full();
        // SAFETY: same as copy overload above.
        ::new (static_cast<void*>(data_ + size_)) T(std::move(value));
        ++size_;
    }

    // ------------------------------------------------------------------
    // emplace_back — in-place construct and return reference.
    // ------------------------------------------------------------------

    template <class... Args>
    T& emplace_back(Args&&... args) {
        grow_if_full();
        // SAFETY: data_ + size_ is within the live buffer; placement-new
        //   begins the lifetime of a T object at that address.
        ::new (static_cast<void*>(data_ + size_)) T(std::forward<Args>(args)...);
        ++size_;
        return data_[size_ - 1U];
    }

    // ------------------------------------------------------------------
    // pop_back
    // ------------------------------------------------------------------

    void pop_back() noexcept {
        ATX_ASSERT(size_ > 0U);
        --size_;
        data_[size_].~T();
    }

    // ------------------------------------------------------------------
    // operator[] — unchecked access (assert in debug).
    // ------------------------------------------------------------------

    [[nodiscard]] T& operator[](usize i) noexcept {
        ATX_ASSERT(i < size_);
        return data_[i];
    }

    [[nodiscard]] const T& operator[](usize i) const noexcept {
        ATX_ASSERT(i < size_);
        return data_[i];
    }

    // ------------------------------------------------------------------
    // at() — bounds-checked access, returns Result<T*>.
    // ------------------------------------------------------------------

    [[nodiscard]] atx::core::Result<T*> at(usize i) noexcept {
        if (i >= size_) {
            return atx::core::Err(atx::core::ErrorCode::OutOfRange);
        }
        return atx::core::Ok(data_ + i);
    }

    [[nodiscard]] atx::core::Result<const T*> at(usize i) const noexcept {
        if (i >= size_) {
            return atx::core::Err(atx::core::ErrorCode::OutOfRange);
        }
        return atx::core::Ok(static_cast<const T*>(data_ + i));
    }

    // ------------------------------------------------------------------
    // reserve — ensure capacity >= n, spilling/growing if needed.
    // ------------------------------------------------------------------

    void reserve(usize n) {
        if (n <= capacity_) {
            return;
        }
        reallocate(n);
    }

    // ------------------------------------------------------------------
    // clear — destroy all live elements; stay in current buffer mode.
    // ------------------------------------------------------------------

    void clear() noexcept {
        destroy_elements();
        size_ = 0U;
        // If we were spilled, free the heap buffer and return to inline.
        if (spilled()) {
            // SAFETY: data_ is the heap buffer obtained from aligned_alloc_bytes.
            //   aligned_free paired with that allocation.
            atx::core::aligned_free(static_cast<void*>(data_));
            data_     = inline_ptr();
            capacity_ = N;
        }
    }

    // ------------------------------------------------------------------
    // Observers
    // ------------------------------------------------------------------

    [[nodiscard]] T*       data()       noexcept { return data_; }
    [[nodiscard]] const T* data() const noexcept { return data_; }

    [[nodiscard]] T*       begin()       noexcept { return data_; }
    [[nodiscard]] const T* begin() const noexcept { return data_; }

    [[nodiscard]] T*       end()       noexcept { return data_ + size_; }
    [[nodiscard]] const T* end() const noexcept { return data_ + size_; }

    [[nodiscard]] usize size()     const noexcept { return size_; }
    [[nodiscard]] usize capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool  empty()    const noexcept { return size_ == 0U; }

    /// True when elements live on the heap (size has exceeded N at some point).
    [[nodiscard]] bool spilled() const noexcept {
        // SAFETY: comparing pointer to inline_slots_ address.  The pointer
        //   comparison is well-defined because both operands have the same
        //   static type (T*) and we are comparing against the known address of
        //   our own inline storage.
        return data_ != inline_ptr();
    }

private:
    // ------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------

    /// Typed pointer to the start of inline storage.
    ///
    /// SAFETY: inline_slots_[0].storage is alignas(T) and sizeof(T) bytes.
    ///   std::launder re-establishes pointer provenance after placement-new.
    [[nodiscard]] T* inline_ptr() noexcept {
        // SAFETY: The inline_slots_ array has proper alignment (alignas(T)) and
        //   size (N * sizeof(T) bytes total).  We treat slot 0 as the start of
        //   a T[N] object.  This is the standard small-buffer-optimisation
        //   technique; the pointer is only dereferenced after placement-new.
        return std::launder(reinterpret_cast<T*>(inline_slots_[0].storage.data()));
    }

    [[nodiscard]] const T* inline_ptr() const noexcept {
        // SAFETY: same as non-const overload.
        return std::launder(reinterpret_cast<const T*>(inline_slots_[0].storage.data()));
    }

    /// Destroy elements [0, size_) in reverse order.  Does NOT reset size_ or
    /// free the buffer — callers do that.
    void destroy_elements() noexcept {
        // Reverse destruction: LIFO-dependent destructors behave predictably.
        for (usize i = size_; i > 0U; --i) {
            data_[i - 1U].~T();
        }
    }

    /// Destroy all live elements and free heap buffer (if spilled).
    /// Leaves the object in an indeterminate state — only call from dtor or
    /// move-assignment after re-initialisation.
    void destroy_and_free() noexcept {
        destroy_elements();
        if (spilled()) {
            // SAFETY: data_ is the heap buffer from aligned_alloc_bytes.
            atx::core::aligned_free(static_cast<void*>(data_));
        }
    }

    /// Allocate a new heap buffer for new_cap elements, move all live elements
    /// in, then destroy the old elements and free the old heap buffer (if
    /// spilled).  Updates data_ and capacity_.
    ///
    /// @pre new_cap > capacity_
    void reallocate(usize new_cap) {
        ATX_ASSERT(new_cap > capacity_);

        // SAFETY: Allocate new_cap * sizeof(T) bytes aligned to alignof(T).
        //   aligned_alloc_bytes returns nullptr on failure.
        void* const raw = atx::core::aligned_alloc_bytes(
            new_cap * sizeof(T), alignof(T));
        ATX_CHECK(raw != nullptr);

        T* const new_buf = static_cast<T*>(raw);

        // Transfer every live element into the new buffer BEFORE destroying any
        // source element.  move_if_noexcept copies (not moves) when T's move
        // ctor may throw and T is copyable, so the source stays intact and we
        // get the strong guarantee.  If a transfer throws, roll back the
        // elements already built into new_buf, free it, and leave *this fully
        // unchanged — no double-destroy of the source.
        usize i = 0U;
        try {
            for (; i < size_; ++i) {
                // SAFETY: new_buf + i is freshly allocated, correctly aligned
                //   memory; placement-new begins the lifetime of T.
                ::new (static_cast<void*>(new_buf + i)) T(std::move_if_noexcept(data_[i]));
            }
        } catch (...) {
            for (usize j = i; j > 0U; --j) {
                new_buf[j - 1U].~T();
            }
            atx::core::aligned_free(raw);
            throw;
        }

        // All elements transferred — now destroy the old elements and free the
        // old heap buffer (if we were already spilled).
        for (usize j = 0U; j < size_; ++j) {
            data_[j].~T();
        }
        if (spilled()) {
            // SAFETY: data_ is the old heap buffer from aligned_alloc_bytes.
            atx::core::aligned_free(static_cast<void*>(data_));
        }

        data_     = new_buf;
        capacity_ = new_cap;
    }

    /// Grow by 2x if the vector is full.
    void grow_if_full() {
        if (size_ == capacity_) {
            reallocate(capacity_ * 2U);
        }
    }

    /// Deep-copy all elements from src.  Assumes *this is currently empty and
    /// in inline state.
    ///
    /// Exception safety (basic guarantee): size_ is advanced after each copy
    /// and, on throw, copy_from destroys what it built, frees any heap buffer
    /// it allocated, and resets *this to the empty/inline state — so neither
    /// this helper's own cleanup nor the caller's destructor leaks or
    /// double-frees.  (Both ctor callers delegate to SmallVector(), so their
    /// dtor also runs; the post-throw reset makes that a harmless no-op.)
    void copy_from(const SmallVector& src) {
        ATX_ASSERT(size_ == 0U);
        if (src.size_ > N) {
            // Allocate a heap buffer sized to src's capacity.
            void* const raw = atx::core::aligned_alloc_bytes(
                src.capacity_ * sizeof(T), alignof(T));
            ATX_CHECK(raw != nullptr);
            data_     = static_cast<T*>(raw);
            capacity_ = src.capacity_;
        }
        try {
            for (usize i = 0U; i < src.size_; ++i) {
                // SAFETY: data_ + i points into the correct buffer (inline or
                //   heap) with sufficient capacity.  Placement-new copies T.
                ::new (static_cast<void*>(data_ + i)) T(src.data_[i]);
                ++size_; // advance so cleanup destroys exactly the live elements
            }
        } catch (...) {
            destroy_and_free();      // destroy [0, size_) and free heap if spilled
            data_     = inline_ptr(); // return to a valid empty/inline state so a
            size_     = 0U;           // following destructor is a no-op
            capacity_ = N;
            throw;
        }
    }

    /// Steal elements from src (move operation).
    ///
    /// SAFETY: See class-level move constructor comment above.
    void steal_from(SmallVector&& src) noexcept(std::is_nothrow_move_constructible_v<T>) {
        ATX_ASSERT(size_ == 0U);
        if (src.spilled()) {
            // Fast path: steal the heap pointer.
            // SAFETY: src.data_ is a heap buffer we take ownership of.
            //   src is reset to inline/empty state so it will not double-free.
            data_     = src.data_;
            capacity_ = src.capacity_;
            size_     = src.size_;

            // Leave src in a valid, empty, inline state.
            src.data_     = src.inline_ptr();
            src.capacity_ = N;
            src.size_     = 0U;
        } else {
            // Slow path: src is inline, element-wise move.
            // We cannot take src's inline_slots_ address — it is part of src.
            // Transfer every element into our storage BEFORE destroying any
            // source element: if a transfer throws, roll back what we built and
            // leave src untouched (no double-destroy).  move_if_noexcept copies
            // when T's move may throw and T is copyable, keeping src intact.
            const usize n = src.size_;
            usize i = 0U;
            try {
                for (; i < n; ++i) {
                    // SAFETY: data_ + i is our own inline storage, correctly
                    //   aligned.  Placement-new constructs from the src element.
                    ::new (static_cast<void*>(data_ + i)) T(std::move_if_noexcept(src.data_[i]));
                }
            } catch (...) {
                for (usize j = i; j > 0U; --j) {
                    data_[j - 1U].~T();
                }
                throw;
            }
            // All elements transferred — destroy the moved-from src elements.
            for (usize j = 0U; j < n; ++j) {
                src.data_[j].~T();
            }
            size_     = n;
            src.size_ = 0U;
            // capacity_ already set to N (inline) in our ctor / reinit.
        }
    }

    /// Swap guts with other — used by copy-assignment (copy-and-swap).
    ///
    /// SAFETY: Cannot swap inline storage addresses directly; we must
    ///   rebuild via temporaries when at least one is inline.  This is
    ///   the same approach FixedVector::swap uses.
    void swap_with(SmallVector& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        SmallVector tmp{std::move(*this)};
        steal_from(std::move(other));
        other.steal_from(std::move(tmp));
    }

    // ------------------------------------------------------------------
    // Data members
    // ------------------------------------------------------------------

    /// Inline raw storage — N element slots, never directly accessed as T[].
    std::array<Slot, N> inline_slots_{};

    /// Points to the active element buffer: either inline_slots_[0] (cast via
    /// inline_ptr()) or a heap buffer owned by this object.
    T* data_;

    /// Number of live elements.
    usize size_;

    /// Current capacity (elements, not bytes).  Starts at N; grows on spill.
    usize capacity_;
};

} // namespace atx::core::container
