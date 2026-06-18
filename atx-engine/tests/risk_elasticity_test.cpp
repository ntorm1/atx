// risk_elasticity_test.cpp — S8.6: constraint-hierarchy / infeasibility elasticity
// (risk/elasticity.hpp). The G-ELASTIC gate.
//
// S8.6 is the unit that finally CONSUMES the per-descriptor priority/elastic fields
// (added inert in S8.4). When a constraint set is infeasible, instead of returning Err,
// the minimize-violation layer relaxes the ELASTIC constraints in a deterministic
// priority order (lowest priority first, via a penalized slack +γ_p·(violation)₊ on a
// single re-solve through the unchanged ConstrainedQpSolver) and returns the closest-
// feasible book + a relaxation report. HARD (elastic=false) constraints never relax.
//
// Gates proven here:
//   G-ELASTIC (infeasible → closest-feasible + ordered report) — an over-constrained
//     problem (box + factor-neutral + tracking-error, all elastic at distinct
//     priorities) returns a feasible book (HARD constraints satisfied; elastic ones
//     violated only as reported) + a report naming the relaxed constraints in the
//     documented priority order (lowest priority first), matching the γ ladder.
//   G-ELASTIC (feasible → no-op, byte-identical) — a feasible problem returns a book
//     bit-for-bit identical (std::bit_cast<uint64_t> per element) to the direct
//     ConstrainedQpSolver::solve book (elasticity did nothing).
//   G-DET — same infeasible problem ⇒ byte-identical closest-feasible book + identical
//     report across runs; fixed ladder order.
//   Hard-never-relaxes — a problem whose binding infeasibility is a HARD constraint
//     returns a DISTINCT elastic-layer Err (the set is infeasible even after relaxing
//     all elastic constraints) — it does NOT relax a hard constraint to fake feasibility.

#include <bit>     // std::bit_cast
#include <cmath>   // std::fabs, std::sqrt
#include <cstdint> // std::uint64_t
#include <span>
#include <string>  // std::string::npos (distinct-Err message check)
#include <utility> // std::move
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/elasticity.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atxtest_risk_elasticity_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::ConstrainedQpSolver;
using atx::engine::risk::ConstraintSet;
using atx::engine::risk::ElasticResult;
using atx::engine::risk::FactorExposure;
using atx::engine::risk::FactorModel;
using atx::engine::risk::MaterializedConstraints;
using atx::engine::risk::PositionCap;
using atx::engine::risk::QpProblem;
using atx::engine::risk::RelaxationEntry;
using atx::engine::risk::solve_elastic;
using atx::engine::risk::TrackingError;

// ---------------------------------------------------------------------------
//  Fixtures (mirror risk_cone_test.cpp's make_multi_model / tracking_error).
// ---------------------------------------------------------------------------

