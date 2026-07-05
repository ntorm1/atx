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

#include <cmath>     // std::erfc, std::exp
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

// ============================================================
//  Standard normal distribution
// ============================================================
//
// Folded in from the C `ats-vol` library (ats_vol_internal.h), which needed
// a high-precision Φ / φ on the option-pricing hot path and had no home for
// them in the shared numeric layer. atx-engine likewise wanted a normal CDF
// over std::erfc; this is the single reusable home.
//
// Not constexpr: std::erfc / std::exp are not constexpr under C++20 (clang-cl).
// The extra-precision constants below beat the <math.h> literals in the last
// few ULPs, which matters to the IV inverter's tail behaviour.

/// 1 / sqrt(2π) — normalizing constant for the standard normal PDF.
template <std::floating_point T>
inline constexpr T inv_sqrt_2pi = T{0.398942280401432677939946059934381868};

/// 1 / sqrt(2) — argument scale for the erfc-based CDF.
template <std::floating_point T>
inline constexpr T inv_sqrt_2 = T{0.707106781186547524400844362104849039};

/// Standard normal probability density φ(x) = (1/√(2π))·exp(-x²/2).
///
/// @param x  Evaluation point.
/// @return   φ(x); underflows smoothly to 0 for |x| beyond ~38.
template <std::floating_point T>
[[nodiscard]] T norm_pdf(T x) noexcept {
    return inv_sqrt_2pi<T> * std::exp(T{-0.5} * x * x);
}

/// Standard normal cumulative distribution Φ(x) = ½·erfc(-x/√2).
///
/// erfc is the precision-friendly primitive: it avoids the catastrophic
/// cancellation a naive `0.5*(1+erf(...))` suffers in the left tail.
///
/// @param x  Evaluation point.
/// @return   Φ(x) ∈ [0, 1].
template <std::floating_point T>
[[nodiscard]] T norm_cdf(T x) noexcept {
    return T{0.5} * std::erfc(-x * inv_sqrt_2<T>);
}

} // namespace atx::core
