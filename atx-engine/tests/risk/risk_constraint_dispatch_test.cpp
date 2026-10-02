#include <cmath>
#include <initializer_list>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/horizon.hpp"
#include "atx/engine/risk/multi_horizon.hpp"
#include "atx/engine/risk/optimizer.hpp"
#include "atx/engine/risk/qp_augment.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atxtest_risk_constraint_dispatch_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::ConstraintSet;
using atx::engine::risk::FactorModel;
using atx::engine::risk::HorizonSources;
using atx::engine::risk::MultiHorizonConfig;
using atx::engine::risk::MultiHorizonOptimizer;
using atx::engine::risk::MultiHorizonResult;
using atx::engine::risk::OptimizerConfig;
using atx::engine::risk::OwnershipCap;
using atx::engine::risk::ParticipationCap;
using atx::engine::risk::PortfolioOptimizer;
using atx::engine::risk::RebalanceSchedule;
using atx::engine::risk::RobustAlpha;
using atx::engine::risk::SectorRiskBudget;
using atx::engine::risk::SignalHorizon;
using atx::engine::risk::TrackingError;
using atx::engine::risk::TurnoverBudget;

// For a neutral book (a, -a), variance is 2.4*a*a and factor exposure is 2*a.
[[nodiscard]] FactorModel model() {
  MatX x(2, 1);
  x << 1.0, -1.0;
  MatX f(1, 1);
  f << 0.1;
  VecX d = VecX::Ones(2);
  auto result = FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  EXPECT_TRUE(result.has_value());
  return std::move(*result);
}

const std::vector<f64> kAlpha{0.5, -0.5};

[[nodiscard]] PortfolioOptimizer portfolio_optimizer(const ConstraintSet &constraints) {
  OptimizerConfig cfg;
  cfg.risk_aversion = 0.5;
  PortfolioOptimizer opt{cfg};
  opt.constraints = constraints;
  opt.qp.rho = 10.0;
  opt.qp.iters = 1500U;
  return opt;
}

[[nodiscard]] atx::core::Result<MultiHorizonResult>
run_horizon(const ConstraintSet &constraints, const FactorModel &v, bool stacked = false,
            const atx::engine::book::CostInputs &cost = {0.0, 0.0, 1.0},
            f64 trade_rate = 1.0, bool capacity_bound = true) {
  MultiHorizonConfig cfg;
  cfg.risk_aversion = 0.5;
  cfg.constraints = constraints;
  cfg.qp.rho = 10.0;
  cfg.qp.iters = 1500U;
  cfg.stacked_mpc = stacked;
  cfg.trade_rate = trade_rate;
  cfg.capacity_bound_gross = capacity_bound;
  const MultiHorizonOptimizer opt{cfg};
  return opt.run(
      RebalanceSchedule{{0U}},
      [](usize) {
        HorizonSources sources;
        sources.pairs.emplace_back(std::span<const f64>{kAlpha}, SignalHorizon::identity());
        return sources;
      },
      [&v](usize) -> const FactorModel & { return v; }, cost);
}

[[nodiscard]] f64 tracking_error(std::span<const f64> w) {
  return std::sqrt(w[0] * w[0] + w[1] * w[1] + 0.1 * (w[0] - w[1]) * (w[0] - w[1]));
}

TEST(RiskConstraintDispatch, PortfolioTrackingOnlyBindsAtRequestedRisk) {
  const FactorModel v = model();
  ConstraintSet cs;
  cs.track = TrackingError{{}, 0.1};
  const auto result = portfolio_optimizer(cs).solve(kAlpha, v, {});
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
  EXPECT_NEAR(tracking_error(*result), 0.1, 2e-6);
  EXPECT_NEAR((*result)[0] + (*result)[1], 0.0, 2e-6);
}

TEST(RiskConstraintDispatch, PortfolioRobustOnlyMatchesAnalyticOptimum) {
  const FactorModel v = model();
  ConstraintSet cs;
  cs.robust = RobustAlpha{0.3};
  const auto result = portfolio_optimizer(cs).solve(kAlpha, v, {});
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
  // Minimize 1.2*a^2 - a + 0.6*abs(a): a = 1/6, within the gross cap.
  EXPECT_NEAR((*result)[0], 1.0 / 6.0, 2e-5);
  EXPECT_NEAR((*result)[1], -1.0 / 6.0, 2e-5);
}

