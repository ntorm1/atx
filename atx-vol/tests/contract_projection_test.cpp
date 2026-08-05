#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
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

TEST(ContractProjection, CrossSectionalPolicyResolvesLikeFastScreenInScalarEntryPoint) {
  PricedSurface source = make_surface(2u, 120.0, timestamp(2026, 7, 10));
  auto prepared = std::move(source).with_query_pricing(QueryPricingTier::RepresentativeFast);
  ASSERT_TRUE(prepared) << (prepared ? std::string{} : prepared.error().to_string());
  const PricedSurface fast = std::move(*prepared);

  OptionProjectionConfig fast_screen_config;
  // FullGreeks (not DefinitionOnly): DefinitionOnly leaves model_mark as NaN,
  // and NaN != NaN under the defaulted operator== used by EXPECT_EQ below.
  fast_screen_config.output = OptionProjectionOutput::FullGreeks;
  fast_screen_config.delta_tolerance = 1.0e-7;
  fast_screen_config.query_execution = QueryExecution::ColdReference;
  fast_screen_config.delta_solve_policy = OptionDeltaSolvePolicy::FastScreenColdConfirm;

  OptionProjectionConfig cross_sectional_config = fast_screen_config;
  cross_sectional_config.delta_solve_policy = OptionDeltaSolvePolicy::CrossSectionalColdConfirm;

  const auto request =
      spec(2u, Side::Call, ProjectedMaturitySpec::days(45), ProjectedStrikeSpec::delta(0.40));
  const auto fast_screen_projected = project_option_contract(fast, request, fast_screen_config);
  const auto cross_sectional_projected =
      project_option_contract(fast, request, cross_sectional_config);
  ASSERT_TRUE(fast_screen_projected)
      << (fast_screen_projected ? std::string{} : fast_screen_projected.error().to_string());
  ASSERT_TRUE(cross_sectional_projected)
      << (cross_sectional_projected ? std::string{}
                                    : cross_sectional_projected.error().to_string());
  EXPECT_EQ(*fast_screen_projected, *cross_sectional_projected);

  const auto cold_delta = fast.delta(cross_sectional_projected->definition.contract.K,
                                     cross_sectional_projected->definition.contract.T, Side::Call,
                                     QueryExecution::ColdReference);
  ASSERT_TRUE(cold_delta) << (cold_delta ? std::string{} : cold_delta.error().to_string());
  EXPECT_NEAR(std::fabs(*cold_delta), 0.40, cross_sectional_config.delta_tolerance);
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

TEST(ContractProjection, ResolveProjectedExpiryMatchesProjectedDefinitionExpiry) {
  const std::int64_t now = timestamp(2026, 7, 10);
  const PricedSurface surface = make_surface(4u, 100.0, now);
  for (const ProjectedMaturitySpec &maturity :
       {ProjectedMaturitySpec::months(3), ProjectedMaturitySpec::days(30),
        ProjectedMaturitySpec::years(0.25)}) {
    auto resolved = resolve_projected_expiry(now, maturity);
    ASSERT_TRUE(resolved) << (resolved ? std::string{} : resolved.error().to_string());
    auto projected = project_option_contract(
        surface, spec(4u, Side::Call, maturity, ProjectedStrikeSpec::atm_forward()),
        OptionProjectionConfig{OptionProjectionOutput::DefinitionOnly});
    ASSERT_TRUE(projected) << (projected ? std::string{} : projected.error().to_string());
    EXPECT_EQ(*resolved, projected->definition.expiry_ts_ns);
  }

  EXPECT_FALSE(resolve_projected_expiry(now, ProjectedMaturitySpec::months(0)));
  EXPECT_FALSE(resolve_projected_expiry(0, ProjectedMaturitySpec::days(30)));
  EXPECT_FALSE(resolve_projected_expiry(now, ProjectedMaturitySpec::absolute(now)));
}

// ── solve_american_delta_batch (cross-sectional solver core) ─────────────────

// Row grid for the batch-delta tests: targets x sides x maturities, ordered so
// rows sharing a maturity are contiguous (bit-identical T runs feed the laned
// evaluate_batch ladder-reuse path).
struct BatchDeltaGrid {
  std::vector<double> T;
  std::vector<Side> side;
  std::vector<double> target;
  std::vector<ProjectedMaturitySpec> maturity;
};

BatchDeltaGrid make_batch_delta_grid(std::int64_t now) {
  BatchDeltaGrid grid;
  for (const ProjectedMaturitySpec &maturity :
       {ProjectedMaturitySpec::days(30), ProjectedMaturitySpec::months(3),
        ProjectedMaturitySpec::years(0.5)}) {
    const auto expiry = resolve_projected_expiry(now, maturity);
    if (!expiry) {
      ADD_FAILURE() << "grid expiry: " << expiry.error().to_string();
      continue;
    }
    const double t = static_cast<double>(*expiry - now) / kNsPerYear;
    for (const Side side : {Side::Call, Side::Put}) {
      for (const double target : {0.10, 0.25, 0.40, 0.55, 0.75}) {
        grid.T.push_back(t);
        grid.side.push_back(side);
        grid.target.push_back(target);
        grid.maturity.push_back(maturity);
      }
    }
  }
  return grid;
}

struct BatchDeltaOutputs {
  std::vector<double> strike;
  std::vector<double> achieved;
  std::vector<std::uint16_t> evaluations;
  std::vector<Status> row_status;

  explicit BatchDeltaOutputs(std::size_t n)
      : strike(n, 0.0), achieved(n, 0.0), evaluations(n, 0u), row_status(n) {}
};

// Mirrors the private `kMaxBatchDeltaPasses` pass budget declared in the
// anonymous namespace of contract_projection.cpp; not exposed via the public
// header, so the Task-4 fallback tests re-declare the same value here purely
// as a loose upper bound distinguishing "stayed inside the batch passes" rows
// from "fell through to the scalar fallback tail" rows. Raised 6 -> 8 with
// the Task-9 accelerant-4 budget bump -- keep in lockstep with the solver.
constexpr std::uint16_t kMirroredMaxBatchDeltaPasses = 8u;

double median_of(std::vector<std::uint16_t> values) {
  std::sort(values.begin(), values.end());
  const std::size_t mid = values.size() / 2;
  if (values.size() % 2 == 1) {
    return static_cast<double>(values[mid]);
  }
  return 0.5 * (static_cast<double>(values[mid - 1]) + static_cast<double>(values[mid]));
}

TEST(ContractProjection, BatchDeltaSolveIsColdConfirmedAgainstScalarDeltaOracle) {
  const std::int64_t now = timestamp(2026, 7, 10);
  const PricedSurface surface = make_surface(2u, 120.0, now);
  const BatchDeltaGrid grid = make_batch_delta_grid(now);
  const std::size_t n = grid.T.size();
  ASSERT_GE(n, 24u);

  constexpr double tolerance = 1.0e-7;
  AmericanDeltaBatchScratch scratch;
  BatchDeltaOutputs out(n);
  const Status solved =
      solve_american_delta_batch(surface, grid.T, grid.side, grid.target, tolerance, scratch,
                                 out.strike, out.achieved, out.evaluations, out.row_status);
  ASSERT_TRUE(solved) << (solved ? std::string{} : solved.error().to_string());
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(out.row_status[i]) << "row " << i << ": " << out.row_status[i].error().to_string();
    ASSERT_TRUE(std::isfinite(out.strike[i])) << "row " << i;
    EXPECT_GT(out.strike[i], 0.0) << "row " << i;
    EXPECT_GE(out.evaluations[i], 1u) << "row " << i;
    // The accepting pass held the half-tolerance internal margin on its own
    // (laned) delta; the CORRECTNESS GATE below is the independent SCALAR cold
    // oracle at the full tolerance.
    EXPECT_LE(std::fabs(std::fabs(out.achieved[i]) - grid.target[i]), 0.5 * tolerance)
        << "row " << i;
    if (grid.side[i] == Side::Call) {
      EXPECT_GT(out.achieved[i], 0.0) << "row " << i;
    } else {
      EXPECT_LT(out.achieved[i], 0.0) << "row " << i;
    }
    const auto oracle =
        surface.delta(out.strike[i], grid.T[i], grid.side[i], QueryExecution::ColdReference);
    ASSERT_TRUE(oracle) << "row " << i << ": " << oracle.error().to_string();
    EXPECT_LE(std::fabs(std::fabs(*oracle) - grid.target[i]), tolerance) << "row " << i;
  }
}

