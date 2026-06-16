#pragma once

// atx::engine::risk — qp_augment: the FACTOR-AUGMENTED SPARSE QP assembly (S8.1).
//
// ===========================================================================
//  Why augment (§0.2 of the S8 plan — the perf keystone)
// ===========================================================================
//  The as-built `ConstrainedQpSolver` is O(M²)/iter because it materializes the
//  augmented constraint matrix Ã DENSE over n≈3M variables and applies P=2λV
//  matrix-free through a Jacobi-PCG. S8.1 reformulates the SAME QP into a sparse
//  quasi-definite form whose KKT can be factored once (S8.2) and reused:
//
//  Introduce the factor aux variable  y = Xᵀw  (K extra vars) with the K equality
//  rows  y − Xᵀw = 0. Then
//
//      ½ wᵀ(2λV) w  =  ½ wᵀ(2λ X F Xᵀ) w  +  ½ wᵀ(2λ D) w
//                   =  ½ yᵀ(2λF) y         +  ½ wᵀ(2λ D) w
//
//  so the Hessian over the (w, y) block is the SPARSE  P = blkdiag(2λD, 2λF):
//  D is the M floored specific variances on the w diagonal, F is the K×K factor
//  covariance on the y block. V is NEVER densified (R4) — F is the cached factor
//  capacitance (R7). The dense XFXᵀ M×M term is gone; it lives in K factor space.
//
// ===========================================================================
//  The P scaling convention — factor of 2λ (DOCUMENTED, verified vs qp_solver.hpp)
// ===========================================================================
//  The as-built solver's objective is  ½ wᵀP w + qᵀw  with  P = 2λV  (see
//  qp_solver.hpp's QpProblem doc "P = 2·risk_aversion·V" and apply_kkt:
//  `out_w = 2λ·(V·v_w)`). To keep the augmented problem the SAME optimization
//  problem (identical optimum) we carry that 2λ into BOTH augmented Hessian
//  blocks: the w-block diagonal is  2λ·D  and the y-block is  2λ·F. Equivalently
//  P_aug = blkdiag(2λD, 2λF) so that ½ xᵀ P_aug x reproduces ½ wᵀ(2λV) w exactly.
//  (λ == 0 ⇒ P_aug == 0, matching the as-built P = 0 — a pure-LP feasibility solve.)
//
// ===========================================================================
//  The augmented constraint stack Ã  (FIXED row order, R1)
// ===========================================================================
//  Variables (fixed column order):  x = [ w (M) ; y (K) ; s (M, gross) ; r (M, turn) ]
//      n_w   = M
//      n_y   = K
//      n_aux = (has_gross ? M : 0) + (has_turnover ? M : 0)
//
//  Rows (fixed emission order — the offsets are reproducible for a fixed set):
//    (0) the K FACTOR-DEFINITION equality rows  y − Xᵀw = 0  (l = u = 0):
//        per factor k:  −X(·,k) on the w-block, +1 at y_k.  These sit FIRST, at
//        rows [0, K), so the augmentation structure is at a known offset.
//    (1) the S1-1 MaterializedConstraints linear rows A (on the w-block), [l, u].
//        Factor-exposure rows are Xᵀ-structured, box/group rows are sparse 0/1,
//        beta is one dense-ish row — all carried as SPARSE triplets.
//    (2) gross L1 split (if has_gross):  w_i − s_i ≤ 0 ; −w_i − s_i ≤ 0 ;
//        s_i ≥ 0 ; Σ s_i ≤ gross.  (structured sparse — never a dense block)
//    (3) turnover L1 split (if has_turnover):  w_i − r_i ≤ ref_i ;
//        −w_i − r_i ≤ −ref_i ; r_i ≥ 0 ; Σ r_i ≤ turnover.
//
//  Every triplet is inserted in a FIXED traversal order (ascending row, then the
//  documented per-row column order) and the CSC is built once via a deterministic
//  setFromTriplets with NO duplicate entries (so no data-dependent dup-summation).
//
// ===========================================================================
//  Determinism (R1 / Track E)
// ===========================================================================
//  No RNG, no data-dependent ordering. The triplet list is emitted in a fixed
//  order; Xᵀ traversal is ascending (factor k, then instrument i); the sparse
//  patterns of P and Ã are a pure function of (M, K, the constraint shape). The
//  numeric values are a pure function of (D, F, λ, X, A, l, u, budgets).

