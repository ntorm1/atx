#include "atx/vol/api/core/vol_time.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "atx/core/error.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr std::int64_t kNsPerSec = 1'000'000'000LL;
constexpr std::int64_t kNsPerHour = 3600LL * kNsPerSec;
constexpr std::int64_t kNsPerDay = 24LL * kNsPerHour;

// Bounded-loop guard (JPL Rule 2): the widest realistic option horizon is a
// handful of years (~1200 days for a 3y LEAPS), so a day-loop bound an order
// of magnitude above 5 years is generous headroom against a pathological
// caller-supplied span while never touching legitimate use.
//
// A span past the bound is REPORTED, not clamped. Clamping made the loop stop
// accruing at ~year 20 and answer anyway: the rest of the span was then reported
// as pure non-trading time, so `vol_time_years` returned a T_vol wrong by whole
// years -- a plausible number with no diagnostic, the same silent-truncation
// class this sprint is closing everywhere else. The bound is not widened
// instead, because nothing this module serves prices a >20y horizon: an option
// that far out is a caller error, and 7324 iterations of session math per query
// is already 20x the widest real use.
constexpr std::int64_t kMaxLoopDays = 20 * 366 + 4;  // ~20 years

struct CivilDate {
  std::int32_t y;
  std::uint32_t m;
  std::uint32_t d;
};

// Howard Hinnant's days_from_civil: exact for the proleptic Gregorian
// calendar; days-since-epoch (1970-01-01 = 0), no timezone attached.
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t y, std::uint32_t m,
                                                     std::uint32_t d) noexcept {
  y -= (m <= 2) ? 1 : 0;
  const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
  const std::uint32_t yoe = static_cast<std::uint32_t>(y - era * 400);              // [0, 399]
  const std::uint32_t doy = (153U * (m + (m > 2 ? 0U - 3U : 9U)) + 2U) / 5U + d - 1U; // [0, 365]
  const std::uint32_t doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;               // [0, 146096]
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Hinnant's civil_from_days: inverse of the above.
[[nodiscard]] constexpr CivilDate civil_from_days(std::int64_t z) noexcept {
  z += 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::uint32_t doe = static_cast<std::uint32_t>(z - era * 146097);           // [0, 146096]
  const std::uint32_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U; // [0,399]
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const std::uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);            // [0, 365]
  const std::uint32_t mp = (5U * doy + 2U) / 153U;                                 // [0, 11]
  const std::uint32_t d = doy - (153U * mp + 2U) / 5U + 1U;                        // [1, 31]
  const std::uint32_t m = mp + (mp < 10U ? 3U : (0U - 9U));                        // [1, 12]
  return CivilDate{static_cast<std::int32_t>(y + (m <= 2 ? 1 : 0)), m, d};
}

// n-th (1-based) occurrence of weekday `target` (0=Sun..6=Sat) in month (y,m),
// as a days-since-epoch value.
[[nodiscard]] constexpr std::int64_t nth_weekday_of_month(std::int32_t y, std::uint32_t m,
                                                          int target, int n) noexcept {
  const std::int64_t first = days_from_civil(y, m, 1U);
  const int first_wd = weekday_from_days(static_cast<std::int32_t>(first));
  const int offset = (target - first_wd + 7) % 7;
  return first + offset + static_cast<std::int64_t>(n - 1) * 7;
}

// US/Eastern modern (2007+) DST rule, resolved at calendar-day granularity:
// both transition instants (02:00 local on the 2nd Sunday of March / 1st
// Sunday of November) fall on Sundays, a non-trading day, so this is exact for
// every session boundary computed below.
[[nodiscard]] constexpr bool is_dst(std::int32_t y, std::uint32_t m, std::uint32_t d) noexcept {
  const std::int64_t z = days_from_civil(y, m, d);
  const std::int64_t start = nth_weekday_of_month(y, 3, /*Sunday=*/0, 2);
  const std::int64_t end = nth_weekday_of_month(y, 11, /*Sunday=*/0, 1);
  return z >= start && z < end;
}

// ET-minus-UTC offset in ns (negative: -4h EDT, -5h EST).
[[nodiscard]] constexpr std::int64_t et_utc_offset_ns(std::int32_t y, std::uint32_t m,
                                                      std::uint32_t d) noexcept {
  return is_dst(y, m, d) ? (-4LL * kNsPerHour) : (-5LL * kNsPerHour);
}

