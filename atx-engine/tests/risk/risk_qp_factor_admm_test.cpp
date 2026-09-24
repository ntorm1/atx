// RiskFactorAdmm_* — Lane 6 factor-space ADMM (qp_factor_admm.hpp): the Woodbury x-update
// path behind QpConfig::factor_space.
//
// Oracles: a dense equality-constrained KKT solve (closed form), the augmented
// ConstrainedQpSolver run to a tight early exit on the same problem, and a brute-force
// sort-based projection onto {lo ≤ z ≤ hi, Σ|z| ≤ G}.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/random.hpp"
#include "atx/engine/risk/admm_schedule.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/qp_factor_admm.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atx_test_l6_optim_factor_admm {
namespace risk = atx::engine::risk;
namespace la = atx::core::linalg;
using atx::f64;
using atx::usize;

struct Book {
  risk::FactorModel model;
  std::vector<f64> beta;
  std::vector<f64> q;
  std::vector<f64> q_next;
};

Book make_book(usize m, usize k, std::uint64_t seed) {
  atx::core::Xoshiro256pp rng{seed};
  const auto em = static_cast<Eigen::Index>(m);
  const auto ek = static_cast<Eigen::Index>(k);
  la::MatX x(em, ek);
  for (Eigen::Index i = 0; i < em; ++i) {
    x(i, 0) = 1.0 + 0.3 * rng.normal();
    for (Eigen::Index j = 1; j < ek; ++j) {
      x(i, j) = rng.normal();
    }
  }
  la::MatX f = la::MatX::Identity(ek, ek) * 0.01;
  f(0, 0) = 0.02;
  if (k > 1) {
    f(0, 1) = f(1, 0) = 0.002;
  }
  la::VecX d(em);
  for (Eigen::Index i = 0; i < em; ++i) {
    d[i] = 0.2 * (1.0 + 3.0 * rng.uniform01());
  }
  auto model = risk::FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  EXPECT_TRUE(model.has_value());
  Book b{std::move(*model), std::vector<f64>(m), std::vector<f64>(m), std::vector<f64>(m)};
  for (usize i = 0; i < m; ++i) {
    b.beta[i] = 0.7 + 0.6 * rng.uniform01();
    const f64 a = 0.1 * rng.normal();
    b.q[i] = -a;
    b.q_next[i] = -(a + 0.01 * rng.normal());
  }
  return b;
}

// The production shape: dollar-neutral, gross ≤ 1, |w| ≤ cap, two factor bounds, beta band.
risk::MaterializedConstraints production_set(const Book &b, usize m, f64 cap,
                                             std::span<const f64> prev = {}) {
  risk::ConstraintSet cs;
  cs.gross.gross_leverage = 1.0;
  cs.gross.dollar_neutral = true;
  cs.pos = risk::PositionCap{cap};
  cs.fexp = risk::FactorExposure{{1U, 2U}, {0.05, 0.05}};
  cs.beta = risk::BetaNeutral{std::span<const f64>(b.beta), 0.05};
  auto c = cs.materialize(b.model.exposures(), prev, m);
  EXPECT_TRUE(c.has_value());
  return std::move(*c);
}

risk::AdmmSchedule tight_schedule() {
  risk::AdmmSchedule s;
  s.early_exit = true;
  s.eps_abs = 1e-10;
  s.eps_rel = 1e-10;
  return s;
}

risk::ConstrainedQpSolver solver(bool factor_space, usize cap = 20000U) {
  risk::ConstrainedQpSolver s;
  s.cfg.iters = cap;
  s.cfg.factor_space = factor_space;
  return s;
}

f64 objective(const risk::FactorModel &v, f64 lambda, std::span<const f64> q,
              std::span<const f64> w) {
  f64 lin = 0.0;
  for (usize i = 0; i < w.size(); ++i) {
    lin += q[i] * w[i];
  }
  return lambda * v.risk(w) + lin; // ½ wᵀ(2λV)w + qᵀw
}

