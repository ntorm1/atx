// strategy_ic_shrink_test.cpp -- composition rule ic-shrink-v1 (platform v8 R-10).
//
// Suite: IcShrinkV1. The runner side (the theme_standardise rule table, its verification and
// the unchanged standardisation) is CompositionV8.IcShrink* in strategy_ic_runner_test.cpp.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/engine/combine/group_shrink.hpp"
#include "strategy_ic_shrink.hpp"

namespace {
using namespace atx;
using Json = nlohmann::json;
namespace st = atx::impl::strategy;
namespace cb = atx::engine::combine;

// The fixture shared with the fitter's test (atx-impl/tools/test_composition_ic_shrink.py).
struct Member {
  std::string id, theme;
  f64 ic{}, shrunk{}, share{}, weight{};
};
struct Fixture {
  std::vector<Member> members;
  std::vector<std::string> themes; // theme order (first appearance)
  std::vector<usize> theme;        // per member
  f64 cap{};
  usize passes{};
  std::vector<std::string> equal, capped;
};
f64 fraction(const Json& pair) { return pair.at(0).get<f64>() / pair.at(1).get<f64>(); }
bool load(Fixture& fx) {
  std::ifstream in(std::filesystem::path(ATX_IMPL_TESTS_DIR) / "fixtures" / "ic_shrink_v1.json");
  if (!in) return false;
  const auto j = Json::parse(in);
  fx.themes = j.at("theme_order").get<std::vector<std::string>>();
  for (const auto& row : j.at("members")) {
    Member m{row.at("id").get<std::string>(), row.at("theme").get<std::string>(), row.at("ic").get<f64>(),
             fraction(row.at("shrunk")), fraction(row.at("share")), fraction(row.at("weight"))};
    const auto at = std::find(fx.themes.begin(), fx.themes.end(), m.theme);
    if (at == fx.themes.end()) return false;
    fx.theme.push_back(static_cast<usize>(at - fx.themes.begin()));
    fx.members.push_back(std::move(m));
  }
  fx.cap = fraction(j.at("cap"));
  fx.passes = j.at("cap_passes").get<usize>();
  fx.equal = j.at("equal_share_themes").get<std::vector<std::string>>();
  fx.capped = j.at("capped").get<std::vector<std::string>>();
  return j.at("intensity").get<f64>() == st::ic_shrink_intensity && j.at("floor").get<f64>() == st::ic_shrink_floor;
}
std::vector<f64> ics(const Fixture& fx) {
  std::vector<f64> out;
  for (const auto& m : fx.members) out.push_back(m.ic);
  return out;
}
// share / T, then the engine cap at 1 / (2T): the rule's last step for any within-theme shares.
std::vector<f64> capped_weights(const std::vector<f64>& share, const std::vector<usize>& theme, usize themes) {
  std::vector<f64> w;
  for (const f64 s : share) w.push_back(s / static_cast<f64>(themes));
  const auto passes = cb::cap_across_groups(w, theme, themes, 1.0 / (2.0 * static_cast<f64>(themes)), 1e-12);
  return passes ? w : std::vector<f64>{};
}
// Largest |a - b|; -1 when the sizes differ (a failed alternative never counts as "apart").
f64 max_gap(const std::vector<f64>& a, const std::vector<f64>& b) {
  if (a.size() != b.size()) return -1.0;
  f64 gap = 0;
  for (usize k = 0; k < a.size(); ++k) gap = std::max(gap, std::abs(a[k] - b[k]));
  return gap;
}

TEST(IcShrinkV1, RegisteredConstants) {
  EXPECT_EQ(st::ic_shrink_rule, "ic-shrink-v1");
  EXPECT_EQ(st::ic_shrink_intensity, 0.5);
  EXPECT_EQ(st::ic_shrink_floor, 0.0);
  EXPECT_EQ(st::ic_shrink_cap_tolerance, 1e-12);
  EXPECT_EQ(st::ic_shrink_weight_tolerance, 1e-12);
}

// The written rule, computed here step by step: theme mean, shrunk = .5 mean + .5 ic, floor 0,
// share = floored / theme sum (equal 1 / n with no positive value), w = share / T; then the cap
// 1 / (2T): the fixture has one member above it, whose excess goes to the members of the other
// themes in proportion to their weights.
TEST(IcShrinkV1, WeightsFollowTheWrittenRule) {
  Fixture fx;
  ASSERT_TRUE(load(fx));
  const usize n = fx.members.size(), themes = fx.themes.size();
  std::vector<f64> total(themes, 0.0), mass(themes, 0.0), count(themes, 0.0), shrunk(n), share(n), w(n);
  for (usize k = 0; k < n; ++k) { total[fx.theme[k]] += fx.members[k].ic; count[fx.theme[k]] += 1.0; }
  for (usize k = 0; k < n; ++k) {
    shrunk[k] = 0.5 * (total[fx.theme[k]] / count[fx.theme[k]]) + 0.5 * fx.members[k].ic;
    mass[fx.theme[k]] += std::max(shrunk[k], 0.0);
  }
  for (usize k = 0; k < n; ++k) {
    const usize t = fx.theme[k];
    share[k] = mass[t] > 0 ? std::max(shrunk[k], 0.0) / mass[t] : 1.0 / count[t];
    w[k] = share[k] / static_cast<f64>(themes);
  }
  const f64 cap = 1.0 / (2.0 * static_cast<f64>(themes));
  usize over = n;
  for (usize k = 0; k < n; ++k)
    if (w[k] > cap) { ASSERT_EQ(over, n) << "the fixture caps one member"; over = k; }
  ASSERT_LT(over, n);
  f64 others = 0;
  for (usize k = 0; k < n; ++k)
    if (fx.theme[k] != fx.theme[over]) others += w[k];
  const f64 excess = w[over] - cap;
  for (usize k = 0; k < n; ++k)
    if (fx.theme[k] != fx.theme[over]) w[k] += excess * w[k] / others;
  w[over] = cap;

  const auto fit = st::ic_shrink_weights(ics(fx), fx.theme, themes);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->shrunk.size(), n);
  ASSERT_EQ(fit->share.size(), n);
  ASSERT_EQ(fit->weights.size(), n);
  EXPECT_LE(max_gap(fit->shrunk, shrunk), 1e-15);
  EXPECT_LE(max_gap(fit->share, share), 1e-15);
  EXPECT_LE(max_gap(fit->weights, w), 1e-15);
  EXPECT_EQ(fit->cap, cap);
  EXPECT_EQ(fit->cap_passes, 1U);
  f64 sum = 0;
  for (const f64 x : fit->weights) {
    sum += x;
    EXPECT_GE(x, 0.0);
    EXPECT_LE(x, cap * (1.0 + 1e-12));
  }
  EXPECT_NEAR(sum, 1.0, 1e-15);
}