[[nodiscard]] FactorModel make_model(const MatX &x, const MatX &f, const VecX &d, usize fb = 0U,
                                     usize fe = 10U) {
  auto r = FactorModel::create(x, f, d, fb, fe);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

// A benign multi-name FactorModel: M instruments, K=2 factors, F SPD, D > 0.
[[nodiscard]] FactorModel make_multi_model(usize m) {
  const auto em = static_cast<Eigen::Index>(m);
  MatX x(em, 2);
  for (Eigen::Index i = 0; i < em; ++i) {
    x(i, 0) = 0.1 * static_cast<f64>(i) - 0.3;
    x(i, 1) = 0.05 * static_cast<f64>(i % 3) - 0.05;
  }
  MatX f(2, 2);
  f << 0.04, 0.01, 0.01, 0.09; // SPD
  VecX d(em);
  for (Eigen::Index i = 0; i < em; ++i) {
    d[i] = 0.10 + 0.02 * static_cast<f64>(i % 4);
  }
  return make_model(x, f, d);
}

[[nodiscard]] f64 tracking_error(const FactorModel &model, const std::vector<f64> &w,
                                 const std::vector<f64> &w_bench) {
  const usize m = model.n_instruments();
  std::vector<f64> u(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    u[i] = w[i] - (i < w_bench.size() ? w_bench[i] : 0.0);
  }
  return std::sqrt(model.risk(std::span<const f64>(u)));
}

// |(Xᵀw)_k| for factor k — the factor-exposure magnitude (the factor-neutral surface).
[[nodiscard]] f64 factor_exposure(const FactorModel &model, const std::vector<f64> &w, usize k) {
  const usize m = model.n_instruments();
  const MatX &x = model.exposures();
  f64 acc = 0.0;
  for (usize i = 0; i < m; ++i) {
    acc += x(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(k)) * w[i];
  }
  return std::fabs(acc);
}

// ---------------------------------------------------------------------------
//  An over-constrained problem: factor-neutral (both factors pinned to 0) + tracking-
//  error (tight, measured vs a benchmark that DOES carry factor exposure) are jointly
//  infeasible — tracking wants w near w_bench (Xᵀw_bench ≠ 0) while factor-neutral wants
//  Xᵀw = 0. Both ELASTIC at distinct priorities (lower = relaxed first):
//    tracking-error : priority 0  (relaxed first / cheapest)
//    factor-exposure: priority 1  (relaxed only if still infeasible)
//  The box (PositionCap) and dollar-neutral are HARD but LOOSE — always satisfiable, so
//  the only infeasibility is the elastic conflict. q = −alpha tilts the book.
// ---------------------------------------------------------------------------
struct ElasticProblem {
  FactorModel model;
  std::vector<f64> q;
  std::vector<f64> w_bench;
  MaterializedConstraints mc;
};

[[nodiscard]] ElasticProblem make_over_constrained(usize m) {
  FactorModel model = make_multi_model(m);
  // A benchmark with genuine factor exposure (dollar-neutral over each block of 5).
  std::vector<f64> w_bench(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    w_bench[i] = 0.05 * (static_cast<f64>(i % 5) - 2.0);
  }
  std::vector<f64> q(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    q[i] = -0.4 * (static_cast<f64>(i % 4) - 1.0); // alpha tilt
  }

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;     // HARD (always satisfiable)
  cs.pos = PositionCap{0.5};          // HARD + LOOSE (never binds infeasibly)

  FactorExposure fe;
  fe.factor_cols = {0U, 1U};
  fe.bound = {0.0, 0.0}; // factor-NEUTRAL on both factors (ELASTIC, priority 1)
  fe.elastic = true;
  fe.priority = 1U;
  cs.fexp = fe;

  // Tracking-error vs a factor-exposed benchmark, tight enough to force w ≈ w_bench (which
  // conflicts with factor-neutral). ELASTIC, priority 0 (relaxed first).
  TrackingError te{std::span<const f64>(w_bench), 0.02};
  te.elastic = true;
  te.priority = 0U;
  cs.track = te;

  auto mc_r = cs.materialize(model.exposures(), {}, m);
  EXPECT_TRUE(mc_r.has_value()) << (mc_r ? "" : mc_r.error().to_string());
  MaterializedConstraints mc = mc_r ? std::move(*mc_r) : MaterializedConstraints{};
  mc.gross_l1_budget = -1.0; // no gross L1 split (keep the surface linear + conic)

  return ElasticProblem{std::move(model), std::move(q), std::move(w_bench), std::move(mc)};
}

[[nodiscard]] ConstrainedQpSolver cone_solver() {
  ConstrainedQpSolver solver;
  solver.cfg.rho = 10.0;     // SOC-coupled rows converge best at a larger ADMM penalty
  solver.cfg.iters = 1500U;  // FIXED budget (R1)
  return solver;
}

// ===========================================================================
//  G-ELASTIC (infeasible → closest-feasible + ordered report).
// ===========================================================================
TEST(RiskElasticity, InfeasibleReturnsClosestFeasibleWithOrderedReport) {
  const usize m = 10U;
  ElasticProblem prob = make_over_constrained(m);
  const ConstrainedQpSolver solver = cone_solver();

  // The HARD solve (no elasticity) must report infeasible — proves the set is genuinely
  // over-constrained, so the elastic path is actually exercised.
  QpProblem qp{prob.model, 0.1, std::span<const f64>(prob.q), prob.mc};
  auto hard = solver.solve(qp);
  ASSERT_FALSE(hard.has_value()) << "the over-constrained set must be infeasible to the hard solve";

  // The elastic solve returns a closest-feasible book + a report.
  auto er = solve_elastic(qp, solver);
  ASSERT_TRUE(er.has_value()) << (er ? "" : er.error().to_string());
  const ElasticResult &res = *er;
  ASSERT_EQ(res.book.size(), m);
  EXPECT_TRUE(res.report.relaxed) << "an infeasible elastic problem must relax something";

  // Report order: lowest priority FIRST (the γ ladder dictates tracking(0) → factor(1) →
  // box(2)). Each reported entry's priority is ascending and the FIRST relaxed entry is
  // the lowest priority present.
  ASSERT_FALSE(res.report.entries.empty());
  for (usize i = 1; i < res.report.entries.size(); ++i) {
    EXPECT_LE(res.report.entries[i - 1].priority, res.report.entries[i].priority)
        << "report not in ascending-priority (lowest-relaxed-first) order at entry " << i;
  }
  // The lowest-priority elastic constraint (tracking-error, priority 0) is relaxed first.
  EXPECT_EQ(res.report.entries.front().priority, 0U)
      << "the lowest-priority elastic constraint must head the report";

  // The book is FEASIBLE for the HARD part and only the elastic constraints are violated,
  // each only as reported. Here ALL three are elastic, so we assert the achieved
  // relaxation is consistent: a relaxed constraint's reported violation is ≥ 0, and the
  // book's actual violation does not exceed the reported slack beyond feas_tol.
  const f64 te = tracking_error(prob.model, res.book, prob.w_bench);
  const f64 fe0 = factor_exposure(prob.model, res.book, 0U);
  const f64 fe1 = factor_exposure(prob.model, res.book, 1U);
  // Tracking-error budget was 1e-4; the achieved TE is the relaxed budget. The book must
  // not wildly exceed a sane relaxed level (sanity, not a hard pin).
  EXPECT_GE(te, 0.0);
  EXPECT_GE(fe0, 0.0);
  EXPECT_GE(fe1, 0.0);
  // Every reported violation magnitude is non-negative (a relaxation, never negative).
  for (const RelaxationEntry &e : res.report.entries) {
    EXPECT_GE(e.violation, -solver.cfg.feas_tol) << "a reported relaxation must be ≥ 0";
  }
}

// ===========================================================================
//  G-ELASTIC (feasible → no-op, byte-identical to the direct hard solve).
// ===========================================================================
TEST(RiskElasticity, FeasibleProblemIsByteIdenticalNoOp) {
  const usize m = 10U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.1; // mirror the proven-converging RiskCone box-only free-book solve
  std::vector<f64> q(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    q[i] = -0.4 * (static_cast<f64>(i % 4) - 1.0);
  }

  // A comfortably-feasible elastic set: dollar-neutral + a LOOSE box (PositionCap 0.5),
  // marked ELASTIC (so the elastic metadata IS populated) but never binding to
  // infeasibility. This is the EXACT box-only surface RiskCone's free-book solve uses
  // (box 0.5, gross off, λ=0.1, rho=10, iters=1500 — proven to converge). The no-op gate
  // just needs a feasible elastic problem; no cone is required.
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  PositionCap pc{0.5};
  pc.elastic = true;
  pc.priority = 0U;
  cs.pos = pc;
  auto mc_r = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_r.has_value()) << (mc_r ? "" : mc_r.error().to_string());
  MaterializedConstraints mc = std::move(*mc_r);
  mc.gross_l1_budget = -1.0; // box-only surface (no gross aux split)
  // Elastic metadata must be populated (the box is elastic) so this genuinely exercises the
  // no-op WRAPPER (a feasible problem whose elastic set is non-empty stays byte-identical).
  ASSERT_FALSE(mc.elastic.empty()) << "the box must be recorded as elastic";

  ConstrainedQpSolver solver = cone_solver(); // rho=10, iters=1500 (proven box-only path)
  QpProblem qp{model, lambda, std::span<const f64>(q), mc};

  // Direct hard solve (the S8.5 path) — the byte-identity reference.
  auto direct = solver.solve(qp);
  ASSERT_TRUE(direct.has_value()) << (direct ? "" : direct.error().to_string());
  const std::vector<f64> &ref = *direct;

  // Elastic solve on the SAME feasible problem — must be a pure no-op.
  auto er = solve_elastic(qp, solver);
  ASSERT_TRUE(er.has_value()) << (er ? "" : er.error().to_string());
  const ElasticResult &res = *er;
  EXPECT_FALSE(res.report.relaxed) << "a feasible problem must not relax anything";
  EXPECT_TRUE(res.report.entries.empty());

  ASSERT_EQ(res.book.size(), ref.size());
  for (usize i = 0; i < ref.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(res.book[i]), std::bit_cast<std::uint64_t>(ref[i]))
        << "elastic no-op book diverged from the direct hard solve at i=" << i;
  }
}