// UTC instant (epoch ns) of a fractional ET wall-clock `hour_et` on ET
// calendar day-number `z_et` (days-since-epoch, same numbering as
// `days_from_civil`). `hour_et` need not be an integer (e.g. 9.5 = 09:30).
//
// Pure integer math throughout (only the intermediate fractional-hour ->
// ns-of-day conversion touches a double, rounded via llround): `z_et *
// kNsPerDay` is ~1.7e18 ns for present-day dates, which exceeds double's
// exact-integer range (2^53), so this stays in int64 rather than promoting to
// double as an earlier draft did.
[[nodiscard]] std::int64_t et_local_to_utc_ns(std::int64_t z_et, double hour_et) noexcept {
  const CivilDate cd = civil_from_days(z_et);
  const std::int64_t offset_ns = et_utc_offset_ns(cd.y, cd.m, cd.d);
  const std::int64_t local_ns_of_day =
      static_cast<std::int64_t>(std::llround(hour_et * 3600.0 * 1.0e9));
  // UTC = ET_local - offset (offset is negative for the western hemisphere,
  // e.g. EDT: UTC = local + 4h).
  return z_et * kNsPerDay + local_ns_of_day - offset_ns;
}

// Floor division by the (positive) day length, i.e. the day-since-epoch index
// containing `ns` (negative ns floors toward -inf, matching civil-date
// numbering for pre-epoch instants).
[[nodiscard]] constexpr std::int64_t day_index(std::int64_t ns) noexcept {
  std::int64_t q = ns / kNsPerDay;
  const std::int64_t r = ns % kNsPerDay;
  if (r < 0) --q;
  return q;
}

}  // namespace

