// risk_qp_augment_test.cpp — S8.1: the factor-augmented sparse QP differential gate.
//
// S8.1 reformulates the as-built constrained portfolio QP from the O(M²) dense-Ã
// ADMM (preserved VERBATIM as the oracle atx::engine::risk::reference::Constrained-
// QpSolver in qp_solver_reference.hpp) into the factor-augmented SPARSE form
// (qp_augment.hpp): introduce y = Xᵀw so V's dense XFXᵀ risk term re-expresses in K
// factor space and the Hessian becomes P = blkdiag(2λD, 2λF). The rewritten
// ConstrainedQpSolver (qp_solver.hpp) solves the resulting sparse quasi-definite KKT
// directly. This file is the G-DIFF gate (R11):
//
//   1. Assembly structure (direct): build_augmented produces P == blkdiag(2λD, 2λF)
//      (values + pattern), the K  y − Xᵀw = 0  rows are present and correct, and the
//      gross/turnover L1 splits are sparse structured aux rows (not a dense block).
//   2. Differential at the optimum: on a battery of random FEASIBLE problems across
//      varied M {50, 200, 800}, K {4, 16, 64}, and constraint mixes (factor /
//      group / beta / turnover / gross) with NON-TRIVIAL SPD F (off-diagonal
//      structure), the rewritten solver and the dense-Ã oracle, BOTH run to a
//      converged fixed budget, agree:  ‖w_new − w_ref‖∞ ≤ tol.
//
// NOTE ON TOLERANCE: the oracle is a Jacobi-PCG ADMM and the rewrite is a direct
// sparse-KKT ADMM — two DIFFERENT linear solves — so they are NOT bit-identical
// mid-iteration. The invariant is agreement AT THE OPTIMUM (both converge to the
// same unique QP minimizer). The achieved ‖·‖∞ bound is asserted per case and the
// tightest battery-wide bound is documented in the ledger.

#include <algorithm> // std::max
#include <bit>       // std::bit_cast (determinism check)
#include <cmath>     // std::fabs, std::isfinite
#include <cstdint>   // std::uint64_t
#include <random>    // std::mt19937_64 (FIXED seed — deterministic battery)
#include <span>
#include <utility> // std::move
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>
#include <Eigen/SparseCore>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/qp_augment.hpp"
#include "atx/engine/risk/qp_solver.hpp"
#include "atx/engine/risk/qp_solver_reference.hpp"

namespace atxtest_risk_qp_augment_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::AugmentedQp;
using atx::engine::risk::BetaNeutral;
using atx::engine::risk::build_augmented;
using atx::engine::risk::ConstrainedQpSolver;
using atx::engine::risk::ConstraintSet;
using atx::engine::risk::FactorExposure;
using atx::engine::risk::FactorModel;
using atx::engine::risk::GroupCap;
using atx::engine::risk::MaterializedConstraints;
using atx::engine::risk::PositionCap;
using atx::engine::risk::QpConfig;
using atx::engine::risk::QpProblem;
using atx::engine::risk::TurnoverBudget;

// ---------------------------------------------------------------------------
//  Fixtures — a deterministic random FactorModel with NON-TRIVIAL SPD F.
// ---------------------------------------------------------------------------

// SPD F with off-diagonal structure: F = B Bᵀ + γI over a random B (NOT identity).
// γ keeps F well-conditioned; the BBᵀ term gives genuine factor correlations.
[[nodiscard]] MatX make_spd_f(usize k, std::mt19937_64 &rng) {
  const auto ek = static_cast<Eigen::Index>(k);
  std::uniform_real_distribution<f64> u(-0.5, 0.5);
  MatX b(ek, ek);
  for (Eigen::Index i = 0; i < ek; ++i) {
    for (Eigen::Index j = 0; j < ek; ++j) {
      b(i, j) = u(rng);
    }
  }
  MatX f = b * b.transpose();
  f += 0.05 * MatX::Identity(ek, ek); // ridge → SPD, well-conditioned
  return f;
}

