// NAV replay: config and input validation, the cost model.
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"
#include "atx/engine/book/two_speed.hpp" // P9 AL-CLOCK: two_speed_slow_theta

namespace atx::impl::strategy {
using namespace nav_book;
namespace {
bool within(f64 x, f64 lo, f64 hi) { return std::isfinite(x) && x >= lo && x <= hi; }
} // namespace
namespace nav_book {
bool delta_order(const NameState& s, usize i) { return !s.delta.empty() && s.delta[i] != 0; }
// The shared per-session liquidity cache: always under rate per-name-v1 (its rates read
// it), and at a fixed rate when --liquidity-cache asks for it.
bool liquidity_cached(const NavReplayConfig& cfg) {
  return cfg.rate == NavRateRule::PerNameV1 || cfg.liquidity_cache;
}
} // namespace nav_book
namespace {
bool valid_id(const std::string& id) {
  return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
  });
}
// Each rule admits only its own parameters, so a spec cannot silently mix them.
bool valid_financing(const NavFinancing& f) {
  if (!valid_id(f.id)) return false;
  switch (f.rule) {
  case NavFinancingRule::FlatShortV0:
    return within(f.flat_short_bps, 0, 100000) && f.long_spread_bps == 0 &&
           f.short_spread_bps == 0 && f.gc_bps == 0 && f.warm_bps == 0 && f.special_bps == 0 &&
           f.day_count == 365 && !f.block_special_shorts;
  case NavFinancingRule::TieredSwapV1:
    return f.flat_short_bps == 0 && within(f.long_spread_bps, 0, 10000) &&
           within(f.short_spread_bps, 0, 10000) && within(f.gc_bps, 0, 100000) &&
           within(f.warm_bps, f.gc_bps, 100000) && within(f.special_bps, f.warm_bps, 100000) &&
           (f.day_count == 360 || f.day_count == 365);
  }
  return false;
}
} // namespace
namespace nav_book {
co::Status validate_nav_config(const NavReplayConfig& cfg) {
  const auto& s = cfg.scenario;
  const bool cost_ok = (s.cost == NavCostRule::FlatBpsV1 || s.cost == NavCostRule::SqrtImpactV1) &&
      within(s.flat_bps, 0, 10000) && within(s.half_spread_bps, 0, 10000) &&
      within(s.commission_bps, 0, 10000) && within(s.impact_y, 0, 100) &&
      within(s.impact_delta, 0, 2) && s.impact_delta > 0 && !std::isnan(s.max_participation) &&
      s.max_participation > 0 && within(s.fallback_daily_vol, 0, 10) && s.fallback_daily_vol > 0;
  if (!valid_id(s.id) || !cost_ok || !valid_financing(s.financing) || !s.stale_exit_sessions ||
      s.stale_exit_sessions > max_dates)
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: invalid scenario");
  // P9 C1 (DEC-10): a leverage rule other than fixed-v1 sets aim-partial-v5's aim leverage.
  const auto law = cfg.leverage.law;
  if ((law != NavLeverageLaw::Fixed && law != NavLeverageLaw::RiskTargetV1 &&
       law != NavLeverageLaw::VolTargetV1) ||
      (law != NavLeverageLaw::Fixed && cfg.target.rule != TargetReplayRule::AimPartialV5))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: a leverage rule (risk-target-v1, vol-target-v1) needs "
                   "aim-partial-v5, whose aim is aim_leverage x desired");
  // P9 C2 (carried C1 minors): the rule reads the pinned atx-risk-v1 store, and risk-target-v1
  // its S, b and C in their declared ranges, here as well as at the v7 command line, so a config
  // built in code cannot reach the scaler without them (vol-target-v1's constants are fixed).
  if (law != NavLeverageLaw::Fixed && !cfg.leverage.risk)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: a leverage rule needs the pinned atx-risk-v1 store (none given)");
  if (law == NavLeverageLaw::RiskTargetV1)
    ATX_TRY_VOID(bk::validate_risk_target(cfg.leverage.params));
  if (!std::isfinite(cfg.initial_nav) || cfg.initial_nav <= 0 || cfg.liquidity_window < 2 ||
      cfg.liquidity_window > max_dates || cfg.min_vol_pairs < 2 ||
      cfg.min_vol_pairs > cfg.liquidity_window || !cfg.max_events ||
      cfg.max_events > max_event_cap || cfg.target.one_way_bps != 0 ||
      cfg.target.annual_borrow_bps != 0 || cfg.warm_start_sessions > max_dates ||
      cfg.book_workers < 1 || cfg.book_workers > max_book_workers)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: invalid recipe (target-replay cost/borrow must be zero; "
                   "book workers 1..64)");
  const bool rate_defaults = cfg.rate_rra == nav_rate_rra && cfg.rate_lambda == nav_rate_lambda &&
                             cfg.rate_min == nav_rate_min && cfg.rate_max == nav_rate_max;
  // P9 AL-CLOCK, info-clock-rate-v1: aim-partial-v5's move at two-speed-v1's two rates, whose
  // slow one is the parent's theta (two_speed_slow_theta, the trade fraction); no per-name-v1
  // parameter.
  if (cfg.rate == NavRateRule::InfoClockV1 &&
      (cfg.target.rule != TargetReplayRule::AimPartialV5 ||
       cfg.target.trade_fraction != bk::two_speed_slow_theta || !rate_defaults))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: rate info-clock-rate-v1 needs aim-partial-v5 at trade_fraction "
                   ".05 (its slow rate theta_s, two-speed-v1's) and takes no rate parameters");
  // Trading rate: per-name-v1 is aim-partial-v5 only; a fixed rate carries no rate
  // parameters (each stays at its declared default). within() also refuses NaN.
  const bool rate_ok = (cfg.rate == NavRateRule::Fixed || cfg.rate == NavRateRule::InfoClockV1)
      ? rate_defaults
      : cfg.rate == NavRateRule::PerNameV1 &&
            cfg.target.rule == TargetReplayRule::AimPartialV5 &&
            within(cfg.rate_rra, 0, 1e6) && cfg.rate_rra > 0 &&
            within(cfg.rate_lambda, 0, 1e6) && cfg.rate_lambda > 0 &&
            within(cfg.rate_min, 0, 1) && cfg.rate_min > 0 &&
            within(cfg.rate_max, cfg.rate_min, 1);
  if (!rate_ok)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: rate per-name-v1 needs aim-partial-v5, rate_rra and "
                   "rate_lambda in (0, 1e6] and 0 < rate_min <= rate_max <= 1; a fixed rate "
                   "takes no rate parameters");
  // two-speed-v1 nets the sleeves with one slow rate theta_s (two_speed_aim divides
  // the fast move by it), so a per-name rate would break the netting identity.
  if (two_speed_on(cfg.target) && cfg.rate != NavRateRule::Fixed)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: two-speed-v1 needs the fixed trading rate (its netted aim "
                   "divides by the one slow rate theta_s); rate per-name-v1 is refused");
  // v6: the order basis is one of its two values; locate-in-aim only zeroes shorts
  // that a regression then re-balances (the borrow fields are checked with the input).
  if ((cfg.order_basis != NavOrderBasis::Target && cfg.order_basis != NavOrderBasis::Delta) ||
      (cfg.locate_in_aim && cfg.target.neutralize == TargetNeutralize::None))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: order basis target|delta; locate-in-aim needs a neutralizing "
                   "construction (--neutralize) and the borrow fields");
  return co::Ok();
}
// The price-exposure session ring the shared construction binds when it neutralizes
// (v8 D-1): charged by the input budget and the pinned run's reserve alike.
u64 ring_bytes(const TargetReplayConfig& target, usize names) {
  return target.neutralize == TargetNeutralize::None
      ? 0 : session_ring_bytes(target.price_risk, names);
}
// A warm start of K sessions decides from role row score_begin - K: never before row 0.
co::Status check_warm_start(usize sessions, usize score_begin) {
  if (sessions <= score_begin) return co::Ok();
  return co::Err(co::ErrorCode::InvalidArgument,
                 "nav replay: --warm-start-sessions " + std::to_string(sessions) +
                     " exceeds the role's pre-score history (score_begin = " +
                     std::to_string(score_begin) + " sessions)");
}
co::Status validate_nav_input(const NavReplayInput& in, const NavReplayConfig& cfg,
                              usize books) {
  ATX_TRY_VOID(validate_nav_config(cfg));
  const auto& x = in.target;
  ATX_TRY_VOID(detail::validate_replay_input(x, cfg.target));
  const usize cells = x.dates * x.instruments;
  if (x.close.size() != cells || in.volume.size() != cells ||
      x.decision_end - x.decision_begin < 3)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: prices+volume required and at least three window sessions");
  ATX_TRY_VOID(check_warm_start(cfg.warm_start_sessions, x.decision_begin));
  if (neutralize_by_industry(cfg.target.neutralize) && x.industry.size() != cells)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: the industry ids need the grp_ff12 field (--fields)");
  for (usize k = 0; k < cells; ++k) {
    const bool bad = x.present[k]
        ? (!std::isfinite(x.close[k]) || x.close[k] <= 0 || !std::isfinite(x.raw_close[k]) ||
           x.raw_close[k] <= 0 || !std::isfinite(in.volume[k]) || in.volume[k] < 0)
        : x.member[k] != 0;
    if (bad)
      return co::Err(co::ErrorCode::InvalidArgument, "nav replay: price/volume/presence contract");
  }
  // v8 E-25: label marks (all three or none) extend x's presence only, at x's own close and
  // raw close bit for bit wherever x is present; a label-only cell is priced finite > 0.
  const auto& m = in.mark;
  if (!m.present.empty() || !m.close.empty() || !m.raw_close.empty()) {
    if (m.present.size() != cells || m.close.size() != cells || m.raw_close.size() != cells)
      return co::Err(co::ErrorCode::InvalidArgument, "nav replay: label marks geometry");
    for (usize k = 0; k < cells; ++k) {
      const bool shown = m.present[k] != 0;
      const bool bad = m.present[k] > 1 ||
          (x.present[k] != 0
               ? (!shown || std::bit_cast<u64>(m.close[k]) != std::bit_cast<u64>(x.close[k]) ||
                  std::bit_cast<u64>(m.raw_close[k]) != std::bit_cast<u64>(x.raw_close[k]))
               : (shown && (!std::isfinite(m.close[k]) || m.close[k] <= 0 ||
                            !std::isfinite(m.raw_close[k]) || m.raw_close[k] <= 0)));
      if (bad)
        return co::Err(co::ErrorCode::InvalidArgument,
                       "nav replay: label marks must extend the role's presence at its prices");
    }
  }
  const auto& f = in.financing;
  const bool tiers = !f.shares_out.empty();
  if (tiers != !f.si_shares.empty() ||
      (tiers && (f.shares_out.size() != cells || f.si_shares.size() != cells)))
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: financing fields geometry");
  // P9 AL-CLOCK: the info clock (ea_days_since) at the input's geometry, every cell in the
  // field's domain (NaN or finite >= 0); rate info-clock-rate-v1 reads it.
  const auto& clock = in.info_clock;
  if (!clock.empty() && clock.size() != cells)
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: info clock (ea_days_since) geometry");
  if (cfg.rate == NavRateRule::InfoClockV1 && clock.empty())
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: rate info-clock-rate-v1 needs the info clock ea_days_since "
                   "(--fields)");
  for (usize k = 0; k < clock.size(); ++k)
    if (!(std::isnan(clock[k]) || (std::isfinite(clock[k]) && clock[k] >= 0)))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav replay: ea_days_since outside its domain (NaN or finite >= 0) at row " +
                         std::to_string(k / x.instruments) + ", name " +
                         std::to_string(k % x.instruments));
  // Envelope EXCLUDES caller-owned input; events are charged at their cap. Every
  // lockstep book holds its state, days and events at once; the construction
  // (desired target and, when neutralizing, the price-risk scratch) and the borrow
  // tiers are shared.
  Budget budget{cfg.target.max_working_bytes};
  if (!budget.add(books, fixed_workspace_bytes) ||
      !budget.add(x.instruments, books * per_name_bytes + shared_name_bytes) ||
      !budget.add(tiers ? x.instruments : 0, tier_name_bytes) ||
      !budget.add(liquidity_cached(cfg) ? x.instruments : 0, rate_name_bytes) ||
      !budget.add(x.decision_end - x.decision_begin, books * sizeof(NavReplayDay)) ||
      !budget.add(cfg.max_events, books * sizeof(NavEvent)) ||
      !budget.add(1, detail::construction_scratch_bytes(cfg.target, x.instruments)) ||
      !budget.add(1, ring_bytes(cfg.target, x.instruments)) ||
      // P9 AL-CLOCK: info-clock-rate-v1's shared rate vector
      !budget.add(cfg.rate == NavRateRule::InfoClockV1 ? x.instruments : 0, sizeof(f64)))
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: workspace budget");
  return co::Ok();
}
co::Result<std::unique_ptr<const bk::ReplayCostModel>> make_cost_model(const NavScenario& s) {
  ATX_TRY(auto extension, v7::extension_cost_model(s)); // L4 hook: reserved cost-v2 ids
  if (extension) return co::Ok(std::move(extension));
  std::unique_ptr<const bk::ReplayCostModel> model;
  switch (s.cost) {
  case NavCostRule::FlatBpsV1: {
    ATX_TRY(auto flat, bk::FlatBpsCost::create(s.flat_bps));
    model = std::make_unique<const bk::FlatBpsCost>(std::move(flat));
    break;
  }
  case NavCostRule::SqrtImpactV1: {
    ATX_TRY(auto impact, bk::SqrtImpactCost::create(
        bk::ReplayImpactCfg{s.impact_y, s.impact_delta}, s.max_participation, s.commission_bps));
    model = std::make_unique<const bk::SqrtImpactCost>(std::move(impact));
    break;
  }
  }
  if (!model) return co::Err(co::ErrorCode::InvalidArgument, "nav replay: cost rule");
  return co::Ok(std::move(model));
}
} // namespace nav_book
} // namespace atx::impl::strategy
