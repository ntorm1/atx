#include <bit>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/replay.hpp"
#include "atx/engine/book/replay_cost.hpp"

namespace atx_test_l8_e2e_replay_cost {
namespace book = atx::engine::book;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;

inline Panel close_panel(atx::usize dates, atx::usize instruments, std::vector<atx::f64> close) {
  return Panel::create(dates, instruments, {"close"}, {std::move(close)}, {}).value();
}

inline std::vector<atx::i64> day_axis(atx::usize count) {
  std::vector<atx::i64> times;
  for (atx::usize i = 0; i < count; ++i) times.push_back(static_cast<atx::i64>(i) * kDay);
  return times;
}

inline book::ReplayConfig base_config() {
  book::ReplayConfig cfg;
  cfg.initial_nav = 1000.0;
  cfg.execution_delay_periods = 0;
  return cfg;
}

// Deterministic drifting prices, 3 names x 8 periods.
inline Panel drifting_panel() {
  std::vector<atx::f64> close;
  for (atx::usize d = 0; d < 8; ++d) {
    for (atx::usize i = 0; i < 3; ++i) {
      close.push_back(50.0 + 7.0 * static_cast<atx::f64>(i) +
                      std::sin(static_cast<atx::f64>(d * 3 + i)) * 3.0);
    }
  }
  return close_panel(8, 3, std::move(close));
}

inline bool same_bits(atx::f64 a, atx::f64 b) {
  return std::bit_cast<atx::u64>(a) == std::bit_cast<atx::u64>(b);
}

inline void expect_identity(const book::ReplayResult &result) {
  for (const auto &row : result.intervals) {
    EXPECT_NEAR(row.cash + row.assets, row.nav, 1.0e-9);
    EXPECT_NEAR(row.pretrade_nav + row.gross_pnl - row.trade_cost - row.borrow_cost, row.nav,
                1.0e-9);
  }
}

inline std::vector<book::LiquidityRow> uniform_liquidity(atx::usize cells, atx::f64 adv) {
  return std::vector<book::LiquidityRow>(cells, book::LiquidityRow{adv, 0.02, 5.0});
}


TEST(BookReplayCost, FlatBpsCostIsBitIdenticalToTradeBps) {
  const auto panel = drifting_panel();
  const std::vector<atx::usize> schedule{0, 2, 5};
  const std::vector<atx::f64> targets{0.4, -0.3, 0.2, -0.2, 0.5, 0.1, 0.0, 0.3, -0.6};
  auto legacy_cfg = base_config();
  legacy_cfg.trade_bps = 12.5;
  const auto legacy =
      book::replay_scheduled_targets(panel, day_axis(8), schedule, targets, legacy_cfg);
  ASSERT_TRUE(legacy.has_value()) << legacy.error().message();

  const auto flat = book::FlatBpsCost::create(12.5).value();
  auto model_cfg = base_config();
  model_cfg.cost_model = &flat;
  const auto modeled =
      book::replay_scheduled_targets(panel, day_axis(8), schedule, targets, model_cfg);
  ASSERT_TRUE(modeled.has_value()) << modeled.error().message();

  ASSERT_EQ(modeled->intervals.size(), legacy->intervals.size());
  for (atx::usize k = 0; k < legacy->intervals.size(); ++k) {
    const auto &a = legacy->intervals[k];
    const auto &b = modeled->intervals[k];
    EXPECT_TRUE(same_bits(a.nav, b.nav)) << k;
    EXPECT_TRUE(same_bits(a.cash, b.cash)) << k;
    EXPECT_TRUE(same_bits(a.trade_cost, b.trade_cost)) << k;
    EXPECT_TRUE(same_bits(a.traded_dollars, b.traded_dollars)) << k;
    EXPECT_TRUE(same_bits(a.gross_pnl, b.gross_pnl)) << k;
  }
  ASSERT_EQ(modeled->trades.size(), legacy->trades.size());
  EXPECT_TRUE(same_bits(modeled->final_nav, legacy->final_nav));
  EXPECT_EQ(modeled->open_working_orders, 0U);
}

TEST(BookReplayCost, SqrtImpactCostIsMonotoneInParticipation) {
  const auto model = book::SqrtImpactCost::create({1.0, 0.5}, 1.0).value();
  const book::LiquidityRow row{1.0e6, 0.02, 5.0};
  atx::f64 previous_fraction = -1.0;
  atx::f64 previous_cost = -1.0;
  for (const auto dollars : {1.0e2, 1.0e3, 1.0e4, 5.0e4, 1.0e5, 5.0e5}) {
    const auto charged = model.cost(0, 0, -dollars, row);
    EXPECT_DOUBLE_EQ(charged.filled_dollars, -dollars);
    EXPECT_GT(charged.cost_dollars, previous_cost);
    const auto fraction = charged.cost_dollars / dollars;
    EXPECT_GT(fraction, previous_fraction);
    previous_fraction = fraction;
    previous_cost = charged.cost_dollars;
  }
  // Exact formula at 1% participation: 5 bps spread + 0.02 * sqrt(0.01).
  const auto at_one_pct = model.cost(0, 0, 1.0e4, row);
  EXPECT_NEAR(at_one_pct.cost_dollars, 1.0e4 * (5.0e-4 + 0.02 * 0.1), 1.0e-9);
}

TEST(BookReplayCost, UnusableLiquidityFillsNothing) {
  const auto model = book::SqrtImpactCost::create({1.0, 0.5}, 0.1).value();
  const auto nan = std::numeric_limits<atx::f64>::quiet_NaN();
  for (const auto &row : {book::LiquidityRow{0.0, 0.02, 5.0}, book::LiquidityRow{nan, 0.02, 5.0},
                          book::LiquidityRow{1.0e6, nan, 5.0}}) {
    const auto charged = model.cost(0, 0, 100.0, row);
    EXPECT_EQ(charged.filled_dollars, 0.0);
    EXPECT_EQ(charged.cost_dollars, 0.0);
  }
}

TEST(BookReplayCost, CreateRejectsInvalidParameters) {
  EXPECT_FALSE(book::FlatBpsCost::create(-1.0).has_value());
  EXPECT_FALSE(book::FlatBpsCost::create(std::numeric_limits<atx::f64>::infinity()).has_value());
  EXPECT_FALSE(book::SqrtImpactCost::create({-1.0, 0.5}, 0.1).has_value());
  EXPECT_FALSE(book::SqrtImpactCost::create({1.0, 0.0}, 0.1).has_value());
  EXPECT_FALSE(book::SqrtImpactCost::create({1.0, 0.5}, 0.0).has_value());
  EXPECT_TRUE(book::SqrtImpactCost::create({1.0, 0.5},
                                           std::numeric_limits<atx::f64>::infinity())
                  .has_value());
}

TEST(BookReplayCost, ParticipationCapPartiallyFillsAndCarriesResidualForward) {
  // One name at a flat price: the only P&L is cost, and fills are exact.
  const auto panel = close_panel(8, 1, std::vector<atx::f64>(8, 100.0));
  const auto liquidity = uniform_liquidity(8, 1000.0);
  const auto model = book::SqrtImpactCost::create({0.0, 0.5}, 0.25).value(); // 250$/day.
  auto cfg = base_config();
  cfg.cost_model = &model;
  cfg.liquidity = liquidity;
  const std::vector<atx::usize> schedule{0};
  const std::vector<atx::f64> targets{0.9}; // 900$ goal == 3.6 days of cap.
  const auto result = book::replay_scheduled_targets(panel, day_axis(8), schedule, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(result->trades.size(), 4U);
  EXPECT_DOUBLE_EQ(result->trades[0].dollar_delta, 250.0);
  EXPECT_DOUBLE_EQ(result->trades[1].dollar_delta, 250.0);
  EXPECT_DOUBLE_EQ(result->trades[2].dollar_delta, 250.0);
  EXPECT_DOUBLE_EQ(result->trades[3].dollar_delta, 150.0);
  for (atx::usize k = 0; k < 4; ++k) {
    EXPECT_EQ(result->trades[k].period, k);
    EXPECT_EQ(result->trades[k].decision_period, 0U);
  }
  EXPECT_DOUBLE_EQ(result->final_tri_units[0], 9.0); // Goal units fixed at decision.
  EXPECT_EQ(result->open_working_orders, 0U);
  EXPECT_EQ(result->effective_rebalances, 1U);
  // Half-spread 5 bps on every filled dollar.
  EXPECT_NEAR(result->intervals[0].trade_cost, 250.0 * 5.0e-4, 1.0e-12);
  EXPECT_NEAR(result->intervals[3].trade_cost, 150.0 * 5.0e-4, 1.0e-12);
  EXPECT_DOUBLE_EQ(result->intervals[4].traded_dollars, 0.0);
  expect_identity(*result);
}

TEST(BookReplayCost, NewDecisionReplacesTheOpenWorkingOrder) {
  const auto panel = close_panel(6, 1, std::vector<atx::f64>(6, 100.0));
  const auto liquidity = uniform_liquidity(6, 1000.0);
  const auto model = book::SqrtImpactCost::create({0.0, 0.5}, 0.1).value(); // 100$/day.
  auto cfg = base_config();
  cfg.cost_model = &model;
  cfg.liquidity = liquidity;
  const std::vector<atx::usize> schedule{0, 2};
  const std::vector<atx::f64> targets{0.9, 0.0};
  const auto result = book::replay_scheduled_targets(panel, day_axis(6), schedule, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  // +100, +100, then the flat target unwinds 200$ at 100$/day.
  ASSERT_EQ(result->trades.size(), 4U);
  EXPECT_DOUBLE_EQ(result->trades[2].dollar_delta, -100.0);
  EXPECT_EQ(result->trades[2].decision_period, 2U);
  EXPECT_DOUBLE_EQ(result->trades[3].dollar_delta, -100.0);
  EXPECT_DOUBLE_EQ(result->final_tri_units[0], 0.0);
  EXPECT_EQ(result->open_working_orders, 0U);
  expect_identity(*result);
}

TEST(BookReplayCost, NetOfImpactPnlIsBelowFrictionless) {
  const auto panel = drifting_panel();
  const auto liquidity = uniform_liquidity(24, 5000.0);
  const auto model = book::SqrtImpactCost::create({0.8, 0.5},
                                                  std::numeric_limits<atx::f64>::infinity())
                         .value();
  const std::vector<atx::usize> schedule{0, 3, 6};
  const std::vector<atx::f64> targets{0.3, -0.3, 0.3, -0.3, 0.3, -0.3, 0.2, 0.2, -0.4};
  const auto frictionless =
      book::replay_scheduled_targets(panel, day_axis(8), schedule, targets, base_config());
  auto cfg = base_config();
  cfg.cost_model = &model;
  cfg.liquidity = liquidity;
  const auto costly = book::replay_scheduled_targets(panel, day_axis(8), schedule, targets, cfg);
  ASSERT_TRUE(frictionless.has_value());
  ASSERT_TRUE(costly.has_value()) << costly.error().message();
  EXPECT_LT(costly->final_nav, frictionless->final_nav);
  atx::f64 total_cost = 0.0;
  for (const auto &row : costly->intervals) total_cost += row.trade_cost;
  EXPECT_GT(total_cost, 0.0);
  expect_identity(*costly);
}

TEST(BookReplayCost, RejectsAmbiguousOrMisShapedConfiguration) {
  const auto panel = close_panel(4, 1, std::vector<atx::f64>(4, 100.0));
  const std::vector<atx::usize> schedule{0};
  const std::vector<atx::f64> targets{0.5};
  const auto flat = book::FlatBpsCost::create(5.0).value();
  auto both = base_config();
  both.trade_bps = 5.0;
  both.cost_model = &flat;
  EXPECT_FALSE(book::replay_scheduled_targets(panel, day_axis(4), schedule, targets, both));

  const auto impact = book::SqrtImpactCost::create({1.0, 0.5}, 0.1).value();
  const auto short_liquidity = uniform_liquidity(3, 1000.0);
  auto misshaped = base_config();
  misshaped.cost_model = &impact;
  misshaped.liquidity = short_liquidity;
  const auto rejected =
      book::replay_scheduled_targets(panel, day_axis(4), schedule, targets, misshaped);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_NE(rejected.error().message().find("liquidity"), std::string::npos);
}

} // namespace atx_test_l8_e2e_replay_cost
