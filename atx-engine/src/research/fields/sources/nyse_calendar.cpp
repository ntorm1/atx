#include "atx/engine/research/fields/sources/nyse_calendar.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <utility>

#include "atx/core/sha256.hpp"
#include "atx/engine/research/fields/clock.hpp"

namespace atx::engine::research::fields {
namespace {

// research_fields_sec.py NYSE_SPECIAL_CLOSURES (constants of the rule, not data).
constexpr std::array<std::string_view, 10> kSpecialClosures{
    "2001-09-11", "2001-09-12", "2001-09-13", "2001-09-14", "2004-06-11",
    "2007-01-02", "2012-10-29", "2012-10-30", "2018-12-05", "2025-01-09"};

constexpr i64 kMonday = 0;
constexpr i64 kThursday = 3;
constexpr i64 kSaturday = 5;
constexpr i64 kSunday = 6;
// Far beyond any role's history; keeps 2 * pre_sessions + 14 well inside i64.
constexpr usize kMaxPreSessions = usize{1} << 20;

[[nodiscard]] constexpr i64 floor_mod(i64 a, i64 m) noexcept {
  const i64 r = a % m;
  return r < 0 ? r + m : r;
}

// _nth_weekday: the n-th `weekday` (Monday 0) of the month.
[[nodiscard]] i64 nth_weekday(i64 year, u32 month, i64 weekday, i64 n) noexcept {
  const i64 first = days_from_civil(year, month, 1);
  return first + floor_mod(weekday - weekday_of(first), 7) + 7 * (n - 1);
}

// _last_weekday: the last `weekday` of the month (the day before the next month's first).
[[nodiscard]] i64 last_weekday(i64 year, u32 month, i64 weekday) noexcept {
  const i64 last = days_from_civil(year + static_cast<i64>(month / 12U), month % 12U + 1U, 1) - 1;
  return last - floor_mod(weekday_of(last) - weekday, 7);
}

// _easter: Gregorian Easter Sunday (anonymous / Meeus-Jones-Butcher); every operand is
// non-negative for year >= 1, so C++ division and remainder equal Python's floor forms.
[[nodiscard]] i64 easter(i64 year) noexcept {
  const i64 a = year % 19;
  const i64 b = year / 100;
  const i64 c = year % 100;
  const i64 d = b / 4;
  const i64 e = b % 4;
  const i64 g = (b - (b + 8) / 25 + 1) / 3;
  const i64 h = (19 * a + b - d - g + 15) % 30;
  const i64 i = c / 4;
  const i64 k = c % 4;
  const i64 el = (32 + 2 * e + 2 * i - h - k) % 7;
  const i64 m = (a + 11 * h + 22 * el) / 451;
  const i64 month = (h + el - 7 * m + 114) / 31;
  const i64 day = (h + el - 7 * m + 114) % 31;
  return days_from_civil(year, static_cast<u32>(month), static_cast<u32>(day + 1));
}

// _observed: a Saturday holiday is observed on Friday, a Sunday one on Monday.
[[nodiscard]] i64 observed(i64 day) noexcept {
  const i64 w = weekday_of(day);
  if (w == kSaturday) {
    return day - 1;
  }
  return w == kSunday ? day + 1 : day;
}

// nyse_holidays(year), appended to `out`.
void add_holidays(i64 year, std::vector<i64> &out) {
  out.push_back(nth_weekday(year, 1, kMonday, 3));
  out.push_back(nth_weekday(year, 2, kMonday, 3));
  out.push_back(easter(year) - 2);
  out.push_back(last_weekday(year, 5, kMonday));
  out.push_back(observed(days_from_civil(year, 7, 4)));
  out.push_back(nth_weekday(year, 9, kMonday, 1));
  out.push_back(nth_weekday(year, 11, kThursday, 4));
  out.push_back(observed(days_from_civil(year, 12, 25)));
  const i64 new_year = days_from_civil(year, 1, 1);
  if (weekday_of(new_year) != kSaturday) { // a weekday or a Sunday New Year is observed
    out.push_back(observed(new_year));
  }
  if (year >= 2022) {
    out.push_back(observed(days_from_civil(year, 6, 19)));
  }
  for (const std::string_view text : kSpecialClosures) {
    const auto day = parse_iso_day(text);
    if (day && year_of(*day) == year) {
      out.push_back(*day);
    }
  }
}

} // namespace

i64 weekday_of(i64 day) noexcept { return floor_mod(day + 3, 7); } // 1970-01-01 was a Thursday

std::vector<i64> nyse_sessions(i64 first_day, i64 last_day) {
  std::vector<i64> out;
  if (last_day < first_day) {
    return out;
  }
  std::vector<i64> holidays;
  for (i64 year = year_of(first_day); year <= year_of(last_day); ++year) {
    add_holidays(year, holidays);
  }
  std::sort(holidays.begin(), holidays.end());
  out.reserve(static_cast<usize>(last_day - first_day) + 1U);
  for (i64 day = first_day; day <= last_day; ++day) {
    if (weekday_of(day) < kSaturday && !std::binary_search(holidays.begin(), holidays.end(), day)) {
      out.push_back(day);
    }
  }
  return out;
}

core::Result<ExtendedAxis> extended_axis(std::span<const i64> role_days, usize pre_sessions,
                                         i64 lookback_days) {
  if (role_days.empty() || lookback_days < 0 || pre_sessions > kMaxPreSessions) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "extended axis: needs a role axis, lookback_days >= 0 and a bounded history");
  }
  const i64 first = role_days.front();
  const auto pre = static_cast<i64>(pre_sessions);
  std::vector<i64> rule = nyse_sessions(first - 2 * pre - 14, first - 1);
  if (rule.size() < pre_sessions) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "extended axis: the NYSE rule calendar is shorter than the requested history");
  }
  if (pre_sessions > 0) {
    rule.erase(rule.begin(), rule.end() - static_cast<isize>(pre_sessions));
    if (lookback_days > 0) {
      rule = nyse_sessions(rule.front() - lookback_days, first - 1);
    }
  } else {
    rule.clear();
  }
  ExtendedAxis out;
  out.prefix = rule.size();
  out.days = std::move(rule);
  out.days.insert(out.days.end(), role_days.begin(), role_days.end());
  return core::Ok(std::move(out));
}

core::Result<SessionCalendarPin> session_calendar_pin() {
  static_assert(std::endian::native == std::endian::little,
                "the calendar digest hashes little-endian i64 days");
  const i64 first = 0; // 1970-01-01: before any pre-role session of any role
  const i64 last = seal_day() - 1;
  const std::vector<i64> days = nyse_sessions(first, last);
  std::string bytes(kNyseCalendarRule);
  bytes.push_back('\n');
  const usize head = bytes.size();
  bytes.resize(head + days.size() * sizeof(i64));
  if (!days.empty()) {
    std::memcpy(bytes.data() + head, days.data(), days.size() * sizeof(i64));
  }
  ATX_TRY(auto digest, core::sha256_hex(std::string_view(bytes)));
  SessionCalendarPin out;
  out.rule = std::string(kNyseCalendarRule);
  out.first = iso_date(first);
  out.last = iso_date(last);
  out.sessions = static_cast<u64>(days.size());
  out.sha256 = std::move(digest);
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::fields
