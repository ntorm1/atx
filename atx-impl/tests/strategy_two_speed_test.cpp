// two-speed-v1 (platform v8 Y-5, lane YCOMB; Rulings PM8-5, PM8-10): the IC composition's fast and
// slow sleeves (the theme parts of the blend, bit for bit, with the fast mass share per date) and the
// NAV construction that trades them: the desired target of every decision equals an independent
// recomputation (each sleeve's own construction, the virtual fast sleeve's recursion, the netted
// aim); with a zero fast share the run is the parent's bit for bit; the option is refused outside
// aim-partial-v5, with the shaping options that keep state, without the replay's state or without
// the sleeves; rule id, recipe and summary carry it only when on.
//
// Suite: TwoSpeed

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/engine/book/two_speed.hpp"
#include "../src/strategy_ic_composition.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_risk_target.hpp"
#include "../src/strategy_spo.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"
#include "../src/strategy_two_speed.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx;
using namespace atx::impl::strategy::spo::fixture;
namespace st = atx::impl::strategy;
namespace sp = atx::impl::strategy::spo;
namespace rt = atx::impl::strategy::risk_target;
namespace v7 = atx::impl::strategy::v7;
using Json = nlohmann::json;
using Ranked = std::pair<f64, usize>;

u64 bits(f64 x) { return std::bit_cast<u64>(x); }
bool same(f64 a, f64 b) { return (std::isnan(a) && std::isnan(b)) || bits(a) == bits(b); }

// The registration (task-YCOMB-report.md, Y-5 half-life table), every entry spelled out.
TEST(TwoSpeed, RegisteredTable) {
  const std::vector<std::pair<std::string, f64>> registered{
      {"value", 252.0}, {"profitability_quality", 252.0}, {"investment_issuance", 252.0},
      {"earnings_momentum", 63.0}, {"price_momentum", 126.0}, {"low_risk", 252.0}, {"short_interest", 63.0},
      {"reversal_seasonality", 5.0}, {"options_implied", 21.0}, {"ownership_flow", 63.0},
      {"filing_events", 21.0}, {"price_volume", 5.0}};
  ASSERT_EQ(st::two_speed_half_lives.size(), registered.size());
  f64 h = 0;
  for (const auto& [theme, half_life] : registered) {
    ASSERT_TRUE(st::two_speed_half_life(theme, h)) << theme;
    EXPECT_EQ(h, half_life) << theme;
    EXPECT_EQ(st::two_speed_fast(h), theme == "reversal_seasonality" || theme == "price_volume") << theme;
  }
  EXPECT_FALSE(st::two_speed_half_life("liquidity", h));
  usize fast = 0;
  for (const auto& [name, half_life] : st::two_speed_half_lives) fast += st::two_speed_fast(half_life) ? 1U : 0U;
  EXPECT_EQ(fast, 2U); // reversal_seasonality and price_volume
  EXPECT_EQ(st::two_speed_rule, "two-speed-v1");
}

// ---- the IC composition's sleeves ----
struct Composer {
  static constexpr usize days = 4, width = 9;
  std::vector<st::IcCompositionCandidate> candidates{{"a1", "a"}, {"b1", "b"}, {"b2", "b"}};
  std::vector<usize> themes{0, 1, 1};
  std::vector<int> signs{1, 1, 1};
  std::vector<u8> member = std::vector<u8>(days * width, 1);
  std::vector<std::vector<f64>> signals = std::vector<std::vector<f64>>(3, std::vector<f64>(days * width));
  st::IcCompositionConfig cfg;
  Composer() {
    cfg.dates = days; cfg.instruments = width; cfg.decision_end = days;
    for (usize d = 0; d < days; ++d) for (usize i = 0; i < width; ++i) {
      const auto at = d * width + i, j = (i + d) % width;
      signals[0][at] = static_cast<f64>((5 * j) % width);
      signals[1][at] = static_cast<f64>(j);
      signals[2][at] = static_cast<f64>((j + 4) % width);
    }
    member[width + 3] = 0;
  }
  std::optional<st::IcCompositionResult> compose(const std::vector<f64>& weights, std::span<const u8> fast = {},
                                                 std::span<const st::IcThemeBlock> schedule = {}) const {
    auto c = st::IcComposition::create(cfg, candidates, member, weights, themes, st::IcThemeRule::standardise);
    if (!c) return std::nullopt;
    if (!schedule.empty() && !c->schedule_theme_masses(schedule)) return std::nullopt;
    if (!fast.empty() && !c->set_theme_sleeves(fast)) return std::nullopt;
    for (usize k = 0; k < candidates.size(); ++k)
      if (!c->add(k, signals[k], signs[k])) return std::nullopt;
    auto out = c->finish();
    if (!out) return std::nullopt;
    return std::move(*out);
  }
};
const std::vector<f64> weights{.5, .25, .25};
const std::vector<u8> a_fast{1, 0};

TEST(TwoSpeed, CompositionSleevesAreTheThemePartsOfTheBlend) {
  const Composer c;
  const auto plain = c.compose(weights), split = c.compose(weights, a_fast);
  const auto a_only = c.compose({.5, 0, 0}), b_only = c.compose({0, .25, .25});
  ASSERT_TRUE(plain && split && a_only && b_only);
  ASSERT_EQ(split->sleeve_fast.size(), plain->signal.size());
  for (usize k = 0; k < plain->signal.size(); ++k) {
    EXPECT_TRUE(same(split->signal[k], plain->signal[k])) << k;       // the blend is unchanged
    EXPECT_TRUE(same(split->sleeve_fast[k], a_only->signal[k])) << k; // fast = theme a's part
    EXPECT_TRUE(same(split->sleeve_slow[k], b_only->signal[k])) << k; // slow = theme b's part
  }
  EXPECT_TRUE(std::isnan(split->sleeve_fast[Composer::width + 3])); // nonmember
  const f64 share = .5 / (0.0 + .5 + (0.0 + .25 + .25));
  for (const f64 x : split->sleeve_fast_share) EXPECT_EQ(x, share);
  EXPECT_TRUE(plain->sleeve_fast.empty() && plain->sleeve_fast_share.empty()); // off: nothing kept
  // Under a theme-tsmom-v1 schedule the fast share follows the masses in force.
  const f64 w_b = 0.0 + .25 + .25;
  const std::vector<st::IcThemeBlock> off{{2, {0.0, 1.0}}};
  const auto scheduled = c.compose(weights, a_fast, off);
  ASSERT_TRUE(scheduled);
  EXPECT_EQ(scheduled->sleeve_fast_share[1], .5 / (.5 + w_b));
  EXPECT_EQ(scheduled->sleeve_fast_share[2], 0.0);
  EXPECT_EQ(scheduled->sleeve_fast[2 * Composer::width], 0.0); // theme a off: the fast sleeve adds nothing
}

