// RiskCostTerms_* — Lane 6 trade-cost augmentation (per-name κ L1, 3/2-power impact via
// rotated SOC epigraphs, borrow-fee short split with a locate box). Analytic checks on a
// 2-name diagonal model where each name decouples into a 1-D problem with a closed form.

#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/cost_terms.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atx_test_l6_optim_cost_terms {
namespace risk = atx::engine::risk;
namespace la = atx::core::linalg;
using atx::f64;
using atx::usize;

// Diagonal V = I (one inert zero-exposure factor): each name is an independent 1-D QP
// min λ w² − α w + cost(w).
risk::FactorModel diag_model(usize count) {
  const auto n = static_cast<Eigen::Index>(count);
  auto model = risk::FactorModel::create(la::MatX::Zero(n, 1), la::MatX::Identity(1, 1),
                                         la::VecX::Ones(n), 0, 1);
  EXPECT_TRUE(model.has_value());
  return std::move(*model);
}

risk::MaterializedConstraints box(usize count, f64 cap) {
  risk::MaterializedConstraints c;
  const auto n = static_cast<Eigen::Index>(count);
  c.A = la::MatX::Identity(n, n);
  c.l = la::VecX::Constant(n, -cap);
  c.u = la::VecX::Constant(n, cap);
  return c;
}

risk::ConstrainedQpSolver accurate_solver() {
  risk::ConstrainedQpSolver s;
  s.cfg.iters = 3000;
  s.cfg.feas_tol = 1e-7;
  return s;
}

TEST(RiskCostTerms, AllInactiveIsByteIdenticalToPlainSolve) {
  const auto model = diag_model(2);
  const auto c = box(2, 1.0);
  const std::vector<f64> q{-0.3, 0.2};
  const std::vector<f64> zeros{0.0, 0.0};
  risk::ConstrainedQpSolver solver;
  const auto plain = solver.solve_with_cert({model, 0.5, q, c});
  ASSERT_TRUE(plain);

  risk::TradeCostTerms terms;
  terms.kappa_lin = zeros;
  terms.c_three_halves = zeros;
  terms.borrow_fee = zeros;
  terms.w_prev = zeros;
  EXPECT_FALSE(terms.active());
  const auto costed = risk::solve_with_costs(solver, {model, 0.5, q, c}, terms);
  ASSERT_TRUE(costed) << costed.error().message();
  ASSERT_EQ(costed->book.size(), plain->book.size());
  EXPECT_EQ(0, std::memcmp(costed->book.data(), plain->book.data(), 2 * sizeof(f64)));
}

TEST(RiskCostTerms, LinearKappaSoftThresholdsAroundPreviousBook) {
  // min λw² − αw + κ|w − w0|; λ = 0.5 ⇒ w = w0 inside the dead zone |α − w0| ≤ κ,
  // else w = α ∓ κ.
  const auto model = diag_model(2);
  const auto c = box(2, 10.0);
  const std::vector<f64> alpha{0.30, 0.50};
  const std::vector<f64> q{-alpha[0], -alpha[1]};
  const std::vector<f64> w0{0.25, 0.0};
  const std::vector<f64> kappa{0.10, 0.20};
  risk::TradeCostTerms terms;
  terms.kappa_lin = kappa;
  terms.w_prev = w0;
  const auto r = risk::solve_with_costs(accurate_solver(), {model, 0.5, q, c}, terms);
  ASSERT_TRUE(r) << r.error().message();
  EXPECT_NEAR(r->book[0], 0.25, 1e-6); // |0.30 − 0.25| ≤ 0.10 ⇒ no trade
  EXPECT_NEAR(r->book[1], 0.30, 1e-6); // 0.50 − 0.20
}

TEST(RiskCostTerms, BorrowFeeShrinksShortLegOnly) {
  // α < 0 ⇒ short; fee f on max(0, −w): w = α + f when α + f < 0. A long name is untouched.
  const auto model = diag_model(2);
  const auto c = box(2, 10.0);
  const std::vector<f64> q{0.40, -0.30}; // α = {−0.40, +0.30}
  const std::vector<f64> fee{0.15, 0.15};
  risk::TradeCostTerms terms;
  terms.borrow_fee = fee;
  const auto r = risk::solve_with_costs(accurate_solver(), {model, 0.5, q, c}, terms);
  ASSERT_TRUE(r) << r.error().message();
  EXPECT_NEAR(r->book[0], -0.25, 1e-6);
  EXPECT_NEAR(r->book[1], 0.30, 1e-6);
}

TEST(RiskCostTerms, LocateCapBoundsShortSize) {
  const auto model = diag_model(2);
  const auto c = box(2, 10.0);
  const std::vector<f64> q{0.40, 0.40}; // both want −0.40
  const std::vector<f64> cap{0.10, std::numeric_limits<f64>::infinity()};
  risk::TradeCostTerms terms;
  terms.locate_cap = cap;
  const auto r = risk::solve_with_costs(accurate_solver(), {model, 0.5, q, c}, terms);
  ASSERT_TRUE(r) << r.error().message();
  EXPECT_NEAR(r->book[0], -0.10, 1e-6);
  EXPECT_NEAR(r->book[1], -0.40, 1e-6);
}

