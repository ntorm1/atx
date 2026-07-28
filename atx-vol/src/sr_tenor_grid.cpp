#include "atx/vol/sr_tenor_grid.hpp"

#include <cstdint>
#include <string>

#include "atx/core/error.hpp"
#include "atx/vol/vol_time.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr std::int64_t kNsPerSec = 1'000'000'000LL;
constexpr std::int64_t kNsPerDay = 24LL * 3600LL * kNsPerSec;

}  // namespace

Result<std::int64_t> advance_trading_days(std::int64_t now_ns, int n, const VolTimeCalendar& cal) {
  if (n < 0) {
    return Err(ErrorCode::InvalidArgument,
               "advance_trading_days: n must be >= 0, got " + std::to_string(n));
  }

  // Split now_ns into a civil day index (days-since-epoch, matching
  // VolTimeCalendar's numbering) + intraday ns-of-day, floor-dividing so
  // pre-epoch instants (negative now_ns) still land on the correct civil day
  // (truncating integer division rounds toward zero, not -inf).
  std::int64_t day = now_ns / kNsPerDay;
  std::int64_t tod = now_ns % kNsPerDay;
  if (tod < 0) {
    tod += kNsPerDay;
    --day;
  }

  // Bounded loop (JPL Rule 2): counting n trading days requires stepping past
  // some number of non-trading civil days too. This calendar's non-trading
  // runs around a trading day never exceed a 3-day weekend+adjacent-holiday
  // cluster, so 2*n + 20 civil steps is a generous, statically-obvious upper
  // bound for every n in SrTenorGrid::kTradingDays (max 504); the loop
  // condition itself enforces it (not just the assert below), so this stays
  // bounded even in a release build with assertions stripped.
  const std::int64_t max_steps = 2LL * static_cast<std::int64_t>(n) + 20LL;
  int remaining = n;
  for (std::int64_t steps = 0; steps < max_steps && remaining > 0; ++steps) {
    ++day;
    const auto day32 = static_cast<std::int32_t>(day);
    if (is_weekend_day(day32)) {
      continue;  // a Saturday is a Saturday at any date, table or no table
    }
    // FAIL CLOSED before consulting the table, on every day whose closure status
    // decides whether this step COUNTS. `is_holiday` cannot distinguish "not a
    // listed closure" from "outside the range the table was populated for" -- both
    // answer false -- so reading it beyond `covers()` silently counts real
    // closures as trading days and lands the tenor one or more days early (plan
    // item 1.10, the same treatment `trading_hours_between` applies at its accrual
    // site). Weekend days above are exempt because their status comes from the day
    // number itself, never from `cal`.
    if (!cal.covers(day32)) {
      return Err(ErrorCode::OutOfRange,
                 "advance_trading_days: day " + std::to_string(day32) +
                     " would be counted as a trading day but lies outside the calendar's "
                     "covered window [" +
                     std::to_string(cal.first_covered_day()) + ", " +
                     std::to_string(cal.last_covered_day()) + "] (days since 1970-01-01)");
    }
    if (cal.is_holiday(day32)) {
      continue;
    }
    --remaining;
  }
  if (remaining > 0) {
    // The bound is generous for any real closure calendar (see max_steps above), so
    // exhausting it means `cal` closes essentially every day in the stepped window
    // and the n-th trading day does not exist there. Report it: the pre-Result code
    // asserted in debug and returned the day the bound stopped on in release, which
    // is an ordinary-looking instant that is simply not the requested tenor.
    return Err(ErrorCode::OutOfRange,
               "advance_trading_days: no " + std::to_string(n) +
                   "-th trading day within " + std::to_string(max_steps) +
                   " civil days of the start (calendar closes the whole window)");
  }

  return Ok(day * kNsPerDay + tod);
}

Result<double> tenor_years(std::int64_t now_ns, int n_trading_days, const TimeSpec& spec) {
  ATX_TRY(const std::int64_t expiry_ns,
          advance_trading_days(now_ns, n_trading_days, VolTimeCalendar::us_default()));
  return time_to_expiry_years(now_ns, expiry_ns, spec);
}

}  // namespace atx::vol
