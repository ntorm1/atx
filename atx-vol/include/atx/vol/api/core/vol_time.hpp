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
// ## Model (SpiderRock VolTimeCalc, constants FITTED to the vendor's own
// ## published `years` DATA — not transcribed from their docs; see below)
//
//   Annual trading hours     = 1638  (252 sessions x 6.5h: the 09:30-16:00 ET
//                               regular session, and nothing beyond it).
//   Annual non-trading hours = 7122  (8760 - 1638).
//   alpha = fraction of total annual variance attributed to trading time
//           (0.7).
//
//   T_vol = TradingHoursRemaining * alpha/1638
//         + NonTradingHoursRemaining * (1-alpha)/7122
//
// Sanity identity: a full RTH session (6.5h, alpha weight) contributes
// 6.5*alpha/1638 years; at alpha=1 a full trading day is therefore exactly
// 1/252 — because 1638 IS 252 x 6.5, so the identity is definitional, not a
// coincidence to be read as confirmation of any particular session width.
//
// ## Why 1638/7122: the vendor's PROSE and the vendor's DATA disagree
//
// Read this before changing a constant here, because the honest framing is
// narrower than "we found the right doc page", and an earlier version of this
// header (and of `docs/plans/2026-08-17-oracle-v2-goal-prompt.md`) got it
// wrong in BOTH directions in turn.
//
// WHAT THE DOCS SAY — 1890/6870, essentially unanimously. A full crawl of
// docs.spiderrockconnect.com (1,439 pages, all three published versions)
// found `1,890` and `6,870` stated on BOTH the VolTimeCalc and OptionPricing
// pages, in every published version, ~13 and ~10 occurrences respectively.
// `1,638` and `7122`/`7,122` appear ZERO times anywhere in that documentation.
// The single `1638` is one prose sentence on VolTimeCalc, and it contradicts
// the LaTeX equation printed directly beneath it
// (`1,890 x 1/8,760 + 6,870 x 1/8,760 = 1`) — note also that 1638 + 6870 =
// 8508, not 8760. Their V7->V8 migration page independently documents the
// trading window being EXTENDED from 6.5h (08:30-15:00 CT) to 7.5h
// (08:30-16:00 CT = 09:30-17:00 ET), with PM expiry moving to 17:00 ET.
// So 1890/6870/7.5h is the documented convention, and this header does NOT
// claim their docs contain a typo.
//
// WHAT THE DATA SAYS — 1638/7122/6.5h. Source: the vendor's own `years`
// column in the licensed oracle store,
// `C:\atx-cache\oracle\spiderrock\date=2026-08-14`, 74 expiries x 19 intraday
// buckets. We match the DATA, because that is what we are reproducing.
//
// THE MOST LIKELY EXPLANATION, and it is a reading, not a fact: SpiderRock's
// `MsgVolTimeCalculator` still exposes a `timeMetric` enum including `SRV6`,
// the legacy alpha/6.5-hour convention, and 1638 = 252 x 6.5 is exactly that
// V7 constant. The `years` column in `tblOptionIntradayHist` is plausibly
// still produced on the V7-flavoured clock while the docs describe V8 — which
// would also make the stray `1638` in their prose a remnant of the same
// convention our data matches, rather than a typo.
//
// The five readings of the data, in the order that matters:
//
//  1. Differencing `years` across adjacent expiries yields two exact
//     per-day constants: 0.003514930 yr per TRADING day and 0.001010952 yr per
//     NON-TRADING day. They normalise:
//     252*0.003514930 + 113*0.001010952 = 1.00002.
//
//  2. Solving those two increments for hourly rates gives alpha/1638 per
//     trading hour and (1-alpha)/7122 per non-trading hour at alpha = 0.7:
//     6.5*(0.7/1638) + 17.5*(0.3/7122) = 0.0035149303 (trading day) and
//     24*(0.3/7122) = 0.0010109520 (non-trading day).
//
//  3. THE DISCRIMINATING EVIDENCE, because (1) and (2) alone do NOT identify
//     the split: a 7.5h day with alpha = 0.710606 reproduces BOTH day
//     increments to the same precision (7.5*0.710606/1890 + 16.5*0.289394/6870
//     = 0.0035149, 24*0.289394/6870 = 0.0010110). Day-level arithmetic is
//     degenerate in (alpha, session width); only an INTRADAY slope breaks it.
//     Regressing `years` for a FIXED expiry against clock time across the 19
//     in-session buckets — no expiry arithmetic, no holiday calendar — gives
//     d(T)/d(trading hour) = 4.27088e-04. Against that: 0.7/1638 = 4.27350e-04
//     (ratio 0.9994), 0.7/1890 = 3.70370e-04 (ratio 1.1531), and the
//     degenerate 0.710606/1890 = 3.75982e-04 (ratio 1.1359). Two separate
//     expiries (2027-03-19 and 2026-09-18) give the identical slope to 5
//     digits, so it is the clock's, not one expiry's. Note that this rules out
//     the DOCUMENTED convention too, not just the degenerate alternative.
//
//  4. ANNUAL NORMALISATION, which needs no docs and no slope comparison. With
//     the measured hourly rates a = 4.27088e-4 (trading) and b = 4.21230e-5
//     (non-trading), a 6.5h session gives
//     252*(6.5a + 17.5b) + 113*24b = 0.99957 — a year. The same sum under a
//     7.5h session (a 7.5/16.5 split) is 1.09658. Only the 6.5h split
//     normalises.
//
//  5. THE WEEKEND INCREMENT rules out 7.5h independently of the intraday
//     slope. Fix a = 4.27088e-4 and solve the per-trading-day increment
//     0.003514930 for b: under a 7.5h day that forces b = 1.8895e-5, hence a
//     non-trading DAY of 24b = 4.535e-4 — against 0.001010952 measured, off by
//     more than a factor of two. Under a 6.5h day it gives b = 4.2221e-5,
//     hence 0.00101331 vs 0.001010952 measured (0.23%).
//
//  6. Cross-bucket residual, as a second opinion on the same data: with
//     1638/7122 the implied anchor lead is 0.94 min (range 0.90-2.82) and the
//     median per-bucket offset is 6.7e-6 yr; with 1890/6870 the lead ranges
//     -9.09 to +48.60 min and the offset is 1.1e-4 yr. Pooled median
//     |relative error| 0.105% vs 0.181%.
//
// WHAT THIS HEADER CLAIMS, EXACTLY. Not that SpiderRock "uses" these numbers
// in production — their docs present alpha = 0.7 as an illustration and never
// state a production constant, and we cannot see their engine. Only this:
// these constants reproduce the vendor's published `years` column FOR TRADE
// DATE 2026-08-14 to 0.06% on the intraday slope and 0.1% pooled. That is one
// trade date. A convention change on their side would not announce itself, so
// re-measure before trusting this over a materially later store.
//
// Consequence for the session window: 1638 = 252 x 6.5 leaves no room for a
// post-close carve-out, so `session_span_hours` is 6.5 and the session closes
// at 16:00 ET — the regular-session close, which is also where PM settlement
// lands (`settlement_instant_ns`). The vendor's prose about variance accruing
// "additionally during the hour immediately after market close" describes
// where variance comes from; whatever produced the `years` column we measured
// does not put that hour in its trading-hour budget.
//
// ## Calendar
//
// `VolTimeCalendar` is an immutable, sorted set of NYSE full-closure dates
// (days-since-epoch, plain proleptic-Gregorian civil-date numbering — the
// same numbering `days_from_civil`/`civil_from_days` produce, independent of
// any timezone). `us_default()` covers 2024-01-01 .. 2032-12-31, in two halves
// that are two DIFFERENT KINDS OF FACT and are deliberately not conflated:
//
//   2024-2028 — OBSERVED. Transcribed from the calendars NYSE has actually
//     published (the exchange's own site listed 2026/2027/2028 and nothing
//     further when this was written). Weekend-observance shifts are already
//     applied, e.g. the 2026 Independence Day closure lands on Friday 07-03 and
//     the 2027 one on Monday 07-05. This half also carries the AD-HOC closures
//     of those years — 2025-01-09, the National Day of Mourning for Jimmy
//     Carter — which is precisely why it stays a literal table.
//
//   2029-2032 — RULE-PROJECTED. Computed by `nyse_rule_based_closures` (see its
//     contract) from the ten computable holidays plus the exchange's weekend
//     shifts. NOTHING here is transcribed, because there is nothing to
//     transcribe: NYSE has not published these years. So any AD-HOC closure
//     that lands in them is ABSENT, and this calendar will credit such a day a
//     full trading session until someone adds it. That is the projection's
//     stated cost, and it is the reason the horizon stops at 2032 rather than
//     running on: the no-ad-hoc-closure assumption decays with distance.
//
// The same generator reproduces all five OBSERVED years exactly (modulo the
// un-guessable 2025-01-09), which is what makes the projection a checked rule
// rather than an assertion — see `VolTime.RuleProjectionReproducesTheObserved
// Table`. Neither half models early closes (half-days); those would need a
// data-override extension.
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
// (Memorial Day 2020 accrued a full trading session before this guard existed),
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
// `us_default()`'s window is exactly 2024-01-01 .. 2032-12-31, the span its
// closure set is populated over (2024-2028 observed, 2029-2032 projected —
// see the Calendar section above). It is deliberately not wider at EITHER end.
// Backwards: the pre-2007 US DST rule differs from the modern one this module
// implements, so even a hypothetical "no closures" answer would be wrong for
// early dates. Forwards: the projection assumes no ad-hoc closure occurs, and
// that assumption is worth four years, not forty. Moving either bound is a
// change to `us_default()`'s construction, not to a caller.
//
// PAST 2032-12-31 THE FAIL-CLOSED BEHAVIOUR IS UNCHANGED, and that is load-
// bearing rather than incidental. In particular a far-dated non-contract — the
// 2099-01-01 sentinel expiry the production option store carries — still
// returns `OutOfRange` (indeed it is refused twice over: past the window AND
// past `kMaxLoopDays`). Extending the projection to swallow it would mean
// guessing seventy years of closures to make a row that is not an option parse,
// which is the exact silent guess this module exists to refuse. What changed
// for that sentinel is the BLAST RADIUS, not the verdict: the OPRA loader now
// drops the uncoverable expiry and counts it (`OpraPanel::n_dropped_uncovered_
// expiry`) instead of failing the whole symbol.
//
// The DATED CLIFF this note used to describe is pushed out, not removed. A long
// tenor measured off a recent snapshot still resolves past the window's end
// eventually. The concrete one in-tree: the earnings-repro pipeline's
// 504-trading-day SR tenor (~2 years), which under the default VolTime
// convention now starts failing for snapshots dated after roughly 2031-01
// rather than 2027-01 — see the DATED CLIFF note on `EarningsReproConfig::time`
// (earnings_repro_config.hpp) for the two ways past it. Raising
// `kUsDefaultProjectedThroughYear` (vol_time.cpp) before 2031 is the durable
// fix, and it is now a one-line change with no new data.
//
// ## Session window / DST
//
// The regular session is `[session_open_hour_et, session_open_hour_et +
// session_span_hours)` in US/Eastern wall-clock time (default 09:30-16:00 ET —
// the regular session, with no post-close carve-out: see the measurement at
// the top of this header for why the budget admits none). US
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