f64 max_abs_diff(std::span<const f64> a, std::span<const f64> b) {
  f64 d = 0.0;
  for (usize i = 0; i < a.size(); ++i) {
    d = std::max(d, std::fabs(a[i] - b[i]));
  }
  return d;
}

// Equality-only problem: min ½wᵀ(2λV)w + qᵀw s.t. Σw = 0 has the closed form
// w = −P⁻¹(q + 1ν), ν = −(1ᵀP⁻¹q)/(1ᵀP⁻¹1). The polish must land on it to rounding.
TEST(RiskFactorAdmm, EqualityOnlyMatchesClosedForm) {
  constexpr usize kM = 40;
  const Book b = make_book(kM, 4U, 11U);
  risk::MaterializedConstraints c{};
  c.A = la::MatX::Ones(1, static_cast<Eigen::Index>(kM));
  c.l = la::VecX::Zero(1);
  c.u = la::VecX::Zero(1);
  const f64 lambda = 1.5;
  const auto r = solver(true).solve_with_cert({b.model, lambda, b.q, c}, tight_schedule());
  ASSERT_TRUE(r) << r.error().message();
  EXPECT_TRUE(r->cert.factor_space);
  EXPECT_TRUE(r->cert.polished);
  la::MatX p = b.model.exposures() * b.model.factor_cov() * b.model.exposures().transpose();
  p.diagonal() += b.model.specific_var();
  p *= 2.0 * lambda;
  const Eigen::LLT<la::MatX> llt(p);
  const la::VecX q = Eigen::Map<const la::VecX>(b.q.data(), static_cast<Eigen::Index>(kM));
  const la::VecX one = la::VecX::Ones(static_cast<Eigen::Index>(kM));
  const la::VecX pq = llt.solve(q);
  const la::VecX p1 = llt.solve(one);
  const f64 nu = -one.dot(pq) / one.dot(p1);
  const la::VecX w = -(pq + nu * p1);
  for (usize i = 0; i < kM; ++i) {
    EXPECT_NEAR(r->book[i], w[static_cast<Eigen::Index>(i)], 1e-12) << i;
  }
}

// The production shape agrees with the augmented solver (both run to a tight exit).
TEST(RiskFactorAdmm, ProductionShapeMatchesAugmentedSolver) {
  // Two seeds at M = 60 keep the (slow, Debug) augmented oracle inside the Fast budget.
  for (const std::uint64_t seed : {3U, 17U}) {
    constexpr usize kM = 60;
    const Book b = make_book(kM, 6U, seed);
    const auto c = production_set(b, kM, 5.0 / static_cast<f64>(kM));
    const risk::QpProblem p{b.model, 1.0, b.q, c};
    const auto aug = solver(false).solve_with_cert(p, tight_schedule());
    const auto fs = solver(true).solve_with_cert(p, tight_schedule());
    ASSERT_TRUE(aug) << aug.error().message();
    ASSERT_TRUE(fs) << fs.error().message();
    EXPECT_FALSE(aug->cert.factor_space);
    EXPECT_TRUE(fs->cert.factor_space);
    EXPECT_LE(max_abs_diff(fs->book, aug->book), 1e-7) << "seed " << seed;
    const f64 fa = objective(b.model, 1.0, b.q, aug->book);
    const f64 ff = objective(b.model, 1.0, b.q, fs->book);
    EXPECT_LE(ff, fa + 1e-9 * (1.0 + std::fabs(fa))) << "seed " << seed;
    f64 gross = 0.0;
    f64 net = 0.0;
    for (const f64 w : fs->book) {
      gross += std::fabs(w);
      net += w;
      EXPECT_LE(std::fabs(w), 5.0 / static_cast<f64>(kM) + 1e-9);
    }
    EXPECT_LE(gross, 1.0 + 1e-9);
    EXPECT_NEAR(net, 0.0, 1e-9);
  }
}

