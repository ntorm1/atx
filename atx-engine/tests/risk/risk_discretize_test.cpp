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

// Review regression: the cap binds even when the continuous book already holds <= max_names
// names. Name 1 is not held by w_cont but sits at 0.05 in w_prev; min-trade used to re-pin
// it there (the all-pinned path, no solver call), returning 2 names under a cap of 1.
TEST(RiskDiscretize, MaxNamesCapHoldsWhenMinTradeWouldRepinAnExitingName) {
  auto model = risk::FactorModel::create(la::MatX::Zero(2, 1), la::MatX::Identity(1, 1),
                                         la::VecX::Constant(2, 0.04), 0, 1);
  ASSERT_TRUE(model);
  const risk::MaterializedConstraints c{};
  const std::vector<f64> q{-0.01, 0.0};
  const std::vector<f64> w_cont{0.3, 0.0};
  const std::vector<f64> w_prev{0.3, 0.05};
  risk::DiscretizeCfg cfg;
  cfg.max_names = 1;
  cfg.min_trade = 0.1;
  const risk::ConstrainedQpSolver solver;
  const auto d = risk::discretize_and_resolve(solver, {*model, 1.0, q, c}, w_prev, w_cont, cfg);
  ASSERT_TRUE(d) << d.error().message();
  EXPECT_LE(d->n_names, 1U);
  EXPECT_EQ(d->book[0], 0.3);
  EXPECT_EQ(d->book[1], 0.0);
  EXPECT_EQ(d->pinned[1], 1U);
}

// Same invariant through the re-solve path: an un-held name with a large w_prev must stay
// flat even though the re-solve re-optimizes the book around the pins.
TEST(RiskDiscretize, MaxNamesCapHoldsThroughResolveForUnheldNames) {
  const auto model = diag_model();
  const auto q = graded_q();
  const risk::ConstrainedQpSolver solver;
  std::vector<f64> prev(kM, 0.0);
  const auto c0 = neutral_constraints(model, prev);
  const auto w = solver.solve({model, 1.0, q, c0});
  ASSERT_TRUE(w);
  std::vector<f64> w_cont = *w;
  for (const usize i : {2U, 3U, 4U, 5U, 6U, 7U}) {
    w_cont[i] = 0.0; // a continuous book holding exactly 4 names
  }
  prev[3] = 0.2; // exiting positions the continuous book does not hold
  prev[6] = -0.2;
  risk::DiscretizeCfg cfg;
  cfg.max_names = 4;
  cfg.min_trade = 0.001;
  const auto d = risk::discretize_and_resolve(solver, {model, 1.0, q, c0}, prev, w_cont, cfg);
  ASSERT_TRUE(d) << d.error().message();
  EXPECT_LE(d->n_names, 4U);
  EXPECT_EQ(d->book[3], 0.0);
  EXPECT_EQ(d->book[6], 0.0);
}

// The re-solve takes the caller's schedule and warm start: warm-started from the continuous
// solve it reaches the cold re-solve's book to the solve tolerance.
TEST(RiskDiscretize, ResolveHonoursScheduleAndWarmStart) {
  const auto model = diag_model();
  const std::vector<f64> prev(kM, 0.0);
  const auto c = neutral_constraints(model, prev);
  const auto q = graded_q();
  risk::ConstrainedQpSolver solver;
  solver.cfg.iters = 5000U;
  risk::AdmmSchedule sched;
  sched.early_exit = true;
  sched.eps_abs = 1e-9;
  sched.eps_rel = 1e-9;
  const risk::QpProblem p{model, 1.0, q, c};
  const auto cont = solver.solve_with_cert(p, sched);
  ASSERT_TRUE(cont) << cont.error().message();
  risk::DiscretizeCfg cfg;
  cfg.max_names = 4;
  cfg.schedule = sched;
  const auto cold = risk::discretize_and_resolve(solver, p, prev, cont->book, cfg);
  cfg.warm = &*cont;
  const auto warm = risk::discretize_and_resolve(solver, p, prev, cont->book, cfg);
  ASSERT_TRUE(cold) << cold.error().message();
  ASSERT_TRUE(warm) << warm.error().message();
  EXPECT_TRUE(warm->resolved);
  for (usize i = 0; i < kM; ++i) {
    EXPECT_NEAR(warm->book[i], cold->book[i], 1e-6) << i;
  }
  EXPECT_LE(warm->n_names, 4U);
  // The pin dual re-layout: y_full of the un-pinned problem gains one 0 per pin row.
  const auto seed = risk::detail::pin_dual_seed(p, cont->y_full, 6U);
  EXPECT_EQ(seed.size(), cont->y_full.size() + 6U);
  EXPECT_TRUE(risk::detail::pin_dual_seed(p, {}, 6U).empty());
}

