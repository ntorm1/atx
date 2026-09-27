#pragma once

#include <iosfwd>
#include <span>
#include <string>
#include <vector>
#include "strategy_target_replay.hpp"

namespace atx::impl::strategy {
// Self-financing marked-dollar NAV replay of one pinned saved blend under the
// same target rules as replay_targets (baseline-v1 / monthly-budget-v2).
//
// Timing: decide at session d (after its mark), fill at session d+1's close,
// first return row d+2. Decisions [begin, end-2); executions <= end-2; return rows
// [begin+2, end); the final session is valuation only. Results never depend on
// rows at or beyond decision_end (they are only contract-validated); session t
// reads only rows <= t (liquidity: [t-w, t)).
// Order per session t: MARK (borrow on pre-mark shorts, drift, stale/write-off)
// -> EXECUTE (working orders at close t, costs to cash) -> DECIDE (targets in
// decision-NAV dollars). Cash earns 0%, short proceeds stay in cash, no rebate.
//
// Missing prices are detected only from present[t]. A held absent name is carried
// at its last observed adjusted close with its orders blocked; a reprint within K
// sessions realizes the true cumulative return and then executes pending orders;
// K consecutive absences write it off at last mark x (1 + haircut). A reprint after
// a write-off is a diagnostic event only. No lookahead anywhere.

enum class NavCostRule : atx::u8 { FlatBpsV1 = 1, SqrtImpactV1 = 2 };

// Adverse terminal haircuts of scenario S3: copies of the engine's causal default
// book::assumed_missing_price_return(ListingExchange::Unknown, side) (Shumway
// -0.55 long / +0.30 short, both losses). Parity is asserted by the NAV tests.
inline constexpr atx::f64 nav_adverse_long_return = -0.55;
inline constexpr atx::f64 nav_adverse_short_return = 0.30;
// Owner targets reported (never optimized against) in every summary.
inline constexpr atx::f64 nav_sharpe_target = 1.0;
inline constexpr atx::f64 nav_monthly_turnover_target = 0.30;

struct NavScenario {
  std::string id;
  NavCostRule cost{NavCostRule::FlatBpsV1};
  atx::f64 flat_bps{};          // FlatBpsV1 per-dollar rate
  atx::f64 half_spread_bps{};   // SqrtImpactV1 liquidity-row half spread
  atx::f64 commission_bps{};    // SqrtImpactV1 commission
  atx::f64 impact_y{}, impact_delta{0.5};
  atx::f64 max_participation{}; // SqrtImpactV1 cap per session (fraction of ADV)
  atx::f64 annual_borrow_bps{}; // on short dollars, calendar days / 365
  atx::f64 fallback_daily_vol{0.05};
  atx::usize stale_exit_sessions{5}; // K consecutive absent marks -> write-off
  bool adverse_terminal{};           // false: haircut 0; true: nav_adverse_* returns
};
// Fixed research scenarios, all run on every invocation, in this order:
// S1 linear-6bps-stale5-v1, S2 modeled-1bn-stale5-v1 (PRIMARY),
// S3 modeled-1bn-terminal-adverse-v1.
[[nodiscard]] std::vector<NavScenario> fixed_nav_scenarios();
inline constexpr atx::usize nav_primary_scenario_index = 1;

struct NavReplayConfig {
  TargetReplayConfig target{}; // one_way_bps and annual_borrow_bps must be zero
  NavScenario scenario{};
  atx::f64 initial_nav{1'000'000'000.0};
  atx::usize liquidity_window{63}, min_vol_pairs{20};
  atx::u64 max_events{262'144}; // explicit refusal beyond
};
// Borrowed for the synchronous call. Prices and volume are required; members must
// be present. Present cells: close/raw finite > 0, volume finite >= 0.
struct NavReplayInput {
  TargetReplayInput target;
  std::span<const atx::f64> volume;
};

// One row per session t in [decision_begin, decision_end). Row decision_begin
// carries only the first decision. Return fields are relative to the previous
// row's pre-trade NAV: net = gross - trade_cost - borrow, where trade cost is the
// PREVIOUS session's fills (costs of fills at t land in the return of t+1).
struct NavReplayDay {
  atx::usize session_index{};
  atx::i64 session{};
  atx::u32 calendar_month{}; // YYYYMM of this session; the execution month of its fills
  bool decision{}, rebalance{}, executed{}, return_observation{};
  atx::f64 pretrade_nav{}, posttrade_nav{};
  atx::f64 net_return{}, gross_return{}, writeoff_return{}, trade_cost_return{}, borrow_return{};
  atx::f64 mark_pnl_dollars{}, writeoff_dollars{}, borrow_dollars{};
  atx::f64 traded_dollars{}, one_way_turnover{}; // turnover = traded / pre-trade NAV
  atx::f64 trade_cost_dollars{}, linear_cost_dollars{}, impact_cost_dollars{};
  atx::f64 unrationed_cost_dollars{}, unfilled_dollars{};
  atx::usize fills{}, capped_fills{}, blocked_absent{}, blocked_liquidity{};
  atx::usize fallback_vol_fills{}, unrationed_unpriced{};
  atx::f64 planned_turnover{}, planned_forced{}, planned_discretionary{}, applied_fraction{};
  atx::f64 planned_gross{}, planned_net{}; // planned weights after the decision
  atx::f64 month_planned{}, budget_excess{}; // decision-month planned turnover (v2 budget)
  atx::f64 long_dollars{}, short_dollars{}, gross_leverage{}, net_leverage{};
  atx::usize held_names{}, stale_names{};
  atx::f64 stale_long_dollars{}, stale_short_dollars{};
  atx::usize guarded_intervals{};
  atx::f64 cash_ratio{}; // cash / post-trade NAV at end of session
};
enum class NavEventKind : atx::u8 {
  GapResolved = 1, WriteOff = 2, Guarded = 3, ReappearedAfterWriteOff = 4, UnresolvedAtEnd = 5
};
struct NavEvent {
  NavEventKind kind{NavEventKind::GapResolved};
  bool short_side{};
  atx::usize run_length{};
  atx::i64 session{};
  atx::u64 instrument_id{};
  atx::f64 exposure{}; // signed dollars at the event's pre-event mark
  atx::f64 r_adj{}, r_raw{}, haircut{}, pnl{}; // NaN where not observable
};
struct NavBucket {
  atx::usize count{};
  atx::f64 gross_exposure{}, pnl{};
};
struct NavReplayResult {
  std::vector<NavReplayDay> days;
  std::vector<NavEvent> events;
  NavBucket gap_run_1, gap_run_2_4, gap_run_5_plus, written_off, reappeared, unresolved, guarded;
  atx::f64 guard_sensitivity{}; // sum h * (r_raw - r_adj) over realized guarded intervals
  atx::u64 participation_fills{};
  atx::f64 participation_p95{}, participation_max{}; // p95: 0.01-decade histogram upper edge
  atx::f64 max_return_identity_error{}, max_cash_book_error{};
  atx::usize deployment_index{}; // decision_end sentinel when nothing ever filled
};
[[nodiscard]] atx::core::Result<NavReplayResult> replay_nav(const NavReplayInput& in,
                                                          const NavReplayConfig& cfg);

struct NavMonth {
  atx::u32 month{};
  atx::usize execution_sessions{}, traded_sessions{}, decision_sessions{};
  atx::f64 one_way_turnover{}, traded_dollars{}, planned_turnover{};
  bool is_deployment_month{};
};
struct NavYear {
  atx::i32 year{};
  atx::usize observations{};
  atx::f64 net_return{};
};
struct NavSummary {
  atx::usize observations{};
  atx::f64 net_sharpe{}, gross_sharpe{}, mean_daily_net{}, ann_mean{}, ann_vol{}, cagr{};
  atx::f64 max_drawdown{}, total_net_return{}, final_nav{};
  bool hac_defined{};
  atx::f64 hac_t{}; // Bartlett lag 5, small-sample corrected; NaN when undefined
  atx::usize hac_lag{};
  std::vector<NavYear> years;
  std::vector<NavMonth> months; // every month with an execution or a decision session
  atx::usize execution_months{};
  atx::f64 mean_monthly_turnover{}, max_monthly_turnover{};
  atx::f64 mean_monthly_turnover_ex_deployment{}, max_monthly_turnover_ex_deployment{};
  atx::usize months_within_target{}, months_within_target_ex_deployment{};
  bool deployed{};
  atx::i64 deployment_session{};
  atx::f64 deployment_turnover{}, deployment_dollars{};
  atx::f64 total_actual_turnover{}, total_planned_turnover{};
  atx::f64 trade_cost_dollars{}, linear_cost_dollars{}, impact_cost_dollars{};
  atx::f64 unrationed_cost_dollars{}, borrow_dollars{}, writeoff_dollars{};
  atx::f64 summed_trade_cost_return{}, summed_borrow_return{}, summed_writeoff_return{};
  atx::usize fills{}, capped_fills{}, blocked_absent{}, blocked_liquidity{};
  atx::usize fallback_vol_fills{}, unrationed_unpriced{};
  atx::f64 unfilled_dollars{};
  atx::f64 mean_gross_leverage{}, max_gross_leverage{}, max_abs_net_leverage{};
  atx::f64 mean_held_names{}, mean_stale_names{}, max_stale_gross_fraction{};
  atx::usize max_stale_names{};
  atx::f64 min_cash_ratio{};
  bool meets_sharpe_target{}, meets_turnover_target_mean{};
  bool meets_turnover_target_mean_ex_deployment{};
  bool meets_turnover_target_all_months{}, meets_turnover_target_all_months_ex_deployment{};
};
// Return statistics over rows with return_observation; turnover by the calendar
// month of the EXECUTION session (all months include deployment; *_ex_deployment
// exclude the deployment month); planned turnover by decision month.
[[nodiscard]] atx::core::Result<NavSummary> summarize_nav(const NavReplayResult& result);

// Pinned saved blend + role (with volume); all fixed scenarios; exclusive output
// directory: recipe.json, daily_<S>.csv, events_<S>.csv, summary.json LAST.
// All scenarios are computed before the directory is created.
[[nodiscard]] atx::core::Status run_nav_replay(const TargetReplayRunConfig& cfg,
                                               std::ostream& progress);
// argv[0] is the "nav" verb. Rejects --one-way-bps / --annual-borrow-bps.
[[nodiscard]] int dispatch_nav_replay(int argc, char** argv, std::ostream& out,
                                      std::ostream& err);
} // namespace atx::impl::strategy
