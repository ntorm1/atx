// risk_qp_s83_test.cpp — S8.3: the four NEW ConstrainedQpSolver features.
//
// The S8.1/S8.2 rewrite turned the dense-PCG ADMM into a factor-augmented SPARSE
// direct-KKT ADMM; S8.3 layers four deterministic accuracy/diagnostic features on
// top of that fixed-iteration core (qp_solver.hpp §S8.3):
//
//   (B) RUIZ EQUILIBRATION  (cfg.ruiz_passes) — a symmetry-preserving diagonal
//       conditioner on the KKT system. It must NOT change the optimum (it is a
//       conditioner, not a different problem), and the whole solve stays
//       bit-deterministic with it on.
//   (C) DETERMINISTIC POLISH (cfg.polish) — an active-set reduced-KKT refinement
//       accepted ONLY if it stays feasible and does not raise the objective. The
//       contract that protects the regression pins is "polish NEVER degrades".
//   (D) INFEASIBILITY CERTIFICATE (solve_with_cert → QpResult::cert) — primal/dual
//       residuals + infeasibility flags computed from the final iterates.
//   (E/R6) WARM-START (QpProblem::x0/y0) — decoupled from termination: it changes
//       the iterate PATH, never the fixed loop count or the converged answer.
//
// Determinism (R1 / G-DET): every count is fixed — no RNG, no clock, no threading.
// Inputs are small hand-written FactorModels (M≈4–9, K=2). Byte-identity is checked
// with std::bit_cast<std::uint64_t>(double) element-wise, mirroring the existing
// RiskQpSolver.TwoSolvesByteIdentical idiom.

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

