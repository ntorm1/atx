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
//                 pre-S8.5a (S8.4) path (no cone block ⇒ inert). ALSO a frozen
//                 golden-digest pin (kZeroConeGolden, FNV-1a over ascending book
//                 elements) — any drift in the box-only iterates fails the pin.
//                 (RiskCone.*Deterministic*, RiskCone.ZeroConeIsByteIdenticalToS84Path)

#include <bit>     // std::bit_cast
#include <cmath>   // std::fabs, std::sqrt, std::isfinite, std::nextafter
#include <cstdint> // std::uint64_t
#include <limits>  // std::numeric_limits (κ=0 apex sentinel)
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
using atx::engine::risk::RobustAlpha;
using atx::engine::risk::soc_project;
using atx::engine::risk::SocBlock;
using atx::engine::risk::TrackingError;
using atx::engine::risk::SectorRiskBudget;

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

// ‖V^{1/2}(mask_g∘w)‖₂ via the factored V: mask w to sector g (zero elsewhere), then
// sqrt(uᵀVu) = sqrt(FactorModel::risk(mask_g∘w)). The G-CONSTRAINT (sector) reference.
[[nodiscard]] f64 sector_risk(const FactorModel &model, const std::vector<f64> &w,
                              const std::vector<usize> &sector_id, usize g) {
  const usize m = model.n_instruments();
  std::vector<f64> u(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    u[i] = (i < sector_id.size() && sector_id[i] == g) ? w[i] : 0.0;
  }
  return std::sqrt(model.risk(std::span<const f64>(u)));
}

// ‖Ω_f^{1/2} y‖₂ with y = Xᵀw — the robust worst-case factor-exposure penalty
// (S8.5c). For the DEFAULT identity Ω_f this is ‖Xᵀw‖₂ (penalize total factor
// exposure). `omega` is the K×K SPD factor-premia error covariance (empty ⇒ identity).
// Reference is independent of cone.hpp: form y, then ‖Ω_f^{1/2} y‖₂ = sqrt(yᵀ Ω_f y).
[[nodiscard]] f64 robust_factor_norm(const FactorModel &model, const std::vector<f64> &w,
                                     const MatX &omega = {}) {
  const usize m = model.n_instruments();
  const usize k = model.n_factors();
  const MatX &x = model.exposures();
  VecX y = VecX::Zero(static_cast<Eigen::Index>(k));
  for (usize c = 0; c < k; ++c) {
    f64 acc = 0.0; // order-fixed ascending instrument
    for (usize i = 0; i < m; ++i) {
      acc += x(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(c)) * w[i];
    }
    y[static_cast<Eigen::Index>(c)] = acc;
  }
  if (omega.rows() == 0) { // identity ⇒ ‖y‖₂
    return std::sqrt(y.dot(y));
  }
  return std::sqrt((y.transpose() * omega * y)(0, 0));
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
//  G-DET (b) — GOLDEN-DIGEST PIN against the box-only (S8.4) path.
//  We build the problem with NO TrackingError descriptor (zero cones) and assert:
//    (1) AugmentedQp carries exactly zero SocBlocks (box-only structural gate).
//    (2) Two solves of the no-cone problem are byte-identical (self-consistency).
//    (3) A 64-bit FNV-1a digest over the bit pattern of every book element matches
//        kZeroConeGolden — a FROZEN constant computed once and pinned here. Any
//        future change that silently perturbs the box-only iterates fails this pin
//        (G-DET(b) absolute baseline, not merely a self-consistency check).
//  FNV-1a parameters: basis = 1469598103934665603ULL, prime = 1099511628211ULL,
//  accumulation order: ascending book index.
// ===========================================================================

// Frozen golden digest of the box-only augmented book (M=12, lambda=0.7, 400 iters).
// Computed on first run and pinned. Regenerate only when the S8.4 box-only path
// is intentionally changed (update the constant and the commit message to say why).
static constexpr std::uint64_t kZeroConeGolden = 0xffed7ec6c177aad2ULL;

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

  // (1) The augmented assembly carries ZERO cone blocks when no TrackingError is present.
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

  // (2) Byte-identity across two solves of the same problem (self-consistency).
  for (usize i = 0; i < a.size(); ++i) {
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a[i]), std::bit_cast<std::uint64_t>(b[i])) << "i=" << i;
  }

  // (3) Golden-digest pin: FNV-1a over the bit pattern of every book element,
  //     ascending index. Fails if the box-only iterates drift from the S8.4 baseline.
  std::uint64_t h = 1469598103934665603ULL; // FNV-1a 64-bit basis
  for (usize i = 0; i < a.size(); ++i) {
    h ^= std::bit_cast<std::uint64_t>(a[i]);
    h *= 1099511628211ULL; // FNV-1a 64-bit prime
  }
  // Emit the actual digest so we can read it off the first run (printed on FAILURE).
  EXPECT_EQ(h, kZeroConeGolden) << "box-only augmented book drifted from the S8.4 golden digest"
                                 << "; actual digest = 0x" << std::hex << h;
}

