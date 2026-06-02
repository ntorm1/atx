#pragma once

// atx::core math — floating-point numeric helpers.
//
// Provides constexpr, noexcept, [[nodiscard]] utility functions over any
// std::floating_point type:
//
//   isclose(a, b, rel_tol, abs_tol)  — approximate equality with tolerances.
//   clamp(v, lo, hi)                 — value-returning clamp (unlike std::clamp
//                                       which returns a const-ref).
//   lerp(a, b, t)                    — linear interpolation / extrapolation.
//   sign(v)                          — returns -1, 0, or +1 as int.
//
// Implementation notes:
//   std::abs and std::max are constexpr in C++23 but not guaranteed under
//   clang-cl targeting C++20.  We supply tiny detail::fp_abs / detail::fp_max
//   helpers so every public function stays constexpr under C++20.
//
//   std::lerp (C++20) is NOT constexpr until C++23, so we provide our own
//   a + t*(b-a) formulation which is constexpr-safe.
//
//   std::clamp returns a const T& which can dangle when called with temporaries.
//   Our clamp returns T by value.
//
// References:
//   - Python math.isclose specification (PEP 485).
//   - IEEE 754-2008 §5.11 for comparison semantics.

#include <concepts>  // std::floating_point
#include <limits>    // std::numeric_limits

namespace atx::core {

// ============================================================
//  Internal helpers (not part of the public API)
// ============================================================

namespace detail {

/// Constexpr absolute value for floating-point types.
/// Equivalent to std::abs but guaranteed constexpr under C++20 / clang-cl.
template <std::floating_point T>
[[nodiscard]] constexpr T fp_abs(T v) noexcept {
    return v < T{0} ? -v : v;
}

/// Constexpr max for floating-point types.
/// Equivalent to std::max but guaranteed constexpr under C++20 / clang-cl.
template <std::floating_point T>
[[nodiscard]] constexpr T fp_max(T a, T b) noexcept {
    return a > b ? a : b;
}

} // namespace detail

// ============================================================
//  isclose
// ============================================================

/// Returns true if a and b are approximately equal.
///
/// The tolerance formula follows PEP 485:
///   |a - b| <= max(rel_tol * max(|a|, |b|), abs_tol)
///
/// Special cases:
///   - Exact equality (covers inf == inf) is handled first.
///   - If both are exactly zero the abs check trivially passes, but the
///     exact-equal fast path already returns true before the general formula.
///
/// @param a        First value.
/// @param b        Second value.
/// @param rel_tol  Relative tolerance (default 1e-9).
/// @param abs_tol  Absolute tolerance floor (default 1e-12).
/// @return true if a and b are within tolerance of each other.
///
/// Precondition: rel_tol >= 0, abs_tol >= 0.  Negative tolerances produce
/// mathematically defined but likely unintended results; no assertion is fired
/// since this is a constexpr leaf function (cannot call ATX_ASSERT which
/// drags in spdlog, a non-constexpr dependency).
template <std::floating_point T>
[[nodiscard]] constexpr bool isclose(
    T a, T b,
    T rel_tol = T{1e-9},
    T abs_tol = T{1e-12}) noexcept {
    // Fast path: bit-identical or inf == inf.
    if (a == b) { return true; }
    // If either value is infinite (and they are not equal, handled above),
    // the relative formula produces inf <= inf = true, which is wrong.
    // Two distinct infinities (or inf vs finite) are never "close".
    if (detail::fp_abs(a) == std::numeric_limits<T>::infinity() ||
        detail::fp_abs(b) == std::numeric_limits<T>::infinity()) {
        return false;
    }
    const T diff      = detail::fp_abs(a - b);
    const T tolerance = detail::fp_max(rel_tol * detail::fp_max(detail::fp_abs(a),
                                                                 detail::fp_abs(b)),
                                       abs_tol);
    return diff <= tolerance;
}

// ============================================================
//  clamp
// ============================================================

/// Returns v clamped to the closed interval [lo, hi], by value.
///
/// Unlike std::clamp (which returns const T&), this returns T to avoid
/// dangling-reference hazards when called with temporaries.
///
/// @param v   Value to clamp.
/// @param lo  Lower bound; must satisfy lo <= hi.
/// @param hi  Upper bound.
/// @return    lo if v < lo; hi if v > hi; otherwise v.
template <std::floating_point T>
[[nodiscard]] constexpr T clamp(T v, T lo, T hi) noexcept {
    // lo <= hi is a precondition.
    // SAFETY: We cannot call ATX_ASSERT here because macro.hpp includes spdlog
    // which is not constexpr-friendly.  The contract is documented; callers must
    // ensure lo <= hi.
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

// ============================================================
//  lerp
// ============================================================

/// Linear interpolation (and extrapolation) between a and b.
///
/// Computes a + t * (b - a).
/// t = 0 returns a; t = 1 returns b; values outside [0, 1] extrapolate.
///
/// Note: std::lerp (C++20) is not constexpr until C++23, so we provide
/// our own formulation which is constexpr under C++20.
///
/// @param a  Start value (returned when t == 0).
/// @param b  End value (returned when t == 1).
/// @param t  Interpolation parameter.
/// @return   a + t * (b - a).
template <std::floating_point T>
[[nodiscard]] constexpr T lerp(T a, T b, T t) noexcept {
    return a + t * (b - a);
}

// ============================================================
//  sign
// ============================================================

/// Returns the sign of v as an integer: -1, 0, or +1.
///
/// IEEE-754: negative zero (-0.0) compares equal to positive zero, so
/// sign(-0.0) == 0 (not -1).  This is intentional and correct.
///
/// @param v  Floating-point value.
/// @return  -1 if v < 0; 0 if v == 0 (including -0.0); +1 if v > 0.
template <std::floating_point T>
[[nodiscard]] constexpr int sign(T v) noexcept {
    if (v < T{0}) { return -1; }
    if (v > T{0}) { return  1; }
    return 0;
}

} // namespace atx::core