// Ruling PM8-16 #9: the fast share of a date counts only the themes with a present member there
// (a theme without one adds nothing to the blend at that date); none present: 0.
TEST(TwoSpeed, FastShareCountsOnlyThemesPresentAtTheDate) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const f64 w_a = 0.5, w_b = 0.0 + .25 + .25, both = w_a / (0.0 + w_a + w_b);
  Composer fast_absent;
  for (usize i = 0; i < Composer::width; ++i) fast_absent.signals[0][2 * Composer::width + i] = nan;
  const auto a = fast_absent.compose(weights, a_fast);
  ASSERT_TRUE(a);
  for (usize d = 0; d < Composer::days; ++d)
    EXPECT_EQ(a->sleeve_fast_share[d], d == 2 ? 0.0 : both) << d;
  for (usize i = 0; i < Composer::width; ++i) // members 0: the fast sleeve adds nothing there
    EXPECT_EQ(a->sleeve_fast[2 * Composer::width + i], 0.0) << i;
  Composer slow_absent;
  for (usize i = 0; i < Composer::width; ++i) {
    slow_absent.signals[1][3 * Composer::width + i] = nan;
    slow_absent.signals[2][3 * Composer::width + i] = nan;
  }
  const auto b = slow_absent.compose(weights, a_fast);
  ASSERT_TRUE(b);
  for (usize d = 0; d < Composer::days; ++d)
    EXPECT_EQ(b->sleeve_fast_share[d], d == 3 ? 1.0 : both) << d;
  Composer none;
  for (auto& s : none.signals)
    for (usize i = 0; i < Composer::width; ++i) s[i] = nan;
  const auto c = none.compose(weights, a_fast);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->sleeve_fast_share[0], 0.0);
  EXPECT_EQ(c->sleeve_fast_share[1], both);
}

TEST(TwoSpeed, CompositionSleeveRefusalsKeepNothing) {
  const Composer c;
  auto comp = st::IcComposition::create(c.cfg, c.candidates, c.member, weights, c.themes,
                                        st::IcThemeRule::standardise);
  ASSERT_TRUE(comp);
  for (const auto& bad : {std::vector<u8>{1}, std::vector<u8>{1, 1}, std::vector<u8>{0, 0}, std::vector<u8>{2, 0}})
    EXPECT_FALSE(comp->set_theme_sleeves(bad));
  for (usize k = 0; k < c.candidates.size(); ++k) ASSERT_TRUE(comp->add(k, c.signals[k], c.signs[k]));
  const auto out = comp->finish();
  ASSERT_TRUE(out);
  EXPECT_TRUE(out->sleeve_fast.empty());
  EXPECT_FALSE(comp->set_theme_sleeves(a_fast)); // after finish
  for (const auto rule : {st::IcThemeRule::redistribute, st::IcThemeRule::residualise}) {
    auto other = st::IcComposition::create(c.cfg, c.candidates, c.member, weights, c.themes, rule);
    ASSERT_TRUE(other);
    EXPECT_FALSE(other->set_theme_sleeves(a_fast));
  }
}

// ---- the NAV construction ----
// The fixture's NAV book at two-speed-v1's registered slow rate theta_s = .05 as its trade fraction
// (Ruling PM8-16 #3), two-speed off: the parent every two-speed run is compared with.
st::NavReplayConfig parent_config() {
  auto c = nav_config();
  c.target.trade_fraction = atx::engine::book::two_speed_slow_theta;
  return c;
}
st::TargetReplayConfig two_speed_config(bool on) {
  auto c = parent_config().target;
  c.two_speed = on;
  return c;
}
struct Sleeves {
  std::vector<f64> fast, slow, share;
  Sleeves(const Role& role, f64 fast_share) : fast(role.signal), slow(role.signal), share(role.d, fast_share) {
    for (auto& x : fast) x = 1.0 - x; // the reversed order (NaN stays NaN)
  }
  void attach(st::TargetReplayInput& x) const {
    x.sleeve_fast = fast; x.sleeve_slow = slow; x.sleeve_fast_share = share;
  }
};

// Closed form over every decision: d_f and d_s are each sleeve's own construction (two_speed off),
// F_next = F + theta_f (L m_f d_f - F) (0 off membership), desired = m_s d_s + (F + (F_next - F) /
// theta) / L, with theta_f = 1 - 2^(-C/5) at the cadence C (Ruling PM8-16 #4: C 1 .1294, C 5 .5).
class TwoSpeedCadence : public ::testing::TestWithParam<usize> {};
TEST_P(TwoSpeedCadence, DesiredIsTheNettedSleeveAim) {
  const Role role(30, 40, 29);
  const Sleeves sleeves(role, 0.3);
  auto x = role.target();
  sleeves.attach(x);
  auto fast_x = x, slow_x = x;
  fast_x.signal = sleeves.fast; slow_x.signal = sleeves.slow;
  auto on = two_speed_config(true), off = two_speed_config(false);
  on.cadence = off.cadence = GetParam();
  const f64 L = on.aim_leverage, theta = on.trade_fraction;
  const f64 theta_f = 1.0 - std::exp2(-static_cast<f64>(GetParam()) / 5.0);
  EXPECT_EQ(theta_f, GetParam() == 5U ? 0.5 : atx::engine::book::two_speed_fast_theta(1.0));
  st::detail::DesiredState state;
  std::vector<f64> F(role.n, 0.0);
  std::vector<Ranked> row;
  st::PriceRiskScratch scratch;
  for (usize d = 0; d < role.d; ++d) {
    std::vector<f64> desired(role.n, 0.0), df(role.n, 0.0), ds(role.n, 0.0);
    st::ConstructionDay rec, rf, rs;
    const auto r = st::detail::form_desired(x, on, d, row, desired, scratch, rec, {}, &state);
    ASSERT_TRUE(r && *r) << d;
    ASSERT_TRUE(st::detail::form_desired(fast_x, off, d, row, df, scratch, rf));
    ASSERT_TRUE(st::detail::form_desired(slow_x, off, d, row, ds, scratch, rs));
    for (usize i = 0; i < role.n; ++i) {
      const f64 before = F[i];
      if (!role.member[d * role.n + i]) {
        F[i] = 0.0;
        EXPECT_EQ(desired[i], 0.0) << d << ' ' << i;
        continue;
      }
      F[i] = before + theta_f * (L * 0.3 * df[i] - before);
      const f64 want = (1.0 - 0.3) * ds[i] + (before + (F[i] - before) / theta) / L;
      EXPECT_NEAR(desired[i], want, 1e-15) << d << ' ' << i;
      EXPECT_NEAR(state.fast[i], F[i], 1e-15) << d << ' ' << i;
    }
  }
}
INSTANTIATE_TEST_SUITE_P(TwoSpeed, TwoSpeedCadence, ::testing::Values(usize{1}, usize{5}));