TEST(ContractProjection, BatchDeltaSolveMatchesDirectProjectionEconomically) {
  const std::int64_t now = timestamp(2026, 7, 10);
  const PricedSurface surface = make_surface(2u, 120.0, now);
  const BatchDeltaGrid grid = make_batch_delta_grid(now);
  const std::size_t n = grid.T.size();

  constexpr double tolerance = 1.0e-7;
  AmericanDeltaBatchScratch scratch;
  BatchDeltaOutputs out(n);
  const Status solved =
      solve_american_delta_batch(surface, grid.T, grid.side, grid.target, tolerance, scratch,
                                 out.strike, out.achieved, out.evaluations, out.row_status);
  ASSERT_TRUE(solved) << (solved ? std::string{} : solved.error().to_string());

  OptionProjectionConfig direct_config;
  direct_config.output = OptionProjectionOutput::DefinitionOnly;
  direct_config.delta_tolerance = tolerance;
  direct_config.query_execution = QueryExecution::ColdReference;
  direct_config.delta_solve_policy = OptionDeltaSolvePolicy::Direct;
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(out.row_status[i]) << "row " << i << ": " << out.row_status[i].error().to_string();
    const auto direct = project_option_contract(
        surface,
        spec(2u, grid.side[i], grid.maturity[i], ProjectedStrikeSpec::delta(grid.target[i])),
        direct_config);
    ASSERT_TRUE(direct) << "row " << i << ": " << direct.error().to_string();
    EXPECT_EQ(direct->definition.contract.T, grid.T[i]) << "row " << i;
    const double direct_strike = direct->definition.contract.K;
    // Both roots satisfy the same cold tolerance; bound route drift without
    // demanding bit equality.
    EXPECT_LE(std::fabs(out.strike[i] - direct_strike), 1.0e-4 * direct_strike) << "row " << i;
  }
}

