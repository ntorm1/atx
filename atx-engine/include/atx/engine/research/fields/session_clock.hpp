#pragma once

// atx::engine::research::fields -- the clock-rule primitives of the SEC and holdings field builders
// (P9 lane A4, migration slice 5), ported from research_fields_sec.py Calendar / Latest / Windowed:
//
//   SessionCalendar  the extended session axis of a role: the NYSE rule sessions (nyse-rule-v1,
//                    sources/nyse_calendar.hpp) in the prefix_days calendar days before the
//                    role's first session, the role's own sessions, then the rule sessions in the
//                    suffix_days after its last one; role row t is axis row prefix() + t. The mark
//                    of axis row u is its day 22:00 UTC (stage.hpp kSessionMarkNs), and a row
//                    stamped `available_at` is first usable at axis row usable_from(available_at):
//                    the first u with available_at < mark(u - 1).
//   LatestRows       rows in clock order (a non-decreasing key per row, each on one column): per
//                    column the last row whose key the stream has passed. The stream only advances;
//                    an earlier bound is a no-op (the Python pointer's rule).
//   WindowedSums<T>  per-column sums of the rows active on [enter, leave) axis rows, advanced axis
//                    row by axis row: at(u) applies every entry and exit at an index <= u, entries
//                    before exits at one index and each in row order (numpy's stable argsort of the
//                    concatenated events and np.add.at), so a floating sum is the Python's bit for
//                    bit.
//
// These are not asof_series.hpp's rule (the FINRA as-of value of a column with a calendar-day
// staleness): LatestRows hands back row indices, from which a builder reads several attributes of
// one row, on a key of its choosing (an axis row, an instant), and clock.hpp is the calendar-day
// and seal header. Not thread-safe (LatestRows and WindowedSums carry stream state); cold path.

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

// research_fields_sec.py CAL_PREFIX_DAYS / CAL_SUFFIX_DAYS.
inline constexpr i64 kSecCalendarPrefixDays = 400;
inline constexpr i64 kSecCalendarSuffixDays = 400;

class SessionCalendar {
public:
  // Err(InvalidArgument) for an empty or not strictly increasing role axis or a negative span.
  [[nodiscard]] static core::Result<SessionCalendar>
  create(std::span<const i64> role_days, i64 prefix_days, i64 suffix_days);

  [[nodiscard]] std::span<const i64> days() const noexcept { return days_; }
  [[nodiscard]] std::span<const i64> marks() const noexcept { return marks_; }
  [[nodiscard]] usize prefix() const noexcept { return prefix_; }
  [[nodiscard]] usize suffix() const noexcept { return suffix_; }
  [[nodiscard]] usize size() const noexcept { return days_.size(); }

  // The first axis row u at which a row stamped `instant_ns` is usable (available_at < mark(u-1)):
  // the event session s is the first whose mark follows the instant, u = s + 1 (size() + 1 when
  // no mark follows it).
  [[nodiscard]] usize usable_from(i64 instant_ns) const noexcept;
  // The axis row of the first session on or after `day` (size() when none).
  [[nodiscard]] usize at_or_after(i64 day) const noexcept;
  // The number of axis sessions on or before `day`.
  [[nodiscard]] usize count_through(i64 day) const noexcept;

  // The Python Calendar.stats block: rule, prefix_days, suffix_days, rule_sessions_not_in_role,
  // role_sessions_not_in_rule, first, last, prefix_sessions, suffix_sessions.
  [[nodiscard]] const nlohmann::json &stats() const noexcept { return stats_; }

private:
  SessionCalendar() = default;

  std::vector<i64> days_;
  std::vector<i64> marks_;
  usize prefix_{};
  usize suffix_{};
  nlohmann::json stats_;
};

class LatestRows {
public:
  // keys non-decreasing and every column < n_columns, else Err(InvalidArgument) ("rows are not in
  // clock order").
  [[nodiscard]] static core::Result<LatestRows> create(std::vector<i64> keys,
                                                       std::vector<usize> columns,
                                                       usize n_columns);

  // Advances over every row with key <= bound (through) or key < bound (before) and returns, per
  // column, the last such row (-1: none yet). The view lives until the next call.
  [[nodiscard]] std::span<const i64> through(i64 bound) noexcept;
  [[nodiscard]] std::span<const i64> before(i64 bound) noexcept;

  [[nodiscard]] usize rows_passed() const noexcept { return next_; }
  [[nodiscard]] usize rows() const noexcept { return keys_.size(); }

private:
  LatestRows() = default;
  void advance_to(usize end) noexcept;

  std::vector<i64> keys_;
  std::vector<usize> columns_;
  std::vector<i64> latest_;
  usize next_{};
};

template <class T> class WindowedSums {
public:
  // One row per (enter, leave, column, value); a row with enter >= leave never counts. Every
  // column < n_columns and the four spans of one length, else Err(InvalidArgument).
  [[nodiscard]] static core::Result<WindowedSums>
  create(std::span<const i64> enter, std::span<const i64> leave, std::span<const usize> columns,
         std::span<const T> values, usize n_columns) {
    if (enter.size() != leave.size() || enter.size() != columns.size() ||
        enter.size() != values.size()) {
      return core::Err(core::ErrorCode::InvalidArgument, "windowed sums: row arrays differ");
    }
    WindowedSums out;
    out.state_.assign(std::max<usize>(n_columns, 1), T{});
    for (usize i = 0; i < enter.size(); ++i) { // entries first, in row order
      if (columns[i] >= std::max<usize>(n_columns, 1)) {
        return core::Err(core::ErrorCode::InvalidArgument, "windowed sums: column out of range");
      }
      if (enter[i] < leave[i]) {
        out.events_.push_back(Event{enter[i], columns[i], values[i]});
      }
    }
    for (usize i = 0; i < enter.size(); ++i) { // then exits, in row order
      if (enter[i] < leave[i]) {
        out.events_.push_back(Event{leave[i], columns[i], static_cast<T>(-values[i])});
      }
    }
    std::stable_sort(out.events_.begin(), out.events_.end(),
                     [](const Event &a, const Event &b) { return a.index < b.index; });
    return core::Ok(std::move(out));
  }

  // The sums once every event at an index <= u is applied (u non-decreasing across calls; an
  // earlier u is a no-op). The view lives until the next call.
  [[nodiscard]] std::span<const T> at(i64 u) noexcept {
    while (next_ < events_.size() && events_[next_].index <= u) {
      state_[events_[next_].column] += events_[next_].delta;
      ++next_;
    }
    return state_;
  }

private:
  struct Event {
    i64 index{};
    usize column{};
    T delta{};
  };

  WindowedSums() = default;

  std::vector<Event> events_;
  std::vector<T> state_;
  usize next_{};
};

} // namespace atx::engine::research::fields