// A zero fast share and the blend as the slow sleeve: F stays 0 and every NAV day is the parent's.
TEST(TwoSpeed, ZeroFastShareIsTheParentRunBitForBit) {
  const Role role(40, 12, 53);
  const Sleeves sleeves(role, 0.0);
  auto in = role.nav();
  sleeves.attach(in.target);
  auto cfg = parent_config();
  const auto parent = st::replay_nav(role.nav(), cfg);
  cfg.target.two_speed = true;
  const auto two = st::replay_nav(in, cfg);
  ASSERT_TRUE(parent) << parent.error().to_string();
  ASSERT_TRUE(two) << two.error().to_string();
  ASSERT_EQ(parent->days.size(), two->days.size());
  for (usize t = 0; t < parent->days.size(); ++t) {
    EXPECT_EQ(bits(two->days[t].net_return), bits(parent->days[t].net_return)) << t;
    EXPECT_EQ(bits(two->days[t].one_way_turnover), bits(parent->days[t].one_way_turnover)) << t;
  }
}

// Review YCOMB #12: the same under price-risk-v1 and --locate-in-aim (borrow fields, so special
// names' short aims are zeroed), with the fast sleeve the runner saves at a zero fast share (flat:
// 0 at members). Every NAV day, every construction record (outcome, statistics, locate count) and
// the construction and NAV summaries are the parent's bit for bit; only the two-speed keys differ.
TEST(TwoSpeed, ZeroFastShareUnderNeutralizationAndLocateInAimIsTheParentRunBitForBit) {
  const Role role(70, 12, 53);
  Lcg g{11};
  std::vector<f64> shares_out(role.d * role.n), si_shares(role.d * role.n);
  for (usize k = 0; k < shares_out.size(); ++k) { // shares out 1e6..1e9, SI ratio 0..0.25
    shares_out[k] = std::pow(10.0, 6 + 3 * g.next());
    si_shares[k] = shares_out[k] * 0.25 * g.next();
  }
  auto parent_in = role.nav();
  parent_in.financing = st::NavFinancingFields{shares_out, si_shares};
  Sleeves sleeves(role, 0.0);
  for (auto& v : sleeves.fast) if (!std::isnan(v)) v = 0.0;
  auto in = parent_in;
  sleeves.attach(in.target);
  auto cfg = parent_config();
  cfg.target = neutral_config(cfg.target);
  cfg.locate_in_aim = true;
  const auto parent = st::replay_nav(parent_in, cfg);
  auto two_cfg = cfg;
  two_cfg.target.two_speed = true;
  const auto two = st::replay_nav(in, two_cfg);
  ASSERT_TRUE(parent) << parent.error().to_string();
  ASSERT_TRUE(two) << two.error().to_string();
  ASSERT_EQ(parent->days.size(), two->days.size());
  std::vector<st::ConstructionDay> parent_records, two_records;
  usize zeroed = 0, skipped = 0;
  for (usize t = 0; t < parent->days.size(); ++t) {
    const auto& a = parent->days[t];
    const auto& b = two->days[t];
    EXPECT_EQ(bits(b.net_return), bits(a.net_return)) << t;
    EXPECT_EQ(bits(b.one_way_turnover), bits(a.one_way_turnover)) << t;
    EXPECT_EQ(bits(b.planned_gross), bits(a.planned_gross)) << t;
    EXPECT_EQ(b.rebalance, a.rebalance) << t;
    const auto& p = a.construction;
    const auto& c = b.construction;
    EXPECT_EQ(c.rebalance, p.rebalance) << t;
    EXPECT_EQ(c.neutralize, p.neutralize) << t;
    EXPECT_EQ(c.neutralize_used, p.neutralize_used) << t;
    EXPECT_EQ(c.neutralize_excluded, p.neutralize_excluded) << t;
    EXPECT_EQ(bits(c.neutralize_excluded_share), bits(p.neutralize_excluded_share)) << t;
    EXPECT_EQ(bits(c.neutralize_amplification), bits(p.neutralize_amplification)) << t;
    EXPECT_EQ(c.locate_zeroed, p.locate_zeroed) << t;
    EXPECT_EQ(c.banded_names, p.banded_names) << t;
    const bool parent_skip = p.neutralize != st::NeutralizeOutcome::NotAttempted &&
                             p.neutralize != st::NeutralizeOutcome::Applied;
    EXPECT_EQ(c.two_speed_sleeve_skipped, parent_skip) << t;
    EXPECT_EQ(c.two_speed_parent_skipped, parent_skip) << t;
    zeroed += p.locate_zeroed;
    skipped += parent_skip ? 1U : 0U;
    if (a.decision) { parent_records.push_back(p); two_records.push_back(c); }
  }
  EXPECT_GT(zeroed, 0U);  // locate-in-aim zeroes special names' short aims
  EXPECT_GT(skipped, 0U); // early decisions skip (the windows fill)
  auto parent_summary = Json::parse(st::detail::construction_summary_json(cfg.target,
                                                                          parent_records));
  auto two_summary = Json::parse(st::detail::construction_summary_json(two_cfg.target,
                                                                       two_records));
  ASSERT_TRUE(two_summary.at("construction").contains("two_speed"));
  for (auto* s : {&parent_summary, &two_summary}) {
    s->at("construction").erase("two_speed");
    s->at("construction").erase("rule_id");
  }
  EXPECT_EQ(two_summary, parent_summary);
  const auto pn = st::summarize_nav(*parent);
  const auto tn = st::summarize_nav(*two);
  ASSERT_TRUE(pn && tn);
  EXPECT_EQ(bits(tn->total_net_return), bits(pn->total_net_return));
  EXPECT_EQ(bits(tn->final_nav), bits(pn->final_nav));
  EXPECT_EQ(bits(tn->total_actual_turnover), bits(pn->total_actual_turnover));
  EXPECT_EQ(bits(tn->trade_cost_dollars), bits(pn->trade_cost_dollars));
  EXPECT_EQ(bits(tn->borrow_dollars), bits(pn->borrow_dollars));
  EXPECT_EQ(tn->blocked_short_name_decisions, pn->blocked_short_name_decisions);
  EXPECT_EQ(tn->member_tier_days, pn->member_tier_days);
}

