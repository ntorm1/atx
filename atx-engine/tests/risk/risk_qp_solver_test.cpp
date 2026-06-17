// risk_qp_solver_test.cpp — S1-2: deterministic fixed-iteration constrained ADMM.
//
// ConstrainedQpSolver solves   min ½ wᵀP w + qᵀw  s.t.  l ≤ A w ≤ u
// with P = 2λV applied MATRIX-FREE through FactorModel::apply (V is the factored
// Barra covariance and is NEVER materialized M×M), a FIXED outer ADMM iteration
// count (no convergence early-exit), a FIXED inner PCG x-update, and a post-loop
// feasibility gate that turns an infeasible set into Err(InvalidArgument) instead
// of a silently-clamped book. The L1 budgets (gross Σ|w|≤L, turnover Σ|Δw|≤T) are
// folded via the standard auxiliary-variable split.
//
// Coverage (the S1-2 contract):
//   1. FactorModel.apply differential (R8): apply(v) == dense X F Xᵀ v + D∘v.
//   2. Equality-constrained optimum (sanity): dollar-neutral-only QP recovers the
//      analytic KKT solution within a loose tol.
//   3. Differential vs a DENSE fixed-K ADMM (R8): factored apply == dense apply.
//   4. Constraint satisfaction (R3): every claimed linear / L1 constraint holds at
//      the returned book within feas_tol.
//   5. Infeasible ⇒ Err(InvalidArgument): a provably-contradictory set returns Err.
//   6. Determinism (R1): two solves ⇒ byte-identical std::vector<f64>.
//   7. Boundaries: q-length mismatch ⇒ Err; single-name universe; q=0 feasible book;
//      zero gross budget ⇒ all-zero w.

#include <bit>     // std::bit_cast
#include <cmath>   // std::fabs, std::isfinite
#include <cstdint> // std::uint64_t
#include <span>
#include <utility> // std::move
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atxtest_risk_qp_solver_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::ConstrainedQpSolver;
using atx::engine::risk::ConstraintSet;
using atx::engine::risk::FactorExposure;
using atx::engine::risk::FactorModel;
using atx::engine::risk::MaterializedConstraints;
using atx::engine::risk::PositionCap;
using atx::engine::risk::QpConfig;
using atx::engine::risk::QpProblem;
using atx::engine::risk::TurnoverBudget;

// ---------------------------------------------------------------------------
//  Fixtures
// ---------------------------------------------------------------------------

