#pragma once

#include <iosfwd>
#include <optional>
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
  // hold-band-v1 (v8 R-4, rank hysteresis; aim-partial-v5 only). Unset (default): off, every
  // output unchanged. b in [0, 1]: on every rebalance decision the members' centred tied ranks
  // r_i in [-.5, .5] pass engine::book::apply_hold_band BEFORE the demean: a member keeps its
  // previous pre-demean desired value unless rank_set_i is unset or |r_i - rank_set_i| > b
  // (then it takes r_i and rank_set_i = r_i); then the unchanged demean, gross 1, locate
  // zeroing and neutralization. The state is per name and carried across decisions; a
  // nonmember's state is untouched (a name missing for a day is compared with its old rank on
  // return) and it follows the exit rule unchanged. The state advances on every cadence
  // decision the construction runs, a guard-skipped one included (the registered order: the
  // band acts before the neutralization and its guard). b = 0 is the identity bit for bit (the
  // kernel still runs and carries its state); only b > 0 writes recipe, rule-id and summary
  // keys.
  std::optional<atx::f64> hold_band{};
  // adv-hold-v1 (v8 R-5, ADV holding cap; aim-partial-v5, NAV replay and decide only). 0
  // (default): off, every output unchanged. Q > 0: on every rebalance that proceeds, after the
  // post-processing (the projection), |desired_i| <= Q ADV_i / (aim_leverage NAV) with ADV_i
  // the raw-dollar ADV the execution trade limit reads for this decision's fills (session
  // d + 1: rows [d + 1 - w, d + 1), the NAV replay's liquidity window) and NAV the run's
  // initial_nav; the clipped mass is spread pro rata over the same side's unclipped names,
  // one pass (engine::book::cap_pro_rata_one_pass), and the residual breach is reported. A
  // pass that clips nothing leaves the desired target bit for bit. Writes recipe, rule-id and
  // summary keys.
  atx::f64 adv_hold_q{};
  // inv-vol-v1 (v8 X, lane XCOMB, capacity; aim-partial-v5, NAV replay only; nav --vol-scale
  // inv-vol-v1). false (default): off, every output unchanged. On: on every rebalance decision the
  // members' centred tied ranks r_i pass engine::book::scale_inverse_vol BEFORE the demean:
  // r_i *= median / max(s_i, inv_vol_floor_fraction x median), s_i the sample SD of daily returns
  // the execution cost model reads for this decision's fills (session d + 1: the NAV replay's
  // liquidity window, rows <= d), median over the members with a finite s_i > 0 (a member without
  // one takes the median); then the unchanged demean, gross 1, locate zeroing, neutralization and
  // ADV cap. Not with hold_band (both act between the ranks and the demean). Writes recipe,
  // rule-id and summary keys.
  bool inv_vol{};
  // norm-score-v1 (v8 Y, lane YCOMB, concentration; aim-partial-v5; nav --rank-shape
  // norm-score-v1). false (default): off, every output unchanged. On: on every rebalance decision
  // the members' centred tied ranks are replaced by their van der Waerden normal scores
  // (engine::book::normal_scores: z = Phi^{-1}(mean 1-based rank of the tie block / (N + 1)))
  // BEFORE the demean; then the unchanged demean, gross 1, locate zeroing, neutralization and ADV
  // cap. Not with hold_band or inv_vol (all three act between the ranks and the demean). Writes
  // recipe, rule-id and summary keys.
  bool norm_score{};
  // two-speed-v1 (v8 Y-5, lane YCOMB; aim-partial-v5; nav --two-speed two-speed-v1; it needs the
  // replay's construction state (the target and NAV replays hold one; nav decide refuses) and the
  // saved sleeves). false (default):
  // off, every output unchanged. On: on every rebalance decision the fast and the slow sleeve blends
  // (TargetReplayInput::sleeve_*) each get the construction above (ranks, demean, gross 1, locate
  // zeroing, neutralization); a virtual fast sleeve F moves toward L m_f d_f at theta_f = 1 -
  // 2^(-1/5) (engine/book/two_speed.hpp; 0 off membership), the book's remainder (current - F) is
  // the slow sleeve, moving toward L m_s d_s at trade_fraction, and the desired target is the aim
  // whose aim-partial-v5 step is exactly that netted move:
  //   desired = m_s d_s + (F_prev + (F_next - F_prev) / trade_fraction) / L,
  // m_f the fast themes' mass share of the decision (m_s = 1 - m_f). Not with hold_band, inv_vol or
  // adv_hold. Writes recipe, rule-id and summary keys.
  bool two_speed{};
};
// inv-vol-v1's registered floor: s_i is raised to at least this fraction of the median (so the
// multiplier is at most 4).
inline constexpr atx::f64 inv_vol_floor_fraction = 0.25;
// norm-score-v1 is on.
[[nodiscard]] constexpr bool norm_score_on(const TargetReplayConfig& c) noexcept {
  return c.norm_score;
}
// two-speed-v1 is on.
[[nodiscard]] constexpr bool two_speed_on(const TargetReplayConfig& c) noexcept {
  return c.two_speed;
}
// adv-hold-v1 is on (Q > 0).
[[nodiscard]] constexpr bool adv_hold_on(const TargetReplayConfig& c) noexcept {
  return c.adv_hold_q > 0;
}
// inv-vol-v1 is on.
[[nodiscard]] constexpr bool inv_vol_on(const TargetReplayConfig& c) noexcept { return c.inv_vol; }
// hold-band-v1 is on (the kernel runs) / declared (b > 0: recipe, rule id and summary keys).
[[nodiscard]] constexpr bool hold_band_on(const TargetReplayConfig& c) noexcept {
  return c.hold_band.has_value();
}
[[nodiscard]] constexpr bool hold_band_declared(const TargetReplayConfig& c) noexcept {
  return c.hold_band.has_value() && *c.hold_band > 0;
}
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
  // two-speed-v1 (v8 Y-5; empty otherwise): the fast and slow sleeve blends (the geometry of
  // `signal`) and the fast themes' mass share per date (`dates` entries).
  std::span<const atx::f64> sleeve_fast{}, sleeve_slow{}, sleeve_fast_share{};
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
  // hold-band-v1 (v8 R-4): members whose desired took the fresh rank (first set included) and
  // members that kept their previous desired value. 0 unless the kernel ran; no CSV column.
  atx::usize hold_moved{}, hold_kept{}, hold_first_set{};
  // adv-hold-v1 (v8 R-5), both sides summed, in desired-weight units (x aim_leverage x NAV for
  // dollars): names clipped to their cap and the mass clipped, the mass no unclipped name could
  // take, and the residual breach after the one pass (names, summed and largest excess over
  // the cap). 0 unless the cap pass ran; no CSV column.
  atx::usize adv_clipped{}, adv_residual_names{};
  atx::f64 adv_clipped_mass{}, adv_unplaced_mass{}, adv_residual_mass{}, adv_residual_max{};
  // inv-vol-v1 (v8 X): members scaled, of them filled (no usable s_i) and floored, the median s
  // (NaN when no member had a usable one: the ranks were left as they are) and the largest
  // multiplier. 0 unless the kernel ran (every cadence decision with the rule on); no CSV column.
  atx::usize inv_vol_scaled{}, inv_vol_filled{}, inv_vol_floored{};
  atx::f64 inv_vol_median{}, inv_vol_max_multiplier{};
  // norm-score-v1 (v8 Y): members given a normal score and the largest |score|. 0 unless the
  // kernel ran (every cadence decision with the rule on); no CSV column.
  atx::usize norm_scored{};
  atx::f64 norm_max_abs{};
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
