#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::data {

// The 16 segment field names, in canonical (digest-stable) order.
inline constexpr std::array<std::string_view, 16> kOratsFields = {
    "open", "high", "low", "close", "closePr", "closeUnadjPr", "volume", "shares",
    "returnFactor", "totalReturn", "cumulReturnFactor", "gics", "earnFlag",
    "atmCenI_21d", "atmCenI_126d", "nEarnCnt_5d"};

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