namespace atxtest_risk_qp_s83_test {

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
using atx::engine::risk::QpResult;

// ---------------------------------------------------------------------------
//  Fixtures (mirror risk_qp_solver_test.cpp — this is a separate TU, so the
//  helpers are re-declared here in this file's own anonymous namespace).
// ---------------------------------------------------------------------------

// Build a FactorModel from raw buffers (copies). Asserts success.
[[nodiscard]] FactorModel make_model(const MatX &x, const MatX &f, const VecX &d, usize fb = 0U,
                                     usize fe = 10U) {
  auto r = FactorModel::create(x, f, d, fb, fe);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// A benign multi-name FactorModel: M instruments, K=2 factors, F SPD, D > 0.
// (Byte-for-byte the same construction as the S1-2 fixture.)
[[nodiscard]] FactorModel make_multi_model(usize m) {
  const auto em = static_cast<Eigen::Index>(m);
  MatX x(em, 2);
  for (Eigen::Index i = 0; i < em; ++i) {
    x(i, 0) = 0.1 * static_cast<f64>(i) - 0.3;
    x(i, 1) = 0.05 * static_cast<f64>(i % 3) - 0.05;
  }
  MatX f(2, 2);
  f << 0.04, 0.01, 0.01, 0.09; // SPD
  VecX d(em);
  for (Eigen::Index i = 0; i < em; ++i) {
    d[i] = 0.10 + 0.02 * static_cast<f64>(i % 4);
  }
  return make_model(x, f, d);
}

// Materialize a ConstraintSet against X and M (asserts success).
[[nodiscard]] MaterializedConstraints materialize(const ConstraintSet &cs, const MatX &x, usize m) {
  auto r = cs.materialize(x, /*w_prev=*/{}, m);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// Materialize, then DISABLE the gross L1 budget (< 0 ⇒ no gross-L1 aux split).
[[nodiscard]] MaterializedConstraints materialize_no_gross(ConstraintSet cs, const MatX &x,
                                                           usize m) {
  MaterializedConstraints mc = materialize(cs, x, m);
  mc.gross_l1_budget = -1.0;
  return mc;
}

// Materialize the dense V = X F Xᵀ + diag(D) via apply() on the standard basis —
// apply() is trusted by RiskQpSolver.FactorModelApplyMatchesDenseForward, and F/D
// are private, so the basis sweep is the clean way to recover V for an objective.
[[nodiscard]] MatX v_from_apply(const FactorModel &model, usize m) {
  MatX v(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(m));
  for (usize j = 0; j < m; ++j) {
    std::vector<f64> e(m, 0.0), col(m, 0.0);
    e[j] = 1.0;
    model.apply(std::span<const f64>(e), std::span<f64>(col));
    for (usize i = 0; i < m; ++i) {
      v(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j)) = col[i];
    }
  }
  return v;
}

// The QP objective ½ wᵀP w + qᵀw with P = 2λV, computed in original w-space (the
// thing polish must never raise). Used by the polish-never-degrades test.
[[nodiscard]] f64 objective_w(const FactorModel &model, usize m, f64 lambda,
                              const std::vector<f64> &q, const std::vector<f64> &w) {
  const MatX v = v_from_apply(model, m);
  VecX wv(static_cast<Eigen::Index>(m));
  for (usize i = 0; i < m; ++i) {
    wv[static_cast<Eigen::Index>(i)] = w[i];
  }
  const VecX pw = (2.0 * lambda * v) * wv; // P w
  f64 quad = 0.0;
  for (usize i = 0; i < m; ++i) {
    quad += w[i] * pw[static_cast<Eigen::Index>(i)];
  }
  f64 lin = 0.0;
  for (usize i = 0; i < m; ++i) {
    lin += q[i] * w[i];
  }
  return 0.5 * quad + lin;
}

// q = −alpha for a small deterministic alpha pattern (length M).
[[nodiscard]] std::vector<f64> make_q(usize m) {
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    // alpha pattern in [-0.4, 0.4]; q = -alpha.
    const f64 alpha = 0.2 * static_cast<f64>((static_cast<int>(i) % 4) - 1);
    q[i] = -alpha;
  }
  return q;
}

// ===========================================================================
//  (B) RUIZ EQUILIBRATION
// ===========================================================================

// (B.a) Equilibration is a conditioner, not a different problem: ruiz_passes=10 and
// ruiz_passes=0 reach the SAME optimum on a well-conditioned constrained QP.
TEST(RiskQpSolver, RuizSameOptimumAsNoRuiz) {
  const usize m = 6U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.7;
  const std::vector<f64> q = make_q(m);

  // A well-conditioned linear-only set: dollar-neutral + a box + a factor bound.
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.4};
  cs.fexp = FactorExposure{{0U}, {0.25}};
  const MaterializedConstraints mc = materialize_no_gross(cs, model.exposures(), m);

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};

  ConstrainedQpSolver solver_on;
  solver_on.cfg.iters = 400U;
  solver_on.cfg.ruiz_passes = 10U;
  auto r_on = solver_on.solve(prob);
  ASSERT_TRUE(r_on.has_value()) << (r_on ? "" : r_on.error().to_string());

  ConstrainedQpSolver solver_off;
  solver_off.cfg.iters = 400U;
  solver_off.cfg.ruiz_passes = 0U; // raw, un-equilibrated path
  auto r_off = solver_off.solve(prob);
  ASSERT_TRUE(r_off.has_value()) << (r_off ? "" : r_off.error().to_string());

  const std::vector<f64> &won = *r_on;
  const std::vector<f64> &woff = *r_off;
  ASSERT_EQ(won.size(), m);
  ASSERT_EQ(woff.size(), m);
  // Loose tol: both are fixed-iteration ADMM books of the SAME QP; equilibration
  // only changes conditioning/convergence path, not the optimum it converges to.
  for (usize i = 0; i < m; ++i) {
    EXPECT_NEAR(won[i], woff[i], 1e-4) << "w[" << i << "] ruiz-on vs ruiz-off";
  }
}

