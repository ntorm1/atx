// RiskDiscretize_* — Lane 6 post-solve pass: max-names, min-trade, lot rounding, re-solve.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/discretize.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atx_test_l6_optim_discretize {
namespace risk = atx::engine::risk;
namespace la = atx::core::linalg;
using atx::f64;
using atx::usize;

constexpr usize kM = 10;

risk::FactorModel diag_model() {
  const auto n = static_cast<Eigen::Index>(kM);
  auto model = risk::FactorModel::create(la::MatX::Zero(n, 1), la::MatX::Identity(1, 1),
                                         la::VecX::Constant(n, 0.04), 0, 1);
  EXPECT_TRUE(model.has_value());
  return std::move(*model);
}

// Graded alphas: 5 longs, 5 shorts of decreasing strength.
std::vector<f64> graded_q() {
  return {-0.050, -0.040, -0.030, -0.020, -0.010, 0.012, 0.022, 0.032, 0.042, 0.052};
}

risk::MaterializedConstraints neutral_constraints(const risk::FactorModel &model,
                                                  std::span<const f64> prev) {
  risk::ConstraintSet spec;
  spec.gross = {1.0, true};
  spec.pos = risk::PositionCap{0.3};
  auto c = spec.materialize(model.exposures(), prev, kM);
  EXPECT_TRUE(c.has_value());
  return std::move(*c);
}

TEST(RiskDiscretize, AllRulesOffReturnsContinuousBookUnchanged) {
  const auto model = diag_model();
  const std::vector<f64> prev(kM, 0.0);
  const auto c = neutral_constraints(model, prev);
  const auto q = graded_q();
  const risk::ConstrainedQpSolver solver;
  const auto w = solver.solve({model, 1.0, q, c});
  ASSERT_TRUE(w);
  const auto d = risk::discretize_and_resolve(solver, {model, 1.0, q, c}, prev, *w, {});
  ASSERT_TRUE(d) << d.error().message();
  EXPECT_FALSE(d->resolved);
  EXPECT_EQ(0, std::memcmp(d->book.data(), w->data(), kM * sizeof(f64)));
}

TEST(RiskDiscretize, MaxNamesKeepsLargestAndResolvesFeasibly) {
  const auto model = diag_model();
  const std::vector<f64> prev(kM, 0.0);
  const auto c = neutral_constraints(model, prev);
  const auto q = graded_q();
  const risk::ConstrainedQpSolver solver;
  const auto w = solver.solve({model, 1.0, q, c});
  ASSERT_TRUE(w);
  risk::DiscretizeCfg cfg;
  cfg.max_names = 4;
  const auto d = risk::discretize_and_resolve(solver, {model, 1.0, q, c}, prev, *w, cfg);
  ASSERT_TRUE(d) << d.error().message();
  EXPECT_TRUE(d->resolved);
  EXPECT_LE(d->n_names, 4U);
  // The strongest two longs and two shorts survive; the rest are flat.
  for (const usize i : {0U, 1U, 8U, 9U}) {
    EXPECT_GT(std::fabs(d->book[i]), 1e-6) << i;
  }
  for (const usize i : {2U, 3U, 4U, 5U, 6U, 7U}) {
    EXPECT_EQ(d->book[i], 0.0) << i;
  }
  EXPECT_LE(d->max_violation, 1e-6); // dollar-neutral + gross still hold after re-solve
}

TEST(RiskDiscretize, MinTradePinsSmallTradesAtPreviousBook) {
  const auto model = diag_model();
  const auto q = graded_q();
  std::vector<f64> prev(kM, 0.0);
  const risk::ConstrainedQpSolver solver;
  {
    const auto c0 = neutral_constraints(model, prev);
    const auto w0 = solver.solve({model, 1.0, q, c0});
    ASSERT_TRUE(w0);
    prev = *w0;
    prev[0] -= 0.004; // a stale book: names 0/9 drifted inward (gross stays < 1)
    prev[9] += 0.004;
  }
  const auto c = neutral_constraints(model, prev);
  const auto w = solver.solve({model, 1.0, q, c});
  ASSERT_TRUE(w);
  risk::DiscretizeCfg cfg;
  cfg.min_trade = 0.01;
  const auto d = risk::discretize_and_resolve(solver, {model, 1.0, q, c}, prev, *w, cfg);
  ASSERT_TRUE(d) << d.error().message();
  for (usize i = 0; i < kM; ++i) {
    const f64 t = std::fabs(d->book[i] - prev[i]);
    EXPECT_TRUE(t == 0.0 || t >= cfg.min_trade) << i << " trade " << t;
  }
  EXPECT_EQ(d->n_trades, 0U); // every drift here is below the threshold
}

TEST(RiskDiscretize, LotRoundingProducesWholeLotTrades) {
  const auto model = diag_model();
  const std::vector<f64> prev(kM, 0.0);
  const auto c = neutral_constraints(model, prev);
  const auto q = graded_q();
  const risk::ConstrainedQpSolver solver;
  const auto w = solver.solve({model, 1.0, q, c});
  ASSERT_TRUE(w);
  const std::vector<f64> lot(kM, 0.005);
  risk::DiscretizeCfg cfg;
  cfg.lot = lot;
  const auto d = risk::discretize_and_resolve(solver, {model, 1.0, q, c}, prev, *w, cfg);
  ASSERT_TRUE(d) << d.error().message();
  for (usize i = 0; i < kM; ++i) {
    const f64 lots = (d->book[i] - prev[i]) / lot[i];
    EXPECT_NEAR(lots, std::round(lots), 1e-9) << i;
    EXPECT_LE(std::fabs(d->book[i] - (*w)[i]), 0.5 * lot[i] + 1e-12) << i;
  }
  EXPECT_LE(d->max_violation, 0.5 * 0.005 * static_cast<f64>(kM));
}

TEST(RiskDiscretize, DeterministicAndRejectsBadInput) {
  const auto model = diag_model();
  const std::vector<f64> prev(kM, 0.0);
  const auto c = neutral_constraints(model, prev);
  const auto q = graded_q();
  const risk::ConstrainedQpSolver solver;
  const auto w = solver.solve({model, 1.0, q, c});
  ASSERT_TRUE(w);
  risk::DiscretizeCfg cfg;
  cfg.max_names = 6;
  cfg.min_trade = 0.001;
  const auto a = risk::discretize_and_resolve(solver, {model, 1.0, q, c}, prev, *w, cfg);
  const auto b = risk::discretize_and_resolve(solver, {model, 1.0, q, c}, prev, *w, cfg);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_EQ(0, std::memcmp(a->book.data(), b->book.data(), kM * sizeof(f64)));
  const std::vector<f64> short_book(kM - 1U, 0.0);
  EXPECT_FALSE(risk::discretize_and_resolve(solver, {model, 1.0, q, c}, prev, short_book, cfg));
  risk::DiscretizeCfg bad;
  bad.min_trade = -1.0;
  EXPECT_FALSE(risk::discretize_and_resolve(solver, {model, 1.0, q, c}, prev, *w, bad));
}

} // namespace atx_test_l6_optim_discretize