TEST(ContractProjection, BatchDeltaSolveRowsAreCompositionInvariantAndRepeatable) {
  const std::int64_t now = timestamp(2026, 7, 10);
  const PricedSurface surface = make_surface(2u, 120.0, now);
  const BatchDeltaGrid grid = make_batch_delta_grid(now);
  const std::size_t n = grid.T.size();

  constexpr double tolerance = 1.0e-7;
  AmericanDeltaBatchScratch scratch;
  BatchDeltaOutputs first(n);
  const Status first_solved =
      solve_american_delta_batch(surface, grid.T, grid.side, grid.target, tolerance, scratch,
                                 first.strike, first.achieved, first.evaluations, first.row_status);
  ASSERT_TRUE(first_solved) << (first_solved ? std::string{} : first_solved.error().to_string());

  // Steady-state reuse allocates nothing: every scratch column keeps its
  // capacity across the second full solve on the same workspace.
  const std::vector<std::size_t> capacities = {scratch.k_log.capacity(),
                                               scratch.strike.capacity(),
                                               scratch.residual.capacity(),
                                               scratch.prev_k.capacity(),
                                               scratch.prev_residual.capacity(),
                                               scratch.forward.capacity(),
                                               scratch.sigma.capacity(),
                                               scratch.signed_d1.capacity(),
                                               scratch.iv.capacity(),
                                               scratch.price.capacity(),
                                               scratch.greeks.capacity(),
                                               scratch.pass_status.capacity(),
                                               scratch.active.capacity(),
                                               scratch.active_strike.capacity(),
                                               scratch.active_t.capacity(),
                                               scratch.active_side.capacity(),
                                               scratch.fallback_rows.capacity()};

  BatchDeltaOutputs second(n);
  const Status second_solved = solve_american_delta_batch(
      surface, grid.T, grid.side, grid.target, tolerance, scratch, second.strike, second.achieved,
      second.evaluations, second.row_status);
  ASSERT_TRUE(second_solved) << (second_solved ? std::string{} : second_solved.error().to_string());
  const std::vector<std::size_t> reused_capacities = {scratch.k_log.capacity(),
                                                      scratch.strike.capacity(),
                                                      scratch.residual.capacity(),
                                                      scratch.prev_k.capacity(),
                                                      scratch.prev_residual.capacity(),
                                                      scratch.forward.capacity(),
                                                      scratch.sigma.capacity(),
                                                      scratch.signed_d1.capacity(),
                                                      scratch.iv.capacity(),
                                                      scratch.price.capacity(),
                                                      scratch.greeks.capacity(),
                                                      scratch.pass_status.capacity(),
                                                      scratch.active.capacity(),
                                                      scratch.active_strike.capacity(),
                                                      scratch.active_t.capacity(),
                                                      scratch.active_side.capacity(),
                                                      scratch.fallback_rows.capacity()};
  EXPECT_EQ(capacities, reused_capacities);

  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(first.row_status[i]) << "row " << i;
    ASSERT_TRUE(second.row_status[i]) << "row " << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(first.strike[i]),
              std::bit_cast<std::uint64_t>(second.strike[i]))
        << "row " << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(first.achieved[i]),
              std::bit_cast<std::uint64_t>(second.achieved[i]))
        << "row " << i;
    EXPECT_EQ(first.evaluations[i], second.evaluations[i]) << "row " << i;
  }

  // Any contiguous sub-span solved as its own batch reproduces the full-batch
  // per-row strikes bit for bit (pack-composition invariance of the laned
  // kernels, pinned by PricedSurface.EvaluateBatchLanedGreeksPackCompositionInvariant).
  ASSERT_GE(n, 22u); // guards the hard-coded sub-span indices below.
  for (const auto &[begin, end] :
       {std::pair<std::size_t, std::size_t>{0u, 10u}, std::pair<std::size_t, std::size_t>{10u, 22u},
        std::pair<std::size_t, std::size_t>{7u, 8u}, std::pair<std::size_t, std::size_t>{0u, n}}) {
    const std::size_t m = end - begin;
    AmericanDeltaBatchScratch sub_scratch;
    BatchDeltaOutputs sub(m);
    const Status sub_solved = solve_american_delta_batch(
        surface, std::span<const double>(grid.T).subspan(begin, m),
        std::span<const Side>(grid.side).subspan(begin, m),
        std::span<const double>(grid.target).subspan(begin, m), tolerance, sub_scratch, sub.strike,
        sub.achieved, sub.evaluations, sub.row_status);
    ASSERT_TRUE(sub_solved) << (sub_solved ? std::string{} : sub_solved.error().to_string());
    for (std::size_t j = 0; j < m; ++j) {
      ASSERT_TRUE(sub.row_status[j]) << "sub-span [" << begin << "," << end << ") row " << j;
      EXPECT_EQ(std::bit_cast<std::uint64_t>(sub.strike[j]),
                std::bit_cast<std::uint64_t>(first.strike[begin + j]))
          << "sub-span [" << begin << "," << end << ") row " << j;
      EXPECT_EQ(std::bit_cast<std::uint64_t>(sub.achieved[j]),
                std::bit_cast<std::uint64_t>(first.achieved[begin + j]))
          << "sub-span [" << begin << "," << end << ") row " << j;
    }
  }
}