TEST(RiskCostTerms, ThreeHalvesImpactMatchesClosedForm) {
  // min λw² − αw + c|w − w0|^{3/2}, trade t = w − w0 > 0: with s = √t,
  // 2λ(w0 + s²) − α + 1.5 c s = 0 ⇒ 2λ s² + 1.5 c s − (α − 2λ w0) = 0.
  const auto model = diag_model(2);
  const auto c = box(2, 10.0);
  const f64 lam = 0.5;
  const std::vector<f64> alpha{0.20, 0.05};
  const std::vector<f64> q{-alpha[0], -alpha[1]};
  const std::vector<f64> w0{0.0, -0.10};
  const std::vector<f64> c32{0.30, 0.10};
  const auto expected = [&](usize i) {
    const f64 g = alpha[i] - 2.0 * lam * w0[i];
    const f64 s = (-1.5 * c32[i] + std::sqrt(2.25 * c32[i] * c32[i] + 8.0 * lam * g)) /
                  (4.0 * lam);
    return w0[i] + s * s;
  };
  risk::TradeCostTerms terms;
  terms.c_three_halves = c32;
  terms.w_prev = w0;
  auto solver = accurate_solver();
  solver.cfg.iters = 6000;
  const auto r = risk::solve_with_costs(solver, {model, lam, q, c}, terms);
  ASSERT_TRUE(r) << r.error().message();
  EXPECT_NEAR(r->book[0], expected(0), 2e-5);
  EXPECT_NEAR(r->book[1], expected(1), 2e-5);
  // The costed trade must be strictly smaller than the frictionless one.
  EXPECT_LT(r->book[0], alpha[0] / (2.0 * lam));
}

TEST(RiskCostTerms, EvaluateMatchesHandComputedBreakdown) {
  const std::vector<f64> w{0.20, -0.30};
  const std::vector<f64> w0{0.10, 0.10};
  const std::vector<f64> kappa{0.01, 0.02};
  const std::vector<f64> c32{0.5, 0.0};
  const std::vector<f64> fee{0.03, 0.04};
  risk::TradeCostTerms terms;
  terms.kappa_lin = kappa;
  terms.c_three_halves = c32;
  terms.borrow_fee = fee;
  terms.w_prev = w0;
  const auto b = risk::evaluate_trade_costs(terms, w);
  ASSERT_TRUE(b);
  EXPECT_NEAR(b->linear, 0.01 * 0.1 + 0.02 * 0.4, 1e-15);
  EXPECT_NEAR(b->impact, 0.5 * std::pow(0.1, 1.5), 1e-15);
  EXPECT_NEAR(b->borrow, 0.04 * 0.3, 1e-15);
  EXPECT_NEAR(b->total(), b->linear + b->impact + b->borrow, 1e-18);
}

TEST(RiskCostTerms, RejectsMalformedTerms) {
  const auto model = diag_model(2);
  const auto c = box(2, 1.0);
  const std::vector<f64> q{0.0, 0.0};
  const std::vector<f64> bad_len{0.1};
  const std::vector<f64> negative{0.1, -0.1};
  const std::vector<f64> nan{0.1, std::numeric_limits<f64>::quiet_NaN()};
  const risk::ConstrainedQpSolver solver;
  risk::TradeCostTerms t1;
  t1.kappa_lin = bad_len;
  EXPECT_FALSE(risk::solve_with_costs(solver, {model, 0.5, q, c}, t1));
  risk::TradeCostTerms t2;
  t2.c_three_halves = negative;
  EXPECT_FALSE(risk::solve_with_costs(solver, {model, 0.5, q, c}, t2));
  risk::TradeCostTerms t3;
  t3.borrow_fee = nan;
  EXPECT_FALSE(risk::solve_with_costs(solver, {model, 0.5, q, c}, t3));
  risk::TradeCostTerms t4;
  t4.locate_cap = negative;
  EXPECT_FALSE(risk::solve_with_costs(solver, {model, 0.5, q, c}, t4));
}

TEST(RiskCostTerms, AugmentationAppendsColumnsAndRowsAfterBaseLayout) {
  const auto model = diag_model(2);
  const auto c = box(2, 1.0);
  const std::vector<f64> q{-0.1, 0.1};
  const auto base = risk::build_augmented(model, 0.5, q, c);
  const std::vector<f64> kappa{0.1, 0.0}; // only name 0 carries a linear cost column
  const std::vector<f64> c32{0.0, 0.2};   // only name 1 carries the 3-column impact epigraph
  const std::vector<f64> fee{0.1, 0.1};   // both names carry a short column
  risk::TradeCostTerms terms;
  terms.kappa_lin = kappa;
  terms.c_three_halves = c32;
  terms.borrow_fee = fee;
  const auto aug = risk::append_cost_terms(base, terms);
  ASSERT_TRUE(aug) << aug.error().message();
  EXPECT_EQ(aug->n_w, base.n_w);
  EXPECT_EQ(aug->n_y, base.n_y);
  EXPECT_EQ(aug->n_aux, base.n_aux + 1U + 3U + 2U);
  // κ: 2 rows; impact: 2 rows + 2 rotated cones × 3 rows; borrow: 2 rows per name.
  EXPECT_EQ(aug->A_tilde.rows(), base.A_tilde.rows() + 2 + (2 + 6) + 4);
  EXPECT_EQ(aug->cones.size(), base.cones.size() + 2U);
}

} // namespace atx_test_l6_optim_cost_terms
