#pragma once

// atx::engine::risk — cone: SECOND-ORDER-CONE (SOC) descriptors + the DETERMINISTIC
// cone projections that let the factor-augmented ADMM (qp_solver.hpp) carry conic
// constraints WITHOUT a new factorization (S8.5a — the cone keystone).
//
// ===========================================================================
//  Why a cone enters ONLY through the z-projection (design rule R5)
// ===========================================================================
//  The S8.1 solver is an OSQP-class operator-splitting (ADMM) method: the x-update
//  is a single fixed sparse-KKT solve  [[P+σI, Ãᵀ],[Ã, −ρ⁻¹I]]  factored ONCE per
//  solve (kkt_ldl.hpp), and ALL constraint geometry lives in the z-update — today a
//  per-row clamp z = clamp(Ãx + y/ρ, l, u). A second-order cone is just a DIFFERENT
//  projection on a contiguous block of Ã-rows: instead of clamping each row into its
//  [l, u] band, we project the whole block jointly onto the SOC / ball. The Hessian
//  P, the KKT matrix, and the no-pivot LDLᵀ are UNTOUCHED — adding cone rows to Ã
//  only widens its sparsity, which the factorization machinery already handles for an
//  arbitrary Ã. So a cone adds NO new factorization and does NOT alter the x-update
//  (R5); it is purely a z-projection swap on its row range.
//
// ===========================================================================
//  The deterministic SOC projection (the keystone primitive)
// ===========================================================================
//  Project a point (s, z) — scalar apex s, vector z — onto the second-order cone
//      K = { (s, z) : ‖z‖₂ ≤ s } :
//    * ‖z‖₂ ≤  s  →  (s, z)                       (already inside)
//    * ‖z‖₂ ≤ −s  →  (0, 0)                       (in the polar cone)
//    * else       →  ρ = (s + ‖z‖₂)/2 ;  (ρ, ρ·z/‖z‖₂)   (boundary projection)
//  ‖z‖₂ is an ORDER-FIXED reduction (ascending index, single accumulator) so it is
//  bitwise-reproducible regardless of thread count (R1 / Track E). This is the closed
//  form the G-DIFF gate checks to ULP across all three branches + the z=0 / s=0 edges.
//
//  For a tracking-error BALL the apex s ≡ radius is a FIXED scalar (not an
//  optimization variable): ball_project(z, radius) is the same map specialized to a
//  constant apex — keep z if ‖z‖₂ ≤ radius, else rescale to radius·z/‖z‖₂. (A radius
//  ≤ 0 collapses the ball to {0}.)
//
// ===========================================================================
//  The tracking-error cone via the low-rank factor structure (NEVER densify V, R4)
// ===========================================================================
//  ‖V^{1/2}u‖₂² = uᵀVu = (Xᵀu)ᵀ F (Xᵀu) + uᵀ D u,  u = w − w_bench,  V = X F Xᵀ + D.
//  Factor F = L_F L_Fᵀ (Cholesky of factor_cov(), K×K — Eigen::LLT, fixed-order /
//  no-pivot; a failed LLT ⇒ Err). Define the cone-argument vector
//      a = [ L_Fᵀ (Xᵀu) ;  sqrt(D) ∘ u ]            (length K + M)
//  so ‖V^{1/2}u‖₂ = ‖a‖₂ and the tracking-error constraint is exactly ‖a‖₂ ≤ te.
//  REUSE the existing factor aux block: Xᵀw = y (n_y = K), so Xᵀu = y − Xᵀw_bench.
//  The factor part of a is therefore the LINEAR map L_Fᵀ on the y-block PLUS the
//  CONSTANT −L_Fᵀ Xᵀw_bench; the specific part sqrt(D)∘(w−w_bench) is sqrt(D)∘ on the
//  w-block PLUS the constant −sqrt(D)∘w_bench. build_augmented emits K+M cone rows
//  whose product Ãx is [L_Fᵀ y ; sqrt(D)∘w] and stores the constant offset
//      b = [ −L_Fᵀ Xᵀw_bench ;  −sqrt(D)∘w_bench ]
//  on a SocBlock; the z-update projects (Ãx + …) + b onto the ball of radius te and
//  subtracts b back so the splitting variable z stays in Ã x units (see qp_solver.hpp).
//
// ===========================================================================
//  Determinism (R1 / R4)
// ===========================================================================
//  No RNG, no clock, no unordered iteration. ‖·‖₂ is an order-fixed single-accumulator
//  reduction. The cone argument is built from exposures() (Xᵀ matvec on a constant
//  w_bench), the K×K Cholesky of factor_cov(), and the elementwise sqrt of
//  specific_var() — all cheap, all fixed-order, NEVER an M×M materialization.

