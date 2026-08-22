#include "oracle_dividends.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

// Implementation of the `ddiv` step-function differencing described in
// oracle_dividends.hpp. Every refusal is recorded and counted; a violated group
// contributes NO schedule.

namespace atx::vol::oracle {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// One expiry inside a group, after the equal-`years` rows have been collapsed.
struct ExpiryPoint {
  double years = 0.0;
  double ddiv = 0.0;
};

// The half-open row range [begin, end) of one (date, underlier) group, over the
// key-sorted index vector.
struct GroupRange {
  std::size_t begin = 0;
  std::size_t end = 0;
};

[[nodiscard]] bool same_group(const OracleRow &a, const OracleRow &b) noexcept {
  return a.date == b.date && a.underlier == b.underlier;
}

void tally(DividendRefusalCounts &counts, DividendRefusal reason) noexcept {
  switch (reason) {
  case DividendRefusal::NonFiniteInput:
    ++counts.non_finite_input;
    return;
  case DividendRefusal::AmbiguousDdivAtExpiry:
    ++counts.ambiguous_ddiv_at_expiry;
    return;
  case DividendRefusal::NonMonotoneDdiv:
    ++counts.non_monotone_ddiv;
    return;
  case DividendRefusal::NonPositiveJump:
    ++counts.non_positive_jump;
    return;
  }
}

void refuse(DividendReconstruction &out, const DividendGroupKey &key, DividendRefusal reason,
            double years) {
  RefusedDividendGroup entry;
  entry.key = key;
  entry.reason = reason;
  entry.years = years;
  out.refused.push_back(std::move(entry));
  tally(out.refusals, reason);
}

// Collapse a group's rows into one point per distinct `years`, ascending.
// Returns false (with `reason` / `years` set) on the first violation.
[[nodiscard]] bool collapse_expiries(std::span<const OracleRow> rows,
                                     std::span<const std::size_t> order, const GroupRange &range,
                                     std::vector<ExpiryPoint> &points, DividendRefusal &reason,
                                     double &bad_years) {
  points.clear();

  // Finiteness FIRST: the sort below needs a strict weak ordering on `years`,
  // and a NaN there would make std::sort's contract unsatisfiable (UB), not
  // merely produce a bad answer.
  for (std::size_t i = range.begin; i < range.end; ++i) {
    const OracleRow &row = rows[order[i]];
    if (!std::isfinite(row.years) || !std::isfinite(row.ddiv)) {
      reason = DividendRefusal::NonFiniteInput;
      bad_years = row.years;
      return false;
    }
  }

  std::vector<std::size_t> by_years(order.begin() + static_cast<std::ptrdiff_t>(range.begin),
                                    order.begin() + static_cast<std::ptrdiff_t>(range.end));
  std::sort(by_years.begin(), by_years.end(),
            [rows](std::size_t a, std::size_t b) { return rows[a].years < rows[b].years; });

  for (const std::size_t idx : by_years) {
    const OracleRow &row = rows[idx];
    if (!points.empty() && points.back().years == row.years) {
      // `ddiv` is a property of the EXPIRY. Two answers at one expiry is not a
      // step function, so nothing differenced out of it would mean anything.
      if (std::abs(points.back().ddiv - row.ddiv) > kDdivFlatTol) {
        reason = DividendRefusal::AmbiguousDdivAtExpiry;
        bad_years = row.years;
        return false;
      }
      continue;
    }
    points.push_back(ExpiryPoint{row.years, row.ddiv});
  }
  return true;
}

// Difference one group's collapsed expiry ladder into a schedule.
[[nodiscard]] bool difference_schedule(const std::vector<ExpiryPoint> &points,
                                       std::vector<CashDividend> &schedule,
                                       DividendRefusal &reason, double &bad_years) {
  schedule.clear();
  // Baseline 0: `ddiv` at the earliest expiry is already a SUM, so a positive
  // value there is a real dividend whose upper bracket is that expiry.
  double previous = 0.0;
  for (const ExpiryPoint &point : points) {
    const double jump = point.ddiv - previous;
    if (jump < -kDdivFlatTol) {
      reason = DividendRefusal::NonMonotoneDdiv;
      bad_years = point.years;
      return false;
    }
    if (jump > kDdivFlatTol) {
      if (jump < kMinDividendJump) {
        reason = DividendRefusal::NonPositiveJump;
        bad_years = point.years;
        return false;
      }
      schedule.push_back(CashDividend{point.years, jump});
    }
    // Carried forward even when the change was folded into "flat", so the
    // emitted amounts keep telescoping against the ORIGINAL column values —
    // that is what holds the accrual invariant at ~1e-15 instead of drifting.
    previous = point.ddiv;
  }
  return true;
}

} // namespace

std::string_view to_string(DividendRefusal reason) noexcept {
  switch (reason) {
  case DividendRefusal::NonFiniteInput:
    return "NonFiniteInput";
  case DividendRefusal::AmbiguousDdivAtExpiry:
    return "AmbiguousDdivAtExpiry";
  case DividendRefusal::NonMonotoneDdiv:
    return "NonMonotoneDdiv";
  case DividendRefusal::NonPositiveJump:
    return "NonPositiveJump";
  }
  return "Unrecognized"; // unreachable for valid enumerators
}

