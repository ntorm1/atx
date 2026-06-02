#pragma once

// atx::core arena — non-owning monotonic bump allocator over a caller-supplied
// buffer.
//
// Design contract:
//   - The Arena does NOT own the backing buffer.  The caller must ensure the
//     buffer outlives the Arena instance.
//   - reset() rewinds the offset to zero but does NOT call any destructors.
//     Consequently, create<T>() is restricted to trivially-destructible T via a
//     static_assert.  This is an intentional safety gate: if you need a
//     destructor you need a different lifetime model, not a bump allocator.
//   - Thread-safety: NONE.  Synchronise externally if multiple threads share an
//     Arena.
//   - allocate() and create() return nullptr on out-of-space; they never throw.
//
// Usage example:
//   alignas(64) std::byte buf[4096];
//   atx::core::Arena arena{std::span<std::byte>(buf)};
//   int* p = arena.create<int>(42);

#include <cstddef>
#include <new>
#include <span>
#include <type_traits>

#include "atx/core/bit.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

namespace atx::core {

/// Non-owning monotonic bump allocator.
///
/// All allocations are bump-allocated from a caller-supplied byte buffer.
/// Freeing individual allocations is not supported; reset() reclaims the whole
/// arena at once.
///
/// @note The backing buffer MUST outlive every Arena instance that references
///       it.  The Arena takes a non-owning view (std::span) and does not free
///       or otherwise touch the buffer on destruction.
class Arena {
public:
    // ------------------------------------------------------------------
    // Construction
    // ------------------------------------------------------------------

    /// Construct from a span of bytes (preferred).
    ///
    /// @param buf  Caller-owned byte buffer. Must remain valid for the
    ///             lifetime of this Arena.
    explicit Arena(std::span<std::byte> buf) noexcept
        : data_{buf.data()}, capacity_{buf.size()}, offset_{0U} {}

    /// Construct from a raw pointer + size (convenience overload).
    ///
    /// @param data  Pointer to the first byte of the caller-owned buffer.
    ///              Must be non-null when size > 0.
    /// @param size  Number of usable bytes starting at data.
    Arena(std::byte* data, usize size) noexcept
        : data_{data}, capacity_{size}, offset_{0U} {}

    // Rule of Zero: this is a non-owning view; default copy/move are fine.
    // Copies share the same underlying buffer and independent offsets, which
    // is intentional (e.g. save/restore checkpoints).
    Arena(const Arena&) noexcept            = default;
    Arena& operator=(const Arena&) noexcept = default;
    Arena(Arena&&) noexcept                 = default;
    Arena& operator=(Arena&&) noexcept      = default;
    ~Arena()                                = default;

    // ------------------------------------------------------------------
    // Core allocation
    // ------------------------------------------------------------------

    /// Allocate size bytes at the given power-of-two alignment.
    ///
    /// @param size       Number of bytes to allocate.  A request for 0 bytes
    ///                   returns nullptr (no allocation is made).
    /// @param alignment  Alignment requirement; must be a power of two and > 0.
    ///
    /// @return  Pointer to the allocated region, or nullptr if the remaining
    ///          capacity is insufficient.
    ///
    /// @pre  is_pow2(alignment)
    ///
    /// SAFETY: Alignment math is performed entirely on integer offsets (not on
    ///   the raw pointer) to avoid pointer-overflow UB.  The aligned offset is
    ///   computed as `(cur + (align - 1)) & ~(align - 1)`, which is valid for
    ///   any unsigned integer value and any power-of-two alignment.  The final
    ///   pointer is reconstructed as `data_ + aligned_offset` after bounds
    ///   verification, so we never form an out-of-bounds pointer value.
    [[nodiscard]] void* allocate(usize size, usize alignment) noexcept {
        ATX_ASSERT(is_pow2(alignment));

        if (size == 0U) {
            return nullptr;
        }

        // Compute the aligned start offset using only integer arithmetic.
        // SAFETY: alignment is a power of two, so (alignment - 1) is a valid
        //   mask.  All arithmetic is on usize (unsigned), so wraparound is
        //   well-defined under C++ unsigned arithmetic rules (no UB).
        const usize aligned_offset = (offset_ + (alignment - 1U)) & ~(alignment - 1U);

        // Check that the aligned region fits without overflow.
        // SAFETY: Both aligned_offset and size are usize; their sum might
        //   theoretically overflow if the buffer is astronomically large, but
        //   usize can hold any valid object size, so capacity_ < SIZE_MAX.
        //   We check (capacity_ - aligned_offset) >= size to avoid overflow.
        if (aligned_offset > capacity_ || (capacity_ - aligned_offset) < size) {
            return nullptr;
        }

        offset_ = aligned_offset + size;

        // SAFETY: data_ points to the start of the caller's buffer.
        //   aligned_offset has been verified to be within [0, capacity_), and
        //   offset_ == aligned_offset + size <= capacity_, so the returned
        //   pointer is within the buffer's bounds.  Pointer arithmetic on
        //   std::byte* is well-defined (std::byte is a character type).
        return data_ + aligned_offset;
    }

