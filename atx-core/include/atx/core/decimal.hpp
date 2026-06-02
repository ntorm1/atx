#pragma once

// atx::core Decimal — exact fixed-point money type, scale 10^-9 (nano).
//
// THE money type. Prices, quantities and notional sit on top of it, so every
// operation is exact (no binary-floating-point drift) and correctness-critical.
//
//   value = mantissa_ / kScale,  kScale = 1e9  (9 decimal places).
//
// The underlying store is a single i64 mantissa. With a 9-dp scale the
// representable integer range is roughly +-9.22e9 whole units with full
// nano precision — ample for prices and sizes; notional that needs more
// headroom should be composed from Decimals, not widen this type.
//
// ------------------------------------------------------------------
//  Design contract
// ------------------------------------------------------------------
//   from_raw(i64)      : exact, constexpr, never fails (any i64 mantissa).
//   from_int(i64)      : whole units; |whole| <= kMaxWhole or it ABORTS in
//                        debug (ATX_ASSERT). Out-of-range whole is a
//                        programmer error, not an expected runtime failure.
//   from_double(f64)   : Result; rounds HALF-AWAY-FROM-ZERO to the nano grid;
//                        Err(InvalidArgument) on NaN/inf/out-of-range.
//   from_string(sv)    : Result; parses [sign] int-digits [ '.' frac-digits ]
//                        with <=9 fractional digits. Garbage / empty / extra
//                        dot / >9 frac digits -> Err(ParseError); magnitude
//                        beyond kMaxWhole -> Err(OutOfRange).
//
//   to_string()        : canonical form — optional '-', integer part (>=1
//                        digit), '.', then the significant fractional digits
//                        with trailing zeros trimmed but AT LEAST ONE kept.
//                        Examples: 3 -> "3.0", 1.5 -> "1.5",
//                        -123.456789 -> "-123.456789", 0 -> "0.0".
//
//   operator+ / -      : exact on mantissas via checked_* ; on overflow they
//                        ABORT in debug (programmer error in normal ranges).
//                        checked_add / checked_sub return Result instead.
//   operator*          : (a.m * b.m) / kScale via a full 128-bit intermediate,
//                        TRUNCATED TOWARD ZERO. Result that exceeds i64 aborts.
//   operator/          : (a.m * kScale) / b.m via 128-bit intermediate,
//                        TRUNCATED TOWARD ZERO. divisor != 0 (ATX_ASSERT).
//   round()            : round HALF-AWAY-FROM-ZERO to the nearest whole unit
//                        (explicit, since * and / truncate).
//   operator<=> / ==   : defaulted — lexicographic on the single mantissa, so
//                        ordering matches numeric ordering exactly.
//
// References:
//   - SEI CERT INT32-C / INT30-C (no signed overflow / unsigned wrap).
//   - The 128-bit seam is documented at detail::mul_div_128 below.

#include <cmath>   // std::nan check helpers via <cmath> std::isfinite
#include <compare> // std::strong_ordering
#include <cstdint> // INT64_MAX
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/safe_math.hpp"
#include "atx/core/types.hpp"

