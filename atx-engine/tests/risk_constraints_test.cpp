// risk_constraints_test.cpp — S1-1: the constraint algebra -> (A, l, u).
//
// ConstraintSet::materialize composes a fixed set of portfolio constraint
// descriptors into the QP linear-inequality form l <= A w <= u. `A` is an R×M
// linear operator carrying ONLY the genuinely-linear rows; the two L1-budget
// constraints (gross Σ|w|<=L, turnover Σ|w−w_prev|<=T) are NOT linear in w and
// ride along as metadata for the S1-2 ADMM auxiliary-variable split.
//
// Row emission is ORDER-FIXED (R1) for determinism:
//   (1) dollar-neutral Σw=0, (2) position box, (3) factor exposure,
//   (4) group, (5) beta.
//
// Coverage (the S1-1 contract):
//   1. Happy — a full ConstraintSet materializes with the exact row count/order.
//   2. EachDescriptorEncodesItsInequality — on-boundary w accepted, over-boundary
//      w violates the claimed row (componentwise A*w vs [l,u]).
//   3. L1BudgetMetadata — gross_l1_budget == L; turnover metadata round-trips.
//   4. OrderFixedDeterminism — two materializations are byte-identical.
//   5. Boundaries/errors — every dim mismatch / negative bound -> InvalidArgument.
//   6. Minimal set — only GrossNet dollar-neutral -> A is 1×M.

#include <optional> // std::optional descriptors
#include <span>     // std::span (group_id / beta / w_prev)
#include <string>   // error message inspection
#include <vector>   // descriptor payloads

#include <gtest/gtest.h>

#include <Eigen/Dense> // Eigen::Index in assertions

#include "atx/core/error.hpp"          // ErrorCode, Result
#include "atx/core/linalg/linalg.hpp"  // MatX, VecX
#include "atx/core/types.hpp"          // f64, usize

#include "atx/engine/risk/constraints.hpp"

