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
// ## Coverage window (fail-closed, plan item 1.10)
//
// A closure table is only meaningful over the span it was actually populated
// for, so a calendar CARRIES that span: `[first_covered_day(),
// last_covered_day()]`, inclusive, in the same days-since-epoch numbering.
// Outside it the closure set is simply unknown — and "unknown" must never be
// read as "open". `trading_hours_between` therefore FAILS CLOSED
// (`ErrorCode::OutOfRange`) as soon as an uncovered day would accrue trading
// time, and `vol_time_years` / `time_to_expiry_years` propagate that error;
// the alternative is a silent full-session credit for every uncovered closure
// (Memorial Day 2020 accrued a full 7.5h session before this guard existed),
// which corrupts every vol-time number derived from it without a single
// visible symptom.
//
// The test is "would this day contribute", not "does the interval lie inside
// the window", and it is applied per-day at the accrual site. That is what
// keeps it correct for a caller-supplied session window of ANY width: the
// session loop walks one padding day beyond each end of the interval, and an
// extended-hours `VolTimeParams` whose close crosses a UTC midnight makes such
// a padding day genuinely accrue. A window-vs-interval precheck would wave
// those two days through.
//
// `us_default()`'s window is exactly 2024-01-01 .. 2028-12-31, the span its
// table enumerates. It is deliberately not wider: the pre-2007 US DST rule
// differs from the modern one this module implements, so even a hypothetical
// "no closures" answer would be wrong for early dates. Widening the window is
// a change to the table, not to a caller.
//
// KNOWN DATED CLIFF from that upper bound. A long tenor measured off a recent
// snapshot resolves past 2028-12-31 well before 2028 arrives, and the caller
// then gets `OutOfRange` for the whole request rather than a partial answer.
// The concrete one in-tree: the earnings-repro pipeline's 504-trading-day SR
// tenor (~2 years), which under the default VolTime convention starts failing
// for snapshots dated after roughly 2027-01 — see the DATED CLIFF note on
// `EarningsReproConfig::time` (earnings_repro_config.hpp) for the two ways
// past it. Extending the table before 2027 is the durable fix.
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

#include "atx/vol/types.hpp"  // Result, ErrorCode (fail-closed coverage window)

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
// number itself), PLUS the inclusive day window that set is complete over.
// `us_default()` carries the NYSE full-closure table for 2024-2028 inclusive
// and declares exactly that window.
class VolTimeCalendar {
 public:
  // Sorts and de-duplicates `holiday_days` (order/duplicates in the input are
  // not significant).
  //
  // The window is a REQUIRED constructor argument, not an inferred one: a
  // table's completeness is knowledge only its author has (an empty holiday
  // list over a covered week and an unpopulated table are indistinguishable
  // from the data alone), and a calendar that cannot say what it covers is
  // precisely the defect this window exists to close.
  //
  // @param holiday_days      full-closure days-since-epoch (any order)
  // @param first_covered_day first day the table is complete over (inclusive)
  // @param last_covered_day  last day the table is complete over (inclusive);
  //                          an empty window (`last < first`) covers nothing
  //                          and makes every vol-time query fail closed
  explicit VolTimeCalendar(std::vector<std::int32_t> holiday_days,
                           std::int32_t first_covered_day, std::int32_t last_covered_day);

  // True iff `day_since_epoch` is a LISTED full closure.
  //
  // CAVEAT — `false` is NOT "this day is open". It is "this day is not in the
  // table", which is also the answer for every day OUTSIDE `covers()`, where the
  // table says nothing at all. Any caller whose result MOVES with that answer
  // (accruing session time, counting a trading day, stepping a tenor) must gate
  // it on `covers(day_since_epoch)` first and fail closed when the day is not
  // covered — see `trading_hours_between` (accrual site) and
  // `advance_trading_days` (sr_tenor_grid.hpp, counting site). Reading an
  // out-of-window `false` as "open" is exactly how Memorial Day 2020 accrued a
  // full 7.5h session (plan item 1.10) and how the trading-day stepper landed
  // tenors on real NYSE closures.
  [[nodiscard]] bool is_holiday(std::int32_t day_since_epoch) const noexcept;

  // Inclusive bounds of the window this calendar's closure set is complete
  // over (days-since-epoch, same numbering as `is_holiday`).
  [[nodiscard]] std::int32_t first_covered_day() const noexcept;
  [[nodiscard]] std::int32_t last_covered_day() const noexcept;

  // True iff `day_since_epoch` lies inside the covered window, i.e. iff
  // `is_holiday(day_since_epoch)` is a KNOWN answer rather than a default-false
  // one. A `false` here is what makes the vol-time entry points fail closed.
  [[nodiscard]] bool covers(std::int32_t day_since_epoch) const noexcept;

  // NYSE full-closure calendar, 2024-2028 inclusive (that is also its covered
  // window). Built once (function-local static) on first call.
  [[nodiscard]] static const VolTimeCalendar& us_default();