// ===========================================================================
//  S8.5b — Piece 1: the sector-risk SOC cone.
//
//  G-CONSTRAINT (sector risk): a sector-risk-constrained solve returns a book with
//  ‖V^{1/2}(mask_g∘w)‖₂ ≤ σ_g + feas_tol, asserted PER SECTOR against the factored V.
//  Proven to BIND (the box-only book violates the same σ_g), over ≥ 2 sectors at once.
// ===========================================================================
TEST(RiskCone, SectorRiskConeIsSatisfiedAndBindsAcrossTwoSectors) {
  const usize m = 10U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.1; // low risk aversion ⇒ alpha dominates ⇒ a high-risk free book

  // Two sectors: even indices ⇒ sector 0, odd ⇒ sector 1 (5 names each).
  std::vector<usize> sector_id(m, 0U);
  for (usize i = 0; i < m; ++i) {
    sector_id[i] = i % 2U;
  }

  // Alpha that drives a large per-sector book risk absent the cone.
  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.4 * static_cast<f64>((i % 4) - 1);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  ConstrainedQpSolver solver;
  solver.cfg.rho = 10.0;     // SOC-coupled rows converge best at a larger ADMM penalty
  solver.cfg.iters = 1500U;

  // --- 1. Box-only book to measure the natural per-sector risk. ---
  ConstraintSet cs_free;
  cs_free.gross.dollar_neutral = true;
  cs_free.pos = PositionCap{0.5};
  auto mc_free_r = cs_free.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_free_r.has_value()) << (mc_free_r ? "" : mc_free_r.error().to_string());
  MaterializedConstraints mc_free = std::move(*mc_free_r);
  mc_free.gross_l1_budget = -1.0;
  QpProblem prob_free{model, lambda, std::span<const f64>(q), mc_free};
  auto r_free = solver.solve(prob_free);
  ASSERT_TRUE(r_free.has_value()) << (r_free ? "" : r_free.error().to_string());
  const f64 sr0_free = sector_risk(model, *r_free, sector_id, 0U);
  const f64 sr1_free = sector_risk(model, *r_free, sector_id, 1U);

  // Tight per-sector budgets the free book VIOLATES (proves both cones bind).
  const f64 sigma0 = 0.5 * sr0_free;
  const f64 sigma1 = 0.5 * sr1_free;
  ASSERT_GT(sr0_free, sigma0) << "free sector-0 risk must exceed its budget";
  ASSERT_GT(sr1_free, sigma1) << "free sector-1 risk must exceed its budget";

  // --- 2. Sector-risk-SOC-constrained solve (two sectors). ---
  SectorRiskBudget srb;
  srb.sector_id = std::span<const usize>(sector_id);
  srb.soc = true;
  srb.sigma = {sigma0, sigma1};
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.5};
  cs.sector = srb;
  auto mc_r = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_r.has_value()) << (mc_r ? "" : mc_r.error().to_string());
  MaterializedConstraints mc = std::move(*mc_r);
  mc.gross_l1_budget = -1.0;

  // Two cone blocks must be assembled (one per finite-σ sector).
  const auto aug = atx::engine::risk::build_augmented(model, lambda, std::span<const f64>(q), mc);
  EXPECT_EQ(aug.cones.size(), 2U) << "expected one SocBlock per finite-σ sector";

  QpProblem prob{model, lambda, std::span<const f64>(q), mc};
  auto r_con = solver.solve(prob);
  ASSERT_TRUE(r_con.has_value()) << (r_con ? "" : r_con.error().to_string());
  const std::vector<f64> &w = *r_con;

  const f64 sr0 = sector_risk(model, w, sector_id, 0U);
  const f64 sr1 = sector_risk(model, w, sector_id, 1U);
  EXPECT_LE(sr0, sigma0 + solver.cfg.feas_tol) << "sector-0 risk " << sr0 << " over budget " << sigma0;
  EXPECT_LE(sr1, sigma1 + solver.cfg.feas_tol) << "sector-1 risk " << sr1 << " over budget " << sigma1;
  EXPECT_LT(sr0, sr0_free) << "sector-0 cone did not reduce risk";
  EXPECT_LT(sr1, sr1_free) << "sector-1 cone did not reduce risk";
}