// Build a FactorModel from raw buffers (copies). Asserts success.
[[nodiscard]] FactorModel make_model(const MatX &x, const MatX &f, const VecX &d, usize fb = 0U,
                                     usize fe = 10U) {
  auto r = FactorModel::create(x, f, d, fb, fe);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// A benign multi-name FactorModel: M instruments, K=2 factors, F SPD, D > 0.
[[nodiscard]] FactorModel make_multi_model(usize m) {
  const auto em = static_cast<Eigen::Index>(m);
  MatX x(em, 2);
  for (Eigen::Index i = 0; i < em; ++i) {
    x(i, 0) = 0.1 * static_cast<f64>(i) - 0.3; // spread of loadings
    x(i, 1) = 0.05 * static_cast<f64>(i % 3) - 0.05;
  }
  MatX f(2, 2);
  f << 0.04, 0.01, 0.01, 0.09; // SPD
  VecX d(em);
  for (Eigen::Index i = 0; i < em; ++i) {
    d[i] = 0.10 + 0.02 * static_cast<f64>(i % 4); // positive, varied
  }
  return make_model(x, f, d);
}

// Dense V = X F Xᵀ + diag(D) — TEST-ONLY oracle (production never materializes it).
[[nodiscard]] MatX dense_v(const MatX &x, const MatX &f, const VecX &d) {
  MatX v = x * f * x.transpose();
  for (Eigen::Index i = 0; i < d.size(); ++i) {
    v(i, i) += d[i];
  }
  return v;
}

// ---------------------------------------------------------------------------
//  A DENSE fixed-K ADMM reference — same algorithm, V materialized.  Used by the
//  differential test (#3) to isolate "factored apply == dense apply". It folds NO
//  L1 budgets: it runs over the LINEAR rows l ≤ A w ≤ u only, with the same fixed
//  outer/inner iteration counts and the same ρ/σ as the solver under test.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<f64> dense_admm_linear(const MatX &P, const VecX &q, const MatX &A,
                                                 const VecX &l, const VecX &u,
                                                 const QpConfig &cfg) {
  const Eigen::Index n = P.rows();
  const Eigen::Index r = A.rows();
  // KKT matrix for the x-update: (P + σI + ρ AᵀA), solved exactly (dense SPD).
  MatX kkt = P;
  kkt += cfg.sigma * MatX::Identity(n, n);
  kkt += cfg.rho * (A.transpose() * A);
  Eigen::LLT<MatX> kkt_llt(kkt);

  VecX x = VecX::Zero(n);
  VecX z = VecX::Zero(r);
  VecX y = VecX::Zero(r);
  for (usize k = 0U; k < cfg.iters; ++k) {
    const VecX rhs = cfg.sigma * x - q + A.transpose() * (cfg.rho * z - y);
    x = kkt_llt.solve(rhs);
    const VecX ax = A * x;
    z = (ax + y / cfg.rho).cwiseMax(l).cwiseMin(u); // clamp into [l, u]
    y += cfg.rho * (ax - z);
  }
  std::vector<f64> out(static_cast<usize>(n));
  for (Eigen::Index i = 0; i < n; ++i) {
    out[static_cast<usize>(i)] = x[i];
  }
  return out;
}