// See vol_time.hpp's doc comment for the day-index convention. Not
// `constexpr` (moved off the anonymous-namespace copy this promotes) -- no
// call site in this TU or elsewhere needs compile-time evaluation, so leaf
// runtime functions are the simpler, header-declared surface.
int weekday_from_days(std::int32_t day_since_epoch) noexcept {
  const std::int64_t z = day_since_epoch;
  return static_cast<int>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

bool is_weekend_day(std::int32_t day_since_epoch) noexcept {
  const int wd = weekday_from_days(day_since_epoch);
  return wd == 0 || wd == 6;
}

std::int64_t settlement_instant_ns(std::int32_t et_day_since_epoch,
                                   SettlementSession settle) noexcept {
  // 16:00 ET (Pm, the regular-session close) or 09:30 ET (Am, the opening
  // print), converted ET->UTC through the same date-resolved modern-DST rule
  // `et_local_to_utc_ns` applies to every session boundary this module computes.
  const double hour_et = (settle == SettlementSession::Am) ? 9.5 : 16.0;
  return et_local_to_utc_ns(static_cast<std::int64_t>(et_day_since_epoch), hour_et);
}

VolTimeCalendar::VolTimeCalendar(std::vector<std::int32_t> holiday_days,
                                 std::int32_t first_covered_day, std::int32_t last_covered_day)
    : days_(std::move(holiday_days)),
      first_covered_day_(first_covered_day),
      last_covered_day_(last_covered_day) {
  std::sort(days_.begin(), days_.end());
  days_.erase(std::unique(days_.begin(), days_.end()), days_.end());
}

bool VolTimeCalendar::is_holiday(std::int32_t day_since_epoch) const noexcept {
  return std::binary_search(days_.begin(), days_.end(), day_since_epoch);
}

std::int32_t VolTimeCalendar::first_covered_day() const noexcept { return first_covered_day_; }

std::int32_t VolTimeCalendar::last_covered_day() const noexcept { return last_covered_day_; }

bool VolTimeCalendar::covers(std::int32_t day_since_epoch) const noexcept {
  return day_since_epoch >= first_covered_day_ && day_since_epoch <= last_covered_day_;
}

// Coverage window of the NYSE closure table in `us_default()` below:
// 2024-01-01 through 2028-12-31, inclusive. These live HERE, adjacent to the
// table, because they are one fact with it: extending the table without
// extending the window silently keeps the new years fail-closed, and extending
// the window without the table reinstates exactly the silent-full-session bug
// the window exists to prevent. Change both in the same edit.
//
// The window is not widened backwards past 2024 "because closures are rare":
// `is_dst` implements the MODERN (2007+) US DST rule only, so pre-2007
// session boundaries would be off by an hour on top of the missing closures.
constexpr std::int32_t kUsDefaultFirstCoveredDay =
    static_cast<std::int32_t>(days_from_civil(2024, 1, 1));
constexpr std::int32_t kUsDefaultLastCoveredDay =
    static_cast<std::int32_t>(days_from_civil(2028, 12, 31));

const VolTimeCalendar& VolTimeCalendar::us_default() {
  static const VolTimeCalendar cal = [] {
    std::vector<std::int32_t> days;
    days.reserve(50);
    auto add = [&days](std::int32_t y, std::uint32_t m, std::uint32_t d) {
      days.push_back(static_cast<std::int32_t>(days_from_civil(y, m, d)));
    };
    // NYSE full-closure table, 2024-2028 inclusive (weekend-observance shifts
    // already applied). 2025-01-09 = National Day of Mourning (Jimmy Carter).
    // 2027 Independence Day observed Monday 07-05 (07-04 is a Sunday); 2027
    // Christmas observed Friday 12-24. 2028 New Year's Day is unobserved
    // (falls on a Saturday) and is intentionally absent from this table.
    add(2024, 1, 1);  add(2024, 1, 15); add(2024, 2, 19); add(2024, 3, 29);
    add(2024, 5, 27); add(2024, 6, 19); add(2024, 7, 4);  add(2024, 9, 2);
    add(2024, 11, 28); add(2024, 12, 25);

    add(2025, 1, 1);  add(2025, 1, 9);  add(2025, 1, 20); add(2025, 2, 17);
    add(2025, 4, 18); add(2025, 5, 26); add(2025, 6, 19); add(2025, 7, 4);
    add(2025, 9, 1);  add(2025, 11, 27); add(2025, 12, 25);

    add(2026, 1, 1);  add(2026, 1, 19); add(2026, 2, 16); add(2026, 4, 3);
    add(2026, 5, 25); add(2026, 6, 19); add(2026, 7, 3);  add(2026, 9, 7);
    add(2026, 11, 26); add(2026, 12, 25);

    add(2027, 1, 1);  add(2027, 1, 18); add(2027, 2, 15); add(2027, 3, 26);
    add(2027, 5, 31); add(2027, 6, 18); add(2027, 7, 5);  add(2027, 9, 6);
    add(2027, 11, 25); add(2027, 12, 24);

    add(2028, 1, 17); add(2028, 2, 21); add(2028, 4, 14); add(2028, 5, 29);
    add(2028, 6, 19); add(2028, 7, 4);  add(2028, 9, 4);  add(2028, 11, 23);
    add(2028, 12, 25);

    return VolTimeCalendar(std::move(days), kUsDefaultFirstCoveredDay, kUsDefaultLastCoveredDay);
  }();
  return cal;
}

Result<double> trading_hours_between(std::int64_t start_ns, std::int64_t end_ns,
                                     const VolTimeParams& p, const VolTimeCalendar& cal) {
  if (end_ns <= start_ns) {
    return Ok(0.0);
  }

  const double open_hour = p.session_open_hour_et;
  const double close_hour = open_hour + p.session_span_hours;

  // Loop over ET calendar day-numbers that could possibly overlap
  // [start_ns, end_ns). The ET/UTC offset is at most 5h, so padding the UTC
  // day-index range by one full day on each side is always sufficient; days
  // outside the true overlap contribute exactly 0 once intersected below.
  //
  // The int32 narrowing is total, not a bet: |day_index(ns)| <= INT64_MAX /
  // 86400e9 ~= 106752 for every representable `ns`, three orders of magnitude
  // inside int32.
  const auto z_first = static_cast<std::int32_t>(day_index(start_ns));
  const auto z_last = static_cast<std::int32_t>(day_index(end_ns));
  const std::int64_t z_lo = static_cast<std::int64_t>(z_first) - 1;
  const std::int64_t z_hi = static_cast<std::int64_t>(z_last) + 1;
  if (z_hi - z_lo > kMaxLoopDays) {
    // FAIL CLOSED rather than clamp: see kMaxLoopDays. A truncated loop answers
    // with a T_vol short by every session past the bound.
    return Err(ErrorCode::OutOfRange,
               "trading_hours_between: interval spans " + std::to_string(z_hi - z_lo) +
                   " days, past the " + std::to_string(kMaxLoopDays) +
                   "-day (~20 year) bound this clock is defined over");
  }

  double trading_ns = 0.0;
  for (std::int64_t z = z_lo; z <= z_hi; ++z) {
    const auto z32 = static_cast<std::int32_t>(z);
    if (is_weekend_day(z32)) {
      continue;  // a Saturday is a Saturday at any date, table or no table
    }
    if (cal.is_holiday(z32)) {
      continue;  // a LISTED closure is positive information at any date
    }

    const std::int64_t sess_open = et_local_to_utc_ns(z, open_hour);
    const std::int64_t sess_close = et_local_to_utc_ns(z, close_hour);

    const std::int64_t lo = std::max(start_ns, sess_open);
    const std::int64_t hi = std::min(end_ns, sess_close);
    if (hi > lo) {
      // FAIL CLOSED here, at the CONTRIBUTION site, so the coverage-checked set
      // is exactly the contributing set for ANY `p`. Gating on the interval's
      // own day span [z_first, z_last] instead would leave the two padding days
      // unchecked, and that is only safe while a session cannot spill across a
      // UTC midnight -- i.e. while `session_open_hour_et + session_span_hours
      // <= 19` ET. Both are caller-supplied and unvalidated, so an
      // extended-hours configuration would silently accrue an UNCOVERED day's
      // trading time: precisely the "unknown day read as open" defect this
      // guard exists to close. Cheap, too -- two integer compares on the days
      // that actually accrue.
      if (!cal.covers(z32)) {
        return Err(ErrorCode::OutOfRange,
                   "trading_hours_between: day " + std::to_string(z32) +
                       " accrues trading time but lies outside the calendar's covered window [" +
                       std::to_string(cal.first_covered_day()) + ", " +
                       std::to_string(cal.last_covered_day()) + "] (days since 1970-01-01)");
      }
      // Each term is at most one session's worth of ns (~2.7e13), far below
      // double's exact-integer range (2^53), so this widening is exact; the
      // running sum stays exact too for any realistic (or even the
      // kMaxLoopDays-guarded) accumulation span.
      trading_ns += static_cast<double>(hi - lo);
    }
  }
  return Ok(trading_ns / (3600.0 * 1.0e9));
}

Result<double> vol_time_years(std::int64_t now_ns, std::int64_t expiry_ns, const VolTimeParams& p,
                              const VolTimeCalendar& cal) {
  if (expiry_ns <= now_ns) {
    return Ok(0.0);
  }
  ATX_TRY(const double trading_h, trading_hours_between(now_ns, expiry_ns, p, cal));
  const double total_h =
      static_cast<double>(expiry_ns - now_ns) / (3600.0 * 1.0e9);
  const double nontrading_h = total_h - trading_h;

  return Ok(trading_h * (p.alpha / p.trading_hours_per_year) +
            nontrading_h * ((1.0 - p.alpha) / p.nontrading_hours_per_year));
}

Result<double> time_to_expiry_years(std::int64_t from_ns, std::int64_t to_ns,
                                    const TimeSpec& spec) {
  switch (spec.convention) {
    case TimeConvention::Calendar365:
      return Ok(static_cast<double>(to_ns - from_ns) / kCalendarYearNs);
    case TimeConvention::VolTime:
      return vol_time_years(from_ns, to_ns, spec.vol_time, VolTimeCalendar::us_default());
  }
  // Unreachable (switch is exhaustive over TimeConvention's two enumerators;
  // -Wswitch enforces it stays that way). A trailing return keeps this a
  // well-formed function for compilers that cannot prove switch exhaustiveness.
  return Ok(static_cast<double>(to_ns - from_ns) / kCalendarYearNs);
}

std::int64_t ns_from_year_fraction(std::int64_t from_ns, double years) noexcept {
  return from_ns + static_cast<std::int64_t>(std::llround(years * kCalendarYearNs));
}

}  // namespace atx::vol