#include <span>    // std::span (q)
#include <utility> // std::move
#include <vector>  // std::vector (triplet scratch)

#include <Eigen/Dense>
#include <Eigen/SparseCore>

#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/risk/constraints.hpp"  // MaterializedConstraints
#include "atx/engine/risk/factor_model.hpp" // FactorModel (exposures / specific_var)

namespace atx::engine::risk {

// A large finite bound standing in for ±∞ on a one-sided split row. Identical to
// the as-built solver's kInf so the augmented feasibility band matches the oracle:
// far above any realistic |w|/|s|/|r|, yet finite so clamp() / the feasibility scan
// stay well-defined (no inf arithmetic).
inline constexpr atx::f64 kAugInf = 1e30;

// The factor-augmented sparse QP. P and A_tilde are CSC sparse (column-major). The
// problem is  minimize ½ xᵀ P x + q_augᵀ x  s.t.  l ≤ A_tilde x ≤ u  over the
// augmented variable x = [w; y; s; r] (n_w + n_y + n_aux entries).
struct AugmentedQp {
  Eigen::SparseMatrix<atx::f64> P;       // (n × n) blkdiag(2λD, 2λF, 0)  — the Hessian
  Eigen::SparseMatrix<atx::f64> A_tilde; // (R̃ × n) augmented constraint matrix
  atx::core::linalg::VecX q_aug;         // (n) augmented linear term [q; 0; 0; 0]
  atx::core::linalg::VecX l;             // (R̃) lower bounds
  atx::core::linalg::VecX u;             // (R̃) upper bounds
  atx::usize n_w = 0;                    // M weight vars (block 0)
  atx::usize n_y = 0;                    // K factor aux vars  y = Xᵀw  (block 1)
  atx::usize n_aux = 0;                  // gross s (M) + turnover r (M) aux vars (block 2)
};

namespace detail {

// Fixed-order count of the augmented constraint rows. MUST mirror the emission
// order in build_augmented EXACTLY (R1).
[[nodiscard]] inline atx::usize aug_total_rows(atx::usize m, atx::usize k,
                                               const MaterializedConstraints &c, bool has_gross,
                                               bool has_turn) noexcept {
  atx::usize r = k;                              // (0) the K  y − Xᵀw = 0  rows
  r += static_cast<atx::usize>(c.A.rows());      // (1) the S1-1 linear rows
  if (has_gross) {
    r += m + m + m + 1U;                         // (2) ±w−s≤0 ; s≥0 ; Σs≤L
  }
  if (has_turn) {
    r += m + m + m + 1U;                         // (3) ±w−r≤ref ; r≥0 ; Σr≤T
  }
  return r;
}

} // namespace detail

// Build the factor-augmented sparse QP from the factored covariance V, the risk
// aversion λ, the linear term q (length M, = −aim), and the materialized S1-1
// constraint block C (linear rows + gross/turnover L1 metadata).
//
// PRECONDITIONS (the solver validates these up front; build is a pure assembler):
//   q.size() == V.n_instruments();  C.A.cols() == M when C.A has rows;
//   C.l.size() == C.u.size() == C.A.rows();  λ ≥ 0.
[[nodiscard]] inline AugmentedQp build_augmented(const FactorModel &V, atx::f64 lambda,
                                                 std::span<const atx::f64> q,
                                                 const MaterializedConstraints &C) {
  namespace cl = atx::core::linalg;
  using Trip = Eigen::Triplet<atx::f64>;

  const atx::usize m = V.n_instruments();
  const atx::usize k = V.n_factors();
  const cl::MatX &X = V.exposures();          // M×K exposures (never densifies V)
  const cl::VecX &D = V.specific_var();        // M floored specific variances (≥ d_min)

  const bool has_gross = C.gross_l1_budget >= 0.0;
  const bool has_turn = C.has_turnover;

  AugmentedQp out;
  out.n_w = m;
  out.n_y = k;
  out.n_aux = (has_gross ? m : 0U) + (has_turn ? m : 0U);
  const atx::usize n = out.n_w + out.n_y + out.n_aux;

  // Column offsets (fixed): w | y | s | r.
  const atx::usize w_off = 0U;
  const atx::usize y_off = m;
  const atx::usize s_off = m + k;                       // gross aux, present iff has_gross
  const atx::usize r_off = s_off + (has_gross ? m : 0U); // turnover aux, present iff has_turn

  const atx::f64 two_lambda = 2.0 * lambda;

  // -------------------------------------------------------------------------
  //  (A) Hessian  P = blkdiag(2λD on w, 2λF on y, 0 on aux).  Fixed insertion
  //      order: every w diagonal (ascending i), then the full y block in column-
  //      major order (factor column b, then row a) so the CSC is reproducible.
  // -------------------------------------------------------------------------
  std::vector<Trip> p_trips;
  p_trips.reserve(m + k * k);
  for (atx::usize i = 0; i < m; ++i) { // 2λ·D on the w diagonal
    p_trips.emplace_back(static_cast<int>(w_off + i), static_cast<int>(w_off + i),
                         two_lambda * D[static_cast<Eigen::Index>(i)]);
  }
  for (atx::usize b = 0; b < k; ++b) {   // 2λ·F on the y block (column-major: col b, row a)
    for (atx::usize a = 0; a < k; ++a) {
      p_trips.emplace_back(static_cast<int>(y_off + a), static_cast<int>(y_off + b),
                           two_lambda * V.factor_cov()(static_cast<Eigen::Index>(a),
                                                       static_cast<Eigen::Index>(b)));
    }
  }
  out.P.resize(static_cast<int>(n), static_cast<int>(n));
  out.P.setFromTriplets(p_trips.begin(), p_trips.end());
  out.P.makeCompressed();

  // -------------------------------------------------------------------------
  //  (B) The augmented linear term  q_aug = [q; 0; 0; 0].
  // -------------------------------------------------------------------------
  out.q_aug = cl::VecX::Zero(static_cast<Eigen::Index>(n));
  for (atx::usize i = 0; i < m; ++i) {
    out.q_aug[static_cast<Eigen::Index>(w_off + i)] = q[i];
  }

  // -------------------------------------------------------------------------
  //  (C) The augmented constraint matrix Ã, with bounds [l, u]. Fixed row order:
  //      (0) y − Xᵀw = 0   (1) S1-1 linear rows   (2) gross split   (3) turnover.
  // -------------------------------------------------------------------------
  const atx::usize r_total = detail::aug_total_rows(m, k, C, has_gross, has_turn);
  out.l = cl::VecX::Zero(static_cast<Eigen::Index>(r_total));
  out.u = cl::VecX::Zero(static_cast<Eigen::Index>(r_total));

  std::vector<Trip> a_trips;
  // worst-case nnz: K(M+1) factor rows + nnz(A) + gross(4M) + turn(4M).
  a_trips.reserve(k * (m + 1U) + static_cast<atx::usize>(C.A.rows()) * m + 8U * m);

  atx::usize row = 0U; // running row cursor (fixed advance, mirrors aug_total_rows)

  // (0) the K factor-definition rows  y_k − Σ_i X(i,k) w_i = 0  (l = u = 0).
  //     Column order per row: the w columns (ascending i), then the single y_k.
  for (atx::usize fk = 0; fk < k; ++fk) {
    for (atx::usize i = 0; i < m; ++i) {
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(w_off + i),
                           -X(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(fk)));
    }
    a_trips.emplace_back(static_cast<int>(row), static_cast<int>(y_off + fk), 1.0);
    out.l[static_cast<Eigen::Index>(row)] = 0.0;
    out.u[static_cast<Eigen::Index>(row)] = 0.0;
    ++row;
  }

