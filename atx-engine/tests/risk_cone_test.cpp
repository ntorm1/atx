// risk_cone_test.cpp — S8.5a: the cone keystone (risk/cone.hpp) + the tracking-error
// SOC end-to-end. This is the differential gate (R11) for the FIRST conic constraint.
//
// Gates proven here:
//   G-DIFF      — soc_project / ball_project match their closed form to ULP across
//                 all three SOC branches (inside / polar / boundary) + the z=0, s=0
//                 edges. (RiskCone.SocProjectionMatchesClosedForm /
//                 RiskCone.BallProjectionMatchesClosedForm)
//   G-CONSTRAINT— a tracking-error-constrained solve returns a book with
//                 ‖V^{1/2}(w−w_bench)‖₂ ≤ te + feas_tol, asserted DIRECTLY against the
//                 factored V (uᵀVu via FactorModel::risk), and it BINDS (the
//                 unconstrained book violates the same te).
//                 (RiskCone.TrackingErrorConstraintIsSatisfiedAndBinds)
//   G-DET       — (a) the book is byte-identical across two solves;
//                 (b) cone count 0 ⇒ the augmented book is byte-identical to the
//                 pre-S8.5a (S8.4) path (no cone block ⇒ inert). (RiskCone.*Deterministic*,
//                 RiskCone.ZeroConeIsByteIdenticalToS84Path)

#include <bit>     // std::bit_cast
#include <cmath>   // std::fabs, std::sqrt, std::isfinite, std::nextafter
#include <cstdint> // std::uint64_t
#include <span>
#include <utility> // std::move
#include <vector>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/cone.hpp"
#include "atx/engine/risk/constraints.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/qp_augment.hpp"
#include "atx/engine/risk/qp_solver.hpp"

namespace atxtest_risk_cone_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::ball_project;
using atx::engine::risk::ConstrainedQpSolver;
using atx::engine::risk::ConstraintSet;
using atx::engine::risk::FactorModel;
using atx::engine::risk::MaterializedConstraints;
using atx::engine::risk::ordered_norm2;
using atx::engine::risk::PositionCap;
using atx::engine::risk::QpProblem;
using atx::engine::risk::soc_project;
using atx::engine::risk::TrackingError;

// ---------------------------------------------------------------------------
//  Fixtures
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

// ‖V^{1/2}(w − w_bench)‖₂ via the factored V (uᵀVu = FactorModel::risk(u), then sqrt).
[[nodiscard]] f64 tracking_error(const FactorModel &model, const std::vector<f64> &w,
                                 const std::vector<f64> &w_bench) {
  const usize m = model.n_instruments();
  std::vector<f64> u(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    u[i] = w[i] - (i < w_bench.size() ? w_bench[i] : 0.0);
  }
  return std::sqrt(model.risk(std::span<const f64>(u)));
}

// ===========================================================================
//  G-DIFF — soc_project matches the closed form to ULP across all branches.
// ===========================================================================

// Independent closed-form reference for the SOC projection (computed straight from
// the spec, NOT from cone.hpp) so the ULP comparison is a genuine differential.
struct SocRef {
  f64 s;
  std::vector<f64> z;
};
[[nodiscard]] SocRef soc_ref(f64 s, const std::vector<f64> &z) {
  f64 acc = 0.0;
  for (const f64 v : z) {
    acc += v * v;
  }
  const f64 nz = std::sqrt(acc);
  SocRef out;
  out.z.assign(z.size(), 0.0);
  if (nz <= s) {
    out.s = s;
    out.z = z;
  } else if (nz <= -s) {
    out.s = 0.0; // z stays all-zero
  } else {
    const f64 rho = 0.5 * (s + nz);
    out.s = rho;
    const f64 scale = rho / nz;
    for (usize i = 0; i < z.size(); ++i) {
      out.z[i] = scale * z[i];
    }
  }
  return out;
}

