#pragma once

// atx::impl — generic point-in-time scalar field ingest for `panel --asof-field`
// (R21-3). First consumer: FINRA short interest (si_shares / si_dtc).
//
// CSV CONTRACT (strict, UTF-8, no BOM, no quoting):
//   header exactly            `security_id,available_at,value`
//   security_id               positive i64 in the panel's instrument namespace
//                             (spiderrock.securityID, the same ids as the panel axis)
//   available_at              strict YYYY-MM-DD (the first calendar date the value
//                             is publicly known, possibly after the close)
//   value                     decimal; empty or "nan" (any case) -> NaN; +-inf rejected
// Rows may be unsorted (sorted by (security_id, available_at) on load); LF or CRLF;
// one optional trailing newline. A bad header, an unparsable row, or a duplicate
// (security_id, available_at) is Err(InvalidArgument / ParseError) naming the line.
//
// JOIN RULE ("available_at<session_date"): for a session labelled date(s), the field
// value is the value of the LATEST row whose available_at < date(s) STRICTLY. A row
// becomes visible on the first session AFTER its availability date, so an
// after-hours publication can never leak into that same day's session. Before the
// first visible row the cell is NaN. With max_stale_days > 0, a visible value whose
// age (date(s) - available_at, calendar days) exceeds max_stale_days is NaN. A
// visible row whose value is NaN yields NaN (latest row wins, no skip-back). Rows
// whose security_id is not on the panel axis are counted and ignored.
//
// Cold path, run once per panel build; allocation is fine. Nothing throws.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl {

struct AsofRow {
    atx::i64 security_id{};
    atx::i64 available_day{}; // days since 1970-01-01 (UTC calendar date)
    atx::f64 value{};
};

// Rows sorted ascending by (security_id, available_day), unique on that pair.
struct AsofTable {
    std::vector<AsofRow> rows;
};

struct AsofColumn {
    std::vector<atx::f64> values;           // date-major, sessions * instruments
    atx::usize rows_matched{};              // rows whose security_id is on the axis
    atx::usize rows_ignored_unknown_id{};   // rows whose security_id is not
};

// [A-Za-z_][A-Za-z0-9_]* — the DSL field-identifier grammar.
[[nodiscard]] bool is_dsl_identifier(std::string_view name) noexcept;

// Parse the CSV text under the contract above.
[[nodiscard]] atx::core::Result<AsofTable> parse_asof_csv(std::string_view text);

// Read a CSV file's bytes (bounded; the caller hashes exactly these bytes).
[[nodiscard]] atx::core::Result<std::string> read_asof_bytes(const std::string &path);

// Materialize the join rule onto the panel axes. `session_keys` are the panel's
// ascending ns session labels (date(s) = floor(key / 1 day)); `instrument_ids` are
// canonical positive i64 decimal strings. max_stale_days < 0 is InvalidArgument.
[[nodiscard]] atx::core::Result<AsofColumn>
build_asof_column(const AsofTable &table, std::span<const atx::i64> session_keys,
                  std::span<const std::string> instrument_ids, atx::i64 max_stale_days);

} // namespace atx::impl
