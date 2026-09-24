// optimizer_production_bench.cpp — Lane 6: the PRODUCTION constrained solve at scale.
//
// Times ConstrainedQpSolver end to end (factor-augmented build → Ruiz(10) → ADMM(300) →
// polish → feasibility gate) on a realistic book: dollar-neutral, gross ≤ 1, |w| ≤ 10/M,
// two factor-exposure bounds and a beta band, with K = 64 factors, over
// M ∈ {1000, 3000, 5000}. Modes (range 1):
//   0  fixed ρ, cold           — the historical operator (the byte-pinned path)
//   1  fixed ρ, warm           — seeded from the previous day's x/y (QpProblem x0/y0)
//   2  schedule, cold          — AdmmSchedule (per-row ρ, pow2 adaptive ρ, α = 1.6)
//   3  schedule, warm
//   4  schedule + early exit, warm — the deterministic residual exit (check_every 25,
//      eps 2e-7) with cfg.iters = 3000 as a CAP; the warm start also carries the adapted ρ
//   5  schedule + early exit, cold
// "Warm" = the solve of a perturbed next-day alpha (10% idiosyncratic noise) seeded from
// the previous day's full primal/dual; the warm handles are computed once outside the
// timed loop. Counters: admm iterations actually run, final primal/dual residuals.
//
// Release numbers come from the equity-bench preset (build only atx-engine-bench):
//   atx-engine-bench --benchmark_filter=BM_OptimizerProduction

#include <span>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include <Eigen/Dense>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/admm_schedule.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/kkt_ldl.hpp"
#include "atx/engine/risk/qp_augment.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atx_bench_l6_optim_production {

using atx::f64;
using atx::usize;
using atx::core::Xoshiro256pp;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
namespace risk = atx::engine::risk;

struct Book {
  risk::FactorModel model;
  std::vector<f64> beta;
  std::vector<f64> q_day0;
  std::vector<f64> q_day1;
  risk::MaterializedConstraints c;
};

[[nodiscard]] Book make_book(usize m, usize k) {
  Xoshiro256pp rng{0x1A6EULL};
  const auto em = static_cast<Eigen::Index>(m);
  const auto ek = static_cast<Eigen::Index>(k);
  MatX x(em, ek);
  for (Eigen::Index i = 0; i < em; ++i) {
    x(i, 0) = 1.0 + 0.3 * rng.normal(); // market-like first factor
    for (Eigen::Index j = 1; j < ek; ++j) {
      x(i, j) = rng.normal();
    }
  }
  MatX f = MatX::Identity(ek, ek) * 0.01;
  f(0, 0) = 0.02;
  VecX d(em);
  for (Eigen::Index i = 0; i < em; ++i) {
    d[i] = 0.2 * (1.0 + 3.0 * rng.uniform01());
  }
  auto model = risk::FactorModel::create(std::move(x), std::move(f), std::move(d), 0U, 1U);
  Book b{std::move(*model), std::vector<f64>(m), std::vector<f64>(m), std::vector<f64>(m), {}};
  for (usize i = 0; i < m; ++i) {
    b.beta[i] = 0.7 + 0.6 * rng.uniform01();
    const f64 a = 0.1 * rng.normal();
    b.q_day0[i] = -a;
    b.q_day1[i] = -(a + 0.01 * rng.normal());
  }
  risk::ConstraintSet cs;
  cs.gross.gross_leverage = 1.0;
  cs.gross.dollar_neutral = true;
  cs.pos = risk::PositionCap{10.0 / static_cast<f64>(m)};
  cs.fexp = risk::FactorExposure{{1U, 2U}, {0.05, 0.05}};
  cs.beta = risk::BetaNeutral{std::span<const f64>(b.beta), 0.05};
  auto c = cs.materialize(b.model.exposures(), {}, m);
  b.c = std::move(*c);
  return b;
}

