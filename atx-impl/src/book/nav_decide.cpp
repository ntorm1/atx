// NAV replay: DECIDE (shared construction, plans, close).
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"
#include "atx/engine/book/two_speed.hpp" // P9 AL-CLOCK: Y-5's fast and slow rates

namespace atx::impl::strategy {
using namespace nav_book;
// P9 AL-CLOCK, info-clock-rate-v1 (strategy_nav_replay.hpp states the rule).
f64 nav_info_clock_fast_theta(usize cadence) noexcept {
  return bk::two_speed_fast_theta(static_cast<f64>(cadence));
}
bool nav_info_clock_fast(f64 days_since) noexcept {
  return days_since >= 0.0 && days_since <= nav_info_clock_window; // NaN: false (slow)
}
co::Status nav_info_clock_rates(std::span<const u8> member, std::span<const f64> days_since,
                                usize cadence, f64 theta_slow, std::span<f64> rates) {
  const usize n = member.size();
  if (days_since.size() != n || rates.size() != n)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "info-clock-rate-v1: one member flag, clock and rate per name");
  if (cadence < 1 || !std::isfinite(theta_slow) || !(theta_slow > 0.0) || theta_slow > 1.0)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "info-clock-rate-v1: cadence >= 1 and the slow rate in (0, 1]");
  for (usize i = 0; i < n; ++i) {
    const f64 v = days_since[i];
    if (member[i] > u8{1} || !(std::isnan(v) || (std::isfinite(v) && v >= 0.0)))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "info-clock-rate-v1: member flags 0/1 and every ea_days_since NaN or finite "
                     ">= 0 (a negative or infinite clock is outside the field's domain)");
  }
  const f64 fast = nav_info_clock_fast_theta(cadence);
  for (usize i = 0; i < n; ++i)
    rates[i] = member[i] != u8{0} && nav_info_clock_fast(days_since[i]) ? fast : theta_slow;
  return co::Ok();
}
namespace {
// STABLE EXTENSION POINT: the only place the NAV path forms its desired target,
// once per rebalance decision for every scenario book. It is the target replay's
// own construction: the tied-rank target, then the configured post-processing
// (price-risk-v1 neutralization with exposures computed once for d and its skip
// guard), before partial/band/budget planning. Reads only sessions <= d. Returns
// false when the guard skips the rebalance (current weights kept, forced exits
// still apply); `out` receives the decision's construction record. With
// locate-in-aim, shared.no_short (the special tier at d) zeroes negative aims first.
// v8 adv-hold-v1: the cap's inputs are every member's raw-dollar ADV the EXECUTE of this
// decision's fills reads (window_liquidity at session d + 1: rows <= d) and the book's NAV:
// the run's initial NAV, or for a capacity book (P9 C1) its multiple's, m x initial NAV (the
// NAV-m book's cap; Ruling E-15 read the initial NAV at every multiple). replay_books forms one
// construction per multiple (cfg is the group's first book).
// v8 X inv-vol-v1: its input is the sigma of the same window.
co::Result<bool> form_desired_target(const TargetReplayInput& x, const NavReplayConfig& cfg,
                                     usize d, Construction& shared, ConstructionDay& out) {
  if (cfg.target.adv_hold_q > 0) {
    auto& adv = shared.state.adv_dollars;
    adv.assign(x.instruments, nan);
    for (usize i = 0; i < x.instruments; ++i)
      if (x.member[d * x.instruments + i])
        adv[i] = window_liquidity(x, x.volume, cfg, d + 1, i).adv;
    shared.state.nav = v7::nav_multiple(cfg.scenario) * cfg.initial_nav;
  }
  // v8 X inv-vol-v1: every member's execution volatility for this decision's fills (the same
  // window, NaN where the scenario's fallback sigma applies; the book-independent window).
  if (inv_vol_on(cfg.target)) {
    auto& sigma = shared.state.sigma;
    sigma.assign(x.instruments, nan);
    for (usize i = 0; i < x.instruments; ++i)
      if (x.member[d * x.instruments + i])
        sigma[i] = window_liquidity(x, x.volume, cfg, d + 1, i).sigma;
  }
  ATX_TRY(const bool proceed,
          detail::form_desired(x, cfg.target, d, shared.row, shared.desired, shared.price, out,
                               shared.no_short, &shared.state));
  // P9 COV-MV: the target rule (K-P9-7 kind target, --target-rule) rewrites the target that
  // proceeds, before any book plans or the calibrate tape records it; the identity without one.
  if (proceed) ATX_TRY_VOID(v7::apply_target_rule(x, d, shared.no_short, shared.desired));
  return co::Ok(proceed);
}
// Engine predictors of name i as of decision d (row d fields and prices, presence
// rows <= d); not available when any is missing or out of its declared domain.
ce::BorrowPredictors borrow_predictors(const TargetReplayInput& x, const NavFinancingFields& f,
                                       const BorrowTiers& tiers, usize d, usize i) {
  ce::BorrowPredictors p;
  const usize k = d * x.instruments + i, first = tiers.first_present[i];
  const f64 shares = f.shares_out[k], short_shares = f.si_shares[k], raw = x.raw_close[k];
  if (!x.present[k] || first > d || !(shares >= nav_shares_out_min) ||
      !(shares <= nav_shares_out_max) || !std::isfinite(short_shares) || short_shares < 0 ||
      !std::isfinite(raw) || !(raw > 0))
    return p;
  const f64 cap = shares * raw;
  if (!std::isfinite(cap)) return p;
  p.available = true;
  p.available_at_ns = x.session_keys[d] + fields_mark_ns;
  p.market_cap_usd = cap; p.raw_price_usd = raw;
  p.short_interest_to_float = short_shares / shares;
  p.ipo_age_calendar_days = first == 0 ? seasoned_age_days
      : static_cast<f64>((x.session_keys[d] - x.session_keys[first]) / day_ns);
  return p;
}
} // namespace
namespace nav_book {
// Borrow tiers at decision d, once for every book. Folds presence rows up to d into
// the first-present rows (monotone: each row is scanned once per replay), then asks
// the engine per name; a missing predictor or Unavailable is warm and flagged. An
// engine refusal of well-formed predictors is an error, never a silent tier.
co::Status classify_borrow(const TargetReplayInput& x, const NavFinancingFields& f, usize d,
                           BorrowTiers& tiers, TierCensus& census) {
  const usize n = x.instruments;
  for (; tiers.scanned <= d; ++tiers.scanned)
    for (usize i = 0; i < n; ++i)
      if (x.present[tiers.scanned * n + i] && tiers.first_present[i] == never_present)
        tiers.first_present[i] = tiers.scanned;
  const i64 session = x.session_keys[d];
  if (session > std::numeric_limits<i64>::max() - decision_clock_ns)
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: borrow decision clock");
  const i64 decision = session + decision_clock_ns;
  const ce::BorrowTierRecipe recipe{}; // thresholds only: every fee is the scenario's
  for (usize i = 0; i < n; ++i) {
    const auto predictors = borrow_predictors(x, f, tiers, d, i);
    u8 tier = tier_warm, missing = 1;
    if (predictors.available) {
      ATX_TRY(const auto estimate, ce::estimate_borrow_tier(predictors, decision, recipe));
      if (estimate.tier != ce::BorrowTier::Unavailable) {
        tier = static_cast<u8>(estimate.tier); missing = 0;
      }
    }
    tiers.tier[i] = tier; tiers.missing[i] = missing;
    if (!x.member[d * n + i]) continue;
    ++census.members[static_cast<usize>(tier - tier_gc)]; census.missing += missing;
  }
  return co::Ok();
}
} // namespace nav_book
namespace {
// A name that may not open or grow a short at the decision: the special tier, or (decide
// --locates only; the replay passes none) no locate.
bool short_guarded(const BorrowTiers& tiers, std::span<const u8> no_locate, usize i) {
  return tiers.tier[i] == tier_special || (!no_locate.empty() && no_locate[i] != 0);
}
} // namespace
namespace nav_book {
// DECIDE, the part shared by every book of decision d: the borrow tiers of d, the
// locate-in-aim mask (allocated only under it), and on a cadence decision the desired
// target (form_desired_target). Returns the effective rebalance (false when the guard
// skips it); `construction` receives the decision's record.
co::Result<bool> decide_construction(const TargetReplayInput& x, const NavFinancingFields& fields,
                                     const NavReplayConfig& cfg, usize d, bool cadence,
                                     std::span<const u8> no_locate, Construction& shared,
                                     BorrowTiers& tiers, TierCensus& census,
                                     ConstructionDay& construction) {
  if (tiers.active) ATX_TRY_VOID(classify_borrow(x, fields, d, tiers, census));
  // Locate-in-aim: this decision's special tier, classified just above, guards the aim
  // (replay_nav_scenarios and nav_decide refuse locate-in-aim without borrow fields).
  for (usize i = 0; i < shared.no_short.size(); ++i)
    shared.no_short[i] = short_guarded(tiers, no_locate, i) ? u8{1} : u8{0};
  bool rebalance = cadence;
  if (rebalance) {
    ATX_TRY(rebalance, form_desired_target(x, cfg, d, shared, construction));
  }
  construction.rebalance = rebalance;
  return co::Ok(rebalance);
}
} // namespace nav_book
namespace {
// Locate rule on the planned weights: a special-tier name may not open or grow a
// short, next = max(next, min(cur, 0)); refused growth is counted in decision-NAV
// dollars.
void block_special_plan(const BorrowTiers& tiers, std::span<const u8> no_locate,
                        const std::vector<f64>& current, std::vector<f64>& planned, f64 nav_post,
                        NavReplayDay& day) {
  for (usize i = 0; i < planned.size(); ++i) {
    if (!short_guarded(tiers, no_locate, i)) continue;
    const f64 floor = std::min(current[i], 0.0);
    if (!(planned[i] < floor)) continue;
    day.blocked_short_dollars += (floor - planned[i]) * nav_post;
    ++day.blocked_short_names; planned[i] = floor;
  }
}
} // namespace
namespace nav_book {
// CONSTRUCTION-RULE DISPATCH SITE, shared by the NAV replay (plan_decision) and the
// daily decide path (detail::nav_decide), so a rule added here runs in both. `planned`
// becomes the target rule's plan from `current` toward p.desired (`spent`: the book's
// month-to-date plan, v2 only; `rates`: the per-name-v1 rates or empty), copied into
// `rule` when that is non-empty, then the scenario's locate block at the decision NAV.
co::Status plan_weights(const PlanInputs& p, f64 spent, std::span<const f64> rates,
                        const std::vector<f64>& current, std::vector<f64>& planned, f64 nav_post,
                        std::span<f64> rule, TargetReplayDay& plan, NavReplayDay& day) {
  std::copy(current.begin(), current.end(), planned.begin());
  if (p.leverage || p.book_state) {
    // P9 C1 (DEC-10): a replay book's own leverage rule (L_t from its current weights, the
    // target it hands on), then its own v7 rule at that leverage (base: its unscaled L).
    const NavReplayConfig* cfg = &p.cfg;
    const std::vector<f64>* desired = &p.desired;
    f64 base = nan;
    if (p.leverage) {
      ATX_TRY(desired, p.leverage->plan(p.x, p.cfg, p.d, p.rebalance, p.label, current,
                                        p.desired, p.two_speed_fast, p.records));
      cfg = &p.leverage->config();
      base = p.cfg.target.aim_leverage;
    }
    ATX_TRY_VOID(v7::plan_book(p.book_state, p.x, *cfg, base, p.d, p.rebalance, spent, nav_post,
                               *desired, planned, plan, rates, p.tiers.tier, p.no_locate,
                               p.liquidity));
  } else {
    // L4/W1 hook: detail::update_weights unless the v7 extension (aim-partial-v6, spo-v1) is
    // installed; spo-v1 also reads the decision's borrow tiers and decide --locates.
    ATX_TRY_VOID(v7::plan(p.x, p.cfg, p.d, p.rebalance, spent, nav_post, p.desired, planned,
                          plan, rates, p.tiers.tier, p.no_locate, p.two_speed_fast));
  }
  if (!rule.empty()) std::copy(planned.begin(), planned.end(), rule.begin());
  if (p.cfg.scenario.financing.block_special_shorts)
    block_special_plan(p.tiers, p.no_locate, current, planned, nav_post, day);
  return co::Ok();
}
} // namespace nav_book
namespace {
// A working order an earlier decision placed and this one keeps (unchanged plan on
// a non-rebalance day) must not open or grow a short once the name is special:
// clamp it to min(held, 0); nothing is left to trade when that is the holding. A
// kept delta order first becomes the target order it currently amounts to (its target
// plus the drift since its decision), so a special name only ever holds target orders
// and later drift cannot carry a fill below the floor.
void clamp_kept_order(NameState& s, usize i, NavReplayDay& day) {
  if (delta_order(s, i)) {
    s.order[i] += s.held[i] - s.anchor[i];
    s.delta[i] = 0;
  }
  const f64 floor = std::min(s.held[i], 0.0);
  if (!(s.order[i] < floor)) return;
  day.blocked_short_dollars += floor - s.order[i]; ++day.blocked_short_names;
  s.order[i] = floor;
  if (floor == s.held[i]) s.active[i] = 0;
}
// rate per-name-v1: this book's rate of every name at decision d into cache.rate,
// per_name_rate_v1 at the book's pre-trade NAV on the session's cached liquidity
// (rate_min for a nonmember, which the rule never moves), one sample per member.
void per_name_rates(const Ctx& c, Book& b, usize d, LiquidityCache& cache) {
  const auto& x = c.x; const auto& cfg = c.cfg; const usize n = x.instruments;
  for (usize i = 0; i < n; ++i) {
    if (!x.member[d * n + i]) { cache.rate[i] = cfg.rate_min; continue; }
    const f64 theta = per_name_rate_v1(cfg.rate_rra, cfg.rate_lambda, b.nav_pre, cache.sigma[i],
                                       cache.adv[i], cfg.rate_min, cfg.rate_max);
    cache.rate[i] = theta; b.rates.add(theta);
  }
}
// Delta basis: a nonzero plan on a name the locate rule does not guard becomes a delta
// order anchored at the decision's holding; a zero plan (an exit must end flat) and a
// special-tier name under the locate rule keep target orders.
void anchor_order(NameState& s, usize i, bool locate_guarded) {
  const bool delta = s.planned[i] != 0 && !locate_guarded;
  s.delta[i] = delta ? u8{1} : u8{0};
  if (delta) s.anchor[i] = s.held[i];
}
// P9 AL-CLOCK (NavInfoClockStats): decision d's members into the book's census, in index order;
// `clock` is row d of the info clock (validate_nav_input: NaN or finite >= 0).
void count_info_clock(const TargetReplayInput& x, std::span<const f64> clock, usize d,
                      const NameState& s, NavInfoClockStats& out) {
  const usize n = x.instruments;
  for (usize i = 0; i < n; ++i) {
    if (!x.member[d * n + i]) continue;
    const f64 trade = std::abs(s.planned[i] - s.current[i]);
    ++out.name_days;
    out.planned_turnover += trade;
    if (std::isnan(clock[i])) {
      ++out.missing_name_days;
    } else if (nav_info_clock_fast(clock[i])) {
      ++out.fast_name_days;
      out.fast_planned_turnover += trade;
    }
  }
}
} // namespace
namespace nav_book {
// DECIDE at d: current weights are marked dollars / post-trade NAV (stale names at
// stale marks); the unchanged target rules plan the next weights toward the shared
// desired target (`rebalance` is effective: cadence day and not skipped), then the
// scenario's locate rule; every changed name gets a working order in decision-NAV
// dollars (zero exits included; a target, or under the delta basis a delta, order).
// On rebalance days unchanged members, and on every day flat nonmembers, cancel.
// Planned turnover, gross/net and the v2 budget are the rule's plan before the locate
// rule (blocked dollars are reported apart). With rate per-name-v1 the aim-partial-v5
// move uses this book's per-name rates.
co::Status plan_decision(const Ctx& c, Book& b, const Construction& shared,
                         const BorrowTiers& tiers, LiquidityCache& cache, usize d,
                         bool rebalance, const ConstructionDay& construction,
                         const cost_v2::DecisionLiquidity* liquidity, NavReplayDay& day) {
  const auto& x = c.x; auto& s = b.names; const usize n = x.instruments;
  if (day.calendar_month != b.month) { b.month = day.calendar_month; b.spent = 0; }
  for (usize i = 0; i < n; ++i) s.current[i] = s.held[i] / b.nav_post;
  TargetReplayDay plan;
  plan.decision = d; plan.session = x.session_keys[d]; plan.calendar_month = day.calendar_month;
  std::span<const f64> rates;
  if (c.cfg.rate == NavRateRule::PerNameV1) { per_name_rates(c, b, d, cache); rates = cache.rate; }
  // P9 AL-CLOCK: the decision's info-clock rates, formed once for every book (run_books).
  if (c.cfg.rate == NavRateRule::InfoClockV1) rates = shared.clock_rate;
  // P9 C1: a main book's scored decisions keep their leverage record (a capacity book none).
  const bool scored = !b.capacity && d >= x.decision_begin;
  const PlanInputs inputs{x,
                          c.cfg,
                          d,
                          rebalance,
                          shared.desired,
                          tiers,
                          {},
                          std::span<const f64>(shared.state.fast_before),
                          b.leverage.get(),
                          b.v7_state.get(),
                          b.label,
                          scored ? &b.result.leverage : nullptr,
                          liquidity};
  const auto rule = b.trace ? std::span<f64>(b.trace->rule) : std::span<f64>{};
  ATX_TRY_VOID(plan_weights(inputs, b.spent, rates, s.current, s.planned, b.nav_post, rule, plan,
                            day));
  b.spent += plan.turnover;
  const bool blocking = c.cfg.scenario.financing.block_special_shorts;
  const bool delta_basis = c.cfg.order_basis == NavOrderBasis::Delta;
  for (usize i = 0; i < n; ++i) {
    if (s.planned[i] != s.current[i]) {
      s.order[i] = s.planned[i] * b.nav_post; s.active[i] = 1;
      if (delta_basis) anchor_order(s, i, blocking && tiers.tier[i] == tier_special);
    } else if (rebalance || !x.member[d * n + i]) {
      s.active[i] = 0;
    } else if (blocking && s.active[i] && tiers.tier[i] == tier_special) {
      clamp_kept_order(s, i, day);
    }
  }
  day.decision = true; day.rebalance = rebalance;
  day.planned_turnover = plan.turnover; day.planned_forced = plan.forced_turnover;
  day.planned_discretionary = plan.discretionary_turnover;
  day.planned_gross = plan.gross; day.planned_net = plan.net;
  day.planned_held_names = plan.held_names; day.decision_members = detail::members_at(x, d);
  day.applied_fraction = plan.applied_fraction; day.month_planned = b.spent;
  if (c.cfg.target.rule == TargetReplayRule::MonthlyTargetBudgetV2)
    day.budget_excess = std::max(0.0, b.spent - c.cfg.target.monthly_budget);
  day.construction = construction; // shared record; the band acts on this book's weights
  day.construction.banded_names = plan.construction.banded_names;
  // P9 AL-CLOCK (Ruling ALCLOCK-1): a main book's scored effective rebalance, members only.
  if (!shared.clock.empty() && rebalance && scored)
    count_info_clock(x, shared.clock, d, s, b.result.construction.info_clock);
  return co::Ok();
}
f64 gross_dollars(const NameState& s) {
  f64 gross = 0;
  for (const f64 h : s.held) gross += std::abs(h);
  return gross;
}
co::Status close_day(const Ctx& c, Book& b, NavReplayDay& day) {
  const auto& s = b.names; f64 longs = 0, shorts = 0;
  for (usize i = 0; i < c.x.instruments; ++i) {
    const f64 h = s.held[i];
    if (h == 0) continue;
    ++day.held_names; (h > 0 ? longs : shorts) += std::abs(h);
    if (s.absent[i]) {
      ++day.stale_names;
      (h > 0 ? day.stale_long_dollars : day.stale_short_dollars) += std::abs(h);
    }
  }
  day.posttrade_nav = b.nav_post; day.long_dollars = longs; day.short_dollars = shorts;
  day.gross_leverage = (longs + shorts) / b.nav_post;
  day.net_leverage = (longs - shorts) / b.nav_post;
  day.cash_ratio = b.cash / b.nav_post;
  const f64 book_error = std::abs(b.cash + (longs - shorts) - b.nav_post) / b.nav_post;
  b.result.max_cash_book_error = std::max(b.result.max_cash_book_error, book_error);
  if (!(book_error <= identity_tolerance))
    return co::Err(co::ErrorCode::Internal, "nav replay: cash + holdings != NAV");
  return co::Ok();
}
// Final session: still-stale holdings stay at their stale marks and are reported.
co::Status report_unresolved(const Ctx& c, Book& b, usize t) {
  const auto& s = b.names;
  for (usize i = 0; i < c.x.instruments; ++i) {
    if (s.held[i] == 0 || !s.absent[i]) continue;
    add(b.result.unresolved, s.held[i], 0);
    ATX_TRY_VOID(push_event(c, b, NavEventKind::UnresolvedAtEnd, t, i,
                            {s.absent[i], s.held[i], nan, nan, 0, nan}));
  }
  return co::Ok();
}
} // namespace nav_book
} // namespace atx::impl::strategy
