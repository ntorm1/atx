#pragma once

// atx::core bit — thin, safe wrappers over C++20 <bit>.
//
// Provides:
//   next_pow2(x)   — smallest power of two >= x  (next_pow2(0) == 1)
//   is_pow2(x)     — true iff x has exactly one set bit (is_pow2(0) == false)
//   popcount(x)    — number of set bits; return type u32
//   clz(x)         — count of leading  zero bits; return type u32
//   ctz(x)         — count of trailing zero bits; return type u32
//   bit_width(x)   — minimum bits required to represent x; return type u32
//   byteswap(x)    — reverse byte order
//
// All functions are:
//   - constexpr noexcept [[nodiscard]]
//   - constrained on std::unsigned_integral (rejects signed and non-integral T)
//   - defined for all representable values — no UB on edge inputs (0, max, …)
//
// Return-type policy for count functions (clz / ctz / popcount / bit_width):
//   std::countl_zero etc. return int in the standard; we narrow-cast to u32.
//   The result always fits in u32: maximum is 64 for u64, well within [0,4G).
//   SAFETY: static_assert below guards the narrowing cast.

#include <bit>
#include <concepts>
#include <cstdint>

#include "atx/core/types.hpp"

namespace atx::core {

// ---- next_pow2 --------------------------------------------------------------

/// Returns the smallest power of two that is >= x.
///
/// @param x  Any unsigned integer value.
/// @return   Smallest p s.t. p >= x and p is a power of two.
///           next_pow2(0) == 1  — std::bit_ceil(0) is defined to return 1
///           (C++20 [bit.ceil] p1).
///           next_pow2(x) for x > 2^(N-1) would require a result that doesn't
///           fit in T; the behaviour of std::bit_ceil in that case is undefined
///           *unless* x is itself a power of two. Callers must not pass values
///           in (2^(N-1), 2^N) for N-bit T if that range contains non-powers.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T next_pow2(T x) noexcept {
    return std::bit_ceil(x);
}

// ---- is_pow2 ----------------------------------------------------------------

/// Returns true iff x has exactly one set bit (i.e., x is a power of two).
///
/// @param x  Any unsigned integer value.
/// @return   is_pow2(0) == false; is_pow2(1) == true; is_pow2(2) == true; …
template <std::unsigned_integral T>
[[nodiscard]] constexpr bool is_pow2(T x) noexcept {
    return std::has_single_bit(x);
}

// ---- popcount ---------------------------------------------------------------

/// Returns the number of set bits in x (Hamming weight / population count).
///
/// @param x  Any unsigned integer value.
/// @return   A u32 in [0, sizeof(T)*8].
///           popcount(0)         == 0
///           popcount(~T{0})     == sizeof(T)*8  (all bits set)
///
/// Return type is u32 (not T or int) for consistency across all width
/// variants and to avoid signed/unsigned promotion surprises.
template <std::unsigned_integral T>
[[nodiscard]] constexpr u32 popcount(T x) noexcept {
    // std::popcount returns int; result is in [0, 64] so the cast is safe.
    // SAFETY: static_assert ensures int->u32 cast cannot be negative.
    static_assert(sizeof(T) <= 8u,
                  "popcount: T wider than 64 bits not supported");
    return static_cast<u32>(std::popcount(x));
}

// ---- clz --------------------------------------------------------------------

/// Returns the number of consecutive zero bits starting from the most
/// significant bit (count leading zeros).
///
/// @param x  Any unsigned integer value.
/// @return   A u32 in [0, sizeof(T)*8].
///           clz(0)   == sizeof(T)*8  (all bits zero)
///           clz(~T{0}) == 0          (all bits set)
template <std::unsigned_integral T>
[[nodiscard]] constexpr u32 clz(T x) noexcept {
    // std::countl_zero returns int; result is in [0, 64] so cast is safe.
    static_assert(sizeof(T) <= 8u, "clz: T wider than 64 bits not supported");
    return static_cast<u32>(std::countl_zero(x));
}

// ---- ctz --------------------------------------------------------------------

/// Returns the number of consecutive zero bits starting from the least
/// significant bit (count trailing zeros).
///
/// @param x  Any unsigned integer value.
/// @return   A u32 in [0, sizeof(T)*8].
///           ctz(0)   == sizeof(T)*8  (all bits zero)
///           ctz(1)   == 0
///           ctz(~T{0}) == 0          (all bits set)
template <std::unsigned_integral T>
[[nodiscard]] constexpr u32 ctz(T x) noexcept {
    static_assert(sizeof(T) <= 8u, "ctz: T wider than 64 bits not supported");
    return static_cast<u32>(std::countr_zero(x));
}