    // ------------------------------------------------------------------
    // Typed construction
    // ------------------------------------------------------------------

    /// Allocate and construct a T in the arena.
    ///
    /// T must be trivially destructible because reset() rewinds the offset
    /// without calling destructors.  Enforced via static_assert to make misuse
    /// a compile error rather than a runtime surprise.
    ///
    /// @param args  Arguments forwarded to T's constructor.
    /// @return      Pointer to the constructed object, or nullptr if no space.
    ///
    /// SAFETY: Placement-new on a void* returned by allocate() is well-defined
    ///   provided the memory is sufficiently aligned for T (which allocate()
    ///   guarantees via alignof(T)) and has at least sizeof(T) bytes available.
    ///   The pointer is cast to T* after construction, which is the documented
    ///   correct use of placement-new (the object's lifetime has begun).
    template <class T, class... Args>
    [[nodiscard]] T* create(Args&&... args) {
        static_assert(
            std::is_trivially_destructible_v<T>,
            "Arena::create<T>: T must be trivially destructible. "
            "Arena::reset() rewinds the offset without calling destructors; "
            "if T has a non-trivial destructor its cleanup will be silently "
            "skipped.  Use a different allocator if T requires destruction.");

        void* const mem = allocate(sizeof(T), alignof(T));
        if (mem == nullptr) {
            return nullptr;
        }

        // SAFETY: mem is aligned to alignof(T) and has sizeof(T) bytes,
        //   both guaranteed by the allocate() call above.  Placement-new
        //   begins the lifetime of a T object at mem.  The resulting pointer
        //   is immediately returned to the caller who owns the object's
        //   lifetime until the arena is reset.
        return ::new (mem) T(std::forward<Args>(args)...);
    }

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    /// Rewind the offset to zero.
    ///
    /// After reset(), all previously allocated memory is considered
    /// reclaimed.  No destructors are called — see create<T> for the
    /// implication.  The backing buffer is not zeroed.
    void reset() noexcept {
        offset_ = 0U;
    }

    // ------------------------------------------------------------------
    // Observers
    // ------------------------------------------------------------------

    /// Returns the number of bytes consumed since construction or last reset().
    [[nodiscard]] usize used() const noexcept {
        return offset_;
    }

    /// Returns the total capacity of the backing buffer in bytes.
    [[nodiscard]] usize capacity() const noexcept {
        return capacity_;
    }

    /// Returns the number of bytes that could still be bump-allocated
    /// (ignoring alignment padding that a future allocation might need).
    [[nodiscard]] usize remaining() const noexcept {
        return capacity_ - offset_;
    }

private:
    std::byte* data_;     ///< Pointer to the start of the backing buffer (non-owning).
    usize      capacity_; ///< Total size of the backing buffer in bytes.
    usize      offset_;   ///< Current bump pointer as a byte offset from data_.
};

} // namespace atx::core
