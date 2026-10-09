// NAV replay: scenarios, workspace, lockstep books.
// P9 C3 (task 1, move-only): moved unchanged from strategy_nav_replay.cpp.

#include "book/nav_replay_internal.hpp"

namespace atx::impl::strategy {
using namespace nav_book;
std::vector<NavFinancing> nav_financing_scenarios() {
  NavFinancing swap;
  swap.id = "swap-fin-v1"; swap.rule = NavFinancingRule::TieredSwapV1; swap.flat_short_bps = 0;
  swap.long_spread_bps = 40; swap.short_spread_bps = 20;
  swap.gc_bps = 30; swap.warm_bps = 100; swap.special_bps = 500;
  swap.day_count = 360; swap.block_special_shorts = true;
  const NavFinancing legacy{}; // flat-300-v0
  NavFinancing engine = swap;
  engine.id = "engine-tiers-v1"; // engine BorrowTierRecipe default fees (asserted by tests)
  engine.gc_bps = 27.5; engine.warm_bps = 300; engine.special_bps = 2750;
  return {swap, legacy, engine};
}
std::vector<NavScenario> fixed_nav_scenarios() {
  NavScenario linear;
  linear.id = "linear-6bps-stale5-v1"; linear.cost = NavCostRule::FlatBpsV1;
  linear.flat_bps = 6; linear.max_participation = inf;
  linear.fallback_daily_vol = 0.05; linear.stale_exit_sessions = 5;
  NavScenario modeled;
  modeled.id = "modeled-1bn-stale5-v1"; modeled.cost = NavCostRule::SqrtImpactV1;
  modeled.half_spread_bps = 5; modeled.commission_bps = 1; modeled.impact_y = 0.6;
  modeled.impact_delta = 0.5; modeled.max_participation = 0.01;
  modeled.fallback_daily_vol = 0.05; modeled.stale_exit_sessions = 5;
  NavScenario adverse = modeled;
  adverse.id = "modeled-1bn-terminal-adverse-v1"; adverse.stale_exit_sessions = 1;
  adverse.adverse_terminal = true;
  return {linear, modeled, adverse};
}
f64 per_name_rate_v1(f64 rra, f64 lambda, f64 nav, f64 daily_vol, f64 adv_dollars,
                     f64 rate_min, f64 rate_max) noexcept {
  const auto usable = [](f64 x) { return std::isfinite(x) && x > 0; };
  if (!usable(rra) || !usable(lambda) || !usable(nav) || !usable(daily_vol) ||
      !usable(adv_dollars))
    return rate_min;
  const f64 theta = std::sqrt(rra * daily_vol * daily_vol * adv_dollars / (lambda * nav));
  // A NaN (inf / inf on overflow) fails the comparison and takes rate_min; +inf clips.
  if (!(theta >= rate_min)) return rate_min;
  return std::min(theta, rate_max);
}
std::vector<NavScenario> nav_scenario_matrix(bool tiered) {
  auto trading = fixed_nav_scenarios();
  if (!tiered) return trading;
  const auto financing = nav_financing_scenarios();
  std::vector<NavScenario> books;
  for (auto s : trading) { s.financing = financing[0]; books.push_back(std::move(s)); }
  for (usize k = 1; k < financing.size(); ++k) {
    auto s = trading[nav_primary_scenario_index]; s.financing = financing[k];
    books.push_back(std::move(s));
  }
  return books;
}
// Conservative bound on everything the NAV path holds beside the loaded input, at the
// run's own geometry (v6 C4; the charge was at max_names x max_dates before): all
// scenario results (days + events at cap), every lockstep book's per-name state, the
// shared construction (with its price-risk scratch when neutralizing), the shared
// borrow tiers, the shared liquidity cache (per-name-v1, or --liquidity-cache at a
// fixed rate: validate_nav_input's predicate) and publication. books <= max_scenarios
// (x nav_max_grid_variants for a construction grid); the session ring when neutralizing.
u64 nav_workspace_reserve_bytes(const NavReplayConfig& base, usize books, bool tiered,
                                usize names, usize sessions, bool holdings) {
  const bool cached = liquidity_cached(base);
  const u64 hold_trace = hold_band_declared(base.target) ? hold_trace_name_bytes : 0;
  // P9 AL-CLOCK: info-clock-rate-v1's shared rate vector (one f64 per name).
  const u64 clock = base.rate == NavRateRule::InfoClockV1 ? sizeof(f64) : 0;
  return publication_slack_bytes + u64{books} * fixed_workspace_bytes +
         u64{names} * (u64{books} * per_name_bytes + shared_name_bytes +
                       (tiered ? tier_name_bytes : 0) + (cached ? rate_name_bytes : 0) +
                       (holdings ? holdings_name_bytes + hold_trace : 0) + clock) +
         detail::construction_scratch_bytes(base.target, names) + ring_bytes(base.target, names) +
         u64{books} * (u64{sessions} * sizeof(NavReplayDay) + base.max_events * sizeof(NavEvent));
}
namespace {
bool same_price_risk(const PriceExposureConfig& a, const PriceExposureConfig& b) {
  return a.beta_window == b.beta_window && a.vol_window == b.vol_window &&
         a.adv_window == b.adv_window && a.min_return_pairs == b.min_return_pairs &&
         a.min_names == b.min_names && a.clip_z == b.clip_z;
}
// Grid variants agree on everything but nav_grid_variant_flags' target keys (rule,
// cadence, trade_fraction, monthly_budget, band_multiple, dust_multiple, aim_leverage,
// exit_rate) and the scenario: the shared construction, the shared liquidity windows and
// the book-independent settings are then one. The v8 construction options (hold band, ADV
// cap, v8 X inv-vol-v1) shape the shared desired target, so they are shared too. The ADV cap
// Q ADV / (aim_leverage NAV) also reads aim_leverage, a variant flag: replay_books then forms one
// construction per distinct leverage (review A-1).
// P9 C1: the variants run one leverage rule (each book its own state of it).
bool same_leverage(const NavLeverageRule& a, const NavLeverageRule& b) {
  return a.law == b.law && a.risk == b.risk && a.params.sigma_star == b.params.sigma_star &&
         a.params.bias == b.params.bias && a.params.cadence == b.params.cadence;
}
bool same_shared(const NavReplayConfig& a, const NavReplayConfig& b) {
  if (!same_leverage(a.leverage, b.leverage)) return false;
  const auto& s = a.target; const auto& t = b.target;
  return s.neutralize == t.neutralize && same_price_risk(s.price_risk, t.price_risk) &&
         s.neutralize_max_amplification == t.neutralize_max_amplification &&
         s.neutralize_max_excluded_share == t.neutralize_max_excluded_share &&
         s.hold_band == t.hold_band && s.adv_hold_q == t.adv_hold_q && s.inv_vol == t.inv_vol &&
         s.norm_score == t.norm_score && // v8 Y norm-score-v1 shapes the shared target too
         s.two_speed == t.two_speed &&   // v8 Y-5 two-speed-v1 forms the shared target (and F)
         s.one_way_bps == t.one_way_bps && s.annual_borrow_bps == t.annual_borrow_bps &&
         s.max_working_bytes == t.max_working_bytes && a.initial_nav == b.initial_nav &&
         a.liquidity_window == b.liquidity_window && a.min_vol_pairs == b.min_vol_pairs &&
         a.max_events == b.max_events && a.rate == b.rate && a.rate_rra == b.rate_rra &&
         a.rate_min == b.rate_min && a.rate_max == b.rate_max &&
         a.rate_lambda == b.rate_lambda && a.order_basis == b.order_basis &&
         a.locate_in_aim == b.locate_in_aim && a.liquidity_cache == b.liquidity_cache &&
         a.warm_start_sessions == b.warm_start_sessions && a.book_workers == b.book_workers;
}
} // namespace
namespace nav_book {
// The variants' lockstep groups (variant indices, in order of first appearance): one group
// of every variant, except with the ADV cap (adv_hold_q > 0, shared by same_shared), whose
// shared construction reads aim_leverage: then one group per distinct aim leverage (review
// A-1), each variant capped at its own leverage as in its standalone run. With v8 Y-5
// two-speed-v1 (never with the cap) likewise one group per distinct aim leverage: the
// shared target encodes the fast sleeve F, built at L (theta_s is pinned at .05 and the
// cadence is one per grid, so L alone separates the standalone constructions).
std::vector<std::vector<usize>> leverage_groups(std::span<const NavReplayConfig> variants) {
  std::vector<std::vector<usize>> groups;
  const bool capped = variants.front().target.adv_hold_q > 0;
  const bool two_speed = two_speed_on(variants.front().target);
  for (usize v = 0; v < variants.size(); ++v) {
    auto found = groups.begin();
    if (capped || two_speed)
      found = std::find_if(groups.begin(), groups.end(), [&](const std::vector<usize>& g) {
        return variants[g.front()].target.aim_leverage == variants[v].target.aim_leverage;
      });
    if (found == groups.end()) groups.push_back({v});
    else found->push_back(v);
  }
  return groups;
}
} // namespace nav_book
namespace {
// The lockstep groups of the books (book indices, in order of first appearance; P9 C1): one
// group of every book, except where the shared construction reads a book's own setting: the
// ADV cap reads its aim leverage (review A-1) and its NAV multiple (a capacity book's cap reads
// m x NAV), two-speed-v1's F its aim leverage. Then one group per distinct (leverage,
// multiple), each formed as in the book's standalone run. Over a grid's books this is
// leverage_groups book by book (the variants' books in order), x1 joins the main books.
std::vector<std::vector<usize>> book_groups(std::span<const NavReplayConfig> configs) {
  std::vector<std::vector<usize>> groups;
  const bool capped = configs.front().target.adv_hold_q > 0;
  const bool two_speed = two_speed_on(configs.front().target);
  const auto key = [&](usize k) {
    return std::pair<f64, f64>{capped || two_speed ? configs[k].target.aim_leverage : 0.0,
                               capped ? v7::nav_multiple(configs[k].scenario) : 1.0};
  };
  for (usize k = 0; k < configs.size(); ++k) {
    const auto mine = key(k);
    auto found = std::find_if(groups.begin(), groups.end(), [&](const std::vector<usize>& g) {
      return key(g.front()) == mine;
    });
    if (found == groups.end()) groups.push_back({k});
    else found->push_back(k);
  }
  return groups;
}
} // namespace
namespace nav_book {
// Every variant x every scenario (book v * scenarios + k) in one lockstep run_books (with
// the ADV cap at several aim leverages or NAV multiples, or two-speed-v1 at several leverages,
// one per book group: book_groups). One variant is replay_nav_scenarios: the same validation
// and the same books. P9 C1: each config gets the v7 seam's leverage rule (v7::configure) and
// the books' records join the seam's after the replay (v7::observe). P9 C2: `tape` is handed to
// a one-group run_books (InvalidArgument for several groups); observe false (the calibration
// pass) keeps the books' records out of the seam's.
co::Result<std::vector<std::vector<NavReplayResult>>> replay_books(
    const NavReplayInput& in, std::span<const NavReplayConfig> variants,
    std::span<const NavScenario> scenarios, const Observer& observer, StageClock* stages,
    ConstructionTape* tape, bool observe) {
  if (scenarios.empty() || scenarios.size() > max_scenarios)
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: 1..16 scenarios per replay");
  if (variants.empty() || variants.size() > nav_max_grid_variants)
    return co::Err(co::ErrorCode::InvalidArgument, "nav grid: 1..16 variants");
  if (observer.sink && (variants.size() != 1 || observer.book >= scenarios.size()))
    return co::Err(co::ErrorCode::InvalidArgument, "nav replay: observed book out of range");
  const auto& base = variants.front();
  // From the second variant: the base against itself would refuse a NaN option (NaN != NaN)
  // here with the grid message instead of the option's own refusal in validate_nav_config.
  for (usize v = 1; v < variants.size(); ++v)
    if (!same_shared(base, variants[v]))
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav grid: variants may differ only in rule, cadence, trade fraction, "
                     "monthly budget, band, dust, aim leverage and exit rate");
  // v8 hold-band-v1: the band's state advances on every shared cadence decision, i.e. on the
  // union of the variants' cadence days; a variant would then carry a state its standalone
  // run never forms, so a hold-band grid has one cadence. Likewise v8 Y-5 two-speed-v1,
  // whose fast sleeve F lives in the shared construction state.
  if (hold_band_on(base.target) || two_speed_on(base.target))
    for (const auto& variant : variants)
      if (variant.target.cadence != base.target.cadence)
        return co::Err(co::ErrorCode::InvalidArgument,
                       "nav grid: with --hold-band or --two-speed every variant has the base "
                       "cadence");
  // P9 AL-CLOCK: info-clock-rate-v1's fast rate reads the cadence (1 - 2^(-C/5)) and run_books
  // forms one rate vector per decision for every book, so its variants share the cadence too.
  if (base.rate == NavRateRule::InfoClockV1)
    for (const auto& variant : variants)
      if (variant.target.cadence != base.target.cadence)
        return co::Err(co::ErrorCode::InvalidArgument,
                       "nav grid: with --rate info-clock-rate-v1 every variant has the base "
                       "cadence (its fast rate 1 - 2^(-C/5) is formed once per decision for "
                       "every book)");
  // Books on a pool: the per-name-v1 rates share one buffer, and the spo engines hold every
  // book's state. P9 C1: every other v7 rule and the leverage rule keep their state per book
  // (made on this thread before the books run), so they run on the pool. P9 AL-CLOCK: so does
  // info-clock-rate-v1 (one rate vector per decision, formed on this thread; fix 1 names it).
  if (base.book_workers > 1 &&
      (base.rate == NavRateRule::PerNameV1 || v7::shared_plan_state()))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "nav replay: --book-workers above 1 needs the fixed rate or "
                   "info-clock-rate-v1 (per-name-v1's rates share one buffer) and no spo rule "
                   "(its engines hold every book's state)");
  const usize books = variants.size() * scenarios.size();
  // The NAV volume is authoritative, also for price-risk neutralization.
  TargetReplayInput x = in.target;
  x.volume = in.volume;
  const NavReplayInput input{x, in.volume, in.financing, in.mark, in.info_clock};
  // v8 E-25: what every book's MARK reads: the label marks, else x's own spans (the reads of
  // the replay without them, so every output is unchanged).
  const NavMarkPrices marks = in.mark.present.empty()
      ? NavMarkPrices{x.close, x.raw_close, x.present} : in.mark;
  try {
    std::vector<NavReplayConfig> configs;
    configs.reserve(books);
    for (const auto& variant : variants)
      for (const auto& scenario : scenarios) {
        auto& cfg = configs.emplace_back(variant);
        cfg.scenario = scenario;
        v7::configure(cfg); // P9 C1: --risk-target / --vol-target as the book's leverage rule
        ATX_TRY_VOID(validate_nav_config(cfg));
        if (scenario.financing.rule == NavFinancingRule::TieredSwapV1 &&
            in.financing.shares_out.empty())
          return co::Err(co::ErrorCode::InvalidArgument,
                         "nav replay: tiered financing requires the borrow fields");
      }
    // The locate-in-aim mask is the decision's special tier: no tiers without fields.
    if (base.locate_in_aim && in.financing.shares_out.empty())
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav replay: locate-in-aim requires the borrow fields");
    // Each variant's own target contract; the budget charges every book of the grid.
    for (usize v = 0; v < variants.size(); ++v)
      ATX_TRY_VOID(validate_nav_input(input, configs[v * scenarios.size()], books));
    std::vector<std::unique_ptr<const bk::ReplayCostModel>> models;
    std::vector<Ctx> ctxs;
    models.reserve(books); ctxs.reserve(books);
    for (const auto& cfg : configs) {
      ATX_TRY(auto model, make_cost_model(cfg.scenario));
      models.push_back(std::move(model));
      const auto& s = cfg.scenario; const auto& f = s.financing;
      const bool sqrt_model = s.cost == NavCostRule::SqrtImpactV1;
      Ctx ctx{x, in.volume, cfg, *models.back(),
              s.adverse_terminal ? nav_adverse_long_return : 0.0,
              s.adverse_terminal ? nav_adverse_short_return : 0.0,
              f.flat_short_bps * 1e-4,
              sqrt_model ? (s.half_spread_bps + s.commission_bps) * 1e-4 : 0.0,
              sqrt_model};
      if (f.rule == NavFinancingRule::TieredSwapV1) {
        ctx.long_rate = f.long_spread_bps * 1e-4;
        ctx.short_rate[tier_gc] = (f.short_spread_bps + f.gc_bps) * 1e-4;
        ctx.short_rate[tier_warm] = (f.short_spread_bps + f.warm_bps) * 1e-4;
        ctx.short_rate[tier_special] = (f.short_spread_bps + f.special_bps) * 1e-4;
      }
      ctx.mark = marks;
      ctx.info_clock = in.info_clock; // P9 AL-CLOCK
      ctxs.push_back(ctx);
    }
    std::unique_ptr<par::DetPool> pool;
    if (base.book_workers > 1) pool = std::make_unique<par::DetPool>(base.book_workers);
    std::vector<NavReplayResult> flat(books);
    const auto groups = book_groups(configs);
    if (groups.size() == 1) { // every book in one lockstep run (always without the cap)
      ATX_TRY(flat, run_books(x, in.financing, ctxs, observer, pool.get(), stages, tape));
    } else if (tape) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "nav calibrate: a recorded construction serves one lockstep group only");
    } else {
      // Review A-1 (P9 C1: and the capacity multiples): each group of books with one leverage
      // and multiple runs its own lockstep (its shared construction capped at that leverage
      // and NAV), so every book is its standalone replay; groups run one after the other. The
      // observed book is observed in its own group.
      std::vector<Ctx> members;
      members.reserve(books);
      for (const auto& group : groups) {
        members.clear();
        Observer seen{};
        for (usize j = 0; j < group.size(); ++j) {
          members.push_back(ctxs[group[j]]);
          if (observer.sink && group[j] == observer.book) seen = Observer{observer.sink, j};
        }
        ATX_TRY(auto part, run_books(x, in.financing, members, seen, pool.get(), stages));
        for (usize j = 0; j < part.size(); ++j) flat[group[j]] = std::move(part[j]);
      }
    }
    // P9 C1: the books' records join the v7 seam's (identity without it); P9 C2: not those of a
    // calibration pass, which publishes nothing.
    if (observe) v7::observe(flat);
    std::vector<std::vector<NavReplayResult>> out(variants.size());
    for (usize k = 0; k < books; ++k) out[k / scenarios.size()].push_back(std::move(flat[k]));
    return co::Ok(std::move(out));
  } catch (const std::system_error&) { // the book pool's threads could not start
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: book worker threads");
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: allocation failed");
  } catch (const std::length_error&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav replay: allocation extent");
  }
}
} // namespace nav_book
namespace {
co::Result<std::vector<NavReplayResult>> replay_observed(const NavReplayInput& in,
                                                         const NavReplayConfig& base,
                                                         std::span<const NavScenario> scenarios,
                                                         const Observer& observer,
                                                         StageClock* stages = nullptr) {
  ATX_TRY(auto grid, replay_books(in, std::span<const NavReplayConfig>(&base, 1), scenarios,
                                  observer, stages));
  return co::Ok(std::move(grid.front()));
}
} // namespace
co::Result<std::vector<NavReplayResult>> replay_nav_scenarios(
    const NavReplayInput& in, const NavReplayConfig& base, std::span<const NavScenario> scenarios) {
  return replay_observed(in, base, scenarios, Observer{});
}
co::Result<std::vector<NavReplayResult>> replay_nav_scenarios(
    const NavReplayInput& in, const NavReplayConfig& base, std::span<const NavScenario> scenarios,
    NavHoldingsSink& sink, usize observed) {
  return replay_observed(in, base, scenarios, Observer{&sink, observed});
}
co::Result<std::vector<std::vector<NavReplayResult>>> replay_nav_grid(
    const NavReplayInput& in, std::span<const NavReplayConfig> variants,
    std::span<const NavScenario> scenarios) {
  return replay_books(in, variants, scenarios, Observer{}, nullptr);
}
co::Result<NavReplayResult> replay_nav(const NavReplayInput& in, const NavReplayConfig& cfg) {
  const std::span<const NavScenario> one(&cfg.scenario, 1);
  ATX_TRY(auto results, replay_nav_scenarios(in, cfg, one));
  return co::Ok(std::move(results.front()));
}
co::Result<NavBorrowTiers> nav_borrow_tiers(const NavReplayInput& in, usize d) {
  const auto& x = in.target; const auto& f = in.financing;
  const usize cells = x.dates * x.instruments;
  if (!x.dates || !x.instruments || x.dates > max_dates || x.instruments > max_names ||
      d >= x.dates || x.session_keys.size() != x.dates || x.present.size() != cells ||
      x.member.size() != cells || x.raw_close.size() != cells || f.shares_out.size() != cells ||
      f.si_shares.size() != cells)
    return co::Err(co::ErrorCode::InvalidArgument, "nav borrow tiers: geometry");
  try {
    BorrowTiers tiers(x.instruments, true);
    TierCensus census;
    ATX_TRY_VOID(classify_borrow(x, f, d, tiers, census));
    return co::Ok(NavBorrowTiers{std::move(tiers.tier), std::move(tiers.missing)});
  } catch (const std::bad_alloc&) {
    return co::Err(co::ErrorCode::OutOfRange, "nav borrow tiers: allocation failed");
  }
}
} // namespace atx::impl::strategy
