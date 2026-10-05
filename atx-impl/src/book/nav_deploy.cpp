// NAV replay: the decide path seams (detail::).
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"

namespace atx::impl::strategy {
using namespace nav_book;
// Seams of the daily decide path (strategy_nav_replay_detail.hpp): the replay's own
// admission, loader, recipe and DECIDE functions, never a copy of them.
namespace detail {
const char* borrow_tier_label(u8 tier) {
  switch (tier) {
  case tier_gc: return "gc";
  case tier_warm: return "warm";
  case tier_special: return "special";
  default: return "none";
  }
}
const char* fill_status_label(NavFillStatus fill) {
  switch (fill) {
  case NavFillStatus::None: return "none";
  case NavFillStatus::Complete: return "complete";
  case NavFillStatus::Capped: return "capped";
  case NavFillStatus::BlockedAbsent: return "blocked-absent";
  case NavFillStatus::BlockedLiquidity: return "blocked-liquidity";
  }
  return "unknown";
}
const char* holdings_csv_columns() {
  return "session_ns,instrument_id,member,stale,tier,tier_missing,side,held_dollars,"
         "held_weight,nav_post,fill,filled_dollars,fill_cost_dollars,unfilled_dollars,"
         "desired_weight,rule_weight,target_weight,planned_trade_weight,planned_trade_dollars,"
         "order_placed,locate_blocked,order_working,order_dollars";
}
NavReplayInput NavDeployLoad::view() const {
  auto x = blend.view();
  x.industry = industry; // empty unless an industry id
  return NavReplayInput{x, blend.volume, NavFinancingFields{shares_out, si_shares}};
}
co::Result<NavDeployLoad> load_nav_deploy(const TargetReplayRunConfig& cfg,
                                          const NavReplayConfig& base,
                                          const NavTurnoverLimits& limits,
                                          const NavFieldsPin& fields) {
  try {
    if (cfg.role_path.empty() || cfg.role_sha256.empty() ||
        fields.manifest_path.empty() != fields.manifest_sha256.empty())
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav deploy: pinned role required; fields path and SHA go together");
    const bool tiered = !fields.manifest_path.empty();
    if (neutralize_by_industry(cfg.target.neutralize) && !tiered)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav deploy: the industry ids need the fields (grp_ff12)");
    ATX_TRY_VOID(validate_nav_config(base));
    ATX_TRY(auto admitted, admit_and_load(cfg, base, fields, 1, false));
    // The recipe run_nav_replay publishes for the same (cfg, base, limits, fields), without a
    // label role (v8 E-25: a --label-role run records one, so it is never a deploy pin).
    const auto method = nav_recipe(cfg, base, nav_scenario_matrix(tiered), limits,
                                   admitted.fields.binding, Json(nullptr));
    ATX_TRY(auto digest, co::sha256_hex(method.dump()));
    NavDeployLoad out;
    out.blend = std::move(admitted.blend);
    out.shares_out = std::move(admitted.fields.shares_out);
    out.si_shares = std::move(admitted.fields.si_shares);
    out.industry = std::move(admitted.fields.industry);
    out.recipe_sha256 = std::move(digest);
    return co::Ok(std::move(out));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav deploy: allocation failed");
  } catch (const std::exception& e) {
    return co::Err(co::ErrorCode::InvalidArgument, std::string("nav deploy: ") + e.what());
  }
}
co::Result<NavDecision> nav_decide(const NavReplayInput& in, const NavReplayConfig& cfg, usize d,
                                   std::span<const f64> held, f64 nav_post,
                                   std::span<const u8> no_locate,
                                   const atx::engine::book::HoldBandState* hold) {
  try {
    // As replay_nav_scenarios: the NAV volume is authoritative, also for price risk.
    TargetReplayInput x = in.target;
    x.volume = in.volume;
    const NavReplayInput input{x, in.volume, in.financing};
    const bool tiered = !in.financing.shares_out.empty();
    if ((cfg.scenario.financing.rule == NavFinancingRule::TieredSwapV1 || cfg.locate_in_aim) &&
        !tiered)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav decide: tiered financing and locate-in-aim need the borrow fields");
    if (cfg.rate != NavRateRule::Fixed ||
        cfg.target.rule == TargetReplayRule::MonthlyTargetBudgetV2)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav decide: rate per-name-v1 and monthly-budget-v2 carry book state "
                     "(pre-trade NAV, month-to-date plan) that positions do not");
    // P9 C1: a leverage rule's estimates are per-book replay state positions do not carry.
    if (cfg.leverage.law != NavLeverageLaw::Fixed)
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav decide: a leverage rule (risk-target-v1, vol-target-v1) carries the "
                     "book's estimates, which positions do not (replay only)");
    // v8 Y-5: two-speed-v1's virtual fast sleeve is replay state the holdings file does not carry.
    if (two_speed_on(cfg.target))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav decide: two-speed-v1 carries the fast sleeve's state, which positions "
                     "do not (replay only)");
    ATX_TRY_VOID(validate_nav_input(input, cfg, 1));
    const usize n = x.instruments;
    if (d < x.decision_begin || d >= x.decision_end)
      return co::Err(co::ErrorCode::OutOfRange, "nav decide: as-of row outside the score window");
    if (held.size() != n || !std::all_of(held.begin(), held.end(), [](f64 h) {
          return std::isfinite(h);
        }) || !std::isfinite(nav_post) || !(nav_post > 0) ||
        (!no_locate.empty() && no_locate.size() != n))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav decide: one finite position per name, NAV > 0, locate mask geometry");
    NavDecision out;
    Construction shared(n, cfg.locate_in_aim);
    if (hold) shared.state.hold = *hold; // v8 R-4: the book's hold-band state entering d
    BorrowTiers tiers(n, tiered);
    TierCensus census;
    out.cadence = (d - x.decision_begin) % cfg.target.cadence == 0;
    ATX_TRY(out.rebalance, decide_construction(x, in.financing, cfg, d, out.cadence,
                                               no_locate, shared, tiers, census,
                                               out.construction));
    out.current.resize(n); out.target.resize(n); out.rule.resize(n);
    for (usize i = 0; i < n; ++i) out.current[i] = held[i] / nav_post;
    out.plan.decision = d; out.plan.session = x.session_keys[d];
    out.plan.calendar_month = calendar_month(out.plan.session);
    NavReplayDay day; // receives the locate block's counts
    const PlanInputs inputs{x,         cfg, d,       out.rebalance, shared.desired, tiers,
                            no_locate, {},  nullptr, nullptr,       {},             nullptr,
                            nullptr};
    ATX_TRY_VOID(plan_weights(inputs, 0.0, {}, out.current, out.target, nav_post, out.rule,
                              out.plan, day));
    out.construction.banded_names = out.plan.construction.banded_names;
    out.members = members_at(x, d);
    out.blocked_short_names = day.blocked_short_names;
    out.blocked_short_dollars = day.blocked_short_dollars;
    out.member_tiers = census.members; out.member_missing_predictors = census.missing;
    out.sigma.assign(n, nan);
    for (usize i = 0; i < n; ++i)
      if (x.member[d * n + i]) out.sigma[i] = window_liquidity(x, in.volume, cfg, d, i).sigma;
    out.desired = std::move(shared.desired); out.no_short = std::move(shared.no_short);
    out.tier = std::move(tiers.tier); out.tier_missing = std::move(tiers.missing);
    out.hold = std::move(shared.state.hold);
    return co::Ok(std::move(out));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav decide: allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav decide: allocation extent");
  }
}
co::Result<std::vector<f64>> execution_adv(const NavReplayInput& in, const NavReplayConfig& cfg,
                                           usize t) {
  const auto& x = in.target;
  const usize n = x.instruments, cells = x.dates * n;
  if (!t || t > x.dates || !cfg.liquidity_window || in.volume.size() != cells ||
      x.raw_close.size() != cells || x.close.size() != cells || x.present.size() != cells)
    return co::Err(co::ErrorCode::InvalidArgument, "nav execution ADV: session or geometry");
  try {
    std::vector<f64> adv(n);
    for (usize i = 0; i < n; ++i) adv[i] = window_liquidity(x, in.volume, cfg, t, i).adv;
    return co::Ok(std::move(adv));
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav execution ADV: allocation failed");
  }
}
} // namespace detail
} // namespace atx::impl::strategy