TEST(ContractProjection, BatchDeltaSolveRejectsStructurallyInvalidSpans) {
  const PricedSurface surface = make_surface(2u, 120.0, timestamp(2026, 7, 10));
  const std::vector<double> T = {0.25, 0.25};
  const std::vector<Side> side = {Side::Call, Side::Put};
  const std::vector<double> target = {0.40, 0.25};
  AmericanDeltaBatchScratch scratch;
  std::vector<double> strike(2, 123.5);
  std::vector<double> achieved(2, 321.25);
  std::vector<std::uint16_t> evaluations(2, 7u);
  std::vector<Status> row_status(2);
  const auto untouched = [&] {
    return strike[0] == 123.5 && strike[1] == 123.5 && achieved[0] == 321.25 &&
           achieved[1] == 321.25 && evaluations[0] == 7u && evaluations[1] == 7u &&
           static_cast<bool>(row_status[0]) && static_cast<bool>(row_status[1]);
  };

  // Mismatched input span length.
  EXPECT_FALSE(solve_american_delta_batch(surface, T, std::span<const Side>(side.data(), 1u),
                                          target, 1.0e-7, scratch, strike, achieved, evaluations,
                                          row_status));
  EXPECT_TRUE(untouched());
  // Mismatched output span length.
  EXPECT_FALSE(solve_american_delta_batch(surface, T, side, target, 1.0e-7, scratch,
                                          std::span<double>(strike.data(), 1u), achieved,
                                          evaluations, row_status));
  EXPECT_TRUE(untouched());
  // Empty batch.
  EXPECT_FALSE(solve_american_delta_batch(surface, {}, {}, {}, 1.0e-7, scratch, {}, {}, {}, {}));
  // Invalid tolerance: zero and above the 1e-3 cap (same predicate as the
  // scalar solver).
  EXPECT_FALSE(solve_american_delta_batch(surface, T, side, target, 0.0, scratch, strike, achieved,
                                          evaluations, row_status));
  EXPECT_TRUE(untouched());
  EXPECT_FALSE(solve_american_delta_batch(surface, T, side, target, 2.0e-3, scratch, strike,
                                          achieved, evaluations, row_status));
  EXPECT_TRUE(untouched());
}

