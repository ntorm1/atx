// RiskAdmmSchedule_* — Lane 6 deterministic adaptive-ρ schedule, warm start and the
// deterministic early exit on ConstrainedQpSolver.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/risk/admm_schedule.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atx_test_l6_optim_admm_schedule {
namespace risk = atx::engine::risk;
namespace la = atx::core::linalg;
using atx::f64;
using atx::usize;

struct Lcg {
  std::uint64_t s;
  f64 next() {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return static_cast<f64>(s >> 11) / static_cast<f64>(1ULL << 53) - 0.5;
  }
};

struct Fixture {
  risk::FactorModel model;
  std::vector<f64> q;
  std::vector<f64> prev;
  risk::MaterializedConstraints c;
};

// A realistic factor book: M names, K factors, dollar-neutral, gross ≤ 1, |w| ≤ 0.08,
// turnover ≤ 0.6 from a nonzero previous book — every constraint family of the pin.
Fixture make_fixture(usize m, usize k, std::uint64_t seed) {
  Lcg g{seed};
  const auto em = static_cast<Eigen::Index>(m);
  const auto ek = static_cast<Eigen::Index>(k);
  la::MatX x(em, ek);
  for (Eigen::Index i = 0; i < em; ++i) {
    for (Eigen::Index j = 0; j < ek; ++j) {
      x(i, j) = 2.0 * g.next();
    }
  }
  la::MatX f = la::MatX::Identity(ek, ek) * 0.02;
  la::VecX d(em);
  for (Eigen::Index i = 0; i < em; ++i) {
    d[i] = 0.01 + 0.02 * (g.next() + 0.5);
  }
  auto model = risk::FactorModel::create(x, f, d, 0, 1);
  EXPECT_TRUE(model.has_value());
  Fixture fx{std::move(*model), {}, {}, {}};
  fx.q.resize(m);
  fx.prev.resize(m);
  for (usize i = 0; i < m; ++i) {
    fx.q[i] = -0.02 * g.next();
    fx.prev[i] = 0.02 * g.next();
  }
  f64 mean = 0.0;
  for (const f64 v : fx.prev) {
    mean += v;
  }
  mean /= static_cast<f64>(m);
  f64 gross = 0.0;
  for (f64 &v : fx.prev) {
    v -= mean;
    gross += std::fabs(v);
  }
  for (f64 &v : fx.prev) {
    v *= 0.8 / gross; // a feasible previous book strictly inside the gross budget
  }
  risk::ConstraintSet spec;
  spec.gross = {1.0, true};
  spec.pos = risk::PositionCap{0.08};
  spec.turn = risk::TurnoverBudget{0.6};
  auto c = spec.materialize(fx.model.exposures(), fx.prev, m);
  EXPECT_TRUE(c.has_value());
  fx.c = std::move(*c);
  return fx;
}

TEST(RiskAdmmSchedule, RoundPow2SnapsToNearestPowerOfTwo) {
  EXPECT_EQ(risk::round_pow2(1.0), 1.0);
  EXPECT_EQ(risk::round_pow2(3.1), 4.0);
  EXPECT_EQ(risk::round_pow2(0.3), 0.25);
  EXPECT_EQ(risk::round_pow2(1000.0), 1024.0);
  EXPECT_EQ(risk::round_pow2(0.0), 0.0); // degenerate input passes through
}

TEST(RiskAdmmSchedule, AdaptRhoBalancesResidualsAndClamps) {
  const risk::AdmmSchedule s;
  risk::AdmmResidualNorms r;
  r.prim = 1.0;
  r.ax = 1.0;
  r.dual = 1e-4;
  r.px = 1.0;
  EXPECT_EQ(risk::adapt_rho(1.0, r, s), 128.0); // sqrt(1e4) = 100 → 2^7
  r.prim = 1e-4;
  r.dual = 1.0;
  EXPECT_EQ(risk::adapt_rho(1.0, r, s), 0.0078125); // 0.01 → 2^-7
  r.prim = 1.0;
  r.dual = 1e-20;
  EXPECT_EQ(risk::adapt_rho(1.0, r, s), risk::round_pow2(s.rho_max));
  r.prim = 0.0; // degenerate ⇒ unchanged
  EXPECT_EQ(risk::adapt_rho(2.0, r, s), 2.0);
}

TEST(RiskAdmmSchedule, ScheduledSolveIsDeterministicAcrossRuns) {
  const auto fx = make_fixture(60, 4, 7);
  risk::ConstrainedQpSolver solver;
  const risk::AdmmSchedule sched;
  const auto a = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, sched);
  const auto b = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, sched);
  ASSERT_TRUE(a) << a.error().message();
  ASSERT_TRUE(b) << b.error().message();
  ASSERT_EQ(a->book.size(), b->book.size());
  EXPECT_EQ(0, std::memcmp(a->book.data(), b->book.data(), a->book.size() * sizeof(f64)));
  EXPECT_EQ(a->cert.rho_final, b->cert.rho_final);
}

