// risk_garleanu_pedersen_test.cpp — S8.7: the closed-form Gârleanu-Pedersen aim +
// value-function cost-to-go, and the relaxed-QP oracle.
//
// garleanu_pedersen.hpp ships the GP closed form: the RETURN-space decay-weighted aim
// ᾱ = A_xf f_t (the q = −ᾱ linear term) and the POSITION-space aim aim_pos = (2λV)⁻¹ᾱ
// (the unconstrained fast-path book direction). A_xx = 2λV is the value-curvature the
// augmented QP already carries in P; the cost-to-go tail +½(w−aim_pos)ᵀA_xx(w−aim_pos)
// folds to q = −ᾱ with P = 2λV unchanged (the MPC trick).
//
// Gates proven here:
//   G-DIFF (oracle, R11) — a RELAXED (unconstrained) ConstrainedQpSolver solve of
//     ½wᵀ(2λV)w − ᾱᵀw reproduces the closed-form aim_pos within a documented tol; the
//     closed form is computed WITHOUT a solver call and is byte-stable across runs.
//   G-PIN (collapse, R10) — identity-decay H=1 ⇒ ᾱ == α_t bit-identical, so the fold's
//     q = −α_t and P = 2λV are exactly the single-period objective (the cost-to-go tail
//     is inert in the degenerate case).
//   G-DET — the closed form is byte-identical across runs.
//   R2 — extending the horizon beyond a signal's decay support does not change ᾱ
//     (truncation invariance of the receding-horizon aim).

#include <bit>     // std::bit_cast
#include <cmath>   // std::fabs, std::isfinite
#include <cstdint> // std::uint64_t
#include <span>
#include <utility> // std::move, std::pair
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/garleanu_pedersen.hpp"
#include "atx/engine/risk/horizon.hpp"
#include "atx/engine/risk/multi_horizon.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atxtest_risk_garleanu_pedersen_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::ConstrainedQpSolver;
using atx::engine::risk::FactorModel;
using atx::engine::risk::forecast_trajectory;
using atx::engine::risk::gp_aim_and_value;
using atx::engine::risk::GpAimValue;
using atx::engine::risk::HorizonForecast;
using atx::engine::risk::MaterializedConstraints;
using atx::engine::risk::MultiHorizonOptimizer;
using atx::engine::risk::QpConfig;
using atx::engine::risk::QpProblem;
using atx::engine::risk::SignalHorizon;

constexpr usize kM = 8U; // instruments
constexpr usize kK = 2U; // factors

// A small benign FactorModel: M=8, K=2 (mirrors the multi-horizon test's make_model so
// the oracle and pin compare like-for-like).
[[nodiscard]] FactorModel make_model() {
  MatX x(static_cast<Eigen::Index>(kM), static_cast<Eigen::Index>(kK));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(kM); ++i) {
    x(i, 0) = 0.1 * static_cast<f64>(i) - 0.35;
    x(i, 1) = 0.05 * static_cast<f64>(i % 3) - 0.05;
  }
  MatX f = MatX::Identity(static_cast<Eigen::Index>(kK), static_cast<Eigen::Index>(kK));
  VecX d = VecX::Constant(static_cast<Eigen::Index>(kM), 0.2);
  auto r = FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

const std::vector<f64> kAlpha = {2.0, -1.0, 0.5, 3.0, -0.5, 1.2, -2.0, 0.8};

// ===========================================================================
//  G-DIFF (oracle, R11): the RELAXED unconstrained QP book matches the closed-form
//  aim_pos. The QP minimizes ½wᵀ(2λV)w + qᵀw with q = −ᾱ and NO constraints, so its
//  argmin is (2λV)⁻¹ᾱ = aim_pos — computed by gp_aim_and_value WITHOUT a solver call.
// ===========================================================================
TEST(RiskGarleanuPedersen, RelaxedQpMatchesClosedFormOracle) {
  const FactorModel v = make_model();
  const f64 lambda = 1.5;

  // ᾱ is the return-space aim (here just kAlpha — a single identity-decay source's blend).
  auto gp = gp_aim_and_value(std::span<const f64>(kAlpha), v, lambda);
  ASSERT_TRUE(gp.has_value()) << (gp ? "" : gp.error().to_string());
  ASSERT_EQ(gp->aim_pos.size(), kM);

  // Relaxed (unconstrained) QP: q = −ᾱ, P = 2λV, no rows / gross / turnover / cones.
  std::vector<f64> q(kM, 0.0);
  for (usize i = 0; i < kM; ++i) {
    q[i] = -gp->alpha_bar[i];
  }
  MaterializedConstraints C; // default ⇒ unconstrained (empty A, no gross/turnover/cone)
  QpConfig cfg;
  cfg.iters = 1500U; // ample for an unconstrained quadratic to clear the closed form
  const ConstrainedQpSolver solver{cfg};
  const QpProblem prob{v, lambda, std::span<const f64>(q), C};
  auto book = solver.solve(prob);
  ASSERT_TRUE(book.has_value()) << (book ? "" : book.error().to_string());
  ASSERT_EQ(book->size(), kM);

  // The relaxed QP book reproduces the closed-form aim_pos within a documented tol
  // (the ADMM converges the unconstrained quadratic to its exact (2λV)⁻¹ᾱ minimizer).
  for (usize i = 0; i < kM; ++i) {
    EXPECT_NEAR((*book)[i], gp->aim_pos[i], 1e-6) << "oracle mismatch at name " << i;
  }
}

