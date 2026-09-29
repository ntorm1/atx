// Cost model v2 (S2-KO, S2-FIM), the capacity scenarios, aim-partial-v6 and the v7 NAV hook
// (platform v7 lane L4). Synthetic inputs only.
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "atx/engine/book/replay_cost.hpp"
#include "../src/strategy_cost_v2.hpp"
#include "../src/strategy_nav_replay.hpp"
#include "../src/strategy_nav_v7.hpp"
#include "../src/strategy_target_replay.hpp"
#include "../src/strategy_target_replay_detail.hpp"

namespace {
using namespace atx;
namespace st = atx::impl::strategy;
namespace cv = atx::impl::strategy::cost_v2;
namespace v7 = atx::impl::strategy::v7;
namespace bk = atx::engine::book;
using Json = nlohmann::json;
constexpr i64 day_ns = 86'400'000'000'000LL;
constexpr f64 missing = std::numeric_limits<f64>::quiet_NaN();

u64 bits(f64 x) { return std::bit_cast<u64>(x); }
struct Lcg {
  u64 state;
  f64 next() { // uniform [0, 1)
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(state >> 11U) * 0x1.0p-53;
  }
};
std::vector<i64> weekdays(usize count) {
  std::vector<i64> out;
  auto date = std::chrono::sys_days{std::chrono::year{2020} / 1 / 2};
  while (out.size() < count) {
    const std::chrono::weekday wd{date};
    if (wd != std::chrono::Saturday && wd != std::chrono::Sunday)
      out.push_back(static_cast<i64>(date.time_since_epoch().count()) * day_ns);
    date += std::chrono::days{1};
  }
  return out;
}
// Random-walk role: every name present; names differ in ADV ($1e6 .. $2.4e7 at price ~100);
// name 11 leaves membership for a stretch (exit / decay path). Signals random per cell.
struct Role {
  usize d{}, n{};
  std::vector<f64> signal, close, raw, volume;
  std::vector<u8> member, present;
  std::vector<i64> sessions;
  std::vector<u64> ids;
  Role(usize dates, usize names, u64 seed)
      : d(dates), n(names), signal(dates * names), close(dates * names), raw(dates * names),
        volume(dates * names), member(dates * names, 1), present(dates * names, 1),
        sessions(weekdays(dates)), ids(names) {
    Lcg rng{seed};
    for (usize i = 0; i < n; ++i) ids[i] = 100 + i;
    for (usize t = 0; t < d; ++t)
      for (usize i = 0; i < n; ++i) {
        const usize k = t * n + i;
        close[k] = t == 0 ? 100.0 : close[k - n] * (1.0 + 0.04 * (rng.next() - 0.5));
        raw[k] = close[k];
        volume[k] = 1e4 * static_cast<f64>(1 + 2 * i) * (0.5 + rng.next());
        signal[k] = rng.next();
      }
    for (usize t = d / 3; t < d / 2; ++t) {
      member[t * n + n - 1] = 0; signal[t * n + n - 1] = missing;
    }
  }
  [[nodiscard]] st::TargetReplayInput target() const {
    return {d, n, 0, d, signal, member, sessions, ids, close, raw, present, volume};
  }
  [[nodiscard]] st::NavReplayInput nav() const { return {target(), volume}; }
};
st::TargetReplayConfig aim_v5() {
  st::TargetReplayConfig c;
  c.rule = st::TargetReplayRule::AimPartialV5; c.cadence = 1; c.trade_fraction = 0.25;
  c.dust_multiple = 0.1; c.aim_leverage = 1.2; c.exit_rate = 0.05;
  return c;
}
st::NavReplayConfig nav_config(const st::NavScenario& s, f64 nav) {
  st::NavReplayConfig c;
  c.target = aim_v5(); c.scenario = s; c.initial_nav = nav;
  c.liquidity_window = 4; c.min_vol_pairs = 2;
  return c;
}
st::NavScenario s1() { return st::fixed_nav_scenarios()[0]; }
st::NavScenario s2() { return st::fixed_nav_scenarios()[st::nav_primary_scenario_index]; }
st::NavScenario s3() { return st::fixed_nav_scenarios()[2]; }
void expect_same_days(const st::NavReplayResult& a, const st::NavReplayResult& b) {
  ASSERT_EQ(a.days.size(), b.days.size());
  for (usize k = 0; k < a.days.size(); ++k) {
    const auto& x = a.days[k]; const auto& y = b.days[k];
    EXPECT_EQ(bits(x.net_return), bits(y.net_return)) << k;
    EXPECT_EQ(bits(x.trade_cost_dollars), bits(y.trade_cost_dollars)) << k;
    EXPECT_EQ(bits(x.traded_dollars), bits(y.traded_dollars)) << k;
    EXPECT_EQ(bits(x.planned_turnover), bits(y.planned_turnover)) << k;
    EXPECT_EQ(bits(x.planned_gross), bits(y.planned_gross)) << k;
    EXPECT_EQ(bits(x.applied_fraction), bits(y.applied_fraction)) << k;
    EXPECT_EQ(bits(x.posttrade_nav), bits(y.posttrade_nav)) << k;
    EXPECT_EQ(x.fills, y.fills) << k;
    EXPECT_EQ(x.capped_fills, y.capped_fills) << k;
    EXPECT_EQ(x.held_names, y.held_names) << k;
    EXPECT_EQ(x.construction.banded_names, y.construction.banded_names) << k;
  }
}
bk::LiquidityRow row(f64 adv, f64 vol) { return bk::LiquidityRow{adv, vol, 0.0}; }

// ---- laws ---------------------------------------------------------------------------------
TEST(CostV2Ko, BenchmarkStockAtOnePercentOfAdvIs1416Bps) {
  // sigma .02 and ADV $40m: W = .02 * 4e7 = W* exactly.
  EXPECT_NEAR(cv::ko_cost_fraction(0.01, 0.02, 4.0e7), 14.16e-4, 1e-15);
  EXPECT_NEAR(cv::ko_cost_fraction(0.0, 0.02, 4.0e7), 2.08e-4, 1e-15);
}
TEST(CostV2Ko, SpreadTermScalesAsActivityToMinusOneThirdImpactTermDoesNot) {
  // 8x the activity halves the kappa0 term; the kappa_I term is (sigma/.02) sqrt(x/.01).
  EXPECT_NEAR(cv::ko_cost_fraction(0.0, 0.02, 3.2e8), 1.04e-4, 1e-15);
  for (const f64 adv : {1e6, 4e7, 3.2e8}) {
    const f64 impact = cv::ko_cost_fraction(0.04, 0.03, adv) - cv::ko_cost_fraction(0.0, 0.03, adv);
    EXPECT_NEAR(impact, 12.08e-4 * 1.5 * 2.0, 1e-13) << adv;
  }
  EXPECT_EQ(cv::ko_cost_fraction(0.01, 0.0, 4.0e7), 0.0);
  EXPECT_TRUE(std::isnan(cv::ko_cost_fraction(0.01, 0.02, 0.0)));
}
TEST(CostV2Fim, TableElevenMedianAtOnePercentAndTheCurve) {
  EXPECT_NEAR(cv::fim_a_bps, 3.87, 1e-12);
  EXPECT_NEAR(cv::fim_cost_fraction(0.01), 14.55e-4, 1e-15);
  EXPECT_NEAR(cv::fim_cost_fraction(0.0), 3.87e-4, 1e-15);
  EXPECT_NEAR(cv::fim_cost_fraction(0.02), (3.87 - 0.53 * 2.0 + 11.21 * std::sqrt(2.0)) * 1e-4, 1e-15);
  EXPECT_TRUE(std::isnan(cv::fim_cost_fraction(-1.0)));
}
TEST(CostV2Model, CapFillsAtMostOnePercentAndUnrationedPricesTheWholeRequest) {
  auto model = cv::NameImpactCost::create(cv::ImpactLaw::KyleObizhaevaV1, 1.0, 0.01);
  ASSERT_TRUE(model);
  const auto liquidity = row(1e6, 0.02);
  const auto capped = model->cost(0, 0, -5e4, liquidity);
  EXPECT_EQ(capped.filled_dollars, -1e4);
  EXPECT_NEAR(capped.cost_dollars, 1e4 * model->cost_fraction(1e4, liquidity), 1e-9);
  EXPECT_NEAR(model->unrationed_cost(0, 0, -5e4, liquidity), 5e4 * model->cost_fraction(5e4, liquidity), 1e-9);
  const f64 small = 1234.5678;
  EXPECT_EQ(bits(model->cost(0, 0, small, liquidity).filled_dollars), bits(small));
  EXPECT_EQ(model->cost(0, 0, 1e3, row(0.0, 0.02)).filled_dollars, 0.0);
  EXPECT_NEAR(model->cost_fraction(1e4, liquidity) - cv::ko_cost_fraction(0.01, 0.02, 1e6), 1e-4, 1e-15);
  auto fim = cv::NameImpactCost::create(cv::ImpactLaw::FimLiveV1, 0.0, 0.01);
  ASSERT_TRUE(fim);
  EXPECT_NEAR(fim->cost_fraction(1e4, liquidity), 14.55e-4, 1e-15);
  EXPECT_FALSE(cv::NameImpactCost::create(cv::ImpactLaw::FimLiveV1, -1.0, 0.01));
  EXPECT_FALSE(cv::NameImpactCost::create(cv::ImpactLaw::FimLiveV1, 0.0, 0.0));
}

// ---- scenarios ----------------------------------------------------------------------------
TEST(CostV2Scenarios, ReservedIdsMapToTheirLawAndRefuseAnotherShape) {
  const auto ko = cv::ko_scenario(s2());
  const auto fim = cv::fim_scenario(s2());
  EXPECT_EQ(ko.id, "modeled-1bn-ko-v1"); EXPECT_EQ(fim.id, "modeled-1bn-fim-v1");
  EXPECT_EQ(ko.commission_bps, s2().commission_bps); EXPECT_EQ(fim.commission_bps, 0.0);
  EXPECT_EQ(ko.max_participation, s2().max_participation);
  EXPECT_EQ(ko.financing.id, s2().financing.id);
  auto model = cv::reserved_cost_model(ko);
  ASSERT_TRUE(model); ASSERT_TRUE(*model);
  const auto* law = dynamic_cast<const cv::NameImpactCost*>(model->get());
  ASSERT_NE(law, nullptr); EXPECT_EQ(law->law(), cv::ImpactLaw::KyleObizhaevaV1);
  auto plain = cv::reserved_cost_model(s2());
  ASSERT_TRUE(plain); EXPECT_FALSE(*plain);
  auto bent = ko; bent.half_spread_bps = 5;
  EXPECT_FALSE(cv::reserved_cost_model(bent));
  EXPECT_STREQ(cv::reserved_cost_rule("modeled-1bn-fim-v1"), "fim-live-v1");
  EXPECT_EQ(cv::reserved_cost_rule("modeled-1bn-stale5-v1"), nullptr);
}
TEST(CostV2Scenarios, StressBooksLeaveS1S2S3BitIdentical) {
  const Role role(40, 12, 11);
  const auto base = nav_config(s2(), 1e8);
  const std::vector<st::NavScenario> three{s1(), s2(), s3()};
  const std::vector<st::NavScenario> five{s1(), s2(), s3(), cv::ko_scenario(s2()), cv::fim_scenario(s2())};
  auto a = st::replay_nav_scenarios(role.nav(), base, three);
  auto b = st::replay_nav_scenarios(role.nav(), base, five);
  ASSERT_TRUE(a) << a.error().to_string();
  ASSERT_TRUE(b) << b.error().to_string();
  for (usize k = 0; k < 3; ++k) expect_same_days((*a)[k], (*b)[k]);
  f64 s2_cost = 0, ko_cost = 0, fim_cost = 0;
  for (usize t = 0; t < (*b)[1].days.size(); ++t) {
    s2_cost += (*b)[1].days[t].trade_cost_dollars; ko_cost += (*b)[3].days[t].trade_cost_dollars;
    fim_cost += (*b)[4].days[t].trade_cost_dollars;
  }
  EXPECT_GT(ko_cost, 0.0); EXPECT_GT(fim_cost, 0.0);
  EXPECT_NE(bits(ko_cost), bits(s2_cost)); EXPECT_NE(bits(fim_cost), bits(s2_cost));
}
TEST(CostV2Capacity, ScaledScenarioIsTheNavMultipleBookDividedByTheMultiple) {
  const Role role(40, 12, 23);
  const auto books = cv::capacity_scenarios(s2());
  ASSERT_EQ(books.size(), 5U);
  EXPECT_EQ(books[1].id, "capacity-x1-v1"); EXPECT_EQ(books[0].id, "capacity-x0p5-v1");
  EXPECT_EQ(bits(books[1].impact_y), bits(s2().impact_y));
  EXPECT_EQ(bits(books[1].max_participation), bits(s2().max_participation));
  EXPECT_EQ(cv::capacity_multiple("capacity-x4-v1"), 4.0);
  EXPECT_TRUE(std::isnan(cv::capacity_multiple("modeled-1bn-stale5-v1")));
  // x1 is S2 bit for bit.
  auto unit = st::replay_nav(role.nav(), nav_config(books[1], 1e8));
  auto primary = st::replay_nav(role.nav(), nav_config(s2(), 1e8));
  ASSERT_TRUE(unit && primary);
  expect_same_days(*unit, *primary);
  // x2 at V equals S2 at 2V divided by 2, with the cap binding.
  auto scaled = st::replay_nav(role.nav(), nav_config(books[2], 1e8));
  auto genuine = st::replay_nav(role.nav(), nav_config(s2(), 2e8));
  ASSERT_TRUE(scaled && genuine);
  usize capped = 0;
  for (usize t = 0; t < genuine->days.size(); ++t) {
    const auto& g = genuine->days[t]; const auto& s = scaled->days[t];
    EXPECT_NEAR(s.net_return, g.net_return, 1e-12 + 1e-9 * std::abs(g.net_return)) << t;
    EXPECT_NEAR(2.0 * s.trade_cost_dollars, g.trade_cost_dollars, 1e-6 + 1e-9 * g.trade_cost_dollars) << t;
    EXPECT_EQ(s.capped_fills, g.capped_fills) << t;
    capped += g.capped_fills;
  }
  EXPECT_GT(capped, 0U);
}

// ---- aim-partial-v6 -----------------------------------------------------------------------
struct Decision {
  std::vector<f64> desired, current, cost;
};
Decision decision(const Role& role, usize d, u64 seed) {
  Lcg rng{seed};
  Decision out;
  out.desired.assign(role.n, 0.0); out.current.assign(role.n, 0.0); out.cost.assign(role.n, missing);
  f64 mean = 0; usize members = 0;
  for (usize i = 0; i < role.n; ++i)
    if (role.member[d * role.n + i]) { out.desired[i] = rng.next(); mean += out.desired[i]; ++members; }
  mean /= static_cast<f64>(members);
  f64 gross = 0;
  for (usize i = 0; i < role.n; ++i)
    if (role.member[d * role.n + i]) { out.desired[i] -= mean; gross += std::abs(out.desired[i]); }
  for (usize i = 0; i < role.n; ++i) {
    out.desired[i] /= gross;
    // Some members sit inside the dust band (current == aim +- tiny), the rest far away.
    out.current[i] = i % 3 == 0 ? 1.2 * out.desired[i] + 1e-4 * (rng.next() - 0.5)
                                 : 0.2 * (rng.next() - 0.5);
    if (role.member[d * role.n + i]) out.cost[i] = 1e-3 * (1.0 + 7.0 * rng.next());
  }
  return out;
}
void expect_same_plan(const st::TargetReplayDay& a, const st::TargetReplayDay& b) {
  EXPECT_EQ(bits(a.turnover), bits(b.turnover)); EXPECT_EQ(bits(a.forced_turnover), bits(b.forced_turnover));
  EXPECT_EQ(bits(a.discretionary_turnover), bits(b.discretionary_turnover));
  EXPECT_EQ(bits(a.gross), bits(b.gross)); EXPECT_EQ(bits(a.net), bits(b.net));
  EXPECT_EQ(bits(a.applied_fraction), bits(b.applied_fraction));
  EXPECT_EQ(bits(a.effective_names), bits(b.effective_names));
  EXPECT_EQ(a.held_names, b.held_names);
  EXPECT_EQ(a.construction.banded_names, b.construction.banded_names);
}
TEST(AimV6, KappaZeroClipOneAndTheDustBandIsV5BitForBit) {
  const Role role(30, 12, 5);
  const auto cfg = aim_v5();
  cv::AimV6Params p;
  p.kappa = 0; p.band_b = cfg.dust_multiple; p.clip_lo = 1; p.clip_hi = 1; p.band_exponent = 0;
  for (const usize d : {usize{3}, usize{12}, usize{20}}) { // 12: name 11 is a nonmember (decay)
    for (const bool rebalance : {true, false}) {
      auto x = decision(role, d, 100 + d);
      auto v5 = x.current, v6 = x.current;
      st::TargetReplayDay a, b;
      ASSERT_TRUE(st::detail::update_weights(role.target(), cfg, d, rebalance, 0.0, x.desired, v5, a));
      const f64 c_bar = cv::finite_median(x.cost);
      cv::AimV6Decision dec;
      cv::form_aim_v6(std::span<const u8>(role.member).subspan(d * role.n, role.n), x.desired, x.cost,
                      c_bar, 1.7 * c_bar, cfg.trade_fraction, p, rebalance, dec);
      ASSERT_TRUE(cv::aim_partial_v6_weights(role.target(), cfg, d, rebalance, dec, v6, b));
      for (usize i = 0; i < role.n; ++i) EXPECT_EQ(bits(v5[i]), bits(v6[i])) << d << ' ' << i;
      expect_same_plan(a, b);
      if (rebalance) EXPECT_GT(a.construction.banded_names, 0U);
    }
  }
}
TEST(AimV6, EqualCostsWithTheDefaultCubeRootBandAreV5BitForBit) {
  const Role role(30, 12, 6);
  const auto cfg = aim_v5();
  cv::AimV6Params p; // kappa 1, exponent 1/3, clip [.5, 1.5]
  p.kappa = 0; p.band_b = cfg.dust_multiple;
  auto x = decision(role, 7, 77);
  for (auto& c : x.cost) if (std::isfinite(c)) c = 2.5e-3;
  auto v5 = x.current, v6 = x.current;
  st::TargetReplayDay a, b;
  ASSERT_TRUE(st::detail::update_weights(role.target(), cfg, 7, true, 0.0, x.desired, v5, a));
  cv::AimV6Decision dec; // c_ref == c_bar: theta_t = theta * clip(1) = theta
  cv::form_aim_v6(std::span<const u8>(role.member).subspan(7 * role.n, role.n), x.desired, x.cost,
                  2.5e-3, 2.5e-3, cfg.trade_fraction, p, true, dec);
  ASSERT_TRUE(cv::aim_partial_v6_weights(role.target(), cfg, 7, true, dec, v6, b));
  for (usize i = 0; i < role.n; ++i) EXPECT_EQ(bits(v5[i]), bits(v6[i])) << i;
  expect_same_plan(a, b);
}
TEST(AimV6, ShrinkBandAndRegimeRateFollowTheDeclaredFormulas) {
  const std::vector<u8> member{1, 1, 1, 1, 0};
  const std::vector<f64> desired{0.3, -0.2, 0.25, -0.25, 0.0};
  const std::vector<f64> cost{1e-3, 2e-3, 4e-3, 8e-3, missing};
  const f64 c_bar = cv::finite_median(cost);
  EXPECT_DOUBLE_EQ(c_bar, 3e-3);
  cv::AimV6Params p; // kappa 1, b .1, clip [.5, 1.5], exponent 1/3
  cv::AimV6Decision dec;
  cv::form_aim_v6(member, desired, cost, c_bar, 6e-3, 0.05, p, true, dec);
  const f64 t0 = 0.3 / (1 + 1.0 / 3), t2 = 0.25 / (1 + 4.0 / 3);
  const f64 t1 = -0.2 / (1 + 2.0 / 3), t3 = -0.25 / (1 + 8.0 / 3);
  const f64 long_scale = 0.55 / (t0 + t2), short_scale = 0.45 / -(t1 + t3);
  EXPECT_NEAR(dec.target[0], t0 * long_scale, 1e-15); EXPECT_NEAR(dec.target[2], t2 * long_scale, 1e-15);
  EXPECT_NEAR(dec.target[1], t1 * short_scale, 1e-15); EXPECT_NEAR(dec.target[3], t3 * short_scale, 1e-15);
  EXPECT_EQ(dec.target[4], 0.0);
  EXPECT_NEAR(dec.target[0] + dec.target[2], 0.55, 1e-15); // side gross kept
  EXPECT_NEAR(dec.band[3], 0.1 / 4 * std::cbrt(8.0 / 3), 1e-15);
  EXPECT_EQ(dec.band[4], -1.0);
  EXPECT_NEAR(dec.theta, 0.05 * std::sqrt(2.0), 1e-15);   // (6/3)^(1/2) inside [.5, 1.5]
  cv::form_aim_v6(member, desired, cost, c_bar, 12e-3, 0.05, p, true, dec);
  EXPECT_NEAR(dec.theta, 0.05 * 1.5, 1e-15);              // (12/3)^(1/2) = 2 clipped to 1.5
  cv::form_aim_v6(member, desired, cost, c_bar, 0.3e-3, 0.05, p, true, dec);
  EXPECT_NEAR(dec.theta, 0.05 * 0.5, 1e-15);              // clipped at .5
  EXPECT_FALSE(cv::validate_aim_v6(p, 0.9));              // theta * hi > 1
  EXPECT_TRUE(cv::validate_aim_v6(p, 0.05));
}
TEST(AimV6, MarginalCostIsTheDerivativeOfTheS2Law) {
  const auto s = s2();
  const f64 q = 5e4, adv = 2e7, sigma = 0.018;
  const f64 h = 1.0;
  const auto total = [&](f64 x) { return x * ((s.half_spread_bps + s.commission_bps) * 1e-4 +
                                              s.impact_y * sigma * std::sqrt(x / adv)); };
  EXPECT_NEAR(cv::marginal_cost_s2(s, q, adv, sigma), (total(q + h) - total(q - h)) / (2 * h), 1e-9);
  EXPECT_NEAR(cv::marginal_cost_s2(s, q, adv, missing),
              6e-4 + 1.5 * 0.6 * s.fallback_daily_vol * std::sqrt(q / adv), 1e-15);
  EXPECT_TRUE(std::isnan(cv::marginal_cost_s2(s, q, 0.0, sigma)));
}
TEST(TransferCoefficient, OneForTheUnconstrainedSignalPortfolioNaNWhenFlat) {
  const std::vector<u8> member{1, 1, 1, 1, 1};
  const std::vector<f64> desired{0.2, -0.1, 0.05, -0.3, 0.15}, sigma{0.01, 0.02, 0.015, 0.03, 0.02};
  std::vector<f64> w(5);
  for (usize i = 0; i < 5; ++i) w[i] = 3.0 * desired[i] / sigma[i];
  EXPECT_NEAR(cv::transfer_coefficient(member, desired, sigma, w), 1.0, 1e-12);
  EXPECT_TRUE(std::isnan(cv::transfer_coefficient(member, desired, sigma, std::vector<f64>(5, 0.1))));
}

// ---- the v7 hook --------------------------------------------------------------------------
TEST(NavV7Hook, NoExtensionIsTheIdentityAtEverySeam) {
  const auto matrix = st::nav_scenario_matrix(false);
  const auto same = v7::run_scenarios(matrix);
  ASSERT_EQ(same.size(), matrix.size());
  Json recipe{{"rule", "aim-partial-v5"}, {"scenarios", Json::array({{{"trading_scenario", "x"},
                                                                      {"cost_rule", "sqrt-impact-v1"}}})}};
  const Json before = recipe;
  v7::extend_recipe(recipe);
  EXPECT_EQ(recipe, before);
  Json summary{{"rule", "aim-partial-v5"}};
  v7::extend_summary(summary);
  EXPECT_EQ(summary, Json({{"rule", "aim-partial-v5"}}));
  Json reserved{{"scenarios", Json::array({{{"trading_scenario", "modeled-1bn-ko-v1"},
                                            {"cost_rule", "sqrt-impact-v1"}}})}};
  v7::extend_recipe(reserved);
  EXPECT_EQ(reserved["scenarios"][0]["cost_rule"], "ko-invariance-v1");
  Json holdings{{"rule", "aim-partial-v5"}};
  v7::extend_holdings(holdings);
  EXPECT_EQ(holdings, Json({{"rule", "aim-partial-v5"}}));
}
TEST(NavV7Hook, HelpListsTheV7FlagsAndV6RelabelsTheHoldingsManifest) {
  std::ostringstream help;
  v7::append_help(help);
  for (const char* flag : {"--cost-v2", "--capacity-curve", "aim-partial-v6", "--cost-shrink-kappa",
                           "--band-b", "--band-exponent", "--rate-clip"})
    EXPECT_NE(help.str().find(flag), std::string::npos) << flag;
  v7::NavV7Options o; o.aim_v6 = true;
  const v7::ScopedNavExtension extension(o);
  Json holdings{{"rule", "aim-partial-v5"}};
  v7::extend_holdings(holdings);
  EXPECT_EQ(holdings["rule"], "aim-partial-v6");
  EXPECT_TRUE(holdings.contains("v7"));
}
TEST(NavV7Hook, ClaimsOnlyTheV7Flags) {
  std::vector<std::string> plain{"nav", "--rule", "aim-partial-v5", "--output", "x"};
  std::vector<std::string> v6{"nav", "--rule", "aim-partial-v6", "--output", "x"};
  std::vector<std::string> stress{"nav", "--output", "x", "--cost-v2"};
  const auto claims = [](std::vector<std::string> args) {
    std::vector<char*> p;
    for (auto& a : args) p.push_back(a.data());
    return v7::claims_nav_args(static_cast<int>(p.size()), p.data());
  };
  EXPECT_FALSE(claims(plain)); EXPECT_TRUE(claims(v6)); EXPECT_TRUE(claims(stress));
}
TEST(NavV7Hook, ScenarioListsForStressAndCapacityPasses) {
  v7::NavV7Options o; o.stress = true; o.capacity = true;
  v7::ScopedNavExtension extension(o);
  const auto main = v7::run_scenarios(st::nav_scenario_matrix(false));
  ASSERT_EQ(main.size(), 5U);
  EXPECT_EQ(main[3].id, "modeled-1bn-ko-v1"); EXPECT_EQ(main[4].id, "modeled-1bn-fim-v1");
  extension.begin_run(v7::NavV7Pass::Capacity);
  const auto capacity = v7::run_scenarios(st::nav_scenario_matrix(false));
  ASSERT_EQ(capacity.size(), 5U);
  EXPECT_EQ(capacity[st::nav_primary_scenario_index].id, "capacity-x1-v1");
}
TEST(NavV7Hook, AimV6NeutralParametersReplayV5BitForBitAndRecordTheTransferCoefficient) {
  const Role role(40, 12, 31);
  const auto cfg = nav_config(s2(), 1e8);
  auto plain = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(plain) << plain.error().to_string();
  v7::NavV7Options o; o.aim_v6 = true;
  o.v6.kappa = 0; o.v6.band_b = cfg.target.dust_multiple; o.v6.clip_lo = 1; o.v6.clip_hi = 1;
  o.v6.band_exponent = 0;
  v7::ScopedNavExtension extension(o);
  auto hooked = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(hooked) << hooked.error().to_string();
  expect_same_days(*plain, *hooked);
  ASSERT_FALSE(extension.tc_records().empty());
  usize finite = 0, priced = 0;
  for (const auto& r : extension.tc_records()) {
    finite += std::isfinite(r.tc) ? 1U : 0U;
    priced += std::isfinite(r.c_bar) && r.c_bar > 0 ? 1U : 0U; // not at the first decision (no ADV yet)
    EXPECT_EQ(r.theta, cfg.target.trade_fraction);
  }
  EXPECT_GT(finite, 0U);
  EXPECT_GT(priced, 0U);
  Json recipe{{"rule", "aim-partial-v5+neutral-price-risk-v1"}};
  v7::extend_recipe(recipe);
  EXPECT_EQ(recipe["rule"], "aim-partial-v6+neutral-price-risk-v1");
  EXPECT_TRUE(recipe.contains("v7"));
}
TEST(NavV7Hook, AimV6DefaultParametersChangeThePlanButKeepTheBookDollarNeutral) {
  const Role role(40, 12, 37);
  const auto cfg = nav_config(s2(), 1e8);
  auto plain = st::replay_nav(role.nav(), cfg);
  v7::NavV7Options o; o.aim_v6 = true; o.v6.band_b = cfg.target.dust_multiple;
  v7::ScopedNavExtension extension(o);
  auto hooked = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(plain && hooked);
  bool differs = false;
  for (usize t = 0; t < plain->days.size(); ++t) {
    differs = differs || bits(plain->days[t].planned_gross) != bits(hooked->days[t].planned_gross);
    if (hooked->days[t].decision) EXPECT_LT(std::abs(hooked->days[t].planned_net), 0.2) << t;
  }
  EXPECT_TRUE(differs);
}
// R1 I-1: under --capacity-curve a v6 capacity book prices c_i with its own scaled law, i.e. the
// NAV-m book's c_i. x1 is the main pass's S2 book bit for bit, x4 at V is the v6 book at 4V
// divided by 4, and the base-scale pricing (the main pass's S2 law on the x4 book) is not.
TEST(NavV7Hook, AimV6CapacityBookIsTheNavMultipleBookAndX1IsTheMainPass) {
  const Role role(40, 12, 23);
  const auto books = cv::capacity_scenarios(s2());
  ASSERT_EQ(books[3].id, "capacity-x4-v1");
  v7::NavV7Options o; o.aim_v6 = true; o.capacity = true;
  o.v6.band_b = aim_v5().dust_multiple; // else defaults: kappa 1, band exponent 1/3, clip .5,1.5
  v7::ScopedNavExtension extension(o);
  const auto pass = [&](v7::NavV7Pass p, const st::NavScenario& s, f64 nav) {
    extension.begin_run(p); // a fresh c_ref history per replayed book
    return st::replay_nav(role.nav(), nav_config(s, nav));
  };
  const auto main = pass(v7::NavV7Pass::Main, s2(), 1e8);
  const auto genuine = pass(v7::NavV7Pass::Main, s2(), 4e8);          // the NAV-4 v6 book
  const auto unit = pass(v7::NavV7Pass::Capacity, books[1], 1e8);
  const auto x4 = pass(v7::NavV7Pass::Capacity, books[3], 1e8);
  const auto base_priced = pass(v7::NavV7Pass::Main, books[3], 1e8); // c_i at base scale (pre-fix)
  ASSERT_TRUE(main && genuine && unit && x4 && base_priced);
  expect_same_days(*unit, *main);
  ASSERT_EQ(x4->days.size(), genuine->days.size());
  ASSERT_EQ(base_priced->days.size(), genuine->days.size());
  usize capped = 0, rebalances = 0;
  f64 base_gap = 0;
  bool plan_differs = false;
  for (usize t = 0; t < genuine->days.size(); ++t) {
    const auto& g = genuine->days[t];
    const auto& s = x4->days[t];
    const auto& b = base_priced->days[t];
    EXPECT_NEAR(s.net_return, g.net_return, 1e-12 + 1e-9 * std::abs(g.net_return)) << t;
    EXPECT_NEAR(4.0 * s.trade_cost_dollars, g.trade_cost_dollars,
                1e-6 + 1e-9 * g.trade_cost_dollars) << t;
    EXPECT_NEAR(s.planned_gross, g.planned_gross, 1e-12 + 1e-9 * g.planned_gross) << t;
    EXPECT_EQ(s.capped_fills, g.capped_fills) << t;
    EXPECT_EQ(s.construction.banded_names, g.construction.banded_names) << t;
    capped += g.capped_fills; rebalances += g.decision ? 1U : 0U;
    base_gap = std::max(base_gap, std::abs(b.net_return - g.net_return));
    plan_differs = plan_differs || bits(b.planned_gross) != bits(s.planned_gross) ||
                   bits(b.planned_turnover) != bits(s.planned_turnover);
  }
  EXPECT_GT(capped, 0U);
  EXPECT_GT(rebalances, 0U);
  EXPECT_TRUE(plan_differs);
  EXPECT_GT(base_gap, 1e-7); // the base-scale c_i replays another construction than the NAV-4 book
}
// The CLI path (parse_nav_v7_args) of the pre-registered identity cell: kappa 0, band exponent
// 0, clip 1,1 and the default band b = --dust-multiple replay aim-partial-v5 bit for bit.
atx::core::Result<v7::NavV7Command> parse_v7(std::vector<std::string> args) {
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  return v7::parse_nav_v7_args(static_cast<int>(argv.size()), argv.data());
}
std::vector<std::string> v6_args(std::initializer_list<std::string> extra) {
  std::vector<std::string> args{"nav", "--rule", "aim-partial-v6", "--cost-shrink-kappa", "0",
                                "--rate-clip", "1,1", "--dust-multiple", "0.1",
                                "--trade-fraction", "0.25", "--output", "x"};
  args.insert(args.end(), extra.begin(), extra.end());
  return args;
}
TEST(NavV7Hook, ParsedIdentityCellBandExponentZeroReplaysV5BitForBit) {
  const Role role(40, 12, 31);
  const auto cfg = nav_config(s2(), 1e8);
  auto plain = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(plain) << plain.error().to_string();
  const auto parsed = parse_v7(v6_args({"--band-exponent", "0"}));
  ASSERT_TRUE(parsed) << parsed.error().to_string();
  const auto& o = parsed->options;
  EXPECT_TRUE(o.aim_v6);
  EXPECT_EQ(bits(o.v6.kappa), bits(0.0));
  EXPECT_EQ(bits(o.v6.band_exponent), bits(0.0));
  EXPECT_EQ(bits(o.v6.band_b), bits(cfg.target.dust_multiple)); // b defaults to the dust
  EXPECT_EQ(bits(o.v6.clip_lo), bits(1.0)); EXPECT_EQ(bits(o.v6.clip_hi), bits(1.0));
  const std::vector<std::string> replay{"nav", "--rule", "aim-partial-v5", "--dust-multiple", "0.1",
                                        "--trade-fraction", "0.25", "--output", "x"};
  EXPECT_EQ(parsed->args, replay); // v7 tokens consumed, the rule rewritten, order kept
  EXPECT_EQ(parsed->output, "x");
  v7::ScopedNavExtension extension(o);
  auto hooked = st::replay_nav(role.nav(), cfg);
  ASSERT_TRUE(hooked) << hooked.error().to_string();
  expect_same_days(*plain, *hooked);
  ASSERT_EQ(plain->days.size(), hooked->days.size());
  for (usize t = 0; t < plain->days.size(); ++t)
    EXPECT_EQ(plain->days[t].construction.banded_names, hooked->days[t].construction.banded_names) << t;
}
TEST(NavV7Hook, ParsedDefaultBandExponentIsOneThirdAndChangesBandedNames) {
  const Role role(60, 24, 43);
  const auto cfg = nav_config(s2(), 1e8);
  const auto banded = [&](std::vector<std::string> args, f64 exponent) {
    std::vector<usize> out;
    const auto parsed = parse_v7(std::move(args));
    EXPECT_TRUE(parsed);
    if (!parsed) return out;
    EXPECT_EQ(bits(parsed->options.v6.band_exponent), bits(exponent));
    v7::ScopedNavExtension extension(parsed->options);
    const auto result = st::replay_nav(role.nav(), cfg);
    EXPECT_TRUE(result);
    if (!result) return out;
    for (const auto& day : result->days) out.push_back(day.construction.banded_names);
    return out;
  };
  // A wide band (b .5, the maximum) so that many gaps sit near its edge.
  const auto uniform = banded(v6_args({"--band-b", "0.5", "--band-exponent", "0"}), 0.0);
  const auto scaled = banded(v6_args({"--band-b", "0.5"}), 1.0 / 3.0);
  ASSERT_EQ(uniform.size(), scaled.size());
  ASSERT_FALSE(uniform.empty());
  usize differing = 0, dusted = 0;
  for (usize t = 0; t < uniform.size(); ++t) {
    differing += uniform[t] != scaled[t] ? 1U : 0U;
    dusted += uniform[t];
  }
  EXPECT_GT(dusted, 0U);
  EXPECT_GT(differing, 0U);
}
TEST(NavV7Hook, ParseRefusesV6ParametersWithoutTheRuleAndOutOfRangeExponents) {
  EXPECT_FALSE(parse_v7({"nav", "--band-exponent", "0", "--output", "x"}));
  EXPECT_FALSE(parse_v7(v6_args({"--band-exponent", "2"})));
  EXPECT_FALSE(parse_v7(v6_args({"--band-exponent", "0", "--band-exponent", "0"})));
  EXPECT_FALSE(parse_v7(v6_args({"--rate", "per-name-v1"})));
  EXPECT_TRUE(parse_v7(v6_args({"--band-exponent", "1"})));
  std::vector<std::string> claimed{"nav", "--band-exponent", "0"};
  std::vector<char*> argv;
  for (auto& a : claimed) argv.push_back(a.data());
  EXPECT_TRUE(v7::claims_nav_args(static_cast<int>(argv.size()), argv.data()));
}
} // namespace
