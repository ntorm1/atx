#pragma once

// atx::core::time — the date/time/timestamp standard library for atx.
//
// Provides, in one header (L8 `time`):
//   Duration   — strong type over i64 nanoseconds; arithmetic + unit conversions.
//   Timestamp  — strong type over i64 nanoseconds since the Unix epoch (UTC).
//   Date       — proleptic-Gregorian civil date (year/month/day) + day arithmetic.
//   CivilTime  — UTC decomposition of a Timestamp (date + hh:mm:ss.nanos).
//   ISO-8601   — to_iso8601 / from_iso8601 (UTC, nanosecond precision).
//   Calendar   — NYSE trading calendar: holidays, half-days, sessions (DST-aware).
//
// Representation: int64 nanoseconds throughout (≈ ±292 years around 1970, i.e.
// usable 1678–2262). All civil math uses Howard Hinnant's days_from_civil /
// civil_from_days algorithms (exact, branch-light, constexpr).
//
// Time-zone scope: only US/Eastern (America/New_York) is modelled, via the modern
// (2007+) US DST rule, because the trading calendar targets US equities. No IANA
// tz database dependency.
//
// Ownership/threading: every type here is a trivially-copyable value; all functions
// are pure (no global state) and thread-safe. Expected failures (parse) return
// Result<T>; programming errors (out-of-range civil fields) trip ATX_ASSERT.

#include <compare>
#include <limits>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/macro.hpp"
#include "atx/core/types.hpp"

namespace atx::core::time {

// =====================================================================
//  Duration — signed nanosecond span.
// =====================================================================
class Duration {
public:
  static constexpr i64 kNsPerUs = 1'000;
  static constexpr i64 kNsPerMs = 1'000'000;
  static constexpr i64 kNsPerSec = 1'000'000'000;
  static constexpr i64 kNsPerMin = 60 * kNsPerSec;
  static constexpr i64 kNsPerHour = 60 * kNsPerMin;
  static constexpr i64 kNsPerDay = 24 * kNsPerHour;

  constexpr Duration() noexcept = default;

  // SAFETY / precondition: the factories below and operator* multiply by a
  // fixed ns-per-unit constant; the result must fit in i64. The widest unit
  // (days) overflows for |n| > ~1.06e8 days (~292 471 years). Callers stay far
  // inside that range for any realistic market timeline. No runtime check is
  // performed (these are constexpr noexcept hot-path constructors, and
  // ATX_ASSERT is not constexpr-friendly — see math.hpp); overflow is UB.
  [[nodiscard]] static constexpr Duration nanoseconds(i64 n) noexcept { return Duration{n}; }
  [[nodiscard]] static constexpr Duration microseconds(i64 n) noexcept { return Duration{n * kNsPerUs}; }
  [[nodiscard]] static constexpr Duration milliseconds(i64 n) noexcept { return Duration{n * kNsPerMs}; }
  [[nodiscard]] static constexpr Duration seconds(i64 n) noexcept { return Duration{n * kNsPerSec}; }
  [[nodiscard]] static constexpr Duration minutes(i64 n) noexcept { return Duration{n * kNsPerMin}; }
  [[nodiscard]] static constexpr Duration hours(i64 n) noexcept { return Duration{n * kNsPerHour}; }
  [[nodiscard]] static constexpr Duration days(i64 n) noexcept { return Duration{n * kNsPerDay}; }
  [[nodiscard]] static constexpr Duration zero() noexcept { return Duration{0}; }

  // Coarse accessors truncate toward zero (integer division).
  [[nodiscard]] constexpr i64 count_ns() const noexcept { return ns_; }
  [[nodiscard]] constexpr i64 count_us() const noexcept { return ns_ / kNsPerUs; }
  [[nodiscard]] constexpr i64 count_ms() const noexcept { return ns_ / kNsPerMs; }
  [[nodiscard]] constexpr i64 count_seconds() const noexcept { return ns_ / kNsPerSec; }
  [[nodiscard]] constexpr i64 count_minutes() const noexcept { return ns_ / kNsPerMin; }
  [[nodiscard]] constexpr i64 count_hours() const noexcept { return ns_ / kNsPerHour; }
  [[nodiscard]] constexpr i64 count_days() const noexcept { return ns_ / kNsPerDay; }
  [[nodiscard]] constexpr f64 as_seconds() const noexcept {
    return static_cast<f64>(ns_) / static_cast<f64>(kNsPerSec);
  }