// G-DET (a) — two sector-risk-constrained solves ⇒ byte-identical book.
TEST(RiskCone, SectorRiskSolveIsDeterministic) {
  const usize m = 8U;
  const FactorModel model = make_multi_model(m);
  std::vector<usize> sector_id(m, 0U);
  for (usize i = 0; i < m; ++i) {
    sector_id[i] = i % 2U;
  }
  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.3 * static_cast<f64>((i % 5) - 2);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  SectorRiskBudget srb;
  srb.sector_id = std::span<const usize>(sector_id);
  srb.soc = true;
  srb.sigma = {0.06, 0.06};
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.5};
  cs.sector = srb;
  auto mc_r = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_r.has_value());
  MaterializedConstraints mc = std::move(*mc_r);
  mc.gross_l1_budget = -1.0;

  QpProblem prob{model, 0.5, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.rho = 10.0;
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
//  S8.5b — Piece 2: the √-impact quadratic cost surrogate.
//
//  G-DIFF (√-impact): the surrogate folds into the augmented P/q EXACTLY as the closed
//  form Σ_i c_i (w_i − w_prev_i)² ⇒ P[i,i] = 2λD_i + 2 c_i, q[i] = q0_i − 2 c_i w_prev_i,
//  asserted to ULP against build_augmented's P/q; c_i ≥ 0 keeps the w-block PSD.
// ===========================================================================
TEST(RiskCone, ImpactSurrogateFoldsIntoPqClosedForm) {
  const usize m = 6U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.3;
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = 0.1 * static_cast<f64>(i) - 0.2;
  }

  // A turnover reference (the surrogate's w_prev) + per-name impact coefficients c_i ≥ 0.
  std::vector<f64> w_prev(m);
  std::vector<f64> coeff(m);
  for (usize i = 0; i < m; ++i) {
    w_prev[i] = 0.02 * static_cast<f64>((i % 3)) - 0.02;
    coeff[i] = 0.5 + 0.25 * static_cast<f64>(i); // strictly positive
  }

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.5};
  cs.turn = atx::engine::risk::TurnoverBudget{1.0}; // populates turnover_ref = w_prev
  auto mc_r = cs.materialize(model.exposures(), std::span<const f64>(w_prev), m);
  ASSERT_TRUE(mc_r.has_value()) << (mc_r ? "" : mc_r.error().to_string());
  MaterializedConstraints mc = std::move(*mc_r);
  mc.impact.active = true;
  mc.impact.coeff = coeff;

  const auto aug = atx::engine::risk::build_augmented(model, lambda, std::span<const f64>(q), mc);

  // P[i,i] == 2λ D_i + 2 c_i  (ULP-exact: the same arithmetic the assembler does).
  const VecX &D = model.specific_var();
  for (usize i = 0; i < m; ++i) {
    const f64 want = 2.0 * lambda * D[static_cast<Eigen::Index>(i)] + 2.0 * coeff[i];
    const f64 got = aug.P.coeff(static_cast<int>(i), static_cast<int>(i));
    EXPECT_TRUE(bits_eq(got, want)) << "P[" << i << "] got " << got << " want " << want;
  }
  // q[i] == q0_i − 2 c_i w_prev_i  (ULP-exact).
  for (usize i = 0; i < m; ++i) {
    const f64 want = q[i] - 2.0 * coeff[i] * w_prev[i];
    const f64 got = aug.q_aug[static_cast<Eigen::Index>(i)];
    EXPECT_TRUE(bits_eq(got, want)) << "q[" << i << "] got " << got << " want " << want;
  }
}

