#pragma once

// atx::core safe_math — overflow-safe integer arithmetic.
//
// Provides two families of operations over any std::integral type T:
//
//   checked_add / checked_sub / checked_mul
//     Return Result<T>: Ok(result) on success, Err(OutOfRange) on overflow.
//     These are [[nodiscard]] noexcept (NOT constexpr): constructing Err
//     builds a std::string, which is not constexpr in C++20.
//
//   sat_add / sat_sub / sat_mul
//     Return T clamped to [min, max]: never overflow, never trap.
//     These ARE constexpr noexcept — no heap allocation.
//
// Implementation strategy (in priority order):
//   1. Compiler builtins  (__builtin_*_overflow on GCC/Clang) — single
//      instruction on modern targets, handles both signed and unsigned.
//   2. Portable pre-check using std::numeric_limits — provably UB-free:
//      the overflowing operation is NEVER performed to detect overflow.
//
// No UB: signed overflow is never invoked; unsigned arithmetic is used only
// when its wrap-around behaviour would itself be the overflow signal.
//
// References:
//   - SEI CERT INT32-C: ensure operations do not result in signed overflow
//   - SEI CERT INT30-C: ensure unsigned integer ops do not wrap
//   - GCC Manual §6.57: integer overflow builtins

#include <concepts>
#include <limits>
#include <string>
#include <type_traits>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::core {

// ============================================================
//  Internal helpers
// ============================================================

namespace detail {

/// Tag: signed integral type.
template <std::integral T>
inline constexpr bool is_signed_v = std::is_signed_v<T>;

/// Compile-time min/max for T.
template <std::integral T>
inline constexpr T int_min = std::numeric_limits<T>::min();
template <std::integral T>
inline constexpr T int_max = std::numeric_limits<T>::max();

// ------------------------------------------------------------------
//  Portable (no-builtin) checked arithmetic — UB-free pre-checks.
//  The overflowing operation is NEVER performed to detect overflow.
// ------------------------------------------------------------------

/// Portable checked addition — signed path.
/// Pre-check: overflow iff (b > 0 && a > max - b) || (b < 0 && a < min - b).
template <std::integral T>
    requires std::is_signed_v<T>
[[nodiscard]] constexpr bool portable_add_overflows(T a, T b) noexcept {
    if (b > T{0} && a > int_max<T> - b) { return true; }
    if (b < T{0} && a < int_min<T> - b) { return true; }
    return false;
}

/// Portable checked addition — unsigned path.
/// Overflow iff a > max - b  (i.e. a + b wraps past max).
template <std::integral T>
    requires std::is_unsigned_v<T>
[[nodiscard]] constexpr bool portable_add_overflows(T a, T b) noexcept {
    return a > int_max<T> - b;
}

/// Portable checked subtraction — signed path.
/// Overflow iff (b < 0 && a > max + b) || (b > 0 && a < min + b).
template <std::integral T>
    requires std::is_signed_v<T>
[[nodiscard]] constexpr bool portable_sub_overflows(T a, T b) noexcept {
    if (b < T{0} && a > int_max<T> + b) { return true; }
    if (b > T{0} && a < int_min<T> + b) { return true; }
    return false;
}

/// Portable checked subtraction — unsigned path.
/// Underflow iff a < b (result would wrap below 0).
template <std::integral T>
    requires std::is_unsigned_v<T>
[[nodiscard]] constexpr bool portable_sub_overflows(T a, T b) noexcept {
    return a < b;
}

/// Portable checked multiplication — signed path.
/// Handles the full INT_MIN * -1 case explicitly; then checks via division.
template <std::integral T>
    requires std::is_signed_v<T>
[[nodiscard]] constexpr bool portable_mul_overflows(T a, T b) noexcept {
    // Zero never overflows.
    if (a == T{0} || b == T{0}) { return false; }
    // INT_MIN * -1 is INT_MIN (unsigned wrap) — not representable as signed.
    if (a == int_min<T> && b == T{-1}) { return true; }
    if (b == int_min<T> && a == T{-1}) { return true; }
    // Division check: overflow iff a * b would exceed the range.
    // Since we excluded zero above, division is safe.
    if (a > T{0} && b > T{0} && a > int_max<T> / b) { return true; }
    if (a < T{0} && b < T{0} && a < int_max<T> / b) { return true; }
    if (a > T{0} && b < T{0} && b < int_min<T> / a) { return true; }
    if (a < T{0} && b > T{0} && a < int_min<T> / b) { return true; }
    return false;
}

/// Portable checked multiplication — unsigned path.
/// Overflow iff a > max / b (when b != 0).
template <std::integral T>
    requires std::is_unsigned_v<T>
[[nodiscard]] constexpr bool portable_mul_overflows(T a, T b) noexcept {
    if (b == T{0}) { return false; }
    return a > int_max<T> / b;
}

} // namespace detail

// ============================================================
//  checked_add
// ============================================================

