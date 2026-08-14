#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/analytics/scenario_grid.hpp"
#include "backtest/strategy_pipeline.hpp"

namespace atx::vol {
namespace {

[[nodiscard]] StrategyConstituent constituent(std::string symbol, std::uint32_t uid,
                                              double weight) {
  return StrategyConstituent{std::move(symbol), uid, weight};
}

[[nodiscard]] OptionOpportunity opportunity(std::string symbol, std::uint64_t id, std::uint32_t uid,
                                            double edge, double vega_per_contract,
                                            double max_abs_contracts) {
  OptionOpportunity value;
  value.symbol = std::move(symbol);
  value.position_id = id;
  value.contract = OptionContract{uid, 100.0, 0.25, Side::Call};
  value.multiplier = 100.0;
  value.valuation_edge = edge;
  value.vega_per_contract = vega_per_contract;
  value.max_abs_contracts = max_abs_contracts;
  return value;
}

[[nodiscard]] NamedPosition named_position(std::string symbol, std::uint64_t id, std::uint32_t uid,
                                           double qty) {
  NamedPosition value;
  value.symbol = std::move(symbol);
  value.position = Position{id, OptionContract{uid, 100.0, 0.25, Side::Call}, qty, 100.0};
  return value;
}

[[nodiscard]] AmericanGreeks greeks(double price, double delta, double gamma, double vega,
                                    double theta, double vanna = 0.0, double volga = 0.0,
                                    double rho = 0.0, double charm = 0.0) {
  AmericanGreeks value;
  value.price = price;
  value.delta = delta;
  value.gamma = gamma;
  value.vega = vega;
  value.theta = theta;
  value.vanna = vanna;
  value.volga = volga;
  value.rho = rho;
  value.charm = charm;
  return value;
}

TEST(StrategyPipelineUniverse, StrictAndDirtyProvenanceFailClosedOnPointInTimeViolations) {
  StrategyUniverseProvenance strict;
  strict.mode = StrategyUniverseMode::StrictConstituents;
  strict.index_symbol = "SPX";
  strict.index_uid = 1u;
  strict.constituents = {constituent("AAPL", 2u, 0.6), constituent("MSFT", 3u, 0.4)};
  strict.effective_ts_ns = 100;
  strict.knowledge_ts_ns = 90;
  strict.source = "vendor-pit";
  strict.source_fingerprint = 42u;

  EXPECT_TRUE(validate_strategy_universe(strict, 100));

  StrategyUniverseProvenance future = strict;
  future.knowledge_ts_ns = 101;
  const Status future_status = validate_strategy_universe(future, 100);
  ASSERT_FALSE(future_status);
  EXPECT_EQ(future_status.error().code(), ErrorCode::InvalidArgument);

  StrategyUniverseProvenance strict_with_proxy = strict;
  strict_with_proxy.proxies.push_back(StrategyProxyMapping{"AAPL", "QQQ", 4u, 1.1});
  EXPECT_FALSE(validate_strategy_universe(strict_with_proxy, 100));

  StrategyUniverseProvenance dirty = strict;
  dirty.mode = StrategyUniverseMode::DirtyProxyBasket;
  dirty.proxies = {StrategyProxyMapping{"AAPL", "QQQ", 4u, 1.1},
                   StrategyProxyMapping{"MSFT", "XLK", 5u, 0.9}};
  EXPECT_TRUE(validate_strategy_universe(dirty, 100));

  dirty.proxies[1].proxy_uid = dirty.proxies[0].proxy_uid;
  EXPECT_FALSE(validate_strategy_universe(dirty, 100));

  dirty = strict;
  dirty.mode = StrategyUniverseMode::DirtyProxyBasket;
  dirty.proxies = {StrategyProxyMapping{"NOT_A_MEMBER", "QQQ", 4u, 1.1}};
  EXPECT_FALSE(validate_strategy_universe(dirty, 100));
}

TEST(StrategyPipelineConstruction,
     RanksLongShortSleevesAndBalancesVegaWithinDeterministicNameCaps) {
  const std::vector<OptionOpportunity> input = {
      opportunity("D", 4u, 14u, -2.0, 100.0, 20.0),
      opportunity("B", 2u, 12u, 2.0, 200.0, 20.0),
      opportunity("C", 3u, 13u, -4.0, 50.0, 20.0),
      opportunity("A", 1u, 11u, 3.0, 100.0, 20.0),
      opportunity("UNSELECTED", 5u, 15u, 0.25, 100.0, 20.0),
  };
  LongShortVolatilityConfig config;
  config.n_long = 2u;
  config.n_short = 2u;
  config.target_gross_vega_per_sleeve = 1'000.0;
  config.max_abs_contracts_per_name = 6.0;
  config.min_abs_edge = 1.0;

  const auto built = construct_systematic_long_short_volatility(input, config);
  ASSERT_TRUE(built) << built.error().message();
  ASSERT_EQ(built->positions.size(), 4u);
  EXPECT_EQ(built->positions[0].symbol, "A");
  EXPECT_EQ(built->positions[1].symbol, "B");
  EXPECT_EQ(built->positions[2].symbol, "C");
  EXPECT_EQ(built->positions[3].symbol, "D");

  EXPECT_DOUBLE_EQ(built->positions[0].position.qty, 4.5);
  EXPECT_DOUBLE_EQ(built->positions[1].position.qty, 2.25);
  EXPECT_DOUBLE_EQ(built->positions[2].position.qty, -6.0);
  EXPECT_DOUBLE_EQ(built->positions[3].position.qty, -6.0);
  EXPECT_DOUBLE_EQ(built->long_gross_vega, 900.0);
  EXPECT_DOUBLE_EQ(built->short_gross_vega, 900.0);
  EXPECT_DOUBLE_EQ(built->net_vega, 0.0);
  EXPECT_TRUE(built->capacity_limited);

  std::vector<OptionOpportunity> reversed(input.rbegin(), input.rend());
  const auto second = construct_systematic_long_short_volatility(reversed, config);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->positions, built->positions);