// A real fast sleeve moves the book: the run completes and differs from the parent's.
TEST(TwoSpeed, NavReplayTradesTheNettedSleeves) {
  const Role role(40, 12, 53);
  const Sleeves sleeves(role, 0.4);
  auto in = role.nav();
  sleeves.attach(in.target);
  auto cfg = parent_config();
  const auto parent = st::replay_nav(role.nav(), cfg);
  cfg.target.two_speed = true;
  const auto two = st::replay_nav(in, cfg);
  ASSERT_TRUE(parent && two);
  bool differs = false;
  for (usize t = 0; t < two->days.size(); ++t)
    differs = differs || bits(two->days[t].net_return) != bits(parent->days[t].net_return);
  EXPECT_TRUE(differs);
  // Without the sleeves the replay refuses.
  const auto bare = st::replay_nav(role.nav(), cfg);
  ASSERT_FALSE(bare);
  EXPECT_NE(bare.error().to_string().find("two-speed-v1 needs the saved sleeves"), std::string::npos);
}

TEST(TwoSpeed, RefusedOutsideItsConstruction) {
  const Role role(20, 12, 31);
  const Sleeves sleeves(role, 0.3);
  auto x = role.target();
  sleeves.attach(x);
  // The target replay holds a construction state: with the sleeves it runs, without them it refuses.
  EXPECT_TRUE(st::replay_targets(x, two_speed_config(true)));
  const auto bare = st::replay_targets(role.target(), two_speed_config(true));
  ASSERT_FALSE(bare);
  EXPECT_NE(bare.error().to_string().find("two-speed-v1 needs the saved sleeves"), std::string::npos);
  // form_desired without a state refuses before any write.
  std::vector<Ranked> row;
  std::vector<f64> desired(role.n, 7.0);
  st::PriceRiskScratch scratch;
  st::ConstructionDay rec;
  const auto stateless = st::detail::form_desired(x, two_speed_config(true), 0, row, desired, scratch, rec);
  ASSERT_FALSE(stateless);
  EXPECT_NE(stateless.error().to_string().find("construction state"), std::string::npos);
  for (const f64 v : desired) EXPECT_EQ(v, 7.0);
  auto banded = two_speed_config(true);
  banded.hold_band = 0.1;
  auto scaled = two_speed_config(true);
  scaled.inv_vol = true;
  auto capped = two_speed_config(true);
  capped.adv_hold_q = 0.05;
  auto baseline = two_speed_config(true);
  baseline.rule = st::TargetReplayRule::BaselineTargetV1;
  baseline.aim_leverage = 1.0; baseline.dust_multiple = 0.0; baseline.exit_rate = 1.0;
  for (const auto* cfg : {&banded, &scaled, &capped, &baseline}) {
    const auto r = st::replay_targets(x, *cfg);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().to_string().find("two-speed-v1 needs aim-partial-v5"), std::string::npos)
        << r.error().to_string();
  }
  // Ruling PM8-16 #3: theta_s is the registered .05, so any other trade fraction is refused.
  for (const f64 theta : {0.25, 0.0500001, 1.0}) {
    auto other = two_speed_config(true);
    other.trade_fraction = theta;
    const auto r = st::replay_targets(x, other);
    ASSERT_FALSE(r) << theta;
    EXPECT_NE(r.error().to_string().find("two-speed-v1 needs trade_fraction .05"),
              std::string::npos)
        << r.error().to_string();
    other.two_speed = false; // the parent at that rate runs
    EXPECT_TRUE(st::replay_targets(role.target(), other)) << theta;
  }
}

