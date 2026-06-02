#include "atx/core/decimal.hpp"

// String parse/format and double conversion bodies for atx::core::Decimal.
//
// These are out-of-line (not in the header) because they allocate (std::string)
// and construct Err() values — neither is constexpr in C++20 — and to keep the
// header lean. Parsing validates ALL input at this boundary (agent profile §4);
// interior code then assumes a well-formed mantissa.

#include <array>   // std::array (to_string fractional buffer)
#include <cmath>   // std::isfinite, std::llround
#include <cstdint> // INT64_MAX
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::core {

// ============================================================
//  from_double
// ============================================================
Result<Decimal> Decimal::from_double(f64 value) noexcept {
  if (!std::isfinite(value)) {
    return Err(ErrorCode::InvalidArgument, "Decimal::from_double: non-finite");
  }
  // Scale to the nano grid, then round HALF-AWAY-FROM-ZERO. We bound the
  // scaled magnitude against INT64_MAX BEFORE converting back to integer so we
  // never invoke an out-of-range double->i64 cast (which is UB).
  const f64 scaled = value * static_cast<f64>(kScale);
  // Deliberately conservative upper limit: this is the largest power-of-two-
  // representable double strictly below (double)INT64_MAX, so the post-round
  // llround result provably fits in i64 without an out-of-range cast (UB). The
  // trade-off is that the top ~1024 nanos of the i64 range are under-accepted —
  // irrelevant for money, and worth it to stay UB-free at the boundary.
  constexpr f64 limit = 9.223372036854775e18; // < (double)INT64_MAX, conservative
  if (scaled > limit || scaled < -limit) {
    return Err(ErrorCode::InvalidArgument, "Decimal::from_double: out of range");
  }
  // std::llround rounds half away from zero and returns long long (>= 64-bit
  // on this target). The range guard above keeps the result within i64.
  const long long rounded = std::llround(scaled);
  return Ok(Decimal::from_raw(static_cast<i64>(rounded)));
}

// ============================================================
//  from_string
// ============================================================
namespace {

[[nodiscard]] constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

} // namespace

Result<Decimal> Decimal::from_string(std::string_view text) {
  if (text.empty()) {
    return Err(ErrorCode::ParseError, "Decimal::from_string: empty");
  }

  usize pos = 0;
  bool negative = false;
  if (text[pos] == '+' || text[pos] == '-') {
    negative = (text[pos] == '-');
    ++pos;
  }

  // Integer part: at least one digit required (reject "-", ".", ".5").
  const usize int_start = pos;
  i64 whole = 0;
  while (pos < text.size() && is_digit(text[pos])) {
    const int digit = text[pos] - '0';
    // Overflow-guarded accumulation against kMaxWhole (so whole*kScale fits).
    if (whole > (kMaxWhole - digit) / 10) {
      return Err(ErrorCode::OutOfRange, "Decimal::from_string: integer overflow");
    }
    whole = whole * 10 + digit;
    ++pos;
  }
  if (pos == int_start) {
    return Err(ErrorCode::ParseError, "Decimal::from_string: no integer digits");
  }

  // Fractional part: optional '.' then up to kFractionDigits digits.
  i64 fraction = 0; // scaled to kScale (i.e. nanos)
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    int frac_digits = 0;
    i64 frac_scale = kScale;
    while (pos < text.size() && is_digit(text[pos])) {
      if (frac_digits >= kFractionDigits) {
        return Err(ErrorCode::ParseError, "Decimal::from_string: too many fractional digits");
      }
      const int digit = text[pos] - '0';
      frac_scale /= 10; // place value of this digit in nanos
      fraction += static_cast<i64>(digit) * frac_scale;
      ++frac_digits;
      ++pos;
    }
    // "7." (dot with no following digits) is accepted as 7.0.
  }

  // Any unconsumed character is garbage (e.g. "1.2.3", "1 2", "1abc").
  if (pos != text.size()) {
    return Err(ErrorCode::ParseError, "Decimal::from_string: trailing garbage");
  }

  // Combine in u64 and bounds-check BEFORE the signed store. `whole <= kMaxWhole`
  // bounds whole*kScale, but adding up to kScale-1 nanos can still push the
  // mantissa past INT64_MAX (e.g. "9223372036.854775808" == INT64_MAX+1), which
  // would be signed-overflow UB. The fit bound is sign-dependent: a positive
  // mantissa must be <= INT64_MAX, while a negative one may reach |INT64_MIN| ==
  // INT64_MAX+1 (the exact min, e.g. "-9223372036.854775808", is representable).
  const u64 umagnitude = static_cast<u64>(whole) * static_cast<u64>(kScale) +
                         static_cast<u64>(fraction);
  const u64 limit = negative ? (static_cast<u64>(INT64_MAX) + 1ULL) // |INT64_MIN|
                             : static_cast<u64>(INT64_MAX);
  if (umagnitude > limit) {
    return Err(ErrorCode::OutOfRange, "Decimal::from_string: mantissa overflow");
  }
  // SAFETY: two's-complement negation of the magnitude; defined for all
  // magnitudes in [0, |INT64_MIN|] including the INT64_MIN boundary, and never
  // forms a positive value > INT64_MAX (rejected above).
  const i64 mantissa =
      negative ? static_cast<i64>(~umagnitude + 1ULL) : static_cast<i64>(umagnitude);
  return Ok(Decimal::from_raw(mantissa));
}

// ============================================================
//  to_string  (canonical form: trim trailing zeros, keep >= 1 frac digit)
// ============================================================
std::string Decimal::to_string() const {
  const i64 mantissa = raw();
  // Magnitude in nanos, sign handled separately to avoid INT64_MIN negation UB.
  const bool negative = mantissa < 0;
  // SAFETY: two's-complement magnitude; defined even for INT64_MIN.
  const u64 magnitude =
      negative ? (~static_cast<u64>(mantissa) + 1ULL) : static_cast<u64>(mantissa);

  const u64 scale = static_cast<u64>(kScale);
  const u64 whole = magnitude / scale;
  const u64 frac = magnitude % scale;

  std::string out;
  if (negative) {
    out.push_back('-');
  }
  out += std::to_string(whole);
  out.push_back('.');

  // Render the fractional part as exactly kFractionDigits, zero-padded (most
  // significant digit first), then trim trailing zeros but keep at least one.
  // std::array iterators keep the index off any unchecked raw subscript
  // (cppcoreguidelines-pro-bounds-constant-array-index).
  constexpr auto kDigits = static_cast<usize>(kFractionDigits);
  std::array<char, kDigits> buf{};
  // place starts at kScale/10 (the 0.1 nano place) and divides down per digit.
  u64 place = static_cast<u64>(kScale) / 10;
  for (char &digit : buf) {
    digit = static_cast<char>('0' + ((frac / place) % 10));
    place /= 10;
  }
  usize len = kDigits;
  while (len > 1 && buf.at(len - 1) == '0') {
    --len;
  }
  out.append(buf.data(), len);
  return out;
}

} // namespace atx::core
