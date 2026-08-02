#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "atx/core/datetime.hpp"
#include "atx/vol/contract_projection.hpp"
#include "atx/vol/portfolio_pricer.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/strategy.hpp"
#include "atx/vol/surface_parity.hpp"
#include "atx/vol/vol_curve.hpp"
#include "atx/vol/vol_surface.hpp"

namespace {

using namespace atx::vol;
namespace time = atx::core::time;

constexpr double kRate = 0.043;

std::int64_t timestamp(int year, unsigned month, unsigned day, unsigned hour = 19,
                       unsigned minute = 55) {
  return time::timestamp_from_utc(year, month, day, hour, minute, 0, 0).unix_nanos();
}

PricedSurface make_surface(std::uint32_t uid, double spot, std::int64_t now) {
  CurveSurface curves;
  std::vector<SliceContext> context;
  std::uint16_t expiry_id = 0;
  for (const double term : {0.03, 0.08, 0.15, 0.25, 0.50, 1.00}) {
    EssviParams parameters{};
    parameters.theta = 0.035 + 0.012 * term;
    parameters.phi = 1.35;
    parameters.rho = -0.32;
    parameters.psi = 0.5;
    parameters.p = 0.5;
    parameters.lambda = 0.5;
    parameters.T = term;
    parameters.F = spot;
    parameters.expiry_id = expiry_id++;
    curves.push(std::make_unique<EssviCurve>(parameters, std::exp(-kRate * term)));
    context.push_back(SliceContext{term, spot, 0.0, 0.02, 120, 7});
  }
  PricingContext pricing;
  pricing.S = spot;
  pricing.r = kRate;
  pricing.now_ts_ns = now;
  pricing.method = AmericanMethod::AndersenLake;
  pricing.al_opts = al_fast_opts();
  pricing.uid = uid;
  auto surface = PricedSurface::create(std::move(curves), std::move(context), pricing);
  EXPECT_TRUE(surface) << (surface ? std::string{} : surface.error().to_string());
  return std::move(*surface);
}

OptionProjectionSpec spec(std::uint32_t uid, Side side, ProjectedMaturitySpec maturity,
                          ProjectedStrikeSpec strike) {
  OptionProjectionSpec result;
  result.uid = uid;
  result.side = side;
  result.maturity = maturity;
  result.strike = strike;
  return result;
}

std::vector<const PricedSurface *> pointers(const std::vector<PricedSurface> &surfaces) {
  std::vector<const PricedSurface *> result;
  for (const PricedSurface &surface : surfaces) {
    result.push_back(&surface);
  }
  return result;
}

TEST(ContractProjection, FortyDeltaThreeCalendarMonthCallBecomesConcreteTheo) {
  const std::int64_t now = timestamp(2024, 1, 31);
  const PricedSurface surface = make_surface(7u, 195.0, now);
  const OptionProjectionSpec request =
      spec(7u, Side::Call, ProjectedMaturitySpec::months(3), ProjectedStrikeSpec::delta(0.40));

  auto projected = project_option_contract(surface, request);
  ASSERT_TRUE(projected) << (projected ? std::string{} : projected.error().to_string());
  EXPECT_EQ(projected->status, OptionProjectionStatus::Ok);
  EXPECT_EQ(projected->definition.valuation_ts_ns, now);
  EXPECT_EQ(projected->definition.expiry_ts_ns, timestamp(2024, 4, 30));
  EXPECT_EQ(projected->definition.contract.uid, 7u);
  EXPECT_EQ(projected->definition.contract.side, Side::Call);
  EXPECT_GT(projected->definition.contract.K, 0.0);
  EXPECT_GT(projected->definition.contract.T, 0.0);
  EXPECT_NE(projected->definition.fingerprint, 0u);
  EXPECT_NEAR(projected->achieved_delta, 0.40, 1.0e-7);
  EXPECT_LE(projected->delta_evaluations, 15u);
  EXPECT_DOUBLE_EQ(projected->model_mark, projected->greeks.price);
  EXPECT_DOUBLE_EQ(surface.iv(projected->definition.contract.K, projected->definition.contract.T),
                   projected->implied_vol);
  auto independent = surface.greeks_analytic(projected->definition.contract.K,
                                             projected->definition.contract.T, Side::Call);
  ASSERT_TRUE(independent);
  EXPECT_DOUBLE_EQ(independent->price, projected->model_mark);
  EXPECT_DOUBLE_EQ(independent->delta, projected->achieved_delta);

  auto legacy =
      resolve_strike_by_delta(surface, projected->definition.contract.T, Side::Call, 0.40);
  ASSERT_TRUE(legacy);
  auto legacy_delta = surface.delta(*legacy, projected->definition.contract.T, Side::Call);
  ASSERT_TRUE(legacy_delta);
  EXPECT_NEAR(*legacy_delta, 0.40, 1.0e-7);
}

TEST(ContractProjection, CalendarMonthEndAndAbsoluteExpiryAreExact) {
  const std::int64_t leap_now = timestamp(2024, 1, 31, 12, 34);
  const PricedSurface leap_surface = make_surface(1u, 100.0, leap_now);
  auto february = project_option_contract(
      leap_surface,
      spec(1u, Side::Put, ProjectedMaturitySpec::months(1), ProjectedStrikeSpec::atm_forward()),
      OptionProjectionConfig{OptionProjectionOutput::DefinitionOnly});
  ASSERT_TRUE(february);
  EXPECT_EQ(february->definition.expiry_ts_ns, timestamp(2024, 2, 29, 12, 34));
  EXPECT_TRUE(std::isnan(february->model_mark));

  const std::int64_t exact_expiry = timestamp(2024, 6, 21, 20, 0);
  auto absolute =
      project_option_contract(leap_surface,
                              spec(1u, Side::Call, ProjectedMaturitySpec::absolute(exact_expiry),
                                   ProjectedStrikeSpec::absolute(105.0)),
                              OptionProjectionConfig{OptionProjectionOutput::Mark});
  ASSERT_TRUE(absolute);
  EXPECT_EQ(absolute->definition.expiry_ts_ns, exact_expiry);
  EXPECT_DOUBLE_EQ(absolute->definition.contract.K, 105.0);
  EXPECT_TRUE(std::isfinite(absolute->model_mark));
}

TEST(ContractProjection, StrikeConventionsResolveAgainstSameSurfacePoint) {
  const PricedSurface surface = make_surface(2u, 120.0, timestamp(2026, 7, 10));
  const auto maturity = ProjectedMaturitySpec::days(30);
  auto atm = project_option_contract(
      surface, spec(2u, Side::Call, maturity, ProjectedStrikeSpec::atm_forward()),
      OptionProjectionConfig{OptionProjectionOutput::DefinitionOnly});
  auto moneyness = project_option_contract(
      surface, spec(2u, Side::Call, maturity, ProjectedStrikeSpec::log_moneyness(0.10)),
      OptionProjectionConfig{OptionProjectionOutput::DefinitionOnly});
  ASSERT_TRUE(atm && moneyness);
  EXPECT_DOUBLE_EQ(atm->definition.contract.K, atm->forward);
  EXPECT_DOUBLE_EQ(moneyness->definition.contract.K, moneyness->forward * std::exp(0.10));
  EXPECT_EQ(atm->definition.expiry_ts_ns, timestamp(2026, 8, 9));
}

TEST(ContractProjection, PriceOptionsRouteCanForceColdOnPreparedFastSurface) {
  PricedSurface source = make_surface(2u, 120.0, timestamp(2026, 7, 10));
  auto prepared = std::move(source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(prepared) << (prepared ? std::string{} : prepared.error().to_string());
  const PricedSurface fast = std::move(*prepared);

  OptionProjectionConfig config;
  config.output = OptionProjectionOutput::FullGreeks;
  config.analytic_greeks = true;
  config.query_execution = QueryExecution::ColdReference;
  const auto projected = project_option_contract(
      fast,
      spec(2u, Side::Put, ProjectedMaturitySpec::days(30), ProjectedStrikeSpec::atm_forward()),
      config);
  ASSERT_TRUE(projected) << (projected ? std::string{} : projected.error().to_string());
  const auto cold =
      fast.greeks_analytic(projected->definition.contract.K, projected->definition.contract.T,
                           Side::Put, QueryExecution::ColdReference);
  ASSERT_TRUE(cold) << (cold ? std::string{} : cold.error().to_string());
  EXPECT_EQ(projected->model_mark, cold->price);
  EXPECT_EQ(projected->greeks.vega, cold->vega);
}

TEST(ContractProjection, FastDeltaScreenIsAlwaysColdConfirmed) {
  PricedSurface source = make_surface(2u, 120.0, timestamp(2026, 7, 10));
  auto prepared = std::move(source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(prepared) << (prepared ? std::string{} : prepared.error().to_string());
  const PricedSurface fast = std::move(*prepared);

  OptionProjectionConfig config;
  config.output = OptionProjectionOutput::DefinitionOnly;
  config.delta_tolerance = 1.0e-7;
  config.query_execution = QueryExecution::ColdReference;
  config.delta_solve_policy = OptionDeltaSolvePolicy::FastScreenColdConfirm;
  const auto projected = project_option_contract(
      fast, spec(2u, Side::Call, ProjectedMaturitySpec::days(45), ProjectedStrikeSpec::delta(0.40)),
      config);
  ASSERT_TRUE(projected) << (projected ? std::string{} : projected.error().to_string());
  const auto cold_delta =
      fast.delta(projected->definition.contract.K, projected->definition.contract.T, Side::Call,
                 QueryExecution::ColdReference);
  ASSERT_TRUE(cold_delta) << (cold_delta ? std::string{} : cold_delta.error().to_string());
  EXPECT_NEAR(std::fabs(*cold_delta), 0.40, config.delta_tolerance);
  EXPECT_EQ(*cold_delta, projected->achieved_delta);
  EXPECT_GT(projected->delta_evaluations, 0u);
}

TEST(ContractProjection, PreparedBatchIsInputOrderedAndThreadInvariant) {
  const std::int64_t now = timestamp(2026, 7, 10);
  std::vector<PricedSurface> surfaces;
  surfaces.push_back(make_surface(1u, 100.0, now));
  surfaces.push_back(make_surface(2u, 200.0, now));
  surfaces.push_back(make_surface(3u, 50.0, now));
  auto set = SurfaceSet::create(pointers(surfaces));
  ASSERT_TRUE(set);

  const std::vector<OptionProjectionSpec> requests = {
      spec(3u, Side::Put, ProjectedMaturitySpec::months(3), ProjectedStrikeSpec::delta(0.25)),
      spec(1u, Side::Call, ProjectedMaturitySpec::days(45), ProjectedStrikeSpec::delta(0.40)),
      spec(2u, Side::Call, ProjectedMaturitySpec::years(0.5), ProjectedStrikeSpec::atm_forward()),
      spec(1u, Side::Put, ProjectedMaturitySpec::days(45),
           ProjectedStrikeSpec::log_moneyness(-0.10)),
      spec(99u, Side::Call, ProjectedMaturitySpec::months(3), ProjectedStrikeSpec::delta(0.40)),
  };
  auto prepared = PreparedOptionProjection::create(requests);
  ASSERT_TRUE(prepared) << (prepared ? std::string{} : prepared.error().to_string());
  EXPECT_NE(prepared->fingerprint(), 0u);

  std::vector<ProjectedOption> serial(requests.size());
  std::vector<ProjectedOption> parallel(requests.size());
  OptionProjectionConfig serial_config;
  serial_config.n_threads = 1;
  OptionProjectionConfig parallel_config = serial_config;
  parallel_config.n_threads = 4;
  ASSERT_TRUE(prepared->project_into(*set, serial, serial_config));
  ASSERT_TRUE(prepared->project_into(*set, parallel, parallel_config));
  EXPECT_EQ(serial, parallel);
  for (std::size_t i = 0; i < 4u; ++i) {
    EXPECT_EQ(serial[i].definition.contract.uid, requests[i].uid);
    EXPECT_EQ(serial[i].status, OptionProjectionStatus::Ok);
    auto scalar = project_option_contract(*set->find(requests[i].uid), requests[i], serial_config);
    ASSERT_TRUE(scalar);
    EXPECT_EQ(serial[i], *scalar);
  }
  EXPECT_EQ(serial.back().status, OptionProjectionStatus::SurfaceUnavailable);
}

TEST(ContractProjection, InvalidSpecsFailWithoutInventingContracts) {
  const PricedSurface surface = make_surface(1u, 100.0, timestamp(2026, 7, 10));
  EXPECT_FALSE(
      project_option_contract(surface, spec(1u, Side::Call, ProjectedMaturitySpec::months(0),
                                            ProjectedStrikeSpec::delta(0.40))));
  EXPECT_FALSE(
      project_option_contract(surface, spec(1u, Side::Call, ProjectedMaturitySpec::months(3),
                                            ProjectedStrikeSpec::delta(1.0))));
  EXPECT_FALSE(
      project_option_contract(surface, spec(2u, Side::Call, ProjectedMaturitySpec::months(3),
                                            ProjectedStrikeSpec::delta(0.40))));
  EXPECT_FALSE(PreparedOptionProjection::create({}));
}

} // namespace