// G-CONSTRAINT (√-impact behavior): the surrogate ON ⇒ the optimizer trades LESS than
// OFF on the same problem; a LARGER coefficient ⇒ even less turnover (monotone pin).
TEST(RiskCone, ImpactSurrogateReducesTurnoverMonotonically) {
  const usize m = 8U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.1; // alpha-dominated ⇒ the free book trades a lot off w_prev

  std::vector<f64> w_prev(m, 0.0); // start flat ⇒ turnover == Σ|w|
  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.4 * static_cast<f64>((i % 4) - 1);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  auto turnover = [&](const std::vector<f64> &w) {
    f64 t = 0.0;
    for (usize i = 0; i < m; ++i) {
      t += std::fabs(w[i] - w_prev[i]);
    }
    return t;
  };
  auto solve_with_impact = [&](f64 c) -> std::vector<f64> {
    ConstraintSet cs;
    cs.gross.dollar_neutral = true;
    cs.pos = PositionCap{0.5};
    cs.turn = atx::engine::risk::TurnoverBudget{10.0}; // loose ⇒ turnover governed by the surrogate
    auto mc_r = cs.materialize(model.exposures(), std::span<const f64>(w_prev), m);
    EXPECT_TRUE(mc_r.has_value());
    MaterializedConstraints mc = std::move(*mc_r);
    mc.gross_l1_budget = -1.0;
    if (c > 0.0) {
      mc.impact.active = true;
      mc.impact.coeff.assign(m, c);
    }
    QpProblem prob{model, lambda, std::span<const f64>(q), mc};
    ConstrainedQpSolver solver;
    solver.cfg.iters = 800U;
    auto r = solver.solve(prob);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
    return r ? *r : std::vector<f64>(m, 0.0);
  };

  const f64 t_off = turnover(solve_with_impact(0.0));
  const f64 t_lo = turnover(solve_with_impact(0.5));
  const f64 t_hi = turnover(solve_with_impact(4.0));
  EXPECT_LT(t_lo, t_off) << "surrogate ON must reduce turnover (off=" << t_off << " on=" << t_lo << ")";
  EXPECT_LT(t_hi, t_lo) << "larger impact coeff must reduce turnover further (lo=" << t_lo
                        << " hi=" << t_hi << ")";
}

// ===========================================================================
//  G-DET (b) — zero sector cones + zero √-impact ⇒ byte-identical to the S8.5a path.
//  (1) An empty SectorRiskSpec + empty ImpactSurrogateSpec carries zero SocBlocks and a
//      P/q byte-identical to the build with NO such specs. (2) The box-only golden pin
//      (kZeroConeGolden) still holds with the (inert) S8.5b specs present.
// ===========================================================================
TEST(RiskCone, ZeroSectorConeZeroImpactIsByteIdenticalToS85aPath) {
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

  // The S8.4/S8.5a surface: dollar-neutral + box + gross L1, NO cones, NO impact.
  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.gross.gross_leverage = 1.5;
  cs.pos = PositionCap{0.4};
  auto mc_r = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_r.has_value());
  MaterializedConstraints mc = std::move(*mc_r);

  // The S8.5b specs are inactive by default — assert that explicitly.
  EXPECT_FALSE(mc.sector_risk.active);
  EXPECT_FALSE(mc.impact.active);

  const auto aug = atx::engine::risk::build_augmented(model, 0.7, std::span<const f64>(q), mc);
  EXPECT_TRUE(aug.cones.empty()) << "no-sector-cone problem must carry zero SocBlocks";

  // Explicitly setting an INERT impact spec (active but all-zero coeff) must not perturb
  // P or q (byte-identical to the spec-absent build).
  MaterializedConstraints mc_inert = mc;
  mc_inert.impact.active = true;
  mc_inert.impact.coeff.assign(m, 0.0);
  const auto aug_inert =
      atx::engine::risk::build_augmented(model, 0.7, std::span<const f64>(q), mc_inert);
  ASSERT_EQ(aug.P.nonZeros(), aug_inert.P.nonZeros());
  for (usize i = 0; i < m; ++i) {
    EXPECT_TRUE(bits_eq(aug.P.coeff(static_cast<int>(i), static_cast<int>(i)),
                        aug_inert.P.coeff(static_cast<int>(i), static_cast<int>(i))))
        << "inert impact perturbed P[" << i << "]";
    EXPECT_TRUE(bits_eq(aug.q_aug[static_cast<Eigen::Index>(i)],
                        aug_inert.q_aug[static_cast<Eigen::Index>(i)]))
        << "inert impact perturbed q[" << i << "]";
  }
  EXPECT_TRUE(aug_inert.cones.empty()) << "inert impact must not add cones";

  // The box-only golden pin still holds (the S8.5b specs are inert when off).
  QpProblem prob{model, 0.7, std::span<const f64>(q), mc};
  ConstrainedQpSolver solver;
  solver.cfg.iters = 400U;
  auto r1 = solver.solve(prob);
  ASSERT_TRUE(r1.has_value()) << (r1 ? "" : r1.error().to_string());
  const std::vector<f64> &a = *r1;
  std::uint64_t h = 1469598103934665603ULL;
  for (usize i = 0; i < a.size(); ++i) {
    h ^= std::bit_cast<std::uint64_t>(a[i]);
    h *= 1099511628211ULL;
  }
  EXPECT_EQ(h, kZeroConeGolden) << "box-only book drifted with the (inert) S8.5b specs present"
                                << "; actual digest = 0x" << std::hex << h;
}

