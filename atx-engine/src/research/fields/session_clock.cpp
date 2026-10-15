#include "atx/engine/research/fields/session_clock.hpp"

#include <iterator>
#include <string>

#include "atx/engine/research/fields/clock.hpp"
#include "atx/engine/research/fields/sources/nyse_calendar.hpp"
#include "atx/engine/research/fields/sources/stage.hpp"

namespace atx::engine::research::fields {
namespace {

[[nodiscard]] core::Error invalid(std::string message) {
  return core::Error(core::ErrorCode::InvalidArgument, "session clock: " + std::move(message));
}

// The number of elements of ascending unique `a` that are not in ascending unique `b` (numpy
// len(setdiff1d(a, b))).
[[nodiscard]] usize missing_from(std::span<const i64> a, std::span<const i64> b) {
  usize out = 0;
  for (const i64 x : a) {
    out += std::binary_search(b.begin(), b.end(), x) ? 0U : 1U;
  }
  return out;
}

} // namespace

core::Result<SessionCalendar> SessionCalendar::create(std::span<const i64> role_days,
                                                      i64 prefix_days, i64 suffix_days) {
  if (role_days.empty() || prefix_days < 0 || suffix_days < 0) {
    return core::Err(invalid("needs a role axis and non-negative calendar spans"));
  }
  for (usize t = 1; t < role_days.size(); ++t) {
    if (role_days[t] <= role_days[t - 1]) {
      return core::Err(invalid("the role's sessions are not strictly increasing"));
    }
  }
  const i64 first = role_days.front();
  const i64 last = role_days.back();
  const std::vector<i64> rule = nyse_sessions(first - prefix_days, last + suffix_days);
  std::vector<i64> inside;
  std::vector<i64> prefix;
  std::vector<i64> suffix;
  for (const i64 d : rule) {
    if (d < first) {
      prefix.push_back(d);
    } else if (d > last) {
      suffix.push_back(d);
    } else {
      inside.push_back(d);
    }
  }
  SessionCalendar out;
  out.prefix_ = prefix.size();
  out.suffix_ = suffix.size();
  out.days_.reserve(prefix.size() + role_days.size() + suffix.size());
  out.days_.insert(out.days_.end(), prefix.begin(), prefix.end());
  out.days_.insert(out.days_.end(), role_days.begin(), role_days.end());
  out.days_.insert(out.days_.end(), suffix.begin(), suffix.end());
  out.marks_.reserve(out.days_.size());
  for (const i64 d : out.days_) {
    out.marks_.push_back(d * kDayNs + kSessionMarkNs);
  }
  out.stats_ = nlohmann::json{
      {"rule", std::string(kNyseCalendarRule) +
                   " outside the role range, the role's sessions inside it"},
      {"prefix_days", prefix_days},
      {"suffix_days", suffix_days},
      {"rule_sessions_not_in_role", missing_from(inside, role_days)},
      {"role_sessions_not_in_rule", missing_from(role_days, inside)},
      {"first", iso_date(out.days_.front())},
      {"last", iso_date(out.days_.back())},
      {"prefix_sessions", out.prefix_},
      {"suffix_sessions", out.suffix_}};
  return core::Ok(std::move(out));
}

usize SessionCalendar::usable_from(i64 instant_ns) const noexcept {
  const auto it = std::upper_bound(marks_.begin(), marks_.end(), instant_ns);
  return static_cast<usize>(std::distance(marks_.begin(), it)) + 1;
}

usize SessionCalendar::at_or_after(i64 day) const noexcept {
  const auto it = std::lower_bound(days_.begin(), days_.end(), day);
  return static_cast<usize>(std::distance(days_.begin(), it));
}

usize SessionCalendar::count_through(i64 day) const noexcept {
  const auto it = std::upper_bound(days_.begin(), days_.end(), day);
  return static_cast<usize>(std::distance(days_.begin(), it));
}

core::Result<LatestRows> LatestRows::create(std::vector<i64> keys, std::vector<usize> columns,
                                            usize n_columns) {
  if (keys.size() != columns.size()) {
    return core::Err(invalid("latest rows: keys and columns differ in length"));
  }
  if (!std::is_sorted(keys.begin(), keys.end())) {
    return core::Err(invalid("latest rows: rows are not in clock order"));
  }
  const usize width = std::max<usize>(n_columns, 1);
  if (std::any_of(columns.begin(), columns.end(), [width](usize c) { return c >= width; })) {
    return core::Err(invalid("latest rows: a column is out of range"));
  }
  LatestRows out;
  out.keys_ = std::move(keys);
  out.columns_ = std::move(columns);
  out.latest_.assign(width, -1);
  return core::Ok(std::move(out));
}

void LatestRows::advance_to(usize end) noexcept {
  for (; next_ < end; ++next_) { // ascending row indices: the last row of a column wins
    latest_[columns_[next_]] = static_cast<i64>(next_);
  }
}

std::span<const i64> LatestRows::through(i64 bound) noexcept {
  const auto it = std::upper_bound(keys_.begin(), keys_.end(), bound);
  advance_to(static_cast<usize>(std::distance(keys_.begin(), it)));
  return latest_;
}

std::span<const i64> LatestRows::before(i64 bound) noexcept {
  const auto it = std::lower_bound(keys_.begin(), keys_.end(), bound);
  advance_to(static_cast<usize>(std::distance(keys_.begin(), it)));
  return latest_;
}

} // namespace atx::engine::research::fields