// Turnover budget + κ penalty + √-impact surrogate: the turnover block's prox (soft-threshold
// then L1 ball, centred at w_prev) and the impact fold agree with the augmented split.
TEST(RiskFactorAdmm, TurnoverPenaltyBudgetAndImpactMatchAugmented) {
  constexpr usize kM = 36;
  const Book b = make_book(kM, 4U, 5U);
  std::vector<f64> prev(kM, 0.0);
  for (usize i = 0; i < kM; ++i) {
    prev[i] = (i % 2U == 0U ? 1.0 : -1.0) * 0.4 / static_cast<f64>(kM);
  }
  auto c = production_set(b, kM, 4.0 / static_cast<f64>(kM), prev);
  c.has_turnover = true;
  c.turnover_budget = 0.3;
  c.turnover_ref = prev;
  c.turnover_penalty = 0.002;
  c.impact.active = true;
  c.impact.coeff.assign(kM, 0.0);
  for (usize i = 0; i < kM; ++i) {
    c.impact.coeff[i] = 0.05 * static_cast<f64>(i % 5U);
  }
  const risk::QpProblem p{b.model, 1.0, b.q, c};
  const auto aug = solver(false).solve_with_cert(p, tight_schedule());
  const auto fs = solver(true).solve_with_cert(p, tight_schedule());
  ASSERT_TRUE(aug) << aug.error().message();
  ASSERT_TRUE(fs) << fs.error().message();
  EXPECT_TRUE(fs->cert.factor_space);
  EXPECT_LE(max_abs_diff(fs->book, aug->book), 1e-7);
  f64 turn = 0.0;
  for (usize i = 0; i < kM; ++i) {
    turn += std::fabs(fs->book[i] - prev[i]);
  }
  EXPECT_LE(turn, 0.3 + 1e-9);
}

// A pin (a single-nonzero equality row, as discretize appends) is honoured exactly.
TEST(RiskFactorAdmm, PinsAreHonouredExactly) {
  constexpr usize kM = 30;
  const Book b = make_book(kM, 3U, 7U);
  auto c = production_set(b, kM, 0.2);
  const Eigen::Index r0 = c.A.rows();
  la::MatX a(r0 + 2, static_cast<Eigen::Index>(kM));
  a.topRows(r0) = c.A;
  a.bottomRows(2).setZero();
  a(r0, 3) = 1.0;
  a(r0 + 1, 11) = 1.0;
  la::VecX l(r0 + 2);
  la::VecX u(r0 + 2);
  l.head(r0) = c.l;
  u.head(r0) = c.u;
  l[r0] = u[r0] = 0.01;
  l[r0 + 1] = u[r0 + 1] = 0.0;
  c.A = a;
  c.l = l;
  c.u = u;
  const auto r = solver(true).solve_with_cert({b.model, 1.0, b.q, c}, tight_schedule());
  ASSERT_TRUE(r) << r.error().message();
  EXPECT_TRUE(r->cert.polished);
  EXPECT_NEAR(r->book[3], 0.01, 1e-12);
  EXPECT_NEAR(r->book[11], 0.0, 1e-12);
}