#include <cmath>   // std::sqrt
#include <span>    // std::span (w_bench)
#include <utility> // std::move
#include <vector>  // std::vector (offset, w_bench snapshot)

#include "atx/core/linalg/linalg.hpp" // VecX
#include "atx/core/types.hpp"         // f64, usize

namespace atx::engine::risk {

// ===========================================================================
//  SocBlock — a contiguous range of Ã-rows projected JOINTLY onto a ball / SOC
//  instead of clamped elementwise. AugmentedQp carries a std::vector<SocBlock>
//  appended in the fixed emission order (after the box rows); the ADMM z-update
//  (qp_solver.hpp) consults it to overwrite ONLY those rows with the projection.
//
//  For the S8.5a tracking-error cone the geometry is the BALL ‖Ãx + offset‖₂ ≤ radius
//  (a fixed-apex SOC): `radius` is te, `offset` is b = [−L_Fᵀ Xᵀw_bench ; −sqrt(D)∘w_bench]
//  (length == dim). The z-update projects (Ãx-target + offset) onto the ball of radius
//  `radius`, then subtracts `offset` so z stays in Ã x units.
// ===========================================================================
struct SocBlock {
  atx::usize row_start = 0;        // first Ã-row of the block (block spans [row_start, row_start+dim))
  atx::usize dim = 0;              // number of rows projected jointly (K + M for tracking-error)
  atx::f64 radius = 0.0;           // ball radius (te for tracking-error); ≤ 0 ⇒ the block collapses to {0}
  atx::core::linalg::VecX offset;  // constant b added to Ãx before projection (length dim)
};

// ===========================================================================
//  TrackingError — the user-facing SOC descriptor (the S8.5 spec form):
//  ‖V^{1/2}(w − w_bench)‖₂ ≤ te_budget. `w_bench` is a length-M benchmark book the
//  tracking error is measured RELATIVE to (an empty span ⇒ tracked vs the all-zero
//  book). It carries the S8.6 relaxation metadata (priority / elastic) like every
//  other descriptor — S8.5a only enforces it (R3); S8.6 consumes those fields.
// ===========================================================================
struct TrackingError {
  std::span<const atx::f64> w_bench; // length-M benchmark book (empty ⇒ all-zero)
  atx::f64 te_budget = 0.0;          // ‖V^{1/2}(w − w_bench)‖₂ ≤ this (≥ 0)
  atx::usize priority = 0;           // S8.6 relaxation rank (lower = relaxed first)
  bool elastic = false;              // S8.6 may relax this cone when infeasible
};

// ===========================================================================
//  TrackingErrorSpec — the materialized tracking-error REQUEST. ConstraintSet's
//  TrackingError descriptor materializes into this (a w_bench snapshot + te budget);
//  build_augmented consumes it together with the FactorModel's F/D to assemble the
//  K+M cone rows and the SocBlock offset. It is carried on MaterializedConstraints
//  because the cone-argument math needs L_F = chol(F) and sqrt(D), which live on the
//  FactorModel (only reachable in build_augmented), not in materialize().
// ===========================================================================
struct TrackingErrorSpec {
  bool active = false;            // a TrackingError descriptor was set
  atx::f64 te_budget = 0.0;       // ‖V^{1/2}(w − w_bench)‖₂ ≤ te_budget
  std::vector<atx::f64> w_bench;  // length-M benchmark book (empty ⇒ treated as all-zero)
};

// Order-fixed Euclidean norm of a vector slice: single accumulator, ASCENDING index
// (R1 — bitwise-reproducible regardless of thread count). Sqrt of Σ vᵢ². The span is
// the cone-argument block; the reduction order is the canonical row order.
[[nodiscard]] inline atx::f64 ordered_norm2(std::span<const atx::f64> v) noexcept {
  atx::f64 acc = 0.0; // single accumulator, ascending index
  for (atx::usize i = 0; i < v.size(); ++i) {
    acc += v[i] * v[i];
  }
  return std::sqrt(acc);
}

// ===========================================================================
//  Deterministic SOC projection onto K = {(s, z) : ‖z‖₂ ≤ s}. Writes the projected
//  vector part into z_out (length == z.size()) and returns the projected apex s*.
//  Three branches (verbatim closed form):
//    * ‖z‖₂ ≤  s  → (s,   z)            (inside)
//    * ‖z‖₂ ≤ −s  → (0,   0)            (polar cone)
//    * else       → (ρ, ρ·z/‖z‖₂), ρ = (s+‖z‖₂)/2   (boundary)
//  ‖z‖₂ via the order-fixed ordered_norm2. z=0 and s=0 edges fall out cleanly
//  (‖z‖₂==0 ≤ s for s≥0 ⇒ inside/identity; ≤ −s for s≤0 ⇒ collapses to 0).
// ===========================================================================
[[nodiscard]] inline atx::f64 soc_project(atx::f64 s, std::span<const atx::f64> z,
                                          std::span<atx::f64> z_out) noexcept {
  const atx::f64 nz = ordered_norm2(z);
  if (nz <= s) { // already inside (covers z=0, s≥0)
    for (atx::usize i = 0; i < z.size(); ++i) {
      z_out[i] = z[i];
    }
    return s;
  }
  if (nz <= -s) { // polar cone ⇒ origin
    for (atx::usize i = 0; i < z.size(); ++i) {
      z_out[i] = 0.0;
    }
    return 0.0;
  }
  // Boundary projection. nz > |s| here ⇒ nz > 0, so the divide is well-defined.
  const atx::f64 rho = 0.5 * (s + nz);
  const atx::f64 scale = rho / nz;
  for (atx::usize i = 0; i < z.size(); ++i) {
    z_out[i] = scale * z[i];
  }
  return rho;
}

// ===========================================================================
//  Ball projection — the FIXED-APEX specialization of the SOC: project z onto the
//  Euclidean ball {z : ‖z‖₂ ≤ radius}. Keeps z when ‖z‖₂ ≤ radius, else rescales to
//  radius·z/‖z‖₂. A radius ≤ 0 collapses the ball to {0}. This is the projection the
//  tracking-error cone uses (the apex te is a constant, not an optimization variable):
//  it is EXACTLY soc_project(radius, z, ·) restricted to its vector part for radius ≥ 0,
//  written directly so the apex never needs threading through the ADMM z-update.
//  ‖·‖₂ via the order-fixed ordered_norm2 (R1).
// ===========================================================================
inline void ball_project(std::span<const atx::f64> z, atx::f64 radius,
                         std::span<atx::f64> z_out) noexcept {
  if (radius <= 0.0) { // degenerate ball ⇒ origin
    for (atx::usize i = 0; i < z.size(); ++i) {
      z_out[i] = 0.0;
    }
    return;
  }
  const atx::f64 nz = ordered_norm2(z);
  if (nz <= radius) { // inside ⇒ identity
    for (atx::usize i = 0; i < z.size(); ++i) {
      z_out[i] = z[i];
    }
    return;
  }
  const atx::f64 scale = radius / nz; // nz > radius > 0 ⇒ well-defined
  for (atx::usize i = 0; i < z.size(); ++i) {
    z_out[i] = scale * z[i];
  }
}

} // namespace atx::engine::risk