namespace {

using atx::f64;
using atx::usize;
using atx::core::ErrorCode;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::BetaNeutral;
using atx::engine::risk::ConstraintSet;
using atx::engine::risk::FactorExposure;
using atx::engine::risk::GroupCap;
using atx::engine::risk::GrossNet;
using atx::engine::risk::MaterializedConstraints;
using atx::engine::risk::PositionCap;
using atx::engine::risk::TurnoverBudget;

// A small M×K exposure matrix: M=4 instruments, K=2 factors. Distinct entries so
// a factor row is recognizable (no accidental symmetry hiding an index bug).
constexpr usize kM = 4U;
constexpr usize kK = 2U;

[[nodiscard]] MatX make_exposures() {
  MatX x(static_cast<Eigen::Index>(kM), static_cast<Eigen::Index>(kK));
  // column 0 (factor 0)         column 1 (factor 1)
  x(0, 0) = 1.0;  x(0, 1) = -2.0;
  x(1, 0) = 2.0;  x(1, 1) = 1.0;
  x(2, 0) = -1.0; x(2, 1) = 3.0;
  x(3, 0) = 0.5;  x(3, 1) = -1.0;
  return x;
}

// A weight vector as a VecX for the A*w boundary checks.
[[nodiscard]] VecX vec(std::initializer_list<f64> xs) {
  VecX v(static_cast<Eigen::Index>(xs.size()));
  Eigen::Index i = 0;
  for (const f64 x : xs) {
    v[i++] = x;
  }
  return v;
}

// True iff every component of A*w lies within [l, u] (with a tiny tolerance).
[[nodiscard]] bool feasible(const MaterializedConstraints& mc, const VecX& w, f64 tol = 1e-12) {
  const VecX aw = mc.A * w;
  for (Eigen::Index r = 0; r < aw.size(); ++r) {
    if (aw[r] < mc.l[r] - tol || aw[r] > mc.u[r] + tol) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// 1. Happy path — full set materializes with the exact row count/order.
// ---------------------------------------------------------------------------
TEST(RiskConstraints, FullSet_Materializes_ExactRowCountAndOrder) {
  const MatX x = make_exposures();
  const std::vector<usize> gid = {0U, 0U, 1U, 1U}; // two groups
  const std::vector<f64> betas = {1.0, 1.0, 1.0, 1.0};

  ConstraintSet cs;
  cs.gross = GrossNet{/*gross_leverage=*/2.0, /*dollar_neutral=*/true};
  cs.pos = PositionCap{/*name_cap=*/0.5};
  cs.fexp = FactorExposure{/*factor_cols=*/{0U, 1U}, /*bound=*/{0.3, 0.4}};
  cs.grp = GroupCap{/*group_id=*/std::span<const usize>(gid), /*cap=*/{0.6, 0.7}};
  cs.beta = BetaNeutral{/*beta=*/std::span<const f64>(betas), /*tol=*/0.1};

  const auto res = cs.materialize(x, /*w_prev=*/{}, kM);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const MaterializedConstraints& mc = *res;

  // (1) dollar-neutral + (M box) + (2 factor) + (2 group) + (1 beta).
  const Eigen::Index expectedR = 1 + static_cast<Eigen::Index>(kM) + 2 + 2 + 1;
  EXPECT_EQ(mc.A.rows(), expectedR);
  EXPECT_EQ(mc.A.cols(), static_cast<Eigen::Index>(kM));
  EXPECT_EQ(mc.l.size(), expectedR);
  EXPECT_EQ(mc.u.size(), expectedR);

  // Row 0 is the dollar-neutral all-ones, l=u=0.
  for (Eigen::Index j = 0; j < static_cast<Eigen::Index>(kM); ++j) {
    EXPECT_DOUBLE_EQ(mc.A(0, j), 1.0);
  }
  EXPECT_DOUBLE_EQ(mc.l[0], 0.0);
  EXPECT_DOUBLE_EQ(mc.u[0], 0.0);

  // Box rows 1..M: row (1+i) is e_i with [-cap, cap].
  for (usize i = 0; i < kM; ++i) {
    const Eigen::Index r = 1 + static_cast<Eigen::Index>(i);
    for (usize j = 0; j < kM; ++j) {
      EXPECT_DOUBLE_EQ(mc.A(r, static_cast<Eigen::Index>(j)), i == j ? 1.0 : 0.0);
    }
    EXPECT_DOUBLE_EQ(mc.l[r], -0.5);
    EXPECT_DOUBLE_EQ(mc.u[r], 0.5);
  }
}

// ---------------------------------------------------------------------------
// 2. Each descriptor encodes its claimed inequality (on-boundary vs over).
// ---------------------------------------------------------------------------
TEST(RiskConstraints, FactorExposure_OnBoundaryFeasible_OverInfeasible) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.gross = GrossNet{2.0, false};                       // no Σw=0 row to entangle
  cs.fexp = FactorExposure{{0U}, {1.0}};                 // |（Xᵀw)_0| <= 1
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const MaterializedConstraints& mc = *res;
  ASSERT_EQ(mc.A.rows(), 1);

  // (Xᵀw)_0 = 1*w0 + 2*w1 + (-1)*w2 + 0.5*w3. Pick w making it exactly +1.
  const VecX on = vec({1.0, 0.0, 0.0, 0.0}); // -> 1.0 on the boundary
  EXPECT_NEAR((mc.A * on)[0], 1.0, 1e-12);
  EXPECT_TRUE(feasible(mc, on));

  const VecX over = vec({2.0, 0.0, 0.0, 0.0}); // -> 2.0, exits [-1, 1]
  EXPECT_FALSE(feasible(mc, over));
}

TEST(RiskConstraints, GroupCap_OnBoundaryFeasible_OverInfeasible) {
  const MatX x = make_exposures();
  const std::vector<usize> gid = {0U, 0U, 1U, 1U};
  ConstraintSet cs;
  cs.gross = GrossNet{2.0, false};
  cs.grp = GroupCap{std::span<const usize>(gid), {1.0, 5.0}}; // |Σ_{g0}|<=1, |Σ_{g1}|<=5
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const MaterializedConstraints& mc = *res;
  ASSERT_EQ(mc.A.rows(), 2);

  // Group 0 = w0 + w1; on the +1 boundary.
  const VecX on = vec({0.4, 0.6, 0.0, 0.0});
  EXPECT_NEAR((mc.A * on)[0], 1.0, 1e-12);
  EXPECT_TRUE(feasible(mc, on));

  const VecX over = vec({0.9, 0.9, 0.0, 0.0}); // group 0 sum = 1.8 > 1
  EXPECT_FALSE(feasible(mc, over));
}

TEST(RiskConstraints, BetaNeutral_OnBoundaryFeasible_OverInfeasible) {
  const MatX x = make_exposures();
  const std::vector<f64> betas = {1.0, 2.0, -1.0, 0.0};
  ConstraintSet cs;
  cs.gross = GrossNet{2.0, false};
  cs.beta = BetaNeutral{std::span<const f64>(betas), /*tol=*/0.5}; // |βᵀw| <= 0.5
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const MaterializedConstraints& mc = *res;
  ASSERT_EQ(mc.A.rows(), 1);

  // βᵀw = w0 + 2 w1 - w2. Pin to exactly +0.5.
  const VecX on = vec({0.5, 0.0, 0.0, 0.0});
  EXPECT_NEAR((mc.A * on)[0], 0.5, 1e-12);
  EXPECT_TRUE(feasible(mc, on));

  const VecX over = vec({0.0, 1.0, 0.0, 0.0}); // βᵀw = 2.0 > 0.5
  EXPECT_FALSE(feasible(mc, over));
}

TEST(RiskConstraints, PositionBox_OnBoundaryFeasible_OverInfeasible) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.gross = GrossNet{2.0, false};
  cs.pos = PositionCap{/*name_cap=*/0.5};
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const MaterializedConstraints& mc = *res;
  ASSERT_EQ(mc.A.rows(), static_cast<Eigen::Index>(kM));

  const VecX on = vec({0.5, -0.5, 0.0, 0.0}); // both at the cap magnitude
  EXPECT_TRUE(feasible(mc, on));

  const VecX over = vec({0.6, 0.0, 0.0, 0.0}); // |w0| = 0.6 > 0.5
  EXPECT_FALSE(feasible(mc, over));
}

TEST(RiskConstraints, DollarNeutral_ExactSumZero) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.gross = GrossNet{2.0, true}; // Σw = 0 only
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const MaterializedConstraints& mc = *res;
  ASSERT_EQ(mc.A.rows(), 1);

