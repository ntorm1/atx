#pragma once

// SpiderRock-style hybrid volatility-time clock (VolTimeCalc).
//
// atx-vol prices maturities as a plain calendar year-fraction (365.25d)
// everywhere today. SpiderRock's production surfaces instead price in a
// *hybrid* volatility-time clock: variance accrues at one rate while the
// market is open (trading hours) and a different, much slower rate the rest
// of the week (overnight/weekend/holiday hours). This header is the standalone
// clock — pure functions, no coupling to `projection.hpp`'s `TimeModel`/
// `TimeMode` (still `Clock`-only in v1); wiring it in as a new `TimeMode` is a
// follow-up task.
//
// ## Production T convention (`TimeConvention` / `TimeSpec`)
//
// `TimeSpec` is the opt-in carrier threaded down the chain/fit/serve path:
// `TimeConvention::Calendar365` (the default) reproduces `data.cpp`'s
// `year_fraction` exactly; `TimeConvention::VolTime` routes the SAME (from,
// to) instant pair through `vol_time_years` above (with `VolTimeCalendar::
// us_default()` — v1 does not support a custom calendar). `time_to_expiry_years`
// is the single conversion entry point production code calls instead of
// `year_fraction`/hand-rolled ns math, so a caller need not branch on the
// convention itself. Its default-`TimeSpec` path is BIT-IDENTICAL to
// `year_fraction`'s calendar-365 formula (both share `kCalendarYearNs` below —
// `year_fraction` itself delegates to `time_to_expiry_years` internally, so
// there is exactly one copy of the constant/expression in the codebase).
//
// ## Model (SpiderRock VolTimeCalc, verbatim)
//
//   Annual trading hours   = 1890  (252 trading days x 7.5h: the 09:30-16:00 ET
//                             RTH session PLUS the hour after close).
//   Annual non-trading hours = 6870  (8760 - 1890).
//   alpha = fraction of total annual variance attributed to trading time
//           (default 0.7).
//
//   T_vol = TradingHoursRemaining * alpha/1890
//         + NonTradingHoursRemaining * (1-alpha)/6870
//
// Sanity identity: a full RTH session (7.5h, alpha weight) contributes
// 7.5*alpha/1890 years; at alpha=1 a full trading day is therefore exactly
// 1/252, matching the familiar "252 trading days a year" convention.
//
// ## Calendar
//
// `VolTimeCalendar` is an immutable, sorted set of NYSE full-closure dates
// (days-since-epoch, plain proleptic-Gregorian civil-date numbering — the
// same numbering `days_from_civil`/`civil_from_days` produce, independent of
// any timezone). `us_default()` carries the exact NYSE full-closure table for
// 2024-2028 inclusive (weekend-observance shifts already applied, e.g. the
// 2026 Independence Day closure lands on Friday 07-03, the 2027 one on Monday
// 07-05). It does NOT model early closes (half-days) or ad-hoc emergency
// closures — those would need a data-override extension.
//
// ## Session window / DST
//
// The regular session is `[session_open_hour_et, session_open_hour_et +
// session_span_hours)` in US/Eastern wall-clock time (default 09:30-17:00 ET:
// the 09:30-16:00 RTH session plus VolTimeCalc's "hour after close"). US
// Eastern civil-to-UTC conversion uses the modern (2007+) DST rule: EDT
// (UTC-4) from the second Sunday of March 02:00 through the first Sunday of
// November 02:00, else EST (UTC-5). Both transition instants fall on Sundays
// (non-trading days), so resolving the DST offset at calendar-day granularity
// is exact for every session boundary this module computes.
//
// ## Thread-safety
//
// `VolTimeCalendar` is an immutable value once constructed (its `us_default()`
// singleton is built once, on first use, via a function-local static — safe
// under concurrent first calls). `trading_hours_between` and `vol_time_years`
// are pure functions of their arguments (no shared state) — safe to call
// concurrently from any number of threads.

#include <cstdint>
#include <vector>

namespace atx::vol {

// Tunable knobs for the hybrid clock. Defaults are SpiderRock VolTimeCalc's
// published constants.
struct VolTimeParams {
  double alpha{0.7};                    // variance fraction in trading hours, in [0,1]
  double trading_hours_per_year{1890.0};
  double nontrading_hours_per_year{6870.0};
  double session_open_hour_et{9.5};     // 09:30 ET
  double session_span_hours{7.5};       // 09:30-17:00 ET (RTH + 1h post-close)

  [[nodiscard]] bool operator==(const VolTimeParams&) const = default;
};

// Immutable named-holiday calendar: a sorted set of full-closure civil dates,
// each expressed as days-since-epoch (1970-01-01 = 0) under plain
// proleptic-Gregorian civil-date numbering (no timezone attached to the day
// number itself). `us_default()` carries the NYSE full-closure table for
// 2024-2028 inclusive.
class VolTimeCalendar {
 public:
  // Sorts and de-duplicates `holiday_days` (order/duplicates in the input are
  // not significant).
  explicit VolTimeCalendar(std::vector<std::int32_t> holiday_days);

  [[nodiscard]] bool is_holiday(std::int32_t day_since_epoch) const noexcept;

  // NYSE full-closure calendar, 2024-2028 inclusive. Built once (function-local
  // static) on first call.
  [[nodiscard]] static const VolTimeCalendar& us_default();

