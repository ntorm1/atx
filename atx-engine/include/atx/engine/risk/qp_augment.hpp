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
//    (4) tracking-error SOC (S8.5a, if tracking.active):  K + M cone rows whose product
//        Ãx is [ L_Fᵀ y ; sqrt(D)∘w ]. These are NOT box rows — their [l, u] band is
//        ±kAugInf and the row range is projected JOINTLY onto the ball ‖Ãx + offset‖₂ ≤
//        te in the ADMM z-update (qp_solver.hpp), where offset = [ −L_Fᵀ Xᵀw_bench ;
//        −sqrt(D)∘w_bench ] is carried on the appended SocBlock. EMPTY ⇒ box-only path
//        (zero cones ⇒ byte-identical to S8.4, R10).
//    (5) sector-risk SOC (S8.5b, if sector_risk.active): one K + M cone block PER sector
//        with a finite σ_g > 0, in ascending sector_id order. Each block's Ãx is
//        [ L_Fᵀ Xᵀ(mask_g∘w) ; sqrt(D)∘(mask_g∘w) ] with offset = 0 (NO benchmark) and
//        is ball-projected to radius σ_g. ROUTE 1 (direct w-block rows — no aux vars):
//        adds NO column / factorization (R5), never densifies V (R4). The √-impact
//        surrogate is NOT a row here — it folds into P's w-diagonal + q (above).
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

#include <cmath>   // std::sqrt (sqrt(D) cone-argument block)
#include <span>    // std::span (q)
#include <utility> // std::move (SocBlock into out.cones)
#include <vector>  // std::vector (triplet scratch)

#include <Eigen/Cholesky> // Eigen::LLT (L_F = chol(F) for the tracking-error cone)
#include <Eigen/Dense>
#include <Eigen/SparseCore>

#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/core/macro.hpp"               // ATX_ASSERT (LLT SPD invariant)

#include "atx/engine/risk/cone.hpp"         // SocBlock (the cone-block descriptor, S8.5a)
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

  // CONE blocks (S8.5a) — contiguous Ã-row ranges projected JOINTLY onto a ball / SOC
  // in the ADMM z-update instead of clamped elementwise (qp_solver.hpp). EMPTY for the
  // box-only path (zero cones ⇒ the z-update is byte-identical to S8.4, R10). The cone
  // rows are emitted AFTER the box rows (fixed order, R1); each row's [l, u] band is
  // ±kAugInf so the inert elementwise clamp leaves them untouched before the projection
  // overwrites them. (S8.5a uses exactly one block: the tracking-error ball.)
  std::vector<SocBlock> cones;
};