TEST(TwoSpeed, RuleIdRecipeAndSummaryCarryTheRuleOnlyWhenOn) {
  const auto on = two_speed_config(true), off = two_speed_config(false);
  EXPECT_NE(st::detail::construction_rule_id(on).find("+two-speed-v1"), std::string::npos);
  EXPECT_EQ(st::detail::construction_rule_id(off).find("two-speed"), std::string::npos);
  auto recipe_on = Json::parse(st::detail::construction_recipe_json(on));
  const auto recipe_off = Json::parse(st::detail::construction_recipe_json(off));
  EXPECT_EQ(recipe_on.at("two_speed"), "two-speed-v1");
  EXPECT_EQ(recipe_on.at("two_speed_theta_fast").get<f64>(),
            atx::engine::book::two_speed_fast_theta(1.0));
  recipe_on.erase("two_speed"); recipe_on.erase("two_speed_theta_fast");
  recipe_on.erase("two_speed_rule");
  EXPECT_EQ(recipe_on, recipe_off);
  const std::vector<st::ConstructionDay> days(3);
  const auto summary_on = Json::parse(st::detail::construction_summary_json(on, days));
  const auto summary_off = Json::parse(st::detail::construction_summary_json(off, days));
  const auto& block = summary_on.at("construction").at("two_speed");
  EXPECT_EQ(block.at("theta_slow").get<f64>(), 0.05);
  EXPECT_EQ(block.at("cadence").get<usize>(), 1U);
  EXPECT_EQ(block.at("theta_fast").get<f64>(), atx::engine::book::two_speed_fast_theta(1.0));
  EXPECT_FALSE(summary_off.at("construction").contains("two_speed"));
  // Ruling PM8-16 #4: at cadence 5 the recipe and the summary print C and theta_f = 1 - 2^(-1) = .5.
  auto weekly = on;
  weekly.cadence = 5;
  EXPECT_EQ(Json::parse(st::detail::construction_recipe_json(weekly)).at("two_speed_theta_fast"),
            0.5);
  const auto summary_weekly = Json::parse(st::detail::construction_summary_json(weekly, days));
  EXPECT_EQ(summary_weekly.at("construction").at("two_speed").at("cadence").get<usize>(), 5U);
  EXPECT_EQ(summary_weekly.at("construction").at("two_speed").at("theta_fast").get<f64>(), 0.5);
}
// ---- Y-5 / Y-1 composition (registered order Y-5 -> X-10 -> Y-1): the netted aim is the book target;
// --vol-target / --risk-target scale its leverage (the scaler reads the net book, never F) ----
std::shared_ptr<const sp::RiskStore> clean_store(const Directory& dir, const Role& role, u64 seed) {
  const std::vector<u8> forecast(role.d, u8{1});
  const auto sha = write_risk_model(dir.path, role.sessions, role.n, forecast, "role-sha", seed);
  auto store = sp::RiskStore::open(dir.path.string(), sha, "role-sha");
  EXPECT_TRUE(store) << store.error().to_string();
  return store ? std::make_shared<const sp::RiskStore>(std::move(*store)) : nullptr;
}
rt::Options vol_options() {
  rt::Options o;
  o.on = true;
  o.law = rt::Law::vol_target_v1;
  return o;
}
atx::core::Result<v7::NavV7Command> parse(std::vector<std::string> args) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  return v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data());
}

// With a zero fast share the two-speed book under vol-target-v1 is the parent under vol-target-v1:
// every NAV day and every L_t the scaler records, bit for bit.
TEST(TwoSpeed, UnderVolTargetZeroShareIsTheParentRunBitForBit) {
  const Directory dir;
  const Role role(70, 12, 53);
  const auto risk = clean_store(dir, role, 3);
  ASSERT_NE(risk, nullptr);
  v7::NavV7Options o;
  o.risk_target = vol_options();
  o.spo_risk = risk;
  auto cfg = parent_config();
  std::vector<f64> parent_returns, parent_leverage;
  {
    const v7::ScopedNavExtension extension(o);
    const auto parent = st::replay_nav(role.nav(), cfg);
    ASSERT_TRUE(parent) << parent.error().to_string();
    for (const auto& day : parent->days) parent_returns.push_back(day.net_return);
    for (const auto& r : extension.risk_target_scaler()->records()) parent_leverage.push_back(r.leverage);
  }
  const Sleeves sleeves(role, 0.0);
  auto in = role.nav();
  sleeves.attach(in.target);
  cfg.target.two_speed = true;
  const v7::ScopedNavExtension extension(o);
  const auto two = st::replay_nav(in, cfg);
  ASSERT_TRUE(two) << two.error().to_string();
  ASSERT_EQ(two->days.size(), parent_returns.size());
  for (usize t = 0; t < parent_returns.size(); ++t)
    EXPECT_EQ(bits(two->days[t].net_return), bits(parent_returns[t])) << t;
  const auto records = extension.risk_target_scaler()->records();
  ASSERT_EQ(records.size(), parent_leverage.size());
  ASSERT_GT(records.size(), 60U);
  for (usize k = 0; k < records.size(); ++k)
    EXPECT_EQ(bits(records[k].leverage), bits(parent_leverage[k])) << k;
}

// A fast share of .3 under vol-target-v1: the hook plans the carried netted aim at the scaler's
// L_t, i.e. update_weights with --aim-leverage L_t on the netted desired target plus the carry of F
// from the book's previous lambda = L_t / L (engine::book::two_speed_carry; Ruling PM8-16 #10), bit
// for bit; without F entering the rebalance the hook refuses before the scaler moves. F follows the
// run's L whatever L_t is. (The book's closed form under a moving L_t is the replay test below.)
TEST(TwoSpeed, UnderVolTargetTheHookPlansTheCarriedNettedAimAtLt) {
  const Directory dir;
  const Role role(50, 12, 41);
  const auto risk = clean_store(dir, role, 5);
  ASSERT_NE(risk, nullptr);
  auto cfg = parent_config();
  cfg.target.two_speed = true;
  const Sleeves sleeves(role, 0.3);
  auto x = role.target();
  sleeves.attach(x);
  auto fast_x = x;
  fast_x.signal = sleeves.fast;
  auto plain = cfg.target;
  plain.two_speed = false;
  const f64 L = cfg.target.aim_leverage, theta = cfg.target.trade_fraction;
  const f64 theta_f = atx::engine::book::two_speed_fast_theta(1.0);
  v7::NavV7Options o;
  o.risk_target = vol_options();
  o.spo_risk = risk;
  const v7::ScopedNavExtension extension(o);
  st::detail::DesiredState state;
  std::vector<Ranked> row;
  st::PriceRiskScratch scratch;
  std::vector<f64> current(role.n, 0.0), F(role.n, 0.0);
  f64 lambda_prev = 0; // the book's lambda at its previous rebalance (none yet)
  for (usize d = 0; d < 45; ++d) {
    std::vector<f64> desired(role.n, 0.0), df(role.n, 0.0);
    st::ConstructionDay rec, rf;
    ASSERT_TRUE(
        st::detail::form_desired(x, cfg.target, d, row, desired, scratch, rec, {}, &state));
    ASSERT_TRUE(st::detail::form_desired(fast_x, plain, d, row, df, scratch, rf));
    std::vector<f64> hooked = current, at_lt = current;
    st::TargetReplayDay a, b;
    if (d == 0) { // without F entering the rebalance the scaled two-speed plan refuses
      std::vector<f64> untouched = current;
      st::TargetReplayDay z;
      EXPECT_FALSE(v7::plan(x, cfg, d, true, 0.0, 1e8, desired, untouched, z, {}));
      EXPECT_TRUE(extension.risk_target_scaler()->records().empty()); // the scaler never moved
    }
    ASSERT_TRUE(v7::plan(x, cfg, d, true, 0.0, 1e8, desired, hooked, a, {}, {}, {},
                         state.fast_before));
    auto t = cfg.target;
    t.aim_leverage = extension.risk_target_scaler()->records().back().leverage;
    const f64 lambda = t.aim_leverage / L;
    std::vector<f64> carried = desired;
    ASSERT_TRUE(atx::engine::book::two_speed_carry(x.member.subspan(d * role.n, role.n),
                                                   state.fast_before,
                                                   lambda_prev > 0 ? lambda_prev : lambda, lambda,
                                                   L, theta, carried));
    lambda_prev = lambda;
    ASSERT_TRUE(st::detail::update_weights(x, t, d, true, 0.0, carried, at_lt, b));
    for (usize i = 0; i < role.n; ++i)
      EXPECT_EQ(bits(hooked[i]), bits(at_lt[i])) << d << ' ' << i;
    current = hooked;
    for (usize i = 0; i < role.n; ++i) {
      const f64 before = F[i];
      F[i] = role.member[d * role.n + i] ? before + theta_f * (L * 0.3 * df[i] - before) : 0.0;
      EXPECT_NEAR(state.fast[i], F[i], 1e-15) << d << ' ' << i;
    }
  }
}

