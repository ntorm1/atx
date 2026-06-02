#pragma once

// atx::core::simd — vectorized span reductions and elementwise ops over xsimd.
//
// Width-agnostic numeric kernels over std::span of a floating-point element
// type. Each kernel processes the span in chunks of xsimd::batch<T>::size and
// finishes any remaining elements ("the tail") with a scalar loop, so the span
// length need NOT be a multiple of the SIMD width.
//
// Public API (namespace atx::core::simd), templated on T ∈ {f32, f64}:
//
//   Reductions (return a scalar):
//     sum(x)        — horizontal sum of all elements.
//     dot(a, b)     — Σ a[i]*b[i];   precondition a.size() == b.size().
//     mean(x)       — sum(x) / size; precondition size > 0.
//     min(x)        — smallest element; precondition size > 0.
//     max(x)        — largest element;  precondition size > 0.
//
//   Elementwise (in place / into out):
//     scale(k, x)   — x[i] *= k.
//     add(a, b, out)— out[i] = a[i] + b[i]; equal sizes.
//     axpy(k, x, y) — y[i] += k * x[i];     equal sizes.
//
// Accumulation order:
//   The vectorized reductions sum the SIMD lanes independently and combine them
//   with a final horizontal reduction. This produces a DIFFERENT floating-point
//   rounding than a naive left-to-right scalar fold; results agree to within a
//   few ULPs, not bit-for-bit. Callers comparing against a scalar reference
//   must use a tolerance (EXPECT_NEAR), not exact equality. This is inherent to
//   any SIMD reduction and is documented, not a defect.
//
// Alignment:
//   std::span carries no alignment guarantee, so every load/store uses the
//   UNALIGNED variants (xsimd::load_unaligned / store_unaligned). See the
//   SAFETY notes at each use site.
//
// Exceptions:
//   None of these functions throw. Preconditions are enforced with ATX_ASSERT,
//   which aborts (does not throw) on violation in debug builds; in release the
//   contract is the caller's responsibility. Hence every function is noexcept.

#include <span>
#include <type_traits>

#include <xsimd/xsimd.hpp>

#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