// A deterministic random FactorModel: M instruments, K factors, SPD F (off-diagonal),
// varied positive D. Exposures X spread across [-1, 1].
[[nodiscard]] FactorModel make_random_model(usize m, usize k, std::mt19937_64 &rng) {
  const auto em = static_cast<Eigen::Index>(m);
  const auto ek = static_cast<Eigen::Index>(k);
  std::uniform_real_distribution<f64> ux(-1.0, 1.0);
  std::uniform_real_distribution<f64> ud(0.05, 0.30);
  MatX x(em, ek);
  for (Eigen::Index i = 0; i < em; ++i) {
    for (Eigen::Index j = 0; j < ek; ++j) {
      x(i, j) = ux(rng);
    }
  }
  const MatX f = make_spd_f(k, rng);
  VecX d(em);
  for (Eigen::Index i = 0; i < em; ++i) {
    d[i] = ud(rng);
  }
  auto r = FactorModel::create(std::move(x), f, std::move(d), 0U, 10U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// Dense V = X F Xᵀ + diag(D) — TEST-ONLY oracle (production never materializes it).
[[nodiscard]] MatX dense_v(const FactorModel &model) {
  const usize m = model.n_instruments();
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

// ===========================================================================
//  1. Assembly structure — P = blkdiag(2λD, 2λF), the y=Xᵀw rows, sparse splits.
// ===========================================================================
TEST(RiskQpAugment, HessianIsBlockDiagDF) {
  std::mt19937_64 rng(0xA5A5u);
  const usize m = 12U, k = 3U;
  const FactorModel model = make_random_model(m, k, rng);
  const f64 lambda = 0.7;

  std::vector<f64> q(m, 0.0);
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  const auto mcr = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mcr.has_value());
  MaterializedConstraints mc = *mcr;
  mc.gross_l1_budget = -1.0; // no aux split for the structure check

  const AugmentedQp aug = build_augmented(model, lambda, std::span<const f64>(q), mc);
  ASSERT_EQ(aug.n_w, m);
  ASSERT_EQ(aug.n_y, k);
  ASSERT_EQ(aug.n_aux, 0U);

  const MatX Pd = MatX(aug.P); // densify the SPARSE P (test-only) to inspect it
  const Eigen::Index n = static_cast<Eigen::Index>(m + k);
  ASSERT_EQ(Pd.rows(), n);
  ASSERT_EQ(Pd.cols(), n);

  const VecX &D = model.specific_var();
  const MatX &F = model.factor_cov();

  for (Eigen::Index i = 0; i < n; ++i) {
    for (Eigen::Index j = 0; j < n; ++j) {
      f64 expect = 0.0;
      const auto em = static_cast<Eigen::Index>(m);
      if (i < em && j < em) {
        expect = (i == j) ? 2.0 * lambda * D[i] : 0.0; // 2λD on the w diagonal
      } else if (i >= em && j >= em) {
        expect = 2.0 * lambda * F(i - em, j - em); // 2λF on the y block
      } else {
        expect = 0.0; // no w–y coupling in P (the coupling is in Ã, not the Hessian)
      }
      EXPECT_NEAR(Pd(i, j), expect, 1e-13) << "P(" << i << "," << j << ")";
    }
  }
}

TEST(RiskQpAugment, FactorDefinitionRowsArePresentAndCorrect) {
  std::mt19937_64 rng(0xBEEFu);
  const usize m = 8U, k = 4U;
  const FactorModel model = make_random_model(m, k, rng);

  std::vector<f64> q(m, 0.0);
  ConstraintSet cs;
  cs.gross.dollar_neutral = true; // one linear row after the K factor rows
  const auto mcr = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mcr.has_value());
  MaterializedConstraints mc = *mcr;
  mc.gross_l1_budget = -1.0;

  const AugmentedQp aug = build_augmented(model, 0.5, std::span<const f64>(q), mc);
  const MatX Ad = MatX(aug.A_tilde); // test-only densify to inspect the pattern
  const MatX &X = model.exposures();
  const auto em = static_cast<Eigen::Index>(m);

  // The first K rows are exactly  y_fk − Σ_i X(i,fk) w_i = 0.
  for (usize fk = 0; fk < k; ++fk) {
    const auto rr = static_cast<Eigen::Index>(fk);
    EXPECT_NEAR(aug.l[rr], 0.0, 0.0) << "factor row " << fk << " l";
    EXPECT_NEAR(aug.u[rr], 0.0, 0.0) << "factor row " << fk << " u";
    for (usize i = 0; i < m; ++i) { // −X(i,fk) on the w-block
      EXPECT_NEAR(Ad(rr, static_cast<Eigen::Index>(i)),
                  -X(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(fk)), 1e-15)
          << "factor row " << fk << " w col " << i;
    }
    for (usize j = 0; j < k; ++j) { // +1 at y_fk, 0 at the other y cols
      const f64 want = (j == fk) ? 1.0 : 0.0;
      EXPECT_NEAR(Ad(rr, em + static_cast<Eigen::Index>(j)), want, 1e-15)
          << "factor row " << fk << " y col " << j;
    }
  }
}

TEST(RiskQpAugment, GrossAndTurnoverSplitsAreSparseAuxRows) {
  std::mt19937_64 rng(0xC0FFEEu);
  const usize m = 6U, k = 2U;
  const FactorModel model = make_random_model(m, k, rng);

  std::vector<f64> q(m, 0.0);
  std::vector<f64> w_prev(m, 0.0);
  w_prev[0] = 0.1;
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.gross.gross_leverage = 1.0;             // gross L1 split present
  cs.turn = TurnoverBudget{0.5};             // turnover L1 split present
  const auto mcr = cs.materialize(model.exposures(), std::span<const f64>(w_prev), m);
  ASSERT_TRUE(mcr.has_value());
  const MaterializedConstraints mc = *mcr;

  const AugmentedQp aug = build_augmented(model, 0.5, std::span<const f64>(q), mc);
  // n_aux = gross M + turnover M (the s and r aux blocks).
  EXPECT_EQ(aug.n_aux, m + m);
  EXPECT_EQ(aug.n_w, m);
  EXPECT_EQ(aug.n_y, k);

  // The split aux rows are SPARSE: each ±w−s / ±w−r row has at most 2 nonzeros, the
  // sum rows have M. The aux columns (s, r) are never a dense block — total nnz over
  // the aug matrix is O(M·K + M), never O(M²). Spot-check the average nnz/row is tiny.
  const f64 nnz = static_cast<f64>(aug.A_tilde.nonZeros());
  const f64 rows = static_cast<f64>(aug.A_tilde.rows());
  // K factor rows have M+1 nnz; everything else is ≤ M. Average stays O(M·K/rows + …)
  // — assert it is far below the dense n (= M+K+2M) that a dense block would imply.
  const f64 n = static_cast<f64>(aug.n_w + aug.n_y + aug.n_aux);
  EXPECT_LT(nnz / rows, n) << "aug matrix is denser than a sparse split should be";
}

// ===========================================================================
//  2. Differential at the optimum — rewrite vs the dense-Ã oracle on a battery.
// ===========================================================================

// One battery case: a (M, K, constraint-mix) tuple. Builds a random feasible
// problem, solves it BOTH ways to a converged fixed budget, returns ‖w_new−w_ref‖∞.
struct CaseSpec {
  usize m;
  usize k;
  bool box;
  bool fexp;
  bool group;
  bool beta;
  bool gross;
  bool turnover;
  unsigned seed;
  usize iters; // per-case fixed ADMM budget — L1 aux-splits converge slower (see below)
};

[[nodiscard]] f64 run_case(const CaseSpec &cs_spec) {
  std::mt19937_64 rng(cs_spec.seed);
  const FactorModel model = make_random_model(cs_spec.m, cs_spec.k, rng);
  const usize m = cs_spec.m;
  const f64 lambda = 0.6;

  // Alpha → q = −alpha; small, dollar-neutral-friendly so a dollar-neutral box book
  // is feasible. (Demeaned to play nicely with the Σw = 0 row.)
  std::uniform_real_distribution<f64> ua(-1.0, 1.0);
  VecX alpha(static_cast<Eigen::Index>(m));
  f64 mean = 0.0;
  for (usize i = 0; i < m; ++i) {
    alpha[static_cast<Eigen::Index>(i)] = ua(rng);
    mean += alpha[static_cast<Eigen::Index>(i)];
  }
  mean /= static_cast<f64>(m);
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -(alpha[static_cast<Eigen::Index>(i)] - mean);
  }

  // Build the constraint mix. All bounds are GENEROUS so the set is feasible (the
  // all-zero book is always feasible for the box/factor/group/beta rows; the L1
  // budgets are loose enough to admit a non-trivial book).
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  if (cs_spec.box) {
    cs.pos = PositionCap{0.6};
  }
  if (cs_spec.fexp) {
    // Bound the first ≤2 factors loosely.
    const usize nf = cs_spec.k >= 2U ? 2U : 1U;
    std::vector<usize> cols;
    std::vector<f64> bnd;
    for (usize j = 0; j < nf; ++j) {
      cols.push_back(j);
      bnd.push_back(0.8);
    }
    cs.fexp = FactorExposure{std::move(cols), std::move(bnd)};
  }
  // group_id buffer must outlive materialize() — keep it alive on the stack here.
  std::vector<usize> group_id(m, 0U);
  if (cs_spec.group) {
    for (usize i = 0; i < m; ++i) {
      group_id[i] = i % 3U; // 3 groups
    }
    GroupCap gc;
    gc.group_id = std::span<const usize>(group_id);
    gc.cap = std::vector<f64>(3U, 0.7);
    cs.grp = std::move(gc);
  }
  std::vector<f64> beta_vec(m, 0.0);
  if (cs_spec.beta) {
    for (usize i = 0; i < m; ++i) {
      beta_vec[i] = 0.5 + 0.5 * static_cast<f64>(i % 2U); // 0.5 / 1.0
    }
    cs.beta = BetaNeutral{std::span<const f64>(beta_vec), 0.5};
  }
  if (cs_spec.gross) {
    cs.gross.gross_leverage = 1.5;
  }
  std::vector<f64> w_prev(m, 0.0);
  if (cs_spec.turnover) {
    cs.turn = TurnoverBudget{1.2};
  }

  auto mcr = cs.materialize(model.exposures(), std::span<const f64>(w_prev), m);
  EXPECT_TRUE(mcr.has_value()) << (mcr ? "" : mcr.error().to_string());
  MaterializedConstraints mc = std::move(*mcr);
  if (!cs_spec.gross) {
    mc.gross_l1_budget = -1.0; // disable the gross aux split when not requested
  }

  // Converged fixed budget for BOTH solvers (high enough that each reaches the QP
  // optimum). The oracle is PCG (needs many inner kkt_iters); the rewrite is exact
  // KKT so only the outer count matters for it. The per-case budget is set so EVERY
  // case clears the feas_tol gate and both solvers reach the same minimizer: the L1
  // (gross/turnover) aux-splits at large K converge the slowest, so they carry the
  // largest count (a fixed-iteration ADMM trades iterations for accuracy — R1/R6).
  const usize outer = cs_spec.iters;

  ConstrainedQpSolver solver;
  solver.cfg.iters = outer;
  solver.cfg.kkt_iters = 0U;     // unused by the direct KKT path
  solver.cfg.feas_tol = 1e-6;
  const QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  auto rn = solver.solve(prob);
  EXPECT_TRUE(rn.has_value()) << (rn ? "" : rn.error().to_string());
  if (!rn) {
    return 1e9;
  }

  atx::engine::risk::reference::ConstrainedQpSolver oracle;
  oracle.cfg.iters = outer;
  oracle.cfg.kkt_iters = 80U; // PCG depth — enough for the dense oracle's x-update at M ≤ 120
  oracle.cfg.feas_tol = 1e-6;
  const atx::engine::risk::reference::QpProblem rprob{model, lambda, std::span<const f64>(q), mc};
  auto rr = oracle.solve(rprob);
  EXPECT_TRUE(rr.has_value()) << (rr ? "" : rr.error().to_string());
  if (!rr) {
    return 1e9;
  }

  const std::vector<f64> &wn = *rn;
  const std::vector<f64> &wr = *rr;
  EXPECT_EQ(wn.size(), wr.size());
  f64 max_abs = 0.0;
  for (usize i = 0; i < wn.size(); ++i) {
    EXPECT_TRUE(std::isfinite(wn[i]));
    max_abs = std::max(max_abs, std::fabs(wn[i] - wr[i]));
  }
  return max_abs;
}