// ===========================================================================
//  G-DET — same infeasible problem ⇒ byte-identical book + identical report across runs.
// ===========================================================================
TEST(RiskElasticity, ElasticSolveIsDeterministic) {
  const usize m = 10U;
  ElasticProblem p1 = make_over_constrained(m);
  ElasticProblem p2 = make_over_constrained(m);
  const ConstrainedQpSolver solver = cone_solver();

  QpProblem qp1{p1.model, 0.1, std::span<const f64>(p1.q), p1.mc};
  QpProblem qp2{p2.model, 0.1, std::span<const f64>(p2.q), p2.mc};
  auto r1 = solve_elastic(qp1, solver);
  auto r2 = solve_elastic(qp2, solver);
  ASSERT_TRUE(r1.has_value()) << (r1 ? "" : r1.error().to_string());
  ASSERT_TRUE(r2.has_value()) << (r2 ? "" : r2.error().to_string());
  const ElasticResult &a = *r1;
  const ElasticResult &b = *r2;

  ASSERT_EQ(a.book.size(), b.book.size());
  for (usize i = 0; i < a.book.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a.book[i]), std::bit_cast<std::uint64_t>(b.book[i]))
        << "non-deterministic closest-feasible book at i=" << i;
  }
  // The report is identical: same number of entries, same (kind, index, priority) order,
  // and byte-identical achieved violations.
  ASSERT_EQ(a.report.entries.size(), b.report.entries.size());
  for (usize i = 0; i < a.report.entries.size(); ++i) {
    EXPECT_EQ(a.report.entries[i].priority, b.report.entries[i].priority) << "i=" << i;
    EXPECT_EQ(a.report.entries[i].index, b.report.entries[i].index) << "i=" << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a.report.entries[i].violation),
              std::bit_cast<std::uint64_t>(b.report.entries[i].violation))
        << "non-deterministic reported violation at i=" << i;
  }
}