TEST(RiskAdmmSchedule, KktResidualAfterScheduleIsTight) {
  const auto fx = make_fixture(60, 4, 11);
  risk::ConstrainedQpSolver solver;
  const auto r = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, risk::AdmmSchedule{});
  ASSERT_TRUE(r) << r.error().message();
  EXPECT_LE(r->cert.prim_res, 1e-8);
  EXPECT_LE(r->cert.dual_res, 1e-8);
}

TEST(RiskAdmmSchedule, ScheduleBeatsFixedRhoWithoutPolish) {
  const auto fx = make_fixture(80, 5, 13);
  risk::ConstrainedQpSolver solver;
  solver.cfg.polish = false;
  solver.cfg.iters = 100;
  solver.cfg.feas_tol = 1e-2; // compare accuracy, not the gate
  const auto fixed = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c});
  const auto sched = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, risk::AdmmSchedule{});
  ASSERT_TRUE(fixed) << fixed.error().message();
  ASSERT_TRUE(sched) << sched.error().message();
  const f64 fixed_res = std::max(fixed->cert.prim_res, fixed->cert.dual_res);
  const f64 sched_res = std::max(sched->cert.prim_res, sched->cert.dual_res);
  EXPECT_LT(sched_res, fixed_res);
}

TEST(RiskAdmmSchedule, DefaultPathUnchangedByNewResultFields) {
  // The unscheduled solve must stay the historical fixed-iteration operator.
  const auto fx = make_fixture(30, 3, 17);
  risk::ConstrainedQpSolver solver;
  const auto a = solver.solve({fx.model, 1.0, fx.q, fx.c});
  const auto b = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c});
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_EQ(0, std::memcmp(a->data(), b->book.data(), a->size() * sizeof(f64)));
  EXPECT_EQ(b->cert.admm_iters, solver.cfg.iters);
  EXPECT_EQ(b->x_full.size(), b->book.size() + 3U + 30U + 30U); // w | y | s | r
}

TEST(RiskAdmmSchedule, WarmStartWithEarlyExitConvergesFasterToSameBook) {
  const auto fx = make_fixture(60, 4, 19);
  risk::ConstrainedQpSolver solver;
  solver.cfg.iters = 400;
  risk::AdmmSchedule sched;
  sched.early_exit = true;
  sched.eps_abs = 1e-9;
  sched.eps_rel = 1e-9;
  const auto cold = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, sched);
  ASSERT_TRUE(cold) << cold.error().message();
  const risk::WarmStart ws{cold->x_full, cold->y_full};
  const auto warm = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, sched, &ws);
  ASSERT_TRUE(warm) << warm.error().message();
  EXPECT_LT(warm->cert.admm_iters, cold->cert.admm_iters);
  for (usize i = 0; i < cold->book.size(); ++i) {
    EXPECT_NEAR(warm->book[i], cold->book[i], 1e-7) << i;
  }
}

TEST(RiskAdmmSchedule, WarmStartZeroRhoIsByteIdenticalToOmittingIt) {
  const auto fx = make_fixture(40, 3, 23);
  risk::ConstrainedQpSolver solver;
  const risk::AdmmSchedule sched{};
  const auto cold = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, sched);
  ASSERT_TRUE(cold) << cold.error().message();
  const risk::WarmStart plain{cold->x_full, cold->y_full};
  const risk::WarmStart zero{cold->x_full, cold->y_full, 0.0};
  const auto a = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, sched, &plain);
  const auto b = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, sched, &zero);
  ASSERT_TRUE(a && b);
  EXPECT_EQ(0, std::memcmp(a->book.data(), b->book.data(), a->book.size() * sizeof(f64)));
}

TEST(RiskAdmmSchedule, WarmStartCarriesAdaptedRhoAndStartsFromIt) {
  // Seeding the previous solve's adapted rho must be honored: with no refactor points the
  // run never adapts, so the final rho IS the seed; the warm solve stays on the same book.
  const auto fx = make_fixture(60, 4, 29);
  risk::ConstrainedQpSolver solver;
  solver.cfg.iters = 400;
  risk::AdmmSchedule sched;
  sched.early_exit = true;
  sched.eps_abs = 1e-9;
  sched.eps_rel = 1e-9;
  const auto cold = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, sched);
  ASSERT_TRUE(cold) << cold.error().message();
  const risk::WarmStart ws{cold->x_full, cold->y_full, cold->cert.rho_final};
  const auto warm = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, sched, &ws);
  ASSERT_TRUE(warm) << warm.error().message();
  for (usize i = 0; i < cold->book.size(); ++i) {
    EXPECT_NEAR(warm->book[i], cold->book[i], 1e-7) << i;
  }
  risk::AdmmSchedule frozen = sched;
  frozen.refactor_at = {0U, 0U, 0U};
  const risk::WarmStart seeded{cold->x_full, cold->y_full, 8.0};
  const auto f = solver.solve_with_cert({fx.model, 1.0, fx.q, fx.c}, frozen, &seeded);
  ASSERT_TRUE(f) << f.error().message();
  EXPECT_EQ(f->cert.rho_final, 8.0);
}

} // namespace atx_test_l6_optim_admm_schedule
