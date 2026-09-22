#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atxtest_risk_qp_polish_test {
namespace risk = atx::engine::risk;
namespace la = atx::core::linalg;

risk::FactorModel unit_model(atx::usize count) {
  const auto n = static_cast<Eigen::Index>(count);
  auto model = risk::FactorModel::create(la::MatX::Zero(n, 1), la::MatX::Identity(1, 1),
                                       la::VecX::Ones(n), 0, 1);
  EXPECT_TRUE(model.has_value()) << (model ? "" : model.error().to_string());
  return std::move(*model);
}

risk::MaterializedConstraints scalar_band(atx::f64 lower, atx::f64 upper) {
  risk::MaterializedConstraints constraints;
  constraints.A = la::MatX::Identity(1, 1);
  constraints.l = la::VecX::Constant(1, lower);
  constraints.u = la::VecX::Constant(1, upper);
  return constraints;
}

risk::ConstrainedQpSolver tight_solver() {
  risk::ConstrainedQpSolver solver;
  solver.cfg.iters = 1200;
  solver.cfg.feas_tol = 1e-12;
  solver.cfg.polish = true;
  solver.cfg.polish_refine = 3;
  solver.cfg.max_factor_bytes = 1'048'576;
  return solver;
}

TEST(RiskQpPolish, BindingTurnoverRemovesRegularizationBiasAndPreservesFreeGrossAuxiliaries) {
  const auto model = unit_model(2);
  const std::vector<atx::f64> q{-0.5, 0.5};
  const std::vector<atx::f64> previous{0.0, 0.0};
  risk::ConstraintSet specification;
  specification.gross = {1.0, true}; // Slack gross auxiliaries have zero objective cost.
  specification.pos = risk::PositionCap{0.5};
  specification.turn = risk::TurnoverBudget{0.2};
  auto constraints = specification.materialize(model.exposures(), previous, 2);
  ASSERT_TRUE(constraints) << constraints.error().message();
  auto solver = tight_solver();
  const auto result = solver.solve_with_cert({model, 0.5, q, *constraints});
  ASSERT_TRUE(result) << result.error().message();
  ASSERT_TRUE(result->cert.polished);
  EXPECT_NEAR(result->book[0], 0.1, 1e-12);
  EXPECT_NEAR(result->book[1], -0.1, 1e-12);
  EXPECT_LE(std::abs(result->book[0]) + std::abs(result->book[1]), 0.2 + 1e-12);
  EXPECT_LE(std::abs(result->book[0] + result->book[1]), 1e-12);
  EXPECT_LE(result->cert.prim_res, 1e-12);
  EXPECT_LE(result->cert.dual_res, 1e-12);
  // The old regularized target retained ~4e-9 per-row bias; initializing a new
  // -q solve at zero instead would erase the unused gross auxiliary slack.
}

TEST(RiskQpPolish, FeasibleEqualityOptimumReplacesInfeasibleLowerObjectiveAfterOneAdmmStep) {
  const auto model = unit_model(1);
  const std::vector<atx::f64> q{0.0};
  const auto constraints = scalar_band(1.0, 1.0);
  auto solver = tight_solver();
  solver.cfg.iters = 1; // Cold ADMM leaves x=0, which violates x=1 but has objective 0.
  solver.cfg.ruiz_passes = 0;
  solver.cfg.polish = false;
  const auto unpolished = solver.solve_with_cert({model, 0.5, q, constraints});
  ASSERT_FALSE(unpolished);
  EXPECT_NE(unpolished.error().message().find("tolerance="), std::string::npos);

  solver.cfg.polish = true;
  const auto polished = solver.solve_with_cert({model, 0.5, q, constraints});
  ASSERT_TRUE(polished) << polished.error().message();
  ASSERT_TRUE(polished->cert.polished);
  EXPECT_NEAR(polished->book[0], 1.0, 1e-12); // True feasible objective is 0.5 > 0.
  EXPECT_LE(polished->cert.prim_res, 1e-12);
  EXPECT_LE(polished->cert.dual_res, 1e-12); // Requires the newly solved equality dual.
}

TEST(RiskQpPolish, WrongActiveMultiplierAndConeViolationPreserveTheAdmmFallback) {
  const auto model = unit_model(1);
  const std::vector<atx::f64> q{0.0};
  const auto constraints = scalar_band(-1.0, 1.0);
  auto solver = tight_solver();
  solver.cfg.iters = 1;
  solver.cfg.ruiz_passes = 0;

  // A deliberately misleading upper-active seed guesses x=+1, but that equality
  // solution has multiplier -1 for the upper bound: it is not the constrained
  // optimum. One seed leaves a feasible ADMM fallback; the other leaves an
  // infeasible fallback which must remain an error despite feasible guessed x=1.
  for (const bool infeasible_seed : {false, true}) {
    const std::vector<atx::f64> x0{infeasible_seed ? -10.0 : 1.0};
    const std::vector<atx::f64> y0{0.0, infeasible_seed ? 10.0 : 2.0};
    const risk::QpProblem problem{model, 0.5, q, constraints, x0, y0};
    solver.cfg.polish = false;
    const auto raw = solver.solve_with_cert(problem);
    solver.cfg.polish = true;
    const auto result = solver.solve_with_cert(problem);
    if (infeasible_seed) {
      EXPECT_FALSE(raw);
      EXPECT_FALSE(result);
    } else {
      ASSERT_TRUE(raw) << raw.error().message();
      ASSERT_TRUE(result) << result.error().message();
      EXPECT_FALSE(result->cert.polished);
      EXPECT_EQ(result->book, raw->book);
      EXPECT_EQ(result->cert.dual_res, raw->cert.dual_res);
    }
  }

  // The linear relaxation's minimizer is +2; the tracking cone requires |w|<=.5.
  // Its full primal and dual ADMM pair must survive the failed polish unchanged.
  risk::MaterializedConstraints cone;
  cone.A = la::MatX::Zero(0, 1);
  cone.l = la::VecX::Zero(0);
  cone.u = la::VecX::Zero(0);
  cone.tracking.active = true;
  cone.tracking.te_budget = 0.5;
  cone.tracking.w_bench = {0.0};
  const std::vector<atx::f64> cone_q{-2.0};
  solver = tight_solver();
  solver.cfg.polish = false;
  const auto raw = solver.solve_with_cert({model, 0.5, cone_q, cone});
  ASSERT_TRUE(raw) << raw.error().message();
  solver.cfg.polish = true;
  const auto result = solver.solve_with_cert({model, 0.5, cone_q, cone});
  ASSERT_TRUE(result) << result.error().message();
  EXPECT_FALSE(result->cert.polished);
  EXPECT_EQ(result->book, raw->book);
  EXPECT_EQ(result->cert.dual_res, raw->cert.dual_res);
  EXPECT_NEAR(result->book[0], 0.5, 1e-12);
}

} // namespace atxtest_risk_qp_polish_test