TEST(RiskConstraintDispatch, PortfolioStandaloneConeValidationIsNotBypassed) {
  const FactorModel v = model();
  ConstraintSet tracking;
  tracking.track = TrackingError{{}, -0.1};
  ConstraintSet robust;
  robust.robust = RobustAlpha{-0.1};
  for (const ConstraintSet &cs : {tracking, robust}) {
    const auto result = portfolio_optimizer(cs).solve(kAlpha, v, {});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

TEST(RiskConstraintDispatch, HorizonTrackingOnlyBindsInBothForecastModes) {
  const FactorModel v = model();
  ConstraintSet cs;
  cs.track = TrackingError{{}, 0.1};
  for (const bool stacked : {false, true}) {
    const auto result = run_horizon(cs, v, stacked);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    ASSERT_EQ(result->books.size(), 1U);
    EXPECT_NEAR(tracking_error(result->books[0]), 0.1, 2e-6);
  }
}

TEST(RiskConstraintDispatch, HorizonRobustOnlyMatchesAnalyticOptimum) {
  const FactorModel v = model();
  ConstraintSet cs;
  cs.robust = RobustAlpha{0.3};
  const auto result = run_horizon(cs, v);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
  ASSERT_EQ(result->books.size(), 1U);
  EXPECT_NEAR(result->books[0][0], 1.0 / 6.0, 2e-5);
  EXPECT_NEAR(result->books[0][1], -1.0 / 6.0, 2e-5);
}

TEST(RiskConstraintDispatch, HorizonSectorOnlyHonorsEachSectorLimit) {
  const FactorModel v = model();
  const std::vector<usize> sectors{0U, 1U};
  ConstraintSet cs;
  cs.sector = SectorRiskBudget{std::span<const usize>{sectors}, {0.1, 0.1}};
  const auto result = run_horizon(cs, v);
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
  ASSERT_EQ(result->books.size(), 1U);
  EXPECT_NEAR(result->books[0][0], 0.1, 2e-6);
  EXPECT_NEAR(result->books[0][1], -0.1, 2e-6);
}

TEST(RiskConstraintDispatch, HorizonCapacityCapsWithoutReferenceDataFailClosed) {
  const FactorModel v = model();
  for (const f64 fraction : {0.1, -0.1}) {
    ConstraintSet participation;
    participation.part = ParticipationCap{fraction};
    ConstraintSet ownership;
    ownership.own = OwnershipCap{fraction};
    for (const ConstraintSet &cs : {participation, ownership}) {
      const auto result = run_horizon(cs, v);
      ASSERT_FALSE(result.has_value());
      EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
    }
  }
}

TEST(RiskConstraintDispatch, HorizonStandaloneConeValidationIsNotBypassed) {
  const FactorModel v = model();
  ConstraintSet tracking;
  tracking.track = TrackingError{{}, -0.1};
  ConstraintSet robust;
  robust.robust = RobustAlpha{-0.1};
  for (const ConstraintSet &cs : {tracking, robust}) {
    const auto result = run_horizon(cs, v);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

TEST(RiskConstraintDispatch, PortfolioTrackingRetainsTurnoverCostAndPreviousBook) {
  const FactorModel v = model();
  ConstraintSet cs;
  cs.track = TrackingError{{}, 100.0}; // nonbinding; only selects augmented dispatch
  auto opt = portfolio_optimizer(cs);
  opt.cfg.turnover_penalty = 0.2;
  for (const f64 previous : {0.1, 0.4}) {
    const std::vector<f64> w_prev{previous, -previous};
    const auto result = opt.solve(kAlpha, v, w_prev);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    // Minimize 1.2*a^2-a+0.4*abs(a-previous): a=.25, or keep .4 in no-trade region.
    const f64 expected = previous == 0.1 ? 0.25 : 0.4;
    EXPECT_NEAR((*result)[0], expected, 2e-5);
    EXPECT_NEAR((*result)[1], -expected, 2e-5);
  }
}

TEST(RiskConstraintDispatch, PortfolioTurnoverCostCoexistsWithHardBudgetAndRobustCone) {
  const FactorModel v = model();
  ConstraintSet cs;
  cs.robust = RobustAlpha{0.1};
  auto opt = portfolio_optimizer(cs);
  opt.cfg.turnover_penalty = 0.2;
  auto result = opt.solve(kAlpha, v, {});
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
  EXPECT_NEAR((*result)[0], 1.0 / 6.0, 2e-5);
  EXPECT_NEAR((*result)[1], -1.0 / 6.0, 2e-5);

  opt.constraints->turn = TurnoverBudget{0.1};
  result = opt.solve(kAlpha, v, {});
  ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
  EXPECT_NEAR((*result)[0], 0.05, 2e-5);
  EXPECT_NEAR((*result)[1], -0.05, 2e-5);
}

TEST(RiskConstraintDispatch, HorizonTrackingRetainsCalibratedTurnoverCost) {
  const FactorModel v = model();
  ConstraintSet cs;
  cs.track = TrackingError{{}, 100.0};
  for (const bool stacked : {false, true}) {
    const auto result = run_horizon(cs, v, stacked, {0.2, 0.0, 1.0});
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    ASSERT_EQ(result->books.size(), 1U);
    EXPECT_NEAR(result->books[0][0], 0.25, 2e-5);
    EXPECT_NEAR(result->books[0][1], -0.25, 2e-5);
  }
}

TEST(RiskConstraintDispatch, HorizonTrackingRetainsCapacityBoundAndHonorsOptOut) {
  const FactorModel v = model();
  ConstraintSet cs;
  cs.track = TrackingError{{}, 100.0};
  for (const bool stacked : {false, true}) {
    for (const bool bound : {false, true}) {
      const auto result = run_horizon(cs, v, stacked, {0.0, 0.0, 0.05}, 1.0, bound);
      ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
      ASSERT_EQ(result->books.size(), 1U);
      const f64 expected = bound ? 0.025 : 5.0 / 12.0;
      EXPECT_NEAR(result->books[0][0], expected, 2e-5);
      EXPECT_NEAR(result->books[0][1], -expected, 2e-5);
    }
  }
}

TEST(RiskConstraintDispatch, HorizonPartialExecutionWithNonzeroBenchmarkFailsClosed) {
  const FactorModel v = model();
  const std::vector<f64> benchmark{0.4, -0.4};
  ConstraintSet cs;
  cs.track = TrackingError{benchmark, 0.05};
  for (const bool stacked : {false, true}) {
    const auto result = run_horizon(cs, v, stacked, {0.0, 0.0, 1.0}, 0.5);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

TEST(RiskConstraintDispatch, QpTurnoverPenaltyRejectsInvalidCoefficientAndReference) {
  const FactorModel v = model();
  const ConstraintSet cs;
  auto materialized = cs.materialize(v.exposures(), {}, 2U);
  ASSERT_TRUE(materialized.has_value());
  const std::vector<f64> q{-0.5, 0.5};
  const atx::engine::risk::ConstrainedQpSolver solver{};
  const atx::engine::risk::QpProblem problem{v, 0.5, q, *materialized};
  materialized->turnover_ref = {0.0, 0.0};
  for (const f64 penalty : {-0.1, std::numeric_limits<f64>::infinity(),
                            std::numeric_limits<f64>::quiet_NaN()}) {
    materialized->turnover_penalty = penalty;
    const auto result = solver.solve(problem);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
  materialized->turnover_penalty = 0.2;
  materialized->turnover_ref = {0.0};
  EXPECT_FALSE(solver.solve(problem).has_value());
  materialized->turnover_ref = {0.0, std::numeric_limits<f64>::quiet_NaN()};
  EXPECT_FALSE(solver.solve(problem).has_value());
}

TEST(RiskConstraintDispatch, AugmentedObjectiveIncludesExactTurnoverPenalty) {
  const FactorModel v = model();
  const ConstraintSet cs;
  auto materialized = cs.materialize(v.exposures(), {}, 2U);
  ASSERT_TRUE(materialized.has_value());
  materialized->turnover_penalty = 0.2;
  materialized->turnover_ref = {0.1, -0.1};
  const std::vector<f64> q{-0.5, 0.5};
  const auto augmented = atx::engine::risk::build_augmented(v, 0.5, q, *materialized);
  // [w0,w1,y,s0,s1,r0,r1], with r=abs(w-prev). No hard turnover budget is present.
  VecX x(7);
  x << 0.25, -0.25, 0.5, 0.25, 0.25, 0.15, 0.15;
  ASSERT_EQ(augmented.P.rows(), x.size());
  const f64 objective = 0.5 * x.dot(augmented.P * x) + augmented.q_aug.dot(x);
  EXPECT_NEAR(objective, 1.2 * 0.25 * 0.25 - 0.25 + 0.4 * 0.15, 1e-12);
}

TEST(RiskConstraintDispatch, StandaloneLiquidityCapsBindPortfolioAndHorizonBooks) {
  const FactorModel v = model();
  const std::vector<f64> adv{100.0, 100.0};
  const std::vector<f64> previous{0.1, -0.1};
  ConstraintSet trade;
  trade.trade = atx::engine::risk::TradeParticipationCap{0.1, {adv, 100.0}};
  ConstraintSet liquidation;
  liquidation.liquidation = atx::engine::risk::DaysToLiquidate{1.0, 0.1, {adv, 100.0}};
  for (const auto &cs : {trade, liquidation}) {
    const auto result = portfolio_optimizer(cs).solve(kAlpha, v, previous);
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().to_string());
    const f64 limit = cs.trade ? 0.2 : 0.1;
    EXPECT_NEAR((*result)[0], limit, 2e-6);
    EXPECT_NEAR((*result)[1], -limit, 2e-6);
    for (const bool stacked : {false, true}) {
      const auto horizon = run_horizon(cs, v, stacked);
      ASSERT_TRUE(horizon.has_value()) << (horizon ? "" : horizon.error().to_string());
      ASSERT_EQ(horizon->books.size(), 1U);
      EXPECT_NEAR(horizon->books[0][0], 0.1, 2e-6);
      EXPECT_NEAR(horizon->books[0][1], -0.1, 2e-6);
    }
  }
}

TEST(RiskConstraintDispatch, StandaloneLiquidityCapsRejectMissingInputs) {
  const FactorModel v = model();
  ConstraintSet trade;
  trade.trade = atx::engine::risk::TradeParticipationCap{};
  ConstraintSet liquidation;
  liquidation.liquidation = atx::engine::risk::DaysToLiquidate{};
  for (const auto &cs : {trade, liquidation}) {
    EXPECT_FALSE(portfolio_optimizer(cs).solve(kAlpha, v, {}).has_value());
    EXPECT_FALSE(run_horizon(cs, v).has_value());
  }
}

TEST(RiskConstraintDispatch, TrueMpcRejectsStageRelativeTradeCap) {
  const FactorModel v = model();
  const std::vector<f64> adv{100.0, 100.0};
  MultiHorizonConfig cfg;
  cfg.true_mpc = true;
  cfg.constraints.trade = atx::engine::risk::TradeParticipationCap{0.1, {adv, 100.0}};
  const auto result = MultiHorizonOptimizer{cfg}.run(
      RebalanceSchedule{{0U}},
      [](usize) {
        HorizonSources sources;
        sources.pairs.emplace_back(std::span<const f64>{kAlpha}, SignalHorizon::identity());
        return sources;
      },
      [&v](usize) -> const FactorModel & { return v; }, {0.0, 0.0, 1.0});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(RiskConstraintDispatch, FastPortfolioRejectsInvalidLimitsAndNonfiniteInputs) {
  const FactorModel v = model();
  for (const f64 invalid : {-0.1, std::numeric_limits<f64>::infinity(),
                            std::numeric_limits<f64>::quiet_NaN()}) {
    for (const auto member : {&OptimizerConfig::risk_aversion, &OptimizerConfig::turnover_penalty,
                              &OptimizerConfig::gross_leverage, &OptimizerConfig::name_cap}) {
      OptimizerConfig cfg;
      cfg.*member = invalid;
      EXPECT_FALSE(PortfolioOptimizer{cfg}.solve(kAlpha, v, {}).has_value());
    }
    ConstraintSet cs;
    cs.pos = atx::engine::risk::PositionCap{invalid};
    EXPECT_FALSE(portfolio_optimizer(cs).solve(kAlpha, v, {}).has_value());
  }
  const PortfolioOptimizer opt;
  for (const f64 invalid : {std::numeric_limits<f64>::infinity(),
                            -std::numeric_limits<f64>::infinity()}) {
    const std::vector<f64> alpha{invalid, -0.5};
    EXPECT_FALSE(opt.solve(alpha, v, {}).has_value());
  }
  const std::vector<f64> no_opinion{std::numeric_limits<f64>::quiet_NaN(), -0.5};
  EXPECT_TRUE(opt.solve(no_opinion, v, {}).has_value());
  for (const f64 invalid : {std::numeric_limits<f64>::infinity(),
                            std::numeric_limits<f64>::quiet_NaN()}) {
    const std::vector<f64> previous{0.1, invalid};
    EXPECT_FALSE(opt.solve(kAlpha, v, previous).has_value());
  }
}

} // namespace atxtest_risk_constraint_dispatch_test