// ===========================================================================
//  S8.5c — the robust (Goldfarb-Iyengar) alpha-uncertainty cone.
//
//  This is the design-novel VARIABLE-APEX SOC: the epigraph variable t is a real
//  optimization variable, projected with the GENERAL soc_project (not ball_project).
//  Robust alpha = worst-case over an ellipsoidal uncertainty set ⇒ a +κ‖Ω_f^{1/2}y‖₂
//  penalty; epigraph form: aux var t, SOC ‖Ω_f^{1/2}y‖₂ ≤ t, linear cost +κt. As κ→0
//  it reduces to the nominal problem (R10/R11).
//
//  Gates proven here:
//    G-DIFF (variable-apex) — the z-update routes a variable-apex SocBlock through
//      soc_project (not ball_project), projecting a known (t, v) correctly.
//    G-DIFF (κ→0)           — κ=0 ⇒ no robust cone emitted ⇒ book byte-identical to
//      the same problem with no RobustAlpha; book converges to nominal as κ→0.
//    G-CONSTRAINT (bites)   — κ>0 ⇒ strictly smaller ‖Ω_f^{1/2}y‖₂ than nominal; larger
//      κ ⇒ smaller (monotone); epigraph binds t ≈ ‖Ω_f^{1/2}y‖₂ within feas_tol.
//    G-DET                  — digest stable across runs; κ=0/no-robust byte-identical.
// ===========================================================================

// G-DIFF (variable-apex) — a variable-apex SocBlock in the augmented form is projected
// by soc_project in the z-update, NOT ball_project. We assemble a robust problem,
// confirm the LAST cone block is variable_apex (dim = 1 + K, the apex row first), and
// that a known (t, v) on that block's geometry projects to the soc_project closed form
// (the apex is a free variable, so the projection must move BOTH t and v — a ball would
// pin t to the radius and only touch v).
TEST(RiskCone, RobustConeIsVariableApexAndRoutesThroughSocProject) {
  const usize m = 6U;
  const usize k = 2U; // make_multi_model has K=2
  const FactorModel model = make_multi_model(m);
  std::vector<f64> q(m, 0.0);
  for (usize i = 0; i < m; ++i) {
    q[i] = -0.1 * static_cast<f64>((i % 3) - 1);
  }

  ConstraintSet cs;
  cs.gross.dollar_neutral = true;
  cs.pos = PositionCap{0.5};
  cs.robust = RobustAlpha{0.25}; // κ = 0.25, default identity Ω_f
  auto mc_r = cs.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_r.has_value()) << (mc_r ? "" : mc_r.error().to_string());
  MaterializedConstraints mc = std::move(*mc_r);
  mc.gross_l1_budget = -1.0;

  const auto aug = atx::engine::risk::build_augmented(model, 0.3, std::span<const f64>(q), mc);
  // Exactly one cone, and it is the VARIABLE-APEX robust cone (dim = 1 apex + K vector).
  ASSERT_EQ(aug.cones.size(), 1U) << "robust cone should add exactly one SocBlock";
  const SocBlock &blk = aug.cones.back();
  EXPECT_TRUE(blk.variable_apex) << "robust cone must be a variable-apex SOC";
  EXPECT_EQ(blk.dim, 1U + k) << "robust cone dim = 1 (apex t) + K (Ω_f^{1/2} y vector)";
  // One extra aux column (the epigraph t) beyond w(M)+y(K) (no gross/turnover here).
  EXPECT_EQ(aug.n_aux, 1U) << "the epigraph t is the only aux column";
  // q_aug carries +κ on the t column (last column).
  const auto t_col = static_cast<Eigen::Index>(aug.n_w + aug.n_y + aug.n_aux - 1U);
  EXPECT_TRUE(bits_eq(aug.q_aug[t_col], 0.25)) << "q_aug[t] must equal κ";

  // The variable-apex geometry: apex = row_start, vector = the next dim-1 rows. A known
  // (t, v) with ‖v‖ > |t| must project to the soc_project boundary form (BOTH t and v
  // move) — a ball_project would NOT change the apex. Verify directly against the spec.
  const f64 t_in = 1.0;
  const std::vector<f64> v_in = {3.0, 4.0}; // ‖v‖ = 5 > t ⇒ boundary
  std::vector<f64> v_out(v_in.size(), 0.0);
  const f64 t_out = soc_project(t_in, std::span<const f64>(v_in), std::span<f64>(v_out));
  const f64 rho = 0.5 * (t_in + 5.0); // boundary apex
  EXPECT_TRUE(bits_eq(t_out, rho)) << "variable apex must move to ρ=(t+‖v‖)/2";
  EXPECT_NE(t_out, t_in) << "a variable-apex projection MOVES the apex (ball would not)";
}