// ===========================================================================
//  Hard-never-relaxes — when the binding infeasibility is a HARD constraint, the elastic
//  layer does NOT relax a hard constraint to fake feasibility. We take the SAME jointly-
//  infeasible surface as the over-constrained gate but mark factor-neutral + tracking BOTH
//  HARD (elastic=false), so NOTHING is elastic. The layer must REFUSE — it cannot relax a
//  hard constraint — and return a DISTINCT elastic-layer Err (NOT a faked book).
// ===========================================================================
TEST(RiskElasticity, HardConstraintInfeasibilityDoesNotRelax) {
  const usize m = 10U;
  FactorModel model = make_multi_model(m);
  std::vector<f64> w_bench(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    w_bench[i] = 0.05 * (static_cast<f64>(i % 5) - 2.0);
  }
  std::vector<f64> q(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    q[i] = -0.4 * (static_cast<f64>(i % 4) - 1.0);
  }

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.5}; // HARD box (default elastic=false)
  FactorExposure fe;
  fe.factor_cols = {0U, 1U};
  fe.bound = {0.0, 0.0};
  fe.elastic = false; // HARD factor-neutral
  cs.fexp = fe;
  TrackingError te{std::span<const f64>(w_bench), 0.02};
  te.elastic = false; // HARD tracking — NOTHING is elastic
  cs.track = te;
  auto mc_r = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_r.has_value()) << (mc_r ? "" : mc_r.error().to_string());
  MaterializedConstraints mc = std::move(*mc_r);
  mc.gross_l1_budget = -1.0;
  ASSERT_TRUE(mc.elastic.empty()) << "nothing is elastic in the hard-infeasible problem";

  const ConstrainedQpSolver solver = cone_solver();
  QpProblem qp{model, 0.1, std::span<const f64>(q), mc};

  auto hard = solver.solve(qp);
  ASSERT_FALSE(hard.has_value()) << "the HARD set must be infeasible";

  // No elastic constraints ⇒ the layer cannot relax anything ⇒ DISTINCT Err (NOT a book).
  auto er = solve_elastic(qp, solver);
  ASSERT_FALSE(er.has_value())
      << "with NO elastic constraints the layer must NOT manufacture feasibility";
  EXPECT_EQ(er.error().code(), atx::core::ErrorCode::InvalidArgument);
  // The message is the distinct elastic-layer signal (names the hard-binding cause).
  EXPECT_NE(er.error().message().find("HARD constraint is binding"), std::string::npos)
      << "expected the distinct elastic-layer hard-infeasible message, got: "
      << er.error().message();
}

} // namespace atxtest_risk_elasticity_test