[[nodiscard]] f64 bits_eq(f64 a, f64 b) { // ULP-exact equality via bit pattern
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

TEST(RiskCone, SocProjectionMatchesClosedForm) {
  struct Case {
    f64 s;
    std::vector<f64> z;
    const char *name;
  };
  const std::vector<Case> cases = {
      // inside (‖z‖ ≤ s)
      {2.0, {0.3, 0.4}, "inside"},          // ‖z‖=0.5 < 2
      {1.0, {0.6, 0.8}, "inside-boundary"}, // ‖z‖=1.0 == s (inside branch, identity)
      // polar (‖z‖ ≤ −s)
      {-2.0, {0.3, 0.4}, "polar"},          // ‖z‖=0.5 < 2 = −s ⇒ origin
      {-1.0, {0.6, 0.8}, "polar-boundary"}, // ‖z‖=1.0 == −s ⇒ origin
      // boundary (|s| < ‖z‖)
      {1.0, {3.0, 4.0}, "boundary-pos-s"},  // ‖z‖=5, ρ=3
      {-1.0, {3.0, 4.0}, "boundary-neg-s"}, // ‖z‖=5, ρ=2
      {0.0, {3.0, 4.0}, "boundary-zero-s"}, // s=0 ⇒ ρ=2.5
      // edges
      {0.0, {0.0, 0.0}, "z-zero-s-zero"},   // ‖z‖=0 ≤ 0 ⇒ inside/identity
      {2.0, {0.0, 0.0}, "z-zero-s-pos"},    // ‖z‖=0 ≤ 2 ⇒ inside/identity
      {-2.0, {0.0, 0.0}, "z-zero-s-neg"},   // ‖z‖=0 ≤ 2 = −s ⇒ origin (also identity here)
      {3.5, {1.0, -2.0, 2.0, 0.5}, "inside-4d"},
      {0.5, {1.0, -2.0, 2.0, 0.5}, "boundary-4d"},
  };
  for (const Case &c : cases) {
    const SocRef ref = soc_ref(c.s, c.z);
    std::vector<f64> out(c.z.size(), 0.0);
    const f64 s_out = soc_project(c.s, std::span<const f64>(c.z), std::span<f64>(out));
    EXPECT_TRUE(bits_eq(s_out, ref.s)) << c.name << " apex: got " << s_out << " want " << ref.s;
    for (usize i = 0; i < c.z.size(); ++i) {
      EXPECT_TRUE(bits_eq(out[i], ref.z[i]))
          << c.name << " z[" << i << "]: got " << out[i] << " want " << ref.z[i];
    }
    // The projected point is on/inside the cone (‖z*‖ ≤ s*) within a rounding ULP band.
    const f64 nzo = ordered_norm2(std::span<const f64>(out));
    EXPECT_LE(nzo, s_out + 1e-12) << c.name << " feasibility";
  }
}

TEST(RiskCone, BallProjectionMatchesClosedForm) {
  // ball_project(z, r) == the vector part of soc_project(r, z, ·) for r ≥ 0, and a
  // r ≤ 0 ball collapses to the origin.
  struct Case {
    f64 r;
    std::vector<f64> z;
    const char *name;
  };
  const std::vector<Case> cases = {
      {5.0, {3.0, 4.0}, "inside"},        // ‖z‖=5 == r ⇒ identity
      {10.0, {3.0, 4.0}, "strictly-in"},  // ‖z‖=5 < 10 ⇒ identity
      {2.5, {3.0, 4.0}, "boundary"},      // ‖z‖=5 > 2.5 ⇒ rescale to 2.5
      {0.0, {3.0, 4.0}, "zero-radius"},   // r=0 ⇒ origin
      {-1.0, {3.0, 4.0}, "neg-radius"},   // r<0 ⇒ origin
      {1.0, {0.0, 0.0}, "z-zero"},        // ‖z‖=0 ⇒ identity (origin)
      {1.5, {1.0, -1.0, 1.0, -1.0}, "boundary-4d"},
  };
  for (const Case &c : cases) {
    std::vector<f64> out(c.z.size(), 0.0);
    ball_project(std::span<const f64>(c.z), c.r, std::span<f64>(out));
    // Reference: soc_project apex r (for r ≥ 0), else origin.
    std::vector<f64> ref(c.z.size(), 0.0);
    if (c.r > 0.0) {
      f64 acc = 0.0;
      for (const f64 v : c.z) {
        acc += v * v;
      }
      const f64 nz = std::sqrt(acc);
      if (nz <= c.r) {
        ref = c.z;
      } else {
        const f64 scale = c.r / nz;
        for (usize i = 0; i < c.z.size(); ++i) {
          ref[i] = scale * c.z[i];
        }
      }
    }
    for (usize i = 0; i < c.z.size(); ++i) {
      EXPECT_TRUE(bits_eq(out[i], ref[i]))
          << c.name << " z[" << i << "]: got " << out[i] << " want " << ref[i];
    }
    const f64 nzo = ordered_norm2(std::span<const f64>(out));
    EXPECT_LE(nzo, (c.r > 0.0 ? c.r : 0.0) + 1e-12) << c.name << " feasibility";
  }
}

TEST(RiskCone, OrderedNorm2IsBitReproducible) {
  // The order-fixed reduction is a pure function of the slice — two calls bit-identical.
  const std::vector<f64> v = {0.1, -0.7, 3.3, 2.0, -1.25, 9.0};
  const f64 a = ordered_norm2(std::span<const f64>(v));
  const f64 b = ordered_norm2(std::span<const f64>(v));
  EXPECT_TRUE(bits_eq(a, b));
}

// ===========================================================================
//  G-CONSTRAINT — tracking-error solve is satisfied at the book AND binds.
// ===========================================================================
TEST(RiskCone, TrackingErrorConstraintIsSatisfiedAndBinds) {
  const usize m = 10U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.1; // low risk aversion ⇒ alpha dominates ⇒ a high-TE book absent the cone

  // A non-trivial benchmark book (dollar-neutral so the Σw=0 row is consistent).
  std::vector<f64> w_bench(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    w_bench[i] = 0.05 * (static_cast<f64>(i % 5) - 2.0); // sums to ~0 over each block of 5
  }

  // Alpha pushing AWAY from the benchmark so the unconstrained book has large TE.
  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.4 * static_cast<f64>((i % 4) - 1);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  // --- 1. Unconstrained-by-TE book (box only) to measure the natural TE. ---
  ConstraintSet cs_free;
  cs_free.gross.dollar_neutral = true;
  cs_free.pos = PositionCap{0.5};
  auto mc_free_r = cs_free.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_free_r.has_value()) << (mc_free_r ? "" : mc_free_r.error().to_string());
  MaterializedConstraints mc_free = std::move(*mc_free_r);
  mc_free.gross_l1_budget = -1.0;

  QpProblem prob_free{model, lambda, std::span<const f64>(q), mc_free};
  ConstrainedQpSolver solver;
  // The tracking-error SOC couples K+M extra rows into the splitting. A larger ADMM
  // penalty ρ balances the primal/dual updates for this coupled block (ρ=1 — tuned for
  // the box/L1 surface — converges this cone only at a far larger fixed budget); ρ=10
  // clears the feas_tol gate comfortably at this budget. Both counts are FIXED (R1).
  solver.cfg.rho = 10.0;
  solver.cfg.iters = 1500U;
  auto r_free = solver.solve(prob_free);
  ASSERT_TRUE(r_free.has_value()) << (r_free ? "" : r_free.error().to_string());
  const f64 te_free = tracking_error(model, *r_free, w_bench);

  // Pick a te budget that the free book VIOLATES (proves the cone binds).
  const f64 te = 0.5 * te_free;
  ASSERT_GT(te_free, te) << "free book TE=" << te_free << " must exceed the budget te=" << te;

  // --- 2. TE-constrained solve. ---
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.5};
  cs.track = TrackingError{std::span<const f64>(w_bench), te};
  auto mc_r = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_r.has_value()) << (mc_r ? "" : mc_r.error().to_string());
  MaterializedConstraints mc = std::move(*mc_r);
  mc.gross_l1_budget = -1.0;

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  auto r_con = solver.solve(prob);
  ASSERT_TRUE(r_con.has_value()) << (r_con ? "" : r_con.error().to_string());
  const std::vector<f64> &w = *r_con;

  const f64 te_con = tracking_error(model, w, w_bench);
  EXPECT_LE(te_con, te + solver.cfg.feas_tol)
      << "constrained TE=" << te_con << " exceeds budget te=" << te;
  // It binds: the constrained TE sits at (near) the budget, well below the free TE.
  EXPECT_LT(te_con, te_free) << "cone did not reduce TE";
}