 private:
  std::vector<std::int32_t> days_;  // sorted, unique
};

// Trading hours (fractional) accrued in `[start_ns, end_ns)`, under the ET
// session window in `p`, skipping weekends and `cal` holidays. Returns 0 if
// `end_ns <= start_ns`.
//
// @param start_ns  interval start, epoch nanoseconds (UTC)
// @param end_ns    interval end, epoch nanoseconds (UTC), exclusive
// @param p         session-window parameters (open hour / span, ET)
// @param cal       holiday calendar
// @return          fractional trading hours in [0, (end_ns-start_ns)/3600e9]
[[nodiscard]] double trading_hours_between(std::int64_t start_ns, std::int64_t end_ns,
                                           const VolTimeParams& p,
                                           const VolTimeCalendar& cal) noexcept;

// SpiderRock VolTimeCalc master formula: converts a wall-clock interval to
// volatility-time years. Total wall hours = (expiry_ns - now_ns)/3600e9;
// non-trading hours = total - trading (via `trading_hours_between`). Returns 0
// for `expiry_ns <= now_ns`.
//
// @param now_ns     evaluation instant, epoch nanoseconds (UTC)
// @param expiry_ns  option expiry instant, epoch nanoseconds (UTC)
// @param p          hybrid-clock parameters (alpha, annual hour budgets)
// @param cal        holiday calendar
// @return           T_vol in years, >= 0
[[nodiscard]] double vol_time_years(std::int64_t now_ns, std::int64_t expiry_ns,
                                    const VolTimeParams& p,
                                    const VolTimeCalendar& cal) noexcept;

// ── Production T convention ─────────────────────────────────────────────────

// Calendar-365 year length in nanoseconds (365.25 * 86400s) — the SOLE copy of
// this constant in atx-vol. `data.cpp`'s `year_fraction` delegates to
// `time_to_expiry_years` (default `TimeSpec`) rather than re-deriving it, so
// `Calendar365` and the legacy ISO-string `year_fraction` can never drift
// apart.
inline constexpr double kCalendarYearNs = 365.25 * 86400.0 * 1.0e9;

// Which clock governs a maturity's year-fraction.
enum class TimeConvention : std::uint8_t {
  Calendar365 = 0, // (to - from) / 365.25y — current behavior, DEFAULT.
  VolTime = 1,     // SpiderRock hybrid clock: vol_time_years(from, to, params, cal).
};

// Carrier threaded from production config (panel-builder options /
// `SessionInputs`) down to chain construction. Default-constructed = the
// historical Calendar365 behavior, bit-identical to `year_fraction`.
// Equality-comparable so seams that receive a TimeSpec from two directions
// (e.g. `VolaSession::from_frame`: `SessionInputs::time` vs the frame's own
// `QuoteFrame::time`) can fail loudly on a mixed-convention handoff.
struct TimeSpec {
  TimeConvention convention{TimeConvention::Calendar365};
  VolTimeParams vol_time{};  // used only when convention == VolTime
  // Calendar: VolTimeCalendar::us_default() in v1; a field reserved for a
  // caller-supplied table is a follow-up (VolTimeCalendar is move-only-free
  // but not trivially copyable into a value member without extra plumbing).

  [[nodiscard]] bool operator==(const TimeSpec&) const = default;
};

// Single conversion entry point for a maturity's year-fraction — replaces
// direct `year_fraction`/hand-rolled ns-math call sites on the production
// fit/serve path so a caller need not branch on `spec.convention` itself.
//
// `spec.convention == Calendar365` (the default): returns
// `(to_ns - from_ns) / kCalendarYearNs`, BIT-IDENTICAL to `year_fraction`'s
// result for the same instant pair (same expression, same constant).
// `spec.convention == VolTime`: returns `vol_time_years(from_ns, to_ns,
// spec.vol_time, VolTimeCalendar::us_default())`.
//
// @param from_ns  evaluation instant, epoch nanoseconds (UTC)
// @param to_ns    maturity instant, epoch nanoseconds (UTC)
// @param spec     governing time convention
// @return         year-fraction (Calendar365: may be negative for to_ns <
//                 from_ns, matching `year_fraction`; VolTime: >= 0, 0 for
//                 to_ns <= from_ns, matching `vol_time_years`)
[[nodiscard]] double time_to_expiry_years(std::int64_t from_ns, std::int64_t to_ns,
                                          const TimeSpec& spec) noexcept;

// Inverse of `time_to_expiry_years` under the DEFAULT Calendar365 convention
// only: reconstructs an absolute instant `from_ns + round(years *
// kCalendarYearNs)`. For a caller that only has a year-fraction (T,
// Calendar365) and needs an absolute epoch instant to compare against an
// absolute-timestamp source it has no other link to (e.g. an earnings-event
// schedule keyed on real listed-expiry timestamps that the T itself was
// computed from, but did not retain) -- see atx/vol/event_vol.hpp's
// `EventSchedule::count_between`, used by both `w_on_inserted_slice`
// (projection.hpp, an arbitrary interpolated query T has no real listed
// expiry to read one from) and `VolaSession::build`'s eMove solve
// (session.cpp, whose fitted eSSVI slices do not currently retain their own
// `expiry_ns` -- see EssviParams::expiry_ns). Round-trips a real listed
// expiry's own `time_to_expiry_years` output to within double-precision
// rounding (sub-nanosecond -- immaterial at any realistic event-schedule
// granularity).
//
// @param from_ns  valuation instant, epoch nanoseconds (UTC)
// @param years    Calendar365 year-fraction from `from_ns`
// @return         `from_ns + round(years * kCalendarYearNs)`
[[nodiscard]] std::int64_t ns_from_year_fraction(std::int64_t from_ns,
                                                 double years) noexcept;

}  // namespace atx::vol
