#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::regime {

enum class CsvFormat : atx::u8 { Fred, Cboe, Yahoo };

// "YYYY-MM-DD" -> unix nanos at UTC midnight. Err(ParseError) on bad shape.
[[nodiscard]] atx::core::Result<atx::i64> date_to_nanos(std::string_view ymd);

// Parse CSV TEXT (no disk). Header row names columns; the date column is the
// first column for all formats. `value_column` selects the numeric column by
// header name. Empty / "." cells -> NaN. Output is sorted-ascending by date and
// deduped (later row wins). Err(ParseError) if the header lacks `value_column`.
[[nodiscard]] atx::core::Result<std::vector<std::pair<atx::i64, atx::f64>>>
parse_series_content(std::string_view content, CsvFormat fmt, std::string_view value_column);

// Read `path`, delegate to parse_series_content. Err(IoError) if unreadable.
[[nodiscard]] atx::core::Result<std::vector<std::pair<atx::i64, atx::f64>>>
parse_series_csv(const std::string &path, CsvFormat fmt, std::string_view value_column);

}  // namespace atx::engine::regime
