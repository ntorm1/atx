#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/risk/kkt_ldl.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atxtest_risk_kkt_ldl_test {

using atx::core::ErrorCode;
using atx::engine::risk::QuasiDefiniteLdl;

QuasiDefiniteLdl::SpMat tiny_kkt() {
  Eigen::Matrix3d dense;
  dense << 4.0, 1.0, 2.0, 1.0, 3.0, -1.0, 2.0, -1.0, -2.0;
  QuasiDefiniteLdl::SpMat sparse = dense.sparseView();
  sparse.makeCompressed();
  return sparse;
}

// This fully populated 3x3 matrix has three strict-lower factor entries under
// every permutation. Count payload elements independently of the factor arrays.
constexpr atx::u64 kTinyFactorBytes = 7 * sizeof(atx::usize) + 9 * sizeof(atx::f64);

TEST(RiskKktLdl, ExactActualFillBudgetPreservesFactorAndSolve) {
  const auto matrix = tiny_kkt();
  QuasiDefiniteLdl reference;
  ASSERT_TRUE(reference.factor_symbolic(matrix));
  ASSERT_TRUE(reference.factor_numeric(matrix));
  QuasiDefiniteLdl limited;
  ASSERT_TRUE(limited.factor_symbolic(matrix, kTinyFactorBytes));
  ASSERT_TRUE(limited.factor_numeric(matrix));
  ASSERT_EQ(limited.Lx().size(), 3U);
  EXPECT_EQ(limited.Lp(), reference.Lp());
  EXPECT_EQ(limited.Li(), reference.Li());
  EXPECT_EQ(limited.Lx(), reference.Lx());
  EXPECT_EQ(limited.diag(), reference.diag());
  EXPECT_EQ(limited.diag_inv(), reference.diag_inv());
  const std::array<atx::f64, 3> rhs{1.0, -2.0, 0.5};
  std::array<atx::f64, 3> expected{}, actual{};
  reference.solve(rhs, expected);
  limited.solve(rhs, actual);
  EXPECT_EQ(actual, expected);
  Eigen::Vector3d x(actual[0], actual[1], actual[2]);
  const Eigen::Vector3d residual = matrix * x - Eigen::Vector3d(rhs[0], rhs[1], rhs[2]);
  EXPECT_LT(residual.cwiseAbs().maxCoeff(), 1e-12);

  QuasiDefiniteLdl rejected;
  const auto status = rejected.factor_symbolic(matrix, kTinyFactorBytes - 1);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::OutOfRange);
  EXPECT_NE(status.error().message().find("max_factor_bytes"), std::string::npos);
  EXPECT_TRUE(rejected.Lp().empty());
  EXPECT_TRUE(rejected.Li().empty());
  EXPECT_TRUE(rejected.Lx().empty());
  EXPECT_TRUE(rejected.diag().empty());
  EXPECT_TRUE(rejected.diag_inv().empty());
  EXPECT_FALSE(rejected.symbolic_ready());
  EXPECT_FALSE(rejected.numeric_ready());
}

TEST(RiskKktLdl, RejectedSymbolicRefactorInvalidatesPriorFactor) {
  const auto matrix = tiny_kkt();
  QuasiDefiniteLdl factor;
  ASSERT_TRUE(factor.factor_symbolic(matrix));
  ASSERT_TRUE(factor.factor_numeric(matrix));
  const auto prior_values = factor.Lx();
  ASSERT_FALSE(factor.factor_symbolic(matrix, 0));
  EXPECT_FALSE(factor.symbolic_ready());
  EXPECT_FALSE(factor.numeric_ready());
  EXPECT_EQ(factor.Lx(), prior_values); // rejected before replacing payload storage
  EXPECT_FALSE(factor.factor_numeric(matrix)); // cannot reuse the old symbolic factor

  ASSERT_TRUE(factor.factor_symbolic(matrix, kTinyFactorBytes));
  ASSERT_TRUE(factor.factor_numeric(matrix));
  QuasiDefiniteLdl::SpMat rectangular(2, 3);
  ASSERT_FALSE(factor.factor_symbolic(rectangular));
  EXPECT_FALSE(factor.symbolic_ready());
  EXPECT_FALSE(factor.numeric_ready());
  EXPECT_FALSE(factor.factor_numeric(matrix));
}

TEST(RiskKktLdl, FailedNumericRefactorCannotRemainReady) {
  const auto matrix = tiny_kkt();
  QuasiDefiniteLdl factor;
  ASSERT_TRUE(factor.factor_symbolic(matrix));
  ASSERT_TRUE(factor.factor_numeric(matrix));
  auto singular = matrix;
  std::fill(singular.valuePtr(), singular.valuePtr() + singular.nonZeros(), 0.0);
  ASSERT_FALSE(factor.factor_numeric(singular));
  EXPECT_TRUE(factor.symbolic_ready());
  EXPECT_FALSE(factor.numeric_ready());
  ASSERT_TRUE(factor.factor_numeric(matrix));
  EXPECT_TRUE(factor.numeric_ready());
}

TEST(RiskKktLdl, QpFactorBudgetPropagatesWithAndWithoutPolish) {
  namespace risk = atx::engine::risk;
  namespace cl = atx::core::linalg;
  cl::MatX exposures(1, 1);
  exposures << 0.2;
  cl::MatX covariance(1, 1);
  covariance << 0.04;
  cl::VecX specific(1);
  specific << 0.1;
  auto model = risk::FactorModel::create(exposures, covariance, specific, 0U, 10U);
  ASSERT_TRUE(model) << (model ? "" : model.error().to_string());
  risk::ConstraintSet constraints;
  constraints.gross.dollar_neutral = false;
  constraints.pos = risk::PositionCap{0.5};
  auto materialized = constraints.materialize(model->exposures(), {}, 1U);
  ASSERT_TRUE(materialized);
  materialized->gross_l1_budget = -1.0;
  const std::array<atx::f64, 1> q{-1.0};
  const risk::QpProblem problem{*model, 1.0, std::span<const atx::f64>(q), *materialized};
  risk::ConstrainedQpSolver solver;
  for (const bool polish : {false, true}) {
    solver.cfg.polish = polish;
    solver.cfg.max_factor_bytes = std::numeric_limits<atx::u64>::max();
    const auto reference = solver.solve_with_cert(problem);
    ASSERT_TRUE(reference) << (reference ? "" : reference.error().to_string());
    solver.cfg.max_factor_bytes = 4096;
    const auto limited = solver.solve_with_cert(problem);
    ASSERT_TRUE(limited) << (limited ? "" : limited.error().to_string());
    EXPECT_EQ(limited->book, reference->book);
    EXPECT_EQ(limited->cert.polished, reference->cert.polished);
    solver.cfg.max_factor_bytes = 0;
    const auto rejected = solver.solve_with_cert(problem);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code(), ErrorCode::OutOfRange);
    EXPECT_NE(rejected.error().message().find("max_factor_bytes"), std::string::npos);
  }
}

} // namespace atxtest_risk_kkt_ldl_test