// Materialize a ConstraintSet against X and M (asserts success).
[[nodiscard]] MaterializedConstraints materialize(const ConstraintSet &cs, const MatX &x, usize m) {
  auto r = cs.materialize(x, /*w_prev=*/{}, m);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// Materialize, then DISABLE the gross L1 budget (gross_l1_budget < 0 ⇒ no gross-L1
// cap, the solver's "no gross aux-split" path). ConstraintSet::materialize always
// emits gross_l1_budget = gross.gross_leverage (≥ 0), so a test that wants to isolate
// the linear / turnover behavior flips the sentinel directly — exactly what the
// MaterializedConstraints contract documents (< 0 ⇒ no gross cap).
[[nodiscard]] MaterializedConstraints materialize_no_gross(ConstraintSet cs, const MatX &x,
                                                           usize m) {
  MaterializedConstraints mc = materialize(cs, x, m);
  mc.gross_l1_budget = -1.0; // disable the gross-L1 aux split
  return mc;
}

// ===========================================================================
//  1. FactorModel.apply differential (R8)
// ===========================================================================
TEST(RiskQpSolver, FactorModelApplyMatchesDenseForward) {
  MatX x(4, 2);
  x << 1.0, 0.0, 0.5, -1.0, -2.0, 0.3, 0.4, 0.7;
  MatX f(2, 2);
  f << 0.04, 0.01, 0.01, 0.09;
  VecX d(4);
  d << 0.10, 0.20, 0.05, 0.15;
  const FactorModel model = make_model(x, f, d);
  const MatX vd = dense_v(x, f, d);

  // Several probe vectors, including a NaN-free varied multi-name fixture.
  std::vector<VecX> probes;
  {
    VecX v(4);
    v << 1.0, -1.0, 1.0, -1.0;
    probes.push_back(v);
    v << 0.3, 0.0, -0.7, 2.1;
    probes.push_back(v);
    v << -1.5, 0.4, 0.0, 0.9;
    probes.push_back(v);
  }
  for (const VecX &v : probes) {
    std::vector<f64> in(4), out(4);
    for (Eigen::Index i = 0; i < 4; ++i) {
      in[static_cast<usize>(i)] = v[i];
    }
    model.apply(std::span<const f64>(in), std::span<f64>(out));
    const VecX ref = vd * v;
    for (Eigen::Index i = 0; i < 4; ++i) {
      ASSERT_TRUE(std::isfinite(out[static_cast<usize>(i)]));
      EXPECT_NEAR(out[static_cast<usize>(i)], ref[i], 1e-12) << "row " << i;
    }
  }
}

TEST(RiskQpSolver, SpecificVarReturnsFlooredDiagonal) {
  MatX x(3, 1);
  x << 1.0, -1.0, 0.5;
  MatX f(1, 1);
  f << 0.04;
  VecX d(3);
  d << 0.10, 0.0, 0.20; // middle entry floored
  const FactorModel model = make_model(x, f, d);
  const VecX &dv = model.specific_var();
  ASSERT_EQ(dv.size(), 3);
  EXPECT_NEAR(dv[0], 0.10, 1e-15);
  EXPECT_GT(dv[1], 0.0);  // floored away from zero
  EXPECT_LT(dv[1], 1e-6); // but tiny
  EXPECT_NEAR(dv[2], 0.20, 1e-15);
}

// ===========================================================================
//  2. Equality-constrained optimum (dollar-neutral only) — analytic KKT pin
// ===========================================================================
TEST(RiskQpSolver, DollarNeutralRecoversAnalyticOptimum) {
  const usize m = 6U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 1.0;

  // q = -alpha (the solver minimizes ½wᵀP w + qᵀw with P = 2λV).
  VecX alpha(static_cast<Eigen::Index>(m));
  alpha << 0.5, -0.2, 0.3, 0.1, -0.4, 0.2;
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  // Constraints: dollar-neutral only (Σw = 0). No box, no L1.
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  const MaterializedConstraints mc = materialize_no_gross(cs, model.exposures(), m);

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.iters = 400U;
  solver.cfg.kkt_iters = 80U;
  auto res = solver.solve(prob);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const std::vector<f64> &w = *res;
  ASSERT_EQ(w.size(), m);

  // Analytic equality-constrained QP: minimize ½wᵀ(2λV)w + qᵀw s.t. 1ᵀw = 0.
  //   [2λV  1][w]   [-q]
  //   [1ᵀ   0][μ] = [ 0]
  // Build V from apply on the standard basis (apply is trusted via test #1; F/D are
  // private on the model, so the basis sweep is the clean way to materialize V here).
  MatX vapply(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(m));
  for (usize j = 0; j < m; ++j) {
    std::vector<f64> e(m, 0.0), col(m, 0.0);
    e[j] = 1.0;
    model.apply(std::span<const f64>(e), std::span<f64>(col));
    for (usize i = 0; i < m; ++i) {
      vapply(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = col[i];
    }
  }
  const Eigen::Index em = static_cast<Eigen::Index>(m);
  MatX kkt = MatX::Zero(em + 1, em + 1);
  kkt.topLeftCorner(em, em) = 2.0 * lambda * vapply;
  for (Eigen::Index i = 0; i < em; ++i) {
    kkt(em, i) = 1.0;
    kkt(i, em) = 1.0;
  }
  VecX rhs = VecX::Zero(em + 1);
  for (Eigen::Index i = 0; i < em; ++i) {
    rhs[i] = -q[static_cast<usize>(i)];
  }
  const VecX sol = kkt.fullPivLu().solve(rhs);
  for (Eigen::Index i = 0; i < em; ++i) {
    EXPECT_NEAR(w[static_cast<usize>(i)], sol[i], 1e-4) << "w[" << i << "]";
  }
}

// ===========================================================================
//  3. Differential vs a DENSE fixed-K ADMM (R8) — factored == dense apply
// ===========================================================================
TEST(RiskQpSolver, FactoredAdmmMatchesDenseAdmmLinear) {
  const usize m = 12U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.7;

  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.1 * static_cast<f64>((i % 5) - 2);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  // Linear constraints only: dollar-neutral + a box + a factor-exposure bound.
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.5};
  cs.fexp = FactorExposure{{0U}, {0.2}}; // |(Xᵀw)_0| ≤ 0.2
  // No L1 budget here — pure linear differential vs the dense ADMM reference.
  const MaterializedConstraints mc = materialize_no_gross(cs, model.exposures(), m);

  QpConfig cfg;
  cfg.iters = 300U;
  cfg.kkt_iters = 120U; // enough PCG to match the dense exact x-update closely
  cfg.rho = 1.0;
  cfg.sigma = 1e-6;

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg = cfg;
  auto res = solver.solve(prob);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const std::vector<f64> &w = *res;

  // Dense reference ADMM over the SAME linear rows, V materialized via apply.
  MatX vapply(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(m));
  for (usize j = 0; j < m; ++j) {
    std::vector<f64> e(m, 0.0), col(m, 0.0);
    e[j] = 1.0;
    model.apply(std::span<const f64>(e), std::span<f64>(col));
    for (usize i = 0; i < m; ++i) {
      vapply(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = col[i];
    }
  }
  VecX qv(static_cast<Eigen::Index>(m));
  for (usize i = 0; i < m; ++i) {
    qv[static_cast<Eigen::Index>(i)] = q[i];
  }
  const MatX P = 2.0 * lambda * vapply;
  const std::vector<f64> ref = dense_admm_linear(P, qv, mc.A, mc.l, mc.u, cfg);

  ASSERT_EQ(w.size(), ref.size());
  for (usize i = 0; i < m; ++i) {
    EXPECT_NEAR(w[i], ref[i], 1e-6) << "w[" << i << "]";
  }
}

// ===========================================================================
//  4. Constraint satisfaction (R3): every claimed constraint holds at the book
// ===========================================================================
TEST(RiskQpSolver, ReturnedBookSatisfiesLinearConstraints) {
  const usize m = 10U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.5;

  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.2 * static_cast<f64>((i % 4) - 1);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.25};
  cs.fexp = FactorExposure{{0U, 1U}, {0.15, 0.15}};
  const MaterializedConstraints mc = materialize_no_gross(cs, model.exposures(), m);

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.iters = 400U;
  solver.cfg.kkt_iters = 100U;
  auto res = solver.solve(prob);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const std::vector<f64> &w = *res;

  const f64 tol = solver.cfg.feas_tol;
  // Every linear row A w ∈ [l, u] within feas_tol.
  VecX wv(static_cast<Eigen::Index>(m));
  for (usize i = 0; i < m; ++i) {
    wv[static_cast<Eigen::Index>(i)] = w[i];
  }
  const VecX ax = mc.A * wv;
  for (Eigen::Index r = 0; r < mc.A.rows(); ++r) {
    EXPECT_LE(ax[r], mc.u[r] + tol) << "row " << r << " upper";
    EXPECT_GE(ax[r], mc.l[r] - tol) << "row " << r << " lower";
  }
}

// ===========================================================================
//  4b. Gross L1 budget satisfaction (aux-split) — Σ|w| ≤ L at the book.
// ===========================================================================
TEST(RiskQpSolver, GrossL1BudgetHoldsAtBook) {
  const usize m = 8U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.3;

  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.3 * static_cast<f64>((i % 3) - 1); // pushes toward large gross
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  const f64 gross_budget = 1.0;
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.gross.gross_leverage = gross_budget; // Σ|w| ≤ 1.0
  const MaterializedConstraints mc = materialize(cs, model.exposures(), m);

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  // Rely on the shipped default iters (600) — the gross aux-split converges under it.
  solver.cfg.kkt_iters = 80U;
  auto res = solver.solve(prob);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const std::vector<f64> &w = *res;

  f64 gross = 0.0;
  for (const f64 wi : w) {
    gross += std::fabs(wi);
  }
  // Gross within budget + a tolerance band (ADMM is fixed-iteration, not exact).
  EXPECT_LE(gross, gross_budget + 5e-2) << "gross = " << gross;
}

// ===========================================================================
//  4c. Turnover L1 budget satisfaction (aux-split) — Σ|w − ref| ≤ T at the book.
// ===========================================================================
TEST(RiskQpSolver, TurnoverL1BudgetHoldsAtBook) {
  const usize m = 8U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.3;

  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.4 * static_cast<f64>((i % 3) - 1);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  // w_prev reference (a small existing book); cap turnover tightly.
  std::vector<f64> w_prev(m, 0.0);
  w_prev[0] = 0.1;
  w_prev[1] = -0.1;
  const f64 turnover_budget = 0.3;

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.turn = TurnoverBudget{turnover_budget};
  auto mcr = cs.materialize(model.exposures(), std::span<const f64>(w_prev), m);
  ASSERT_TRUE(mcr.has_value()) << (mcr ? "" : mcr.error().to_string());
  MaterializedConstraints mc = std::move(*mcr);
  mc.gross_l1_budget = -1.0; // isolate the turnover budget (no gross aux-split)

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  // Rely on the shipped default iters (600) — the turnover aux-split converges under it.
  solver.cfg.kkt_iters = 80U;
  auto res = solver.solve(prob);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const std::vector<f64> &w = *res;

  f64 turnover = 0.0;
  for (usize i = 0; i < m; ++i) {
    turnover += std::fabs(w[i] - w_prev[i]);
  }
  EXPECT_LE(turnover, turnover_budget + 5e-2) << "turnover = " << turnover;
}

// ===========================================================================
//  5. Infeasible ⇒ Err(InvalidArgument)
// ===========================================================================
TEST(RiskQpSolver, InfeasibleConstraintsReturnErr) {
  // Construct a provably-infeasible LINEAR set: a single name forced to be both
  // ≥ +1 (lower bound) and ≤ -1 (upper bound) on the same row is impossible. We
  // build the MaterializedConstraints directly so we can pin contradictory bounds.
  const usize m = 3U;
  const FactorModel model = make_multi_model(m);

  std::vector<f64> q(m, 0.0);

  MaterializedConstraints mc;
  mc.A = MatX::Zero(1, static_cast<Eigen::Index>(m));
  mc.A(0, 0) = 1.0; // row picks out w_0
  mc.l = VecX::Constant(1, 1.0);
  mc.u = VecX::Constant(1, -1.0); // l > u on the same row ⇒ no feasible w_0
  mc.gross_l1_budget = -1.0;
  mc.has_turnover = false;

  QpProblem prob{model, 1.0, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.iters = 200U;
  auto res = solver.solve(prob);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(RiskQpSolver, ZeroGrossBudgetForcesZeroBook) {
  // Σ|w| ≤ 0 ⇒ the only feasible book is w == 0 (a feasible, not infeasible, set).
  const usize m = 5U;
  const FactorModel model = make_multi_model(m);

  VecX alpha(static_cast<Eigen::Index>(m));
  alpha << 0.5, -0.3, 0.2, 0.4, -0.1;
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  ConstraintSet cs;
  cs.gross.dollar_neutral = false; // dollar-neutral is redundant with all-zero
  cs.gross.gross_leverage = 0.0;   // Σ|w| ≤ 0
  const MaterializedConstraints mc = materialize(cs, model.exposures(), m);

  QpProblem prob{model, 1.0, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.iters = 400U;
  solver.cfg.kkt_iters = 60U;
  auto res = solver.solve(prob);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const std::vector<f64> &w = *res;
  for (usize i = 0; i < m; ++i) {
    EXPECT_NEAR(w[i], 0.0, 1e-3) << "w[" << i << "]";
  }
}

// ===========================================================================
//  6. Determinism (R1): two solves ⇒ byte-identical std::vector<f64>
// ===========================================================================
TEST(RiskQpSolver, TwoSolvesByteIdentical) {
  const usize m = 9U;
  const FactorModel model = make_multi_model(m);

  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.15 * static_cast<f64>((i % 6) - 3);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.gross.gross_leverage = 1.0; // include the L1 split in the determinism check
  cs.pos = PositionCap{0.4};
  const MaterializedConstraints mc = materialize(cs, model.exposures(), m);

  QpProblem prob{model, 0.8, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  // Rely on the shipped default iters (600) — clears the gross aux-split feas gate.
  solver.cfg.kkt_iters = 60U;

  auto r1 = solver.solve(prob);
  auto r2 = solver.solve(prob);
  ASSERT_TRUE(r1.has_value()) << (r1 ? "" : r1.error().to_string());
  ASSERT_TRUE(r2.has_value()) << (r2 ? "" : r2.error().to_string());
  const std::vector<f64> &a = *r1;
  const std::vector<f64> &b = *r2;
  ASSERT_EQ(a.size(), b.size());
  for (usize i = 0; i < a.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a[i]), std::bit_cast<std::uint64_t>(b[i]))
        << "element " << i;
  }
}

// ===========================================================================
//  7. Boundaries
// ===========================================================================
TEST(RiskQpSolver, QLengthMismatchReturnsErr) {
  const usize m = 4U;
  const FactorModel model = make_multi_model(m);
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  const MaterializedConstraints mc = materialize_no_gross(cs, model.exposures(), m);

  std::vector<f64> q(m - 1U, 0.0); // wrong length
  QpProblem prob{model, 1.0, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  auto res = solver.solve(prob);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(RiskQpSolver, SingleNameUniverse) {
  // M=1: with a box |w_0| ≤ 0.5 and q pushing positive, the optimum pins near +0.5
  // (or to the unconstrained risk optimum if interior). Just assert feasibility +
  // finiteness — a tiny universe must not crash or NaN.
  MatX x(1, 1);
  x << 0.2;
  MatX f(1, 1);
  f << 0.04;
  VecX d(1);
  d << 0.1;
  const FactorModel model = make_model(x, f, d);

  std::vector<f64> q{-1.0}; // alpha = +1 ⇒ wants w_0 > 0
  ConstraintSet cs;
  cs.gross.dollar_neutral = false;
  cs.pos = PositionCap{0.5};
  const MaterializedConstraints mc = materialize_no_gross(cs, model.exposures(), 1U);

  QpProblem prob{model, 1.0, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.iters = 300U;
  solver.cfg.kkt_iters = 40U;
  auto res = solver.solve(prob);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const std::vector<f64> &w = *res;
  ASSERT_EQ(w.size(), 1U);
  ASSERT_TRUE(std::isfinite(w[0]));
  EXPECT_LE(std::fabs(w[0]), 0.5 + solver.cfg.feas_tol);
  EXPECT_GT(w[0], 0.0); // alpha pushes positive
}

TEST(RiskQpSolver, ZeroObjectiveFeasibleBook) {
  // q = 0, λ small: with only a box, the all-zero book is optimal AND feasible. The
  // solver must return a feasible (here ~zero) book without error.
  const usize m = 6U;
  const FactorModel model = make_multi_model(m);
  std::vector<f64> q(m, 0.0);

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.3};
  const MaterializedConstraints mc = materialize_no_gross(cs, model.exposures(), m);

  QpProblem prob{model, 1.0, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.iters = 200U;
  solver.cfg.kkt_iters = 40U;
  auto res = solver.solve(prob);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const std::vector<f64> &w = *res;
  for (usize i = 0; i < m; ++i) {
    ASSERT_TRUE(std::isfinite(w[i]));
    EXPECT_LE(std::fabs(w[i]), 0.3 + solver.cfg.feas_tol);
  }
}


}  // namespace atxtest_risk_qp_solver_test
