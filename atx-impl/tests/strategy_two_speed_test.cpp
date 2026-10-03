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
#include <bit>
#include <cmath>
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
st::TargetReplayConfig two_speed_config(bool on) {
  auto c = nav_config().target;
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
// theta) / L.
TEST(TwoSpeed, DesiredIsTheNettedSleeveAim) {
  const Role role(30, 40, 29);
  const Sleeves sleeves(role, 0.3);
  auto x = role.target();
  sleeves.attach(x);
  auto fast_x = x, slow_x = x;
  fast_x.signal = sleeves.fast; slow_x.signal = sleeves.slow;
  const auto on = two_speed_config(true), off = two_speed_config(false);
  const f64 L = on.aim_leverage, theta = on.trade_fraction, theta_f = atx::engine::book::two_speed_fast_theta();
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

// A zero fast share and the blend as the slow sleeve: F stays 0 and every NAV day is the parent's.
TEST(TwoSpeed, ZeroFastShareIsTheParentRunBitForBit) {
  const Role role(40, 12, 53);
  const Sleeves sleeves(role, 0.0);
  auto in = role.nav();
  sleeves.attach(in.target);
  auto cfg = nav_config();
  const auto parent = st::replay_nav(role.nav(), cfg);
  cfg.target.two_speed = true;
  const auto two = st::replay_nav(in, cfg);
  ASSERT_TRUE(parent) << parent.error().to_string();
  ASSERT_TRUE(two) << two.error().to_string();
  ASSERT_EQ(parent->days.size(), two->days.size());
  for (usize t = 0; t < parent->days.size(); ++t) {
    EXPECT_EQ(bits(two->days[t].net_return), bits(parent->days[t].net_return)) << t;
    EXPECT_EQ(bits(two->days[t].turnover), bits(parent->days[t].turnover)) << t;
  }
}

// A real fast sleeve moves the book: the run completes and differs from the parent's.
TEST(TwoSpeed, NavReplayTradesTheNettedSleeves) {
  const Role role(40, 12, 53);
  const Sleeves sleeves(role, 0.4);
  auto in = role.nav();
  sleeves.attach(in.target);
  auto cfg = nav_config();
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
}

TEST(TwoSpeed, RuleIdRecipeAndSummaryCarryTheRuleOnlyWhenOn) {
  const auto on = two_speed_config(true), off = two_speed_config(false);
  EXPECT_NE(st::detail::construction_rule_id(on).find("+two-speed-v1"), std::string::npos);
  EXPECT_EQ(st::detail::construction_rule_id(off).find("two-speed"), std::string::npos);
  auto recipe_on = Json::parse(st::detail::construction_recipe_json(on));
  const auto recipe_off = Json::parse(st::detail::construction_recipe_json(off));
  EXPECT_EQ(recipe_on.at("two_speed"), "two-speed-v1");
  EXPECT_EQ(recipe_on.at("two_speed_theta_fast").get<f64>(), atx::engine::book::two_speed_fast_theta());
  recipe_on.erase("two_speed"); recipe_on.erase("two_speed_theta_fast"); recipe_on.erase("two_speed_rule");
  EXPECT_EQ(recipe_on, recipe_off);
  const std::vector<st::ConstructionDay> days(3);
  const auto summary_on = Json::parse(st::detail::construction_summary_json(on, days));
  const auto summary_off = Json::parse(st::detail::construction_summary_json(off, days));
  EXPECT_EQ(summary_on.at("construction").at("two_speed").at("theta_slow").get<f64>(), on.trade_fraction);
  EXPECT_FALSE(summary_off.at("construction").contains("two_speed"));
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
  auto cfg = nav_config();
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

// A fast share of .3 under vol-target-v1. (1) The hook plans the netted aim at the scaler's L_t:
// update_weights with --aim-leverage L_t on the same desired target, bit for bit. (2) Closed form at
// L_t = 1 against the run's L 1.2 (no dust, immediate exit): the book moves toward (1 / L) x the
// netted target T = L m_s d_s + F + (F_next - F) / theta, and F follows the run's L whatever L_t is.
TEST(TwoSpeed, UnderVolTargetTheScalerScalesTheNettedTarget) {
  const Directory dir;
  const Role role(50, 12, 41);
  const auto risk = clean_store(dir, role, 5);
  ASSERT_NE(risk, nullptr);
  auto cfg = nav_config();
  cfg.target.two_speed = true;
  const Sleeves sleeves(role, 0.3);
  auto x = role.target();
  sleeves.attach(x);
  auto fast_x = x, slow_x = x;
  fast_x.signal = sleeves.fast;
  slow_x.signal = sleeves.slow;
  auto plain = cfg.target;
  plain.two_speed = false;
  auto fixed = cfg.target;
  fixed.aim_leverage = 1.0; fixed.dust_multiple = 0.0; fixed.exit_rate = 1.0;
  const f64 L = cfg.target.aim_leverage, theta = cfg.target.trade_fraction;
  const f64 theta_f = atx::engine::book::two_speed_fast_theta();
  v7::NavV7Options o;
  o.risk_target = vol_options();
  o.spo_risk = risk;
  const v7::ScopedNavExtension extension(o);
  st::detail::DesiredState state;
  std::vector<Ranked> row;
  st::PriceRiskScratch scratch;
  std::vector<f64> current(role.n, 0.0), closed(role.n, 0.0), F(role.n, 0.0);
  for (usize d = 0; d < 45; ++d) {
    std::vector<f64> desired(role.n, 0.0), df(role.n, 0.0), ds(role.n, 0.0);
    st::ConstructionDay rec, rf, rs;
    ASSERT_TRUE(st::detail::form_desired(x, cfg.target, d, row, desired, scratch, rec, {}, &state));
    ASSERT_TRUE(st::detail::form_desired(fast_x, plain, d, row, df, scratch, rf));
    ASSERT_TRUE(st::detail::form_desired(slow_x, plain, d, row, ds, scratch, rs));
    std::vector<f64> hooked = current, at_lt = current;
    st::TargetReplayDay a, b;
    ASSERT_TRUE(v7::plan(x, cfg, d, true, 0.0, 1e8, desired, hooked, a, {}));
    auto t = cfg.target;
    t.aim_leverage = extension.risk_target_scaler()->records().back().leverage;
    ASSERT_TRUE(st::detail::update_weights(x, t, d, true, 0.0, desired, at_lt, b));
    for (usize i = 0; i < role.n; ++i) EXPECT_EQ(bits(hooked[i]), bits(at_lt[i])) << d << ' ' << i;
    current = hooked;
    std::vector<f64> step = closed;
    st::TargetReplayDay c;
    ASSERT_TRUE(st::detail::update_weights(x, fixed, d, true, 0.0, desired, step, c));
    for (usize i = 0; i < role.n; ++i) {
      if (!role.member[d * role.n + i]) {
        F[i] = 0.0;
        EXPECT_EQ(step[i], 0.0) << d << ' ' << i;
        continue;
      }
      const f64 before = F[i];
      F[i] = before + theta_f * (L * 0.3 * df[i] - before);
      const f64 netted = L * (1.0 - 0.3) * ds[i] + before + (F[i] - before) / theta;
      EXPECT_NEAR(step[i], closed[i] + theta * ((1.0 / L) * netted - closed[i]), 1e-14) << d << ' ' << i;
      EXPECT_NEAR(state.fast[i], F[i], 1e-15) << d << ' ' << i;
    }
    closed = step;
  }
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

// The NAV summary's construction.two_speed prints both skip counts (mechanics only).
TEST(TwoSpeed, SummaryPrintsTheSleeveSkipsBesideTheParents) {
  std::vector<st::ConstructionDay> days(5);
  days[1].two_speed_sleeve_skipped = true;
  days[3].two_speed_sleeve_skipped = true;
  days[3].two_speed_parent_skipped = true;
  days[4].two_speed_parent_skipped = true;
  days[0].two_speed_parent_skipped = true;
  const auto s = Json::parse(st::detail::construction_summary_json(two_speed_config(true), days));
  const auto& block = s.at("construction").at("two_speed");
  EXPECT_EQ(block.at("rebalances_skipped_by_a_sleeve").get<usize>(), 2U);
  EXPECT_EQ(block.at("parent_rebalances_skipped").get<usize>(), 3U);
}
} // namespace