TEST(ContractProjection, BatchDeltaSolveRoutesHardRowsThroughScalarFallbackAndStillConfirms) {
  const std::int64_t now = timestamp(2026, 7, 10);
  const PricedSurface surface = make_surface(2u, 120.0, now);
  const BatchDeltaGrid grid = make_batch_delta_grid(now);
  const std::size_t n_easy = grid.T.size();
  ASSERT_GE(n_easy, 24u);

  // Hard rows appended after the easy grid: boundary-adjacent targets and a
  // very short (days(3), T ~= 0.008) maturity per the brief, PLUS deep-wing
  // targets (0.999) at the 3-month and 2-year maturities that are empirically
  // confirmed (via direct single-row probing) to defeat the vectorized
  // Newton/secant passes -- the 3-month/0.999 rows exhaust the full batch
  // pass budget ("did not converge"), the 2-year/0.999 rows hit the secant
  // degenerate-gap guard -- so at least one row is genuinely difficult and
  // must fall through to the Task-4 scalar fallback tail to still confirm.
  const auto short_expiry = resolve_projected_expiry(now, ProjectedMaturitySpec::days(3));
  ASSERT_TRUE(short_expiry) << short_expiry.error().to_string();
  const double short_t = static_cast<double>(*short_expiry - now) / kNsPerYear;
  const auto month_expiry = resolve_projected_expiry(now, ProjectedMaturitySpec::months(3));
  ASSERT_TRUE(month_expiry) << month_expiry.error().to_string();
  const double month_t = static_cast<double>(*month_expiry - now) / kNsPerYear;
  const auto year_expiry = resolve_projected_expiry(now, ProjectedMaturitySpec::years(2.0));
  ASSERT_TRUE(year_expiry) << year_expiry.error().to_string();
  const double year_t = static_cast<double>(*year_expiry - now) / kNsPerYear;

  struct HardRow {
    double t;
    Side side;
    double target;
  };
  const std::vector<HardRow> hard_rows = {
      {short_t, Side::Call, 0.40},  {short_t, Side::Put, 0.40},  {month_t, Side::Call, 0.02},
      {month_t, Side::Put, 0.02},   {month_t, Side::Call, 0.95}, {month_t, Side::Put, 0.95},
      {month_t, Side::Call, 0.999}, {month_t, Side::Put, 0.999}, {year_t, Side::Call, 0.999},
      {year_t, Side::Put, 0.999},
  };

  std::vector<double> T = grid.T;
  std::vector<Side> side = grid.side;
  std::vector<double> target = grid.target;
  for (const HardRow &row : hard_rows) {
    T.push_back(row.t);
    side.push_back(row.side);
    target.push_back(row.target);
  }
  const std::size_t n = T.size();

  constexpr double tolerance = 1.0e-7;
  AmericanDeltaBatchScratch scratch;
  BatchDeltaOutputs out(n);
  const Status solved =
      solve_american_delta_batch(surface, T, side, target, tolerance, scratch, out.strike,
                                 out.achieved, out.evaluations, out.row_status);
  ASSERT_TRUE(solved) << (solved ? std::string{} : solved.error().to_string());

  std::vector<std::uint16_t> all_evaluations;
  bool any_fallback_exercised = false;
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(out.row_status[i]) << "row " << i << ": " << out.row_status[i].error().to_string();
    ASSERT_TRUE(std::isfinite(out.strike[i])) << "row " << i;
    EXPECT_GT(out.strike[i], 0.0) << "row " << i;
    const auto oracle = surface.delta(out.strike[i], T[i], side[i], QueryExecution::ColdReference);
    ASSERT_TRUE(oracle) << "row " << i << ": " << oracle.error().to_string();
    EXPECT_LE(std::fabs(std::fabs(*oracle) - target[i]), tolerance) << "row " << i;
    all_evaluations.push_back(out.evaluations[i]);
    if (i < n_easy) {
      // Easy rows must converge WITHIN the batch pass budget; a regression
      // that silently routed everything through scalar fallback would blow
      // this bound.
      EXPECT_LE(out.evaluations[i], static_cast<std::uint16_t>(kMirroredMaxBatchDeltaPasses + 1u))
          << "row " << i;
    } else if (out.evaluations[i] > kMirroredMaxBatchDeltaPasses) {
      any_fallback_exercised = true;
    }
  }
  EXPECT_TRUE(any_fallback_exercised)
      << "expected at least one hard row's evaluations_out to exceed the batch pass budget, "
         "proving the scalar fallback tail actually ran";
  EXPECT_LE(median_of(all_evaluations), 4.0);
}