  friend constexpr Duration operator+(Duration a, Duration b) noexcept { return Duration{a.ns_ + b.ns_}; }
  friend constexpr Duration operator-(Duration a, Duration b) noexcept { return Duration{a.ns_ - b.ns_}; }
  [[nodiscard]] constexpr Duration operator-() const noexcept { return Duration{-ns_}; }
  friend constexpr Duration operator*(Duration d, i64 k) noexcept { return Duration{d.ns_ * k}; }
  friend constexpr Duration operator*(i64 k, Duration d) noexcept { return Duration{d.ns_ * k}; }
  // SAFETY / precondition: k != 0. Integer division by zero is UB; this is a
  // constexpr noexcept op so no runtime guard is emitted (consistent with the
  // rest of the type). Also k == -1 with d == i64::min overflows — also a
  // documented precondition.
  [[nodiscard]] friend constexpr Duration operator/(Duration d, i64 k) noexcept { return Duration{d.ns_ / k}; }
  friend constexpr auto operator<=>(Duration, Duration) noexcept = default;

private:
  explicit constexpr Duration(i64 ns) noexcept : ns_{ns} {}
  i64 ns_{};
};

// =====================================================================
//  Timestamp — nanoseconds since 1970-01-01T00:00:00Z (UTC).
// =====================================================================
class Timestamp {
public:
  constexpr Timestamp() noexcept = default;

  [[nodiscard]] static constexpr Timestamp from_unix_nanos(i64 ns) noexcept { return Timestamp{ns}; }
  [[nodiscard]] static constexpr Timestamp from_unix_micros(i64 us) noexcept { return Timestamp{us * Duration::kNsPerUs}; }
  [[nodiscard]] static constexpr Timestamp from_unix_millis(i64 ms) noexcept { return Timestamp{ms * Duration::kNsPerMs}; }
  [[nodiscard]] static constexpr Timestamp from_unix_seconds(i64 s) noexcept { return Timestamp{s * Duration::kNsPerSec}; }
  [[nodiscard]] static constexpr Timestamp epoch() noexcept { return Timestamp{0}; }
  [[nodiscard]] static constexpr Timestamp min() noexcept { return Timestamp{std::numeric_limits<i64>::min()}; }
  [[nodiscard]] static constexpr Timestamp max() noexcept { return Timestamp{std::numeric_limits<i64>::max()}; }

  // Coarse accessors truncate toward zero; prefer unix_nanos() for exactness.
  [[nodiscard]] constexpr i64 unix_nanos() const noexcept { return ns_; }
  [[nodiscard]] constexpr i64 unix_micros() const noexcept { return ns_ / Duration::kNsPerUs; }
  [[nodiscard]] constexpr i64 unix_millis() const noexcept { return ns_ / Duration::kNsPerMs; }
  [[nodiscard]] constexpr i64 unix_seconds() const noexcept { return ns_ / Duration::kNsPerSec; }

  friend constexpr Timestamp operator+(Timestamp t, Duration d) noexcept { return Timestamp{t.ns_ + d.count_ns()}; }
  friend constexpr Timestamp operator+(Duration d, Timestamp t) noexcept { return Timestamp{t.ns_ + d.count_ns()}; }
  friend constexpr Timestamp operator-(Timestamp t, Duration d) noexcept { return Timestamp{t.ns_ - d.count_ns()}; }
  friend constexpr Duration operator-(Timestamp a, Timestamp b) noexcept { return Duration::nanoseconds(a.ns_ - b.ns_); }
  friend constexpr auto operator<=>(Timestamp, Timestamp) noexcept = default;

private:
  explicit constexpr Timestamp(i64 ns) noexcept : ns_{ns} {}
  i64 ns_{};
};

// =====================================================================
//  Civil date (proleptic Gregorian).
// =====================================================================
enum class Weekday : u8 { Monday = 1, Tuesday, Wednesday, Thursday, Friday, Saturday, Sunday };

struct Date {
  i32 year{};
  u32 month{};
  u32 day{};

  friend constexpr auto operator<=>(const Date &, const Date &) noexcept = default;