/// Adds a + b and returns the result, or Err(OutOfRange) if the addition
/// would overflow (signed) or wrap (unsigned).
///
/// @param a  First operand.
/// @param b  Second operand.
/// @return   Ok(a + b) on success; Err(OutOfRange) on overflow.
///
/// Not constexpr: Err() constructs a std::string (not constexpr in C++20).
template <std::integral T>
[[nodiscard]] Result<T> checked_add(T a, T b) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    // SAFETY: __builtin_add_overflow is well-defined for all integral types
    // on GCC and Clang (both signed and unsigned). It writes the wrapped result
    // through the pointer and returns true iff overflow occurred, with no UB.
    T result{};
    if (__builtin_add_overflow(a, b, &result)) {
        return Err(ErrorCode::OutOfRange, "checked_add: integer overflow");
    }
    return Ok(result);
#else
    if (detail::portable_add_overflows(a, b)) {
        return Err(ErrorCode::OutOfRange, "checked_add: integer overflow");
    }
    return Ok(static_cast<T>(a + b));
#endif
}

// ============================================================
//  checked_sub
// ============================================================

/// Subtracts b from a and returns the result, or Err(OutOfRange) on overflow.
///
/// @param a  Minuend.
/// @param b  Subtrahend.
/// @return   Ok(a - b) on success; Err(OutOfRange) on overflow/underflow.
template <std::integral T>
[[nodiscard]] Result<T> checked_sub(T a, T b) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    // SAFETY: same contract as __builtin_add_overflow above.
    T result{};
    if (__builtin_sub_overflow(a, b, &result)) {
        return Err(ErrorCode::OutOfRange, "checked_sub: integer overflow");
    }
    return Ok(result);
#else
    if (detail::portable_sub_overflows(a, b)) {
        return Err(ErrorCode::OutOfRange, "checked_sub: integer overflow");
    }
    return Ok(static_cast<T>(a - b));
#endif
}

// ============================================================
//  checked_mul
// ============================================================

/// Multiplies a * b and returns the result, or Err(OutOfRange) on overflow.
///
/// @param a  First factor.
/// @param b  Second factor.
/// @return   Ok(a * b) on success; Err(OutOfRange) on overflow.
template <std::integral T>
[[nodiscard]] Result<T> checked_mul(T a, T b) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    // SAFETY: same contract as __builtin_add_overflow above.
    T result{};
    if (__builtin_mul_overflow(a, b, &result)) {
        return Err(ErrorCode::OutOfRange, "checked_mul: integer overflow");
    }
    return Ok(result);
#else
    if (detail::portable_mul_overflows(a, b)) {
        return Err(ErrorCode::OutOfRange, "checked_mul: integer overflow");
    }
    return Ok(static_cast<T>(a * b));
#endif
}

// ============================================================
//  sat_add
// ============================================================

/// Saturating addition: returns a + b clamped to [min(T), max(T)].
///
/// @param a  First operand.
/// @param b  Second operand.
/// @return   a + b if no overflow; max(T) on positive overflow; min(T) on
///           negative overflow (unsigned: max(T) if a + b would wrap).
///
/// constexpr: no heap allocation; all paths are constant-expression-evaluable.
template <std::integral T>
[[nodiscard]] constexpr T sat_add(T a, T b) noexcept {
    // Use the portable pre-check so this can remain constexpr.
    // The builtin path cannot be used here because __builtin_add_overflow is
    // not constexpr (it's a compiler intrinsic, not a constexpr function).
    if (!detail::portable_add_overflows(a, b)) {
        return static_cast<T>(a + b);
    }
    // Saturate: for signed, direction depends on sign of b.
    // For unsigned, overflow means result > max, so saturate to max.
    if constexpr (std::is_signed_v<T>) {
        return (b > T{0}) ? detail::int_max<T> : detail::int_min<T>;
    } else {
        return detail::int_max<T>;
    }
}

// ============================================================
//  sat_sub
// ============================================================

/// Saturating subtraction: returns a - b clamped to [min(T), max(T)].
///
/// @param a  Minuend.
/// @param b  Subtrahend.
/// @return   a - b if no overflow; max(T) on positive overflow; min(T) on
///           negative overflow (unsigned: 0, i.e. min(T), if a < b).
template <std::integral T>
[[nodiscard]] constexpr T sat_sub(T a, T b) noexcept {
    if (!detail::portable_sub_overflows(a, b)) {
        return static_cast<T>(a - b);
    }
    if constexpr (std::is_signed_v<T>) {
        // b < 0 → subtracting a negative → a - b > max
        // b > 0 → subtracting a positive → a - b < min
        return (b < T{0}) ? detail::int_max<T> : detail::int_min<T>;
    } else {
        // Unsigned underflow always saturates to 0 (== min).
        return detail::int_min<T>; // which is 0 for unsigned
    }
}

// ============================================================
//  sat_mul
// ============================================================

/// Saturating multiplication: returns a * b clamped to [min(T), max(T)].
///
/// @param a  First factor.
/// @param b  Second factor.
/// @return   a * b if no overflow; saturated to max(T) or min(T) otherwise.
template <std::integral T>
[[nodiscard]] constexpr T sat_mul(T a, T b) noexcept {
    if (!detail::portable_mul_overflows(a, b)) {
        return static_cast<T>(a * b);
    }
    if constexpr (std::is_signed_v<T>) {
        // Overflow direction: same sign → positive overflow → max.
        //                     different signs → negative overflow → min.
        const bool same_sign = (a > T{0} && b > T{0}) || (a < T{0} && b < T{0});
        return same_sign ? detail::int_max<T> : detail::int_min<T>;
    } else {
        return detail::int_max<T>;
    }
}

} // namespace atx::core
