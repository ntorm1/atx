// Out-of-line bodies for atx/core/datetime.hpp: ISO-8601 formatting/parsing and
// the NYSE trading calendar. The pure value types (Duration, Timestamp, Date,
// civil math) are header-inline constexpr; only the formatting and calendar-rule
// logic live here, per the atx-core layout convention.

#include "atx/core/datetime.hpp"

#include <array>
#include <string>
#include <string_view>

#include "atx/core/error.hpp" // Result, Ok, Err, ErrorCode
#include "atx/core/macro.hpp" // ATX_CHECK, ATX_ASSERT
#include "atx/core/types.hpp" // i32, i64, u32, usize

namespace atx::core::time {

namespace {

// ---- ISO-8601 parsing primitives -------------------------------------------
constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// Append `value` as exactly `width` zero-padded decimal digits to `out`. `width`
// is bounded by the caller (≤ 9, the nanosecond field), so the scratch array
// cannot overflow. std::array keeps the index off any unchecked raw subscript.
void append_padded(std::string &out, u32 value, usize width) {
  std::array<char, 9> tmp{};
  for (usize i = width; i-- > 0;) {
    tmp.at(i) = static_cast<char>('0' + value % 10U);
    value /= 10U;
  }
  out.append(tmp.data(), width);
}

// Consume exactly `count` decimal digits at s[pos] into `out`; advance pos. The
// caller bounds `count` (≤ 9) so the u32 accumulator cannot overflow.
bool take_uint(std::string_view s, usize &pos, usize count, u32 &out) noexcept {
  if (pos + count > s.size()) {
    return false;
  }
  u32 v = 0;
  for (usize i = 0; i < count; ++i) {
    const char c = s[pos + i];
    if (!is_digit(c)) {
      return false;
    }
    v = v * 10U + static_cast<u32>(c - '0');
  }
  pos += count;
  out = v;
  return true;
}

bool take_char(std::string_view s, usize &pos, char expect) noexcept {
  if (pos >= s.size() || s[pos] != expect) {
    return false;
  }
  ++pos;
  return true;
}

// ---- NYSE holiday observance ------------------------------------------------
// Shift a fixed-date holiday off a weekend. Sunday → Monday always. Saturday →
// Friday only when `observe_friday` (true for all NYSE holidays except New Year's,
// which the NYSE does not pull back to December 31).
Date observed_holiday(Date h, bool observe_friday) noexcept {
  const Weekday wd = weekday(h);
  if (wd == Weekday::Saturday) {
    return observe_friday ? Date::from_days(h.to_days() - 1) : h;
  }
  if (wd == Weekday::Sunday) {
    return Date::from_days(h.to_days() + 1);
  }
  return h;
}

} // namespace

// ---- ISO-8601 ---------------------------------------------------------------
std::string to_iso8601(Timestamp ts) {
  const CivilTime ct = to_civil_utc(ts);
  // Timestamp range (i64 ns from epoch) spans years 1678–2262, so the year is
  // always a non-negative four-digit value: "YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ".
  ATX_CHECK(ct.date.year >= 0 && ct.date.year <= 9999);
  std::string out;
  out.reserve(30);
  append_padded(out, static_cast<u32>(ct.date.year), 4);
  out.push_back('-');
  append_padded(out, ct.date.month, 2);
  out.push_back('-');
  append_padded(out, ct.date.day, 2);
  out.push_back('T');
  append_padded(out, ct.hour, 2);
  out.push_back(':');
  append_padded(out, ct.minute, 2);
  out.push_back(':');
  append_padded(out, ct.second, 2);
  out.push_back('.');
  append_padded(out, ct.nano, 9);
  out.push_back('Z');
  return out;
}

Result<Timestamp> from_iso8601(std::string_view s) {
  usize pos = 0;
  u32 yu = 0;
  u32 mo = 0;
  u32 d = 0;
  u32 h = 0;
  u32 mi = 0;
  u32 sec = 0;
  if (!take_uint(s, pos, 4, yu)) {
    return Err(ErrorCode::ParseError, "year");
  }
  if (!take_char(s, pos, '-')) {
    return Err(ErrorCode::ParseError, "expected '-'");
  }
  if (!take_uint(s, pos, 2, mo)) {
    return Err(ErrorCode::ParseError, "month");
  }
  if (!take_char(s, pos, '-')) {
    return Err(ErrorCode::ParseError, "expected '-'");
  }
  if (!take_uint(s, pos, 2, d)) {
    return Err(ErrorCode::ParseError, "day");
  }
  if (!take_char(s, pos, 'T')) {
    return Err(ErrorCode::ParseError, "expected 'T'");
  }
  if (!take_uint(s, pos, 2, h)) {
    return Err(ErrorCode::ParseError, "hour");
  }
  if (!take_char(s, pos, ':')) {
    return Err(ErrorCode::ParseError, "expected ':'");
  }
  if (!take_uint(s, pos, 2, mi)) {
    return Err(ErrorCode::ParseError, "minute");
  }
  if (!take_char(s, pos, ':')) {
    return Err(ErrorCode::ParseError, "expected ':'");
  }
  if (!take_uint(s, pos, 2, sec)) {
    return Err(ErrorCode::ParseError, "second");
  }

  u32 nano = 0;
  if (pos < s.size() && s[pos] == '.') {
    ++pos;
    u32 scale = 100'000'000U; // first fractional digit weight (1e8 ns)
    usize digits = 0;
    while (pos < s.size() && is_digit(s[pos])) {
      if (digits < 9) {
        nano += static_cast<u32>(s[pos] - '0') * scale;
        scale /= 10U;
      }
      ++pos;
      ++digits;
    }
    if (digits == 0) {
      return Err(ErrorCode::ParseError, "empty fraction");
    }
  }

  if (!take_char(s, pos, 'Z')) {
    return Err(ErrorCode::ParseError, "expected 'Z'");
  }
  if (pos != s.size()) {
    return Err(ErrorCode::ParseError, "trailing characters");
  }

  const i32 y = static_cast<i32>(yu);
  if (!is_valid_date(y, mo, d) || h > 23U || mi > 59U || sec > 59U) {
    return Err(ErrorCode::ParseError, "field out of range");
  }
  return Ok(timestamp_from_utc(y, mo, d, h, mi, sec, nano));
}

// ---- NYSE trading calendar --------------------------------------------------
bool Calendar::is_holiday(Date d) const noexcept {
  if (is_weekend(d)) {
    return false; // weekends are non-trading but not "holidays"
  }
  const i32 y = d.year;
  if (d == observed_holiday(Date{y, 1, 1}, /*observe_friday=*/false)) {
    return true; // New Year's
  }
  if (d == nth_weekday_of_month(y, 1, Weekday::Monday, 3)) {
    return true; // MLK
  }
  if (d == nth_weekday_of_month(y, 2, Weekday::Monday, 3)) {
    return true; // Washington
  }
  if (d == good_friday(y)) {
    return true; // Good Friday
  }
  if (d == last_weekday_of_month(y, 5, Weekday::Monday)) {
    return true; // Memorial
  }
  if (y >= 2022 && d == observed_holiday(Date{y, 6, 19}, true)) {
    return true; // Juneteenth
  }
  if (d == observed_holiday(Date{y, 7, 4}, true)) {
    return true; // Independence
  }
  if (d == nth_weekday_of_month(y, 9, Weekday::Monday, 1)) {
    return true; // Labor
  }
  if (d == nth_weekday_of_month(y, 11, Weekday::Thursday, 4)) {
    return true; // Thanksgiving
  }
  if (d == observed_holiday(Date{y, 12, 25}, true)) {
    return true; // Christmas
  }
  return false;
}

bool Calendar::is_trading_day(Date d) const noexcept { return !is_weekend(d) && !is_holiday(d); }

bool Calendar::is_early_close(Date d) const noexcept {
  if (!is_trading_day(d)) {
    return false; // an early close is still a trading day
  }
  const i32 y = d.year;
  const Date thanksgiving = nth_weekday_of_month(y, 11, Weekday::Thursday, 4);
  if (d == Date::from_days(thanksgiving.to_days() + 1)) {
    return true; // day after Thanksgiving
  }
  if (d == Date{y, 12, 24}) {
    return true; // Christmas Eve (a weekday here)
  }
  if (d == Date{y, 7, 3}) {                                         // July 3, if July 4 is a trading day
    const Date independence{y, 7, 4};
    if (!is_weekend(independence)) {
      return true;
    }
  }
  return false;
}

Date Calendar::next_trading_day(Date d) const noexcept {
  // Bounded: the largest holiday+weekend gap on the NYSE calendar is a few days.
  Date c = d;
  for (int i = 0; i < 16; ++i) {
    c = Date::from_days(c.to_days() + 1);
    if (is_trading_day(c)) {
      return c;
    }
  }
  ATX_ASSERT(false); // unreachable for the NYSE rule set
  return d;
}

Date Calendar::prev_trading_day(Date d) const noexcept {
  Date c = d;
  for (int i = 0; i < 16; ++i) {
    c = Date::from_days(c.to_days() - 1);
    if (is_trading_day(c)) {
      return c;
    }
  }
  ATX_ASSERT(false); // unreachable for the NYSE rule set
  return d;
}

Timestamp Calendar::session_open(Date d) const noexcept {
  ATX_ASSERT(is_trading_day(d));
  const i64 off_h = eastern_is_dst(d) ? 4 : 5; // UTC = local ET + offset
  return timestamp_from_utc(d.year, d.month, d.day, 9, 30, 0, 0) + Duration::hours(off_h);
}

Timestamp Calendar::session_close(Date d) const noexcept {
  ATX_ASSERT(is_trading_day(d));
  const i64 off_h = eastern_is_dst(d) ? 4 : 5;
  const u32 close_h = is_early_close(d) ? 13U : 16U;
  return timestamp_from_utc(d.year, d.month, d.day, close_h, 0, 0, 0) + Duration::hours(off_h);
}

} // namespace atx::core::time
