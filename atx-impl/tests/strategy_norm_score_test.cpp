// norm-score-v1 (platform v8 Y, lane YCOMB, concentration): the construction option of the
// target and NAV replays (TargetReplayConfig::norm_score, nav --rank-shape norm-score-v1). The
// desired target of a decision equals an independent recomputation from the definition (the
// members sorted by the blend, the van der Waerden score of each tie block, demeaned and scaled to
// gross 1) and puts more gross on the tails than the tied rank; flag absent, the construction is
// the tied rank's bit for bit and no key is written; the rule is refused outside aim-partial-v5
// and together with the hold band or inv-vol-v1; the rule id, recipe and summary carry it; the NAV
// replay runs it on every book.
//
// Suite: NormScore

#include <algorithm>
#include <bit>
#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/engine/eval/stats_ext.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"
#include "strategy_spo_fixture.hpp"

namespace {
using namespace atx;
using namespace atx::impl::strategy::spo::fixture;
namespace st = atx::impl::strategy;
namespace v7 = atx::impl::strategy::v7;
using Json = nlohmann::json;
using Ranked = std::pair<f64, usize>;

u64 bit_pattern(f64 x) { return std::bit_cast<u64>(x); }

st::TargetReplayConfig shaped_config(bool on) {
  st::TargetReplayConfig c;
  c.rule = st::TargetReplayRule::AimPartialV5;
  c.cadence = 1;
  c.trade_fraction = 0.25;
  c.dust_multiple = 0.1;
  c.aim_leverage = 1.2;
  c.norm_score = on;
  return c;
}

// The definition, recomputed: members sorted by (signal, index), z = Phi^{-1}((b + e + 1) /
// (2 (N + 1))) per tie block, demeaned over the members in sorted order, gross 1.
std::vector<f64> expected_desired(const Role& role, usize d) {
  const usize n = role.n;
  std::vector<Ranked> row;
  for (usize i = 0; i < n; ++i)
    if (role.member[d * n + i]) row.emplace_back(role.signal[d * n + i], i);
  std::sort(row.begin(), row.end());
  std::vector<f64> z(n, 0.0);
  const f64 m = static_cast<f64>(row.size());
  for (usize b = 0; b < row.size();) {
    usize e = b + 1;
    while (e < row.size() && row[e].first == row[b].first) ++e;
    const f64 score = atx::engine::eval::norm_ppf(
        (static_cast<f64>(b) + static_cast<f64>(e) + 1.0) / (2.0 * (m + 1.0)));
    for (usize k = b; k < e; ++k) z[row[k].second] = score;
    b = e;
  }
  f64 sum = 0;
  for (const auto& r : row) sum += z[r.second];
  const f64 mean = sum / m;
  f64 gross = 0;
  for (const auto& r : row) { z[r.second] -= mean; gross += std::abs(z[r.second]); }
  for (const auto& r : row) z[r.second] /= gross;
  return z;
}

TEST(NormScore, DesiredIsTheDemeanedGrossOneNormalScore) {
  const Role role(30, 40, 29);
  const auto x = role.target();
  for (const usize d : {0U, 7U, 12U, 29U}) { // 12: the last name is out of membership
    std::vector<Ranked> row;
    std::vector<f64> shaped(role.n, 0.0), plain(role.n, 0.0);
    st::PriceRiskScratch scratch;
    st::ConstructionDay on, off;
    const auto a = st::detail::form_desired(x, shaped_config(true), d, row, shaped, scratch, on);
    const auto b = st::detail::form_desired(x, shaped_config(false), d, row, plain, scratch, off);
    ASSERT_TRUE(a && *a) << d;
    ASSERT_TRUE(b && *b) << d;
    const auto want = expected_desired(role, d);
    f64 gross = 0, top_shaped = 0, top_plain = 0;
    usize members = 0;
    for (usize i = 0; i < role.n; ++i) {
      EXPECT_NEAR(shaped[i], want[i], 1e-15) << d << ' ' << i;
      gross += std::abs(shaped[i]);
      top_shaped = std::max(top_shaped, std::abs(shaped[i]));
      top_plain = std::max(top_plain, std::abs(plain[i]));
      members += role.member[d * role.n + i] ? 1U : 0U;
    }
    EXPECT_NEAR(gross, 1.0, 1e-14) << d;
    EXPECT_GT(top_shaped, top_plain) << d; // the tails carry more of the gross
    EXPECT_EQ(on.norm_scored, members) << d;
    EXPECT_GT(on.norm_max_abs, 1.0) << d;
    EXPECT_EQ(off.norm_scored, 0U) << d; // flag absent: the kernel never ran
    // Flag absent: the tied rank's target, bit for bit.
    std::vector<f64> reference(role.n, 0.0);
    st::detail::desired_target(x.signal.subspan(d * role.n, role.n),
                               x.member.subspan(d * role.n, role.n), row, reference);
    for (usize i = 0; i < role.n; ++i)
      EXPECT_EQ(bit_pattern(plain[i]), bit_pattern(reference[i])) << d << ' ' << i;
  }
}

TEST(NormScore, RefusedOutsideAimPartialAndWithTheOtherRankShapes) {
  const Role role(20, 12, 31);
  const auto x = role.target();
  ASSERT_TRUE(st::replay_targets(x, shaped_config(true))); // control
  auto baseline = shaped_config(true);
  baseline.rule = st::TargetReplayRule::BaselineTargetV1;
  baseline.aim_leverage = 1.0;
  baseline.dust_multiple = 0.0;
  auto banded = shaped_config(true);
  banded.hold_band = 0.1;
  auto scaled = shaped_config(true);
  scaled.inv_vol = true;
  for (const auto* cfg : {&baseline, &banded, &scaled}) {
    const auto r = st::replay_targets(x, *cfg);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().to_string().find("norm-score-v1"), std::string::npos);
  }
  auto plain_baseline = baseline;
  plain_baseline.norm_score = false;
  EXPECT_TRUE(st::replay_targets(x, plain_baseline)); // its control
}

