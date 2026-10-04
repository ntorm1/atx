#pragma once

// atx::engine::research::eval -- the eval verb's computation (platform P9 B2, contract K-P9-5):
// one request in, one JSON document out. Every statistic of record the research verdicts print
// is formed here on the engine (eval/deflated_sharpe.hpp, min_trl.hpp, pbo.hpp, the house rows
// of deflated_sharpe_house.hpp) and the research ledger library (research/ledger), so
// nav_summ.py --engine-stats only reads and prints.
//
// Document (schema atx.research-eval/v1; NaN prints as null):
//   config        the request's parameters
//   ledger        null | {path, exists, lines, chain_head, chain_rule, window_id}
//   cells[]       per --daily series, in order:
//                   label, path, net_moments {sessions, sr_daily, skew, kurtosis},
//                   psr (backtest_integrity.psr_report), min_trl [{sr_star_annual, sessions,
//                   years}],
//                   dsr {house_v1 (nav_summ.ledger_dsr: N = construction trials by the defect
//                        rule, + 1 unless ledgered or a history read; V = the window's cell
//                        variance; legacy variance beside), tot (dsr_total.ledger_rows: the
//                        tot and v8 rows), cross (nav_summ.dsr_rows over the cells, N =
//                        dsr_n), lo (lo_null_dsr, N = dsr_n), onc (effective_n_dsr)},
//                   paired (nav_summ.paired_stats keys plus K-P9-5's se = the LW studentized
//                        SE, p_one, p_two, and the reference; one_minus_rho and degenerate of
//                        ruling B2-R1: a degenerate pair has null memmel_se / t / se / p) or null
//                        with paired_note,
//                   conditional (conditional.hpp on the paired dSR and its Memmel SE) | null
//   onc           null | backtest_integrity.effective_trials (cluster members by label)
//   pbo           null | backtest_integrity.cscv_pbo keys, logits, labels, engine diagnostics
//                 (with constant_candidates: ruling B2-R2, Sharpe 0 in every split)
//   hand[]        DSR_hand rows (dsr_total.hand_row) of --hand books
//   conditional   {threshold, alpha, se_basis, rows (the --step lineage, then the cells),
//                  cumulative {cells, raw_sum, adjusted_sum, null_bias_sum}} -- printed only
//   psr, min_trl, dsr, paired   K-P9-5's top level: the first cell's blocks
//
// The ledger is read, verified and never written (ruling SQL-9).

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/data/research_window.hpp"
#include "atx/engine/research/eval/paired.hpp"
#include "atx/engine/research/ledger/ledger_file.hpp"

namespace atx::engine::research::eval {

inline constexpr std::string_view kEvalSchema = "atx.research-eval/v1";
inline constexpr usize kV8Draws = 4999U; // v8-prereg item 4
inline constexpr u64 kV8Seed = 20260929U;
inline constexpr usize kDefaultDsrN = 10U; // R6'
inline constexpr usize kDefaultOncInit = 10U;
inline constexpr u64 kDefaultOncSeed = 20260927U;
inline constexpr f64 kDefaultPsrAlpha = 0.05;
inline constexpr f64 kDefaultConditionalAlpha = 0.05;

struct LabeledPath {
  std::string label;
  std::string path;
};

struct HandBook {
  std::string trial_id;
  std::string path;
};

// One accepted step of a lineage given by value (its paired dSR and SE).
struct LineageStep {
  std::string label;
  f64 raw{};
  f64 se{};
};

struct EvalRequest {
  std::vector<LabeledPath> daily;          // the cells; the first is the scored cell
  std::optional<std::string> paired;       // reference series of every cell
  std::vector<LabeledPath> paired_of;      // per-cell reference (label -> path), wins
  std::string protocol{"v7"};
  PairedConfig paired_config;              // v8: 4,999 draws, seed 20260929 unless given
  usize dsr_n{kDefaultDsrN};
  std::vector<f64> psr_benchmarks{0.0, 0.5};
  f64 psr_alpha{kDefaultPsrAlpha};
  bool onc{false};
  std::vector<LabeledPath> onc_series;     // default: the cells
  usize onc_init{kDefaultOncInit};
  u64 onc_seed{kDefaultOncSeed};
  bool pbo{false};
  std::vector<LabeledPath> pbo_series;     // default: the cells
  usize pbo_blocks{16U};
  std::optional<std::string> ledger;
  // An absent ledger reads as empty (ledger_read) or is refused (dsr_total.read_ledger).
  atx::engine::research::ledger::MissingLedger ledger_missing{
      atx::engine::research::ledger::MissingLedger::Empty};
  std::string window_id{std::string(atx::engine::data::kResearchWindowId)};
  // Trusted-caller inputs (eval_cli.hpp): the + 1 of house_v1 / tot follows them as given.
  std::vector<LabeledPath> trials;         // label -> trial_id
  std::vector<std::string> history;        // labels scored as history reads (Ruling E-41)
  std::vector<HandBook> hands;
  std::vector<LineageStep> steps;
  f64 threshold{0.0};
  f64 conditional_alpha{kDefaultConditionalAlpha};
};

// InvalidArgument on an inconsistent request (no cell, a repeated label, dsr_n < 2, an
// unknown label, --hand without a ledger, too few common sessions for ONC / PBO); the series,
// ledger and statistic errors otherwise.
[[nodiscard]] core::Result<nlohmann::json> evaluate(const EvalRequest &request);

// The verb's text print of a document (the scored cell, the conditional block, ONC and PBO).
[[nodiscard]] std::string eval_text(const nlohmann::json &doc);

} // namespace atx::engine::research::eval
