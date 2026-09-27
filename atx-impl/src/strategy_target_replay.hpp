#pragma once

#include <iosfwd>
#include <span>
#include <string>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_price_exposures.hpp"

namespace atx::impl::strategy {
enum class TargetReplayRule : atx::u8 { BaselineTargetV1 = 1, MonthlyTargetBudgetV2 = 2 };
// Post-processing of each rebalance decision's desired target.
enum class TargetNeutralize : atx::u8 { None = 0, PriceRiskV1 = 1 };
struct TargetReplayConfig {
  TargetReplayRule rule{TargetReplayRule::BaselineTargetV1};
  atx::usize cadence{5};
  atx::f64 trade_fraction{0.25}, monthly_budget{0.30};
  atx::f64 one_way_bps{}, annual_borrow_bps{};
  atx::u64 max_working_bytes{512ULL << 20};
  // Construction options. The defaults (no neutralization, band 0) reproduce the
  // replay without them bit-for-bit and leave every recipe and CSV unchanged.
  // price-risk-v1: after the tied-rank target, neutralize_price_risk against the
  // decision's trailing beta/vol/log-ADV (price_risk; the CLI uses its defaults).
  // A data refusal (Unavailable), an amplification entry/residual gross above
  // neutralize_max_amplification, or an excluded-row gross share above
  // neutralize_max_excluded_share SKIPS that rebalance: current weights are kept
  // and forced exits still apply. Requires prices and volume.
  TargetNeutralize neutralize{TargetNeutralize::None};
  PriceExposureConfig price_risk{};
  atx::f64 neutralize_max_amplification{5.0}, neutralize_max_excluded_share{0.5};
  // No-trade band (0 = off): on a rebalance decision a member with
  // |desired - current| <= band_multiple / N_d (N_d = members at d) keeps its
  // current weight; the others move by the rule's fraction. Banded names are
  // excluded from the monthly-budget-v2 distance.
  atx::f64 band_multiple{};
};
// All spans are borrowed for this synchronous call, date-major, immutable.
// Prices are optional ALL together. Presence is source presence, independent of
// decision membership. Signal members must be finite; nonmembers must be NaN.
// volume (raw shares, same geometry) is optional; price-risk neutralization needs it.
struct TargetReplayInput {
  atx::usize dates{}, instruments{}, decision_begin{}, decision_end{};
  std::span<const atx::f64> signal;
  std::span<const atx::u8> member;
  std::span<const atx::i64> session_keys;
  std::span<const atx::u64> instrument_ids;
  std::span<const atx::f64> close, raw_close;
  std::span<const atx::u8> present;
  std::span<const atx::f64> volume{};
};
// Per-decision construction record (neutralization outcome and band activity).
enum class NeutralizeOutcome : atx::u8 {
  NotAttempted = 0, Applied = 1, SkippedTooFewNames = 2, SkippedExcludedShare = 3,
  SkippedRefused = 4, SkippedAmplification = 5
};
struct ConstructionDay {
  bool rebalance{}; // effective: a cadence decision that was not skipped
  NeutralizeOutcome neutralize{NeutralizeOutcome::NotAttempted};
  atx::usize neutralize_used{}, neutralize_excluded{}, banded_names{};
  atx::f64 neutralize_excluded_share{}; // excluded-row gross / entry gross
  atx::f64 neutralize_amplification{};  // entry gross / residual gross; NaN if undefined
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
  ConstructionDay construction{};
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
// Price-risk neutralization requires the role (its prices and volume).
[[nodiscard]] atx::core::Status run_target_replay(
    const TargetReplayRunConfig&, std::ostream& progress);
[[nodiscard]] int dispatch_target_replay(
    int argc, char** argv, std::ostream& out, std::ostream& err);
} // namespace atx::impl::strategy