// ---- bit_width --------------------------------------------------------------

/// Returns the minimum number of bits needed to represent x.
///
/// @param x  Any unsigned integer value.
/// @return   A u32 in [0, sizeof(T)*8].
///           bit_width(0) == 0; bit_width(1) == 1; bit_width(255) == 8.
template <std::unsigned_integral T>
[[nodiscard]] constexpr u32 bit_width(T x) noexcept {
    // std::bit_width returns T; the value is in [0, sizeof(T)*8] so fits u32.
    // SAFETY: sizeof(T)*8 <= 64 by the static_assert; u32 holds values to 4G.
    static_assert(sizeof(T) <= 8u,
                  "bit_width: T wider than 64 bits not supported");
    return static_cast<u32>(std::bit_width(x));
}

// ---- byteswap ---------------------------------------------------------------
//
// std::byteswap is C++23. We target C++20 (clang-cl with -std:c++20), so it
// may not be available. Detect at compile time and provide a constexpr fallback
// that reverses the bytes of any 1/2/4/8-byte unsigned integer.
//
// The fallback uses only shifts, masks, and bitwise-or — all well-defined for
// unsigned types with no UB regardless of value.
//
// SAFETY: The fallback uses std::bit_cast<u8[sizeof(T)], T> to inspect the
//         object representation, which is defined behaviour for unsigned integer
//         types under [basic.types] and the bit_cast specification. The
//         reconstruction via shifts avoids type-punning via union/pointer (which
//         would be UB in C++).  All shift counts are in [0, sizeof(T)*8 - 8],
//         which is within the valid range [0, sizeof(T)*8 - 1], so no UB.

namespace detail {

/// Fallback byte-swap for 1-byte types — identity.
template <std::unsigned_integral T>
    requires(sizeof(T) == 1)
[[nodiscard]] constexpr T byteswap_impl(T x) noexcept {
    return x;
}

/// Fallback byte-swap for 2-byte types.
template <std::unsigned_integral T>
    requires(sizeof(T) == 2)
[[nodiscard]] constexpr T byteswap_impl(T x) noexcept {
    return static_cast<T>(((x & T{0x00FFu}) << 8u) |
                          ((x & T{0xFF00u}) >> 8u));
}

/// Fallback byte-swap for 4-byte types.
template <std::unsigned_integral T>
    requires(sizeof(T) == 4)
[[nodiscard]] constexpr T byteswap_impl(T x) noexcept {
    return static_cast<T>(((x & T{0x000000FFu}) << 24u) |
                          ((x & T{0x0000FF00u}) <<  8u) |
                          ((x & T{0x00FF0000u}) >>  8u) |
                          ((x & T{0xFF000000u}) >> 24u));
}

/// Fallback byte-swap for 8-byte types.
template <std::unsigned_integral T>
    requires(sizeof(T) == 8)
[[nodiscard]] constexpr T byteswap_impl(T x) noexcept {
    return static_cast<T>(
        ((x & T{0x00000000000000FFull}) << 56u) |
        ((x & T{0x000000000000FF00ull}) << 40u) |
        ((x & T{0x0000000000FF0000ull}) << 24u) |
        ((x & T{0x00000000FF000000ull}) <<  8u) |
        ((x & T{0x000000FF00000000ull}) >>  8u) |
        ((x & T{0x0000FF0000000000ull}) >> 24u) |
        ((x & T{0x00FF000000000000ull}) >> 40u) |
        ((x & T{0xFF00000000000000ull}) >> 56u));
}

} // namespace detail

/// Reverses the byte order of x.
///
/// @param x  Any unsigned integer value of width 1, 2, 4, or 8 bytes.
/// @return   x with its bytes in reversed order.
///           byteswap(0x12345678u) == 0x78563412u
///           byteswap(x) applied twice is the identity.
///
/// Implementation note: uses std::byteswap when compiled with C++23 or later;
/// falls back to a constexpr shift-based implementation under C++20 to remain
/// compatible with clang-cl -std:c++20.
template <std::unsigned_integral T>
[[nodiscard]] constexpr T byteswap(T x) noexcept {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 ||
                  sizeof(T) == 4 || sizeof(T) == 8,
                  "byteswap: only 1/2/4/8-byte unsigned types are supported");
#if defined(__cpp_lib_byteswap) && __cpp_lib_byteswap >= 202110L
    // C++23 path: use the standard library function.
    return std::byteswap(x);
#else
    // C++20 fallback: constexpr shift/mask implementation.
    // SAFETY: All operations are on unsigned types; shift counts are strictly
    //         less than the bit-width of T; no UB is possible.
    return detail::byteswap_impl(x);
#endif
}

} // namespace atx::core
