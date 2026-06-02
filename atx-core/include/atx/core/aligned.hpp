#pragma once

// atx::core aligned — cache-line-aligned allocation and fixed inline buffer.
//
// Provides:
//   aligned_alloc_bytes(size, alignment) — portable aligned heap allocation
//   aligned_free(p)                      — paired deallocation
//   AlignedBuffer<T, N>                  — cache-line-aligned inline fixed buffer
//
// Allocation contract:
//   - alignment MUST be a power of two (asserted in debug).
//   - aligned_alloc_bytes / aligned_free MUST be paired; never mix with plain
//     malloc/free or operator new/delete.
//   - Returns nullptr on allocation failure; callers must check.
//
// Platform note: on this toolchain (clang-cl, _MSC_VER defined) the MSVC path
// (_aligned_malloc / _aligned_free) is active. The POSIX branch is provided
// correct-by-construction but is untested on this build.

#include <array>
#include <cstddef>
#include <span>

#include "atx/core/bit.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/platform.hpp"
#include "atx/core/types.hpp"

#if defined(_MSC_VER)
#  include <malloc.h>  // _aligned_malloc, _aligned_free
#else
#  include <cstdlib>   // std::aligned_alloc, std::free
#endif

namespace atx::core {

// =====================================================================
//  aligned_alloc_bytes
//
//  Allocates `size` bytes with at least `alignment`-byte alignment.
//  `alignment` must be a power of two.
//
//  @param size       Number of bytes to allocate. May be 0 (returns nullptr
//                    or an implementation-defined non-null pointer — treat
//                    as nullptr for portability).
//  @param alignment  Required alignment in bytes. Must be a power of two.
//  @return           Aligned pointer, or nullptr on failure or size == 0.
//
//  SAFETY: Delegates to _aligned_malloc (MSVC) or std::aligned_alloc (POSIX).
//          Both are platform intrinsics that guarantee the returned pointer
//          meets the requested alignment; we verify the precondition with
//          ATX_ASSERT rather than encoding it in the type system because the
//          alignment value is often runtime-determined (e.g., SIMD width).
//          The caller MUST free the result with aligned_free — mixing with
//          plain free / operator delete is undefined behaviour.
// =====================================================================
[[nodiscard]] inline void* aligned_alloc_bytes(usize size,
                                               usize alignment) noexcept {
    ATX_ASSERT(is_pow2(alignment));

    if (size == 0U) {
        return nullptr;
    }

#if defined(_MSC_VER)
    // SAFETY: _aligned_malloc(size, alignment) is the MSVC-documented API for
    //         aligned allocation. It returns nullptr on failure. Result must be
    //         freed with _aligned_free — not free() or delete.
    return _aligned_malloc(size, alignment);
#else
    // POSIX: std::aligned_alloc requires size to be a multiple of alignment.
    // Round up to the nearest multiple to satisfy this precondition without
    // restricting callers.
    //
    // SAFETY: std::aligned_alloc(alignment, rounded_size) is C11/C++17 standard.
    //         Result must be freed with std::free — not _aligned_free.
    const usize remainder = size % alignment;
    const usize rounded   = (remainder == 0U) ? size : (size + alignment - remainder);
    return std::aligned_alloc(alignment, rounded);
#endif
}

// =====================================================================
//  aligned_free
//
//  Releases memory previously returned by aligned_alloc_bytes.
//  Passing nullptr is safe (no-op on both MSVC and POSIX).
//
//  @param p  Pointer returned by aligned_alloc_bytes, or nullptr.
//
//  SAFETY: Must pair with aligned_alloc_bytes. Do NOT pass a pointer
//          obtained from plain malloc/new — that is undefined behaviour
//          on MSVC (_aligned_free internal heap metadata mismatch) and
//          implementation-defined on POSIX.
// =====================================================================
inline void aligned_free(void* p) noexcept {
#if defined(_MSC_VER)
    // SAFETY: _aligned_free(nullptr) is documented as a safe no-op (MSVC CRT).
    _aligned_free(p);
#else
    // SAFETY: std::free(nullptr) is defined by the C standard to be a no-op.
    std::free(p); // NOLINT(cppcoreguidelines-no-malloc)
#endif
}

// =====================================================================
//  AlignedBuffer<T, N>
//
//  A fixed-capacity, cache-line-aligned inline buffer of N elements of
//  type T. No heap allocation — storage is a member. Intended for
//  SIMD/stats scratch buffers where cache-line alignment is required and
//  the size is known at compile time.
//
//  Alignment: alignas(kCacheLineSize) guarantees the buffer starts on a
//  cache-line boundary. This prevents false sharing between adjacent
//  objects and enables aligned SIMD loads (AVX requires 32-byte alignment,
//  which is a divisor of 64).
//
//  Element initialisation: AlignedBuffer value-initialises its storage
//  via the std::array member default constructor, which zero-initialises
//  trivial types and default-constructs non-trivial ones. If T is not
//  default-constructible the class will not compile — an intentional
//  constraint (illegal states unrepresentable).
//
//  Ownership: Rule of Zero — the implicitly-defined copy/move/destructor
//  are all correct; no special members are defined.
//
//  @tparam T  Element type. Must be default-constructible.
//  @tparam N  Number of elements. Must be > 0 (enforced by static_assert).
// =====================================================================
template <class T, usize N>
class AlignedBuffer {
    static_assert(N > 0U, "AlignedBuffer: N must be > 0");

public:
    // ---- Type aliases (mirrors std::array / std::span conventions) ----
    using value_type      = T;
    using size_type       = usize;
    using reference       = T&;
    using const_reference = const T&;
    using pointer         = T*;
    using const_pointer   = const T*;
    using iterator        = T*;
    using const_iterator  = const T*;