namespace atx::core {

// ============================================================
//  128-bit portability seam
// ============================================================
//
// mul_div_128(a, b, divisor): computes a*b/divisor truncating toward zero,
// forming the FULL 128-bit product so that a*b can exceed i64 (it routinely
// does at the nano scale). Sets `overflow` if the truncated result does not
// fit in i64. `divisor` must be non-zero (caller guarantees / asserts).
//
// Toolchain note (clang-cl + MSVC runtime, the build target here):
//   * `__int128` MULTIPLY links fine, but `__int128` DIVIDE pulls in the
//     compiler-rt builtin `__divti3`, which is NOT linked under clang-cl ->
//     a link error. And `_div128` is NOT declared by clang-cl's <intrin.h>.
//   So we never perform a native 128-bit DIVISION. We use the wide type only
//   to form the product, then divide with a portable, branch-bounded 128/64
//   long-division routine in `udiv_128_by_64`. This compiles AND links on
//   clang-cl, clang, gcc and MSVC alike, with no runtime-builtin dependency.
namespace detail {

/// Unsigned 128-bit (hi:lo) divided by 64-bit divisor.
/// Returns the low 64 bits of the quotient; sets *rem to the remainder and
/// *overflow true iff the quotient does not fit in 64 bits.
///
/// Bounded loop (exactly 128 iterations) using only 64-bit ops — no UB, no
/// runtime builtins. Schoolbook restoring long division.
[[nodiscard]] constexpr u64 udiv_128_by_64(u64 hi, u64 lo, u64 divisor, u64 &rem,
                                           bool &overflow) noexcept {
  u64 quotient = 0;
  u64 remainder = 0;
  overflow = false;
  // 128 fixed iterations: bit 127 down to bit 0. Statically bounded (§3).
  for (int i = 127; i >= 0; --i) {
    const u64 bit = (i >= 64) ? ((hi >> (i - 64)) & 1ULL) : ((lo >> i) & 1ULL);
    remainder = (remainder << 1) | bit;
    if (remainder >= divisor) {
      remainder -= divisor;
      if (i >= 64) {
        // A set quotient bit at position >= 64 means the quotient overflows u64.
        overflow = true;
      }
      // SAFETY: i & 63 keeps the shift in [0,63]; only the low 64 quotient bits
      // are retained (high bits, if any, are flagged via `overflow`).
      quotient |= (1ULL << (i & 63));
    }
  }
  rem = remainder;
  return quotient;
}

/// Forms the full 128-bit unsigned product of two u64 values as hi:lo.
///
/// SAFETY: where `__int128` exists (clang/clang-cl/gcc) we use a widening
/// multiply ONLY (no 128-bit division), which lowers to a single inline
/// multiply / links via __multi3 — never the missing __divti3. The MSVC
/// `#else` branch uses `_umul128` (declared in <intrin.h>); it is correct by
/// construction but UNTESTED in this clang-cl toolchain.
constexpr void umul_64_to_128(u64 a, u64 b, u64 &hi, u64 &lo) noexcept {
#if defined(__SIZEOF_INT128__)
  // SAFETY: unsigned 128-bit multiply is well-defined; only the product is
  // formed in 128 bits — we never divide a __int128.
  const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
  lo = static_cast<u64>(product);
  hi = static_cast<u64>(product >> 64);
#else
  // SAFETY (MSVC, untested here): _umul128 returns the low 64 bits and writes
  // the high 64 bits through the out-parameter — exact 64x64->128 product.
  unsigned long long high = 0;
  lo = _umul128(a, b, &high);
  hi = high;
#endif
}

/// Computes a*b/divisor truncated toward zero with a full 128-bit product.
/// `divisor` must be non-zero. Sets `overflow` if the i64 result does not fit.
[[nodiscard]] constexpr i64 mul_div_128(i64 a, i64 b, i64 divisor, bool &overflow) noexcept {
  ATX_ASSERT(divisor != 0);
  overflow = false;
  if (a == 0 || b == 0) {
    return 0;
  }
  // Work on magnitudes; apply the result sign at the end. This sidesteps the
  // INT64_MIN / -1 hazard: we never negate a signed value, we form unsigned
  // magnitudes directly via two's-complement-safe abs.
  const bool negative = (a < 0) != (b < 0);
  // SAFETY: -(unsigned)x computes the two's-complement magnitude of a negative
  // signed value without signed-overflow UB (works even for INT64_MIN).
  const u64 ua =
      (a < 0) ? (~static_cast<u64>(a) + 1ULL) : static_cast<u64>(a);
  const u64 ub =
      (b < 0) ? (~static_cast<u64>(b) + 1ULL) : static_cast<u64>(b);
  const u64 ud =
      (divisor < 0) ? (~static_cast<u64>(divisor) + 1ULL) : static_cast<u64>(divisor);
  const bool divisor_negative = divisor < 0;

  u64 hi = 0;
  u64 lo = 0;
  umul_64_to_128(ua, ub, hi, lo);

  u64 remainder = 0;
  bool quotient_overflow = false;
  const u64 magnitude = udiv_128_by_64(hi, lo, ud, remainder, quotient_overflow);

  const bool result_negative = negative != divisor_negative;
  // Fit check: positive results must be <= INT64_MAX; negative results may
  // reach magnitude INT64_MAX+1 (== INT64_MIN).
  const u64 positive_limit = static_cast<u64>(INT64_MAX);
  const u64 negative_limit = static_cast<u64>(INT64_MAX) + 1ULL; // |INT64_MIN|
  if (quotient_overflow || magnitude > (result_negative ? negative_limit : positive_limit)) {
    overflow = true;
    return 0;
  }
  if (result_negative) {
    // SAFETY: two's-complement negation of the magnitude; defined for all
    // magnitudes in [0, |INT64_MIN|] including the INT64_MIN boundary.
    return static_cast<i64>(~magnitude + 1ULL);
  }
  return static_cast<i64>(magnitude);
}

} // namespace detail

// ============================================================
//  Decimal
// ============================================================
class Decimal {
public:
  /// Scale: 1 whole unit == kScale raw mantissa units (9 decimal places).
  static constexpr i64 kScale = 1'000'000'000LL;

