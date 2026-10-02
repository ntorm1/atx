// strategy_ic_theme_erc_test.cpp -- composition rule theme-erc-v1 (platform v8 expansion X, lane
// XCOMB): equal-risk-contribution theme shares times the parent's within-theme shares, then the
// member cap 1/(2T).
//
// Suite: ThemeErcV1. The runner side (the theme_standardise rule table, its verification and the
// unchanged standardisation) is CompositionV8.ThemeErc* in strategy_ic_runner_test.cpp.
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
#include "atx/engine/combine/group_erc.hpp"
#include "atx/engine/combine/group_shrink.hpp"
#include "strategy_ic_theme_erc.hpp"

namespace {
using namespace atx;
using Json = nlohmann::json;
namespace st = atx::impl::strategy;
namespace cb = atx::engine::combine;

// The fixture shared with the fitter's test (atx-impl/tools/test_composition_theme_erc.py).
struct ErcMember {
  std::string id, theme;
  f64 share{}, before{}, weight{};
};
struct ErcFixture {
  std::vector<ErcMember> members;
  std::vector<std::string> order; // the covariance's theme order (theme indices)
  std::vector<usize> theme;       // per member, index into `order`
  std::vector<f64> covariance;    // row-major, order x order
  std::vector<f64> theme_share;   // per theme of `order`
  f64 cap{};
  usize passes{};
  std::vector<std::string> capped;
};
f64 erc_fraction(const Json& pair) { return pair.at(0).get<f64>() / pair.at(1).get<f64>(); }
bool load_erc(ErcFixture& fx) {
  std::ifstream in(std::filesystem::path(ATX_IMPL_TESTS_DIR) / "fixtures" / "theme_erc_v1.json");
  if (!in) return false;
  const auto j = Json::parse(in);
  fx.order = j.at("covariance").at("themes").get<std::vector<std::string>>();
  for (const auto& row : j.at("covariance").at("matrix"))
    for (const auto& value : row) fx.covariance.push_back(erc_fraction(value));
  for (const auto& name : fx.order) fx.theme_share.push_back(erc_fraction(j.at("theme_shares").at(name)));
  for (const auto& row : j.at("members")) {
    ErcMember m{row.at("id").get<std::string>(), row.at("theme").get<std::string>(),
                erc_fraction(row.at("share")), erc_fraction(row.at("weight_before_cap")),
                erc_fraction(row.at("weight"))};
    const auto at = std::find(fx.order.begin(), fx.order.end(), m.theme);
    if (at == fx.order.end()) return false;
    fx.theme.push_back(static_cast<usize>(at - fx.order.begin()));
    fx.members.push_back(std::move(m));
  }
  fx.cap = erc_fraction(j.at("cap"));
  fx.passes = j.at("cap_passes").get<usize>();
  fx.capped = j.at("capped").get<std::vector<std::string>>();
  return j.at("sweeps").get<usize>() == st::theme_erc_sweeps &&
         j.at("dispersion").get<f64>() == st::theme_erc_dispersion &&
         fx.covariance.size() == fx.order.size() * fx.order.size();
}
std::vector<f64> erc_shares_of(const ErcFixture& fx) {
  std::vector<f64> out;
  for (const auto& m : fx.members) out.push_back(m.share);
  return out;
}
// share x theme share, then the engine cap at 1 / (2T) (empty when infeasible).
std::vector<f64> erc_capped(const ErcFixture& fx, const std::vector<f64>& theme_share) {
  std::vector<f64> w;
  for (usize k = 0; k < fx.members.size(); ++k) w.push_back(fx.members[k].share * theme_share[fx.theme[k]]);
  const auto themes = fx.order.size();
  const auto passes = cb::cap_across_groups(w, fx.theme, themes, 1.0 / (2.0 * static_cast<f64>(themes)), 1e-12);
  return passes ? w : std::vector<f64>{};
}
// Largest |a - b|; -1 when the sizes differ (a failed alternative never counts as "apart").
f64 erc_gap(const std::vector<f64>& a, const std::vector<f64>& b) {
  if (a.size() != b.size()) return -1.0;
  f64 gap = 0;
  for (usize k = 0; k < a.size(); ++k) gap = std::max(gap, std::abs(a[k] - b[k]));
  return gap;
}

TEST(ThemeErcV1, RegisteredConstants) {
  EXPECT_EQ(st::theme_erc_rule, "theme-erc-v1");
  EXPECT_EQ(st::theme_erc_sweeps, 10000U);
  EXPECT_EQ(st::theme_erc_dispersion, 1e-10);
  EXPECT_EQ(st::theme_erc_share_tolerance, 1e-12);
  EXPECT_EQ(st::theme_erc_cap_tolerance, 1e-12);
  EXPECT_EQ(st::theme_erc_weight_tolerance, 1e-12);
}

// The exact fractions of the shared fixture (the fitter's test asserts the same ones), so the
// Python rule and this one agree on it.
TEST(ThemeErcV1, SharedFixtureFractions) {
  ErcFixture fx;
  ASSERT_TRUE(load_erc(fx));
  const auto fit = st::theme_erc_weights(erc_shares_of(fx), fx.theme, fx.order.size(), fx.covariance);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->theme_share.size(), fx.order.size());
  ASSERT_EQ(fit->weights.size(), fx.members.size());
  for (usize t = 0; t < fx.order.size(); ++t)
    EXPECT_NEAR(fit->theme_share[t], fx.theme_share[t], 1e-15) << fx.order[t];
  for (usize k = 0; k < fx.members.size(); ++k) {
    SCOPED_TRACE(fx.members[k].id);
    EXPECT_NEAR(fit->weights[k], fx.members[k].weight, 1e-15);
    const bool capped = std::find(fx.capped.begin(), fx.capped.end(), fx.members[k].id) != fx.capped.end();
    EXPECT_EQ(fit->weights[k] == fx.cap, capped);
  }
  EXPECT_EQ(fit->cap, fx.cap);
  EXPECT_EQ(fit->cap_passes, fx.passes);
  EXPECT_LE(fit->dispersion, 1e-14);
}