    // ---- Capacity ----

    /// Returns the number of elements (always N).
    [[nodiscard]] static constexpr size_type size() noexcept { return N; }

    // ---- Element access ----

    /// Returns a pointer to the first element.
    ///
    /// SAFETY: The pointer is valid for the lifetime of this AlignedBuffer.
    ///         Alignment is guaranteed to be at least kCacheLineSize bytes.
    [[nodiscard]] pointer data() noexcept {
        return storage_.data();
    }

    [[nodiscard]] const_pointer data() const noexcept {
        return storage_.data();
    }

    /// Indexed element access. index must be < N (unchecked).
    [[nodiscard]] reference operator[](size_type index) noexcept {
        ATX_ASSERT(index < N);
        return storage_[index];
    }

    [[nodiscard]] const_reference operator[](size_type index) const noexcept {
        ATX_ASSERT(index < N);
        return storage_[index];
    }

    // ---- Iterators ----

    [[nodiscard]] iterator begin() noexcept { return storage_.data(); }
    [[nodiscard]] iterator end()   noexcept { return storage_.data() + N; }

    [[nodiscard]] const_iterator begin() const noexcept { return storage_.data(); }
    [[nodiscard]] const_iterator end()   const noexcept { return storage_.data() + N; }

    [[nodiscard]] const_iterator cbegin() const noexcept { return storage_.data(); }
    [[nodiscard]] const_iterator cend()   const noexcept { return storage_.data() + N; }

    // ---- Span views ----

    /// Returns a non-owning span over all elements.
    ///
    /// SAFETY: The span must not outlive this AlignedBuffer.
    [[nodiscard]] std::span<T> span() noexcept {
        return std::span<T>{storage_.data(), N};
    }

    [[nodiscard]] std::span<const T> span() const noexcept {
        return std::span<const T>{storage_.data(), N};
    }

private:
    // alignas(kCacheLineSize) ensures the array starts on a cache-line
    // boundary. The attribute applies to the storage_ member itself, so
    // even when AlignedBuffer is stack-allocated the alignment is respected
    // (compilers pad the enclosing stack frame accordingly).
    //
    // SAFETY: std::array<T, N> is a standard layout type that holds elements
    //         contiguously. Applying alignas here is well-formed per [dcl.align].
    alignas(kCacheLineSize) std::array<T, N> storage_{};
};

} // namespace atx::core