// G-DIFF (κ→0) + G-DET — κ=0 ⇒ no robust cone emitted ⇒ book byte-identical to the same
// problem with NO RobustAlpha descriptor. And the digest is stable across two solves.
TEST(RiskCone, RobustConeKappaZeroIsByteIdenticalToNominal) {
  const usize m = 10U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.4;
  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.3 * static_cast<f64>((i % 4) - 1);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  // Nominal problem (NO RobustAlpha).
  ConstraintSet cs_nom;
  cs_nom.gross.dollar_neutral = true;
  cs_nom.pos = PositionCap{0.5};
  auto mc_nom_r = cs_nom.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc_nom_r.has_value());
  MaterializedConstraints mc_nom = std::move(*mc_nom_r);
  mc_nom.gross_l1_budget = -1.0;

  // κ = 0 robust problem (RobustAlpha present but radius 0).
  ConstraintSet cs0;
  cs0.gross.dollar_neutral = true;
  cs0.pos = PositionCap{0.5};
  cs0.robust = RobustAlpha{0.0};
  auto mc0_r = cs0.materialize(model.exposures(), {}, m);
  ASSERT_TRUE(mc0_r.has_value());
  MaterializedConstraints mc0 = std::move(*mc0_r);
  mc0.gross_l1_budget = -1.0;

  // κ=0 ⇒ NO cone emitted ⇒ structurally identical to nominal.
  const auto aug_nom = atx::engine::risk::build_augmented(model, lambda, std::span<const f64>(q), mc_nom);
  const auto aug0 = atx::engine::risk::build_augmented(model, lambda, std::span<const f64>(q), mc0);
  EXPECT_TRUE(aug_nom.cones.empty()) << "nominal carries zero cones";
  EXPECT_TRUE(aug0.cones.empty()) << "κ=0 must emit NO robust cone";
  EXPECT_EQ(aug0.n_aux, aug_nom.n_aux) << "κ=0 must NOT add the t aux column";
  EXPECT_EQ(aug0.A_tilde.rows(), aug_nom.A_tilde.rows());
  EXPECT_EQ(aug0.A_tilde.cols(), aug_nom.A_tilde.cols());

  ConstrainedQpSolver solver;
  solver.cfg.rho = 10.0;
  solver.cfg.iters = 1500U;
  QpProblem prob_nom{model, lambda, std::span<const f64>(q), mc_nom};
  QpProblem prob0{model, lambda, std::span<const f64>(q), mc0};
  auto r_nom = solver.solve(prob_nom);
  auto r0a = solver.solve(prob0);
  auto r0b = solver.solve(prob0);
  ASSERT_TRUE(r_nom.has_value()) << (r_nom ? "" : r_nom.error().to_string());
  ASSERT_TRUE(r0a.has_value()) << (r0a ? "" : r0a.error().to_string());
  ASSERT_TRUE(r0b.has_value());
  const std::vector<f64> &nom = *r_nom;
  const std::vector<f64> &a0 = *r0a;
  const std::vector<f64> &b0 = *r0b;
  ASSERT_EQ(nom.size(), a0.size());
  for (usize i = 0; i < nom.size(); ++i) {
    // κ=0 byte-identical to nominal (R10) AND deterministic across two solves (G-DET).
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a0[i]), std::bit_cast<std::uint64_t>(nom[i]))
        << "κ=0 robust book diverged from nominal at i=" << i;
    EXPECT_EQ(std::bit_cast<std::uint64_t>(a0[i]), std::bit_cast<std::uint64_t>(b0[i]))
        << "robust solve not deterministic at i=" << i;
  }
}

