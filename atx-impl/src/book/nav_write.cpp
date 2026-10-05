// NAV replay: recipe / summary JSON and the CSV writers.
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"

namespace atx::impl::strategy {
using namespace nav_book;
namespace nav_book {
Json finite_or_null(f64 x) { return std::isfinite(x) ? Json(x) : Json(nullptr); }
} // namespace nav_book
namespace {
Json bucket_json(const NavBucket& b) {
  return Json{{"count", b.count}, {"gross_exposure_dollars", b.gross_exposure},
              {"pnl_dollars", b.pnl}};
}
constexpr const char* timing_declaration =
    "decide at session d after its mark (modeled +22h mark, +23h decision); fill at the close "
    "of d+1; first return row d+2; decisions [begin,end-2), executions <= end-2, return rows "
    "[begin+2,end) (last two scored decisions dropped vs target replay); cadence phase relative "
    "to begin; final session valuation only, no liquidation";
constexpr const char* accounting_declaration =
    "self-financing marked-dollar book; per session MARK->EXECUTE->DECIDE; cash 0%, no short "
    "rebate, short proceeds stay in cash (excess-return accounting: the swap benchmark "
    "cancels against NAV collateral); fill costs debited to cash at the fill and land in "
    "the next return row; financing on pre-mark dollars x calendar days / the scenario day "
    "count (scenarios[].financing): borrow = the short leg, long_financing = the long leg; "
    "r_t = NAVpre_t/NAVpre_{t-1}-1 = gross(drift+write-off) - trade_cost(t-1) - borrow - "
    "long_financing, checked; cash+holdings reconciled to NAV every session";
constexpr const char* financing_declaration =
    "flat-short-v0: flat_short_bps on every pre-mark short dollar x calendar days/365 (the "
    "legacy accrual); tiered-swap-v1 (prime-broker portfolio swap, benchmark cancelled): "
    "long_spread_bps on every pre-mark long dollar + (short_spread_bps + tier fee) on every "
    "pre-mark short dollar x calendar days/day_count, the fee from the name's borrow tier at "
    "the latest decision <= t-1 (a short that migrates into special pays special until it "
    "exits); block_special_shorts: at DECIDE, after the rule (band/partial/budget), a "
    "special-tier name's next weight = max(next, min(current, 0)) (no new or increased "
    "short; reductions pass), a kept working order is clamped likewise; blocked short "
    "dollars reported; the rule's planned turnover/budget are before the block";
constexpr const char* borrow_tier_declaration =
    "engine estimate_borrow_tier public-predictor-prior-v1 with its default thresholds "
    "(market cap < 1e9 USD, raw price < 5 USD, SI ratio >= 0.10, IPO age < 365 calendar days; "
    "0 flags GC, 1 warm, 2+ special), computed once per decision d for every name from rows "
    "<= d and shared by every scenario (fees are per scenario): market cap = shares_out[d] x "
    "raw_close[d] (mktcap_lagged not used: its presence is not point in time); raw price = "
    "raw_close[d]; SI ratio = si_shares[d] / shares_out[d] (shares_out >= float: understated); "
    "IPO age = calendar days since the first present role session (present at role session "
    "0: seasoned); shares_out outside [1e5, 5e10], non-finite or negative si_shares, an "
    "absent name or an engine Unavailable -> warm, counted as missing; available_at = "
    "session + 22h (fields visibility mark), decision = session + 23h";
constexpr const char* target_declaration =
    "tied-rank;neutral-desired-gross1;unchanged target-replay rules on drifted marked weights "
    "(held/post-trade NAV);working orders fixed in decision-NAV dollars until filled, replaced "
    "at a rebalance, or cancelled";
constexpr const char* missing_declaration =
    "causal stale-carry K-session write-off: absence only from present[t]; held absent names "
    "carried at the last observed adjusted close with orders blocked; a reprint within K "
    "realizes the cumulative adjusted return, then pending orders execute at that close; K "
    "consecutive absences write off at last mark x (1+haircut); a reprint after write-off is a "
    "diagnostic event only; still stale at the final session: valued at the stale mark and "
    "reported unresolved; entries on absent names unfilled";
constexpr const char* guard_declaration =
    "adjusted |log| > 1.5 or > |raw log| + 0.10 over the realized (possibly gapped) interval "
    "is flagged and still realized at the adjusted return; sensitivity sum h*(r_raw - r_adj) "
    "reported";
constexpr const char* liquidity_declaration =
    "execution t: ADV = sum over [t-w,t) of present raw_close*volume / w (absent = 0, never "
    "rescaled); sigma = sample SD of adjusted simple returns over adjacent present unguarded "
    "pairs (k-1,k), k in [t-w,t), if >= min_vol_pairs, else fallback; ADV = 0 -> no fill "
    "(liquidity-blocked)";
// The per-name aim_partial text is head + detail::nonmember_exit_clause + tail ("nonmembers
// exit to 0" at exit_rate 1: the declaration byte for byte as before the split).
constexpr const char* per_name_rate_head =
    "rebalance decision: each member moves next = current + theta_i * (aim_leverage * desired "
    "- current) unless |aim_leverage * desired - current| <= dust_multiple / N_d (N_d = "
    "members at d; 0 = off), which keeps its weight and is counted in banded_names; "
    "non-rebalance decisions keep member weights; ";
constexpr const char* per_name_rate_tail =
    "; rate per-name-v1: "
    "theta_i = clip(sqrt(rate_rra * sigma_i^2 * ADV_i / (rate_lambda * NAV_d)), rate_min, "
    "rate_max), NAV_d the book's pre-trade NAV at d, sigma_i and ADV_i the name's liquidity "
    "row of session d (window [d-w, d), the liquidity declaration); ADV <= 0, sigma <= 0 or "
    "fewer than min_vol_pairs pairs -> rate_min; trade_fraction (theta) unused; "
    "applied_fraction = the members' mean rate; monthly_budget unused; "
    "construction.v5.rate_stats: one sample per member per decision, quantiles exact at the "
    "clip bounds, else the upper edge of a 4096-bin histogram of [rate_min, rate_max]";
constexpr const char* order_basis_declaration =
    "delta (v6 prereg C1): at DECIDE a name whose plan changes to a NONZERO weight gets a delta "
    "order: decision-NAV dollars (planned - current) x NAVpost = planned x NAVpost - held_d; "
    "at EXECUTE t it requests that delta minus the dollars already filled on it (never "
    "drift-adjusted), so price drift between decision and fill rides and the next DECIDE "
    "re-plans from the drifted holding at theta; a complete fill lands on planned x NAVpost "
    "plus the drift since the decision (exactly the target without drift); a capped residual "
    "keeps its remaining delta until filled, replaced by a decision that changes the name's "
    "plan, or cancelled (a rebalance with an unchanged plan, a flat nonmember, a write-off); "
    "a zero plan (every exit) and, under block_special_shorts, any order on a special-tier "
    "name are target orders (a kept delta order on a name that turns special becomes the "
    "target order it amounts to, then is clamped); target (default): the order is the "
    "decision-NAV dollar target and EXECUTE requests target - held";
constexpr const char* locate_in_aim_declaration =
    "v6 prereg C3: at each rebalance decision, after the tied-rank target and before the "
    "neutralization, a member in the special borrow tier of that decision (borrow_tiers, "
    "rows <= d, shared by every book) with a negative desired weight is set to 0; the "
    "neutralization then regresses the zeroed target (net and beta re-balanced over its "
    "used rows) and rescales it to the zeroed target's gross; applies to every book of the "
    "run (the construction is shared); each book's block_special_shorts rule still applies "
    "after the target rule as a safety net; requires the borrow fields and a neutralizing "
    "construction";
constexpr const char* warm_start_declaration =
    "v8 D-0 (review C-7): every book decides and trades from role row score_begin - K (K = "
    "warm_start_sessions <= score_begin: never before the role's first row) under every rule, "
    "cost and financing of this recipe; the MARK of session score_begin closes the warm-up "
    "and is not reported; each book is then resized to initial_nav (holdings, cash, working "
    "orders, delta anchors and written-off exposures scaled by one factor; weights "
    "unchanged) and every reported quantity restarts: rows, events, missing/guard buckets, "
    "participation, rate statistics and accounting checks cover sessions >= score_begin "
    "only; row score_begin is the base row (its fills and decision are scored), return rows "
    "are [score_begin+1, end); a deployment in the warm-up is not a scored deployment; the "
    "cadence phase stays relative to score_begin";
constexpr const char* label_role_declaration =
    "v8 E-25 (Ruling E-25): the books are MARKED by the pinned label role: MARK at t reads its "
    "present, close and raw_close (drift and gap P&L, the guard, stale carry, the K-session "
    "write-off, reprints), so a termination it prices (prepare_recent_research.py "
    "delisting-return-on-termination-v1: present at T = L+1 at close[L] x (1 + r)) realizes r "
    "at T and is then carried and written off at that mark; financing accrues on the marked "
    "dollars as always. Every other input is --role's: the signal and membership, the "
    "construction (tied ranks, hold band, ADV cap, neutralization exposures), the borrow tiers, "
    "the liquidity and ADV windows (volume) and execution tradability (present): no label value "
    "enters a decision input or a trade limit, and no order fills on a label-only cell (an "
    "imputed price, volume 0, its cause classifiable after T); every fill is on a session where "
    "--role is present, where the label close is --role's bit for bit. The book's own NAV and "
    "drifted weights, which DECIDE reads, include every realized return, a terminal one "
    "included. Admission, from the two pinned manifests before any payload: score_end_ns (when "
    "declared) not past the research seal; every manifest key but files and universe equal to "
    "--role's (dates, instruments, score window, source, projection, membership recipe); the "
    "same file set, every extent equal and every SHA-256 equal except close.f64, raw_close.f64, "
    "volume.f64 and present.u8 (the payloads --delisting-returns patches: sessions and ids are "
    "shared); member.u8 shared, or differing only by the members the label role declares "
    "cleared on termination sessions (universe.delisting.returns_applied true and "
    "applied.members_cleared_on_termination_session N > 0, and then score_member_counts may "
    "differ); universe absent in both, or the same id, base role (manifest, member), "
    "identity-bridge and SIC-events pins and --role's delisting-stage pin when it has one. "
    "Payloads: no role session at or after the seal; --role's presence extended only; member & "
    "present & close > 0 equal to --role's on every cell; a differing member.u8 differs from "
    "--role's on exactly the N declared cells, each one --role has absent and keeps a member "
    "and the label role presents and clears";
} // namespace
namespace nav_book {
// Output label of a book: the trading id alone without fields (the legacy names),
// "<trading>+<financing>" in the financing matrix.
std::string scenario_label(const NavScenario& s, bool tiered) {
  return tiered ? s.id + "+" + s.financing.id : s.id;
}
} // namespace nav_book
namespace {
Json financing_json(const NavFinancing& f) {
  const bool flat = f.rule == NavFinancingRule::FlatShortV0;
  return Json{{"id", f.id}, {"rule", flat ? "flat-short-v0" : "tiered-swap-v1"},
      {"flat_short_bps", f.flat_short_bps}, {"long_spread_bps", f.long_spread_bps},
      {"short_spread_bps", f.short_spread_bps},
      {"tier_fee_bps", {{"gc", f.gc_bps}, {"warm", f.warm_bps}, {"special", f.special_bps}}},
      {"day_count", "ACT/" + std::to_string(f.day_count)},
      {"block_special_shorts", f.block_special_shorts}};
}
Json scenario_recipe(const NavScenario& s, bool primary, bool tiered) {
  const Json participation = std::isfinite(s.max_participation) ? Json(s.max_participation)
                                                                : Json("uncapped");
  return Json{{"id", scenario_label(s, tiered)}, {"trading_scenario", s.id},
      {"financing_scenario", s.financing.id}, {"primary", primary},
      {"cost_rule", s.cost == NavCostRule::FlatBpsV1 ? "flat-bps-v1" : "sqrt-impact-v1"},
      {"flat_bps", s.flat_bps}, {"half_spread_bps", s.half_spread_bps},
      {"commission_bps", s.commission_bps}, {"impact_y", s.impact_y},
      {"impact_delta", s.impact_delta}, {"max_participation", participation},
      {"financing", financing_json(s.financing)},
      {"fallback_daily_vol", s.fallback_daily_vol},
      {"stale_exit_sessions", s.stale_exit_sessions},
      {"terminal_haircut_long", s.adverse_terminal ? nav_adverse_long_return : 0.0},
      {"terminal_haircut_short", s.adverse_terminal ? nav_adverse_short_return : 0.0}};
}
} // namespace
namespace nav_book {
// fields: the pinned borrow-field binding, or null without --fields. label: the summary's
// label_role record (v8 E-25), or null without --label-role.
Json nav_recipe(const TargetReplayRunConfig& cfg, const NavReplayConfig& base,
                std::span<const NavScenario> scenarios, const NavTurnoverLimits& limits,
                const Json& fields, const Json& label) {
  const bool tiered = !fields.is_null();
  Json list = Json::array();
  for (usize k = 0; k < scenarios.size(); ++k)
    list.push_back(scenario_recipe(scenarios[k], k == nav_primary_scenario_index, tiered));
  auto j = Json{{"schema", "atx.dsl-nav-replay/v1"},
      {"combined_sha256", cfg.combined_sha256}, {"role_sha256", cfg.role_sha256},
      {"rule", detail::construction_rule_id(cfg.target)}, {"cadence", cfg.target.cadence},
      {"trade_fraction", cfg.target.trade_fraction},
      {"monthly_budget", cfg.target.monthly_budget},
      {"initial_nav", base.initial_nav}, {"liquidity_window", base.liquidity_window},
      {"min_vol_pairs", base.min_vol_pairs}, {"max_events", base.max_events},
      {"max_working_bytes", cfg.target.max_working_bytes}, {"scenarios", std::move(list)},
      {"primary_scenario", scenario_label(scenarios[nav_primary_scenario_index], tiered)},
      {"sharpe_target", nav_sharpe_target},
      {"monthly_turnover_target", nav_monthly_turnover_target},
      {"timing", timing_declaration}, {"accounting", accounting_declaration},
      {"target", target_declaration}, {"turnover", turnover_definition},
      {"missing", missing_declaration}, {"guard", guard_declaration},
      {"liquidity", liquidity_declaration}, {"desired_target_postprocess", "none"},
      {"cost_input_status", tiered ? "declared-unfitted-scenario-modeled-locate-rule"
                                   : "declared-unfitted-scenario-no-locate"},
      {"monthly_turnover_target_status", "legacy-reporting-only;retired-2026-09-27"},
      {"daily_turnover_mean_max", limits.daily_mean_max},
      {"daily_turnover_p95_max", limits.daily_p95_max},
      {"daily_turnover_definition", daily_turnover_definition},
      {"financing", financing_declaration}, {"primary_financing_available", tiered},
      {"financing_fields", fields}};
  if (tiered) j["borrow_tiers"] = borrow_tier_declaration;
  // Construction keys only when non-default (overwrites desired_target_postprocess
  // when neutralizing), so the default recipe carries no construction entries.
  const auto construction = detail::construction_recipe_json(cfg.target);
  if (!construction.empty()) j.update(Json::parse(construction));
  // rate per-name-v1 (aim-partial-v5 only) replaces the fixed rate's keys; a fixed
  // rate adds nothing, so its recipe is the T30 recipe byte for byte.
  if (base.rate == NavRateRule::PerNameV1) {
    j["rate"] = "per-name-v1";
    j["rate_rra"] = base.rate_rra; j["rate_lambda"] = base.rate_lambda;
    j["rate_min"] = base.rate_min; j["rate_max"] = base.rate_max;
    j["aim_partial"] = std::string(per_name_rate_head) +
                       detail::nonmember_exit_clause(cfg.target) + per_name_rate_tail;
  }
  // v6 execution options: keys only when non-default, so the default recipe is the v5
  // recipe byte for byte. The liquidity cache changes no output and records nothing.
  if (base.order_basis == NavOrderBasis::Delta) {
    j["order_basis"] = "delta"; j["order_basis_rule"] = order_basis_declaration;
  }
  if (base.locate_in_aim) {
    j["locate_in_aim"] = true; j["locate_in_aim_rule"] = locate_in_aim_declaration;
  }
  // v8 warm start: keys only when on (the flat-start recipe is unchanged byte for byte).
  if (base.warm_start_sessions) {
    j["warm_start_sessions"] = base.warm_start_sessions;
    j["warm_start_rule"] = warm_start_declaration;
  }
  // v8 E-25: only with --label-role (the recipe without it is unchanged byte for byte).
  if (!label.is_null())
    j["label_role"] = Json{{"manifest_sha256", label.at("manifest_sha256")},
                           {"rule", label_role_declaration}};
  v7::extend_recipe(j); // L4 hook: identity unless extended or a reserved cost-v2 id
  return j;
}
Json rate_stats_json(const NavRateStats& s) {
  return Json{{"n", s.n}, {"mean", finite_or_null(s.mean)}, {"min", finite_or_null(s.min)},
      {"max", finite_or_null(s.max)}, {"p05", finite_or_null(s.p05)},
      {"p50", finite_or_null(s.p50)}, {"p95", finite_or_null(s.p95)},
      {"at_min_count", s.at_min_count}, {"at_max_count", s.at_max_count},
      {"share_at_min", finite_or_null(s.share_at_min)},
      {"share_at_max", finite_or_null(s.share_at_max)}};
}
} // namespace nav_book
namespace {
Json months_json(const NavSummary& s) {
  Json months = Json::array();
  for (const auto& m : s.months)
    months.push_back({{"month", m.month}, {"one_way_turnover", m.one_way_turnover},
        {"traded_dollars", m.traded_dollars}, {"execution_sessions", m.execution_sessions},
        {"traded_sessions", m.traded_sessions}, {"is_deployment_month", m.is_deployment_month},
        {"planned_turnover_decision_month", m.planned_turnover},
        {"decision_sessions", m.decision_sessions}});
  return months;
}
Json turnover_json(const NavSummary& s) {
  f64 monthly_total = 0;
  for (const auto& m : s.months) monthly_total += m.one_way_turnover;
  return Json{{"turnover_definition", turnover_definition}, {"months", months_json(s)},
      {"execution_months", s.execution_months},
      {"mean_monthly_one_way_turnover", finite_or_null(s.mean_monthly_turnover)},
      {"max_monthly_one_way_turnover", s.max_monthly_turnover},
      {"mean_monthly_one_way_turnover_ex_deployment_month",
       finite_or_null(s.mean_monthly_turnover_ex_deployment)},
      {"max_monthly_one_way_turnover_ex_deployment_month",
       finite_or_null(s.max_monthly_turnover_ex_deployment)},
      {"months_le_0.30", s.months_within_target},
      {"months_le_0.30_ex_deployment_month", s.months_within_target_ex_deployment},
      {"deployment", {{"occurred", s.deployed},
          {"session_ns", s.deployed ? Json(s.deployment_session) : Json(nullptr)},
          {"one_way_turnover", s.deployment_turnover},
          {"traded_dollars", s.deployment_dollars}}},
      {"planned_vs_actual", {
          {"total_planned_turnover_decision_basis", s.total_planned_turnover},
          {"total_actual_one_way_turnover_execution_basis", s.total_actual_turnover},
          {"actual_minus_planned", s.total_actual_turnover - s.total_planned_turnover},
          {"monthly_reconciled_total", monthly_total},
          {"monthly_reconciliation_error", monthly_total - s.total_actual_turnover},
          {"basis", "planned = sum|next-cur| on drifted marked weights at decision NAV; "
                    "actual = sum|fills|/pre-trade NAV at execution; gaps: drift, caps and "
                    "working orders, blocked absent names, write-offs"}}}};
}
Json risk_json(const NavReplayResult& r, const NavSummary& s) {
  return Json{
      {"costs", {{"trade_cost_dollars", s.trade_cost_dollars},
          {"linear_cost_dollars", s.linear_cost_dollars},
          {"impact_cost_dollars", s.impact_cost_dollars},
          {"unrationed_full_request_cost_dollars", s.unrationed_cost_dollars},
          {"unrationed_unpriced_requests", s.unrationed_unpriced},
          {"borrow_dollars", s.borrow_dollars}, {"writeoff_pnl_dollars", s.writeoff_dollars},
          {"summed_trade_cost_return", s.summed_trade_cost_return},
          {"summed_borrow_return", s.summed_borrow_return},
          {"summed_writeoff_return", s.summed_writeoff_return}}},
      {"guard", {{"guarded_intervals", r.guarded.count},
          {"guarded_gross_exposure_dollars", r.guarded.gross_exposure},
          {"raw_minus_adjusted_pnl_sensitivity_dollars", r.guard_sensitivity}}},
      {"missing", {{"gap_run_1", bucket_json(r.gap_run_1)},
          {"gap_run_2_4", bucket_json(r.gap_run_2_4)},
          {"gap_run_5_plus", bucket_json(r.gap_run_5_plus)},
          {"written_off", bucket_json(r.written_off)},
          {"reappeared_after_write_off_forgone", bucket_json(r.reappeared)},
          {"unresolved_at_end_stale_mark", bucket_json(r.unresolved)},
          {"blocked_absent_order_sessions", s.blocked_absent}}},
      {"exposure", {{"mean_gross_leverage", s.mean_gross_leverage},
          {"max_gross_leverage", s.max_gross_leverage},
          {"max_abs_net_leverage", s.max_abs_net_leverage},
          {"mean_held_names", s.mean_held_names}, {"mean_stale_names", s.mean_stale_names},
          {"max_stale_names", s.max_stale_names},
          {"max_stale_gross_fraction", s.max_stale_gross_fraction},
          {"min_cash_ratio", s.min_cash_ratio}}},
      {"capacity", {{"fills", s.fills}, {"capped_fills", s.capped_fills},
          {"unfilled_dollars", s.unfilled_dollars},
          {"liquidity_blocked_orders", s.blocked_liquidity},
          {"fallback_vol_fills", s.fallback_vol_fills},
          {"participation_fills", r.participation_fills},
          {"participation_p95_upper_bound", finite_or_null(r.participation_p95)},
          {"participation_p95_resolution", "0.01-decade histogram bin upper edge"},
          {"participation_max", finite_or_null(r.participation_max)}}},
      {"accounting_checks", {{"max_return_identity_error", r.max_return_identity_error},
          {"max_cash_book_relative_error", r.max_cash_book_error},
          {"tolerance", identity_tolerance}}}};
}
Json tier_json(const std::array<f64, 3>& v) {
  return Json{{"gc", finite_or_null(v[0])}, {"warm", finite_or_null(v[1])},
              {"special", finite_or_null(v[2])}};
}
Json financing_summary(const NavScenario& sc, const NavSummary& s, bool tiered) {
  const f64 short_leg = s.borrow_dollars;
  return Json{{"spec", financing_json(sc.financing)}, {"tiers_available", tiered},
      {"long_financing_dollars", s.long_financing_dollars},
      {"short_financing_dollars", short_leg},
      {"total_financing_dollars", s.long_financing_dollars + short_leg},
      {"short_financing_by_tier_dollars", tier_json(s.short_financing_by_tier)},
      {"summed_long_financing_return", s.summed_long_financing_return},
      {"summed_short_financing_return", s.summed_borrow_return},
      {"short_dollar_share_by_tier", {{"basis", "pre-mark short dollars on return rows with "
                                                "shorts; NaN/null without tiers"},
          {"sessions", s.short_share_sessions}, {"mean", tier_json(s.short_share_mean)},
          {"p95", tier_json(s.short_share_p95)}}},
      {"missing_predictor_short_name_days", s.missing_predictor_short_name_days},
      {"missing_predictor_member_decisions", s.missing_predictor_member_days},
      {"member_decisions_by_tier", {{"gc", s.member_tier_days[0]},
          {"warm", s.member_tier_days[1]}, {"special", s.member_tier_days[2]}}},
      {"blocked_short_dollars", s.blocked_short_dollars},
      {"blocked_short_name_decisions", s.blocked_short_name_decisions},
      {"net_exposure", {{"mean_net_leverage", s.mean_net_leverage},
          {"mean_abs_net_leverage", s.mean_abs_net_leverage},
          {"max_abs_net_leverage", s.max_abs_net_leverage}}}};
}
} // namespace
namespace nav_book {
Json scenario_summary(const NavScenario& sc, bool primary, bool tiered, const NavReplayResult& r,
                      const NavSummary& s, const std::string& daily_sha,
                      const std::string& events_sha) {
  Json years = Json::array();
  for (const auto& y : s.years)
    years.push_back({{"year", y.year}, {"observations", y.observations},
                     {"net_compounded_return", y.net_return}});
  Json j{{"scenario", scenario_label(sc, tiered)}, {"trading_scenario", sc.id},
      {"financing_scenario", sc.financing.id}, {"primary", primary},
      {"observations", s.observations},
      {"net_sharpe", finite_or_null(s.net_sharpe)},
      {"gross_sharpe", finite_or_null(s.gross_sharpe)},
      {"hac_t", finite_or_null(s.hac_t)}, {"hac_defined", s.hac_defined},
      {"hac_kernel", "bartlett"}, {"hac_lag", s.hac_lag},
      {"mean_daily_net_return", s.mean_daily_net}, {"ann_mean", s.ann_mean},
      {"ann_vol", finite_or_null(s.ann_vol)}, {"cagr", s.cagr},
      {"max_drawdown", s.max_drawdown}, {"total_net_return", s.total_net_return},
      {"final_nav", s.final_nav}, {"calendar_year_returns", std::move(years)},
      {"meets_sharpe_target", s.meets_sharpe_target},
      {"meets_turnover_target_mean", s.meets_turnover_target_mean},
      {"meets_turnover_target_mean_ex_deployment_month",
       s.meets_turnover_target_mean_ex_deployment},
      {"meets_turnover_target_all_months", s.meets_turnover_target_all_months},
      {"meets_turnover_target_all_months_ex_deployment_month",
       s.meets_turnover_target_all_months_ex_deployment},
      {"events", r.events.size()}, {"daily_csv_sha256", daily_sha},
      {"events_csv_sha256", events_sha}};
  j.update(turnover_json(s));
  j.update(risk_json(r, s));
  j["daily_turnover_gmv"] = Json{{"definition", daily_turnover_definition},
      {"basis", "executed-fills;forced-exits-included;deployment-session-excluded"},
      {"sessions", s.daily_turnover_sessions},
      {"zero_gmv_sessions_excluded", s.daily_turnover_zero_gmv_sessions},
      {"mean", finite_or_null(s.daily_turnover_mean)},
      {"median", finite_or_null(s.daily_turnover_median)},
      {"p95", finite_or_null(s.daily_turnover_p95)},
      {"max", finite_or_null(s.daily_turnover_max)},
      {"max_session_ns", s.daily_turnover_sessions ? Json(s.daily_turnover_max_session)
                                                   : Json(nullptr)},
      {"quantile", "linear interpolation at (n-1)q over sorted sessions (numpy default)"},
      {"mean_max", s.daily_limits.daily_mean_max}, {"p95_max", s.daily_limits.daily_p95_max}};
  j["meets_daily_turnover_mean"] = s.meets_daily_turnover_mean;
  j["meets_daily_turnover_p95"] = s.meets_daily_turnover_p95;
  j["financing"] = financing_summary(sc, s, tiered);
  return j;
}
co::Status write_json(const std::filesystem::path& path, const Json& j) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "nav replay: JSON output");
  file << j.dump(2) << '\n'; file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "nav replay: JSON close"));
}
} // namespace nav_book
namespace {
constexpr const char* financing_csv_columns =
    ",long_financing_dollars,long_financing_return,short_gc_dollars,short_warm_dollars,"
    "short_special_dollars,short_financing_gc_dollars,short_financing_warm_dollars,"
    "short_financing_special_dollars,missing_predictor_shorts,missing_predictor_short_dollars,"
    "blocked_short_names,blocked_short_dollars,member_gc,member_warm,member_special,"
    "member_missing_predictors";
void write_financing_csv(std::ostream& file, const NavReplayDay& d) {
  file << ',' << d.long_financing_dollars << ',' << d.long_financing_return;
  for (const f64 v : d.short_dollars_by_tier) file << ',' << v;
  for (const f64 v : d.short_financing_by_tier) file << ',' << v;
  file << ',' << d.missing_predictor_shorts << ',' << d.missing_predictor_short_dollars << ','
       << d.blocked_short_names << ',' << d.blocked_short_dollars;
  for (const usize v : d.member_tiers) file << ',' << v;
  file << ',' << d.member_missing_predictors;
}
} // namespace
namespace nav_book {
// The pre-existing columns come first, unchanged; the GMV turnover columns are
// appended, then the construction columns only when an option is non-default, then
// the financing columns only in the financing matrix (with borrow fields).
co::Status write_daily(const std::filesystem::path& path, const NavReplayResult& r,
                       bool construction, bool financing) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "nav replay: daily output");
  file.imbue(std::locale::classic()); file << std::setprecision(17);
  file << "session_index,session_ns,exec_month,decision,rebalance,executed,return_observation,"
          "pretrade_nav,posttrade_nav,net_return,gross_return,writeoff_return,trade_cost_return,"
          "borrow_return,mark_pnl_dollars,writeoff_dollars,borrow_dollars,traded_dollars,"
          "one_way_turnover,trade_cost_dollars,linear_cost_dollars,impact_cost_dollars,"
          "unrationed_cost_dollars,unrationed_unpriced,fills,capped_fills,unfilled_dollars,"
          "blocked_absent,blocked_liquidity,fallback_vol_fills,planned_turnover,planned_forced,"
          "planned_discretionary,applied_fraction,planned_gross,planned_net,month_planned,"
          "budget_excess,long_dollars,"
          "short_dollars,gross_leverage,net_leverage,held_names,stale_names,stale_long_dollars,"
          "stale_short_dollars,guarded_intervals,cash_ratio,pretrade_gross_dollars,"
          "one_way_turnover_gmv";
  if (construction) file << detail::construction_csv_columns();
  if (financing) file << financing_csv_columns;
  file << '\n';
  for (const auto& d : r.days) {
    file << d.session_index << ',' << d.session << ',' << d.calendar_month << ','
         << d.decision << ',' << d.rebalance << ',' << d.executed << ','
         << d.return_observation << ',' << d.pretrade_nav
         << ',' << d.posttrade_nav << ',' << d.net_return << ',' << d.gross_return << ','
         << d.writeoff_return << ',' << d.trade_cost_return << ',' << d.borrow_return << ','
         << d.mark_pnl_dollars << ',' << d.writeoff_dollars << ',' << d.borrow_dollars << ','
         << d.traded_dollars << ',' << d.one_way_turnover << ',' << d.trade_cost_dollars << ','
         << d.linear_cost_dollars << ',' << d.impact_cost_dollars << ','
         << d.unrationed_cost_dollars << ',' << d.unrationed_unpriced << ',' << d.fills << ','
         << d.capped_fills << ','
         << d.unfilled_dollars << ',' << d.blocked_absent << ',' << d.blocked_liquidity << ','
         << d.fallback_vol_fills << ',' << d.planned_turnover << ',' << d.planned_forced << ','
         << d.planned_discretionary << ',' << d.applied_fraction << ',' << d.planned_gross << ','
         << d.planned_net << ',' << d.month_planned << ','
         << d.budget_excess << ',' << d.long_dollars << ',' << d.short_dollars << ','
         << d.gross_leverage << ',' << d.net_leverage << ',' << d.held_names << ',' << d.stale_names
         << ',' << d.stale_long_dollars << ',' << d.stale_short_dollars << ','
         << d.guarded_intervals << ',' << d.cash_ratio << ',' << d.pretrade_gross_dollars << ',';
    if (std::isnan(d.one_way_turnover_gmv)) file << "nan"; else file << d.one_way_turnover_gmv;
    if (construction) detail::write_construction_csv(file, d.construction);
    if (financing) write_financing_csv(file, d);
    file << '\n';
  }
  file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "nav replay: daily close"));
}
} // namespace nav_book
namespace {
const char* event_label(NavEventKind kind) {
  switch (kind) {
  case NavEventKind::GapResolved: return "gap-resolved";
  case NavEventKind::WriteOff: return "write-off";
  case NavEventKind::Guarded: return "guarded";
  case NavEventKind::ReappearedAfterWriteOff: return "reappeared-after-write-off";
  case NavEventKind::UnresolvedAtEnd: return "unresolved-at-end";
  }
  return "unknown";
}
} // namespace
namespace nav_book {
// Deterministic spelling of unobservable values across standard libraries.
void write_value(std::ostream& out, f64 x) {
  if (std::isnan(x)) out << "nan"; else out << x;
}
co::Status write_events(const std::filesystem::path& path, const NavReplayResult& r) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return co::Err(co::ErrorCode::IoError, "nav replay: events output");
  file.imbue(std::locale::classic()); file << std::setprecision(17);
  file << "kind,session_ns,instrument_id,side,run_length,exposure_dollars,r_adj,r_raw,"
          "haircut,pnl_dollars\n";
  for (const auto& e : r.events) {
    file << event_label(e.kind) << ',' << e.session << ',' << e.instrument_id << ','
         << (e.short_side ? "short" : "long") << ',' << e.run_length << ',';
    write_value(file, e.exposure); file << ','; write_value(file, e.r_adj); file << ',';
    write_value(file, e.r_raw); file << ','; write_value(file, e.haircut); file << ',';
    write_value(file, e.pnl); file << '\n';
  }
  file.close();
  return file ? co::Ok() : co::Status(co::Err(co::ErrorCode::IoError, "nav replay: events close"));
}
} // namespace nav_book
} // namespace atx::impl::strategy