// The written rule, computed here step by step: the theme shares are the ones whose risk
// contributions b_t (C b)_t are equal (checked on the contributions, not on the solver), each
// member takes share x its theme's share, and the cap 1 / (2T) binds on one member whose excess
// goes to the members of the other themes in proportion to their weights.
TEST(ThemeErcV1, WeightsFollowTheWrittenRule) {
  ErcFixture fx;
  ASSERT_TRUE(load_erc(fx));
  const usize n = fx.members.size(), themes = fx.order.size();
  const auto fit = st::theme_erc_weights(erc_shares_of(fx), fx.theme, themes, fx.covariance);
  ASSERT_TRUE(fit) << fit.error().to_string();
  ASSERT_EQ(fit->contribution.size(), themes);
  f64 lo = std::numeric_limits<f64>::infinity(), hi = 0, sum = 0;
  for (usize t = 0; t < themes; ++t) {
    f64 marginal = 0;
    for (usize u = 0; u < themes; ++u) marginal += fx.covariance[t * themes + u] * fit->theme_share[u];
    const f64 c = fit->theme_share[t] * marginal;
    EXPECT_NEAR(fit->contribution[t], c, 1e-15) << t;
    lo = std::min(lo, c); hi = std::max(hi, c); sum += fit->theme_share[t];
  }
  EXPECT_LE(hi - lo, 1e-14 * hi);
  EXPECT_NEAR(sum, 1.0, 1e-15);
  std::vector<f64> w(n);
  for (usize k = 0; k < n; ++k) {
    w[k] = fx.members[k].share * fit->theme_share[fx.theme[k]];
    EXPECT_NEAR(w[k], fx.members[k].before, 1e-15) << fx.members[k].id;
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
  EXPECT_LE(erc_gap(fit->weights, w), 1e-15);
  f64 total = 0;
  for (const f64 x : fit->weights) {
    total += x;
    EXPECT_GE(x, 0.0);
    EXPECT_LE(x, cap * (1.0 + 1e-12));
  }
  EXPECT_NEAR(total, 1.0, 1e-15);
}

// Review T-1: the fixture must fail a wrong rule. Equal theme shares (the parent's 1/T), inverse
// volatility shares (no correlation), no cap, and the covariance read in the members' theme order
// instead of its recorded order each move some weight by more than 1e-3.
TEST(ThemeErcV1, FixtureTellsWrongRulesApart) {
  ErcFixture fx;
  ASSERT_TRUE(load_erc(fx));
  const usize n = fx.members.size(), themes = fx.order.size();
  const auto fit = st::theme_erc_weights(erc_shares_of(fx), fx.theme, themes, fx.covariance);
  ASSERT_TRUE(fit) << fit.error().to_string();
  const std::vector<f64> equal(themes, 1.0 / static_cast<f64>(themes));
  EXPECT_GT(erc_gap(erc_capped(fx, equal), fit->weights), 1e-3);
  std::vector<f64> inverse(themes);
  f64 mass = 0;
  for (usize t = 0; t < themes; ++t) {
    inverse[t] = 1.0 / std::sqrt(fx.covariance[t * themes + t]);
    mass += inverse[t];
  }
  for (auto& v : inverse) v /= mass;
  EXPECT_GT(erc_gap(erc_capped(fx, inverse), fit->weights), 1e-3);
  std::vector<f64> uncapped(n);
  for (usize k = 0; k < n; ++k) uncapped[k] = fx.members[k].share * fit->theme_share[fx.theme[k]];
  EXPECT_GT(erc_gap(uncapped, fit->weights), 1e-3);
  // The matrix taken in the members' first-appearance order (value, momentum, quality): the shares
  // the solver gives for row t land on the theme that appears t-th.
  std::vector<usize> first;
  for (const usize t : fx.theme)
    if (std::find(first.begin(), first.end(), t) == first.end()) first.push_back(t);
  ASSERT_EQ(first.size(), themes);
  const auto solved = cb::group_erc_shares(fx.covariance, themes, st::theme_erc_sweeps);
  ASSERT_TRUE(solved) << solved.error().to_string();
  std::vector<f64> misread(themes);
  for (usize r = 0; r < themes; ++r) misread[first[r]] = solved->share[r];
  EXPECT_GT(erc_gap(erc_capped(fx, misread), fit->weights), 1e-3);
}

TEST(ThemeErcV1, RefusesBadInputsAndNamesTheRule) {
  ErcFixture fx;
  ASSERT_TRUE(load_erc(fx));
  const auto share = erc_shares_of(fx);
  const usize themes = fx.order.size();
  const auto refused = [&](std::vector<f64> s, std::vector<usize> theme, usize count, std::vector<f64> covariance,
                           const std::string& what) {
    const auto fit = st::theme_erc_weights(s, theme, count, covariance);
    if (fit) return ::testing::AssertionFailure() << "admitted: " << what;
    const auto message = fit.error().to_string();
    if (message.find("theme-erc-v1: ") == std::string::npos || message.find(what) == std::string::npos)
      return ::testing::AssertionFailure() << message << " lacks " << what;
    return ::testing::AssertionSuccess();
  };
  EXPECT_TRUE(refused({}, {}, 1, {1.0}, "1..256 members"));
  EXPECT_TRUE(refused(share, fx.theme, 0, fx.covariance, "1..256 members"));
  EXPECT_TRUE(refused(share, std::vector<usize>(share.size() - 1, 0), themes, fx.covariance, "1..256 members"));
  auto out_of_range = fx.theme;
  out_of_range[0] = themes;
  EXPECT_TRUE(refused(share, out_of_range, themes, fx.covariance, "a theme index out of range"));
  auto negative = share;
  negative[1] = -0.1;
  EXPECT_TRUE(refused(negative, fx.theme, themes, fx.covariance, "finite and >= 0"));
  auto not_finite = share;
  not_finite[1] = std::numeric_limits<f64>::quiet_NaN();
  EXPECT_TRUE(refused(not_finite, fx.theme, themes, fx.covariance, "finite and >= 0"));
  auto off = share;
  off[0] += 1e-9;
  EXPECT_TRUE(refused(off, fx.theme, themes, fx.covariance, "do not sum to 1"));
  std::vector<f64> four(16, 0.0);
  for (usize t = 0; t < 4; ++t) four[t * 4 + t] = 1.0;
  EXPECT_TRUE(refused(share, fx.theme, 4, four, "a theme without a member"));
  auto asymmetric = fx.covariance;
  asymmetric[1] += 1e-9;
  EXPECT_TRUE(refused(share, fx.theme, themes, asymmetric, "group erc: the covariance is not symmetric"));
  auto zero = fx.covariance;
  zero[0] = 0.0;
  EXPECT_TRUE(refused(share, fx.theme, themes, zero, "group erc: a variance that is not > 0"));
  // One theme of two members .75 / .25: the cap 1/2 binds and no other theme can take the excess.
  EXPECT_TRUE(refused({0.75, 0.25}, {0, 0}, 1, {2.0}, "group cap"));
  // Admitted: every share at its theme's equal split, the cap not binding.
  const auto plain = st::theme_erc_weights(std::vector<f64>{0.5, 0.5, 0.5, 0.5}, std::vector<usize>{0, 0, 1, 1}, 2,
                                           std::vector<f64>{1.0, 0.0, 0.0, 1.0});
  ASSERT_TRUE(plain) << plain.error().to_string();
  EXPECT_EQ(plain->weights, (std::vector<f64>{0.25, 0.25, 0.25, 0.25}));
  EXPECT_EQ(plain->cap_passes, 0U);
}
} // namespace
