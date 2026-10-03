#pragma once

// atx::engine::research::ledger -- era pooling and the trial identities of the research ledger
// (platform v8 task H-1; atx-engine/tools/era_pool.py, backtest_integrity.trial_id). Platform
// P9 B2 (migration slice 8: "ledger: counting, DSR inputs, era pooling").
//
//   check_era_id        an era id names files: [A-Za-z0-9_]+
//   check_era_windows   era windows [begin, end) in date order: ids, emptiness, the seal, TRAIN
//                       straddle, a TRAIN era past the TRAIN end, order and overlap
//   segment_starts      the start row of each era in the pooled series (sessions strictly
//                       increasing, each era after the previous one's last session)
//   weighted_mean       sum w v / sum w (one era: its value, no arithmetic)
//   pooled_sha256       one era: its SHA-256; several: SHA-256 of the compact sorted-key JSON
//                       {"parts": [...], "schema": "atx.era-pool/v1"}
//   pool_label          "pool:" + the era dirs joined by commas (forward slashes)
//   trial_id            first 16 hex digits of SHA-256 of the compact JSON [kind, series_sha256]
//
// The JSON texts are Python's json.dumps(..., separators=(",", ":")) of ASCII strings; a
// non-ASCII string is refused (InvalidArgument) rather than escaped differently.

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::ledger {

inline constexpr std::string_view kEraPoolSchema = "atx.era-pool/v1";
inline constexpr std::string_view kPoolPrefix = "pool:";

struct EraWindow {
  std::string id;
  i64 begin_ns{};
  i64 end_ns{}; // exclusive
};

struct EraSessions {
  std::string id;
  std::vector<i64> sessions;
};

[[nodiscard]] core::Status check_era_id(std::string_view id);
[[nodiscard]] core::Status check_era_windows(std::span<const EraWindow> windows,
                                             i64 train_begin_ns, i64 train_end_ns, i64 seal_ns);
[[nodiscard]] core::Result<std::vector<usize>> segment_starts(std::span<const EraSessions> eras);
[[nodiscard]] core::Result<f64> weighted_mean(std::span<const f64> values,
                                              std::span<const f64> weights);
[[nodiscard]] core::Result<std::string> pooled_sha256(std::span<const std::string> parts);
[[nodiscard]] std::string pool_label(std::span<const std::string> dirs);
[[nodiscard]] core::Result<std::string> trial_id(std::string_view kind,
                                                 std::string_view series_sha256);
// trial_id(kind, pooled_sha256(parts)).
[[nodiscard]] core::Result<std::string> pooled_trial_id(std::string_view kind,
                                                        std::span<const std::string> parts);

// Python's json.dumps of one ASCII string (quotes, \" \\ \b \f \n \r \t, other controls as
// \u00XX); InvalidArgument for a non-ASCII byte.
[[nodiscard]] core::Result<std::string> python_json_string(std::string_view text);

// YYYY-MM-DD of a day count since 1970-01-01 (proleptic Gregorian, UTC).
[[nodiscard]] std::string iso_date_from_days(i64 days);

} // namespace atx::engine::research::ledger