#include "atx/vol/api/core/types.hpp"  // Result, ErrorCode (fail-closed coverage window)

namespace atx::vol {

// Tunable knobs for the hybrid clock. Defaults are FITTED to SpiderRock's
// published `years` DATA (trade date 2026-08-14), not transcribed from their
// documentation — which states 1890/6870/7.5h and disagrees with its own data.
// The full derivation, the doc-crawl evidence, and the exact scope of what
// this reproduces are at the top of this header. Read it before editing these.
struct VolTimeParams {
  double alpha{0.7};                    // variance fraction in trading hours, in [0,1]
  double trading_hours_per_year{1638.0};    // 252 x 6.5
  double nontrading_hours_per_year{7122.0}; // 8760 - 1638
  double session_open_hour_et{9.5};     // 09:30 ET
  double session_span_hours{6.5};       // 09:30-16:00 ET (the regular session)

  [[nodiscard]] bool operator==(const VolTimeParams&) const = default;
};

// Immutable named-holiday calendar: a sorted set of full-closure civil dates,
// each expressed as days-since-epoch (1970-01-01 = 0) under plain
// proleptic-Gregorian civil-date numbering (no timezone attached to the day
// number itself), PLUS the inclusive day window that set is complete over.
// `us_default()` carries the NYSE full-closure set for 2024-2032 inclusive
// (2024-2028 observed, 2029-2032 rule-projected) and declares exactly that
// window.
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
  // full trading session (plan item 1.10) and how the trading-day stepper landed
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

