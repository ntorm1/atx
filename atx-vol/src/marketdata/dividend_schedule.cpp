#include "atx/vol/api/marketdata/dividend_schedule.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace atx::vol {
namespace {

constexpr std::size_t kMaxExpiries = kDividendScheduleMaxExpiries;
// An expiry more than a century out is not a listed option; refusing it also
// keeps every day count far inside the int64 nanosecond range of the instants.
constexpr std::int64_t kMaxHorizonDays = 36'525;
// Bisection steps for the split spacing: 2^-64 of the cadence is far below a
// nanosecond, so the loop bound is never the accuracy limit.
constexpr int kSpacingBisections = 64;

using Reason = DividendScheduleReason;

// One increment of the dividend sum between two consecutive expiries.
struct Increment {
  std::size_t expiry = 0;  // ladder index of the expiry that first includes it
  std::int32_t lo = 0;     // relative day of the previous expiry (0 = valuation date)
  std::int32_t hi = 0;     // relative day of this expiry
  double amount = 0.0;
  double days = 0.0;       // provisional single-event time, inside [lo + 1, hi]
  bool integer = false;    // the PV-implied time is an integer day in the window
  std::size_t count = 1;   // events the count pass assigns
  // More units by amount than its window holds apart (header, pass 2): kept as
  // one event and flagged, never split into fewer events than its units.
  bool overfull = false;
  DividendEventKind kind = DividendEventKind::Exact; // how the place pass resolved it
};

struct Work {
  std::array<DividendLadderExpiry, kMaxExpiries> ladder{};
  std::size_t n_ladder = 0;
  std::array<Increment, kMaxExpiries> increments{};
  std::size_t n_increments = 0;
};

bool valid_config(const DividendScheduleConfig &c) noexcept {
  const auto positive = [](double x) { return std::isfinite(x) && x > 0.0; };
  const auto nonnegative = [](double x) { return std::isfinite(x) && x >= 0.0; };
  return positive(c.day_basis) && nonnegative(c.amount_tolerance) &&
         nonnegative(c.day_tolerance) && c.day_tolerance < 0.5 &&
         nonnegative(c.bracket_tolerance_days) && nonnegative(c.pv_relative_tolerance) &&
         nonnegative(c.pv_inconsistent_relative) && nonnegative(c.pv_absolute_floor) &&
         nonnegative(c.min_rate) && positive(c.cadence_spacing_fraction) &&
         positive(c.min_event_spacing_days) && c.max_events_per_increment >= 1 &&
         c.max_events_per_increment <= kDividendScheduleMaxEvents;
}

bool same_values(const DividendLadderExpiry &a, const DividendLadderExpiry &b) noexcept {
  return a.years == b.years && a.rate == b.rate && a.dividend_sum == b.dividend_sum &&
         a.dividend_pv == b.dividend_pv;
}

Reason check_expiry(std::int32_t valuation_day, const DividendLadderExpiry &e) noexcept {
  if (!std::isfinite(e.years) || !std::isfinite(e.rate) || !std::isfinite(e.dividend_sum) ||
      !std::isfinite(e.dividend_pv)) {
    return Reason::NonFiniteInput;
  }
  if (!(e.years > 0.0) || e.dividend_sum < 0.0 || e.dividend_pv < 0.0) {
    return Reason::NegativeInput;
  }
  const std::int64_t horizon = static_cast<std::int64_t>(e.expiry_day) - valuation_day;
  if (horizon <= 0) {
    return Reason::ExpiryNotAfterValuation;
  }
  if (horizon > kMaxHorizonDays) {
    return Reason::ExpiryBeyondHorizon;
  }
  return Reason::None;
}

// Insertion into the day-sorted ladder. Identical duplicates merge; conflicting
// ones refuse, because two different ladders cannot both describe one expiry.
Reason load_ladder(std::int32_t valuation_day, std::span<const DividendLadderExpiry> in,
                   Work &w, std::int32_t &failed_day) noexcept {
  for (const DividendLadderExpiry &e : in) {
    failed_day = e.expiry_day;
    const Reason bad = check_expiry(valuation_day, e);
    if (bad != Reason::None) {
      return bad;
    }
    const auto begin = w.ladder.begin();
    const auto end = begin + static_cast<std::ptrdiff_t>(w.n_ladder);
    const auto pos = std::lower_bound(begin, end, e.expiry_day,
        [](const DividendLadderExpiry &x, std::int32_t day) { return x.expiry_day < day; });
    if (pos != end && pos->expiry_day == e.expiry_day) {
      if (!same_values(*pos, e)) {
        return Reason::DuplicateExpiryConflict;
      }
      continue;
    }
    if (w.n_ladder == kMaxExpiries) {
      return Reason::TooManyExpiries;
    }
    std::move_backward(pos, end, end + 1);
    *pos = e;
    ++w.n_ladder;
  }
  failed_day = 0;
  return Reason::None;
}

double pv_of_increments(const Work &w, double rate, double basis) noexcept {
  double pv = 0.0;
  for (std::size_t i = 0; i < w.n_increments; ++i) {
    pv += w.increments[i].amount * std::exp(-rate * w.increments[i].days / basis);
  }
  return pv;
}

// Pass 1 for one increment: date it as ONE event at its PV-implied time.
Reason solve_increment(const DividendScheduleConfig &c, Work &w, std::size_t k,
                       std::int32_t lo, std::int32_t hi, double amount) noexcept {
  const DividendLadderExpiry &e = w.ladder[k];
  if (!(std::abs(e.rate) >= c.min_rate)) {
    return Reason::UntimedIncrement;
  }
  const double rest = e.dividend_pv - pv_of_increments(w, e.rate, c.day_basis);
  if (!(rest > 0.0)) {
    return Reason::NonPositivePv;
  }
  const double t = -std::log(rest / amount) * c.day_basis / e.rate;
  if (!std::isfinite(t)) {
    return Reason::NonPositivePv;
  }
  const double first = static_cast<double>(lo) + 1.0;
  const double last = static_cast<double>(hi);
  if (t < first - c.bracket_tolerance_days || t > last + c.bracket_tolerance_days) {
    return Reason::ExDateOutsideBracket;
  }
  const double nearest = std::round(t);
  Increment &inc = w.increments[w.n_increments++];
  inc = Increment{};
  inc.expiry = k;
  inc.lo = lo;
  inc.hi = hi;
  inc.amount = amount;
  inc.integer = std::abs(t - nearest) <= c.day_tolerance && nearest >= first && nearest <= last;
  inc.days = inc.integer ? nearest : std::clamp(t, first, last);
  return Reason::None;
}

struct PeelOutcome {
  std::size_t consistent = 0; // ladder prefix [0, consistent) is consistent
  Reason reason = Reason::None;
};

PeelOutcome peel(std::int32_t valuation_day, const DividendScheduleConfig &c, Work &w) noexcept {
  double previous_sum = 0.0;
  std::int32_t lo = 0;
  for (std::size_t k = 0; k < w.n_ladder; ++k) {
    const DividendLadderExpiry &e = w.ladder[k];
    const std::int32_t hi = e.expiry_day - valuation_day;
    const double amount = e.dividend_sum - previous_sum;
    if (amount < -c.amount_tolerance) {
      return {k, Reason::DecreasingSum};
    }
    if (amount > c.amount_tolerance) {
      const Reason bad = solve_increment(c, w, k, lo, hi, amount);
      if (bad != Reason::None) {
        return {k, bad};
      }
      // A flat expiry leaves the baseline alone, so sub-tolerance noise is not
      // absorbed into the baseline and later re-counted as cash.
      previous_sum = e.dividend_sum;
    }
    lo = hi;
  }
  return {w.n_ladder, Reason::None};
}

double median(std::array<double, kMaxExpiries> &values, std::size_t n) noexcept {
  if (n == 0) {
    return 0.0;
  }
  const auto begin = values.begin();
  std::sort(begin, begin + static_cast<std::ptrdiff_t>(n));
  return n % 2 == 1 ? values[n / 2] : 0.5 * values[n / 2 - 1] + 0.5 * values[n / 2];
}

std::size_t amount_count(double amount, double unit, std::size_t cap) noexcept {
  if (!(unit > 0.0)) {
    return 1;
  }
  // Clamp BEFORE the integer conversion so a tiny unit cannot overflow it.
  const double ratio = std::min(amount / unit, static_cast<double>(cap) + 1.0);
  return std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(ratio)));
}