namespace detail {

// Count of sector-risk SOC cones with a FINITE, STRICTLY-POSITIVE budget σ_g — the
// sectors that actually emit a (K+M)-row cone (a non-positive σ_g is a degenerate
// ball {0}, treated as "no SOC for that sector" and skipped; R1 — fixed convention).
// Order-irrelevant for the count (it is a tally), but the EMISSION below walks the same
// ascending sector_id (== ascending sigma index) order.
[[nodiscard]] inline atx::usize active_sector_cone_count(const MaterializedConstraints &c) noexcept {
  if (!c.sector_risk.active) {
    return 0U;
  }
  atx::usize n = 0U;
  for (const atx::f64 s : c.sector_risk.sigma) {
    if (s > 0.0) {
      ++n;
    }
  }
  return n;
}

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
  if (c.tracking.active) {
    r += k + m;                                  // (4) tracking-error SOC: K + M cone rows
  }
  // (5) sector-risk SOC (S8.5b): K + M cone rows per sector with a finite σ_g > 0.
  r += active_sector_cone_count(c) * (k + m);
  // (6) robust alpha SOC (S8.5c): when κ > 0, ONE variable-apex cone = 1 apex row + K
  //     cone-argument rows (the epigraph t + Ω_f^{1/2} y). κ ≤ 0 ⇒ no rows (R10).
  if (c.robust.active && c.robust.kappa > 0.0) {
    r += 1U + k;
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
  // S8.5c robust alpha: ONE epigraph aux column t when κ > 0 (κ ≤ 0 ⇒ no column, no cone
  // ⇒ byte-identical to S8.5b, R10). Appended AFTER the gross/turnover aux columns.
  const bool has_robust = C.robust.active && C.robust.kappa > 0.0;

  AugmentedQp out;
  out.n_w = m;
  out.n_y = k;
  out.n_aux = (has_gross ? m : 0U) + (has_turn ? m : 0U) + (has_robust ? 1U : 0U);
  const atx::usize n = out.n_w + out.n_y + out.n_aux;

  // Column offsets (fixed): w | y | s | r | t.
  const atx::usize w_off = 0U;
  const atx::usize y_off = m;
  const atx::usize s_off = m + k;                       // gross aux, present iff has_gross
  const atx::usize r_off = s_off + (has_gross ? m : 0U); // turnover aux, present iff has_turn
  const atx::usize t_off = r_off + (has_turn ? m : 0U);  // robust epigraph aux, present iff has_robust

  const atx::f64 two_lambda = 2.0 * lambda;

  // √-impact quadratic surrogate (S8.5b, R9): the PRE-COMPUTED per-name coefficient
  // c_i ≥ 0 (from the ONE cost surface — exec::ImpactCfg — populated by the caller on
  // C.impact.coeff). The surrogate cost Σ_i c_i (w_i − w_prev_i)² folds into P's w-diag
  // (P[i,i] += 2 c_i) and q (q[i] += −2 c_i w_prev_i). INERT (returns 0) when the spec is
  // inactive / a coeff is absent ⇒ the +0.0 fold is byte-identical to the pre-S8.5b P/q
  // (R10). c_i is clamped to ≥ 0 so the w-block stays PSD even if a caller mis-prices.
  const bool has_impact = C.impact.active && !C.impact.coeff.empty();
  const auto impact_c = [&](atx::usize i) noexcept -> atx::f64 {
    if (!has_impact || i >= C.impact.coeff.size()) {
      return 0.0;
    }
    const atx::f64 ci = C.impact.coeff[i];
    return (ci > 0.0) ? ci : 0.0; // PSD guard: negative coeff cannot enter P
  };
  // w_prev for the surrogate IS the turnover L1 reference (reused). Absent ⇒ flat zero.
  const auto impact_wprev = [&](atx::usize i) noexcept -> atx::f64 {
    return (C.has_turnover && i < C.turnover_ref.size()) ? C.turnover_ref[i] : 0.0;
  };

  // -------------------------------------------------------------------------
  //  (A) Hessian  P = blkdiag(2λD on w, 2λF on y, 0 on aux).  Fixed insertion
  //      order: every w diagonal (ascending i), then the full y block in column-
  //      major order (factor column b, then row a) so the CSC is reproducible.
  //      The √-impact surrogate adds 2 c_i to the w-diagonal VALUE only (R5 — the
  //      sparsity pattern is unchanged; a zero c_i adds an exact +0.0 ⇒ byte-identical).
  // -------------------------------------------------------------------------
  std::vector<Trip> p_trips;
  p_trips.reserve(m + k * k);
  for (atx::usize i = 0; i < m; ++i) { // 2λ·D (+ 2 c_i for √-impact) on the w diagonal
    const atx::f64 ci = impact_c(i);
    const atx::f64 diag = (ci > 0.0) ? (two_lambda * D[static_cast<Eigen::Index>(i)] + 2.0 * ci)
                                     : two_lambda * D[static_cast<Eigen::Index>(i)];
    p_trips.emplace_back(static_cast<int>(w_off + i), static_cast<int>(w_off + i), diag);
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
  //  (B) The augmented linear term  q_aug = [q; 0; 0; 0]. The √-impact surrogate
  //      adds −2 c_i w_prev_i to the w-block (the linear part of c_i(w_i−w_prev_i)²;
  //      the constant c_i w_prev_i² is dropped — irrelevant to the argmin). Not added
  //      when c_i == 0 ⇒ byte-identical to the pre-S8.5b q when the surrogate is off (R10).
  // -------------------------------------------------------------------------
  out.q_aug = cl::VecX::Zero(static_cast<Eigen::Index>(n));
  for (atx::usize i = 0; i < m; ++i) {
    const atx::f64 ci = impact_c(i);
    out.q_aug[static_cast<Eigen::Index>(w_off + i)] =
        (ci > 0.0) ? (q[i] - 2.0 * ci * impact_wprev(i)) : q[i];
  }
  // S8.5c robust epigraph cost: q_aug[t_col] = κ (the linear +κt term that, against the
  // SOC ‖Ω_f^{1/2} y‖₂ ≤ t, makes the effective penalty κ‖Ω_f^{1/2} y‖₂ at the optimum).
  // The t column's P block is 0 (no quadratic on the epigraph — like the gross/turnover
  // aux columns). Not written when has_robust is false ⇒ q_aug byte-identical to S8.5b.
  if (has_robust) {
    out.q_aug[static_cast<Eigen::Index>(t_off)] = C.robust.kappa;
  }

  // -------------------------------------------------------------------------
  //  (C) The augmented constraint matrix Ã, with bounds [l, u]. Fixed row order:
  //      (0) y − Xᵀw = 0   (1) S1-1 linear rows   (2) gross split   (3) turnover
  //      (4) tracking-error SOC   (5) sector-risk SOC (one K+M cone per finite-σ sector).
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

  // (4) tracking-error SOC (S8.5a): K + M cone rows whose product Ãx is
  //     [ L_Fᵀ y ; sqrt(D) ∘ w ], so with the SocBlock offset
  //     b = [ −L_Fᵀ Xᵀw_bench ; −sqrt(D) ∘ w_bench ]  the cone argument is
  //     a = Ãx + b = [ L_Fᵀ(Xᵀw − Xᵀw_bench) ; sqrt(D)∘(w − w_bench) ], and
  //     ‖a‖₂ = ‖V^{1/2}(w − w_bench)‖₂. The constraint ‖a‖₂ ≤ te is enforced as a
  //     BALL projection on this row range in the z-update (qp_solver.hpp). The rows'
  //     [l, u] band is ±kAugInf so the elementwise box clamp is inert on them — the
  //     projection overwrites them. NEVER densifies V (R4): L_F is the K×K Cholesky of
  //     factor_cov(); sqrt(D) is elementwise on specific_var(); Xᵀw_bench is one Xᵀ
  //     matvec on the constant benchmark book.
  if (C.tracking.active) {
    // L_F = chol(F) (lower). F is SPD by FactorModel::create's contract (its own
    // Cholesky succeeded at construction), so this LLT cannot fail for a valid model.
    const Eigen::LLT<cl::MatX> f_llt(V.factor_cov());
    ATX_ASSERT(f_llt.info() == Eigen::Success); // invariant: FactorModel::create enforces SPD
    const cl::MatX L_F = f_llt.matrixL(); // K×K lower-triangular factor (L_F L_Fᵀ = F)

    // Xᵀw_bench (K) — the constant factor exposure of the benchmark book.
    cl::VecX xtb = cl::VecX::Zero(static_cast<Eigen::Index>(k));
    for (atx::usize fk = 0; fk < k; ++fk) {
      atx::f64 acc = 0.0; // order-fixed: ascending instrument i
      for (atx::usize i = 0; i < m; ++i) {
        acc += X(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(fk)) *
               C.tracking.w_bench[i];
      }
      xtb[static_cast<Eigen::Index>(fk)] = acc;
    }

    const atx::usize cone_start = row;
    SocBlock blk;
    blk.row_start = cone_start;
    blk.dim = k + m;
    blk.radius = C.tracking.te_budget;
    blk.offset = cl::VecX::Zero(static_cast<Eigen::Index>(k + m));

    // (4a) factor part — K rows. Row c of L_Fᵀ y is Σ_a L_F(a,c)·y_a, so entry
    //      (cone row c, y_off + a) = L_F(a, c). Offset b_c = −(L_Fᵀ Xᵀw_bench)_c =
    //      −Σ_a L_F(a,c)·xtb_a. Column order: ascending factor a (the y-block).
    for (atx::usize c = 0; c < k; ++c) {
      atx::f64 off = 0.0;
      for (atx::usize a = 0; a < k; ++a) {
        const atx::f64 lac = L_F(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(c));
        if (lac != 0.0) { // L_F is lower-triangular ⇒ a < c entries are exact zero
          a_trips.emplace_back(static_cast<int>(row), static_cast<int>(y_off + a), lac);
        }
        off += lac * xtb[static_cast<Eigen::Index>(a)];
      }
      blk.offset[static_cast<Eigen::Index>(c)] = -off;
      out.l[static_cast<Eigen::Index>(row)] = -kAugInf; // inert box band — projection owns these rows
      out.u[static_cast<Eigen::Index>(row)] = kAugInf;
      ++row;
    }

    // (4b) specific part — M rows. Row i is sqrt(D_i) at w_off+i; offset
    //      b_{K+i} = −sqrt(D_i)·w_bench_i.
    for (atx::usize i = 0; i < m; ++i) {
      const atx::f64 sd = std::sqrt(D[static_cast<Eigen::Index>(i)]);
      a_trips.emplace_back(static_cast<int>(row), static_cast<int>(w_off + i), sd);
      blk.offset[static_cast<Eigen::Index>(k + i)] = -sd * C.tracking.w_bench[i];
      out.l[static_cast<Eigen::Index>(row)] = -kAugInf;
      out.u[static_cast<Eigen::Index>(row)] = kAugInf;
      ++row;
    }

    out.cones.push_back(std::move(blk));
  }

  // (5) sector-risk SOC (S8.5b): for each sector g with a finite σ_g > 0, the cone
  //     ‖V^{1/2}(mask_g∘w)‖₂ ≤ σ_g. With u = mask_g∘w and V = X F Xᵀ + D this is
  //     ‖a_g‖₂ ≤ σ_g where a_g = [ L_Fᵀ(Xᵀ(mask_g∘w)) ; sqrt(D)∘(mask_g∘w) ] — the SAME
  //     low-rank identity as tracking-error but with NO benchmark ⇒ offset = 0. We use
  //     ROUTE 1 (direct rows on the w-block — NO per-sector aux vars): it adds NO new
  //     columns / factorization (R5) and never densifies V (R4 — the factor part is the
  //     K×K-per-masked-name product L_Fᵀ Xᵀ restricted to the mask, the specific part is
  //     sqrt(D) on the masked diagonal). dim = K + M (we emit M specific rows, the
  //     unmasked names carrying an exact-zero row — simplest fixed pattern, R1). Cones
  //     emitted in ASCENDING sector_id (== ascending σ index) order (R1).
  if (detail::active_sector_cone_count(C) > 0U) {
    // L_F = chol(F) (lower). SPD by FactorModel::create's contract — cannot fail here.
    const Eigen::LLT<cl::MatX> f_llt(V.factor_cov());
    ATX_ASSERT(f_llt.info() == Eigen::Success); // invariant: FactorModel::create enforces SPD
    const cl::MatX L_F = f_llt.matrixL();        // K×K lower-triangular factor (L_F L_Fᵀ = F)

    const std::vector<atx::usize> &sec = C.sector_risk.sector_id; // length M (snapshot)
    const atx::usize n_sectors = C.sector_risk.sigma.size();
    for (atx::usize g = 0; g < n_sectors; ++g) {
      const atx::f64 sigma_g = C.sector_risk.sigma[g];
      if (!(sigma_g > 0.0)) {
        continue; // non-positive budget ⇒ degenerate ball; skip (no cone for this sector)
      }
      SocBlock blk;
      blk.row_start = row;
      blk.dim = k + m;
      blk.radius = sigma_g;
      blk.offset = cl::VecX::Zero(static_cast<Eigen::Index>(k + m)); // NO benchmark ⇒ b = 0

      // (5a) factor part — K rows. Row c maps w via (L_Fᵀ Xᵀ diag(mask_g)): entry
      //      (cone row c, w_off+j) = Σ_a L_F(a,c)·X(j,a), emitted ONLY for masked names
      //      j ∈ g (unmasked columns are exact zero). Column order: ascending j.
      for (atx::usize c = 0; c < k; ++c) {
        for (atx::usize j = 0; j < m; ++j) {
          if (sec[j] != g) {
            continue; // not in sector g ⇒ zero contribution (mask)
          }
          atx::f64 v = 0.0; // (L_Fᵀ)[c,:]·X[j,:] = Σ_a L_F(a,c)·X(j,a) — order-fixed ascending a
          for (atx::usize a = 0; a < k; ++a) {
            v += L_F(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(c)) *
                 X(static_cast<Eigen::Index>(j), static_cast<Eigen::Index>(a));
          }
          if (v != 0.0) {
            a_trips.emplace_back(static_cast<int>(row), static_cast<int>(w_off + j), v);
          }
        }
        out.l[static_cast<Eigen::Index>(row)] = -kAugInf; // inert box band — projection owns these rows
        out.u[static_cast<Eigen::Index>(row)] = kAugInf;
        ++row;
      }

      // (5b) specific part — M rows. Row i is sqrt(D_i) at w_off+i ONLY for masked names
      //      i ∈ g (unmasked names ⇒ exact-zero row). offset = 0 (no benchmark).
      for (atx::usize i = 0; i < m; ++i) {
        if (sec[i] == g) {
          const atx::f64 sd = std::sqrt(D[static_cast<Eigen::Index>(i)]);
          a_trips.emplace_back(static_cast<int>(row), static_cast<int>(w_off + i), sd);
        }
        out.l[static_cast<Eigen::Index>(row)] = -kAugInf;
        out.u[static_cast<Eigen::Index>(row)] = kAugInf;
        ++row;
      }

      out.cones.push_back(std::move(blk));
    }
  }

  // (6) robust alpha-uncertainty SOC (S8.5c): the variable-apex cone ‖Ω_f^{1/2} y‖₂ ≤ t
  //     with the epigraph aux var t. Emits 1 + K rows: an APEX row (Ãx == t, a single
  //     +1.0 at the t column) FIRST, then K cone-argument rows producing Ω_f^{1/2} y on
  //     the y-block. With Ω_f = G Gᵀ (G = lower Cholesky, K×K via Eigen::LLT) the map
  //     Ω_f^{1/2} is realized as Gᵀ: row c of (Gᵀ y) is Σ_a G(a,c)·y_a, so entry
  //     (cone row c, y_off + a) = G(a, c), giving ‖Ω_f^{1/2} y‖₂² = yᵀ G Gᵀ y = yᵀ Ω_f y.
  //     Ω_f EMPTY ⇒ the identity (G = I) ⇒ the cone argument is y directly (entry +1.0 at
  //     y_off + c on row c) ⇒ the penalty is κ‖y‖₂ = κ‖Xᵀw‖₂. offset = 0 (no benchmark /
  //     constant). The block is marked variable_apex so the z-update routes it through
  //     soc_project (apex t is a real variable), NOT ball_project. NEVER densifies V (R4 —
  //     all K×K / K work on the y-block; reuses the factor aux y, Goldfarb-Iyengar).
  if (has_robust) {
    const atx::usize cone_start = row;
    SocBlock blk;
    blk.row_start = cone_start;
    blk.dim = 1U + k;                 // 1 apex row (t) + K cone-argument rows
    blk.radius = 0.0;                 // IGNORED for a variable-apex SOC (the apex IS t)
    blk.offset = cl::VecX::Zero(static_cast<Eigen::Index>(1U + k)); // no benchmark ⇒ b = 0
    blk.variable_apex = true;         // ⇒ soc_project in the z-update (not ball_project)

    // (6a) apex row — Ãx on this row == t (a single +1.0 at the epigraph column t_off).
    a_trips.emplace_back(static_cast<int>(row), static_cast<int>(t_off), 1.0);
    out.l[static_cast<Eigen::Index>(row)] = -kAugInf; // inert box band — the SOC owns this row
    out.u[static_cast<Eigen::Index>(row)] = kAugInf;
    ++row;

    // (6b) K cone-argument rows — Ω_f^{1/2} y. Empty Ω_f ⇒ identity (G = I); else G = chol(Ω_f).
    if (C.robust.omega_f.rows() == 0) { // identity ⇒ cone argument is y directly
      for (atx::usize c = 0; c < k; ++c) {
        a_trips.emplace_back(static_cast<int>(row), static_cast<int>(y_off + c), 1.0);
        out.l[static_cast<Eigen::Index>(row)] = -kAugInf;
        out.u[static_cast<Eigen::Index>(row)] = kAugInf;
        ++row;
      }
    } else { // supplied Ω_f = G Gᵀ (G = lower-triangular Cholesky); emit Gᵀ on the y-block
      const Eigen::LLT<cl::MatX> om_llt(C.robust.omega_f);
      ATX_ASSERT(om_llt.info() == Eigen::Success); // Ω_f SPD invariant (validated K×K up front)
      const cl::MatX G = om_llt.matrixL(); // K×K lower-triangular factor (G Gᵀ = Ω_f)
      for (atx::usize c = 0; c < k; ++c) {
        for (atx::usize a = 0; a < k; ++a) {
          const atx::f64 gac = G(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(c));
          if (gac != 0.0) { // G lower-triangular ⇒ a < c entries are exact zero
            a_trips.emplace_back(static_cast<int>(row), static_cast<int>(y_off + a), gac);
          }
        }
        out.l[static_cast<Eigen::Index>(row)] = -kAugInf;
        out.u[static_cast<Eigen::Index>(row)] = kAugInf;
        ++row;
      }
    }

    out.cones.push_back(std::move(blk));
  }

  out.A_tilde.resize(static_cast<int>(r_total), static_cast<int>(n));
  out.A_tilde.setFromTriplets(a_trips.begin(), a_trips.end());
  out.A_tilde.makeCompressed();

  return out;
}

} // namespace atx::engine::risk
