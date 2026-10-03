#pragma once

// atx::engine::research::fields -- calendar days, strict ISO dates and the reader-side seal of the
// research field builders (platform core migration slice 1,
// docs/plans/2026-10-02-platform-core-migration.md).
//
// A "day" counts calendar days since 1970-01-01 (UTC): the unit of prepare_research_fields.py's
// day_of / date_of. Session labels are midnight-UTC nanoseconds, kDayNs per day. The seal is
// research_window.hpp's kSealBeginNs (the one source of the research window): a role must end
// before it, and every source row available on or after it is dropped before use and counted. No
// file here hard-codes a seal date.

#include <optional>
#include <string>
#include <string_view>

#include "atx/core/types.hpp"

namespace atx::engine::research::fields {

inline constexpr i64 kDayNs = 86'400'000'000'000LL;

struct CivilDate {
  i64 year{};
  u32 month{};
  u32 day{};
};

// Proleptic Gregorian conversions (H. Hinnant's days_from_civil / civil_from_days), exact for every
// date with a year inside +-2^40. Precondition of days_from_civil: 1 <= month <= 12 and
// 1 <= day <= 31.
[[nodiscard]] i64 days_from_civil(i64 year, u32 month, u32 day) noexcept;
[[nodiscard]] CivilDate civil_from_days(i64 day) noexcept;
[[nodiscard]] i64 year_of(i64 day) noexcept;

// Strict "YYYY-MM-DD": ten characters, year 0001..9999, a valid month and day of month (leap years
// by the Gregorian rule). nullopt for anything else.
[[nodiscard]] std::optional<i64> parse_iso_day(std::string_view text) noexcept;
// "YYYY-MM-DD" of a day (zero-padded four-digit year; a year outside 0..9999 prints its decimal
// value).
[[nodiscard]] std::string iso_date(i64 day);

// The first sealed day (research_window.hpp kSealBeginNs / kDayNs) and the predicate on it.
[[nodiscard]] i64 seal_day() noexcept;
[[nodiscard]] bool is_sealed_day(i64 day) noexcept;
// "research fields: <what> at or after the research seal <seal date> (<window id>)".
[[nodiscard]] std::string seal_refusal(std::string_view what);

} // namespace atx::engine::research::fields