// (B.b) Determinism with Ruiz ON: two solves are byte-identical.
TEST(RiskQpSolver, RuizSolveByteIdentical) {
  const usize m = 7U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.6;
  const std::vector<f64> q = make_q(m);

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.gross.gross_leverage = 1.0; // include the L1 aux-split in the determinism check
  cs.pos = PositionCap{0.5};
  const MaterializedConstraints mc = materialize(cs, model.exposures(), m);

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.ruiz_passes = 10U; // equilibration explicitly on

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
//  (C) DETERMINISTIC POLISH — never degrades feasibility or objective
// ===========================================================================
TEST(RiskQpSolver, PolishNeverDegradesObjective) {
  const usize m = 6U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.5;
  const std::vector<f64> q = make_q(m);

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.35};
  cs.fexp = FactorExposure{{0U, 1U}, {0.2, 0.2}};
  const MaterializedConstraints mc = materialize_no_gross(cs, model.exposures(), m);

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};

  ConstrainedQpSolver solver_polish;
  solver_polish.cfg.iters = 300U;
  solver_polish.cfg.polish = true;
  auto r_p = solver_polish.solve(prob);
  ASSERT_TRUE(r_p.has_value()) << (r_p ? "" : r_p.error().to_string());

  ConstrainedQpSolver solver_raw;
  solver_raw.cfg.iters = 300U;
  solver_raw.cfg.polish = false;
  auto r_r = solver_raw.solve(prob);
  ASSERT_TRUE(r_r.has_value()) << (r_r ? "" : r_r.error().to_string());

  const std::vector<f64> &wp = *r_p; // polished book
  const std::vector<f64> &wr = *r_r; // un-polished (ADMM) book
  ASSERT_EQ(wp.size(), m);
  ASSERT_EQ(wr.size(), m);

  // (1) The polished book is feasible (it passed the solver's R3 gate, re-check the
  //     linear rows here directly within feas_tol for an explicit contract).
  const f64 tol = solver_polish.cfg.feas_tol;
  VecX wpv(static_cast<Eigen::Index>(m));
  for (usize i = 0; i < m; ++i) {
    wpv[static_cast<Eigen::Index>(i)] = wp[i];
  }
  const VecX ax = mc.A * wpv;
  for (Eigen::Index rr = 0; rr < mc.A.rows(); ++rr) {
    EXPECT_LE(ax[rr], mc.u[rr] + tol) << "polished row " << rr << " upper";
    EXPECT_GE(ax[rr], mc.l[rr] - tol) << "polished row " << rr << " lower";
  }

  // (2) Polish NEVER degrades: objective(polished) ≤ objective(un-polished) + slack.
  //     The slack absorbs the un-polished ADMM book's own residual feasibility play
  //     (it is itself only fixed-iteration-accurate); the contract is "no worse".
  const f64 obj_p = objective_w(model, m, lambda, q, wp);
  const f64 obj_r = objective_w(model, m, lambda, q, wr);
  EXPECT_LE(obj_p, obj_r + 1e-6) << "polished obj " << obj_p << " vs raw obj " << obj_r;
}

// ===========================================================================
//  (D) INFEASIBILITY CERTIFICATE — small residuals on a feasible, well-cond problem
// ===========================================================================
TEST(RiskQpSolver, CertificateSmallResidualsOnFeasibleProblem) {
  const usize m = 6U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.7;
  const std::vector<f64> q = make_q(m);

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.4};
  cs.fexp = FactorExposure{{0U}, {0.25}};
  const MaterializedConstraints mc = materialize_no_gross(cs, model.exposures(), m);

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.iters = 400U;
  solver.cfg.polish = true;
  auto res = solver.solve_with_cert(prob);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const QpResult &out = *res;

  // The problem is feasible and well-conditioned: the primal residual (distance of
  // Ãx from the feasible band) must be at/under the feasibility tolerance — it
  // passed the R3 gate, which enforces exactly this in original units.
  EXPECT_LE(out.cert.prim_res, solver.cfg.feas_tol)
      << "prim_res = " << out.cert.prim_res;
  // Dual residual ‖Px + q̃ + Ãᵀy‖∞. With polish + 400 iters on a benign linear set
  // this is small; allow a loose 1e-3 bound — the dual is a fixed-iteration estimate
  // and is NOT gated to machine precision (no convergence early-exit, R1).
  EXPECT_LE(out.cert.dual_res, 1e-3) << "dual_res = " << out.cert.dual_res;
  // A clearly feasible book ⇒ the primal-infeasibility detector must NOT fire.
  EXPECT_FALSE(out.cert.primal_infeasible);
  // A bounded box QP is never dual-infeasible.
  EXPECT_FALSE(out.cert.dual_infeasible);
}