namespace atx::core::simd {

namespace detail {

// All kernels are constrained to floating-point element types. xsimd::batch is
// well-defined for f32/f64 on every supported ISA; integer batches have
// different reduction semantics that this module deliberately does not cover.
template <typename T>
inline constexpr bool is_simd_float_v = std::is_floating_point_v<T>;

} // namespace detail

// =====================================================================
//  sum — horizontal sum of all elements
// =====================================================================

/// Returns the sum of every element in x. Empty span returns T{0}.
///
/// @param x  Elements to sum (read-only).
/// @return   Σ x[i], lane-parallel accumulation (see file header on ordering).
template <typename T>
[[nodiscard]] T sum(std::span<const T> x) noexcept {
    static_assert(detail::is_simd_float_v<T>, "simd::sum requires a floating-point T");
    using batch                = xsimd::batch<T>;
    constexpr usize kWidth     = batch::size;
    const usize     n          = x.size();
    const usize     vec_end    = n - (n % kWidth); // largest multiple of kWidth <= n

    batch acc(T{0});
    for (usize i = 0U; i < vec_end; i += kWidth) {
        // SAFETY: load_unaligned — a std::span has no alignment guarantee, so we
        // must not assume the data pointer is batch-aligned. The unaligned load
        // is always defined; [i, i+kWidth) is in-bounds because i < vec_end.
        acc += batch::load_unaligned(x.data() + i);
    }
    T total = xsimd::reduce_add(acc);
    for (usize i = vec_end; i < n; ++i) { total += x[i]; } // scalar tail
    return total;
}

// =====================================================================
//  dot — inner product Σ a[i]*b[i]
// =====================================================================

/// Returns the dot product of a and b.
///
/// Precondition: a.size() == b.size() (ATX_ASSERT).
///
/// @param a  First operand.
/// @param b  Second operand (same length as a).
/// @return   Σ a[i]*b[i], lane-parallel accumulation (see file header).
template <typename T>
[[nodiscard]] T dot(std::span<const T> a, std::span<const T> b) noexcept {
    static_assert(detail::is_simd_float_v<T>, "simd::dot requires a floating-point T");
    ATX_ASSERT(a.size() == b.size());
    using batch            = xsimd::batch<T>;
    constexpr usize kWidth = batch::size;
    const usize     n      = a.size();
    const usize     vec_end = n - (n % kWidth);

    batch acc(T{0});
    for (usize i = 0U; i < vec_end; i += kWidth) {
        // SAFETY: load_unaligned for both operands — spans are not guaranteed
        // aligned. fma(x, y, z) = x*y + z; equivalent to acc += va*vb but lets
        // the hardware fuse the multiply-add when available.
        const batch va = batch::load_unaligned(a.data() + i);
        const batch vb = batch::load_unaligned(b.data() + i);
        acc = xsimd::fma(va, vb, acc);
    }
    T total = xsimd::reduce_add(acc);
    for (usize i = vec_end; i < n; ++i) { total += a[i] * b[i]; } // scalar tail
    return total;
}

// =====================================================================
//  mean — sum / size
// =====================================================================

/// Returns the arithmetic mean of x.
///
/// Precondition: x.size() > 0 (ATX_ASSERT) — the mean of an empty set is
/// undefined; we abort rather than divide by zero.
///
/// @param x  Elements to average.
/// @return   sum(x) / x.size().
template <typename T>
[[nodiscard]] T mean(std::span<const T> x) noexcept {
    static_assert(detail::is_simd_float_v<T>, "simd::mean requires a floating-point T");
    ATX_ASSERT(!x.empty());
    return sum(x) / static_cast<T>(x.size());
}

// =====================================================================
//  min / max
// =====================================================================

/// Returns the smallest element of x.
///
/// Precondition: x.size() > 0 (ATX_ASSERT).
///
/// @param x  Elements to scan.
/// @return   min over x[i].
template <typename T>
[[nodiscard]] T min(std::span<const T> x) noexcept {
    static_assert(detail::is_simd_float_v<T>, "simd::min requires a floating-point T");
    ATX_ASSERT(!x.empty());
    using batch            = xsimd::batch<T>;
    constexpr usize kWidth = batch::size;
    const usize     n      = x.size();

    if (n < kWidth) { // too short for even one batch — pure scalar scan
        T m = x[0];
        for (usize i = 1U; i < n; ++i) { m = x[i] < m ? x[i] : m; }
        return m;
    }
    const usize vec_end = n - (n % kWidth);
    // SAFETY: load_unaligned — span is unaligned; [0, kWidth) is in-bounds
    // because we took the n < kWidth fast path above.
    batch acc = batch::load_unaligned(x.data());
    for (usize i = kWidth; i < vec_end; i += kWidth) {
        acc = xsimd::min(acc, batch::load_unaligned(x.data() + i));
    }
    T m = xsimd::reduce_min(acc);
    for (usize i = vec_end; i < n; ++i) { m = x[i] < m ? x[i] : m; } // tail
    return m;
}

/// Returns the largest element of x.
///
/// Precondition: x.size() > 0 (ATX_ASSERT).
///
/// @param x  Elements to scan.
/// @return   max over x[i].
template <typename T>
[[nodiscard]] T max(std::span<const T> x) noexcept {
    static_assert(detail::is_simd_float_v<T>, "simd::max requires a floating-point T");
    ATX_ASSERT(!x.empty());
    using batch            = xsimd::batch<T>;
    constexpr usize kWidth = batch::size;
    const usize     n      = x.size();

    if (n < kWidth) {
        T m = x[0];
        for (usize i = 1U; i < n; ++i) { m = x[i] > m ? x[i] : m; }
        return m;
    }
    const usize vec_end = n - (n % kWidth);
    // SAFETY: load_unaligned — span is unaligned; first batch is in-bounds.
    batch acc = batch::load_unaligned(x.data());
    for (usize i = kWidth; i < vec_end; i += kWidth) {
        acc = xsimd::max(acc, batch::load_unaligned(x.data() + i));
    }
    T m = xsimd::reduce_max(acc);
    for (usize i = vec_end; i < n; ++i) { m = x[i] > m ? x[i] : m; } // tail
    return m;
}

// =====================================================================
//  scale — x *= k in place
// =====================================================================

/// Multiplies every element of x by k, in place.
///
/// @param k  Scalar multiplier.
/// @param x  Elements to scale (mutated).
template <typename T>
void scale(T k, std::span<T> x) noexcept {
    static_assert(detail::is_simd_float_v<T>, "simd::scale requires a floating-point T");
    using batch            = xsimd::batch<T>;
    constexpr usize kWidth = batch::size;
    const usize     n      = x.size();
    const usize     vec_end = n - (n % kWidth);

    const batch vk(k);
    for (usize i = 0U; i < vec_end; i += kWidth) {
        // SAFETY: load/store_unaligned — span is not guaranteed aligned; the
        // [i, i+kWidth) window is in-bounds for i < vec_end.
        const batch v = batch::load_unaligned(x.data() + i);
        (v * vk).store_unaligned(x.data() + i);
    }
    for (usize i = vec_end; i < n; ++i) { x[i] *= k; } // scalar tail
}

// =====================================================================
//  add — out = a + b
// =====================================================================

/// Computes out[i] = a[i] + b[i] for every element.
///
/// Precondition: a.size() == b.size() == out.size() (ATX_ASSERT).
///
/// @param a    First addend.
/// @param b    Second addend.
/// @param out  Destination (may alias a or b safely — read happens before store
///             per index window).
template <typename T>
void add(std::span<const T> a, std::span<const T> b, std::span<T> out) noexcept {
    static_assert(detail::is_simd_float_v<T>, "simd::add requires a floating-point T");
    ATX_ASSERT(a.size() == b.size());
    ATX_ASSERT(a.size() == out.size());
    using batch            = xsimd::batch<T>;
    constexpr usize kWidth = batch::size;
    const usize     n      = a.size();
    const usize     vec_end = n - (n % kWidth);

    for (usize i = 0U; i < vec_end; i += kWidth) {
        // SAFETY: load/store_unaligned — none of the three spans is guaranteed
        // aligned; every [i, i+kWidth) access is in-bounds for i < vec_end.
        const batch va = batch::load_unaligned(a.data() + i);
        const batch vb = batch::load_unaligned(b.data() + i);
        (va + vb).store_unaligned(out.data() + i);
    }
    for (usize i = vec_end; i < n; ++i) { out[i] = a[i] + b[i]; } // scalar tail
}

// =====================================================================
//  axpy — y += k * x   (BLAS level-1)
// =====================================================================

/// Computes y[i] += k * x[i] for every element, in place on y.
///
/// Precondition: x.size() == y.size() (ATX_ASSERT).
///
/// @param k  Scalar multiplier.
/// @param x  Source vector (read-only).
/// @param y  Accumulator vector (mutated).
template <typename T>
void axpy(T k, std::span<const T> x, std::span<T> y) noexcept {
    static_assert(detail::is_simd_float_v<T>, "simd::axpy requires a floating-point T");
    ATX_ASSERT(x.size() == y.size());
    using batch            = xsimd::batch<T>;
    constexpr usize kWidth = batch::size;
    const usize     n      = x.size();
    const usize     vec_end = n - (n % kWidth);

    const batch vk(k);
    for (usize i = 0U; i < vec_end; i += kWidth) {
        // SAFETY: load/store_unaligned — spans are not guaranteed aligned. fma
        // computes k*x + y in one step; window [i, i+kWidth) is in-bounds.
        const batch vx = batch::load_unaligned(x.data() + i);
        const batch vy = batch::load_unaligned(y.data() + i);
        xsimd::fma(vk, vx, vy).store_unaligned(y.data() + i);
    }
    for (usize i = vec_end; i < n; ++i) { y[i] += k * x[i]; } // scalar tail
}

} // namespace atx::core::simd
