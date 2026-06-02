#include "atx/core/decimal.hpp"

// String parse/format and double conversion bodies for atx::core::Decimal.
//
// These are out-of-line (not in the header) because they allocate (std::string)
// and construct Err() values — neither is constexpr in C++20 — and to keep the
// header lean. Parsing validates ALL input at this boundary (agent profile §4);
// interior code then assumes a well-formed mantissa.

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
  // A small epsilon over INT64_MAX guards the boundary; doubles cannot
  // represent INT64_MAX exactly, so compare against its double value.
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

  // whole <= kMaxWhole was enforced above, so whole*kScale + fraction fits i64.
  const i64 magnitude = whole * kScale + fraction;
  return Ok(Decimal::from_raw(negative ? -magnitude : magnitude));
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

  // Render the fractional part as exactly kFractionDigits, zero-padded, then
  // trim trailing zeros but keep at least one digit.
  char buf[kFractionDigits];
  u64 f = frac;
  for (int i = kFractionDigits - 1; i >= 0; --i) {
    buf[i] = static_cast<char>('0' + (f % 10));
    f /= 10;
  }
  int last = kFractionDigits - 1;
  while (last > 0 && buf[last] == '0') {
    --last;
  }
  out.append(buf, static_cast<usize>(last + 1));
  return out;
}

} // namespace atx::core