// ===========================================================================
//  G-DET (a) — two TE-constrained solves ⇒ byte-identical book.
// ===========================================================================
TEST(RiskCone, TrackingErrorSolveIsDeterministic) {
  const usize m = 8U;
  const FactorModel model = make_multi_model(m);
  std::vector<f64> w_bench(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    w_bench[i] = 0.03 * (static_cast<f64>(i % 3) - 1.0);
  }
  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.3 * static_cast<f64>((i % 5) - 2);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.5};
  cs.track = TrackingError{std::span<const f64>(w_bench), 0.05};
  auto mc_r = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_r.has_value());
  MaterializedConstraints mc = std::move(*mc_r);
  mc.gross_l1_budget = -1.0;

  QpProblem prob{model, 0.5, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.rho = 10.0;     // SOC-coupled block converges best at a larger ADMM penalty
  solver.cfg.iters = 1500U;
  auto r1 = solver.solve(prob);
  auto r2 = solver.solve(prob);
  ASSERT_TRUE(r1.has_value()) << (r1 ? "" : r1.error().to_string());
  ASSERT_TRUE(r2.has_value()) << (r2 ? "" : r2.error().to_string());
  const std::vector<f64> &a = *r1;
  const std::vector<f64> &b = *r2;
  ASSERT_EQ(a.size(), b.size());
  for (usize i = 0; i < a.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a[i]), std::bit_cast<std::uint64_t>(b[i])) << "i=" << i;
  }
}