  /// Largest whole-unit magnitude that fits in the mantissa without overflow.
  static constexpr i64 kMaxWhole = INT64_MAX / kScale; // 9'223'372'036

  /// Number of fractional decimal digits (== log10(kScale)).
  static constexpr int kFractionDigits = 9;

  // ---- special members: value type, Rule of Zero ----
  constexpr Decimal() noexcept = default;

  // ---- factories ----

  /// Exact construction from a raw mantissa (value = mantissa / kScale).
  /// Never fails; any i64 is a valid mantissa.
  [[nodiscard]] static constexpr Decimal from_raw(i64 mantissa) noexcept {
    return Decimal{mantissa};
  }

  /// Whole-unit construction. PRECONDITION: |whole| <= kMaxWhole.
  /// Violation ABORTS in debug (ATX_ASSERT); it is a programmer error, not an
  /// expected runtime failure (use from_string for untrusted input).
  [[nodiscard]] static constexpr Decimal from_int(i64 whole) noexcept {
    ATX_ASSERT(whole <= kMaxWhole && whole >= -kMaxWhole);
    return Decimal{whole * kScale};
  }

  /// Construct from a double, rounding HALF-AWAY-FROM-ZERO to the nano grid.
  /// Returns Err(InvalidArgument) on NaN, +-inf, or out-of-representable-range.
  /// Note: double has ~15-17 significant digits, so the input itself may not be
  /// exact; this rounds the (already approximate) double to the nearest nano.
  [[nodiscard]] static Result<Decimal> from_double(f64 value) noexcept;

  /// Parse "[+|-] int-digits [ '.' frac-digits ]" with <= kFractionDigits
  /// fractional digits. Err(ParseError) on malformed input; Err(OutOfRange)
  /// when the integer magnitude exceeds kMaxWhole.
  [[nodiscard]] static Result<Decimal> from_string(std::string_view text);

  // ---- accessors ----

  /// Raw mantissa (value * kScale).
  [[nodiscard]] constexpr i64 raw() const noexcept { return mantissa_; }

  /// Lossy conversion to double (for display / heuristics, never for money math).
  [[nodiscard]] constexpr f64 to_double() const noexcept {
    return static_cast<f64>(mantissa_) / static_cast<f64>(kScale);
  }

  /// Canonical decimal string (see header contract / to_string policy above).
  [[nodiscard]] std::string to_string() const;

  // ---- arithmetic ----

  /// Exact addition. PRECONDITION: result fits in i64 (ABORTS in debug on
  /// overflow). Use checked_add for untrusted/extreme operands.
  [[nodiscard]] constexpr Decimal operator+(const Decimal &other) const noexcept {
    return Decimal{add_or_abort(mantissa_, other.mantissa_)};
  }