  [[nodiscard]] constexpr i64 to_days() const noexcept;          // days since 1970-01-01
  [[nodiscard]] static constexpr Date from_days(i64 z) noexcept; // inverse of to_days
};

// Days since 1970-01-01 for a civil (y, m, d). Hinnant's algorithm (exact for the
// proleptic Gregorian calendar; relies on well-defined unsigned modular arithmetic).
[[nodiscard]] constexpr i64 days_from_civil(i32 y, u32 m, u32 d) noexcept {
  y -= (m <= 2) ? 1 : 0;
  const i64 era = (y >= 0 ? y : y - 399) / 400;
  const u32 yoe = static_cast<u32>(static_cast<i64>(y) - era * 400);          // [0, 399]
  const u32 doy = (153U * (m + (m > 2 ? -3U : 9U)) + 2U) / 5U + d - 1U;       // [0, 365]
  const u32 doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;                   // [0, 146096]
  return era * 146097 + static_cast<i64>(doe) - 719468;
}

[[nodiscard]] constexpr Date civil_from_days(i64 z) noexcept {
  z += 719468;
  const i64 era = (z >= 0 ? z : z - 146096) / 146097;
  const u32 doe = static_cast<u32>(z - era * 146097);                         // [0, 146096]
  const u32 yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;  // [0, 399]
  const i64 y = static_cast<i64>(yoe) + era * 400;
  const u32 doy = doe - (365U * yoe + yoe / 4U - yoe / 100U); // [0, 365]
  const u32 mp = (5U * doy + 2U) / 153U;                      // [0, 11]
  const u32 d = doy - (153U * mp + 2U) / 5U + 1U;             // [1, 31]
  const u32 m = mp + (mp < 10U ? 3U : static_cast<u32>(-9));  // [1, 12]
  return Date{static_cast<i32>(y + (m <= 2 ? 1 : 0)), m, d};
}

constexpr i64 Date::to_days() const noexcept { return days_from_civil(year, month, day); }
constexpr Date Date::from_days(i64 z) noexcept { return civil_from_days(z); }

// 0 = Sunday .. 6 = Saturday, for days since 1970-01-01 (Hinnant).
[[nodiscard]] constexpr u32 weekday_from_days(i64 z) noexcept {
  return static_cast<u32>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

[[nodiscard]] constexpr Weekday weekday(Date d) noexcept {
  const u32 w0 = weekday_from_days(d.to_days());        // 0=Sun..6=Sat
  return static_cast<Weekday>(w0 == 0U ? 7U : w0);      // ISO: Mon=1..Sun=7
}

[[nodiscard]] constexpr bool is_weekend(Date d) noexcept {
  const Weekday w = weekday(d);
  return w == Weekday::Saturday || w == Weekday::Sunday;
}

[[nodiscard]] constexpr bool is_leap_year(i32 y) noexcept {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

[[nodiscard]] constexpr u32 days_in_month(i32 y, u32 m) noexcept {
  switch (m) {
  case 1: case 3: case 5: case 7: case 8: case 10: case 12:
    return 31U;
  case 4: case 6: case 9: case 11:
    return 30U;
  case 2:
    return is_leap_year(y) ? 29U : 28U;
  default:
    return 0U; // invalid month
  }
}

[[nodiscard]] constexpr bool is_valid_date(i32 y, u32 m, u32 d) noexcept {
  if (m < 1U || m > 12U) {
    return false;
  }
  if (d < 1U) {
    return false;
  }
  return d <= days_in_month(y, m);
}

// n-th (1-based) given weekday in a month, e.g. nth_weekday_of_month(y,1,Monday,3) = MLK Day.
[[nodiscard]] constexpr Date nth_weekday_of_month(i32 y, u32 m, Weekday wd, u32 n) noexcept {
  const u32 first_wd = static_cast<u32>(weekday(Date{y, m, 1}));   // 1..7
  const u32 target = static_cast<u32>(wd);
  const u32 offset = (target + 7U - first_wd) % 7U;
  return Date{y, m, 1U + offset + (n - 1U) * 7U};
}

[[nodiscard]] constexpr Date last_weekday_of_month(i32 y, u32 m, Weekday wd) noexcept {
  const u32 dim = days_in_month(y, m);
  const u32 last_wd = static_cast<u32>(weekday(Date{y, m, dim}));
  const u32 target = static_cast<u32>(wd);
  return Date{y, m, dim - ((last_wd + 7U - target) % 7U)};
}

// Easter Sunday (Gregorian; Anonymous/Meeus computus). Good Friday = Easter − 2 days.
[[nodiscard]] constexpr Date easter_sunday(i32 y) noexcept {
  const i32 a = y % 19;
  const i32 b = y / 100;
  const i32 c = y % 100;
  const i32 d = b / 4;
  const i32 e = b % 4;
  const i32 f = (b + 8) / 25;
  const i32 g = (b - f + 1) / 3;
  const i32 h = (19 * a + b - d - g + 15) % 30;
  const i32 i = c / 4;
  const i32 k = c % 4;
  const i32 l = (32 + 2 * e + 2 * i - h - k) % 7;
  const i32 mm = (a + 11 * h + 22 * l) / 451;
  const i32 month = (h + l - 7 * mm + 114) / 31;
  const i32 day = ((h + l - 7 * mm + 114) % 31) + 1;
  return Date{y, static_cast<u32>(month), static_cast<u32>(day)};
}

[[nodiscard]] constexpr Date good_friday(i32 y) noexcept {
  return Date::from_days(easter_sunday(y).to_days() - 2);
}

// =====================================================================
//  US/Eastern DST (modern 2007+ rule). Trading-day granularity is exact for
//  session times because both transitions fall on Sundays (non-trading).
// =====================================================================
[[nodiscard]] constexpr bool eastern_is_dst(Date d) noexcept {
  const i64 start = nth_weekday_of_month(d.year, 3, Weekday::Sunday, 2).to_days();  // 2nd Sun Mar
  const i64 end = nth_weekday_of_month(d.year, 11, Weekday::Sunday, 1).to_days();   // 1st Sun Nov
  const i64 z = d.to_days();
  return z >= start && z < end;
}

[[nodiscard]] constexpr Duration eastern_utc_offset(Date d) noexcept {
  return eastern_is_dst(d) ? Duration::hours(-4) : Duration::hours(-5);
}

// =====================================================================
//  Civil time <-> Timestamp (UTC).
// =====================================================================
struct CivilTime {
  Date date{};
  u32 hour{};
  u32 minute{};
  u32 second{};
  u32 nano{};
};

[[nodiscard]] constexpr CivilTime to_civil_utc(Timestamp ts) noexcept {
  const i64 ns = ts.unix_nanos();
  i64 days = ns / Duration::kNsPerDay;
  i64 rem = ns % Duration::kNsPerDay;
  if (rem < 0) {                       // floor toward −∞ so time-of-day is in [0, day)
    rem += Duration::kNsPerDay;
    days -= 1;
  }
  CivilTime ct{};
  ct.date = Date::from_days(days);
  ct.hour = static_cast<u32>(rem / Duration::kNsPerHour);
  rem %= Duration::kNsPerHour;
  ct.minute = static_cast<u32>(rem / Duration::kNsPerMin);
  rem %= Duration::kNsPerMin;
  ct.second = static_cast<u32>(rem / Duration::kNsPerSec);
  ct.nano = static_cast<u32>(rem % Duration::kNsPerSec);
  return ct;
}

[[nodiscard]] constexpr Timestamp timestamp_from_utc(i32 y, u32 mo, u32 d, u32 h, u32 mi, u32 s,
                                                     u32 nano) noexcept {
  ATX_ASSERT(is_valid_date(y, mo, d));
  const i64 ns = days_from_civil(y, mo, d) * Duration::kNsPerDay +
                 static_cast<i64>(h) * Duration::kNsPerHour +
                 static_cast<i64>(mi) * Duration::kNsPerMin +
                 static_cast<i64>(s) * Duration::kNsPerSec + static_cast<i64>(nano);
  return Timestamp::from_unix_nanos(ns);
}

// =====================================================================
//  ISO-8601 (UTC, nanosecond precision). Defined in datetime.cpp.
//
//  Format:  YYYY-MM-DDТHH:MM:SS.nnnnnnnnnZ   (always 9 fractional digits, 'Z').
//  Parse:   accepts the above; the fractional part is optional (1..9 digits,
//           zero-padded to nanos); requires a trailing 'Z'. Any deviation, or an
//           out-of-range field, returns ErrorCode::ParseError.
// =====================================================================
[[nodiscard]] std::string to_iso8601(Timestamp ts);
[[nodiscard]] atx::core::Result<Timestamp> from_iso8601(std::string_view s);

// =====================================================================
//  NYSE trading calendar.
//
//  Models the rule-based regular-session schedule: ten standard holidays (with
//  weekend-observance shifting), Good Friday, three 1:00pm-ET early closes, and
//  DST-aware 09:30/16:00 ET sessions. Member bodies in datetime.cpp.
//
//  NOT modelled: ad-hoc / emergency closures (9/11, Hurricane Sandy, presidential
//  funerals). Those require a data-override table — a future extension. Juneteenth
//  is recognised from 2022 onward (first NYSE observance).
// =====================================================================
class Calendar {
public:
  Calendar() noexcept = default; // default ruleset = NYSE

  [[nodiscard]] bool is_holiday(Date d) const noexcept;
  [[nodiscard]] bool is_early_close(Date d) const noexcept;
  [[nodiscard]] bool is_trading_day(Date d) const noexcept; // weekday && !holiday

  [[nodiscard]] Date next_trading_day(Date d) const noexcept; // strictly after d
  [[nodiscard]] Date prev_trading_day(Date d) const noexcept; // strictly before d

  // Session boundaries as UTC instants. Precondition: is_trading_day(d).
  [[nodiscard]] Timestamp session_open(Date d) const noexcept;  // 09:30 ET
  [[nodiscard]] Timestamp session_close(Date d) const noexcept; // 16:00 ET, or 13:00 ET on a half-day
};

} // namespace atx::core::time
