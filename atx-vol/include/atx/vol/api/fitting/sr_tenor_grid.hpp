#pragma once

// SpiderRock censored-term tenor grid (fixed TRADING-day horizons) plus a
// calendar-aware tenor -> year-fraction converter.
//
// ## Why trading days, not calendar days
//
// SpiderRock's `tbltickerhistory` censored-term columns (and the `nEarnCnt_Nd`
// scheduled-event counts that go with them) are keyed on a fixed TRADING-day
// horizon -- 21 trading days is "one month", 63 is "one quarter", 252 is "one
// year" -- NOT a fixed calendar-day horizon. A tenor's year-fraction is
// therefore NOT the naive `N/365.25`: it is the year-fraction between `now`
// and the instant `N` NYSE trading days from `now` (weekends and
// `VolTimeCalendar` holidays skipped), run through the caller's `TimeSpec`
// convention. `advance_trading_days` is the calendar-stepping primitive;
// `tenor_years` composes it with `time_to_expiry_years` (see
// `atx/vol/vol_time.hpp`) to produce that year-fraction directly.
//
// ## Day-index arithmetic (no civil Y/M/D round-trip needed)
//
// `VolTimeCalendar::is_holiday` and `advance_trading_days` both key off plain
// "days-since-epoch" integers (1970-01-01 = 0, no timezone attached to the
// day number itself -- see `vol_time.hpp`'s `VolTimeCalendar` doc comment).
// `floor(epoch_ns / 86400e9)` IS that same day-since-epoch index by
// construction (a UTC calendar day boundary every 86400e9 ns, matching the
// proleptic-Gregorian numbering `days_from_civil`/`civil_from_days` produce
// internally in `vol_time.cpp`), and weekday parity (Sat/Sun) is a direct
// `mod 7` of that same index (`weekday_from_days`/`is_weekend_day`, public on
// `vol_time.hpp`). So stepping trading days never needs to convert through a
// civil (year, month, day) triple at all -- unlike `vol_time.cpp`'s
// ET-session-boundary math, which DOES need `days_from_civil`/
// `civil_from_days` (and DST resolution) because it must locate a wall-clock
// hour on a specific ET calendar day; this module simply does not need that
// extra machinery.
//
// ## Thread-safety
//
// Both functions are pure functions of their arguments (plus the immutable
// `VolTimeCalendar`) -- safe to call concurrently from any number of threads.

#include <array>
#include <cstdint>

#include "atx/vol/api/core/vol_time.hpp"

namespace atx::vol {

// SpiderRock's 12-point censored-term tenor grid, in TRADING days (matches
// `tbltickerhistory`'s column horizons and `nEarnCnt_Nd`'s event-count
// window): 5,10 (near-dated), 21/42/63/84/105/126 (1-6 months, 21td/mo),
// 189/252 (9/12 months, 63td/qtr), 378/504 (18/24 months). This is a fixed,
// SpiderRock-native grid -- not derived from any per-underlying listed-expiry
// schedule.
struct SrTenorGrid {
  static constexpr std::array<int, 12> kTradingDays{5,   10,  21,  42,  63,  84,
                                                     105, 126, 189, 252, 378, 504};
};

// Advances `now_ns` by `n` NYSE trading days: steps one civil (UTC) day at a
// time, skipping Saturdays/Sundays and `cal.is_holiday(day)`, counting only
// the days that survive both filters, until `n` such days have been counted.
// The intraday time-of-day of `now_ns` is preserved exactly (only the civil
// day advances) -- this mirrors `nEarnCnt_Nd`'s definition, which counts
// trading DAYS, not trading hours.
//
// `n == 0` returns `now_ns` unchanged (zero-day advance): it reads no calendar
// day at all, so it is answerable regardless of `cal`'s coverage.
//
// FAILS CLOSED with `ErrorCode::OutOfRange` as soon as a day that would be
// COUNTED as a trading day falls outside `cal`'s covered window (see
// `VolTimeCalendar::covers` in vol_time.hpp). `is_holiday` answers false both
// for "not a listed closure" and for "outside the populated range", so beyond
// the window every real closure would read as open and the returned instant
// would land one or more days early -- silently. Weekends are exempt from the
// check: their status comes from the day number, never from the table.
//
// @param now_ns  evaluation instant, epoch nanoseconds (UTC)
// @param n       number of NYSE trading days to advance (>= 0)
// @param cal     holiday calendar (weekends are always skipped regardless)
// @return        epoch nanoseconds of the `n`-th trading day after `now_ns`,
//                same intraday time-of-day as `now_ns`;
//                `ErrorCode::InvalidArgument` for `n < 0`;
//                `ErrorCode::OutOfRange` when a counted day leaves `cal`'s
//                window, or when `cal` closes every day in the stepped window
//                so the `n`-th trading day does not exist there
[[nodiscard]] Result<std::int64_t> advance_trading_days(std::int64_t now_ns, int n,
                                                        const VolTimeCalendar& cal);

// Year-fraction of a fixed-trading-day tenor from `now_ns`: advances `now_ns`
// by `n_trading_days` NYSE trading days (`advance_trading_days`, against
// `VolTimeCalendar::us_default()` -- v1 does not support a caller-supplied
// calendar, matching `time_to_expiry_years`'s own `VolTime`-branch
// restriction), then applies `spec`'s time convention
// (`time_to_expiry_years`). This is NOT `n_trading_days / 252.0`: the
// resulting T reflects the actual number of calendar days the trading-day
// horizon spans (which varies with how many weekends/holidays it crosses),
// then whatever convention (Calendar365 or VolTime) `spec` selects.
//
// Returns `Result<double>` to PROPAGATE both fail-closed coverage errors: the
// long end of the grid is 504 trading days (~2 years), so a snapshot anchored
// late in the default calendar's 2024-2032 window resolves an expiry beyond it.
// Swallowing that into a NaN would poison the censored-term fit downstream with
// no signal. Note this bites on EVERY convention, `Calendar365` included: the
// year-fraction convention reads no calendar, but the expiry INSTANT it is
// applied to is produced by `advance_trading_days`, whose day-stepping needs the
// closure table. Only `time_to_expiry_years`'s own error is convention-specific.
//
// @param now_ns          evaluation instant, epoch nanoseconds (UTC)
// @param n_trading_days  tenor horizon, NYSE trading days (>= 0)
// @param spec            governing time convention (default: Calendar365)
// @return                year-fraction to the tenor's expiry instant;
//                        `ErrorCode::OutOfRange` when the trading-day walk (any
//                        convention) or the `VolTime` clock leaves the default
//                        calendar's covered window
[[nodiscard]] Result<double> tenor_years(std::int64_t now_ns, int n_trading_days,
                                         const TimeSpec& spec);

}  // namespace atx::vol