 private:
  std::vector<std::int32_t> days_;  // sorted, unique
  std::int32_t first_covered_day_;
  std::int32_t last_covered_day_;
};

// Day-of-week for a proleptic-Gregorian days-since-epoch index (Howard
// Hinnant's `weekday_from_days` identity, same numbering as
// `VolTimeCalendar`'s `day_since_epoch`): 1970-01-01 = day 0 = Thursday.
// Promoted to the public `atx::vol` surface (out of vol_time.cpp's anonymous
// namespace) so other TUs needing the same weekday primitive (e.g.
// sr_tenor_grid.cpp's trading-day stepper) can call it directly instead of
// re-deriving it.
//
// @param day_since_epoch  days-since-epoch (1970-01-01 = 0)
// @return                 weekday index, 0=Sunday .. 6=Saturday
[[nodiscard]] int weekday_from_days(std::int32_t day_since_epoch) noexcept;

// True if `day_since_epoch` falls on a Saturday or Sunday (see
// `weekday_from_days` for the day-index convention).
[[nodiscard]] bool is_weekend_day(std::int32_t day_since_epoch) noexcept;

// ── Option settlement instants ──────────────────────────────────────────────
//
// Which intraday wall-clock instant a listed option's expiry lands on. The
// entire US single-name / ETF equity-option universe is PM-settled (16:00 ET,
// the regular-session close); a handful of cash-settled index series (e.g. the
// SPX/NDX "AM" specials) settle on the 09:30 ET opening print. Default PM.
enum class SettlementSession : std::uint8_t {
  Pm = 0,  // 16:00 ET regular-session close — the equity/ETF default
  Am = 1,  // 09:30 ET opening print — AM-settled cash index series
};

// UTC epoch-ns of the settlement instant for an option expiring on ET calendar
// day `et_day_since_epoch` (days-since-epoch, the same civil-day numbering as
// `VolTimeCalendar` / `weekday_from_days`): 16:00 ET for `SettlementSession::Pm`,
// 09:30 ET for `SettlementSession::Am`. ET->UTC uses the modern (2007+) DST rule
// (EDT = UTC-4 / EST = UTC-5), so the SAME 16:00 ET expiry is 20:00Z in summer
// and 21:00Z in winter — the reason a midnight-UTC expiry parse mis-states front
// T by ~0.8 trading day. Half-day early closes are NOT modelled (the instant is
// the nominal 16:00/09:30 ET regardless), consistent with `VolTimeCalendar`,
// which likewise carries no early-close table.
[[nodiscard]] std::int64_t settlement_instant_ns(std::int32_t et_day_since_epoch,
                                                 SettlementSession settle) noexcept;

// Trading hours (fractional) accrued in `[start_ns, end_ns)`, under the ET
// session window in `p`, skipping weekends and `cal` holidays. Returns 0 if
// `end_ns <= start_ns` (a degenerate interval reads no calendar day at all, so
// it is answerable regardless of coverage).
//
// FAILS CLOSED with `ErrorCode::OutOfRange` as soon as a day that would accrue
// trading time falls outside `cal`'s covered window. The check sits at the
// ACCRUAL site, so the coverage-checked set is exactly the contributing set for
// any `p` — including an extended-hours session window whose close spills past
// a UTC midnight and therefore makes a neighbouring day contribute. Days that
// contribute nothing (weekends, listed closures, and days the interval simply
// does not overlap) are never checked: their status cannot move the answer.
//
// Also fails closed (`ErrorCode::OutOfRange`) on an interval spanning more than
// ~20 years — the day loop's static bound. Nothing this clock serves prices a
// horizon that long, and the loop used to CLAMP there and answer anyway, which
// reported every session past year ~20 as non-trading time.
//
// @param start_ns  interval start, epoch nanoseconds (UTC)
// @param end_ns    interval end, epoch nanoseconds (UTC), exclusive
// @param p         session-window parameters (open hour / span, ET)
// @param cal       holiday calendar (supplies the covered window)
// @return          fractional trading hours in [0, (end_ns-start_ns)/3600e9],
//                  or `ErrorCode::OutOfRange` outside `cal`'s window
[[nodiscard]] Result<double> trading_hours_between(std::int64_t start_ns, std::int64_t end_ns,
                                                   const VolTimeParams& p,
                                                   const VolTimeCalendar& cal);

// SpiderRock VolTimeCalc master formula: converts a wall-clock interval to
// volatility-time years. Total wall hours = (expiry_ns - now_ns)/3600e9;
// non-trading hours = total - trading (via `trading_hours_between`). Returns 0
// for `expiry_ns <= now_ns`, and propagates `trading_hours_between`'s
// out-of-coverage error otherwise.
//
// @param now_ns     evaluation instant, epoch nanoseconds (UTC)
// @param expiry_ns  option expiry instant, epoch nanoseconds (UTC)
// @param p          hybrid-clock parameters (alpha, annual hour budgets)
// @param cal        holiday calendar (supplies the covered window)
// @return           T_vol in years, >= 0, or `ErrorCode::OutOfRange` outside
//                   `cal`'s covered window
[[nodiscard]] Result<double> vol_time_years(std::int64_t now_ns, std::int64_t expiry_ns,
                                            const VolTimeParams& p, const VolTimeCalendar& cal);