// Pass 2: unit, cadence and the event count of every increment.
void count_events(const DividendScheduleConfig &c, Work &w, DividendScheduleResult &out) noexcept {
  std::array<double, kMaxExpiries> scratch{};
  std::size_t n = 0;
  for (std::size_t i = 0; i < w.n_increments; ++i) {
    if (w.increments[i].integer) {
      scratch[n++] = w.increments[i].amount;
    }
  }
  const double unit = median(scratch, n);
  const std::size_t cap = c.max_events_per_increment;
  const auto single = [&](const Increment &inc) {
    return inc.integer && amount_count(inc.amount, unit, cap) == 1;
  };
  n = 0;
  for (std::size_t i = 1; i < w.n_increments; ++i) {
    if (single(w.increments[i - 1]) && single(w.increments[i])) {
      scratch[n++] = w.increments[i].days - w.increments[i - 1].days;
    }
  }
  const double cadence = median(scratch, n);
  const double spacing =
      cadence > 0.0 ? c.cadence_spacing_fraction * cadence : c.min_event_spacing_days;
  for (std::size_t i = 0; i < w.n_increments; ++i) {
    Increment &inc = w.increments[i];
    const double width = static_cast<double>(inc.hi - inc.lo - 1);
    const double fit = std::min(1.0 + std::floor(width / spacing), static_cast<double>(cap));
    const std::size_t units = amount_count(inc.amount, unit, cap);
    const auto room = static_cast<std::size_t>(fit);
    // A run of regular dividends needs `units` events; a window that holds fewer
    // (but more than one) cannot carry that run, and splitting it into `room`
    // events would invent a dividend size nobody paid. A window that holds ONE
    // event keeps the increment whole and exactly dated (a special).
    inc.overfull = units > room && room >= 2;
    inc.count = inc.overfull ? 1 : std::min({units, room, cap});
  }
  out.unit_amount = unit;
  out.cadence_days = cadence;
}

