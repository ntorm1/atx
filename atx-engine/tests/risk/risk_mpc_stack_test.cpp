// RiskMpcStack_* — Lane 6 true stacked multi-period MPC QP (H <= 3).
//
// Oracles: the H = 1 closed form, the H-step Gârleanu-Pedersen backward recursion
// (gp_riccati with cfg.horizon = H) on the unconstrained problem, and a dense stacked
// KKT solve (Eigen) with an equality constraint.

#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/risk/admm_schedule.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/gp_riccati.hpp"
#include "atx/engine/risk/mpc_stack.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atx_test_l6_optim_mpc_stack {
namespace risk = atx::engine::risk;
namespace la = atx::core::linalg;
using atx::f64;
using atx::usize;

constexpr usize kM = 4;

risk::FactorModel model4() {
  la::MatX x(kM, 2);
  x << 1.0, 0.2, 0.8, -0.5, 1.1, 0.4, 0.6, -0.9;
  la::MatX f(2, 2);
  f << 0.04, 0.006, 0.006, 0.02;
  la::VecX d(kM);
  d << 0.02, 0.03, 0.025, 0.04;
  auto m = risk::FactorModel::create(x, f, d, 0, 2);
  EXPECT_TRUE(m.has_value());
  return std::move(*m);
}

la::MatX sigma_of(const risk::FactorModel &v) {
  la::MatX s = v.exposures() * v.factor_cov() * v.exposures().transpose();
  s.diagonal() += v.specific_var();
  return s;
}

risk::ConstrainedQpSolver tight_solver() {
  risk::ConstrainedQpSolver s;
  s.cfg.iters = 400U;
  return s;
}

const std::vector<f64> kLambda{0.5, 0.8, 0.3, 1.2};
const std::vector<f64> kPrev{0.1, -0.05, 0.0, 0.2};

// Dense stacked KKT oracle: minimize ½xᵀPx + qᵀx s.t. E x = e (x = [w_1..w_H]).
std::vector<la::VecX> dense_stack(const risk::FactorModel &v, f64 gamma, f64 rho,
                                  const std::vector<std::vector<f64>> &alpha,
                                  const la::MatX &a_eq, const la::VecX &b_eq) {
  const auto m = static_cast<Eigen::Index>(kM);
  const auto hh = static_cast<Eigen::Index>(alpha.size());
  const la::MatX sig = sigma_of(v);
  la::MatX lam = la::MatX::Zero(m, m);
  for (Eigen::Index i = 0; i < m; ++i) {
    lam(i, i) = kLambda[static_cast<usize>(i)];
  }
  const f64 rb = 1.0 - rho;
  const Eigen::Index n = hh * m;
  la::MatX p = la::MatX::Zero(n, n);
  la::VecX q = la::VecX::Zero(n);
  f64 disc = 1.0;
  for (Eigen::Index h = 0; h < hh; ++h) {
    disc *= rb;
    const f64 cd = disc / rb;
    p.block(h * m, h * m, m, m) += disc * gamma * sig + cd * lam;
    if (h > 0) {
      p.block((h - 1) * m, (h - 1) * m, m, m) += cd * lam;
      p.block(h * m, (h - 1) * m, m, m) -= cd * lam;
      p.block((h - 1) * m, h * m, m, m) -= cd * lam;
    }
    for (Eigen::Index i = 0; i < m; ++i) {
      q[h * m + i] = -disc * alpha[static_cast<usize>(h)][static_cast<usize>(i)];
    }
  }
  for (Eigen::Index i = 0; i < m; ++i) { // ½(w_1 − w_0)ᵀΛ(w_1 − w_0) ⇒ −Λw_0 in q
    q[i] -= lam(i, i) * kPrev[static_cast<usize>(i)];
  }
  const Eigen::Index ne = a_eq.rows() * hh;
  la::MatX kkt = la::MatX::Zero(n + ne, n + ne);
  la::VecX rhs = la::VecX::Zero(n + ne);
  kkt.topLeftCorner(n, n) = p;
  rhs.head(n) = -q;
  for (Eigen::Index h = 0; h < hh; ++h) {
    for (Eigen::Index r = 0; r < a_eq.rows(); ++r) {
      const Eigen::Index row = n + h * a_eq.rows() + r;
      kkt.block(row, h * m, 1, m) = a_eq.row(r);
      kkt.block(h * m, row, m, 1) = a_eq.row(r).transpose();
      rhs[row] = b_eq[r];
    }
  }
  const la::VecX sol = kkt.fullPivLu().solve(rhs);
  std::vector<la::VecX> out;
  for (Eigen::Index h = 0; h < hh; ++h) {
    out.emplace_back(sol.segment(h * m, m));
  }
  return out;
}

