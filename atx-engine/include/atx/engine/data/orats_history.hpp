#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::data {

// The 16 SEGMENT field names, in canonical (digest-stable) order.
//
// IMPORTANT: these are the on-disk atx-tsdb SEGMENT field names, which are capped
// at 15 chars (the segment format NUL-pads into a 16-byte buffer, so a 16th+ char
// is silently truncated). The TSV SOURCE column for index 10 is "cumulReturnFactor"
// (17 chars) — too long for a segment name — so the segment field is named
// "cumReturnFactor" (15 chars). resolve_header() maps it back to the real TSV
// header name "cumulReturnFactor" (the same special-case it applies for gics->GICS).
inline constexpr std::array<std::string_view, 16> kOratsFields = {
    "open", "high", "low", "close", "closePr", "closeUnadjPr", "volume", "shares",
    "returnFactor", "totalReturn", "cumReturnFactor", "gics", "earnFlag",
    "atmCenI_21d", "atmCenI_126d", "nEarnCnt_5d"};

// Compile-time guard: an ORATS SEGMENT field name must fit the atx-tsdb on-disk
// field-name capacity (kFieldNameLen=16 NUL-padded -> max 15 usable chars), else
// it would silently truncate when written by build_from_long and never resolve on
// read-back. Keep this in lockstep with atx::tsdb::kFieldNameLen.
[[nodiscard]] consteval bool atx_orats_field_names_fit() {
  for (std::string_view sv : kOratsFields) {
    if (sv.size() > 15) return false; // atx-tsdb kFieldNameLen(16) - 1
  }
  return true;
}
static_assert(atx_orats_field_names_fit(),
              "an ORATS segment field name exceeds the atx-tsdb 15-char field-name "
              "limit (silent truncation)");

struct OratsLoadConfig {
  std::string zip_path;       // .../tbltickerhistory3_10y.zip
  std::string out_dir;        // data/orats_history_1d (created if absent)
  atx::i64 min_date_nanos;    // inclusive floor; rows with tradingDate < this are dropped
  atx::i64 created_at_nanos;  // stamped into every segment header (provenance)
};

struct OratsLoadStats {
  atx::i64 rows_read{};           // data rows seen (excludes header)
  atx::i64 rows_kept{};           // rows >= min_date with a parseable date+securityID
  atx::i64 rows_filtered{};       // rows dropped by the date floor
  atx::i64 rows_malformed{};      // rows dropped for a bad date / missing securityID
  atx::i64 dates_written{};       // .seg files written
  atx::i64 distinct_securities{}; // unique securityIDs across kept rows
};

// Stream the zip, project kOratsFields, filter by date, and write one sealed
// `<out_dir>/YYYY-MM-DD.seg` per trading date (symbol name = securityID) plus
// `<out_dir>/_symbology.parquet` (securityID, ticker_tk, todayTicker) and
// `<out_dir>/_manifest.json`. The input MUST be date-major (non-decreasing
// tradingDate); a date regression fails closed with Err(InvalidArgument).
[[nodiscard]] atx::core::Result<OratsLoadStats> load_orats_history(const OratsLoadConfig &cfg);

namespace detail {
// "YYYY-MM-DD" -> midnight-UTC unix nanos; std::nullopt on a malformed date.
[[nodiscard]] std::optional<atx::i64> date_to_nanos(std::string_view ymd);

// Resolve the projected column indices from the header line (tab-separated names).
// Err(ParseError) if any required column (tradingDate, securityID, ticker_tk,
// todayTicker, GICS, or any kOratsFields source) is absent. `gics`/`earnFlag` map
// to header names "GICS"/"earnFlag"; the rest map by identical name.
struct ColumnIndex {
  int tradingDate{-1}, securityID{-1}, ticker_tk{-1}, todayTicker{-1};
  std::array<int, 16> field{}; // index in the TSV for each kOratsFields entry
};
[[nodiscard]] atx::core::Result<ColumnIndex> resolve_header(std::string_view header_line);
} // namespace detail
} // namespace atx::engine::data