TEST(ContractProjection, BatchDeltaSolveReportsRowFailuresWithoutInventingStrikes) {
  const std::int64_t now = timestamp(2026, 7, 10);
  const PricedSurface surface = make_surface(2u, 120.0, now);
  const BatchDeltaGrid grid = make_batch_delta_grid(now);
  ASSERT_GE(grid.T.size(), 6u);
  const double normal_t = grid.T[0];

  // Bad rows interleaved with good ones: an out-of-range target on each side
  // of (0,1), plus a T so far beyond the last fitted slice that the
  // extrapolated forward overflows to +inf (surface data genuinely
  // unavailable at that T in this surface's flat-extrapolation model, unlike
  // a merely-large-but-still-finite T; verified empirically -- this surface
  // holds forward/iv flat, never NaN, for any FINITE T out to roughly 3.5e4
  // years given its rate/dividend parameters).
  constexpr double kFarBeyondFittedT = 50000.0;
  std::vector<double> T;
  std::vector<Side> side;
  std::vector<double> target;
  std::vector<bool> is_bad;
  std::vector<atx::core::ErrorCode> expected_code;
  const auto push_good = [&](std::size_t grid_index) {
    T.push_back(grid.T[grid_index]);
    side.push_back(grid.side[grid_index]);
    target.push_back(grid.target[grid_index]);
    is_bad.push_back(false);
    expected_code.push_back(ErrorCode::Unknown); // unused: only read for bad rows
  };
  const auto push_bad = [&](double t, Side s, double bad_target, ErrorCode code) {
    T.push_back(t);
    side.push_back(s);
    target.push_back(bad_target);
    is_bad.push_back(true);
    expected_code.push_back(code);
  };

  push_good(0);
  push_bad(normal_t, Side::Call, 1.5, ErrorCode::InvalidArgument);
  push_good(1);
  push_bad(normal_t, Side::Put, -0.1, ErrorCode::InvalidArgument);
  push_good(2);
  push_bad(kFarBeyondFittedT, Side::Call, 0.40, ErrorCode::Unavailable);
  push_good(3);

  const std::size_t n = T.size();
  constexpr double tolerance = 1.0e-7;
  AmericanDeltaBatchScratch scratch;
  BatchDeltaOutputs mixed(n);
  const Status solved =
      solve_american_delta_batch(surface, T, side, target, tolerance, scratch, mixed.strike,
                                 mixed.achieved, mixed.evaluations, mixed.row_status);
  ASSERT_TRUE(solved) << (solved ? std::string{} : solved.error().to_string());

  std::vector<double> good_T, good_target;
  std::vector<Side> good_side;
  std::vector<std::size_t> good_rows;
  for (std::size_t i = 0; i < n; ++i) {
    if (is_bad[i]) {
      EXPECT_FALSE(mixed.row_status[i]) << "row " << i << " expected to fail";
      if (!mixed.row_status[i]) {
        EXPECT_EQ(mixed.row_status[i].error().code(), expected_code[i]) << "row " << i;
      }
      EXPECT_TRUE(std::isnan(mixed.strike[i])) << "row " << i;
      EXPECT_TRUE(std::isnan(mixed.achieved[i])) << "row " << i;
    } else {
      ASSERT_TRUE(mixed.row_status[i])
          << "row " << i << ": " << mixed.row_status[i].error().to_string();
      EXPECT_TRUE(std::isfinite(mixed.strike[i])) << "row " << i;
      good_T.push_back(T[i]);
      good_side.push_back(side[i]);
      good_target.push_back(target[i]);
      good_rows.push_back(i);
    }
  }

  // Neighboring valid rows are unaffected by the interleaved failures: solved
  // alone (their own batch, same scratch discipline) they reproduce
  // bit-identical strikes/deltas.
  AmericanDeltaBatchScratch alone_scratch;
  BatchDeltaOutputs alone(good_T.size());
  const Status alone_solved =
      solve_american_delta_batch(surface, good_T, good_side, good_target, tolerance, alone_scratch,
                                 alone.strike, alone.achieved, alone.evaluations, alone.row_status);
  ASSERT_TRUE(alone_solved) << (alone_solved ? std::string{} : alone_solved.error().to_string());
  for (std::size_t j = 0; j < good_rows.size(); ++j) {
    ASSERT_TRUE(alone.row_status[j]) << "alone row " << j;
    const std::size_t mixed_row = good_rows[j];
    EXPECT_EQ(std::bit_cast<std::uint64_t>(alone.strike[j]),
              std::bit_cast<std::uint64_t>(mixed.strike[mixed_row]))
        << "good row " << mixed_row;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(alone.achieved[j]),
              std::bit_cast<std::uint64_t>(mixed.achieved[mixed_row]))
        << "good row " << mixed_row;
  }
}

