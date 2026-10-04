#pragma once

// atx::engine::research::fields -- fields si_shares and si_dtc, ported from
// prepare_research_fields.py (read_schedule, parse_asof_csv, finra_field; migration slice 1).
// Publication-lagged as-of fields: the FINRA short-interest producer's as-of CSVs
// (`<finra>/asof/<name>.csv`, pinned by `<finra>/asof/manifest.json` outputs.<name>.sha256),
// joined onto the role by the as-of clock rule (asof_series.hpp) with a 45-day staleness.
//
// Order of checks, as the Python: the CSV bytes against the producer receipt; the strict CSV
// contract; every available_at an official dissemination date of
// `<finra>/dissemination_schedule.csv`; rows available on or after the research seal dropped and
// counted (reader-side seal); rows of ids off the role axis counted and ignored. Vintage risk: a
// visible row disseminated on or before the last dissemination of a settlement before 2021-06-01
// comes from FINRA's later republication; its finite member cells and the last session showing one
// are reported.

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/engine/research/fields/field_spec.hpp"
#include "atx/engine/research/fields/field_writer.hpp"
#include "atx/engine/research/fields/role_axes.hpp"

namespace atx::engine::research::fields {

inline constexpr i64 kFinraMaxStaleDays = 45;
// Settlements before this date come from FINRA's later consolidated republication (the Python's
// FINRA_REPUBLICATION_SETTLEMENT_BEFORE).
inline constexpr std::string_view kFinraRepublicationSettlementBefore = "2021-06-01";

// The declared spec of si_shares or si_dtc (verbatim text of the Python FIELDS entries); nullptr
// for another name.
[[nodiscard]] const FieldSpec *finra_spec(std::string_view name);

// One as-of CSV row.
struct AsofCsvRow {
  i64 security_id{};
  i64 available_day{};
  f64 value{};
};

// The strict as-of CSV contract: header exactly `security_id,available_at,value` (LF or CRLF), no
// empty line anywhere, at most one trailing newline; security_id a positive decimal i64;
// available_at strict YYYY-MM-DD; value empty or "nan" (any case) -> NaN, else a finite decimal
// matching -?(D+.?D*|.D+)([eE][-+]?D+)?. Rows come back sorted by (security_id, available_day); a
// duplicate pair is an error. Err(ParseError / InvalidArgument).
[[nodiscard]] core::Result<std::vector<AsofCsvRow>> parse_asof_csv(std::string_view bytes);

struct DisseminationSchedule {
  std::vector<i64> dissemination_days;   // ascending, unique
  std::optional<i64> vintage_cutoff_day; // the last dissemination of a settlement before 2021-06-01
  SourceRecord source;
};

// `<finra>/dissemination_schedule.csv` (a header row naming settlement_date and dissemination_date;
// RFC 4180 quoting; blank lines skipped).
[[nodiscard]] core::Result<DisseminationSchedule>
read_dissemination_schedule(const std::filesystem::path &finra_root);

struct FinraStats {
  u64 rows_total{};
  // Manifest key rows_sealed_dropped (renamed with the Python builder, FD-5: the old key named the
  // superseded 2025 boundary).
  u64 rows_sealed{};
  u64 rows_matched_axis{};
  u64 rows_ignored_unknown_id{};
  i64 max_stale_days{kFinraMaxStaleDays};
};

struct VintageRisk {
  std::string rule;
  u64 finite_member_cells{};
  std::optional<std::string> last_session_with_republished_visible_cell;
  std::optional<std::string> first_session_vintage_safe;
};

struct FinraField {
  WrittenField field;
  std::vector<SourceRecord> sources; // the CSV, the producer receipt, the dissemination schedule
  FinraStats stats;
  VintageRisk vintage;
  std::optional<std::string> vintage_safe_from;
};

// Writes `output_dir`/<name>.f64 for name si_shares or si_dtc (output_dir exists).
[[nodiscard]] core::Result<FinraField> build_finra_field(std::string_view name,
                                                         const std::filesystem::path &finra_root,
                                                         const DisseminationSchedule &schedule,
                                                         const RoleAxes &role,
                                                         const std::filesystem::path &output_dir);

} // namespace atx::engine::research::fields
