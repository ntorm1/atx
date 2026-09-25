// RiskMhTrueMpc_* — Lane 6: MultiHorizonOptimizer's true stacked-MPC mode (cfg.true_mpc)
// routes each period through solve_mpc_stack and trades its first move.
//
// Oracles: the direct solve_mpc_stack call on the same inputs (the wiring adds nothing),
// and the unconstrained H-step Gârleanu-Pedersen policy (gp_riccati) through the driver.

#include <cmath>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/risk/admm_schedule.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/gp_riccati.hpp"
#include "atx/engine/risk/horizon.hpp"
#include "atx/engine/risk/mpc_stack.hpp"
#include "atx/engine/risk/multi_horizon.hpp"

namespace atx_test_l6_optim_mh_true_mpc {
namespace risk = atx::engine::risk;
namespace la = atx::core::linalg;
using atx::f64;
using atx::usize;

constexpr usize kM = 4;
const std::vector<f64> kAlpha{0.02, -0.01, 0.015, -0.025};
const std::vector<f64> kLambda{0.5, 0.8, 0.3, 1.2};

risk::FactorModel model4() {
  la::MatX x(static_cast<Eigen::Index>(kM), 2);
  x << 1.0, 0.2, 0.8, -0.5, 1.1, 0.4, 0.6, -0.9;
  la::MatX f(2, 2);
  f << 0.04, 0.006, 0.006, 0.02;
  la::VecX d(static_cast<Eigen::Index>(kM));
  d << 0.02, 0.03, 0.025, 0.04;
  auto m = risk::FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  EXPECT_TRUE(m.has_value());
  return std::move(*m);
}

risk::MultiHorizonConfig mpc_cfg(usize horizon) {
  risk::MultiHorizonConfig cfg;
  cfg.risk_aversion = 2.0;
  cfg.horizon = horizon;
  cfg.true_mpc = true;
  cfg.mpc_impact_diag = kLambda;
  cfg.mpc_discount = 0.01;
  cfg.qp.iters = 20000U;
  cfg.mpc_schedule.early_exit = true;
  cfg.mpc_schedule.eps_abs = 1e-11;
  cfg.mpc_schedule.eps_rel = 1e-11;
  cfg.constraints.gross.gross_leverage = 1e9; // effectively unconstrained
  cfg.constraints.gross.dollar_neutral = false;
  cfg.capacity_bound_gross = false;
  return cfg;
}

atx::core::Result<risk::MultiHorizonResult> run(const risk::MultiHorizonConfig &cfg,
                                                const risk::FactorModel &v, f64 halflife,
                                                atx::usize periods = 1U, f64 kappa = 0.0) {
  const risk::MultiHorizonOptimizer opt{cfg};
  std::vector<usize> sched;
  for (usize s = 0; s < periods; ++s) {
    sched.push_back(s);
  }
  return opt.run(
      risk::RebalanceSchedule{sched},
      [halflife](usize) {
        risk::HorizonSources s;
        s.pairs.emplace_back(std::span<const f64>{kAlpha}, risk::SignalHorizon{halflife});
        return s;
      },
      [&v](usize) -> const risk::FactorModel & { return v; },
      atx::engine::book::CostInputs{kappa, 0.0, 1e9});
}

// The driver's first period equals a direct solve_mpc_stack call on the same inputs.
TEST(RiskMhTrueMpc, FirstPeriodEqualsDirectStackedSolve) {
  const auto v = model4();
  for (const usize hz : {1U, 2U, 3U}) {
    const auto cfg = mpc_cfg(hz);
    const auto r = run(cfg, v, 2.0);
    ASSERT_TRUE(r) << r.error().message();
    auto traj = risk::forecast_trajectory(
        std::vector<std::pair<std::span<const f64>, risk::SignalHorizon>>{
            {std::span<const f64>{kAlpha}, risk::SignalHorizon{2.0}}},
        kM, hz);
    ASSERT_TRUE(traj);
    std::vector<std::vector<f64>> alpha(traj->alpha.begin(),
                                        traj->alpha.begin() + static_cast<std::ptrdiff_t>(hz));
    auto c = cfg.constraints.materialize(v.exposures(), {}, kM);
    ASSERT_TRUE(c);
    const risk::ConstrainedQpSolver solver{cfg.qp};
    const risk::MpcStackProblem p{v, 2.0 * cfg.risk_aversion, kLambda, alpha, {}, *c,
                                  cfg.mpc_discount};
    const auto direct = risk::solve_mpc_stack(solver, p, &cfg.mpc_schedule);
    ASSERT_TRUE(direct) << direct.error().message();
    for (usize i = 0; i < kM; ++i) {
      EXPECT_EQ(r->books[0][i], direct->path[0][i]) << "H=" << hz << " i=" << i;
    }
  }
}

// Unconstrained, the traded first move is the H-step GP policy step from the zero book.
TEST(RiskMhTrueMpc, UnconstrainedFirstMoveIsTheGpPolicy) {
  const auto v = model4();
  const f64 halflife = 2.0;
  const f64 phi = 1.0 - std::exp2(-1.0 / halflife); // decay(h) = (1 − φ)^h
  for (const usize hz : {1U, 2U, 3U}) {
    const auto cfg = mpc_cfg(hz);
    const auto r = run(cfg, v, halflife);
    ASSERT_TRUE(r) << r.error().message();
    // One signal f = 1 with loadings B = α_t and decay φ: α_h = B (1 − φ)^{h−1}.
    la::MatX b(static_cast<Eigen::Index>(kM), 1);
    for (usize i = 0; i < kM; ++i) {
      b(static_cast<Eigen::Index>(i), 0) = kAlpha[i];
    }
    risk::GpRiccatiCfg gcfg;
    gcfg.rho = cfg.mpc_discount;
    gcfg.horizon = hz;
    const std::vector<f64> phis{phi};
    const auto pol = risk::gp_riccati(v, 2.0 * cfg.risk_aversion, {kLambda, 0.0}, phis, b, gcfg);
    ASSERT_TRUE(pol) << pol.error().message();
    const std::vector<f64> zero(kM, 0.0);
    const std::vector<f64> f{1.0};
    const auto x1 = pol->step(zero, f);
    ASSERT_TRUE(x1);
    for (usize i = 0; i < kM; ++i) {
      EXPECT_NEAR(r->books[0][i], (*x1)[i], 1e-8) << "H=" << hz << " i=" << i;
    }
  }
}

// A persistent signal is traded toward more aggressively by a longer-horizon MPC (the
// trade cost is amortized over more periods of expected return): |w_1| grows with H.
TEST(RiskMhTrueMpc, LongerHorizonTradesMoreOnAPersistentSignal) {
  const auto v = model4();
  f64 prev_norm = 0.0;
  for (const usize hz : {1U, 2U, 3U}) {
    const auto r = run(mpc_cfg(hz), v, 50.0);
    ASSERT_TRUE(r) << r.error().message();
    f64 n = 0.0;
    for (const f64 w : r->books[0]) {
      n += w * w;
    }
    EXPECT_GT(n, prev_norm) << "H=" << hz;
    prev_norm = n;
  }
}

void expect_err(const atx::core::Result<risk::MultiHorizonResult> &r, atx::core::ErrorCode code,
                const char *what) {
  ASSERT_FALSE(r) << what;
  EXPECT_EQ(r.error().code(), code) << what;
}

// The mode's requirements fail typed instead of silently dropping a cost or a knob.
TEST(RiskMhTrueMpc, RequirementsFailTyped) {
  const auto v = model4();
  using atx::core::ErrorCode;
  {
    auto cfg = mpc_cfg(2U);
    cfg.stacked_mpc = true;
    expect_err(run(cfg, v, 2.0), ErrorCode::InvalidArgument, "stacked_mpc");
  }
  {
    auto cfg = mpc_cfg(2U);
    cfg.trade_rate = 0.5;
    expect_err(run(cfg, v, 2.0), ErrorCode::InvalidArgument, "trade_rate");
  }
  // κ > 0 would otherwise be silently dropped (the stacked QP prices trades through Λ).
  expect_err(run(mpc_cfg(2U), v, 2.0, 1U, 0.001), ErrorCode::InvalidArgument, "kappa");
  expect_err(run(mpc_cfg(4U), v, 2.0), ErrorCode::OutOfRange, "horizon 4");
  expect_err(run(mpc_cfg(0U), v, 2.0), ErrorCode::OutOfRange, "horizon 0");
  {
    auto cfg = mpc_cfg(2U);
    cfg.mpc_impact_diag.clear();
    expect_err(run(cfg, v, 2.0), ErrorCode::InvalidArgument, "impact");
  }
  {
    auto cfg = mpc_cfg(2U);
    cfg.constraints.turn = risk::TurnoverBudget{0.1}; // not expressible in the stacked QP
    expect_err(run(cfg, v, 2.0), ErrorCode::InvalidArgument, "turnover");
  }
}

// Multi-period chain: w_prev threads from the realized first move; the constrained path
// (dollar-neutral, gross) is honoured every period.
TEST(RiskMhTrueMpc, ConstrainedChainThreadsThePreviousBook) {
  const auto v = model4();
  auto cfg = mpc_cfg(2U);
  cfg.constraints.gross.gross_leverage = 0.5;
  cfg.constraints.gross.dollar_neutral = true;
  const auto r = run(cfg, v, 3.0, 3U);
  ASSERT_TRUE(r) << r.error().message();
  ASSERT_EQ(r->books.size(), 3U);
  for (const auto &book : r->books) {
    f64 net = 0.0;
    f64 gross = 0.0;
    for (const f64 w : book) {
      net += w;
      gross += std::fabs(w);
    }
    EXPECT_NEAR(net, 0.0, 1e-7);
    EXPECT_LE(gross, 0.5 + 1e-7);
  }
  // Period 2 starts from period 1's book, so it moves less than period 1 did from zero.
  EXPECT_LT(r->turnover[1], r->turnover[0]);
}

} // namespace atx_test_l6_optim_mh_true_mpc