// Warm start (previous primal, dual, ρ) converges to the next day's book in fewer
// iterations than a cold start, and the book matches the cold solve.
TEST(RiskFactorAdmm, WarmStartCutsIterationsAndMatchesCold) {
  constexpr usize kM = 200;
  const Book b = make_book(kM, 8U, 13U);
  const auto c = production_set(b, kM, 10.0 / static_cast<f64>(kM));
  risk::AdmmSchedule s;
  s.early_exit = true;
  s.eps_abs = 2e-7;
  s.eps_rel = 2e-7;
  s.check_every = 5; // fine exit granularity so the warm/cold iteration gap is visible
  const auto sv = solver(true, 5000U);
  const auto day0 = sv.solve_with_cert({b.model, 1.0, b.q, c}, s);
  ASSERT_TRUE(day0) << day0.error().message();
  const risk::WarmStart ws{day0->x_full, day0->y_full, day0->cert.rho_final};
  const risk::QpProblem p1{b.model, 1.0, b.q_next, c};
  const auto cold = sv.solve_with_cert(p1, s);
  const auto warm = sv.solve_with_cert(p1, s, &ws);
  ASSERT_TRUE(cold) << cold.error().message();
  ASSERT_TRUE(warm) << warm.error().message();
  EXPECT_LT(warm->cert.admm_iters, cold->cert.admm_iters);
  EXPECT_LE(max_abs_diff(warm->book, cold->book), 1e-7)
      << "polished cold/warm " << cold->cert.polished << "/" << warm->cert.polished
      << " prim " << cold->cert.prim_res << "/" << warm->cert.prim_res << " day0 polished "
      << day0->cert.polished << " iters " << day0->cert.admm_iters;
  // A warm start in the augmented layout is detected by length and ignored (cold start).
  const auto aug0 = solver(false).solve_with_cert({b.model, 1.0, b.q, c}, s);
  ASSERT_TRUE(aug0) << aug0.error().message();
  const risk::WarmStart foreign{aug0->x_full, aug0->y_full, 0.0};
  const auto mixed = sv.solve_with_cert(p1, s, &foreign);
  ASSERT_TRUE(mixed) << mixed.error().message();
  EXPECT_EQ(0, std::memcmp(mixed->book.data(), cold->book.data(), kM * sizeof(f64)));
}

TEST(RiskFactorAdmm, DeterministicByteIdentical) {
  constexpr usize kM = 120;
  const Book b = make_book(kM, 5U, 19U);
  const auto c = production_set(b, kM, 8.0 / static_cast<f64>(kM));
  const risk::QpProblem p{b.model, 1.0, b.q, c};
  const auto a = solver(true, 3000U).solve_with_cert(p, tight_schedule());
  const auto z = solver(true, 3000U).solve_with_cert(p, tight_schedule());
  ASSERT_TRUE(a);
  ASSERT_TRUE(z);
  EXPECT_EQ(a->cert.admm_iters, z->cert.admm_iters);
  EXPECT_EQ(0, std::memcmp(a->book.data(), z->book.data(), kM * sizeof(f64)));
}

// Cones are out of scope: the flag falls back to the augmented path (never an error).
TEST(RiskFactorAdmm, ConeSetsFallBackToAugmentedPath) {
  constexpr usize kM = 20;
  const Book b = make_book(kM, 3U, 23U);
  risk::ConstraintSet cs;
  cs.gross.gross_leverage = 1.0;
  cs.gross.dollar_neutral = true;
  cs.track = risk::TrackingError{{}, 0.05};
  auto c = cs.materialize(b.model.exposures(), {}, kM);
  ASSERT_TRUE(c);
  EXPECT_FALSE(risk::factor_admm_eligible(*c));
  const auto r = solver(true, 3000U).solve_with_cert({b.model, 1.0, b.q, *c}, tight_schedule());
  ASSERT_TRUE(r) << r.error().message();
  EXPECT_FALSE(r->cert.factor_space);
}