DividendScheduleEvent make_event(std::int32_t valuation_day, std::int32_t increment_expiry_day,
                                 double amount, double days, std::size_t count,
                                 DividendEventKind kind) noexcept {
  DividendScheduleEvent ev;
  ev.amount = amount;
  ev.days = days;
  ev.ex_day = valuation_day + static_cast<std::int32_t>(std::llround(days));
  ev.increment_expiry_day = increment_expiry_day;
  ev.increment_events = static_cast<std::uint16_t>(count);
  ev.kind = kind;
  return ev;
}

double pv_of_events(const DividendScheduleResult &out, double rate, double basis) noexcept {
  double pv = 0.0;
  for (std::size_t i = 0; i < out.n_events; ++i) {
    pv += out.events[i].amount * std::exp(-rate * out.events[i].days / basis);
  }
  return pv;
}

// THE DECLARED SPLIT RULE (header): n equal amounts, equally spaced, PV-exact.
// Returns false when even coincident events cannot reproduce the PV inside the
// window; the caller then keeps the increment whole.
bool split_increment(std::int32_t valuation_day, const DividendScheduleConfig &c,
                     const Increment &inc, const DividendLadderExpiry &e, double rest,
                     double cadence, DividendScheduleResult &out) noexcept {
  const std::size_t n = inc.count;
  const double share = inc.amount / static_cast<double>(n);
  const double first = static_cast<double>(inc.lo) + 1.0;
  const double last = static_cast<double>(inc.hi);
  const auto first_time = [&](double h) {
    double s = 0.0;
    for (std::size_t j = 0; j < n; ++j) {
      s += std::exp(-e.rate * static_cast<double>(j) * h / c.day_basis);
    }
    // share * s * exp(-rate * t1 / basis) == rest
    return -std::log(rest / (share * s)) * c.day_basis / e.rate;
  };
  const auto feasible = [&](double h) {
    const double t1 = first_time(h);
    return std::isfinite(t1) && t1 >= first && t1 + static_cast<double>(n - 1) * h <= last;
  };
  if (!(rest > 0.0) || !feasible(0.0)) {
    return false;
  }
  double h = cadence > 0.0 ? cadence
                           : static_cast<double>(inc.hi - inc.lo) / static_cast<double>(n);
  if (!feasible(h)) {
    // Invariant: `good` is feasible (h = 0 is), `bad` is not.
    double good = 0.0;
    double bad = h;
    for (int it = 0; it < kSpacingBisections; ++it) {
      const double mid = 0.5 * (good + bad);
      (feasible(mid) ? good : bad) = mid;
    }
    h = good;
  }
  const double t1 = first_time(h);
  for (std::size_t j = 0; j < n; ++j) {
    const double days = std::clamp(t1 + static_cast<double>(j) * h, first, last);
    out.events[out.n_events++] = make_event(valuation_day, e.expiry_day, share, days, n,
                                            DividendEventKind::SumSplit);
  }
  return true;
}