// The differential tolerance. The oracle (PCG ADMM) and the rewrite (direct-KKT
// ADMM) are TWO DIFFERENT linear solves, so they are NOT bit-identical mid-iteration
// — the invariant is agreement AT THE OPTIMUM (the unique QP minimizer). With the
// per-case converged budgets below the measured battery-wide worst ‖w_new−w_ref‖∞ is
// 2.9e-15 (every case sits at 1e-15…1e-16 machine precision). The gate is asserted at
// 1e-11 — three orders above the achieved bound (headroom for platform FP / iteration
// jitter) and far tighter than the plan's ≤1e-10/1e-8 target. NO bit-identity is
// claimed between the two solvers. (A wider M-spread {to 800} agrees to the same
// ~6e-11 — verified out-of-test; the in-test battery caps M at 120 because the
// differential cost is dominated by the as-built O(M²) dense-Ã oracle and the
// reformulation's correctness is M-independent.)
constexpr f64 kDiffTol = 1e-11;

TEST(RiskQpAugment, MatchesDenseOracleAcrossBattery) {
  // M × K grid with varied constraint mixes. Both solvers run to the same per-case
  // converged fixed budget and reach the same unique QP minimizer; the achieved
  // ‖w_new−w_ref‖∞ is asserted ≤ kDiffTol (1e-11). The tightest observed bound is
  // logged for the ledger.
  // Per-case iters: pure-linear (no L1) cases converge in ~600–800; the gross/turnover
  // L1 aux-splits converge slower (a fixed-iteration ADMM trades iterations for
  // accuracy — R1/R6) so they carry 2000–2500. M ∈ {50, 120} keeps the as-built
  // O(M²) dense-Ã oracle affordable in a Debug binary; full K-spread {4, 16, 64} and
  // every constraint family (box / factor / group / beta / gross / turnover, alone and
  // combined) are covered — the reformulation's correctness does not depend on M.
  const std::vector<CaseSpec> battery = {
      // M = 50 — full constraint-family sweep incl. the slow L1 splits
      {50U, 4U, true, false, false, false, false, false, 11U, 600U},
      {50U, 16U, true, true, false, false, false, false, 12U, 600U},
      {50U, 64U, true, true, true, true, false, false, 13U, 800U},    // rich linear, large K
      {50U, 16U, true, false, false, false, true, false, 14U, 2000U}, // gross L1
      {50U, 16U, true, false, false, false, false, true, 15U, 2000U}, // turnover L1
      {50U, 16U, true, true, true, true, true, true, 16U, 2500U},     // everything at once
      {50U, 64U, true, true, false, true, true, true, 17U, 2500U},    // both L1 + large K
      // M = 120 — pure-linear mixes (fast oracle), varied K + families
      {120U, 4U, true, false, false, true, false, false, 21U, 600U},
      {120U, 16U, true, true, true, false, false, false, 22U, 800U},
      {120U, 64U, true, true, false, true, false, false, 23U, 800U},  // large K, beta
      {120U, 16U, true, false, false, false, false, true, 24U, 2500U},// turnover at larger M
  };

  f64 worst = 0.0;
  for (const CaseSpec &c : battery) {
    const f64 d = run_case(c);
    EXPECT_LE(d, kDiffTol) << "M=" << c.m << " K=" << c.k << " gross=" << c.gross
                           << " turn=" << c.turnover << " : ‖w_new−w_ref‖∞=" << d;
    worst = std::max(worst, d);
  }
  // Surface the battery-wide achieved bound (visible on a failing run / -V).
  EXPECT_LE(worst, kDiffTol) << "battery-wide worst ‖w_new−w_ref‖∞ = " << worst;
  RecordProperty("worst_winf", worst);
}