  // (1) the S1-1 linear rows A (w-block only); skip exact-zero entries (sparse).
  for (Eigen::Index ar = 0; ar < C.A.rows(); ++ar) {
    for (atx::usize j = 0; j < m; ++j) {
      const atx::f64 aij = C.A(ar, static_cast<Eigen::Index>(j));
      if (aij != 0.0) {
        a_trips.emplace_back(static_cast<int>(row), static_cast<int>(w_off + j), aij);
      }
    }
    out.l[static_cast<Eigen::Index>(row)] = C.l[ar];
    out.u[static_cast<Eigen::Index>(row)] = C.u[ar];
    ++row;
  }

  // (2) gross L1 split (structured sparse): s_i ≥ |w_i| and Σ s_i ≤ gross.
  if (has_gross) {
    for (atx::usize i = 0; i < m; ++i) { // w_i − s_i ≤ 0
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(w_off + i), 1.0);
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(s_off + i), -1.0);
      out.l[static_cast<Eigen::Index>(row)] = -kAugInf;
      out.u[static_cast<Eigen::Index>(row)] = 0.0;
      ++row;
    }
    for (atx::usize i = 0; i < m; ++i) { // −w_i − s_i ≤ 0
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(w_off + i), -1.0);
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(s_off + i), -1.0);
      out.l[static_cast<Eigen::Index>(row)] = -kAugInf;
      out.u[static_cast<Eigen::Index>(row)] = 0.0;
      ++row;
    }
    for (atx::usize i = 0; i < m; ++i) { // s_i ≥ 0
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(s_off + i), 1.0);
      out.l[static_cast<Eigen::Index>(row)] = 0.0;
      out.u[static_cast<Eigen::Index>(row)] = kAugInf;
      ++row;
    }
    for (atx::usize i = 0; i < m; ++i) { // Σ s_i ≤ gross
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(s_off + i), 1.0);
    }
    out.l[static_cast<Eigen::Index>(row)] = -kAugInf;
    out.u[static_cast<Eigen::Index>(row)] = C.gross_l1_budget;
    ++row;
  }

  // (3) turnover L1 split (structured sparse): r_i ≥ |w_i − ref_i| and Σ r_i ≤ T.
  if (has_turn) {
    for (atx::usize i = 0; i < m; ++i) { // w_i − r_i ≤ ref_i
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(w_off + i), 1.0);
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(r_off + i), -1.0);
      out.l[static_cast<Eigen::Index>(row)] = -kAugInf;
      out.u[static_cast<Eigen::Index>(row)] = C.turnover_ref[i];
      ++row;
    }
    for (atx::usize i = 0; i < m; ++i) { // −w_i − r_i ≤ −ref_i
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(w_off + i), -1.0);
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(r_off + i), -1.0);
      out.l[static_cast<Eigen::Index>(row)] = -kAugInf;
      out.u[static_cast<Eigen::Index>(row)] = -C.turnover_ref[i];
      ++row;
    }
    for (atx::usize i = 0; i < m; ++i) { // r_i ≥ 0
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(r_off + i), 1.0);
      out.l[static_cast<Eigen::Index>(row)] = 0.0;
      out.u[static_cast<Eigen::Index>(row)] = kAugInf;
      ++row;
    }
    for (atx::usize i = 0; i < m; ++i) { // Σ r_i ≤ T
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(r_off + i), 1.0);
    }
    out.l[static_cast<Eigen::Index>(row)] = -kAugInf;
    out.u[static_cast<Eigen::Index>(row)] = C.turnover_budget;
    ++row;
  }

  out.A_tilde.resize(static_cast<int>(r_total), static_cast<int>(n));
  out.A_tilde.setFromTriplets(a_trips.begin(), a_trips.end());
  out.A_tilde.makeCompressed();

  return out;
}

} // namespace atx::engine::risk