// Pass 3: final placement, in ladder order, each increment's PV taken net of
// the FINAL events before it so the PV identity holds at every increment.
Reason place_events(std::int32_t valuation_day, const DividendScheduleConfig &c, Work &w,
                    DividendScheduleResult &out) noexcept {
  for (std::size_t i = 0; i < w.n_increments; ++i) {
    Increment &inc = w.increments[i];
    const DividendLadderExpiry &e = w.ladder[inc.expiry];
    if (out.n_events + inc.count > kDividendScheduleMaxEvents) {
      return Reason::TooManyEvents;
    }
    const double rest = e.dividend_pv - pv_of_events(out, e.rate, c.day_basis);
    if (inc.count >= 2 && split_increment(valuation_day, c, inc, e, rest, out.cadence_days, out)) {
      inc.kind = DividendEventKind::SumSplit;
      continue;
    }
    // A single event is dated AND classified from the final events before it:
    // pass 1 judged its time against provisional single-event stand-ins for
    // earlier split increments, priced at another expiry's rate (review M2).
    const double first = static_cast<double>(inc.lo) + 1.0;
    const double last = static_cast<double>(inc.hi);
    const double t = -std::log(rest / inc.amount) * c.day_basis / e.rate;
    const double nearest = std::round(t);
    const bool integer = std::isfinite(t) && std::abs(t - nearest) <= c.day_tolerance &&
                         nearest >= first && nearest <= last;
    double days = inc.days;
    if (inc.count == 1 && !inc.overfull && integer) {
      inc.kind = DividendEventKind::Exact;
      days = nearest;
    } else {
      inc.kind = DividendEventKind::NonIntegerDay;
      if (std::isfinite(t)) {
        days = std::clamp(t, first, last);
      }
    }
    out.events[out.n_events++] =
        make_event(valuation_day, e.expiry_day, inc.amount, days, 1, inc.kind);
  }
  return Reason::None;
}

// Pass 4: reprice every consistent expiry from the finished schedule. Returns
// the first ladder index with a GROSS PV miss, or `consistent` when none.
std::size_t verify(std::int32_t valuation_day, const DividendScheduleConfig &c, const Work &w,
                   std::size_t consistent, DividendScheduleResult &out) noexcept {
  for (std::size_t k = 0; k < consistent; ++k) {
    const DividendLadderExpiry &e = w.ladder[k];
    const double horizon = static_cast<double>(e.expiry_day - valuation_day);
    double sum = 0.0;
    double pv = 0.0;
    for (std::size_t i = 0; i < out.n_events; ++i) {
      const DividendScheduleEvent &ev = out.events[i];
      if (ev.days <= horizon) {
        sum += ev.amount;
        pv += ev.amount * std::exp(-e.rate * ev.days / c.day_basis);
      }
    }
    out.max_sum_abs_residual = std::max(out.max_sum_abs_residual, std::abs(sum - e.dividend_sum));
    const double miss = std::abs(pv - e.dividend_pv);
    const double relative = miss / std::max(e.dividend_pv, c.pv_absolute_floor);
    out.max_pv_relative_residual = std::max(out.max_pv_relative_residual, relative);
    if (relative > c.pv_relative_tolerance) {
      ++out.pv_residual_exceedances;
    }
    if (relative > c.pv_inconsistent_relative && miss > c.pv_absolute_floor) {
      return k;
    }
  }
  return consistent;
}