  // NYSE full-closure calendar, 2024-2032 inclusive (that is also its covered
  // window): 2024-2028 OBSERVED from published NYSE calendars, 2029-2032
  // RULE-PROJECTED — see the Calendar section at the top of this header for
  // what the projection can and cannot know. Built once (function-local static)
  // on first call.
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

// The RULE-BASED half of the NYSE closure calendar for one calendar year: the
// ten computable full closures, with the exchange's weekend-observance shifts
// applied, as days-since-epoch (ascending, unique).
//
// WHAT THIS CAN AND CANNOT KNOW — the distinction the whole design turns on.
// NYSE full closures are two different kinds of fact:
//
//   * RULE-BASED, and therefore computable to any horizon: New Year's Day,
//     Martin Luther King Jr. Day (3rd Mon Jan), Washington's Birthday (3rd Mon
//     Feb), Good Friday (Gregorian Easter minus two days), Memorial Day (last
//     Mon May), Juneteenth (Jun 19), Independence Day (Jul 4), Labor Day (1st
//     Mon Sep), Thanksgiving (4th Thu Nov), Christmas (Dec 25).
//   * AD-HOC, and therefore not computable by anything: national days of
//     mourning (2025-01-09, Jimmy Carter), weather closures (Hurricane Sandy,
//     2012), 9/11. These can only ever be a table.
//
// This function returns ONLY the first kind. It never guesses the second, so a
// calendar built from it alone is complete only up to an ad-hoc closure nobody
// can foresee. `VolTimeCalendar::us_default()` therefore uses the OBSERVED
// table for the years NYSE has actually published and this projection only
// past them — see its doc comment for which years are which.
//
// Weekend observance, exactly as the exchange applies it: a fixed-date closure
// falling on a Saturday moves to the preceding Friday, one falling on a Sunday
// moves to the following Monday. The ONE exception is New Year's Day, which
// when it falls on a Saturday is not observed at all — NYSE's own calendar
// footnote, "Because the holiday falls on Saturday, January 1, 2028, no New
// Year's Day holiday is observed." Such a year yields NINE closures, not ten.
// The floating Monday/Thursday holidays and Good Friday can never land on a
// weekend and are never shifted.
//
// Juneteenth became an NYSE closure in 2022, so this projection is only valid
// from 2022 onward; `is_dst` (vol_time.cpp) implementing the modern (2007+) US
// DST rule is a second, independent reason not to run it backwards.
//
// This models FULL closures only. NYSE half-days (the 13:00 ET early closes
// around Independence Day, Thanksgiving and Christmas) are not modelled here,
// consistent with the rest of this module. Note in passing that the shift rule
// removes one: when Christmas moves to Friday 12-24 that day is a full closure,
// so such a year has no Christmas Eve early close at all.
//
// @param year  calendar year (proleptic Gregorian)
// @return      ascending, unique days-since-epoch of that year's rule-based
//              NYSE full closures (9 or 10 entries)
[[nodiscard]] std::vector<std::int32_t> nyse_rule_based_closures(std::int32_t year);

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
// `ErrorCode::OutOfRange` failure outside the 2024-2032 covered window.
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