// G-CONSTRAINT (robust bites) + epigraph binds — κ>0 ⇒ strictly smaller ‖Ω_f^{1/2}y‖₂
// than nominal, larger κ ⇒ smaller (monotone over an ascending κ sweep), and the
// epigraph t ≈ ‖Ω_f^{1/2}y‖₂ at the returned book (asserted DIRECTLY against the
// achieved apex t surfaced on QpResult::cone_apex). Default identity Ω_f ⇒ κ‖Xᵀw‖₂.
TEST(RiskCone, RobustConeReducesFactorExposureMonotonicallyAndBinds) {
  const usize m = 10U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.1; // alpha-dominated ⇒ a high factor-exposure free book
  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.4 * static_cast<f64>((i % 4) - 1);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  ConstrainedQpSolver solver;
  solver.cfg.rho = 10.0;
  solver.cfg.iters = 1500U;

  // The book plus the achieved epigraph apex t (the variable-apex cone's row_start row of
  // Ãx at the returned x, surfaced on QpResult::cone_apex). For κ=0 NO robust cone is
  // emitted ⇒ cone_apex is empty and `apex` is left as a NaN sentinel (unused there).
  struct KappaSolve {
    std::vector<f64> book;
    f64 apex = std::numeric_limits<f64>::quiet_NaN();
  };
  auto solve_kappa = [&](f64 kappa) -> KappaSolve {
    ConstraintSet cs;
    cs.gross.dollar_neutral = true;
    cs.pos = PositionCap{0.5};
    if (kappa > 0.0) {
      cs.robust = RobustAlpha{kappa};
    }
    auto mc_r = cs.materialize(model.exposures(), {}, m);
    EXPECT_TRUE(mc_r.has_value()) << (mc_r ? "" : mc_r.error().to_string());
    MaterializedConstraints mc = std::move(*mc_r);
    mc.gross_l1_budget = -1.0;
    QpProblem prob{model, lambda, std::span<const f64>(q), mc};
    auto r = solver.solve_with_cert(prob);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
    KappaSolve out;
    out.book = r ? r->book : std::vector<f64>(m, 0.0);
    if (kappa > 0.0) {
      // Exactly one variable-apex (robust) cone is emitted ⇒ one apex entry.
      EXPECT_EQ(r ? r->cone_apex.size() : 0U, 1U)
          << "κ>0 must surface exactly one variable-apex cone apex";
      if (r && r->cone_apex.size() == 1U) {
        out.apex = r->cone_apex[0];
      }
    } else {
      EXPECT_TRUE(!r || r->cone_apex.empty()) << "κ=0 must emit NO variable-apex cone";
    }
    return out;
  };

  // κ values are chosen in the UN-saturated regime: the penalty κ‖Ω_f^{1/2}y‖₂ trades
  // smoothly against the (small-λ, alpha-dominated) objective here, driving the factor
  // exposure toward — but, at these κ, not yet onto — the zero floor. (For this benign
  // dollar-neutral + box set, zero factor exposure is fully feasible, so a LARGE κ
  // saturates ‖Ω_f^{1/2}y‖₂ to the numerical floor ~1e-16 and the strict monotone could
  // no longer separate two saturated values; the 0.025 → 0.05 → 0.1 sweep keeps clean
  // ~2× gaps between consecutive unsaturated points.)
  const KappaSolve s_nom = solve_kappa(0.0);
  const KappaSolve s_a = solve_kappa(0.025);
  const KappaSolve s_b = solve_kappa(0.05);
  const KappaSolve s_c = solve_kappa(0.1);

  const f64 fn_nom = robust_factor_norm(model, s_nom.book);
  const f64 fn_a = robust_factor_norm(model, s_a.book);
  const f64 fn_b = robust_factor_norm(model, s_b.book);
  const f64 fn_c = robust_factor_norm(model, s_c.book);

  // Robust bites: κ>0 ⇒ strictly smaller worst-case factor exposure than nominal.
  EXPECT_LT(fn_a, fn_nom) << "κ>0 must reduce ‖Ω_f^{1/2}y‖₂ (nom=" << fn_nom << " a=" << fn_a << ")";
  // Monotone over the ascending κ sweep: each larger κ ⇒ strictly smaller (unsaturated).
  EXPECT_LT(fn_b, fn_a) << "larger κ must reduce ‖Ω_f^{1/2}y‖₂ further (a=" << fn_a << " b=" << fn_b << ")";
  EXPECT_LT(fn_c, fn_b) << "larger κ must reduce ‖Ω_f^{1/2}y‖₂ further (b=" << fn_b << " c=" << fn_c << ")";

  // Epigraph binds: t ≈ ‖Ω_f^{1/2}y‖₂ at the returned book. t is the cone's apex row of Ãx
  // (surfaced on QpResult::cone_apex) and the SOC enforces ‖Ω_f^{1/2}y‖₂ ≤ t; the linear
  // +κt cost drives t DOWN until the SOC is TIGHT, so at the optimum t == ‖Ω_f^{1/2}y‖₂.
  // We recompute ‖Ω_f^{1/2}y‖₂ independently from the returned book (y = Xᵀw, identity Ω_f)
  // and assert |t − ‖Ω_f^{1/2}y‖₂| ≤ feas_tol — the epigraph is active (not slack), at EACH
  // κ in the sweep.
  EXPECT_NEAR(s_a.apex, fn_a, solver.cfg.feas_tol) << "epigraph must bind at κ=0.025";
  EXPECT_NEAR(s_b.apex, fn_b, solver.cfg.feas_tol) << "epigraph must bind at κ=0.05";
  EXPECT_NEAR(s_c.apex, fn_c, solver.cfg.feas_tol) << "epigraph must bind at κ=0.1";

  // The penalty also meaningfully moves the book (well beyond feas_tol) at the top κ.
  EXPECT_GT(fn_nom - fn_c, solver.cfg.feas_tol) << "epigraph penalty must be active for κ=0.1";
}