// ── Production T convention ─────────────────────────────────────────────────

// Calendar-365 year length in nanoseconds (365.25 * 86400s) — THE definition of
// this constant in atx-vol, and now the only one. It used to be the "canonical"
// copy among four: `portfolio_pricer.hpp`'s `kNsPerYear` restated the same
// expression, and `rates_curve.cpp`'s `forward_div_corrected` and
// `dividend.cpp`'s `hybrid_forward_div_jacobian` each carried a bare
// `(1.0e9 * 365.25 * 86400.0)` literal. All three now name this one:
// `kNsPerYear` is an alias (its spelling is public API and stays), and the two
// literals are gone. `data.cpp`'s `year_fraction` already delegated to
// `time_to_expiry_years` (default `TimeSpec`) rather than re-deriving, so
// `Calendar365` and the legacy ISO-string `year_fraction` could never drift.
//
// The mirrors' two spellings rounded to the SAME double, so replacing them was
// bit-exact rather than merely near: 365.25 * 86400 = 31'557'600 and
// 1e9 * 365.25 = 3.6525e11 are each exactly representable, and their common
// product 3.15576e16 = 2^14 * 39447 * 48828125 is itself exactly representable,
// so neither association order rounds at all. The assert below keeps that true
// if the value is ever re-derived into something order-sensitive.
//
// KNOWN NAME/VALUE DISCREPANCY — INTENTIONALLY LEFT AS 365.25 (core-review
// finding 10, A9 item 2). The `Calendar365` name promises an ACT/365 year, but
// the value is the Julian 365.25-day year, so every default-clock maturity's T is
// ~0.07% short and IVs sit ~3 bp below an ACT/365 vendor. It is INTERNALLY
// self-consistent (inverse `ns_from_year_fraction` uses the same constant), so
// only external comparisons inherit the bias. It is NOT changed to 365.0 here
// because Calendar365 is the DEFAULT convention for the ENTIRE fit/serve/backtest/
// earnings pipeline: a 0.07% T shift repins hundreds of bit/tight-tolerance
// values (earnings-repro ATM vols, surface-archive CRCs, backtest PnL, fitted
// slices) — far beyond the ~dozen-pin budget the A9 task set for an in-batch
// change. Note what the unification above DID retire from that rationale: the
// old text also warned that changing this copy would desync it from the mirrors,
// creating a new internal inconsistency. That hazard is gone — a re-derivation is
// now a one-line change here that every consumer inherits. What still defers it
// is the repin cost alone, so the follow-up is a full-corpus repin sweep (or a
// rename to Julian365), tracked for the PM — not this cleanup batch.
inline constexpr double kCalendarYearNs = 365.25 * 86400.0 * 1.0e9;
static_assert(kCalendarYearNs == 1.0e9 * 365.25 * 86400.0,
              "calendar-year ns must not depend on multiplication order — the "
              "mirrors this replaced used the other spelling");

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
// result for the same instant pair (same expression, same constant). This
// branch reads no calendar and is INFALLIBLE — it never returns an error, so
// every default-`TimeSpec` caller is unconditionally in the success arm.
// `spec.convention == VolTime`: returns `vol_time_years(from_ns, to_ns,
// spec.vol_time, VolTimeCalendar::us_default())`, INCLUDING its
// `ErrorCode::OutOfRange` failure outside the 2024-2028 covered window.
//
// @param from_ns  evaluation instant, epoch nanoseconds (UTC)
// @param to_ns    maturity instant, epoch nanoseconds (UTC)
// @param spec     governing time convention
// @return         year-fraction (Calendar365: may be negative for to_ns <
//                 from_ns, matching `year_fraction`; VolTime: >= 0, 0 for
//                 to_ns <= from_ns, matching `vol_time_years`), or
//                 `ErrorCode::OutOfRange` on the VolTime branch outside the
//                 default calendar's covered window
[[nodiscard]] Result<double> time_to_expiry_years(std::int64_t from_ns, std::int64_t to_ns,
                                                  const TimeSpec& spec);

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
// rounding: the intermediate values sit at ~6e16 magnitude (ULP ~8ns), so
// the few roundings involved land within tens of nanoseconds for
// multi-year horizons -- well under a microsecond, immaterial at any
// realistic event-schedule granularity (events land on day boundaries).
//
// @param from_ns  valuation instant, epoch nanoseconds (UTC)
// @param years    Calendar365 year-fraction from `from_ns`
// @return         `from_ns + round(years * kCalendarYearNs)`
[[nodiscard]] std::int64_t ns_from_year_fraction(std::int64_t from_ns,
                                                 double years) noexcept;

}  // namespace atx::vol