TEST(ContractProjection, BatchDeltaSolveEvaluationCountsSaturate) {
  const std::int64_t now = timestamp(2026, 7, 10);
  const PricedSurface surface = make_surface(2u, 120.0, now);
  const BatchDeltaGrid grid = make_batch_delta_grid(now);
  const std::size_t n_easy = grid.T.size();
  ASSERT_GE(n_easy, 24u);

  // One row hard enough to require the scalar fallback tail (empirically
  // confirmed by direct single-row probing to exhaust the batch pass budget
  // with "did not converge"), so its evaluations_out demonstrably includes
  // the fallback's own DeltaSolution evaluations added on top of the
  // batch-pass count via the saturating add_evaluations helper (the helper
  // itself is already unit-exercised transitively, per the brief; this only
  // pins the batch-solver's use of it).
  const auto month_expiry = resolve_projected_expiry(now, ProjectedMaturitySpec::months(3));
  ASSERT_TRUE(month_expiry) << month_expiry.error().to_string();
  const double month_t = static_cast<double>(*month_expiry - now) / kNsPerYear;

  std::vector<double> T = grid.T;
  std::vector<Side> side = grid.side;
  std::vector<double> target = grid.target;
  T.push_back(month_t);
  side.push_back(Side::Put);
  target.push_back(0.999);
  const std::size_t n = T.size();
  const std::size_t hard_row = n - 1u;

  constexpr double tolerance = 1.0e-7;
  AmericanDeltaBatchScratch scratch;
  BatchDeltaOutputs out(n);
  const Status solved =
      solve_american_delta_batch(surface, T, side, target, tolerance, scratch, out.strike,
                                 out.achieved, out.evaluations, out.row_status);
  ASSERT_TRUE(solved) << (solved ? std::string{} : solved.error().to_string());

  // Monotone-nonzero: every successful row's evaluation count is at least
  // one (the counter only ever grows from zero via add_evaluations, never
  // resets), and finite/bounded (no wraparound).
  for (std::size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(out.row_status[i]) << "row " << i << ": " << out.row_status[i].error().to_string();
    EXPECT_GE(out.evaluations[i], 1u) << "row " << i;
    EXPECT_LT(out.evaluations[i], std::numeric_limits<std::uint16_t>::max()) << "row " << i;
  }
  // The hard row's count must exceed the batch-only budget: proof the
  // fallback's evaluations were folded in via add_evaluations, not dropped.
  EXPECT_GT(out.evaluations[hard_row], kMirroredMaxBatchDeltaPasses) << "hard row " << hard_row;
}

TEST(ContractProjection, ProjectedDefinitionFingerprintHelperMatchesProjection) {
  const PricedSurface surface = make_surface(5u, 100.0, timestamp(2026, 7, 10));
  auto projected = project_option_contract(
      surface,
      spec(5u, Side::Call, ProjectedMaturitySpec::days(30), ProjectedStrikeSpec::atm_forward()),
      OptionProjectionConfig{OptionProjectionOutput::DefinitionOnly});
  ASSERT_TRUE(projected) << (projected ? std::string{} : projected.error().to_string());
  const std::uint64_t fingerprint = projected_definition_fingerprint(projected->definition);
  EXPECT_NE(fingerprint, 0u);
  EXPECT_EQ(fingerprint, projected->definition.fingerprint);

  ProjectedOptionDefinition changed = projected->definition;
  changed.contract.K += 1.0;
  EXPECT_NE(projected_definition_fingerprint(changed), fingerprint);
}

} // namespace