  const VecX on = vec({1.0, -1.0, 0.5, -0.5}); // sums to 0 exactly
  EXPECT_NEAR((mc.A * on)[0], 0.0, 1e-12);
  EXPECT_TRUE(feasible(mc, on));

  const VecX over = vec({1.0, 0.0, 0.0, 0.0}); // sums to 1, exits [0, 0]
  EXPECT_FALSE(feasible(mc, over));
}

// ---------------------------------------------------------------------------
// 3. L1-budget metadata round-trips.
// ---------------------------------------------------------------------------
TEST(RiskConstraints, L1Budget_GrossAndTurnoverMetadata) {
  const MatX x = make_exposures();
  const std::vector<f64> wprev = {0.1, -0.2, 0.3, -0.4};
  ConstraintSet cs;
  cs.gross = GrossNet{/*gross_leverage=*/1.5, /*dollar_neutral=*/true};
  cs.turn = TurnoverBudget{/*max_turnover=*/0.75};

  const auto res = cs.materialize(x, std::span<const f64>(wprev), kM);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const MaterializedConstraints& mc = *res;

  EXPECT_DOUBLE_EQ(mc.gross_l1_budget, 1.5);
  EXPECT_TRUE(mc.has_turnover);
  EXPECT_DOUBLE_EQ(mc.turnover_budget, 0.75);
  ASSERT_EQ(mc.turnover_ref.size(), kM);
  for (usize i = 0; i < kM; ++i) {
    EXPECT_DOUBLE_EQ(mc.turnover_ref[i], wprev[i]);
  }
}

TEST(RiskConstraints, L1Budget_NoTurnover_FlagFalse) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.gross = GrossNet{1.0, true}; // no turnover descriptor
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const MaterializedConstraints& mc = *res;

  EXPECT_DOUBLE_EQ(mc.gross_l1_budget, 1.0);
  EXPECT_FALSE(mc.has_turnover);
  EXPECT_TRUE(mc.turnover_ref.empty());
}

