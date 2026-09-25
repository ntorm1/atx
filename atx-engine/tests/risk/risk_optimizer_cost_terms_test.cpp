// RiskOptimizerCostTerms_* — Lane 6: one cost calibration for replay and optimizer
// (cost/optimizer_cost_terms.hpp). The oracle is the Lane 8 replay model itself: the
// optimizer's priced cost of a trade must equal what the replay charges for it.

#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/random.hpp"
#include "atx/engine/book/replay_cost.hpp"
#include "atx/engine/cost/borrow.hpp"
#include "atx/engine/cost/optimizer_cost_terms.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/cost_terms.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atx_test_l6_optim_cost_calibration {
namespace book = atx::engine::book;
namespace cost = atx::engine::cost;
namespace risk = atx::engine::risk;
namespace la = atx::core::linalg;
using atx::f64;
using atx::usize;

constexpr usize kM = 12;
constexpr f64 kBook = 5e7; // $50m book

std::vector<book::LiquidityRow> liquidity(atx::core::Xoshiro256pp &rng) {
  std::vector<book::LiquidityRow> rows(kM);
  for (usize i = 0; i < kM; ++i) {
    rows[i].adv_dollars = 2e6 + 5e7 * rng.uniform01();
    rows[i].daily_vol = 0.01 + 0.03 * rng.uniform01();
    rows[i].half_spread_bps = 1.0 + 8.0 * rng.uniform01();
  }
  return rows;
}

// Priced (optimizer) cost == charged (replay) cost, trade by trade, as a book fraction.
TEST(RiskOptimizerCostTerms, SqrtImpactPricingEqualsReplayCharge) {
  atx::core::Xoshiro256pp rng{7U};
  const auto rows = liquidity(rng);
  auto model = book::SqrtImpactCost::create(book::ReplayImpactCfg{0.8, 0.5},
                                            std::numeric_limits<f64>::infinity(), 0.5);
  ASSERT_TRUE(model);
  std::vector<f64> prev(kM, 0.0);
  std::vector<f64> w(kM, 0.0);
  for (usize i = 0; i < kM; ++i) {
    prev[i] = 0.01 * rng.normal();
    w[i] = prev[i] + 0.02 * rng.normal();
  }
  auto terms = cost::cost_terms_from_sqrt_impact(*model, rows, kBook, prev);
  ASSERT_TRUE(terms) << terms.error().message();
  const auto priced = risk::evaluate_trade_costs(terms->view(), w);
  ASSERT_TRUE(priced) << priced.error().message();
  f64 lin = 0.0;
  f64 charged = 0.0;
  for (usize i = 0; i < kM; ++i) {
    const f64 dollars = (w[i] - prev[i]) * kBook;
    const auto c = model->cost(i, 0U, dollars, rows[i]);
    EXPECT_EQ(c.filled_dollars, dollars) << i; // uncapped ⇒ full fill
    charged += c.cost_dollars / kBook;
    lin += std::fabs(w[i] - prev[i]) * (rows[i].half_spread_bps + 0.5) * 1e-4;
  }
  EXPECT_NEAR(priced->linear + priced->impact, charged, 1e-12 * charged);
  EXPECT_NEAR(priced->linear, lin, 1e-13 * lin);
  EXPECT_EQ(priced->borrow, 0.0);
}

TEST(RiskOptimizerCostTerms, FlatBpsPricingEqualsReplayCharge) {
  auto model = book::FlatBpsCost::create(7.5);
  ASSERT_TRUE(model);
  const std::vector<f64> w{0.1, -0.05, 0.0, 0.02};
  auto terms = cost::cost_terms_from_flat(*model, 4U);
  ASSERT_TRUE(terms);
  const auto priced = risk::evaluate_trade_costs(terms->view(), w);
  ASSERT_TRUE(priced);
  f64 charged = 0.0;
  for (usize i = 0; i < 4U; ++i) {
    charged += model->cost(i, 0U, w[i] * kBook, {}).cost_dollars / kBook;
  }
  EXPECT_NEAR(priced->total(), charged, 1e-15);
}

// Names the replay refuses to fill are flagged (coefficients 0, max_trade 0); a finite
// participation cap becomes a per-name trade bound in weight units; δ ≠ 1/2 is rejected.
TEST(RiskOptimizerCostTerms, UntradeableCapAndExponentContracts) {
  atx::core::Xoshiro256pp rng{9U};
  auto rows = liquidity(rng);
  rows[2].adv_dollars = 0.0;
  rows[5].daily_vol = std::numeric_limits<f64>::quiet_NaN();
  auto capped = book::SqrtImpactCost::create(book::ReplayImpactCfg{1.0, 0.5}, 0.1);
  ASSERT_TRUE(capped);
  auto terms = cost::cost_terms_from_sqrt_impact(*capped, rows, kBook);
  ASSERT_TRUE(terms) << terms.error().message();
  for (usize i = 0; i < kM; ++i) {
    const bool bad = (i == 2U || i == 5U);
    EXPECT_EQ(terms->untradeable[i], bad ? 1U : 0U) << i;
    if (bad) {
      EXPECT_EQ(terms->kappa_lin[i], 0.0);
      EXPECT_EQ(terms->c_three_halves[i], 0.0);
      EXPECT_EQ(terms->max_trade[i], 0.0);
      EXPECT_EQ(capped->cost(i, 0U, 1e5, rows[i]).filled_dollars, 0.0); // the replay agrees
    } else {
      EXPECT_DOUBLE_EQ(terms->max_trade[i], 0.1 * rows[i].adv_dollars / kBook);
      const f64 max_dollars = terms->max_trade[i] * kBook;
      EXPECT_DOUBLE_EQ(capped->cost(i, 0U, 2.0 * max_dollars, rows[i]).filled_dollars,
                       max_dollars);
    }
  }
  auto linear_law = book::SqrtImpactCost::create(book::ReplayImpactCfg{1.0, 1.0}, 0.1);
  ASSERT_TRUE(linear_law);
  const auto r = cost::cost_terms_from_sqrt_impact(*linear_law, rows, kBook);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_FALSE(cost::cost_terms_from_sqrt_impact(*capped, rows, 0.0));
  const std::vector<f64> short_prev(kM - 1U, 0.0);
  EXPECT_FALSE(cost::cost_terms_from_sqrt_impact(*capped, rows, kBook, short_prev));
}

