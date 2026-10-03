#pragma once

// Private to research/ledger: one ledger line as a JSON value, read the way Python's
// json.loads reads it. Python writes NaN / Infinity / -Infinity tokens (json.dumps'
// allow_nan default); nlohmann does not read them, so they are replaced by null outside
// strings first. No rule of the ledger reads a non-finite value as a number (dsr_variance
// keeps finite SRs only), so null and NaN select the same lines.

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"

namespace atx::engine::research::ledger::detail {

// The text with every NaN, Infinity and -Infinity token outside a string read as null.
[[nodiscard]] std::string null_nonfinite_tokens(std::string_view text);

// The line's JSON value; ParseError naming `where` when it is not JSON.
[[nodiscard]] core::Result<nlohmann::json> parse_json_line(std::string_view line,
                                                           const std::string &where);

} // namespace atx::engine::research::ledger::detail