// The exact fractions of the shared fixture (the fitter's test asserts the same ones), so the
// Python rule and this one agree on it to 1e-15.
TEST(IcShrinkV1, SharedFixtureFractions) {
  Fixture fx;
  ASSERT_TRUE(load(fx));
  const auto fit = st::ic_shrink_weights(ics(fx), fx.theme, fx.themes.size());
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->weights.size(), fx.members.size());
  ASSERT_EQ(fit->equal_theme.size(), fx.themes.size());
  for (usize k = 0; k < fx.members.size(); ++k) {
    SCOPED_TRACE(fx.members[k].id);
    EXPECT_NEAR(fit->shrunk[k], fx.members[k].shrunk, 1e-15);
    EXPECT_NEAR(fit->share[k], fx.members[k].share, 1e-15);
    EXPECT_NEAR(fit->weights[k], fx.members[k].weight, 1e-15);
    const bool capped = std::find(fx.capped.begin(), fx.capped.end(), fx.members[k].id) != fx.capped.end();
    EXPECT_EQ(fit->weights[k] == fx.cap, capped);
  }
  for (usize t = 0; t < fx.themes.size(); ++t) {
    const bool equal = std::find(fx.equal.begin(), fx.equal.end(), fx.themes[t]) != fx.equal.end();
    EXPECT_EQ(fit->equal_theme[t] != 0U, equal) << fx.themes[t];
  }
  EXPECT_EQ(fit->cap, fx.cap);
  EXPECT_EQ(fit->cap_passes, fx.passes);
}