// The observed book's decisions: the plan's current weight (held_weight, bit for bit) and the
// rule's plan per instrument (a name the export omits has both 0).
struct Plans final : st::NavHoldingsSink {
  explicit Plans(usize names) : n(names) {}
  usize n;
  std::vector<usize> rows;
  std::vector<std::vector<f64>> current, rule;
  atx::core::Status session(const st::NavReplayDay& day,
                            std::span<const st::NavHolding> names) override {
    if (!day.decision) return atx::core::Ok();
    rows.push_back(day.session_index);
    auto& c = current.emplace_back(n, 0.0);
    auto& r = rule.emplace_back(n, 0.0);
    for (const auto& h : names) {
      c[h.index] = h.held_weight;
      r[h.index] = h.rule_weight;
    }
    return atx::core::Ok();
  }
};

// Review YCOMB #13 (Ruling PM8-16 #10) through replay_nav: under risk-target-v1 with a target far
// below the book's forecast (sigma* 1e-4, so every estimate clips at the floor .8 L) L_t leaves L
// at the first estimate. Every rebalance's plan of every member is, in closed form, the remainder
// R = c - lambda_prev F moving at theta_s toward lambda L m_s d_s plus the scaled fast sleeve
// lambda F_next: c the plan's current weight, d_s and d_f each sleeve's own construction, F the
// recursion F_next = F + theta_f (L m_f d_f - F), lambda = L_t / L from the scaler's records and
// lambda_prev the book's lambda at its previous rebalance (no dust, immediate exit).
TEST(TwoSpeed, UnderARiskTargetTheBookIsTheScaledSleeveDecomposition) {
  const Directory dir;
  const Role role(50, 12, 41);
  const auto risk = clean_store(dir, role, 5);
  ASSERT_NE(risk, nullptr);
  auto cfg = parent_config();
  cfg.target.two_speed = true;
  cfg.target.dust_multiple = 0.0;
  cfg.target.exit_rate = 1.0;
  const Sleeves sleeves(role, 0.3);
  auto in = role.nav();
  sleeves.attach(in.target);
  auto fast_x = in.target, slow_x = in.target;
  fast_x.signal = sleeves.fast;
  slow_x.signal = sleeves.slow;
  auto plain = cfg.target;
  plain.two_speed = false;
  v7::NavV7Options o;
  o.risk_target.on = true;
  o.risk_target.law = rt::Law::risk_target_v1;
  o.risk_target.params.sigma_star = 1e-4;
  o.spo_risk = risk;
  const v7::ScopedNavExtension extension(o);
  Plans plans(role.n);
  const std::array<st::NavScenario, 1> one{cfg.scenario};
  const auto run = st::replay_nav_scenarios(in, cfg, one, plans, 0);
  ASSERT_TRUE(run) << run.error().to_string();
  std::map<i64, f64> leverage; // L_t of every scored decision, by session
  for (const auto& r : extension.risk_target_scaler()->records()) leverage[r.session] = r.leverage;
  const f64 L = cfg.target.aim_leverage, theta = cfg.target.trade_fraction;
  const f64 theta_f = atx::engine::book::two_speed_fast_theta(1.0);
  std::vector<Ranked> row;
  st::PriceRiskScratch scratch;
  std::vector<f64> F(role.n, 0.0);
  f64 lambda_prev = 0, carry_max = 0;
  usize scaled = 0, rescaled = 0, checked = 0;
  ASSERT_GT(plans.rows.size(), 40U);
  for (usize k = 0; k < plans.rows.size(); ++k) {
    const usize d = plans.rows[k];
    ASSERT_EQ(leverage.count(role.sessions[d]), 1U) << d;
    const f64 lambda = leverage.at(role.sessions[d]) / L;
    if (lambda_prev == 0) lambda_prev = lambda;
    scaled += lambda != 1.0 ? 1U : 0U;
    rescaled += lambda != lambda_prev ? 1U : 0U;
    std::vector<f64> df(role.n, 0.0), ds(role.n, 0.0);
    st::ConstructionDay rf, rs;
    ASSERT_TRUE(st::detail::form_desired(fast_x, plain, d, row, df, scratch, rf));
    ASSERT_TRUE(st::detail::form_desired(slow_x, plain, d, row, ds, scratch, rs));
    for (usize i = 0; i < role.n; ++i) {
      if (!role.member[d * role.n + i]) { // the immediate exit
        F[i] = 0.0;
        EXPECT_EQ(plans.rule[k][i], 0.0) << d << ' ' << i;
        continue;
      }
      const f64 c = plans.current[k][i];
      const f64 before = F[i];
      F[i] = before + theta_f * (L * 0.3 * df[i] - before);
      const f64 remainder = c - lambda_prev * before;
      const f64 want = remainder +
          theta * (lambda * L * (1.0 - 0.3) * ds[i] - remainder) + lambda * F[i];
      EXPECT_NEAR(plans.rule[k][i], want, 1e-13) << d << ' ' << i;
      carry_max = std::max(carry_max,
                           std::abs((lambda - lambda_prev) * (1.0 - theta) / theta * before));
      ++checked;
    }
    lambda_prev = lambda;
  }
  EXPECT_GT(checked, 0U);
  EXPECT_GT(scaled, 0U);      // L_t != L
  EXPECT_GT(rescaled, 0U);    // the scale moved between two rebalances
  EXPECT_GT(carry_max, 1e-6); // and there the carry moved the plan
}

