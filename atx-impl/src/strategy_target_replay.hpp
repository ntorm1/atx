#pragma once

#include <iosfwd>
#include <span>
#include <string>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::impl::strategy {
enum class TargetReplayRule : atx::u8 { BaselineTargetV1 = 1, MonthlyTargetBudgetV2 = 2 };
struct TargetReplayConfig {
  TargetReplayRule rule{TargetReplayRule::BaselineTargetV1};
  atx::usize cadence{5};
  atx::f64 trade_fraction{0.25}, monthly_budget{0.30};
  atx::f64 one_way_bps{}, annual_borrow_bps{};
  atx::u64 max_working_bytes{512ULL << 20};
};
// All spans are borrowed for this synchronous call, date-major, immutable.
// Prices are optional ALL together. Presence is source presence, independent of
// decision membership. Signal members must be finite; nonmembers must be NaN.
struct TargetReplayInput {
  atx::usize dates{}, instruments{}, decision_begin{}, decision_end{};
  std::span<const atx::f64> signal;
  std::span<const atx::u8> member;
  std::span<const atx::i64> session_keys;
  std::span<const atx::u64> instrument_ids;
  std::span<const atx::f64> close, raw_close;
  std::span<const atx::u8> present;
};
struct TargetReplayDay {
  atx::usize decision{}, entry{}, endpoint{}; // dates sentinel if beyond input
  atx::i64 session{};
  atx::u32 calendar_month{}; // YYYYMM of DECISION session, never executed turnover
  atx::f64 turnover{}, forced_turnover{}, discretionary_turnover{}, deployment_turnover{};
  atx::f64 month_turnover{}, budget_excess{}, applied_fraction{};
  atx::f64 gross{}, net{}, long_weight{}, short_weight{}, max_abs_weight{}, effective_names{};
  atx::usize held_names{};
  bool return_mature{}, return_complete{};
  atx::f64 observed_return_component{}, missing_long{}, missing_short{}, missing_gross{};
  atx::usize missing_names{}, guarded_names{};
  atx::f64 modeled_trade_cost{}, modeled_borrow_cost{};
  atx::f64 complete_gross_return{}, complete_net_return{}; // NaN if not complete
};
struct TargetReplayResult {
  std::vector<TargetReplayDay> days;
  atx::f64 total_turnover{}, forced_turnover{}, discretionary_turnover{};
  atx::f64 deployment_turnover{};
  atx::usize deployment_date{}; // dates sentinel for flat result
};
// O(N) mutable scratch + O(D) result; envelope EXCLUDES caller-owned input.
// Calendar budget charges initial deployment and forced exits first. Exits are
// never deferred to satisfy budget; unavoidable excess is reported. The partial
// target proxy has no price drift, survivor renormalization, fills or cash book.
// Optional rough return is target[d] * close[d+2]/close[d+1]-1. It is explicitly
// a constant-weight approximation, NOT a self-financing NAV/Sharpe estimator.
// Missing/guarded held returns make that complete-day return unavailable; their
// long/short/gross exposure remains visible alongside the observed component.
[[nodiscard]] atx::core::Result<TargetReplayResult> replay_targets(
    const TargetReplayInput&, const TargetReplayConfig&);

struct TargetReplayRunConfig {
  std::string combined_path, combined_sha256, output_directory;
  std::string role_path, role_sha256; // optional paired pin for rough returns
  TargetReplayConfig target{};
};
// One externally pinned saved blend; no DSL evaluation or orientation fitting.
// Exclusive output directory; summary.json is published only after CSV closes.
[[nodiscard]] atx::core::Status run_target_replay(
    const TargetReplayRunConfig&, std::ostream& progress);
[[nodiscard]] int dispatch_target_replay(
    int argc, char** argv, std::ostream& out, std::ostream& err);
} // namespace atx::impl::strategy