// ===========================================================================
//  (E / R6) WARM-START — decoupled from termination
// ===========================================================================

// (E.a) Cold-solve to w*, then warm-start the SAME problem with x0 = w* (length-M
// w-block warm-start). The result must be feasible and match the cold optimum: the
// warm-start changes the iterate path, NOT the fixed loop count or the answer.
TEST(RiskQpSolver, WarmStartMatchesColdOptimum) {
  const usize m = 6U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.7;
  const std::vector<f64> q = make_q(m);

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.4};
  cs.fexp = FactorExposure{{0U}, {0.25}};
  const MaterializedConstraints mc = materialize_no_gross(cs, model.exposures(), m);

  // Cold solve (no warm-start).
  QpProblem cold{model, lambda, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.iters = 400U;
  auto r_cold = solver.solve(cold);
  ASSERT_TRUE(r_cold.has_value()) << (r_cold ? "" : r_cold.error().to_string());
  const std::vector<f64> wstar = *r_cold; // copy — used as the warm-start seed

  // Warm solve: x0 = w* (length-M w-block; the solver seeds y/aux to 0 itself).
  QpProblem warm{model, lambda, std::span<const f64>(q), mc};
  warm.x0 = std::span<const f64>(wstar);
  auto r_warm = solver.solve(warm);
  ASSERT_TRUE(r_warm.has_value()) << (r_warm ? "" : r_warm.error().to_string());
  const std::vector<f64> &ww = *r_warm;
  ASSERT_EQ(ww.size(), m);

  // Feasible (re-check the linear rows within feas_tol).
  const f64 tol = solver.cfg.feas_tol;
  VecX wv(static_cast<Eigen::Index>(m));
  for (usize i = 0; i < m; ++i) {
    wv[static_cast<Eigen::Index>(i)] = ww[i];
  }
  const VecX ax = mc.A * wv;
  for (Eigen::Index rr = 0; rr < mc.A.rows(); ++rr) {
    EXPECT_LE(ax[rr], mc.u[rr] + tol) << "warm row " << rr << " upper";
    EXPECT_GE(ax[rr], mc.l[rr] - tol) << "warm row " << rr << " lower";
  }

  // Matches the cold optimum (warm-start is an accuracy lever, not a different
  // answer). Loose tol — both are fixed-iteration books of the same QP.
  for (usize i = 0; i < m; ++i) {
    EXPECT_NEAR(ww[i], wstar[i], 1e-4) << "w[" << i << "] warm vs cold";
  }
}

// (E.b) A warm-started solve is still byte-deterministic across two identical runs.
TEST(RiskQpSolver, WarmStartSolveByteIdentical) {
  const usize m = 6U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.7;
  const std::vector<f64> q = make_q(m);

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.4};
  const MaterializedConstraints mc = materialize_no_gross(cs, model.exposures(), m);

  // A fixed, hand-written warm-start seed (length-M w-block) — no RNG, no clock.
  std::vector<f64> x0(m);
  for (usize i = 0; i < m; ++i) {
    x0[i] = 0.05 * static_cast<f64>((static_cast<int>(i) % 3) - 1);
  }

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  prob.x0 = std::span<const f64>(x0);
  ConstrainedQpSolver solver;
  solver.cfg.iters = 300U;

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

} // namespace atxtest_risk_qp_s83_test