TEST(RiskConstraints, L1Budget_Turnover_EmptyWPrev_ZeroRef) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.gross = GrossNet{1.0, true};
  cs.turn = TurnoverBudget{0.5};
  const auto res = cs.materialize(x, /*w_prev=*/{}, kM); // empty ref -> flat/zero
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const MaterializedConstraints& mc = *res;

  EXPECT_TRUE(mc.has_turnover);
  EXPECT_DOUBLE_EQ(mc.turnover_budget, 0.5);
  ASSERT_EQ(mc.turnover_ref.size(), kM);
  for (usize i = 0; i < kM; ++i) {
    EXPECT_DOUBLE_EQ(mc.turnover_ref[i], 0.0);
  }
}

// ---------------------------------------------------------------------------
// 4. Order-fixed / determinism (R1): two materializations are byte-identical.
// ---------------------------------------------------------------------------
TEST(RiskConstraints, Materialize_TwiceByteIdentical) {
  const MatX x = make_exposures();
  const std::vector<usize> gid = {0U, 1U, 0U, 1U};
  const std::vector<f64> betas = {0.5, -0.5, 1.0, -1.0};
  const std::vector<f64> wprev = {0.2, 0.2, -0.2, -0.2};

  ConstraintSet cs;
  cs.gross = GrossNet{2.0, true};
  cs.pos = PositionCap{0.5};
  cs.fexp = FactorExposure{{1U, 0U}, {0.3, 0.6}};
  cs.grp = GroupCap{std::span<const usize>(gid), {0.4, 0.4}};
  cs.beta = BetaNeutral{std::span<const f64>(betas), 0.2};
  cs.turn = TurnoverBudget{0.9};

  const auto r1 = cs.materialize(x, std::span<const f64>(wprev), kM);
  const auto r2 = cs.materialize(x, std::span<const f64>(wprev), kM);
  ASSERT_TRUE(r1.has_value()) << (r1 ? "" : r1.error().to_string());
  ASSERT_TRUE(r2.has_value()) << (r2 ? "" : r2.error().to_string());

  const MaterializedConstraints& a = *r1;
  const MaterializedConstraints& b = *r2;
  ASSERT_EQ(a.A.rows(), b.A.rows());
  ASSERT_EQ(a.A.cols(), b.A.cols());
  for (Eigen::Index r = 0; r < a.A.rows(); ++r) {
    for (Eigen::Index c = 0; c < a.A.cols(); ++c) {
      EXPECT_EQ(a.A(r, c), b.A(r, c)); // exact equality (no tolerance)
    }
    EXPECT_EQ(a.l[r], b.l[r]);
    EXPECT_EQ(a.u[r], b.u[r]);
  }
  EXPECT_EQ(a.gross_l1_budget, b.gross_l1_budget);
  EXPECT_EQ(a.has_turnover, b.has_turnover);
  EXPECT_EQ(a.turnover_budget, b.turnover_budget);
  ASSERT_EQ(a.turnover_ref.size(), b.turnover_ref.size());
  for (usize i = 0; i < a.turnover_ref.size(); ++i) {
    EXPECT_EQ(a.turnover_ref[i], b.turnover_ref[i]);
  }
}

// ---------------------------------------------------------------------------
// 5. Boundaries / errors.
// ---------------------------------------------------------------------------
TEST(RiskConstraints, FactorExposure_LengthMismatch_InvalidArgument) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.fexp = FactorExposure{/*factor_cols=*/{0U, 1U}, /*bound=*/{0.3}}; // 2 vs 1
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskConstraints, FactorExposure_ColIndexOutOfRange_OutOfRange) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.fexp = FactorExposure{/*factor_cols=*/{kK}, /*bound=*/{0.3}}; // col == K, out of range
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_FALSE(res.has_value());
  // A bad factor-column INDEX is OutOfRange (per the as-built error-code policy:
  // OutOfRange for a bad index; InvalidArgument for dim mismatch / bad bound).
  EXPECT_EQ(res.error().code(), ErrorCode::OutOfRange);
}