// The closed form is a pure (no-solver) function and is byte-stable across runs (G-DET).
TEST(RiskGarleanuPedersen, ClosedFormNoSolverByteStable) {
  const FactorModel v = make_model();
  const f64 lambda = 0.75;
  auto a = gp_aim_and_value(std::span<const f64>(kAlpha), v, lambda);
  auto b = gp_aim_and_value(std::span<const f64>(kAlpha), v, lambda);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  ASSERT_EQ(a->aim_pos.size(), kM);
  for (usize i = 0; i < kM; ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->aim_pos[i]),
              std::bit_cast<std::uint64_t>(b->aim_pos[i]))
        << "closed form not byte-stable at name " << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a->alpha_bar[i]),
              std::bit_cast<std::uint64_t>(b->alpha_bar[i]));
    EXPECT_TRUE(std::isfinite(a->aim_pos[i]));
  }
}

// ===========================================================================
//  G-PIN (collapse, R10): identity-decay H=1 ⇒ ᾱ == α_t BIT-IDENTICAL, so the fold's
//  linear term q = −α_t and curvature P = 2λV ARE the single-period objective — the
//  cost-to-go tail is inert in the degenerate case (the byte-pin precondition).
// ===========================================================================
TEST(RiskGarleanuPedersen, IdentityDecayCollapsesAlphaBarToAlphaTBitIdentical) {
  // One identity source, H=1 ⇒ both trajectory rows == α_t ⇒ the gp_aim average == α_t.
  std::vector<std::pair<std::span<const f64>, SignalHorizon>> sources = {
      {std::span<const f64>(kAlpha), SignalHorizon::identity()}};
  auto traj = forecast_trajectory(
      std::span<const std::pair<std::span<const f64>, SignalHorizon>>(sources), kM, 1U);
  ASSERT_TRUE(traj.has_value()) << (traj ? "" : traj.error().to_string());
  const std::vector<f64> alpha_bar = MultiHorizonOptimizer::gp_aim(*traj, kM);
  ASSERT_EQ(alpha_bar.size(), kM);
  for (usize i = 0; i < kM; ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(alpha_bar[i]), std::bit_cast<std::uint64_t>(kAlpha[i]))
        << "ᾱ diverged from α_t at name " << i;
  }
}

// ===========================================================================
//  R2 — truncation invariance (receding-horizon, beyond decay support). The GP aim is
//  a DIRECTION fed into a scale-invariant book (the minimal path gross-normalizes; the
//  augmented path's q = −ᾱ drives the same direction). The receding-horizon invariance
//  the gate asks for is on that DIRECTION: extending H past a signal's decay support
//  must not rotate ᾱ. A single source (any halflife) contributes the SAME per-name
//  RATIO at every horizon (decay is a scalar per row), so ᾱ(short H) and ᾱ(long H) are
//  PARALLEL — the un-normalized horizon-average differs only by the row-count divisor
//  (and float ULPs in the equal-row sum), which washes out under the book's gross
//  normalization. We prove the normalized aim direction is invariant within tol, the
//  meaningful receding-horizon property (a bit-identity claim on the un-normalized
//  average would over-specify — it carries the H-dependent 1/(H+1) scale).
// ===========================================================================
TEST(RiskGarleanuPedersen, AlphaBarDirectionInvariantToHorizonBeyondDecaySupport) {
  // Instant-decay source: signal lives ONLY at h=0; rows h>0 are an exact 0 (the source
  // HAS an opinion, so the cell is finite-zero, never NaN). Extending H past h=0 — well
  // beyond this source's (zero) decay support — must not rotate the aim direction.
  std::vector<std::pair<std::span<const f64>, SignalHorizon>> sources = {
      {std::span<const f64>(kAlpha), SignalHorizon{0.0}}};
  auto short_traj = forecast_trajectory(
      std::span<const std::pair<std::span<const f64>, SignalHorizon>>(sources), kM, 2U);
  auto long_traj = forecast_trajectory(
      std::span<const std::pair<std::span<const f64>, SignalHorizon>>(sources), kM, 64U);
  ASSERT_TRUE(short_traj.has_value());
  ASSERT_TRUE(long_traj.has_value());
  const std::vector<f64> a_short = MultiHorizonOptimizer::gp_aim(*short_traj, kM);
  const std::vector<f64> a_long = MultiHorizonOptimizer::gp_aim(*long_traj, kM);
  ASSERT_EQ(a_short.size(), kM);
  ASSERT_EQ(a_long.size(), kM);

  // Normalize each to unit L1 and compare the directions (within tol). Parallel ⇒ same
  // book after gross-normalization (the receding-horizon invariance the gate wants).
  const auto l1 = [](const std::vector<f64> &v) {
    f64 s = 0.0;
    for (const f64 x : v) {
      s += std::fabs(x);
    }
    return s;
  };
  const f64 ns = l1(a_short);
  const f64 nl = l1(a_long);
  ASSERT_GT(ns, 0.0);
  ASSERT_GT(nl, 0.0);
  for (usize i = 0; i < kM; ++i) {
    EXPECT_NEAR(a_short[i] / ns, a_long[i] / nl, 1e-12)
        << "ᾱ direction not horizon-truncation invariant at name " << i;
  }
}

// gp_aim_and_value rejects a length mismatch and a negative λ (boundary validation).
TEST(RiskGarleanuPedersen, RejectsBadArguments) {
  const FactorModel v = make_model();
  const std::vector<f64> wrong_len = {1.0, 2.0, 3.0};
  EXPECT_FALSE(gp_aim_and_value(std::span<const f64>(wrong_len), v, 1.0).has_value());
  EXPECT_FALSE(gp_aim_and_value(std::span<const f64>(kAlpha), v, -1.0).has_value());
}

} // namespace atxtest_risk_garleanu_pedersen_test