// An infeasible set fails typed (never a silently clamped book).
TEST(RiskFactorAdmm, InfeasibleSetFailsTyped) {
  constexpr usize kM = 10;
  const Book b = make_book(kM, 2U, 31U);
  risk::MaterializedConstraints c{};
  c.A = la::MatX::Zero(1 + static_cast<Eigen::Index>(kM), static_cast<Eigen::Index>(kM));
  c.l = la::VecX::Zero(1 + static_cast<Eigen::Index>(kM));
  c.u = la::VecX::Zero(1 + static_cast<Eigen::Index>(kM));
  c.A.row(0).setOnes();
  c.l[0] = c.u[0] = 1.0; // Σw = 1 ...
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(kM); ++i) {
    c.A(1 + i, i) = 1.0;
    c.l[1 + i] = -0.01; // ... with |w_i| ≤ 0.01
    c.u[1 + i] = 0.01;
  }
  const auto r = solver(true, 2000U).solve_with_cert({b.model, 1.0, b.q, c}, tight_schedule());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
  // Contradictory single-name bounds are rejected up front.
  c.A.row(0).setZero();
  c.l[0] = c.u[0] = 0.0;
  c.l[1] = 0.5;
  c.u[1] = 0.6; // w_0 ∈ [0.5, 0.6] ...
  la::MatX a2(c.A.rows() + 1, c.A.cols());
  a2.topRows(c.A.rows()) = c.A;
  a2.bottomRows(1).setZero();
  a2(c.A.rows(), 0) = 2.0; // ... and 2 w_0 ∈ [0, 0.2]
  la::VecX l2(c.l.size() + 1);
  la::VecX u2(c.u.size() + 1);
  l2.head(c.l.size()) = c.l;
  u2.head(c.u.size()) = c.u;
  l2[c.l.size()] = 0.0;
  u2[c.u.size()] = 0.2;
  c.A = a2;
  c.l = l2;
  c.u = u2;
  const auto r2 = solver(true, 200U).solve_with_cert({b.model, 1.0, b.q, c}, tight_schedule());
  ASSERT_FALSE(r2);
  EXPECT_EQ(r2.error().code(), atx::core::ErrorCode::InvalidArgument);
}

// Brute-force projection onto {lo ≤ z ≤ hi, Σ|z| ≤ G}: bisection on τ to machine precision.
std::vector<f64> brute_project(const std::vector<f64> &v, const std::vector<f64> &lo,
                               const std::vector<f64> &hi, f64 g) {
  auto at = [&](f64 tau) {
    std::vector<f64> z(v.size());
    for (usize i = 0; i < v.size(); ++i) {
      z[i] = risk::detail::fa_soft_clamp(v[i], tau, lo[i], hi[i]);
    }
    return z;
  };
  auto l1 = [](const std::vector<f64> &z) {
    f64 s = 0.0;
    for (const f64 x : z) {
      s += std::fabs(x);
    }
    return s;
  };
  if (l1(at(0.0)) <= g) {
    return at(0.0);
  }
  f64 a = 0.0;
  f64 b = 0.0;
  for (const f64 x : v) {
    b = std::max(b, std::fabs(x));
  }
  for (int it = 0; it < 400; ++it) {
    const f64 c = 0.5 * (a + b);
    (l1(at(c)) > g ? a : b) = c;
  }
  return at(b);
}

TEST(RiskFactorAdmm, BoxL1ProjectionMatchesBruteForce) {
  atx::core::Xoshiro256pp rng{41U};
  for (int trial = 0; trial < 50; ++trial) {
    const usize n = 5U + static_cast<usize>(trial);
    la::VecX v(static_cast<Eigen::Index>(n));
    std::vector<f64> vv(n);
    std::vector<f64> lo(n);
    std::vector<f64> hi(n);
    const std::vector<atx::u8> on(n, 1U);
    for (usize i = 0; i < n; ++i) {
      vv[i] = rng.normal();
      v[static_cast<Eigen::Index>(i)] = vv[i];
      const f64 cap = 0.2 + rng.uniform01();
      lo[i] = (trial % 3 == 0) ? 0.1 * rng.uniform01() - 0.05 : -cap; // some boxes exclude 0
      hi[i] = cap;
    }
    const f64 g = 0.3 * static_cast<f64>(n) * rng.uniform01();
    const f64 tau = risk::detail::fa_find_tau(v, lo, hi, on, g, 0.0);
    const auto ref = brute_project(vv, lo, hi, g);
    for (usize i = 0; i < n; ++i) {
      EXPECT_NEAR(risk::detail::fa_soft_clamp(vv[i], tau, lo[i], hi[i]), ref[i], 1e-12)
          << "trial " << trial << " i " << i;
    }
  }
}

} // namespace atx_test_l6_optim_factor_admm
