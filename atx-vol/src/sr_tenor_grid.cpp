#include "atx/vol/sr_tenor_grid.hpp"

#include <cassert>
#include <cstdint>

#include "atx/vol/vol_time.hpp"

namespace atx::vol {

namespace {

constexpr std::int64_t kNsPerSec = 1'000'000'000LL;
constexpr std::int64_t kNsPerDay = 24LL * 3600LL * kNsPerSec;

}  // namespace

std::int64_t advance_trading_days(std::int64_t now_ns, int n,
                                  const VolTimeCalendar& cal) noexcept {
  assert(n >= 0);  // contract: caller advances forward only

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
    if (is_weekend_day(static_cast<std::int32_t>(day))) {
      continue;
    }
    if (cal.is_holiday(static_cast<std::int32_t>(day))) {
      continue;
    }
    --remaining;
  }
  assert(remaining == 0);  // the max_steps bound above must have been sufficient

  return day * kNsPerDay + tod;
}

Result<double> tenor_years(std::int64_t now_ns, int n_trading_days, const TimeSpec& spec) {
  const std::int64_t expiry_ns =
      advance_trading_days(now_ns, n_trading_days, VolTimeCalendar::us_default());
  return time_to_expiry_years(now_ns, expiry_ns, spec);
}

}  // namespace atx::vol