TEST(RiskConstraints, FactorExposure_NegativeBound_InvalidArgument) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.fexp = FactorExposure{{0U}, {-0.1}}; // bound < 0
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskConstraints, GroupId_LengthMismatch_InvalidArgument) {
  const MatX x = make_exposures();
  const std::vector<usize> gid = {0U, 0U, 1U}; // length 3 != M=4
  ConstraintSet cs;
  cs.grp = GroupCap{std::span<const usize>(gid), {0.5, 0.5}};
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskConstraints, GroupCap_NegativeCap_InvalidArgument) {
  const MatX x = make_exposures();
  const std::vector<usize> gid = {0U, 0U, 1U, 1U};
  ConstraintSet cs;
  cs.grp = GroupCap{std::span<const usize>(gid), {0.5, -0.5}}; // cap[1] < 0
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskConstraints, GroupCap_WrongGroupCount_InvalidArgument) {
  const MatX x = make_exposures();
  const std::vector<usize> gid = {0U, 0U, 1U, 1U}; // G = 2
  ConstraintSet cs;
  cs.grp = GroupCap{std::span<const usize>(gid), {0.5}}; // cap.size()=1 != G=2
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskConstraints, BetaNeutral_LengthMismatch_InvalidArgument) {
  const MatX x = make_exposures();
  const std::vector<f64> betas = {1.0, 1.0, 1.0}; // length 3 != M=4
  ConstraintSet cs;
  cs.beta = BetaNeutral{std::span<const f64>(betas), 0.1};
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskConstraints, BetaNeutral_NegativeTol_InvalidArgument) {
  const MatX x = make_exposures();
  const std::vector<f64> betas = {1.0, 1.0, 1.0, 1.0};
  ConstraintSet cs;
  cs.beta = BetaNeutral{std::span<const f64>(betas), -0.1}; // tol < 0
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskConstraints, PositionCap_NegativeCap_InvalidArgument) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.pos = PositionCap{-0.5}; // name_cap < 0
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskConstraints, GrossNet_NegativeLeverage_InvalidArgument) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.gross = GrossNet{-1.0, true}; // gross_leverage < 0
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskConstraints, TurnoverBudget_Negative_InvalidArgument) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.gross = GrossNet{1.0, true};
  cs.turn = TurnoverBudget{-0.5}; // max_turnover < 0
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskConstraints, ExposureRowMismatch_InvalidArgument) {
  const MatX x = make_exposures();        // X has kM rows
  ConstraintSet cs;
  cs.fexp = FactorExposure{{0U}, {0.3}};
  const auto res = cs.materialize(x, {}, kM + 1U); // claim M = kM+1 != X.rows()
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(RiskConstraints, EmptyOptionals_OmitTheirRows) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.gross = GrossNet{1.0, true}; // ONLY the Σw=0 row; all optionals empty
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  EXPECT_EQ(res->A.rows(), 1);
}

// ---------------------------------------------------------------------------
// 6. Minimal set — only GrossNet dollar-neutral -> A is 1×M.
// ---------------------------------------------------------------------------
TEST(RiskConstraints, MinimalSet_OnlyGross_OneRow) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.gross = GrossNet{/*gross_leverage=*/1.0, /*dollar_neutral=*/true};
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const MaterializedConstraints& mc = *res;

  EXPECT_EQ(mc.A.rows(), 1);
  EXPECT_EQ(mc.A.cols(), static_cast<Eigen::Index>(kM));
  EXPECT_DOUBLE_EQ(mc.gross_l1_budget, 1.0);
  EXPECT_FALSE(mc.has_turnover);
}

// A dollar_neutral=false GrossNet with no other descriptors emits ZERO linear
// rows (A is 0×M) — the gross-L1 cap is metadata-only, not a linear row.
TEST(RiskConstraints, GrossNetOnly_NotDollarNeutral_ZeroRows) {
  const MatX x = make_exposures();
  ConstraintSet cs;
  cs.gross = GrossNet{/*gross_leverage=*/1.0, /*dollar_neutral=*/false};
  const auto res = cs.materialize(x, {}, kM);
  ASSERT_TRUE(res.has_value()) << (res ? "" : res.error().to_string());
  const MaterializedConstraints& mc = *res;

  EXPECT_EQ(mc.A.rows(), 0);
  EXPECT_EQ(mc.A.cols(), static_cast<Eigen::Index>(kM));
  EXPECT_DOUBLE_EQ(mc.gross_l1_budget, 1.0);
}

} // namespace