TEST(RiskMpcStack, HorizonOneMatchesClosedForm) {
  const auto v = model4();
  const risk::MaterializedConstraints c{};
  const std::vector<std::vector<f64>> alpha{{0.02, -0.01, 0.015, 0.005}};
  const f64 gamma = 3.0;
  const f64 rho = 0.02;
  const risk::MpcStackProblem p{v, gamma, kLambda, alpha, kPrev, c, rho};
  const auto r = risk::solve_mpc_stack(tight_solver(), p);
  ASSERT_TRUE(r) << r.error().message();
  const auto oracle = dense_stack(v, gamma, rho, alpha,
                                  la::MatX(0, static_cast<Eigen::Index>(kM)), la::VecX(0));
  ASSERT_EQ(r->path.size(), 1U);
  ASSERT_EQ(r->qp.book.size(), kM);
  for (usize i = 0; i < kM; ++i) {
    EXPECT_NEAR(r->path[0][i], oracle[0][static_cast<Eigen::Index>(i)], 1e-8) << i;
    EXPECT_EQ(r->qp.book[i], r->path[0][i]);
  }
}

TEST(RiskMpcStack, HorizonThreeFirstMoveMatchesGpRiccatiBackwardRecursion) {
  const auto v = model4();
  la::MatX b(kM, 2);
  b << 0.010, -0.004, 0.003, 0.008, -0.006, 0.002, 0.004, 0.005;
  const std::vector<f64> phi{0.1, 0.5};
  const std::vector<f64> f{1.2, -0.7};
  const f64 gamma = 2.5;
  const f64 rho = 0.03;
  for (usize hz = 1; hz <= risk::kMpcMaxHorizon; ++hz) {
    // α_h = B (I − Φ)^{h−1} f — the expected return path the GP recursion prices.
    std::vector<std::vector<f64>> alpha;
    std::vector<f64> fh = f;
    for (usize h = 0; h < hz; ++h) {
      std::vector<f64> a(kM, 0.0);
      for (usize i = 0; i < kM; ++i) {
        for (usize s = 0; s < 2; ++s) {
          a[i] += b(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(s)) * fh[s];
        }
      }
      alpha.push_back(a);
      for (usize s = 0; s < 2; ++s) {
        fh[s] *= 1.0 - phi[s];
      }
    }
    const risk::MaterializedConstraints c{};
    const risk::MpcStackProblem p{v, gamma, kLambda, alpha, kPrev, c, rho};
    const auto r = risk::solve_mpc_stack(tight_solver(), p);
    ASSERT_TRUE(r) << r.error().message();

    risk::GpRiccatiCfg cfg;
    cfg.rho = rho;
    cfg.horizon = hz;
    const auto pol = risk::gp_riccati(v, gamma, {kLambda, 0.0}, phi, b, cfg);
    ASSERT_TRUE(pol) << pol.error().message();
    const auto x1 = pol->step(kPrev, f);
    ASSERT_TRUE(x1) << x1.error().message();
    for (usize i = 0; i < kM; ++i) {
      EXPECT_NEAR(r->path[0][i], (*x1)[i], 1e-8) << "H=" << hz << " i=" << i;
    }
  }
}

TEST(RiskMpcStack, DollarNeutralStackMatchesDenseKkt) {
  const auto v = model4();
  risk::MaterializedConstraints c{};
  c.A = la::MatX::Ones(1, static_cast<Eigen::Index>(kM));
  c.l = la::VecX::Zero(1);
  c.u = la::VecX::Zero(1);
  const std::vector<std::vector<f64>> alpha{
      {0.02, -0.01, 0.015, 0.005}, {0.01, -0.012, 0.004, 0.0}, {0.0, -0.006, 0.001, -0.004}};
  const f64 gamma = 4.0;
  const risk::MpcStackProblem p{v, gamma, kLambda, alpha, kPrev, c, 0.0};
  const risk::AdmmSchedule sched{};
  const auto r = risk::solve_mpc_stack(tight_solver(), p, &sched);
  ASSERT_TRUE(r) << r.error().message();
  const auto oracle = dense_stack(v, gamma, 0.0, alpha, c.A, c.l);
  ASSERT_EQ(r->path.size(), 3U);
  for (usize h = 0; h < 3U; ++h) {
    f64 net = 0.0;
    for (usize i = 0; i < kM; ++i) {
      EXPECT_NEAR(r->path[h][i], oracle[h][static_cast<Eigen::Index>(i)], 1e-7)
          << "h=" << h << " i=" << i;
      net += r->path[h][i];
    }
    EXPECT_NEAR(net, 0.0, 1e-7);
  }
}

TEST(RiskMpcStack, GrossBudgetHoldsInEveryPeriod) {
  const auto v = model4();
  risk::MaterializedConstraints c{};
  c.gross_l1_budget = 0.3;
  const std::vector<std::vector<f64>> alpha{
      {0.5, -0.4, 0.45, 0.3}, {0.4, -0.3, 0.35, 0.2}, {0.3, -0.2, 0.25, 0.1}};
  const risk::MpcStackProblem p{v, 1.0, kLambda, alpha, {}, c, 0.0};
  const auto r = risk::solve_mpc_stack(tight_solver(), p);
  ASSERT_TRUE(r) << r.error().message();
  for (usize h = 0; h < 3U; ++h) {
    f64 g = 0.0;
    for (const f64 w : r->path[h]) {
      g += std::fabs(w);
    }
    EXPECT_LE(g, 0.3 + 1e-6) << h;
    EXPECT_NEAR(g, 0.3, 1e-4) << h; // the alphas are large ⇒ the budget binds
  }
}