void BM_OptimizerProduction(benchmark::State &state) {
  const auto m = static_cast<usize>(state.range(0));
  const auto mode = state.range(1);
  const Book b = make_book(m, 64U);
  risk::ConstrainedQpSolver solver; // shipped defaults: iters 300, Ruiz 10, polish on
  solver.cfg.iters = 300U;
  risk::AdmmSchedule sched;
  sched.early_exit = (mode == 4 || mode == 5);
  if (sched.early_exit) {
    solver.cfg.iters = 3000U; // a CAP: the deterministic residual exit ends the solve
  }
  sched.eps_abs = 2e-7;
  sched.eps_rel = 2e-7;
  const bool use_sched = mode >= 2;
  const bool warm = (mode == 1 || mode == 3 || mode == 4);

  // Day 0 (untimed) supplies the warm handles.
  const risk::QpProblem p0{b.model, 1.0, std::span<const f64>(b.q_day0), b.c};
  auto r0 = use_sched ? solver.solve_with_cert(p0, sched) : solver.solve_with_cert(p0);
  if (!r0) {
    state.SkipWithError(("day-0: " + r0.error().to_string()).c_str());
    return;
  }
  const risk::WarmStart ws{std::span<const f64>(r0->x_full), std::span<const f64>(r0->y_full),
                          r0->cert.rho_final};
  risk::QpProblem p1{b.model, 1.0, std::span<const f64>(b.q_day1), b.c};
  if (warm && !use_sched) {
    p1.x0 = ws.x0;
    p1.y0 = ws.y0;
  }

  f64 iters = 0.0;
  f64 prim = 0.0;
  f64 dual = 0.0;
  for (auto _ : state) {
    auto r = use_sched ? solver.solve_with_cert(p1, sched, warm ? &ws : nullptr)
                       : solver.solve_with_cert(p1);
    if (!r) {
      state.SkipWithError(("day-1: " + r.error().to_string()).c_str());
      return;
    }
    iters = static_cast<f64>(r->cert.admm_iters);
    prim = r->cert.prim_res;
    dual = r->cert.dual_res;
    benchmark::DoNotOptimize(r);
    benchmark::ClobberMemory();
  }
  state.counters["admm_iters"] = iters;
  state.counters["prim_res"] = prim;
  state.counters["dual_res"] = dual;
}
BENCHMARK(BM_OptimizerProduction)
    ->ArgsProduct({{1000, 3000, 5000}, {0, 1, 2, 3, 4, 5}})
    ->ArgNames({"M", "mode"})
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime()
    ->Iterations(3);

// Diagnostic: the ADMM KKT's LDLᵀ fill and the cost of ONE triangular solve — the per-
// iteration floor of the direct-KKT ADMM (the KKT is assembled exactly as the solver's
// build_kkt does: [P + σI, Ãᵀ; Ã, −ρ⁻¹I]).
void BM_OptimizerProductionKktSolve(benchmark::State &state) {
  using SpMat = Eigen::SparseMatrix<f64>;
  const auto m = static_cast<usize>(state.range(0));
  const Book b = make_book(m, 64U);
  const risk::AugmentedQp aug =
      risk::build_augmented(b.model, 1.0, std::span<const f64>(b.q_day0), b.c);
  const auto n = static_cast<int>(aug.n_w + aug.n_y + aug.n_aux);
  const auto r = static_cast<int>(aug.A_tilde.rows());
  std::vector<Eigen::Triplet<f64>> t;
  for (int c = 0; c < n; ++c) {
    t.emplace_back(c, c, 1e-6);
    for (SpMat::InnerIterator it(aug.P, c); it; ++it) {
      t.emplace_back(static_cast<int>(it.row()), c, it.value());
    }
    for (SpMat::InnerIterator it(aug.A_tilde, c); it; ++it) {
      t.emplace_back(n + static_cast<int>(it.row()), c, it.value());
      t.emplace_back(c, n + static_cast<int>(it.row()), it.value());
    }
  }
  for (int i = 0; i < r; ++i) {
    t.emplace_back(n + i, n + i, -1.0);
  }
  SpMat kkt(n + r, n + r);
  kkt.setFromTriplets(t.begin(), t.end());
  kkt.makeCompressed();
  risk::QuasiDefiniteLdl ldl;
  if (!ldl.factor_symbolic(kkt) || !ldl.factor_numeric(kkt)) {
    state.SkipWithError("factor failed");
    return;
  }
  std::vector<f64> rhs(static_cast<usize>(n + r), 1.0);
  std::vector<f64> sol(rhs.size(), 0.0);
  for (auto _ : state) {
    ldl.solve(std::span<const f64>(rhs), std::span<f64>(sol));
    benchmark::DoNotOptimize(sol.data());
  }
  state.counters["dim"] = static_cast<f64>(n + r);
  state.counters["kkt_nnz"] = static_cast<f64>(kkt.nonZeros());
  state.counters["L_nnz"] = static_cast<f64>(ldl.Li().size());
}
BENCHMARK(BM_OptimizerProductionKktSolve)
    ->Arg(1000)->Arg(3000)->Arg(5000)
    ->Unit(benchmark::kMicrosecond);

} // namespace atx_bench_l6_optim_production