void truncate_events(std::int32_t valuation_day, std::int32_t through_day,
                     DividendScheduleResult &out) noexcept {
  const double horizon = static_cast<double>(through_day - valuation_day);
  std::size_t kept = 0;
  for (std::size_t i = 0; i < out.n_events; ++i) {
    if (out.events[i].days <= horizon) {
      out.events[kept++] = out.events[i];
    }
  }
  for (std::size_t i = kept; i < out.n_events; ++i) {
    out.events[i] = DividendScheduleEvent{};
  }
  out.n_events = kept;
}

void tally_increments(const Work &w, DividendScheduleResult &out) noexcept {
  const std::int32_t through = out.resolved_through_day - out.valuation_day;
  for (std::size_t i = 0; i < w.n_increments; ++i) {
    const Increment &inc = w.increments[i];
    if (inc.hi > through) {
      continue;
    }
    ++out.increments;
    switch (inc.kind) {
    case DividendEventKind::Exact:
      ++out.exact_increments;
      break;
    case DividendEventKind::NonIntegerDay:
      ++out.non_integer_increments;
      break;
    case DividendEventKind::SumSplit:
      ++out.split_increments;
      break;
    }
  }
}

DividendScheduleResult refuse(DividendScheduleResult out, Reason reason,
                              std::int32_t failed_day) noexcept {
  out.status = DividendScheduleStatus::Invalid;
  out.reason = reason;
  out.failed_expiry_day = failed_day;
  out.resolved_through_day = out.valuation_day;
  out.n_events = 0;
  out.events = {};
  return out;
}

} // namespace

bool DividendScheduleResult::covers(std::int32_t expiry_day) const noexcept {
  return status != DividendScheduleStatus::Invalid && expiry_day > valuation_day &&
         expiry_day <= resolved_through_day;
}

DividendScheduleResult infer_dividend_schedule(std::int32_t valuation_day,
                                               std::span<const DividendLadderExpiry> expiries,
                                               const DividendScheduleConfig &config) noexcept {
  DividendScheduleResult out;
  out.valuation_day = valuation_day;
  out.resolved_through_day = valuation_day;
  if (!valid_config(config)) {
    return refuse(out, Reason::InvalidConfig, 0);
  }
  Work w;
  std::int32_t failed_day = 0;
  const Reason loaded = load_ladder(valuation_day, expiries, w, failed_day);
  if (loaded != Reason::None) {
    return refuse(out, loaded, failed_day);
  }
  out.expiries = w.n_ladder;
  const PeelOutcome peeled = peel(valuation_day, config, w);
  count_events(config, w, out);
  const Reason placed = place_events(valuation_day, config, w, out);
  if (placed != Reason::None) {
    return refuse(out, placed, 0);
  }
  std::size_t consistent = peeled.consistent;
  Reason reason = peeled.reason;
  const std::size_t verified = verify(valuation_day, config, w, consistent, out);
  if (verified < consistent) {
    consistent = verified;
    reason = Reason::RepricingMismatch;
  }
  out.resolved_through_day =
      consistent > 0 ? w.ladder[consistent - 1].expiry_day : valuation_day;
  truncate_events(valuation_day, out.resolved_through_day, out);
  tally_increments(w, out);
  if (consistent < w.n_ladder) {
    out.status = DividendScheduleStatus::Inconsistent;
    out.reason = reason;
    out.failed_expiry_day = w.ladder[consistent].expiry_day;
  } else if (w.n_increments == 0) {
    out.status = DividendScheduleStatus::NoDividends;
  } else if (out.exact_increments == out.increments) {
    out.status = DividendScheduleStatus::Ok;
  } else {
    out.status = DividendScheduleStatus::Ambiguous;
  }
  return out;
}