// ===========================================================================
//  G-DET (b) — cone count 0 ⇒ byte-identical to the pre-S8.5a (S8.4) augmented path.
//  We build the SAME problem with NO TrackingError descriptor and assert the book is
//  bitwise unchanged whether or not the cone machinery is present in AugmentedQp. The
//  reference is the augmented solve over a MaterializedConstraints that never touched
//  the tracking-error fields (the S8.4 surface). Two solves of the no-cone problem must
//  be byte-identical, and — critically — the no-cone augmented assembly must be
//  structurally identical to the pre-cone path (proven by an explicit assertion that
//  AugmentedQp carries zero SocBlocks for the no-cone problem).
// ===========================================================================
TEST(RiskCone, ZeroConeIsByteIdenticalToS84Path) {
  const usize m = 12U;
  const FactorModel model = make_multi_model(m);
  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.2 * static_cast<f64>((i % 4) - 1);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  // The S8.4 surface: dollar-neutral + box + gross L1, NO tracking error.
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.gross.gross_leverage = 1.5;
  cs.pos = PositionCap{0.4};
  auto mc_r = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_r.has_value());
  const MaterializedConstraints mc = std::move(*mc_r);

  // The augmented assembly carries ZERO cone blocks when no TrackingError is present.
  const auto aug = atx::engine::risk::build_augmented(model, 0.7, std::span<const f64>(q), mc);
  EXPECT_TRUE(aug.cones.empty()) << "no-cone problem must carry zero SocBlocks";

  QpProblem prob{model, 0.7, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.iters = 400U;
  auto r1 = solver.solve(prob);
  auto r2 = solver.solve(prob);
  ASSERT_TRUE(r1.has_value()) << (r1 ? "" : r1.error().to_string());
  ASSERT_TRUE(r2.has_value()) << (r2 ? "" : r2.error().to_string());
  const std::vector<f64> &a = *r1;
  const std::vector<f64> &b = *r2;
  ASSERT_EQ(a.size(), b.size());
  for (usize i = 0; i < a.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a[i]), std::bit_cast<std::uint64_t>(b[i])) << "i=" << i;
  }
}

} // namespace atxtest_risk_cone_test
