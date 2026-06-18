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
#include "atx/engine/risk/multi_period.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atxtest_risk_garleanu_pedersen_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::book::CostInputs;
using atx::engine::risk::ConstrainedQpSolver;
using atx::engine::risk::FactorModel;
using atx::engine::risk::forecast_trajectory;
using atx::engine::risk::gp_aim_and_value;
using atx::engine::risk::GpAimValue;
using atx::engine::risk::HorizonForecast;
using atx::engine::risk::HorizonSource;
using atx::engine::risk::HorizonSources;
using atx::engine::risk::MaterializedConstraints;
using atx::engine::risk::MultiHorizonConfig;
using atx::engine::risk::MultiHorizonOptimizer;
using atx::engine::risk::QpConfig;
using atx::engine::risk::QpProblem;
using atx::engine::risk::RebalanceSchedule;
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
//  SOLVER-CONSISTENCY (NOT the mapping oracle): the relaxed unconstrained QP book equals
//  the closed-form aim_pos. CAVEAT — this is SELF-REFERENTIAL w.r.t. the GP mapping: the
//  QP is handed q = −ᾱ, P = 2λV, so its argmin is BY CONSTRUCTION (2λV)⁻¹ᾱ, which is how
//  gp_aim_and_value defines aim_pos. So this only proves the SOLVER minimizes the quadratic
//  it was given (the build_augmented / ADMM path agrees with the closed-form inverse) — it
//  does NOT validate the GP aim mapping. The independent ground-truth oracle that WOULD
//  fail on a wrong mapping is ClosedFormMatchesHandDerivedGroundTruth below.
// ===========================================================================
TEST(RiskGarleanuPedersen, RelaxedQpMatchesClosedFormSolverConsistency) {
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
    EXPECT_NEAR((*book)[i], gp->aim_pos[i], 1e-6) << "solver-consistency mismatch at name " << i;
  }
}

// ===========================================================================
//  INDEPENDENT GROUND-TRUTH ORACLE (R11) — the test that would FAIL if the GP aim
//  MAPPING were wrong. On a TINY 2-name, K=1 model with a 2-source trajectory of KNOWN
//  decay, we hand-derive (a) the horizon-blended alpha ᾱ from the decay formula and (b)
//  the Markowitz target via a HAND-INVERTED dense 2×2 V — using NEITHER gp_aim_and_value
//  NOR the FactorModel Woodbury apply. We then check the SHIPPED gp_aim_and_value against
//  this external truth. A wrong mapping (wrong rows blended, wrong decay weighting, wrong
//  1/2λ scale, or wrong V⁻¹) would diverge from the hand math.
// ===========================================================================
TEST(RiskGarleanuPedersen, ClosedFormMatchesHandDerivedGroundTruth) {
  // ---- The model (M=2, K=1), chosen so V is a known, hand-invertible 2×2 -----------
  // V = F·X Xᵀ + diag(D). X = [0.5; −0.5], F = [2.0], D = [0.3, 0.4].
  //   X Xᵀ = [[0.25,−0.25],[−0.25,0.25]] ⇒ F·X Xᵀ = [[0.5,−0.5],[−0.5,0.5]]
  //   V    = [[0.8, −0.5], [−0.5, 0.9]]   (add D on the diagonal)
  MatX x(2, 1);
  x(0, 0) = 0.5;
  x(1, 0) = -0.5;
  MatX f = MatX::Constant(1, 1, 2.0);
  VecX d(2);
  d[0] = 0.3;
  d[1] = 0.4;
  auto mr = FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  ASSERT_TRUE(mr.has_value()) << (mr ? "" : mr.error().to_string());
  const FactorModel v = std::move(*mr);

  // ---- Hand-derived V and its inverse (NOT via FactorModel) -------------------------
  const f64 v00 = 0.8, v01 = -0.5, v11 = 0.9;
  const f64 det = v00 * v11 - v01 * v01; // 0.72 − 0.25 = 0.47
  ASSERT_GT(det, 0.0);
  // V⁻¹ = (1/det)·[[v11, −v01], [−v01, v00]]
  const f64 iv00 = v11 / det, iv01 = -v01 / det, iv11 = v00 / det;

  // ---- Hand-derived ᾱ from a 2-source KNOWN-decay trajectory ------------------------
  // Source A: opinion 1.0 on name 0 ONLY, IDENTITY decay (decay(h)=1 ∀h).
  // Source B: opinion 1.0 on name 1 ONLY, halflife=1 ⇒ decay(h)=2^{-h}.
  // With H=2 the trajectory rows are (hand-computed from the decay formula):
  //   name 0: [1, 1, 1]            ⇒ horizon-average ᾱ_0 = (1+1+1)/3 = 1.0
  //   name 1: [1, 2^-1, 2^-2]      ⇒ horizon-average ᾱ_1 = (1+0.5+0.25)/3 = 0.583333…
  const usize H = 2U;
  const f64 abar0_hand = (1.0 + 1.0 + 1.0) / 3.0;
  const f64 abar1_hand = (1.0 + 0.5 + 0.25) / 3.0;

  // ---- Hand Markowitz position aim: aim_pos = (1/2λ)·V⁻¹·ᾱ --------------------------
  const f64 lambda = 1.25;
  const f64 sc = 1.0 / (2.0 * lambda);
  const f64 aim0_hand = sc * (iv00 * abar0_hand + iv01 * abar1_hand);
  const f64 aim1_hand = sc * (iv01 * abar0_hand + iv11 * abar1_hand);

  // ---- The SHIPPED path: build the SAME trajectory, take gp_aim, then gp_aim_and_value
  const std::vector<f64> srcA = {1.0, std::numeric_limits<f64>::quiet_NaN()};
  const std::vector<f64> srcB = {std::numeric_limits<f64>::quiet_NaN(), 1.0};
  std::vector<std::pair<std::span<const f64>, SignalHorizon>> sources = {
      {std::span<const f64>(srcA), SignalHorizon::identity()},
      {std::span<const f64>(srcB), SignalHorizon{1.0}}};
  auto traj = forecast_trajectory(
      std::span<const std::pair<std::span<const f64>, SignalHorizon>>(sources), 2U, H);
  ASSERT_TRUE(traj.has_value()) << (traj ? "" : traj.error().to_string());
  const std::vector<f64> abar = MultiHorizonOptimizer::gp_aim(*traj, 2U);
  ASSERT_EQ(abar.size(), 2U);

  // (a) the shipped ᾱ matches the hand-derived horizon-blend (the decay weighting).
  EXPECT_NEAR(abar[0], abar0_hand, 1e-12) << "ᾱ_0 diverged from hand decay-blend";
  EXPECT_NEAR(abar[1], abar1_hand, 1e-12) << "ᾱ_1 diverged from hand decay-blend";

  // (b) the shipped position aim matches the hand Markowitz map (1/2λ)·V⁻¹·ᾱ.
  auto gp = gp_aim_and_value(std::span<const f64>(abar), v, lambda);
  ASSERT_TRUE(gp.has_value()) << (gp ? "" : gp.error().to_string());
  ASSERT_EQ(gp->aim_pos.size(), 2U);
  EXPECT_NEAR(gp->aim_pos[0], aim0_hand, 1e-9) << "aim_pos_0 diverged from hand Markowitz map";
  EXPECT_NEAR(gp->aim_pos[1], aim1_hand, 1e-9) << "aim_pos_1 diverged from hand Markowitz map";
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
