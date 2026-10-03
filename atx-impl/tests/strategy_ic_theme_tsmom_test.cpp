// theme-tsmom-v1 (platform v8 Y, lane YCOMB, rule Y-2): the mass kernel on its closed form and on the
// fixture the Python fitter's kernel writes (atx-impl/tests/fixtures/theme_tsmom_v1.json, bit for bit),
// and the composition's theme-mass schedule: blocks repeating W_theme are the unscheduled blend bit
// for bit; a block switching a theme off gives, from its first date, the blend of the other theme
// alone at the block's mass and leaves the dates before it untouched; refusals keep nothing.
//
// Suite: ThemeTsmom

#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "../src/strategy_ic_composition.hpp"
#include "../src/strategy_ic_theme_tsmom.hpp"

namespace {
using namespace atx;
namespace st = atx::impl::strategy;
using Json = nlohmann::json;

u64 bits(f64 x) { return std::bit_cast<u64>(x); }

TEST(ThemeTsmom, RegisteredConstants) {
  EXPECT_EQ(st::theme_tsmom_rule, "theme-tsmom-v1");
  EXPECT_EQ(st::theme_tsmom_lookback, 252U);
  EXPECT_EQ(st::theme_tsmom_lag, 3U);
  EXPECT_EQ(st::theme_tsmom_step, 21U);
}

TEST(ThemeTsmom, MassesClosedForm) {
  const std::vector<f64> parent{0.2, 0.3, 0.5};
  std::vector<f64> out(3U, -1.0);
  const auto off = st::theme_tsmom_masses(parent, std::vector<f64>{1.0, -1.0, 2.0}, out);
  ASSERT_TRUE(off) << off.error().to_string();
  EXPECT_EQ(*off, 1U);
  const f64 scale = (0.0 + 0.2 + 0.3 + 0.5) / (0.0 + 0.2 + 0.5); // S / K, summed in theme order
  EXPECT_EQ(bits(out[0]), bits(0.2 * scale));
  EXPECT_EQ(bits(out[1]), bits(0.0));
  EXPECT_EQ(bits(out[2]), bits(0.5 * scale));
  EXPECT_NEAR(out[0] + out[1] + out[2], 1.0, 1e-15);
  // Every theme won, or none did (zero is not a win): the parent's masses verbatim.
  for (const auto& trailing : {std::vector<f64>{0.1, 0.2, 0.3}, std::vector<f64>{-0.1, 0.0, -2.0}}) {
    std::vector<f64> same(3U, -1.0);
    const auto none = st::theme_tsmom_masses(parent, trailing, same);
    ASSERT_TRUE(none);
    EXPECT_EQ(*none, 0U);
    for (usize t = 0; t < 3U; ++t) EXPECT_EQ(bits(same[t]), bits(parent[t]));
  }
  // Wrong rules told apart: the freed mass split equally, or not handed on at all.
  std::vector<f64> m(3U);
  ASSERT_TRUE(st::theme_tsmom_masses(std::vector<f64>{0.1, 0.3, 0.6}, std::vector<f64>{-1.0, 1.0, 1.0}, m));
  EXPECT_GT(std::abs(m[1] - 0.35), 1e-3);
  EXPECT_GT(std::abs(m[1] - 0.3), 1e-3);
}

TEST(ThemeTsmom, MassesRefuseBeforeAnyWrite) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const std::vector<std::pair<std::vector<f64>, std::vector<f64>>> cases{
      {{}, {}}, {{0.5}, {0.1, 0.2}}, {{0.0, 1.0}, {1.0, 1.0}}, {{nan}, {1.0}}, {{1.0, 1.0}, {1.0, nan}}};
  for (const auto& [parent, trailing] : cases) {
    std::vector<f64> out(parent.size(), -7.0);
    const auto r = st::theme_tsmom_masses(parent, trailing, out);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().to_string().find("theme-tsmom-v1"), std::string::npos);
    for (const f64 v : out) EXPECT_EQ(v, -7.0);
  }
  std::vector<f64> short_out(1U);
  EXPECT_FALSE(st::theme_tsmom_masses(std::vector<f64>{1.0, 1.0}, std::vector<f64>{1.0, 1.0}, short_out));
}