// The pin re-solve through the factor-space path (pins fold into the box block; the warm
// dual layout is unchanged by the pins) agrees with the augmented re-solve.
TEST(RiskDiscretize, FactorSpaceResolveMatchesAugmented) {
  const auto model = diag_model();
  const std::vector<f64> prev(kM, 0.0);
  const auto c = neutral_constraints(model, prev);
  const auto q = graded_q();
  risk::AdmmSchedule sched;
  sched.early_exit = true;
  sched.eps_abs = 1e-10;
  sched.eps_rel = 1e-10;
  const risk::QpProblem p{model, 1.0, q, c};
  risk::ConstrainedQpSolver aug;
  aug.cfg.iters = 20000U;
  risk::ConstrainedQpSolver fs = aug;
  fs.cfg.factor_space = true;
  const auto cont = fs.solve_with_cert(p, sched);
  ASSERT_TRUE(cont) << cont.error().message();
  ASSERT_TRUE(cont->cert.factor_space);
  risk::DiscretizeCfg cfg;
  cfg.max_names = 4;
  cfg.schedule = sched;
  const auto a = risk::discretize_and_resolve(aug, p, prev, cont->book, cfg);
  cfg.warm = &*cont;
  const auto f = risk::discretize_and_resolve(fs, p, prev, cont->book, cfg);
  ASSERT_TRUE(a) << a.error().message();
  ASSERT_TRUE(f) << f.error().message();
  EXPECT_TRUE(f->resolved);
  EXPECT_LE(f->n_names, 4U);
  for (usize i = 0; i < kM; ++i) {
    EXPECT_NEAR(f->book[i], a->book[i], 1e-7) << i;
  }
  EXPECT_LE(f->max_violation, 1e-9);
}

TEST(RiskDiscretizeR1, PinsAppendAfterImplicitCsrBoxesWithoutDenseMaterialization) {
  risk::ConstraintSet cs;
  cs.pos = risk::PositionCap{0.5};
  cs.storage.rule = risk::ConstraintStorageRule::SparseCsrV2;
  const atx::core::linalg::MatX x = atx::core::linalg::MatX::Zero(3, 1);
  const auto original = cs.materialize(x, {}, 3);
  ASSERT_TRUE(original);
  const std::vector<atx::u8> pinned{1, 0, 1};
  const std::vector<atx::f64> target{0.1, 0.0, -0.1};
  const auto constrained = risk::detail::with_pins(*original, pinned, target);
  ASSERT_TRUE(constrained) << constrained.error().message();
  EXPECT_EQ(constrained->A.size(), 0);
  EXPECT_EQ(constrained->row_count(), 6U);
  EXPECT_EQ(constrained->csr.box_count, 3U);
  EXPECT_EQ(constrained->csr.column_indices[3], 0U);
  EXPECT_EQ(constrained->csr.column_indices[4], 2U);
  EXPECT_DOUBLE_EQ(constrained->l[4], 0.1);
  EXPECT_DOUBLE_EQ(constrained->u[5], -0.1);
  EXPECT_DOUBLE_EQ(risk::detail::book_violation(*constrained, target), 0.0);
  auto limited = *original;
  limited.storage.max_nnz = original->stored_nonzeros() + 1;
  EXPECT_FALSE(risk::detail::with_pins(limited, pinned, target));
}

} // namespace atx_test_l6_optim_discretize
