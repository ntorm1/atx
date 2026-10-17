#pragma once

// Discrete cash-dividend schedule inferred from a chain's per-expiry dividend
// aggregates.
//
// ## What the data carries
//
// A listed chain (one underlier, one deliverable, one valuation instant) often
// arrives with two dividend fields per expiry and no ex-dates:
//
//   dividend_sum  D_e = sum of cash dividends with ex-date in (valuation, expiry e]
//   dividend_pv   P_e = sum_i D_i * exp(-rate_e * days_i / day_basis)
//
// where `rate_e` is that expiry's own zero rate and `days_i` the whole calendar
// days from the valuation date to ex-date i (ACT/365 for the SpiderRock export:
// measured integer-day for the first event on 98.5% of 3,177 payers, docs/LEDGER.md
// 2026-10-16). D_e is a step function of the expiry; P_e encodes WHEN each step's
// cash is paid. So between two consecutive expiries whose sums differ, the
// increment is the cash paid inside that bracket and the PV increment pins its
// discount-weighted time:
//
//   inc_e  = D_e - D_prev
//   rest_e = P_e - sum_{known events} D_k * exp(-rate_e * days_k / day_basis)
//   one event:  days = -ln(rest_e / inc_e) * day_basis / rate_e
//
// ## Inference (three passes, allocation-free, deterministic)
//
//  1. Peel. Expiries are sorted by day; each increment above `amount_tolerance`
//     is solved as ONE event at its PV-implied time, with every earlier increment
//     represented by its own solved event (integer day when exact, fractional
//     otherwise). The solved time must lie in the bracket's calendar window
//     [prev_day + 1, day] (an ex-date ON the previous expiry date belongs to that
//     expiry), within `bracket_tolerance_days`; otherwise the ladder is
//     Inconsistent from that expiry on. A decreasing sum is Inconsistent too.
//  2. Count. The UNIT is the median amount of increments solved to an integer
//     day (|days - round(days)| <= day_tolerance). The CADENCE is the median
//     spacing between two exactly dated events whose increments are ADJACENT in
//     the ladder (only then are they consecutive dividends). Each increment
//     holds n = round(inc / unit) events, capped by how many fit in its window
//     at `cadence_spacing_fraction * cadence` (or `min_event_spacing_days` when
//     the cadence is unknown) and by `max_events_per_increment`. A window that
//     holds ONE event keeps the increment whole and exactly dated, so a special
//     dividend in a bracket narrower than about that spacing + 1 day is never
//     read as a run of regular ones. In a WIDER bracket the amount alone cannot
//     tell a special from a run: a special worth no more units than its window
//     holds apart IS split as a run (flagged SumSplit), while an increment
//     holding MORE units than its window holds apart (room for two or more)
//     cannot be a run and is kept as ONE event at its PV-exact time, flagged
//     NonIntegerDay — never split into fewer events than its units.
//  3. Place (THE DECLARED SPLIT RULE). An increment with n >= 2 is split into n
//     EQUAL amounts inc/n at EQUALLY SPACED times t_j = t_1 + (j-1)*h inside its
//     window. The spacing h is the cadence when known, else window width / n;
//     t_1 is solved in closed form so the n events reproduce the increment's PV
//     at that expiry's rate exactly, and h is shrunk by bisection when the
//     events would leave the window. Times stay fractional (PV-exact); every
//     split event is flagged SumSplit. An increment with n == 1 is ONE event,
//     its time re-solved against the FINAL events before it (pass 1 dated it
//     against provisional single-event stand-ins for earlier split increments)
//     and classified from that final time: Exact at the integer day when it is
//     one, else at its PV-exact time, flagged NonIntegerDay. Nothing is guessed
//     silently: both flagged kinds make the schedule Ambiguous and are counted.
//
// A final pass reprices every expiry's sum and PV from the finished schedule and
// reports the worst residual; a gross PV miss (> `pv_inconsistent_relative`)
// makes the ladder Inconsistent from that expiry on.
//
// ## Using the schedule
//
// A schedule is a property of the chain's valuation DATE. Two helpers convert
// it: `dividend_schedule_instants` emits `DividendEvent` instants for the shared
// escrow/lattice route (`discrete_div_route`, dividend.hpp), and
// `dividend_schedule_expiry_events` maps the events of one expiry onto THAT
// expiry's own carry clock for the V&N lattice (`CashDividend`, american.hpp):
//
//   tau_i = expiry_years * days_i / (expiry_day - valuation_day)
//
// i.e. the event keeps its fraction of the option's calendar life. That is a
// constant clock-speed assumption — the same one the quote slice already makes
// between its variance and calendar clocks — and is exact at expiry (an
// ex-date on the expiry date lands at tau == T).
//
// Thread-safety: pure functions of their arguments.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "atx/vol/api/pricing/american.hpp"    // CashDividend (the lattice's tau-space event)
#include "atx/vol/api/pricing/rates_curve.hpp" // DividendEvent (instant-space event)