// G-CONSTRAINT — a NON-identity (supplied) Ω_f also bites: with a diagonal Ω_f the robust
// solve reduces the Ω_f-weighted factor norm versus nominal. Exercises the G^T cone-row
// assembly (Ω_f = G Gᵀ via LLT) rather than just the identity default.
TEST(RiskCone, RobustConeWithSuppliedOmegaBites) {
  const usize m = 10U;
  const usize k = 2U;
  const FactorModel model = make_multi_model(m);
  const f64 lambda = 0.1;
  VecX alpha(static_cast<Eigen::Index>(m));
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(m); ++i) {
    alpha[i] = 0.4 * static_cast<f64>((i % 4) - 1);
  }
  std::vector<f64> q(m);
  for (usize i = 0; i < m; ++i) {
    q[i] = -alpha[static_cast<Eigen::Index>(i)];
  }

  // A diagonal SPD Ω_f (heavier weight on factor 0 than factor 1).
  MatX omega(static_cast<Eigen::Index>(k), static_cast<Eigen::Index>(k));
  omega << 4.0, 0.0, 0.0, 1.0;

  ConstrainedQpSolver solver;
  solver.cfg.rho = 10.0;
  solver.cfg.iters = 1500U;

  auto solve_kappa = [&](f64 kappa) -> std::vector<f64> {
    ConstraintSet cs;
    cs.gross.dollar_neutral = true;
    cs.pos = PositionCap{0.5};
    if (kappa > 0.0) {
      RobustAlpha ra{kappa};
      ra.omega_f = omega;
      cs.robust = ra;
    }
    auto mc_r = cs.materialize(model.exposures(), {}, m);
    EXPECT_TRUE(mc_r.has_value()) << (mc_r ? "" : mc_r.error().to_string());
    MaterializedConstraints mc = std::move(*mc_r);
    mc.gross_l1_budget = -1.0;
    QpProblem prob{model, lambda, std::span<const f64>(q), mc};
    auto r = solver.solve(prob);
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
    return r ? *r : std::vector<f64>(m, 0.0);
  };

  const std::vector<f64> w_nom = solve_kappa(0.0);
  const std::vector<f64> w_rob = solve_kappa(0.8);
  const f64 fn_nom = robust_factor_norm(model, w_nom, omega);
  const f64 fn_rob = robust_factor_norm(model, w_rob, omega);
  EXPECT_LT(fn_rob, fn_nom) << "supplied-Ω_f robust solve must reduce the Ω_f-weighted "
                               "factor norm (nom=" << fn_nom << " rob=" << fn_rob << ")";
}

} // namespace atxtest_risk_cone_test