// The fixture the Python kernel wrote (composition_theme_tsmom.masses): every mass bit for bit.
TEST(ThemeTsmom, SharedFixtureMasses) {
  std::ifstream in(std::filesystem::path(ATX_IMPL_TESTS_DIR) / "fixtures" / "theme_tsmom_v1.json");
  ASSERT_TRUE(in);
  const auto fx = Json::parse(in);
  ASSERT_EQ(fx.at("rule"), "theme-tsmom-v1");
  ASSERT_GE(fx.at("cases").size(), 5U);
  for (const auto& c : fx.at("cases")) {
    const auto parent = c.at("parent").get<std::vector<f64>>();
    const auto trailing = c.at("trailing").get<std::vector<f64>>();
    const auto want = c.at("masses").get<std::vector<f64>>();
    std::vector<f64> out(parent.size());
    const auto off = st::theme_tsmom_masses(parent, trailing, out);
    ASSERT_TRUE(off) << c.dump();
    EXPECT_EQ(*off, c.at("off").get<usize>()) << c.dump();
    for (usize t = 0; t < out.size(); ++t) EXPECT_EQ(bits(out[t]), bits(want[t])) << c.dump() << ' ' << t;
  }
}

// Two themes over nine names and four dates: a (one member) and b (three members whose ranks
// disagree), standardised (ew-theme-std-v1's per-date re-rank).
struct Fixture {
  static constexpr usize days = 4, width = 9;
  std::vector<st::IcCompositionCandidate> candidates{{"a1", "a"}, {"b1", "b"}, {"b2", "b"}, {"b3", "b"}};
  std::vector<usize> themes{0, 1, 1, 1};
  std::vector<int> signs{1, 1, 1, 1};
  std::vector<u8> member = std::vector<u8>(days * width, 1);
  std::vector<std::vector<f64>> signals = std::vector<std::vector<f64>>(4, std::vector<f64>(days * width));
  st::IcCompositionConfig cfg;
  Fixture() {
    cfg.dates = days; cfg.instruments = width; cfg.decision_end = days;
    for (usize d = 0; d < days; ++d) for (usize i = 0; i < width; ++i) {
      const auto at = d * width + i, j = (i + d) % width;
      signals[0][at] = static_cast<f64>((5 * j) % width);
      signals[1][at] = static_cast<f64>(j);
      signals[2][at] = static_cast<f64>((j + 8) % width);
      signals[3][at] = static_cast<f64>((8 * j + 1) % width);
    }
    member[2 * width + 4] = 0; // name 4 leaves on date 2
  }
  std::optional<st::IcCompositionResult> compose(const std::vector<f64>& weights,
                                                 std::span<const st::IcThemeBlock> schedule = {},
                                                 st::IcThemeRule rule = st::IcThemeRule::standardise) const {
    auto c = st::IcComposition::create(cfg, candidates, member, weights, themes, rule);
    if (!c) return std::nullopt;
    if (!schedule.empty() && !c->schedule_theme_masses(schedule)) return std::nullopt;
    for (usize k = 0; k < candidates.size(); ++k)
      if (!c->add(k, signals[k], signs[k])) return std::nullopt;
    auto out = c->finish();
    if (!out) return std::nullopt;
    return std::move(*out);
  }
};
const std::vector<f64> both{.5, 1.0 / 6, 1.0 / 6, 1.0 / 6};
const f64 w_a = .5, w_b = 0.0 + 1.0 / 6 + 1.0 / 6 + 1.0 / 6; // W_theme as the composition sums it

TEST(ThemeTsmom, ScheduleRepeatingTheParentMassesIsTheBlendBitForBit) {
  const Fixture f;
  const auto plain = f.compose(both);
  const std::vector<st::IcThemeBlock> same{{1, {w_a, w_b}}, {1, {w_a, w_b}}, {3, {w_a, w_b}}, {4, {w_a, w_b}}};
  const auto scheduled = f.compose(both, same);
  ASSERT_TRUE(plain); ASSERT_TRUE(scheduled);
  ASSERT_EQ(plain->signal.size(), scheduled->signal.size());
  for (usize k = 0; k < plain->signal.size(); ++k) EXPECT_EQ(bits(scheduled->signal[k]), bits(plain->signal[k])) << k;
  for (usize d = 0; d < Fixture::days; ++d) {
    EXPECT_EQ(bits(scheduled->planned_turnover[d]), bits(plain->planned_turnover[d])) << d;
    EXPECT_EQ(bits(scheduled->planned_gross[d]), bits(plain->planned_gross[d])) << d;
  }
}