  reversed.push_back(opportunity("a", 99u, 99u, 5.0, 100.0, 2.0));
  const auto duplicate = construct_systematic_long_short_volatility(reversed, config);
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().code(), ErrorCode::InvalidArgument);
}

TEST(StrategyPipelineRisk, AggregatesRiskAndSupportsClampOrReject) {
  const std::vector<PositionRiskInput> inputs = {
      PositionRiskInput{Position{2u, OptionContract{2u, 100.0, 0.25, Side::Put}, -1.0, 100.0},
                        greeks(3.0, -0.4, 0.02, 2.0, -0.1)},
      PositionRiskInput{Position{1u, OptionContract{1u, 100.0, 0.25, Side::Call}, 2.0, 100.0},
                        greeks(5.0, 0.5, 0.01, 3.0, -0.2)},
  };
  const auto risk = aggregate_strategy_risk(inputs);
  ASSERT_TRUE(risk);
  EXPECT_DOUBLE_EQ(risk->delta, 140.0);
  EXPECT_DOUBLE_EQ(risk->gamma, 0.0);
  EXPECT_DOUBLE_EQ(risk->vega, 400.0);
  EXPECT_DOUBLE_EQ(risk->theta, -30.0);
  EXPECT_DOUBLE_EQ(risk->gross_notional, 1'300.0);

  ScalarRiskLimits limits;
  limits.max_abs_delta = 70.0;
  limits.max_gross_notional = 1'000.0;
  limits.max_worst_scenario_loss = 100.0;
  limits.action = ScalarRiskBreachAction::Clamp;
  const std::vector<double> scenarios = {-200.0, 40.0};
  const auto clamped = apply_scalar_risk_overlay(*risk, scenarios, limits);
  ASSERT_TRUE(clamped);
  EXPECT_DOUBLE_EQ(clamped->scale, 0.5);
  EXPECT_DOUBLE_EQ(clamped->risk.delta, 70.0);
  EXPECT_DOUBLE_EQ(clamped->risk.vega, 200.0);
  ASSERT_EQ(clamped->scenario_pnl.size(), 2u);
  EXPECT_DOUBLE_EQ(clamped->scenario_pnl[0], -100.0);
  EXPECT_TRUE(clamped->constrained());

  limits.action = ScalarRiskBreachAction::Reject;
  const auto rejected = apply_scalar_risk_overlay(*risk, scenarios, limits);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), ErrorCode::Unavailable);

  limits.action = ScalarRiskBreachAction::Clamp;
  limits.max_abs_delta = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(apply_scalar_risk_overlay(*risk, scenarios, limits));
}