DividendExpiryEvents dividend_schedule_expiry_events(const DividendScheduleResult &schedule,
                                                     std::int32_t expiry_day,
                                                     double expiry_years,
                                                     std::span<CashDividend> out) noexcept {
  DividendExpiryEvents result;
  if (!(expiry_years > 0.0) || !std::isfinite(expiry_years) ||
      expiry_day <= schedule.valuation_day) {
    result.coverage = DividendExpiryCoverage::InvalidInput;
    return result;
  }
  if (!schedule.covers(expiry_day)) {
    return result;
  }
  const double horizon = static_cast<double>(expiry_day - schedule.valuation_day);
  for (const DividendScheduleEvent &ev : schedule.view()) {
    if (!(ev.days <= horizon)) {
      continue;
    }
    if (result.count == out.size()) {
      return DividendExpiryEvents{DividendExpiryCoverage::BufferTooSmall, 0, 0, 0.0};
    }
    // An ex-date on the expiry date lands at exactly T, not one rounding off it.
    const double tau = ev.days == horizon ? expiry_years : expiry_years * (ev.days / horizon);
    out[result.count++] = CashDividend{tau, ev.amount};
    result.cash += ev.amount;
    if (ev.kind != DividendEventKind::Exact) {
      ++result.ambiguous;
    }
  }
  result.coverage = DividendExpiryCoverage::Covered;
  return result;
}

std::size_t dividend_schedule_instants(const DividendScheduleResult &schedule,
                                       std::span<DividendEvent> out) noexcept {
  if (out.size() < schedule.n_events) {
    return 0;
  }
  constexpr double kNsPerDay = static_cast<double>(kDividendScheduleNsPerDay);
  // Valuation day plus a bounded horizon: refuse rather than overflow int64.
  const double origin = static_cast<double>(schedule.valuation_day) * kNsPerDay;
  constexpr double kLimit = 9.0e18;
  if (!(std::abs(origin) + static_cast<double>(kMaxHorizonDays + 1) * kNsPerDay < kLimit)) {
    return 0;
  }
  const std::int64_t base =
      static_cast<std::int64_t>(schedule.valuation_day) * kDividendScheduleNsPerDay;
  for (std::size_t i = 0; i < schedule.n_events; ++i) {
    const DividendScheduleEvent &ev = schedule.events[i];
    out[i] = DividendEvent{base + std::llround(ev.days * kNsPerDay), ev.amount};
  }
  return schedule.n_events;
}

const char *to_string(DividendScheduleStatus value) noexcept {
  switch (value) {
  case DividendScheduleStatus::Ok:
    return "ok";
  case DividendScheduleStatus::NoDividends:
    return "no_dividends";
  case DividendScheduleStatus::Ambiguous:
    return "ambiguous";
  case DividendScheduleStatus::Inconsistent:
    return "inconsistent";
  case DividendScheduleStatus::Invalid:
    return "invalid";
  }
  return "unknown";
}

const char *to_string(DividendScheduleReason value) noexcept {
  switch (value) {
  case Reason::None:
    return "none";
  case Reason::InvalidConfig:
    return "invalid_config";
  case Reason::NonFiniteInput:
    return "non_finite_input";
  case Reason::NegativeInput:
    return "negative_input";
  case Reason::ExpiryNotAfterValuation:
    return "expiry_not_after_valuation";
  case Reason::ExpiryBeyondHorizon:
    return "expiry_beyond_horizon";
  case Reason::DuplicateExpiryConflict:
    return "duplicate_expiry_conflict";
  case Reason::TooManyExpiries:
    return "too_many_expiries";
  case Reason::TooManyEvents:
    return "too_many_events";
  case Reason::DecreasingSum:
    return "decreasing_sum";
  case Reason::NonPositivePv:
    return "non_positive_pv";
  case Reason::ExDateOutsideBracket:
    return "ex_date_outside_bracket";
  case Reason::UntimedIncrement:
    return "untimed_increment";
  case Reason::RepricingMismatch:
    return "repricing_mismatch";
  }
  return "unknown";
}

const char *to_string(DividendEventKind value) noexcept {
  switch (value) {
  case DividendEventKind::Exact:
    return "exact";
  case DividendEventKind::NonIntegerDay:
    return "non_integer_day";
  case DividendEventKind::SumSplit:
    return "sum_split";
  }
  return "unknown";
}

const char *to_string(DividendExpiryCoverage value) noexcept {
  switch (value) {
  case DividendExpiryCoverage::Covered:
    return "covered";
  case DividendExpiryCoverage::NotCovered:
    return "not_covered";
  case DividendExpiryCoverage::InvalidInput:
    return "invalid_input";
  case DividendExpiryCoverage::BufferTooSmall:
    return "buffer_too_small";
  }
  return "unknown";
}

} // namespace atx::vol