TEST(TwoSpeed, ParseComposesWithTheScalersAndRefusesTheRest) {
  const std::vector<std::string> base{"nav", "--rule", "aim-partial-v5", "--aim-leverage", "2", "--output", "x",
                                      "--two-speed", "two-speed-v1"};
  const std::vector<std::string> store{"--risk-model", "risk", "--risk-model-sha256", "abc"};
  for (const auto& scaler : {std::vector<std::string>{"--vol-target", "vol-target-v1"},
                             std::vector<std::string>{"--risk-target", ".05"}}) {
    auto args = base;
    args.insert(args.end(), scaler.begin(), scaler.end());
    args.insert(args.end(), store.begin(), store.end());
    const auto r = parse(args);
    ASSERT_TRUE(r) << r.error().to_string();
    EXPECT_TRUE(r->options.risk_target.on);
    EXPECT_NE(std::find(r->args.begin(), r->args.end(), "--two-speed"), r->args.end()); // the replay's flag
  }
  for (const auto& args : {std::vector<std::string>{"nav", "--rule", "aim-partial-v6", "--output", "x", "--two-speed",
                                                    "two-speed-v1"},
                           std::vector<std::string>{"nav", "--rule", "aim-partial-v5", "--rate", "per-name-v1",
                                                    "--output", "x", "--two-speed", "two-speed-v1"}}) {
    const auto r = parse(args);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().to_string().find("--two-speed"), std::string::npos) << r.error().to_string();
  }
}

// Review YCOMB #1: the netted aim divides the fast move by the one slow rate, so a per-name rate
// (each name stepping at its own theta_i) is refused by the NAV config itself, v7 flags or not.
TEST(TwoSpeed, PerNameRateIsRefused) {
  const Role role(30, 12, 53);
  const Sleeves sleeves(role, 0.3);
  auto in = role.nav();
  sleeves.attach(in.target);
  auto cfg = parent_config();
  cfg.target.two_speed = true;
  cfg.rate = st::NavRateRule::PerNameV1;
  const auto r = st::replay_nav(in, cfg);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error().to_string().find("two-speed-v1 needs the fixed trading rate"),
            std::string::npos)
      << r.error().to_string();
}

// Review YCOMB #2: F lives in the shared construction, so a two-speed grid has one cadence, and
// its variants run one lockstep per aim leverage: each is its standalone replay bit for bit.
TEST(TwoSpeed, GridHasOneCadenceAndOneLockstepPerLeverage) {
  const Role role(40, 12, 53);
  const Sleeves sleeves(role, 0.4);
  auto in = role.nav();
  sleeves.attach(in.target);
  auto base = parent_config();
  base.target.two_speed = true;
  const std::array<st::NavScenario, 1> one{base.scenario};
  auto slower = base;
  slower.target.cadence = 2;
  const std::vector<st::NavReplayConfig> mixed{base, slower};
  const auto refused = st::replay_nav_grid(in, mixed, one);
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().to_string().find("--two-speed every variant has the base cadence"),
            std::string::npos)
      << refused.error().to_string();
  auto higher = base;
  higher.target.aim_leverage = 1.5;
  auto dusty = base; // the base's leverage: the base's lockstep group
  dusty.target.dust_multiple = 0.2;
  const std::vector<st::NavReplayConfig> grid{base, higher, dusty};
  const auto run = st::replay_nav_grid(in, grid, one);
  ASSERT_TRUE(run) << run.error().to_string();
  ASSERT_EQ(run->size(), grid.size());
  for (usize v = 0; v < grid.size(); ++v) {
    const auto alone = st::replay_nav_scenarios(in, grid[v], one);
    ASSERT_TRUE(alone) << alone.error().to_string();
    ASSERT_EQ((*run)[v].size(), 1U) << v;
    const auto& a = (*run)[v].front().days;
    const auto& b = alone->front().days;
    ASSERT_EQ(a.size(), b.size()) << v;
    for (usize t = 0; t < a.size(); ++t) {
      EXPECT_EQ(bits(a[t].net_return), bits(b[t].net_return)) << v << ' ' << t;
      EXPECT_EQ(bits(a[t].planned_gross), bits(b[t].planned_gross)) << v << ' ' << t;
    }
  }
}

// price-risk-v1 on short windows a 70-session fixture fills (beta 40, vol 20: strategy_live_test's
// v6.1 book).
st::TargetReplayConfig neutral_config(st::TargetReplayConfig c) {
  c.neutralize = st::TargetNeutralize::PriceRiskV1;
  c.price_risk.beta_window = 40; c.price_risk.vol_window = 20;
  c.price_risk.adv_window = 10; c.price_risk.min_return_pairs = 20;
  c.price_risk.min_names = 5;
  return c;
}

