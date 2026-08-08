#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/vol/historical_projection.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/vol_curve.hpp"

namespace {

using namespace atx::vol;
namespace time = atx::core::time;

std::int64_t timestamp(int year, unsigned month, unsigned day) {
  return time::timestamp_from_utc(year, month, day, 19, 55, 0, 0).unix_nanos();
}

PricedSurface make_surface(std::uint32_t uid, double spot, std::int64_t now) {
  CurveSurface curves;
  std::vector<SliceContext> context;
  std::uint16_t expiry_id = 0;
  for (const double term : {0.05, 0.15, 0.25, 0.50, 1.00}) {
    EssviParams parameters{};
    parameters.theta = 0.04 + 0.01 * term;
    parameters.phi = 1.2;
    parameters.rho = -0.25;
    parameters.psi = 0.5;
    parameters.p = 0.5;
    parameters.lambda = 0.5;
    parameters.T = term;
    parameters.F = spot;
    parameters.expiry_id = expiry_id++;
    curves.push(std::make_unique<EssviCurve>(parameters, std::exp(-0.04 * term)));
    context.push_back(SliceContext{term, spot, 0.0, 0.02, 100, 7});
  }
  PricingContext pricing;
  pricing.S = spot;
  pricing.r = 0.04;
  pricing.now_ts_ns = now;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = uid;
  return std::move(*PricedSurface::create(std::move(curves), std::move(context), pricing));
}

RelativeOptionPosition position(std::uint32_t uid, Side side, double delta, double quantity) {
  RelativeOptionPosition result;
  result.option.uid = uid;
  result.option.side = side;
  result.option.maturity = ProjectedMaturitySpec::months(3);
  result.option.strike = ProjectedStrikeSpec::delta(delta);
  result.option.multiplier = 100.0;
  result.quantity = quantity;
  return result;
}

TEST(HistoricalProjection, BatchMatchesScalarAndIsThreadInvariant) {
  const std::vector<RelativeOptionPosition> positions = {position(1u, Side::Call, 0.40, 2.0),
                                                         position(1u, Side::Put, 0.40, -0.75)};
  auto prepared = PreparedHistoricalProjection::create(positions);
  ASSERT_TRUE(prepared);
  EXPECT_NE(prepared->fingerprint(), 0u);

  std::vector<PricedSurface> surfaces_a;
  surfaces_a.push_back(make_surface(1u, 100.0, timestamp(2026, 1, 2)));
  std::vector<PricedSurface> surfaces_b;
  surfaces_b.push_back(make_surface(1u, 112.0, timestamp(2026, 1, 5)));
  const std::vector<const PricedSurface *> pointers_a{&surfaces_a[0]};
  const std::vector<const PricedSurface *> pointers_b{&surfaces_b[0]};
  auto set_a = SurfaceSet::create(pointers_a);
  auto set_b = SurfaceSet::create(pointers_b);
  ASSERT_TRUE(set_a && set_b);
  const std::vector<HistoricalProjectionScenario> scenarios = {{timestamp(2026, 1, 2), &*set_a},
                                                               {timestamp(2026, 1, 5), &*set_b}};

  std::vector<HistoricalProjectionFrame> serial_frames(2);
  std::vector<HistoricalProjectionFrame> parallel_frames(2);
  std::vector<ProjectedOption> serial_legs(4);
  std::vector<ProjectedOption> parallel_legs(4);
  ASSERT_TRUE(prepared->evaluate_into(scenarios, serial_frames, serial_legs,
                                      HistoricalProjectionConfig{true, 1.0e-7, 1u}));
  ASSERT_TRUE(prepared->evaluate_into(scenarios, parallel_frames, parallel_legs,
                                      HistoricalProjectionConfig{true, 1.0e-7, 4u}));
  EXPECT_EQ(serial_frames, parallel_frames);
  EXPECT_EQ(serial_legs, parallel_legs);

  for (std::size_t scenario = 0; scenario < scenarios.size(); ++scenario) {
    double expected_value = 0.0;
    double expected_vega = 0.0;
    for (std::size_t leg = 0; leg < positions.size(); ++leg) {
      auto scalar =
          project_option_contract(*scenarios[scenario].surfaces->find(1u), positions[leg].option);
      ASSERT_TRUE(scalar);
      EXPECT_EQ(serial_legs[scenario * positions.size() + leg], *scalar);
      const double scale = positions[leg].quantity * positions[leg].option.multiplier;
      expected_value += scale * scalar->model_mark;
      expected_vega += scale * scalar->greeks.vega;
    }
    EXPECT_DOUBLE_EQ(serial_frames[scenario].value, expected_value);
    EXPECT_DOUBLE_EQ(serial_frames[scenario].vega, expected_vega);
    EXPECT_EQ(serial_frames[scenario].n_ok, 2u);
    EXPECT_EQ(serial_frames[scenario].n_failed, 0u);
    EXPECT_NE(serial_frames[scenario].definition_fingerprint, 0u);
  }
}

TEST(HistoricalProjection, MissingSurfaceIsExplicitAndPoisonsAggregate) {
  const std::vector<RelativeOptionPosition> positions = {position(9u, Side::Call, 0.40, 1.0)};
  auto prepared = PreparedHistoricalProjection::create(positions);
  ASSERT_TRUE(prepared);
  std::vector<PricedSurface> surfaces;
  surfaces.push_back(make_surface(1u, 100.0, timestamp(2026, 1, 2)));
  const std::vector<const PricedSurface *> pointers{&surfaces[0]};
  auto set = SurfaceSet::create(pointers);
  ASSERT_TRUE(set);
  const std::vector<HistoricalProjectionScenario> scenarios = {{timestamp(2026, 1, 2), &*set}};
  std::vector<HistoricalProjectionFrame> frames(1);
  std::vector<ProjectedOption> legs(1);
  ASSERT_TRUE(prepared->evaluate_into(scenarios, frames, legs));
  EXPECT_EQ(legs[0].status, OptionProjectionStatus::SurfaceUnavailable);
  EXPECT_EQ(frames[0].n_failed, 1u);
  EXPECT_TRUE(std::isnan(frames[0].value));
  EXPECT_EQ(frames[0].definition_fingerprint, 0u);
}

TEST(HistoricalProjection, ScenarioTimestampMustMatchSurfaceValuation) {
  const std::vector<RelativeOptionPosition> positions = {position(1u, Side::Call, 0.40, 1.0)};
  auto prepared = PreparedHistoricalProjection::create(positions);
  ASSERT_TRUE(prepared);
  const std::int64_t valuation = timestamp(2026, 1, 2);
  std::vector<PricedSurface> surfaces;
  surfaces.push_back(make_surface(1u, 100.0, valuation));
  const std::vector<const PricedSurface *> pointers{&surfaces[0]};
  auto set = SurfaceSet::create(pointers);
  ASSERT_TRUE(set);
  const std::vector<HistoricalProjectionScenario> scenarios = {{valuation + 1, &*set}};
  std::vector<HistoricalProjectionFrame> frames(1);
  std::vector<ProjectedOption> legs(1);
  ASSERT_TRUE(prepared->evaluate_into(scenarios, frames, legs));
  EXPECT_EQ(legs[0].status, OptionProjectionStatus::Ok);
  EXPECT_EQ(frames[0].n_failed, 1u);
  EXPECT_TRUE(std::isnan(frames[0].value));
}

// [proj] I2: evaluate_into's default (Configured) must reproduce the SAME
// leg the underlying PreparedOptionProjection would have produced under
// Configured -- non-VaR callers see no change. An explicit ColdReference
// override must instead reproduce the independent scalar ColdReference
// oracle exactly, on an accelerator-backed (RepresentativeFast) surface
// where the two routes are genuinely different code paths.
TEST(HistoricalProjection,
     EvaluateIntoDefaultConfiguredVsExplicitColdReferenceMatchIndependentOracles) {
  const std::uint32_t uid = 1u;
  PricedSurface source = make_surface(uid, 100.0, timestamp(2026, 1, 2));
  auto prepared_fast = std::move(source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(prepared_fast) << (prepared_fast ? std::string{} : prepared_fast.error().to_string());
  const PricedSurface fast = std::move(*prepared_fast);
  const std::vector<const PricedSurface *> pointers{&fast};
  auto set = SurfaceSet::create(pointers);
  ASSERT_TRUE(set);

  const std::vector<RelativeOptionPosition> positions = {position(uid, Side::Call, 0.40, 1.0)};
  auto prepared = PreparedHistoricalProjection::create(positions);
  ASSERT_TRUE(prepared);
  const std::vector<HistoricalProjectionScenario> scenarios = {{fast.pricing().now_ts_ns, &*set}};

  std::vector<HistoricalProjectionFrame> configured_frames(1);
  std::vector<ProjectedOption> configured_legs(1);
  ASSERT_TRUE(prepared->evaluate_into(scenarios, configured_frames, configured_legs));
  ASSERT_EQ(configured_legs[0].status, OptionProjectionStatus::Ok);

  std::vector<HistoricalProjectionFrame> cold_frames(1);
  std::vector<ProjectedOption> cold_legs(1);
  ASSERT_TRUE(prepared->evaluate_into(scenarios, cold_frames, cold_legs,
                                      HistoricalProjectionConfig{}, QueryExecution::ColdReference));
  ASSERT_EQ(cold_legs[0].status, OptionProjectionStatus::Ok);

  OptionProjectionConfig cold_check;
  cold_check.output = OptionProjectionOutput::FullGreeks;
  cold_check.query_execution = QueryExecution::ColdReference;
  auto cold_oracle = project_option_contract(fast, positions[0].option, cold_check);
  ASSERT_TRUE(cold_oracle) << (cold_oracle ? std::string{} : cold_oracle.error().to_string());
  EXPECT_EQ(cold_legs[0], *cold_oracle)
      << "explicit ColdReference must reach OptionProjectionConfig::query_execution, not silently "
         "inherit the surface's prepared accelerator tier ([proj] I2)";

  OptionProjectionConfig configured_check;
  configured_check.output = OptionProjectionOutput::FullGreeks;
  configured_check.query_execution = QueryExecution::Configured;
  auto configured_oracle = project_option_contract(fast, positions[0].option, configured_check);
  ASSERT_TRUE(configured_oracle);
  EXPECT_EQ(configured_legs[0], *configured_oracle)
      << "default execution (Configured) must keep non-VaR callers unchanged";

  // Sanity: this fixture genuinely exercises the fast tier under Configured
  // (otherwise the two EXPECT_EQ above would be vacuous -- Configured and
  // ColdReference would coincide even without the fix).
  EXPECT_EQ(fast.query_pricing_route(configured_legs[0].definition.contract.K,
                                     configured_legs[0].definition.contract.T, Side::Call),
            QueryPricingRoute::RepresentativeFast);
}

TEST(HistoricalProjection, VarUsesNearestRankAndInclusiveTail) {
  std::vector<HistoricalProjectionFrame> frames(6);
  for (std::size_t i = 0; i < 5; ++i) {
    frames[i].value = 90.0 + 5.0 * static_cast<double>(i);
    frames[i].n_ok = 1u;
    frames[i].definition_fingerprint = i + 1u;
  }
  frames[5].value = 1.0;
  frames[5].n_failed = 1u;
  auto risk = projected_historical_var(frames, 100.0, 0.80);
  ASSERT_TRUE(risk);
  EXPECT_EQ(risk->n_scenarios, 5u);
  EXPECT_DOUBLE_EQ(risk->value_at_risk, 5.0);
  EXPECT_DOUBLE_EQ(risk->expected_shortfall, 7.5);
  EXPECT_FALSE(projected_historical_var(frames, 100.0, 1.0));
}

} // namespace
