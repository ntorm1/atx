#pragma once

// atx::engine::research::fields -- the NYSE rule calendar (rule nyse-rule-v1, ported from
// research_fields_sec.py nyse_holidays / nyse_sessions) and the extended session axis the vendor
// panel reads (research_fields_price.py extended_days, session_calendar). Days count calendar days
// since 1970-01-01 (clock.hpp).
//
// nyse-rule-v1 full-day closures of a year: New Year (a Sunday New Year is observed on Monday; a
// Saturday New Year is not observed), MLK and Presidents (3rd Monday of January / February), Good
// Friday (Gregorian Easter, anonymous / Meeus-Jones-Butcher algorithm, minus two days), Memorial
// (last Monday of May), Juneteenth (from 2022), Independence, Labor (1st Monday of September),
// Thanksgiving (4th Thursday of November), Christmas (a Saturday holiday is observed on Friday, a
// Sunday one on Monday), plus the special closures listed in the .cpp. A session is a weekday that
// is not a closure.
//
// The extended axis of a role: `pre_sessions` rule sessions before the role's first session (and,
// with `lookback_days`, every rule session in the lookback_days calendar days before the first of
// them), then the role's own sessions. Role row t is axis row t + prefix.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

inline constexpr std::string_view kNyseCalendarRule = "nyse-rule-v1";

// Monday 0 .. Sunday 6 (Python date.weekday()).
[[nodiscard]] i64 weekday_of(i64 day) noexcept;

// The rule's sessions in [first_day, last_day] (ascending; empty when last_day < first_day).
[[nodiscard]] std::vector<i64> nyse_sessions(i64 first_day, i64 last_day);

struct ExtendedAxis {
  std::vector<i64> days; // strictly increasing
  usize prefix{};        // rule sessions before the role (axis row of role row 0)
};

// research_fields_price.extended_days. Err(InvalidArgument) for an empty role axis, a negative
// lookback, or a rule calendar shorter than pre_sessions before the role.
[[nodiscard]] core::Result<ExtendedAxis>
extended_axis(std::span<const i64> role_days, usize pre_sessions, i64 lookback_days);

// research_fields_price.session_calendar: the rule sessions from 1970-01-01 through the day before
// the research seal, pinned by the SHA-256 of "nyse-rule-v1\n" followed by each session's day as a
// little-endian i64 (a --reuse input of the fields on the extended axis).
struct SessionCalendarPin {
  std::string rule;
  std::string first;
  std::string last;
  u64 sessions{};
  std::string sha256;
};
[[nodiscard]] core::Result<SessionCalendarPin> session_calendar_pin();

} // namespace atx::engine::research::fields