namespace atx::vol {

// Bounded storage (JPL rule 2). A listed chain has well under 128 expiries
// (74 distinct expiry dates in the whole 2026-10-14 SpiderRock file), and 256
// events cover a weekly payer for five years.
inline constexpr std::size_t kDividendScheduleMaxExpiries = 128;
inline constexpr std::size_t kDividendScheduleMaxEvents = 256;
inline constexpr std::int64_t kDividendScheduleNsPerDay = 86'400'000'000'000;

// One expiry of ONE chain at ONE valuation instant. The caller establishes that
// identity; the module sees only the dividend ladder.
struct DividendLadderExpiry {
  // Calendar day number of the expiry date, on the same day numbering as the
  // valuation day passed to `infer_dividend_schedule` (e.g. days since
  // 1970-01-01). Must be strictly after the valuation day.
  std::int32_t expiry_day = 0;
  // The chain's carry clock to this expiry (> 0). Not used by the inference;
  // validated here so a ladder that cannot later be mapped onto its own clocks
  // is refused up front.
  double years = 0.0;
  // Continuously compounded zero rate the PV was discounted with (finite).
  double rate = 0.0;
  // Undiscounted cumulative cash dividends with ex-date in (valuation, expiry].
  double dividend_sum = 0.0;
  // Their present value at `rate` on the `day_basis` day count.
  double dividend_pv = 0.0;
};

struct DividendScheduleConfig {
  double day_basis = 365.0; // days per year in the PV identity (ACT/365)
  // Increments at or below this are flat (the export's ddiv grid is 1e-4); a
  // decrease beyond it is Inconsistent.
  double amount_tolerance = 5.0e-5;
  // |days - round(days)| at or below this is an exactly dated (integer) day.
  double day_tolerance = 0.05;
  // A PV-implied time may sit this far outside its window before the ladder is
  // Inconsistent; inside the tolerance it is clamped into the window.
  double bracket_tolerance_days = 1.0;
  // Final repricing: relative PV residual above `pv_relative_tolerance` is
  // counted; above `pv_inconsistent_relative` (and `pv_absolute_floor` in cash)
  // it makes the ladder Inconsistent from that expiry on.
  double pv_relative_tolerance = 1.0e-5;
  double pv_inconsistent_relative = 1.0e-3;
  double pv_absolute_floor = 1.0e-4;
  // Below this |rate| the PV carries no timing information and an increment
  // cannot be dated (Inconsistent, reason UntimedIncrement).
  double min_rate = 1.0e-6;
  // Count cap: events in one window are at least this fraction of the cadence
  // apart, or `min_event_spacing_days` apart when the cadence is unknown.
  double cadence_spacing_fraction = 0.5;
  double min_event_spacing_days = 7.0;
  std::size_t max_events_per_increment = 64;
};

enum class DividendScheduleStatus : std::uint8_t {
  // Every increment is one event at an integer day.
  Ok,
  // Every expiry carries a zero dividend sum: nothing to schedule.
  NoDividends,
  // Consistent, but at least one event is not exactly identified: a sum split by
  // the declared rule (SumSplit) or a single event at a non-integer PV-implied
  // day (NonIntegerDay). Per-event kinds say which.
  Ambiguous,
  // The ladder contradicts the PV identity from some expiry on. The events of
  // the consistent prefix are returned and `resolved_through_day` bounds them.
  Inconsistent,
  // Malformed input or configuration, or capacity exceeded. No events.
  Invalid
};

enum class DividendScheduleReason : std::uint8_t {
  None,
  // Invalid
  InvalidConfig,
  NonFiniteInput,
  NegativeInput,
  ExpiryNotAfterValuation,
  ExpiryBeyondHorizon, // more than 36,525 days (a century) after valuation
  DuplicateExpiryConflict,
  TooManyExpiries,
  TooManyEvents,
  // Inconsistent
  DecreasingSum,
  NonPositivePv,
  ExDateOutsideBracket,
  UntimedIncrement,
  RepricingMismatch
};

enum class DividendEventKind : std::uint8_t {
  // One event at an integer PV-implied day, its amount one unit.
  Exact,
  // One event at its PV-exact time that is not exactly identified: the time is
  // not an integer day, or the amount reads as several units that its window
  // could not hold apart (then the time may be integral; the amount is whole).
  NonIntegerDay,
  // One of n equal events the declared split rule placed (header, pass 3).
  SumSplit
};

[[nodiscard]] const char *to_string(DividendScheduleStatus value) noexcept;
[[nodiscard]] const char *to_string(DividendScheduleReason value) noexcept;
[[nodiscard]] const char *to_string(DividendEventKind value) noexcept;

struct DividendScheduleEvent {
  double amount = 0.0; // cash per share, > 0
  // Ex-time in calendar days after the valuation date: an integer for Exact
  // events, the PV-exact fractional time otherwise. Always in the increment's
  // window [previous expiry day + 1, expiry day] (relative days).
  double days = 0.0;
  // valuation_day + round(days): the calendar date the event is reported on.
  std::int32_t ex_day = 0;
  // Day of the first expiry whose sum includes this event (the bracket's top).
  std::int32_t increment_expiry_day = 0;
  // How many events the increment was split into (1 unless SumSplit).
  std::uint16_t increment_events = 1;
  DividendEventKind kind = DividendEventKind::Exact;
};

struct DividendScheduleResult {
  DividendScheduleStatus status = DividendScheduleStatus::Invalid;
  DividendScheduleReason reason = DividendScheduleReason::None;
  std::int32_t valuation_day = 0;
  // Unique expiries after sorting and merging identical duplicates.
  std::size_t expiries = 0;
  // Events are valid for every expiry with day <= resolved_through_day. Equal to
  // the last expiry's day unless Inconsistent (then the last consistent expiry,
  // or valuation_day when none is); valuation_day when Invalid.
  std::int32_t resolved_through_day = 0;
  // The expiry at which an Inconsistent/Invalid verdict was reached (0 if none).
  std::int32_t failed_expiry_day = 0;
  // Increments by how they were resolved.
  std::size_t increments = 0;
  std::size_t exact_increments = 0;
  std::size_t split_increments = 0;
  std::size_t non_integer_increments = 0;
  // Unit amount and cadence used by the count/placement passes (0 when unknown).
  double unit_amount = 0.0;
  double cadence_days = 0.0;
  // Final repricing over the resolved expiries.
  double max_pv_relative_residual = 0.0;
  double max_sum_abs_residual = 0.0;
  std::size_t pv_residual_exceedances = 0; // above pv_relative_tolerance
  std::size_t n_events = 0;
  std::array<DividendScheduleEvent, kDividendScheduleMaxEvents> events{};