// Review YCOMB #5: when only the fast sleeve's neutralization skips, the record carries the fast
// sleeve's outcome and statistics (before, the slow sleeve's Applied beside a skipped rebalance),
// so the summary counts the decision as a cadence decision and as a neutralization skip.
TEST(TwoSpeed, AFastOnlySkipIsTheRecordsSkip) {
  const Role role(70, 12, 53), other(70, 12, 97); // one membership, two random blends
  const std::vector<f64> share(role.d, 0.3);
  auto x = role.target();
  x.sleeve_fast = other.signal; x.sleeve_slow = role.signal; x.sleeve_fast_share = share;
  auto fast_x = x;
  fast_x.signal = other.signal;
  const auto plain = neutral_config(two_speed_config(false));
  // A decision where both sleeves neutralize and the fast sleeve amplifies more.
  std::vector<Ranked> row;
  st::PriceRiskScratch scratch;
  st::ConstructionDay fast_record, slow_record;
  usize at = role.d;
  for (usize d = 45; d < role.d && at == role.d; ++d) {
    std::vector<f64> df(role.n, 0.0), ds(role.n, 0.0);
    st::ConstructionDay rf, rs;
    const auto f = st::detail::form_desired(fast_x, plain, d, row, df, scratch, rf);
    const auto s = st::detail::form_desired(x, plain, d, row, ds, scratch, rs);
    ASSERT_TRUE(f && s) << d;
    if (*f && *s && std::isfinite(rf.neutralize_amplification) &&
        std::isfinite(rs.neutralize_amplification) &&
        rf.neutralize_amplification > rs.neutralize_amplification) {
      at = d; fast_record = rf; slow_record = rs;
    }
  }
  ASSERT_LT(at, role.d);
  auto on = neutral_config(two_speed_config(true));
  on.neutralize_max_amplification =
      0.5 * (fast_record.neutralize_amplification + slow_record.neutralize_amplification);
  st::detail::DesiredState state;
  std::vector<f64> desired(role.n, 0.0);
  st::ConstructionDay rec;
  const auto r = st::detail::form_desired(x, on, at, row, desired, scratch, rec, {}, &state);
  ASSERT_TRUE(r) << r.error().to_string();
  EXPECT_FALSE(*r); // the fast sleeve's skip skips the rebalance
  EXPECT_TRUE(rec.two_speed_sleeve_skipped);
  EXPECT_EQ(rec.neutralize, st::NeutralizeOutcome::SkippedAmplification);
  EXPECT_EQ(bits(rec.neutralize_amplification), bits(fast_record.neutralize_amplification));
  EXPECT_EQ(rec.neutralize_used, fast_record.neutralize_used);
  EXPECT_EQ(rec.neutralize_excluded, fast_record.neutralize_excluded);
  EXPECT_EQ(bits(rec.neutralize_excluded_share), bits(fast_record.neutralize_excluded_share));
  const std::vector<st::ConstructionDay> days{rec};
  const auto s = Json::parse(st::detail::construction_summary_json(on, days)).at("construction");
  EXPECT_EQ(s.at("cadence_rebalance_decisions").get<usize>(), 1U);
  EXPECT_EQ(s.at("rebalanced_decisions").get<usize>(), 0U);
  EXPECT_EQ(s.at("neutralize_skipped_decisions").get<usize>(), 1U);
  EXPECT_EQ(s.at("neutralize_skip_reasons").at("amplification").get<usize>(), 1U);
  EXPECT_EQ(s.at("two_speed").at("rebalances_skipped_by_a_sleeve").get<usize>(), 1U);
}

// Review YCOMB #7: the workspace budget charges two-speed's per-name state (F, F entering the
// rebalance, the fast desired target, the parent diagnostic's scratch); off, nothing.
TEST(TwoSpeed, BudgetChargesTheSleeveState) {
  for (const usize n : {usize{1}, usize{12}, usize{20000}}) {
    const auto on = st::detail::construction_scratch_bytes(two_speed_config(true), n);
    const auto off = st::detail::construction_scratch_bytes(two_speed_config(false), n);
    EXPECT_EQ(on - off, u64{n} * 4U * sizeof(f64)) << n;
    EXPECT_EQ(off, 0U) << n; // no neutralization and no other v8 option: no state
  }
}

// Review YCOMB #8: with the flags absent the record grows only by norm-score-v1's two fields; the
// two-speed flags sit in the padding after `neutralize` (no tail padding: the record ends at its
// last f64).
TEST(TwoSpeed, FlagsSitInTheRecordsPadding) {
  EXPECT_LT(offsetof(st::ConstructionDay, two_speed_parent_failed),
            offsetof(st::ConstructionDay, neutralize_used));
  EXPECT_EQ(sizeof(st::ConstructionDay), offsetof(st::ConstructionDay, norm_max_abs) + sizeof(f64));
}

// The NAV summary's construction.two_speed prints both skip counts (mechanics only).
TEST(TwoSpeed, SummaryPrintsTheSleeveSkipsBesideTheParents) {
  std::vector<st::ConstructionDay> days(5);
  days[1].two_speed_sleeve_skipped = true;
  days[3].two_speed_sleeve_skipped = true;
  days[3].two_speed_parent_skipped = true;
  days[4].two_speed_parent_skipped = true;
  days[0].two_speed_parent_skipped = true;
  days[2].two_speed_parent_failed = true; // review YCOMB #6: a diagnostic error is a count
  const auto s = Json::parse(st::detail::construction_summary_json(two_speed_config(true), days));
  const auto& block = s.at("construction").at("two_speed");
  EXPECT_EQ(block.at("rebalances_skipped_by_a_sleeve").get<usize>(), 2U);
  EXPECT_EQ(block.at("parent_rebalances_skipped").get<usize>(), 3U);
  EXPECT_EQ(block.at("parent_constructions_failed").get<usize>(), 1U);
}

// Review YCOMB #6: under a neutralization the parent diagnostic runs beside the sleeves on every
// decision and records (never raises); on the fixture it never fails, and its skip count is the
// parent's own run's skip count.
TEST(TwoSpeed, ParentDiagnosticRecordsAndMatchesTheParentsSkips) {
  const Role role(70, 12, 53);
  const Sleeves sleeves(role, 0.3);
  auto x = role.target();
  sleeves.attach(x);
  const auto on = neutral_config(two_speed_config(true));
  const auto parent = st::replay_targets(role.target(), neutral_config(two_speed_config(false)));
  const auto two = st::replay_targets(x, on);
  ASSERT_TRUE(parent) << parent.error().to_string();
  ASSERT_TRUE(two) << two.error().to_string();
  ASSERT_EQ(parent->days.size(), two->days.size());
  usize parent_skips = 0, diagnosed = 0;
  for (usize t = 0; t < two->days.size(); ++t) {
    const auto& p = parent->days[t].construction;
    const auto& c = two->days[t].construction;
    EXPECT_FALSE(c.two_speed_parent_failed) << t;
    const bool skipped = p.neutralize != st::NeutralizeOutcome::NotAttempted &&
                         p.neutralize != st::NeutralizeOutcome::Applied;
    EXPECT_EQ(c.two_speed_parent_skipped, skipped) << t;
    parent_skips += skipped ? 1U : 0U;
    diagnosed += c.two_speed_parent_skipped ? 1U : 0U;
  }
  EXPECT_GT(parent_skips, 0U); // the windows fill after the first sessions: early skips
  EXPECT_EQ(diagnosed, parent_skips);
}
} // namespace