// Review YCOMB #11 (Ruling PM8-16 #11): the nav parser rewrites --rule aim-partial-v6 to v5 for the
// replay, so --rank-shape would have run under v6's plan; it is refused, and kept with v5.
TEST(NormScore, NavParseRefusesTheRankShapeWithAimPartialV6) {
  const auto parse = [](std::vector<std::string> args) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    return v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data());
  };
  const auto v6 = parse({"nav", "--rule", "aim-partial-v6", "--output", "x", "--rank-shape",
                         "norm-score-v1"});
  ASSERT_FALSE(v6);
  EXPECT_NE(v6.error().to_string().find("--rank-shape norm-score-v1 needs aim-partial-v5"),
            std::string::npos)
      << v6.error().to_string();
  const auto v6_plain = parse({"nav", "--rule", "aim-partial-v6", "--output", "x"});
  EXPECT_TRUE(v6_plain) << v6_plain.error().to_string(); // its control
}

TEST(NormScore, RuleIdRecipeAndSummaryCarryTheRuleOnlyWhenOn) {
  const Role role(30, 12, 37);
  const auto x = role.target();
  const auto on = shaped_config(true), off = shaped_config(false);
  EXPECT_EQ(st::detail::construction_rule_id(on), "aim-partial-v5+norm-score-v1");
  EXPECT_EQ(st::detail::construction_rule_id(off), "aim-partial-v5");
  const auto recipe_on = Json::parse(st::detail::construction_recipe_json(on));
  const auto recipe_off = Json::parse(st::detail::construction_recipe_json(off));
  EXPECT_EQ(recipe_on.at("rank_shape"), "norm-score-v1");
  EXPECT_TRUE(recipe_on.contains("rank_shape_rule"));
  EXPECT_FALSE(recipe_off.contains("rank_shape"));
  EXPECT_FALSE(recipe_off.contains("rank_shape_rule"));
  auto rest_on = recipe_on;
  rest_on.erase("rank_shape");
  rest_on.erase("rank_shape_rule");
  EXPECT_EQ(rest_on, recipe_off); // nothing else moves
  const auto replay_on = st::replay_targets(x, on);
  const auto replay_off = st::replay_targets(x, off);
  ASSERT_TRUE(replay_on && replay_off);
  std::vector<st::ConstructionDay> days_on, days_off;
  for (const auto& d : replay_on->days) days_on.push_back(d.construction);
  for (const auto& d : replay_off->days) days_off.push_back(d.construction);
  const auto summary_on = Json::parse(st::detail::construction_summary_json(on, days_on));
  const auto summary_off = Json::parse(st::detail::construction_summary_json(off, days_off));
  const auto& shape = summary_on.at("construction").at("rank_shape");
  EXPECT_EQ(shape.at("id"), "norm-score-v1");
  EXPECT_GT(shape.at("scored_decisions").get<usize>(), 0U);
  EXPECT_LE(shape.at("scored_decisions").get<usize>(), replay_on->days.size());
  EXPECT_GT(shape.at("max_abs_score").get<f64>(), 1.0);
  EXPECT_FALSE(summary_off.at("construction").contains("rank_shape"));
  // The shape reaches the plan: some day's planned weights differ.
  bool moved = false;
  for (usize t = 0; t < replay_on->days.size(); ++t)
    moved = moved || bit_pattern(replay_on->days[t].max_abs_weight) !=
                         bit_pattern(replay_off->days[t].max_abs_weight);
  EXPECT_TRUE(moved);
}

// The NAV replay forms one shaped target for every book: the run completes and its first
// rebalance holds the shaped target's largest weight (L x the tail).
TEST(NormScore, NavReplayRunsTheShapedTargetOnEveryBook) {
  const Role role(40, 12, 53);
  auto cfg = nav_config();
  cfg.target.norm_score = true;
  const auto shaped = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(shaped) << shaped.error().to_string();
  auto plain_cfg = nav_config();
  const auto plain = st::replay_nav(role.nav(), plain_cfg);
  ASSERT_TRUE(plain) << plain.error().to_string();
  ASSERT_EQ(shaped->days.size(), plain->days.size());
  bool differs = false;
  for (usize t = 0; t < shaped->days.size(); ++t)
    differs = differs ||
              bit_pattern(shaped->days[t].net_return) != bit_pattern(plain->days[t].net_return);
  EXPECT_TRUE(differs);
}
} // namespace