  [[nodiscard]] std::span<const DividendScheduleEvent> view() const noexcept {
    return {events.data(), n_events};
  }
  // True when the schedule describes the dividends up to `expiry_day`.
  [[nodiscard]] bool covers(std::int32_t expiry_day) const noexcept;
};

// Infer the schedule of one chain.
//
// @param valuation_day calendar day number of the valuation date (the PV's day 0)
// @param expiries      the chain's expiries in any order; identical duplicates
//                      (same day and values) are merged, conflicting ones refused
// @return the schedule and its typed status; never throws, never allocates
[[nodiscard]] DividendScheduleResult
infer_dividend_schedule(std::int32_t valuation_day, std::span<const DividendLadderExpiry> expiries,
                        const DividendScheduleConfig &config = {}) noexcept;

enum class DividendExpiryCoverage : std::uint8_t {
  Covered,       // the schedule describes this expiry; `count` events lie in its life
  NotCovered,    // Invalid schedule, or expiry past `resolved_through_day`
  InvalidInput,  // expiry not after the valuation day, or non-finite/non-positive years
  BufferTooSmall // `out` cannot hold the events in this expiry's life
};

[[nodiscard]] const char *to_string(DividendExpiryCoverage value) noexcept;

struct DividendExpiryEvents {
  DividendExpiryCoverage coverage = DividendExpiryCoverage::NotCovered;
  std::size_t count = 0;     // events written to `out`
  std::size_t ambiguous = 0; // of which not Exact
  double cash = 0.0;         // undiscounted sum of their amounts
};

// The events with ex-date in (valuation, expiry_day], as lattice dividends on
// the expiry's own carry clock: tau = expiry_years * days / (expiry_day -
// valuation_day), in schedule order. An ex-date on the expiry date maps to
// exactly `expiry_years`.
[[nodiscard]] DividendExpiryEvents
dividend_schedule_expiry_events(const DividendScheduleResult &schedule, std::int32_t expiry_day,
                                double expiry_years, std::span<CashDividend> out) noexcept;

// Every event as an instant on the UTC day grid: ex_date_ns =
// (valuation_day * kDividendScheduleNsPerDay) + round(days * kDividendScheduleNsPerDay).
// Pair it with now_ts_ns = valuation_day * kDividendScheduleNsPerDay and
// expiry_ns = expiry_day * kDividendScheduleNsPerDay: the instant window then
// admits exactly the events `dividend_schedule_expiry_events` returns.
//
// @return events written; 0 when `out` is smaller than `schedule.n_events`
[[nodiscard]] std::size_t dividend_schedule_instants(const DividendScheduleResult &schedule,
                                                     std::span<DividendEvent> out) noexcept;

} // namespace atx::vol
