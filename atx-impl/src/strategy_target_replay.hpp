#pragma once

#include <iosfwd>
#include <span>
#include <string>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "strategy_price_exposures.hpp"

namespace atx::impl::strategy {
enum class TargetReplayRule : atx::u8 {
  BaselineTargetV1 = 1, MonthlyTargetBudgetV2 = 2, AimPartialV5 = 3
};
// Post-processing of each rebalance decision's desired target.
enum class TargetNeutralize : atx::u8 {
  None = 0, PriceRiskV1 = 1, PriceRiskIndV1 = 2, PriceRiskIndV2 = 3
};
// The industry ids (v6 C5) demean within industry groups and need
// TargetReplayInput::industry: the pinned role field named industry_group_field.
[[nodiscard]] constexpr bool neutralize_by_industry(TargetNeutralize id) noexcept {
  return id == TargetNeutralize::PriceRiskIndV1 || id == TargetNeutralize::PriceRiskIndV2;
}
inline constexpr const char* industry_group_field = "grp_ff12";
// price-risk-ind-v2 exposure windows (v6 C5, review F7): slower vol and log ADV; the
// beta window is price-risk-v1's (252).
inline constexpr atx::usize price_risk_ind_v2_vol_window = 126;
inline constexpr atx::usize price_risk_ind_v2_adv_window = 252;
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
  // price-risk-ind-v1: the same regressors plus within-industry demeaning
  // (neutralize_price_risk_within_groups on TargetReplayInput::industry; same guard).
  // price-risk-ind-v2: ind-v1 whose price_risk must carry vol_window 126 and
  // adv_window 252 (the CLI id sets them; any other windows are refused).
  TargetNeutralize neutralize{TargetNeutralize::None};
  PriceExposureConfig price_risk{};
  atx::f64 neutralize_max_amplification{5.0}, neutralize_max_excluded_share{0.5};
  // No-trade band (0 = off): on a rebalance decision a member with
  // |desired - current| <= band_multiple / N_d (N_d = members at d) keeps its
  // current weight; the others move by the rule's fraction. Banded names are
  // excluded from the monthly-budget-v2 distance.
  atx::f64 band_multiple{};
  // aim-partial-v5 (pre-registered R5'): on every rebalance decision each member moves
  //   next_i = current_i + theta_i * (aim_leverage * desired_i - current_i)
  // unless |aim_leverage * desired_i - current_i| <= dust_multiple / N_d (dust band,
  // 0 = off; a dusted member keeps its weight and is counted in banded_names).
  // theta_i = trade_fraction, or the per-name rate span when one is supplied (T36).
  // A non-rebalance decision (cadence > 1 or a skipped rebalance) trades only forced
  // exits; nonmembers are forced to 0 (exit_rate 1). Under this rule band_multiple must be 0
  // and monthly_budget is ignored; aim_leverage in [1, 2], dust_multiple in [0, 0.5].
  // Every other rule requires aim_leverage 1 and dust_multiple 0 and is unchanged.
  atx::f64 aim_leverage{1.0}, dust_multiple{};
  // Nonmember exit rate (v6 prereg C2), in (0, 1]. 1 (default): every nonmember exits to 0 at
  // once, the rule above bit for bit. r < 1 (aim-partial-v5 with dust_multiple > 0 and
  // prices only): at every decision a nonmember PRESENT at d moves
  //   next_i = current_i * (1 - r),
  // set to 0 when |next_i| <= dust_multiple / N_d (N_d = members at d; every name when
  // N_d = 0), so an exit completes; a nonmember absent at d still exits to 0 at once.
  // Only r < 1 writes keys: recipe exit_rate + exit_rate_rule (aim_partial's nonmember
  // clause then names exit_rate_rule) and summary construction.v5.exit_rate.
  atx::f64 exit_rate{1.0};
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
  // Industry group id per cell (the industry ids only; empty otherwise): an integer
  // in [0, kMaxGroupId] as f64, NaN = unknown (one residual group). Same geometry.
  std::span<const atx::f64> industry{};
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
  // Industry ids only (NeutralizeStats; summary JSON, never CSV columns).
  atx::usize neutralize_groups{}, neutralize_unknown_group_names{};
  atx::usize neutralize_fallback_names{};
  // NAV locate-in-aim (v6 prereg C3): members whose negative desired weight was set to 0
  // before neutralization because they may not be shorted. 0 otherwise; no CSV column.
  atx::usize locate_zeroed{};
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