TEST(RiskMpcStack, LongerHorizonTradesLessOnFastDecayingSignal) {
  // A signal that vanishes after one period: H = 1 trades toward it, H = 3 knows the
  // position must be unwound (at Λ cost) and trades strictly less.
  const auto v = model4();
  const risk::MaterializedConstraints c{};
  const std::vector<f64> a1{0.03, 0.0, 0.0, 0.0};
  const std::vector<f64> z(kM, 0.0);
  const std::vector<std::vector<f64>> h1{a1};
  const std::vector<std::vector<f64>> h3{a1, z, z};
  const auto r1 =
      risk::solve_mpc_stack(tight_solver(), {v, 2.0, kLambda, h1, {}, c, 0.0});
  const auto r3 =
      risk::solve_mpc_stack(tight_solver(), {v, 2.0, kLambda, h3, {}, c, 0.0});
  ASSERT_TRUE(r1 && r3);
  EXPECT_GT(r1->path[0][0], 0.0);
  EXPECT_LT(r3->path[0][0], r1->path[0][0] - 1e-4);
  EXPECT_GT(r3->path[0][0], 0.0);
}

TEST(RiskMpcStack, TwoSolvesByteIdentical) {
  const auto v = model4();
  const risk::MaterializedConstraints c{};
  const std::vector<std::vector<f64>> alpha{{0.02, -0.01, 0.015, 0.005},
                                            {0.01, -0.005, 0.01, 0.0}};
  const risk::MpcStackProblem p{v, 2.0, kLambda, alpha, kPrev, c, 0.01};
  const risk::AdmmSchedule sched{};
  const auto a = risk::solve_mpc_stack(tight_solver(), p, &sched);
  const auto b = risk::solve_mpc_stack(tight_solver(), p, &sched);
  ASSERT_TRUE(a && b);
  for (usize h = 0; h < 2U; ++h) {
    for (usize i = 0; i < kM; ++i) {
      EXPECT_EQ(std::bit_cast<std::uint64_t>(a->path[h][i]),
                std::bit_cast<std::uint64_t>(b->path[h][i]));
    }
  }
}

TEST(RiskMpcStack, RejectsBadHorizonAndMalformedInputs) {
  const auto v = model4();
  const risk::MaterializedConstraints c{};
  const std::vector<std::vector<f64>> none{};
  const std::vector<std::vector<f64>> four(4, std::vector<f64>(kM, 0.0));
  const std::vector<std::vector<f64>> one{std::vector<f64>(kM, 0.01)};
  const auto s = tight_solver();
  const auto e0 = risk::solve_mpc_stack(s, {v, 1.0, kLambda, none, {}, c, 0.0});
  ASSERT_FALSE(e0);
  EXPECT_EQ(e0.error().code(), atx::core::ErrorCode::OutOfRange);
  const auto e4 = risk::solve_mpc_stack(s, {v, 1.0, kLambda, four, {}, c, 0.0});
  ASSERT_FALSE(e4);
  EXPECT_EQ(e4.error().code(), atx::core::ErrorCode::OutOfRange);

  const std::vector<f64> bad_lambda{0.5, 0.0, 0.3, 1.2};
  const auto el = risk::solve_mpc_stack(s, {v, 1.0, bad_lambda, one, {}, c, 0.0});
  ASSERT_FALSE(el);
  EXPECT_EQ(el.error().code(), atx::core::ErrorCode::InvalidArgument);
  const auto eg = risk::solve_mpc_stack(s, {v, 0.0, kLambda, one, {}, c, 0.0});
  ASSERT_FALSE(eg);
  EXPECT_EQ(eg.error().code(), atx::core::ErrorCode::InvalidArgument);
  const auto er = risk::solve_mpc_stack(s, {v, 1.0, kLambda, one, {}, c, 1.0});
  ASSERT_FALSE(er);
  EXPECT_EQ(er.error().code(), atx::core::ErrorCode::InvalidArgument);

  risk::MaterializedConstraints turn{};
  turn.has_turnover = true;
  turn.turnover_budget = 0.1;
  turn.turnover_ref = std::vector<f64>(kM, 0.0);
  const auto et = risk::solve_mpc_stack(s, {v, 1.0, kLambda, one, {}, turn, 0.0});
  ASSERT_FALSE(et);
  EXPECT_EQ(et.error().code(), atx::core::ErrorCode::InvalidArgument);
}

} // namespace atx_test_l6_optim_mpc_stack