// The borrow leg prices exactly the S6-5 accrual: short notional · rate · days / denom.
TEST(RiskOptimizerCostTerms, BorrowLegMatchesAccrualFormula) {
  auto model = book::FlatBpsCost::create(0.0);
  ASSERT_TRUE(model);
  const std::vector<f64> prev(4U, 0.0);
  auto terms = cost::cost_terms_from_flat(*model, 4U, prev);
  ASSERT_TRUE(terms);
  const std::vector<f64> rate{0.01, 0.25, 0.0, 0.05};
  const std::vector<f64> locate{1e6, 2e5, std::numeric_limits<f64>::infinity(), 0.0};
  ASSERT_TRUE(cost::add_borrow_terms(*terms, rate, 3.0, cost::DayCount::D365, locate, kBook));
  const std::vector<f64> w{-0.01, -0.002, -0.3, 0.2};
  const auto priced = risk::evaluate_trade_costs(terms->view(), w);
  ASSERT_TRUE(priced) << priced.error().message();
  f64 expect = 0.0;
  for (usize i = 0; i < 4U; ++i) {
    const f64 short_notional = std::max(0.0, -w[i]) * kBook;
    expect += short_notional * rate[i] * 3.0 / 365.0 / kBook;
  }
  EXPECT_NEAR(priced->borrow, expect, 1e-15);
  EXPECT_DOUBLE_EQ(terms->locate_cap[0], 1e6 / kBook);
  EXPECT_TRUE(std::isinf(terms->locate_cap[2]));
  EXPECT_FALSE(cost::add_borrow_terms(*terms, std::vector<f64>{0.1}, 1.0, cost::DayCount::D360));
  const std::vector<f64> neg{0.1, -0.1, 0.0, 0.0};
  EXPECT_FALSE(cost::add_borrow_terms(*terms, neg, 1.0, cost::DayCount::D360));
}

// End to end: the calibrated terms drive solve_with_costs, and pricing the replay's cost
// makes the optimizer trade less than the cost-free solve.
TEST(RiskOptimizerCostTerms, CalibratedTermsDriveTheCostedSolve) {
  atx::core::Xoshiro256pp rng{3U};
  const auto rows = liquidity(rng);
  la::MatX x(static_cast<Eigen::Index>(kM), 1);
  la::VecX d(static_cast<Eigen::Index>(kM));
  std::vector<f64> q(kM, 0.0);
  for (usize i = 0; i < kM; ++i) {
    x(static_cast<Eigen::Index>(i), 0) = 1.0 + 0.2 * rng.normal();
    d[static_cast<Eigen::Index>(i)] = 0.02;
    q[i] = -0.004 * rng.normal();
  }
  auto v = risk::FactorModel::create(std::move(x), la::MatX::Identity(1, 1) * 0.01, std::move(d),
                                     0U, 1U);
  ASSERT_TRUE(v);
  risk::ConstraintSet cs;
  cs.gross.gross_leverage = 1.0;
  cs.gross.dollar_neutral = true;
  auto c = cs.materialize(v->exposures(), {}, kM);
  ASSERT_TRUE(c);
  auto model = book::SqrtImpactCost::create(book::ReplayImpactCfg{1.0, 0.5},
                                            std::numeric_limits<f64>::infinity());
  ASSERT_TRUE(model);
  auto terms = cost::cost_terms_from_sqrt_impact(*model, rows, kBook);
  ASSERT_TRUE(terms);
  risk::ConstrainedQpSolver solver;
  solver.cfg.iters = 5000U;
  risk::AdmmSchedule sched;
  sched.early_exit = true;
  sched.eps_abs = 1e-9;
  sched.eps_rel = 1e-9;
  const risk::QpProblem p{*v, 1.0, q, *c};
  const auto free = solver.solve_with_cert(p, sched);
  const auto costed = risk::solve_with_costs(solver, p, terms->view(), &sched);
  ASSERT_TRUE(free) << free.error().message();
  ASSERT_TRUE(costed) << costed.error().message();
  f64 t_free = 0.0;
  f64 t_cost = 0.0;
  for (usize i = 0; i < kM; ++i) {
    t_free += std::fabs(free->book[i]);
    t_cost += std::fabs(costed->book[i]);
  }
  EXPECT_LT(t_cost, t_free);
}

} // namespace atx_test_l6_optim_cost_calibration
