#pragma once

// atx::engine::research::ledger -- the trial-counting rules of the research ledger: which
// lines add trials to N, the N of the deflated Sharpe ratio, N_tot, and the window variance V.
// One C++ copy of backtest_integrity.py's rules (platform v8 prereg items 2, 3 and 7; reviews
// C-3, C-5, F-1; Rulings E-33, E-41; task H-1) and dsr_total.trial_breakdown (PM7-1/2/7).
// Platform P9 B2 (migration slice 8). The Python writer keeps writing the lines (SQL-9); these
// functions read them exactly as the Python readers do:
//
//   event line        kind protocol | defect | validation: no cell, adds 0
//   campaign line     kind mining-campaign: adds 0; its registry.count is printed beside N
//   era line          has the key era_of: an era shard of a pool, adds 0
//   pool line         window.label == "POOL": one trial for all its eras
//   history line      key era, an era line, an "ERA ..." window or a pool line: never a TRAIN cell
//   history read      a pool line beginning before TRAIN, or an era block (not a pool line)
//                     whose era_of names no pool line: adds 0 (Ruling E-41)
//   invalid cell      defect.invalid truthy on a cell line, or named by a defect line
//   trial_counts      0 for event / window re-run / blind re-run / era / campaign / history
//                     read lines and for an invalid cell no blind or returns re-run names;
//                     else int(count) (default 1)
//   ledger_n          sum of the kind's counts (a line without kind is that kind), + 1 when
//                     the scored cell has no line yet
//   N_tot             every line's count (any kind) + the campaigns' registry counts
//   dsr_variance      ddof-1 variance (numpy order) of s2_net_sr / sqrt(252) over the kind's
//                     finite-SR lines on the window, invalid / replaced and history lines out;
//                     the legacy variance (lines without window_id) beside it
//
// A ledger value's Python identity is its JSON value; sets of trial ids compare dumped JSON.
// Errors: a count or registry count that Python's int() refuses is InvalidArgument.

#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::research::ledger {

inline constexpr std::string_view kConstruction = "construction";
inline constexpr std::string_view kMiningCampaign = "mining-campaign";
inline constexpr std::string_view kDefect = "defect";
inline constexpr std::string_view kValidation = "validation";
inline constexpr std::string_view kProtocol = "protocol";
inline constexpr std::string_view kPoolWindow = "POOL";
inline constexpr f64 kAnnualization = 252.0;

using Record = nlohmann::json;
using Records = std::span<const Record>;

[[nodiscard]] bool is_event(const Record &rec);
[[nodiscard]] bool is_campaign(const Record &rec);
[[nodiscard]] bool is_era_line(const Record &rec);
[[nodiscard]] bool is_pool_line(const Record &rec);
[[nodiscard]] bool is_history_line(const Record &rec);
// window.first_session (a string) sorts before the research window's TRAIN begin date.
[[nodiscard]] bool begins_before_train(const Record &rec);

// Indices of the history reads (Ruling E-41), ledger order.
[[nodiscard]] std::vector<usize> history_read_lines(Records records);
// The invalid cells' trial ids (dumped JSON values).
[[nodiscard]] std::set<std::string> invalid_ids(Records records);
// Trials each line adds to N, ledger order.
[[nodiscard]] core::Result<std::vector<i64>> trial_counts(Records records);
// The N that gates: the kind's trials + 1 when the scored cell is not in the ledger.
[[nodiscard]] core::Result<i64> ledger_n(Records records, bool scored_in_ledger,
                                         std::string_view kind = kConstruction);
// Indices of the lines whose result V leaves out (invalid or blind-replaced, unless a
// returns re-run names them).
[[nodiscard]] std::vector<usize> excluded_lines(Records records);
// Sum of the campaign lines' registry.count (never part of N).
[[nodiscard]] core::Result<i64> campaign_registry_count(Records records);

// N_tot and its parts (dsr_total.trial_breakdown). by_kind is sorted by kind text; a line
// without kind is a construction line, a null kind reads "null".
struct TrialBreakdown {
  std::vector<std::pair<std::string, i64>> by_kind;
  i64 ledger_trials{};
  i64 campaign_registry{};
  i64 n_tot{};
};
[[nodiscard]] core::Result<TrialBreakdown> trial_breakdown(Records records);

// backtest_integrity.dsr_variance.
struct DsrVariance {
  i64 n{};
  std::string window_id;
  std::optional<f64> variance_sr;
  usize cells{};
  std::string variance_source;
  std::optional<f64> legacy_variance_sr;
  usize legacy_cells{};
};
[[nodiscard]] core::Result<DsrVariance> dsr_variance(Records records, std::string_view window_id,
                                                     std::string_view kind = kConstruction);

// The trial ids of the construction lines (string ids only).
[[nodiscard]] std::set<std::string> construction_ids(Records records);
// Lines up to and including the first construction line with this trial id; nullopt when
// none (DSR_hand refuses such a book).
[[nodiscard]] std::optional<usize> construction_prefix_length(Records records,
                                                              std::string_view trial_id);

} // namespace atx::engine::research::ledger