// Review T-1: the fixture must be able to fail a wrong rule. Equal within-theme shares (the
// shrinkage target, ew-theme-v1's split), no shrinkage (intensity 0), full shrinkage (intensity
// 1), no floor, no equal-share fallback and no cap each move some weight by more than 1e-3.
TEST(IcShrinkV1, FixtureTellsWrongRulesApart) {
  Fixture fx;
  ASSERT_TRUE(load(fx));
  const auto ic = ics(fx);
  const usize n = ic.size(), themes = fx.themes.size();
  const auto fit = st::ic_shrink_weights(ic, fx.theme, themes);
  ASSERT_TRUE(fit);
  ASSERT_EQ(fit->weights.size(), n);
  ASSERT_EQ(fit->equal_theme.size(), themes);
  std::vector<f64> count(themes, 0.0);
  for (const usize t : fx.theme) count[t] += 1.0;
  std::vector<f64> equal(n);
  for (usize k = 0; k < n; ++k) equal[k] = 1.0 / count[fx.theme[k]];
  EXPECT_GT(max_gap(capped_weights(equal, fx.theme, themes), fit->weights), 1e-3);
  for (const f64 intensity : {0.0, 1.0}) {
    const auto shares = cb::group_shrink_shares(ic, fx.theme, themes, intensity, 0.0);
    ASSERT_TRUE(shares);
    EXPECT_GT(max_gap(capped_weights(shares->share, fx.theme, themes), fit->weights), 1e-3) << intensity;
  }
  // No floor: shares of the raw shrunk values (a negative weight for the floored member).
  std::vector<f64> sum(themes, 0.0), unfloored(n);
  for (usize k = 0; k < n; ++k) sum[fx.theme[k]] += fit->shrunk[k];
  for (usize k = 0; k < n; ++k) unfloored[k] = fit->shrunk[k] / sum[fx.theme[k]] / static_cast<f64>(themes);
  EXPECT_GT(max_gap(unfloored, fit->weights), 1e-3);
  // No fallback: a theme without a positive shrunk value would get nothing.
  std::vector<f64> dropped = fit->share;
  for (usize k = 0; k < n; ++k)
    if (fit->equal_theme[fx.theme[k]] != 0U) dropped[k] = 0.0;
  EXPECT_GT(max_gap(capped_weights(dropped, fx.theme, themes), fit->weights), 1e-3);
  // No cap.
  std::vector<f64> uncapped(n);
  for (usize k = 0; k < n; ++k) uncapped[k] = fit->share[k] / static_cast<f64>(themes);
  EXPECT_GT(max_gap(uncapped, fit->weights), 1e-3);
}

TEST(IcShrinkV1, RefusesBadInputsAndAnInfeasibleCap) {
  // Admitted: two themes of two equal members, every weight exactly at the cap 1/4.
  const std::vector<f64> four{0.002, 0.002, 0.001, 0.001};
  const std::vector<usize> pairs{0, 0, 1, 1};
  const auto at_cap = st::ic_shrink_weights(four, pairs, 2);
  ASSERT_TRUE(at_cap) << at_cap.error().to_string();
  EXPECT_EQ(at_cap->weights, (std::vector<f64>{0.25, 0.25, 0.25, 0.25}));
  EXPECT_EQ(at_cap->cap_passes, 0U);
  EXPECT_FALSE(st::ic_shrink_weights({}, {}, 1));
  EXPECT_FALSE(st::ic_shrink_weights(four, pairs, 0));
  EXPECT_FALSE(st::ic_shrink_weights(four, std::vector<usize>{0, 0, 1}, 2));      // shapes
  EXPECT_FALSE(st::ic_shrink_weights(four, std::vector<usize>{0, 0, 1, 2}, 2));   // theme index out of range
  EXPECT_FALSE(st::ic_shrink_weights(four, pairs, 3));                            // a theme without a member
  EXPECT_FALSE(st::ic_shrink_weights(std::vector<f64>{0.002, 0.002, 0.001, std::numeric_limits<f64>::quiet_NaN()},
                                     pairs, 2));
  EXPECT_FALSE(st::ic_shrink_weights(std::vector<f64>(257, 0.001), std::vector<usize>(257, 0), 1));
  std::vector<f64> wide(33, 0.001);
  std::vector<usize> each(33);
  for (usize k = 0; k < each.size(); ++k) each[k] = k;
  EXPECT_FALSE(st::ic_shrink_weights(wide, each, 33));                            // 33 themes
  // One theme: the cap 1/2 binds on the larger member and no other theme can take the excess;
  // two single-member themes: both capped at 1/4, nobody left to take the excess.
  EXPECT_FALSE(st::ic_shrink_weights(std::vector<f64>{0.002, 0.001}, std::vector<usize>{0, 1}, 2));
  const auto alone = st::ic_shrink_weights(std::vector<f64>{0.002, 0.001}, std::vector<usize>{0, 0}, 1);
  ASSERT_FALSE(alone);
  EXPECT_NE(alone.error().to_string().find("ic-shrink-v1: group cap"), std::string::npos)
      << alone.error().to_string();
}
} // namespace