TEST(StrategyPipelineScenario,
     AppliesIndexConditionedUidSpecificSpotResidualAndVolShocksDeterministically) {
  const ScenarioRiskInput index{
      Position{20u, OptionContract{1u, 100.0, 0.25, Side::Put}, -1.0, 100.0}, 100.0,
      greeks(4.0, -0.5, 0.02, 5.0, -0.1, 0.3, 2.0, 0.4, 0.2)};
  const ScenarioRiskInput component{
      Position{10u, OptionContract{2u, 50.0, 0.25, Side::Call}, 2.0, 100.0}, 50.0,
      greeks(2.0, 0.4, 0.01, 3.0, -0.2, 0.1, 1.0, 0.2, 0.05)};
  const std::vector<ScenarioRiskInput> inputs = {index, component};

  ConditionalComponentScenario scenario;
  scenario.index_uid = 1u;
  scenario.index_spot_pct = -0.10;
  scenario.index_vol_bump = 0.04;
  scenario.dt = 1.0 / 365.25;
  scenario.dr = 0.001;
  scenario.components = {ComponentShockModel{2u, 1.5, 0.01, 0.5, 0.02}};

  const auto result = conditional_component_scenario_pnl(inputs, scenario);
  ASSERT_TRUE(result) << result.error().message();
  ASSERT_EQ(result->by_uid.size(), 2u);
  EXPECT_EQ(result->by_uid[0].uid, 1u);
  EXPECT_EQ(result->by_uid[1].uid, 2u);

  const double index_leg =
      scenario_taylor_leg(index.greeks_per_share, -10.0, 0.04, scenario.dt, scenario.dr) *
      index.position.qty * index.position.multiplier;
  const double component_leg =
      scenario_taylor_leg(component.greeks_per_share, -7.0, 0.04, scenario.dt, scenario.dr) *
      component.position.qty * component.position.multiplier;
  EXPECT_DOUBLE_EQ(result->by_uid[0].pnl, index_leg);
  EXPECT_DOUBLE_EQ(result->by_uid[1].pnl, component_leg);
  EXPECT_DOUBLE_EQ(result->total_pnl, index_leg + component_leg);

  std::vector<ScenarioRiskInput> reversed(inputs.rbegin(), inputs.rend());
  const auto second = conditional_component_scenario_pnl(reversed, scenario);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->by_uid, result->by_uid);
  EXPECT_DOUBLE_EQ(second->total_pnl, result->total_pnl);

  scenario.components.push_back(ComponentShockModel{2u, 1.0, 0.0, 1.0, 0.0});
  EXPECT_FALSE(conditional_component_scenario_pnl(inputs, scenario));
}

TEST(StrategyPipelineIntent, GeneratesVersionedResearchOnlyTargetMinusCurrentDeltasInStableOrder) {
  const std::vector<NamedPosition> target = {
      named_position("C", 30u, 3u, 2.0),
      named_position("A", 10u, 1u, 5.0),
  };
  const std::vector<NamedPosition> current = {
      named_position("B", 20u, 2u, -3.0),
      named_position("A", 10u, 1u, 2.0),
  };
  const std::vector<HedgeTarget> hedges = {
      HedgeTarget{3u, 50.0, 20.0},
      HedgeTarget{1u, -10.0, 5.0},
  };

  AlgoParameters algo;
  algo.style = AlgoStyle::Adaptive;
  algo.max_participation = ParticipationRate{0.15};
  algo.horizon = TimeInForceSeconds{300u};
  algo.limit_offset = LimitOffsetBps{2.5};

  const auto intent = make_basket_order_intent(1234u, 1'000, target, current, hedges, algo);
  ASSERT_TRUE(intent) << intent.error().message();
  EXPECT_EQ(intent->schema_version, kBasketOrderIntentSchemaVersion);
  EXPECT_EQ(intent->disposition, IntentDisposition::ResearchOnly);
  ASSERT_EQ(intent->option_orders.size(), 3u);
  EXPECT_EQ(intent->option_orders[0].position_id, 10u);
  EXPECT_DOUBLE_EQ(intent->option_orders[0].quantity_delta, 3.0);
  EXPECT_EQ(intent->option_orders[1].position_id, 20u);
  EXPECT_DOUBLE_EQ(intent->option_orders[1].quantity_delta, 3.0);
  EXPECT_EQ(intent->option_orders[2].position_id, 30u);
  EXPECT_DOUBLE_EQ(intent->option_orders[2].quantity_delta, 2.0);

  ASSERT_EQ(intent->hedges.size(), 2u);
  EXPECT_EQ(intent->hedges[0].schema_version, kHedgeInstructionSchemaVersion);
  EXPECT_EQ(intent->hedges[0].uid, 1u);
  EXPECT_DOUBLE_EQ(intent->hedges[0].shares_to_trade, 15.0);
  EXPECT_EQ(intent->hedges[1].uid, 3u);
  EXPECT_DOUBLE_EQ(intent->hedges[1].shares_to_trade, -30.0);

  const std::vector<NamedPosition> target_reversed(target.rbegin(), target.rend());
  const std::vector<NamedPosition> current_reversed(current.rbegin(), current.rend());
  const std::vector<HedgeTarget> hedges_reversed(hedges.rbegin(), hedges.rend());
  const auto second = make_basket_order_intent(1234u, 1'000, target_reversed, current_reversed,
                                               hedges_reversed, algo);
  ASSERT_TRUE(second);
  EXPECT_EQ(second->option_orders, intent->option_orders);
  EXPECT_EQ(second->hedges, intent->hedges);

  std::vector<NamedPosition> duplicate = target;
  duplicate.push_back(target.front());
  EXPECT_FALSE(make_basket_order_intent(1234u, 1'000, duplicate, current, hedges, algo));
  EXPECT_FALSE(make_basket_order_intent(1234u, 0, target, current, hedges, algo));
}

} // namespace
} // namespace atx::vol