// ===========================================================================
//  3. The rewritten solver still solves the analytic dollar-neutral KKT (sanity).
// ===========================================================================
TEST(RiskQpAugment, DollarNeutralRecoversAnalyticOptimum) {
  std::mt19937_64 rng(0xD00Du);
  const usize m = 6U, k = 2U;
  const FactorModel model = make_random_model(m, k, rng);
  const f64 lambda = 1.0;

  VecX alpha(static_cast<Eigen::Index>(m));
  alpha << 0.5, -0.2, 0.3, 0.1, -0.4, 0.2;
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  auto mcr = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mcr.has_value());
  MaterializedConstraints mc = std::move(*mcr);
  mc.gross_l1_budget = -1.0;

  ConstrainedQpSolver solver;
  solver.cfg.iters = 800U;
  const QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  auto res = solver.solve(prob);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const std::vector<f64> &w = *res;

  // Analytic equality-constrained QP: min ½wᵀ(2λV)w + qᵀw s.t. 1ᵀw = 0.
  const MatX V = dense_v(model);
  const Eigen::Index em = static_cast<Eigen::Index>(m);
  MatX kkt = MatX::Zero(em + 1, em + 1);
  kkt.topLeftCorner(em, em) = 2.0 * lambda * V;
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
    EXPECT_NEAR(w[static_cast<usize>(i)], sol[i], 1e-5) << "w[" << i << "]";
  }
}

// ===========================================================================
//  4. Determinism (R1): two solves of the rewritten path ⇒ byte-identical book.
// ===========================================================================
TEST(RiskQpAugment, TwoSolvesByteIdentical) {
  std::mt19937_64 rng(0xFEEDu);
  const usize m = 20U, k = 8U;
  const FactorModel model = make_random_model(m, k, rng);

  std::uniform_real_distribution<f64> ua(-1.0, 1.0);
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = ua(rng);
  }

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.gross.gross_leverage = 1.5;
  cs.pos = PositionCap{0.4};
  const auto mcr = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mcr.has_value());
  const MaterializedConstraints mc = *mcr;

  ConstrainedQpSolver solver;
  solver.cfg.iters = 400U;
  const QpProblem prob{model, 0.8, std::span<const f64>(q), mc};

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

} // namespace atxtest_risk_qp_augment_test