// Theme a off from date 2 (mass 0), theme b at mass w_b there: dates 0-1 are the two-theme blend,
// dates 2-3 the blend of b alone (weights with a1 at zero), each bit for bit; a block from date 3
// restoring the parent's masses brings date 3 back.
TEST(ThemeTsmom, ABlockSwitchesAThemeOffFromItsFirstDateOnly) {
  const Fixture f;
  const auto two = f.compose(both);
  const auto b_only = f.compose(std::vector<f64>{0.0, 1.0 / 6, 1.0 / 6, 1.0 / 6});
  const std::vector<st::IcThemeBlock> off{{2, {0.0, w_b}}};
  const std::vector<st::IcThemeBlock> back{{2, {0.0, w_b}}, {3, {w_a, w_b}}};
  const auto switched = f.compose(both, off), restored = f.compose(both, back);
  ASSERT_TRUE(two); ASSERT_TRUE(b_only); ASSERT_TRUE(switched); ASSERT_TRUE(restored);
  bool moved = false;
  for (usize d = 0; d < Fixture::days; ++d)
    for (usize i = 0; i < Fixture::width; ++i) {
      const usize k = d * Fixture::width + i;
      const auto& want = d < 2 ? two->signal : b_only->signal;
      EXPECT_EQ(bits(switched->signal[k]), bits(want[k])) << d << ' ' << i;
      const auto& again = (d < 2 || d == 3) ? two->signal : b_only->signal;
      EXPECT_EQ(bits(restored->signal[k]), bits(again[k])) << d << ' ' << i;
      moved = moved || (d >= 2 && bits(two->signal[k]) != bits(b_only->signal[k]));
    }
  EXPECT_TRUE(moved); // the fixture tells the two blends apart
  EXPECT_TRUE(std::isnan(switched->signal[2 * Fixture::width + 4])); // nonmember stays NaN
}

TEST(ThemeTsmom, ScheduleRefusalsKeepNothing) {
  const Fixture f;
  auto c = st::IcComposition::create(f.cfg, f.candidates, f.member, both, f.themes, st::IcThemeRule::standardise);
  ASSERT_TRUE(c);
  const std::vector<std::vector<st::IcThemeBlock>> bad{
      {{2, {w_a}}},                       // one mass for two themes
      {{2, {w_a, -1.0}}},                 // negative
      {{2, {w_a, std::numeric_limits<f64>::infinity()}}},
      {{3, {w_a, w_b}}, {2, {w_a, w_b}}}, // decreasing begin
      {{5, {w_a, w_b}}}};                 // after the last date + 1
  for (const auto& blocks : bad) {
    const auto r = c->schedule_theme_masses(blocks);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().to_string().find("theme schedule"), std::string::npos);
  }
  // Nothing was kept: the composition finishes as the unscheduled blend.
  for (usize k = 0; k < f.candidates.size(); ++k) ASSERT_TRUE(c->add(k, f.signals[k], f.signs[k]));
  const auto out = c->finish();
  const auto plain = f.compose(both);
  ASSERT_TRUE(out); ASSERT_TRUE(plain);
  for (usize k = 0; k < plain->signal.size(); ++k) EXPECT_EQ(bits(out->signal[k]), bits(plain->signal[k])) << k;
  const std::vector<st::IcThemeBlock> good{{1, {w_a, w_b}}};
  EXPECT_FALSE(c->schedule_theme_masses(good)); // after finish
  // Other rules: redistribution, residualisation, and no themes.
  for (const auto rule : {st::IcThemeRule::redistribute, st::IcThemeRule::residualise}) {
    auto other = st::IcComposition::create(f.cfg, f.candidates, f.member, both, f.themes, rule);
    ASSERT_TRUE(other);
    EXPECT_FALSE(other->schedule_theme_masses(good));
  }
  auto plain_path = st::IcComposition::create(f.cfg, f.candidates, f.member, both, {}, st::IcThemeRule::standardise);
  ASSERT_TRUE(plain_path);
  EXPECT_FALSE(plain_path->schedule_theme_masses(good));
}
} // namespace