DividendReconstruction reconstruct_dividends(std::span<const OracleRow> rows) {
  DividendReconstruction out;
  out.rows_seen = rows.size();
  if (rows.empty()) {
    return out;
  }

  // Key-sort only on the two STRING fields: string comparison is total, so this
  // sort is well defined even when a row carries a non-finite `years` that the
  // per-group finiteness screen has not seen yet.
  std::vector<std::size_t> order(rows.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::sort(order.begin(), order.end(), [rows](std::size_t a, std::size_t b) {
    if (rows[a].date != rows[b].date) {
      return rows[a].date < rows[b].date;
    }
    return rows[a].underlier < rows[b].underlier;
  });

  std::vector<ExpiryPoint> points;
  std::vector<CashDividend> schedule;
  std::size_t begin = 0;
  while (begin < order.size()) {
    std::size_t end = begin + 1U;
    while (end < order.size() && same_group(rows[order[begin]], rows[order[end]])) {
      ++end;
    }
    ++out.groups_seen;

    const OracleRow &head = rows[order[begin]];
    DividendGroupKey key{head.date, head.underlier};
    const GroupRange range{begin, end};

    DividendRefusal reason = DividendRefusal::NonFiniteInput;
    double bad_years = 0.0;
    if (!collapse_expiries(rows, order, range, points, reason, bad_years) ||
        !difference_schedule(points, schedule, reason, bad_years)) {
      refuse(out, key, reason, bad_years);
      begin = end;
      continue;
    }

    DividendSchedule accepted;
    accepted.key = std::move(key);
    accepted.dividends = schedule;
    accepted.rows = end - begin;
    accepted.expiries = points.size();
    out.schedules.push_back(std::move(accepted));
    begin = end;
  }
  return out;
}

double accrued_dividend(std::span<const CashDividend> dividends, double years) noexcept {
  double total = 0.0;
  for (const CashDividend &dividend : dividends) {
    if (dividend.tau <= years) {
      total += dividend.amount;
    }
  }
  return total;
}

Result<std::vector<CashDividend>> split_merged_dividends(std::span<const CashDividend> dividends,
                                                         const DividendCadence &cadence) {
  if (!std::isfinite(cadence.period_years) || !(cadence.period_years > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "cadence period must be finite and > 0");
  }
  if (cadence.max_parts < 1) {
    return Err(ErrorCode::InvalidArgument, "cadence max_parts must be >= 1");
  }

  // The input contract is the shape `reconstruct_dividends` emits; anything else
  // fails closed here rather than producing a schedule with a silently reordered
  // or negative event.
  double previous_tau = 0.0;
  for (const CashDividend &dividend : dividends) {
    if (!std::isfinite(dividend.tau) || !std::isfinite(dividend.amount)) {
      return Err(ErrorCode::InvalidArgument, "dividend tau and amount must be finite");
    }
    if (!(dividend.amount > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "dividend amount must be > 0");
    }
    if (!(dividend.tau > previous_tau)) {
      return Err(ErrorCode::InvalidArgument, "schedule must be strictly ascending in tau, from 0");
    }
    previous_tau = dividend.tau;
  }

  std::vector<CashDividend> out;
  out.reserve(dividends.size());
  double last_emitted_tau = 0.0;
  for (std::size_t i = 0; i < dividends.size(); ++i) {
    const CashDividend &dividend = dividends[i];
    // No previous ex-date means no gap to read a part count from, so a merge at
    // the front of a schedule is undetectable and is left alone.
    const double gap = (i == 0) ? 0.0 : dividend.tau - dividends[i - 1U].tau;
    const double implied = (i == 0) ? 1.0 : std::nearbyint(gap / cadence.period_years);
    if (implied > static_cast<double>(cadence.max_parts)) {
      return Err(ErrorCode::OutOfRange,
                 "a gap implies more than max_parts instalments — this is not the cadence "
                 "that produced this schedule");
    }
    const int parts = (implied < 1.0) ? 1 : static_cast<int>(implied);
    if (parts == 1) {
      if (!(dividend.tau > last_emitted_tau)) {
        return Err(ErrorCode::OutOfRange, "split produced a non-ascending schedule");
      }
      out.push_back(dividend);
      last_emitted_tau = dividend.tau;
      continue;
    }
    const double each = dividend.amount / static_cast<double>(parts);
    for (int j = 0; j < parts; ++j) {
      // The LAST instalment keeps the recovered tau exactly: that one is the
      // upper bracket the expiry ladder actually gave, not an inference.
      const double back = static_cast<double>(parts - 1 - j) * cadence.period_years;
      const double tau = dividend.tau - back;
      if (!(tau > 0.0) || !(tau > last_emitted_tau)) {
        return Err(ErrorCode::OutOfRange,
                   "a placed instalment lands at or before tau 0 or the previous ex-date");
      }
      out.push_back(CashDividend{tau, each});
      last_emitted_tau = tau;
    }
  }
  return Ok(out);
}

} // namespace atx::vol::oracle