  /// Exact subtraction. PRECONDITION: result fits in i64 (ABORTS in debug).
  [[nodiscard]] constexpr Decimal operator-(const Decimal &other) const noexcept {
    return Decimal{sub_or_abort(mantissa_, other.mantissa_)};
  }

  /// Unary negation. PRECONDITION: mantissa != INT64_MIN (ABORTS in debug).
  [[nodiscard]] constexpr Decimal operator-() const noexcept {
    ATX_ASSERT(mantissa_ != INT64_MIN);
    return Decimal{-mantissa_};
  }

  /// Checked addition: Ok(sum) or Err(OutOfRange) on mantissa overflow.
  [[nodiscard]] Result<Decimal> checked_add(const Decimal &other) const noexcept {
    ATX_TRY(auto sum, atx::core::checked_add<i64>(mantissa_, other.mantissa_));
    return Ok(Decimal{sum});
  }

  /// Checked subtraction: Ok(diff) or Err(OutOfRange) on mantissa overflow.
  [[nodiscard]] Result<Decimal> checked_sub(const Decimal &other) const noexcept {
    ATX_TRY(auto diff, atx::core::checked_sub<i64>(mantissa_, other.mantissa_));
    return Ok(Decimal{diff});
  }

  /// Multiplication with rescale, TRUNCATED TOWARD ZERO via 128-bit product.
  /// PRECONDITION: the rescaled result fits in i64 (ABORTS in debug otherwise).
  [[nodiscard]] constexpr Decimal operator*(const Decimal &other) const noexcept {
    bool overflow = false;
    const i64 result = detail::mul_div_128(mantissa_, other.mantissa_, kScale, overflow);
    ATX_ASSERT(!overflow);
    return Decimal{result};
  }

  /// Division with rescale, TRUNCATED TOWARD ZERO via 128-bit intermediate.
  /// PRECONDITIONS: divisor != 0 and the result fits in i64 (ABORT in debug).
  [[nodiscard]] constexpr Decimal operator/(const Decimal &other) const noexcept {
    ATX_ASSERT(other.mantissa_ != 0);
    bool overflow = false;
    const i64 result = detail::mul_div_128(mantissa_, kScale, other.mantissa_, overflow);
    ATX_ASSERT(!overflow);
    return Decimal{result};
  }

  /// Round HALF-AWAY-FROM-ZERO to the nearest whole unit.
  /// PRECONDITION: the rounded mantissa fits in i64 (ABORTS in debug).
  [[nodiscard]] constexpr Decimal round() const noexcept {
    constexpr i64 half = kScale / 2;
    // Add/subtract half toward the value's sign, then truncate to the grid.
    if (mantissa_ >= 0) {
      const i64 bumped = add_or_abort(mantissa_, half);
      return Decimal{(bumped / kScale) * kScale};
    }
    const i64 bumped = sub_or_abort(mantissa_, half);
    return Decimal{(bumped / kScale) * kScale};
  }

  // ---- comparison (defaulted; lexicographic on the single mantissa) ----
  [[nodiscard]] constexpr bool operator==(const Decimal &) const noexcept = default;
  [[nodiscard]] constexpr auto operator<=>(const Decimal &) const noexcept = default;

private:
  explicit constexpr Decimal(i64 mantissa) noexcept : mantissa_{mantissa} {}

  /// Exact i64 add that ABORTS on overflow (constexpr-friendly: uses the
  /// portable pre-check, never invokes signed-overflow UB).
  [[nodiscard]] static constexpr i64 add_or_abort(i64 a, i64 b) noexcept {
    ATX_ASSERT(!detail::portable_add_overflows<i64>(a, b));
    return a + b;
  }

  /// Exact i64 subtract that ABORTS on overflow (constexpr-friendly).
  [[nodiscard]] static constexpr i64 sub_or_abort(i64 a, i64 b) noexcept {
    ATX_ASSERT(!detail::portable_sub_overflows<i64>(a, b));
    return a - b;
  }

  i64 mantissa_{0};
};

} // namespace atx::core
