#pragma once

// atx::engine::risk — QuasiDefiniteLdl: a DETERMINISTIC no-pivot static LDLᵀ
// factorization of a symmetric quasi-definite KKT matrix (S8.2).
//
// ===========================================================================
//  What this is
// ===========================================================================
//  The ADMM x-update in `ConstrainedQpSolver` solves the OSQP quasi-definite KKT
//  system (qp_solver.hpp)
//
//      K = [ P + σI    Ãᵀ   ]      σ, ρ > 0
//          [ Ã       −ρ⁻¹I  ]
//
//  K is symmetric *quasi-definite* (Vanderbei 1995, SIAM J. Optim. 5(1):100-113,
//  Thm 2): the (1,1) block P+σI is positive definite (σ>0 lifts the rank-deficient
//  factor Hessian) and the (2,2) block −ρ⁻¹I is negative definite. Such a matrix is
//  *strongly factorizable in ANY symmetric permutation with NO numerical pivoting*.
//  That theorem is the determinism keystone (R5): because no pivot decision is ever
//  data-dependent, the factor is a pure, fixed-order function of K and the (data-
//  independent) fill-reducing permutation — same input ⇒ byte-identical factor.
//
//  This is an LDLᵀ, NOT a Cholesky: D is a DIAGONAL with BOTH positive and negative
//  entries (the inertia mirrors the block structure — one negative pivot per
//  constraint row), so there is NO sqrt anywhere and D is never assumed > 0.
//
//  The algorithm is the QDLDL kernel (osqp/qdldl, Apache-2.0) REIMPLEMENTED from
//  scratch — we do not link it (no new external dependency). Eigen is used ONLY for
//  CSC storage (`Eigen::SparseMatrix<f64>`, column-major) and for the AMD fill-
//  reducing permutation (`Eigen::AMDOrdering<int>`); the numeric factorization and
//  triangular solves are hand-rolled here.
//
// ===========================================================================
//  Symbolic / numeric split (cacheability)
// ===========================================================================
//  factor_symbolic(K) depends ONLY on the sparsity PATTERN of K: it computes the AMD
//  permutation ONCE, the elimination tree, the per-column nonzero counts of L, and
//  the CSC allocation plan (Lp, Li). It is cacheable across rebalances (the KKT
//  pattern is fixed for a fixed constraint set).
//
//  factor_numeric(K) fills L's values (Lx) and the D diagonal over the cached
//  structure in a FIXED elimination order. No pivoting, no sqrt. It precomputes the
//  reciprocals D⁻¹ once so the inner solve loop is DIVISION-FREE (R5 / QDLDL).
//
//  solve(rhs, x): apply the permutation, forward-solve L (unit lower triangular),
//  scale by D⁻¹, back-solve Lᵀ, then apply the inverse permutation. Every loop runs
//  in a fixed CSC traversal order; there is no RNG, clock, threading, or data-
//  dependent branch anywhere — the solve is byte-identical across thread counts.
//
// ===========================================================================
//  Determinism contract (R1 / R5 / G-DET)
// ===========================================================================
//   * No numerical pivoting — the elimination order is the fixed AMD permutation,
//     chosen from the PATTERN of K only (independent of K's values).
//   * No RNG, no clock, no threading; the factorization is purely serial.
//   * Fixed-order reductions: every accumulation walks L/K in ascending CSC order.
//   * Division-free inner loop: D⁻¹ is precomputed once in factor_numeric.
//   * Same input ⇒ byte-identical Lx, D, perm and byte-identical solve output.

#include <cassert> // assert (solve() span-length precondition)
#include <span>    // std::span (solve I/O)
#include <string>  // std::to_string (zero-pivot diagnostic message)
#include <vector>  // std::vector (CSC factor storage + symbolic workspaces)

#include <Eigen/Dense>           // Eigen::MatrixXd (reconstruct() diagnostic)
#include <Eigen/OrderingMethods> // Eigen::AMDOrdering (fill-reducing permutation)
#include <Eigen/SparseCore>      // Eigen::SparseMatrix (CSC storage)

#include "atx/core/error.hpp"         // Status, Ok, Err, ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX (reconstruct() diagnostic)
#include "atx/core/types.hpp"         // f64, usize, i32

namespace atx::engine::risk {

// A deterministic no-pivot LDLᵀ for a symmetric quasi-definite KKT matrix.
//
// Lifecycle: factor_symbolic(K) once per sparsity pattern, factor_numeric(K) once
// per value-set (e.g. once per QP solve for fixed ρ, σ), then solve(rhs, x) any
// number of times (every ADMM outer iteration). The symbolic result is retained on
// the instance so a re-factor with the same pattern can skip straight to numeric.
class QuasiDefiniteLdl {
public:
  using SpMat = Eigen::SparseMatrix<atx::f64>; // column-major CSC, matches qp_solver
  using Status = atx::core::Status;

  // -------------------------------------------------------------------------
  //  Symbolic phase — AMD permutation + elimination tree + L pattern + alloc plan.
  //  Depends ONLY on the sparsity pattern of K. Cacheable across rebalances.
  //  Err(InvalidArgument) if K is not square; the upper triangle is what is read.
  // -------------------------------------------------------------------------
  [[nodiscard]] Status factor_symbolic(const SpMat &K) {
    namespace co = atx::core;
    if (K.rows() != K.cols()) {
      return co::Err(co::ErrorCode::InvalidArgument, "QuasiDefiniteLdl: K must be square");
    }
    n_ = static_cast<atx::usize>(K.rows());

    // (1) Fill-reducing AMD permutation from the PATTERN only. AMD operates on the
    //     symmetric pattern Kᵀ+K, so it is well-defined on our symmetric K and is a
    //     pure function of the pattern — the determinism anchor (R5). perm_ maps
    //     OLD index -> NEW position (solve() applies it in both directions).
    Eigen::AMDOrdering<int> ordering;
    Eigen::PermutationMatrix<Eigen::Dynamic, Eigen::Dynamic, int> p;
    ordering(K.template selfadjointView<Eigen::Upper>(), p);

    perm_.assign(n_, 0);
    if (static_cast<atx::usize>(p.size()) == n_) {
      for (atx::usize i = 0; i < n_; ++i) {
        perm_[i] = static_cast<atx::usize>(p.indices()[static_cast<Eigen::Index>(i)]); // old i -> new position
      }
    } else {
      // AMDOrdering can return an empty permutation for a trivial/empty pattern;
      // fall back to the identity (still deterministic, just no fill reduction).
      for (atx::usize i = 0; i < n_; ++i) {
        perm_[i] = i;
      }
    }

    // (2) Materialize the upper triangle of the PERMUTED matrix Kp = P K Pᵀ in CSC.
    //     We only need the pattern here, but we reuse the same permuted-assembly
    //     routine numerically below, so build values too (cheap, and keeps the two
    //     passes structurally identical — fewer ways to diverge).
    build_permuted_upper(K);

    // (3) Elimination tree + per-column L nonzero counts from the permuted pattern.
    if (!build_etree()) {
      return co::Err(co::ErrorCode::Internal,
                     "QuasiDefiniteLdl: elimination-tree build failed (pattern not upper-"
                     "triangular consistent)");
    }

    // (4) Column pointers Lp from the counts; allocate Li/Lx. L is unit lower-
    //     triangular in the PERMUTED ordering (diagonal implicit).
    Lp_.assign(n_ + 1, 0);
    for (atx::usize c = 0; c < n_; ++c) {
      Lp_[c + 1] = Lp_[c] + Lnz_[c];
    }
    const atx::usize lnnz = Lp_[n_];
    Li_.assign(lnnz, 0);
    Lx_.assign(lnnz, 0.0);
    D_.assign(n_, 0.0);
    Dinv_.assign(n_, 0.0);
    symbolic_ready_ = true;
    numeric_ready_ = false;
    return co::Ok();
  }

  // -------------------------------------------------------------------------
  //  Numeric phase — QDLDL no-pivot pass over the cached symbolic structure.
  //  Fills Lx and the D diagonal in the fixed elimination order; precomputes D⁻¹.
  //  Err(Internal) on a zero pivot (should not happen for a regularized quasi-
  //  definite KKT — reported rather than dividing by zero).
  // -------------------------------------------------------------------------
  [[nodiscard]] Status factor_numeric(const SpMat &K) {
    namespace co = atx::core;
    if (!symbolic_ready_) {
      return co::Err(co::ErrorCode::Internal,
                     "QuasiDefiniteLdl: factor_numeric called before factor_symbolic");
    }
    if (static_cast<atx::usize>(K.rows()) != n_ || static_cast<atx::usize>(K.cols()) != n_) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "QuasiDefiniteLdl: factor_numeric K dimension differs from symbolic");
    }

    // Refresh the permuted upper-triangle VALUES (pattern is identical to symbolic).
    build_permuted_upper(K);

    // QDLDL numeric kernel. Dense scatter workspaces, all length n.
    //   yMarkers[i] : 1 if column i's accumulator yVals[i] is "live" this column.
    //   yVals[i]    : running dense accumulator of the current column's L entries.
    //   yIdx        : the live row indices (the reach), filled via the etree.
    //   elimBuffer  : stack of pattern indices while walking each entry's etree path.
    //   nextInCol[c]: next free write slot in L's column c (advances as we fill Lx).
    std::vector<atx::i32> yMarkers(n_, 0);
    std::vector<atx::f64> yVals(n_, 0.0);
    std::vector<atx::usize> yIdx(n_, 0);
    std::vector<atx::usize> elimBuffer(n_, 0);
    std::vector<atx::usize> nextInCol(n_, 0);
    for (atx::usize c = 0; c < n_; ++c) {
      nextInCol[c] = Lp_[c];
    }

    for (atx::usize k = 0; k < n_; ++k) {
      // --- (a) Scatter column k of the permuted upper triangle. The strict-upper
      //         entries seed the accumulator; the diagonal seeds D[k]. The reach of
      //         L's row k is built via the elimination tree in the EXACT QDLDL
      //         order (children-before-parents, appended reversed from elimBuffer),
      //         then processed in REVERSE below — this is the fixed order the kernel
      //         requires for a correct (and deterministic) factorization. ---------
      atx::usize nnzY = 0;
      D_[k] = 0.0;
      const atx::usize col_start = Kup_p_[k];
      const atx::usize col_end = Kup_p_[k + 1];
      for (atx::usize idx = col_start; idx < col_end; ++idx) {
        const atx::usize bidx = Kup_i_[idx]; // row index, bidx <= k (upper triangle)
        const atx::f64 v = Kup_x_[idx];
        if (bidx == k) {
          D_[k] = v; // diagonal seed (each (k,k) cell is unique in our assembly)
          continue;
        }
        yVals[bidx] = v; // y(bidx) = b(bidx)

        // Trace the elimination path: bidx, then its etree ancestors strictly below
        // k, stopping at the first already-visited node. Each newly-touched node is
        // pushed onto elimBuffer, then flushed in REVERSE into the reach yIdx.
        if (yMarkers[bidx] == 0) {
          yMarkers[bidx] = 1;
          elimBuffer[0] = bidx;
          atx::usize nnzE = 1;

          atx::usize nextIdx = etree_[bidx];
          while (nextIdx != kInvalid && nextIdx < k) {
            if (yMarkers[nextIdx] == 1) {
              break;
            }
            yMarkers[nextIdx] = 1;
            elimBuffer[nnzE] = nextIdx;
            ++nnzE;
            nextIdx = etree_[nextIdx];
          }
          while (nnzE > 0) {
            yIdx[nnzY++] = elimBuffer[--nnzE];
          }
        }
      }

      // --- (b) Eliminate. Process the reach in REVERSE (i = nnzY-1 .. 0) so each
      //         ancestor column's accumulator is fully formed before it is consumed.
      //         The inner reduction walks column cidx of L in ascending CSC order
      //         (fixed-order — determinism). ----------------------------------------
      for (atx::usize t = nnzY; t-- > 0;) {
        const atx::usize cidx = yIdx[t];          // an ancestor column cidx < k
        const atx::usize cfill = nextInCol[cidx]; // entries already written in col cidx
        const atx::f64 yVals_cidx = yVals[cidx];
        for (atx::usize j = Lp_[cidx]; j < cfill; ++j) {
          yVals[Li_[j]] -= Lx_[j] * yVals_cidx;
        }
        // L[k, cidx] = y(cidx) · D⁻¹[cidx]; subtract its rank-1 effect from the pivot.
        Li_[cfill] = k;
        const atx::f64 lkc = yVals_cidx * Dinv_[cidx];
        Lx_[cfill] = lkc;
        D_[k] -= yVals_cidx * lkc;
        nextInCol[cidx] = cfill + 1;
        // Reset this accumulator slot / marker for the next column.
        yVals[cidx] = 0.0;
        yMarkers[cidx] = 0;
      }

      // --- (c) Finalize the pivot. Quasi-definite ⇒ nonzero pivots with no pivoting;
      //         a zero pivot means the matrix is not quasi-definite (report). ----
      if (D_[k] == 0.0) {
        return co::Err(co::ErrorCode::Internal,
                       "QuasiDefiniteLdl: zero pivot at column " + std::to_string(k) +
                           " — matrix is not quasi-definite (check σ, ρ > 0)");
      }
      Dinv_[k] = 1.0 / D_[k];
    }

    numeric_ready_ = true;
    return co::Ok();
  }

  // -------------------------------------------------------------------------
  //  Solve K x = rhs. Applies P, forward-solves L, scales by D⁻¹, back-solves Lᵀ,
  //  then applies Pᵀ. Order-fixed and division-free (D⁻¹ precomputed). rhs and x
  //  may alias different buffers; both must have length n.
  // -------------------------------------------------------------------------
  void solve(std::span<const atx::f64> rhs, std::span<atx::f64> x) const {
    assert(rhs.size() == n_ && x.size() == n_ && "QuasiDefiniteLdl::solve: rhs/x must have length n");
    // (1) Permute rhs into the factorization ordering: xp[new] = rhs[old].
    std::vector<atx::f64> xp(n_, 0.0);
    for (atx::usize i = 0; i < n_; ++i) {
      xp[perm_[i]] = rhs[i];
    }

    // (2) Forward solve L xp = xp (unit lower triangular; diagonal implicit = 1).
    //     Column-oriented: each solved xp[c] propagates into its sub-diagonal rows.
    for (atx::usize c = 0; c < n_; ++c) {
      const atx::f64 xc = xp[c];
      const atx::usize cstart = Lp_[c];
      const atx::usize cend = Lp_[c + 1];
      for (atx::usize j = cstart; j < cend; ++j) {
        xp[Li_[j]] -= Lx_[j] * xc;
      }
    }

    // (3) Diagonal solve: xp = D⁻¹ xp (division-free — reciprocals precomputed).
    for (atx::usize i = 0; i < n_; ++i) {
      xp[i] *= Dinv_[i];
    }

    // (4) Back solve Lᵀ xp = xp (descending columns; Lᵀ's row c uses col-c entries).
    for (atx::usize c = n_; c-- > 0;) {
      atx::f64 acc = xp[c];
      const atx::usize cstart = Lp_[c];
      const atx::usize cend = Lp_[c + 1];
      for (atx::usize j = cstart; j < cend; ++j) {
        acc -= Lx_[j] * xp[Li_[j]];
      }
      xp[c] = acc;
    }

    // (5) Un-permute back to the caller's ordering: x[old] = xp[new].
    for (atx::usize i = 0; i < n_; ++i) {
      x[i] = xp[perm_[i]];
    }
  }

  // -------------------------------------------------------------------------
  //  Diagnostics / test hooks (G-DIFF, G-DET). Not on the hot path.
  // -------------------------------------------------------------------------

  // Reconstruct K' = Pᵀ (L D Lᵀ) P in the ORIGINAL ordering as a dense matrix, for
  // the differential reconstruction gate (‖K' − K‖∞). Returns the (n×n) dense form.
  [[nodiscard]] atx::core::linalg::MatX reconstruct() const {
    using atx::core::linalg::MatX;
    const auto en = static_cast<Eigen::Index>(n_);
    // M = L D Lᵀ in the PERMUTED ordering. Build L explicitly (unit lower) then form.
    MatX L = MatX::Identity(en, en);
    for (atx::usize c = 0; c < n_; ++c) {
      for (atx::usize j = Lp_[c]; j < Lp_[c + 1]; ++j) {
        L(static_cast<Eigen::Index>(Li_[j]), static_cast<Eigen::Index>(c)) = Lx_[j];
      }
    }
    MatX Dd = MatX::Zero(en, en);
    for (atx::usize i = 0; i < n_; ++i) {
      Dd(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i)) = D_[i];
    }
    const MatX M = L * Dd * L.transpose(); // permuted reconstruction

    // Un-permute: K'(old_i, old_j) = M(new_i, new_j). perm_[old] = new.
    MatX out(en, en);
    for (atx::usize oi = 0; oi < n_; ++oi) {
      for (atx::usize oj = 0; oj < n_; ++oj) {
        out(static_cast<Eigen::Index>(oi), static_cast<Eigen::Index>(oj)) =
            M(static_cast<Eigen::Index>(perm_[oi]), static_cast<Eigen::Index>(perm_[oj]));
      }
    }
    return out;
  }

  [[nodiscard]] atx::usize dim() const noexcept { return n_; }
  [[nodiscard]] bool symbolic_ready() const noexcept { return symbolic_ready_; }
  [[nodiscard]] bool numeric_ready() const noexcept { return numeric_ready_; }

  // The D diagonal (permuted ordering). The inertia (count of negatives) must equal
  // the number of constraint rows of the quasi-definite KKT — the G-DIFF inertia
  // cross-check that confirms no pivoting reordered the signs.
  [[nodiscard]] const std::vector<atx::f64> &diag() const noexcept { return D_; }
  [[nodiscard]] const std::vector<atx::f64> &diag_inv() const noexcept { return Dinv_; }
  [[nodiscard]] const std::vector<atx::usize> &perm() const noexcept { return perm_; }
  [[nodiscard]] const std::vector<atx::usize> &Lp() const noexcept { return Lp_; }
  [[nodiscard]] const std::vector<atx::usize> &Li() const noexcept { return Li_; }
  [[nodiscard]] const std::vector<atx::f64> &Lx() const noexcept { return Lx_; }

  // Count of negative D entries = the matrix's negative inertia (the # of ND pivots).
  [[nodiscard]] atx::usize negative_inertia() const noexcept {
    atx::usize neg = 0;
    for (atx::usize i = 0; i < n_; ++i) {
      if (D_[i] < 0.0) {
        ++neg;
      }
    }
    return neg;
  }

private:
  // A sentinel "no parent" for the elimination tree (the virtual root's parent).
  static constexpr atx::usize kInvalid = static_cast<atx::usize>(-1);

  // -------------------------------------------------------------------------
  //  Assemble the UPPER triangle of the permuted matrix Kp = P K Pᵀ in CSC.
  //  We walk the lower OR upper stored entries of the symmetric K, map each (i,j)
  //  through perm_, and keep the upper-triangular permuted entries (row ≤ col).
  //  Built in a fixed per-column, ascending-row order (determinism). Our assembly
  //  is duplicate-free by construction (each permuted (i,j) is written once), so no
  //  cell summing occurs; the diagonal is seeded by assignment in factor_numeric.
  // -------------------------------------------------------------------------
  void build_permuted_upper(const SpMat &K) {
    // First pass: count per-column nonzeros of the permuted upper triangle.
    Kup_p_.assign(n_ + 1, 0);
    // Walk K once; K is symmetric so each stored (i,j) with value v contributes a
    // permuted entry at (pi, pj). We only keep the upper-triangular permuted cell
    // (min(pi,pj), max(pi,pj)); the off-diagonal is stored ONCE in the upper part.
    // To stay duplicate-free we read only K's stored upper triangle (it is built
    // symmetric in qp_solver, but we canonicalize by always taking i<=j source).
    // Count.
    for (int col = 0; col < static_cast<int>(n_); ++col) {
      for (SpMat::InnerIterator it(K, col); it; ++it) {
        const atx::usize si = static_cast<atx::usize>(it.row());
        const atx::usize sj = static_cast<atx::usize>(it.col());
        if (si > sj) {
          continue; // use only the source upper triangle; symmetry gives the rest
        }
        const atx::usize pi = perm_[si];
        const atx::usize pj = perm_[sj];
        const atx::usize ucol = pi < pj ? pj : pi; // store in the larger permuted index col
        Kup_p_[ucol + 1]++;
      }
    }
    for (atx::usize c = 0; c < n_; ++c) {
      Kup_p_[c + 1] += Kup_p_[c];
    }
    const atx::usize nnz = Kup_p_[n_];
    Kup_i_.assign(nnz, 0);
    Kup_x_.assign(nnz, 0.0);

    // Second pass: scatter into per-column slots, then sort each column's rows
    // ascending (fixed order). We fill with a per-column write cursor.
    std::vector<atx::usize> cursor(Kup_p_.begin(), Kup_p_.end() - 1);
    for (int col = 0; col < static_cast<int>(n_); ++col) {
      for (SpMat::InnerIterator it(K, col); it; ++it) {
        const atx::usize si = static_cast<atx::usize>(it.row());
        const atx::usize sj = static_cast<atx::usize>(it.col());
        if (si > sj) {
          continue;
        }
        const atx::usize pi = perm_[si];
        const atx::usize pj = perm_[sj];
        const atx::usize urow = pi < pj ? pi : pj;
        const atx::usize ucol = pi < pj ? pj : pi;
        const atx::usize slot = cursor[ucol]++;
        Kup_i_[slot] = urow;
        Kup_x_[slot] = it.value();
      }
    }

    // Sort each column's entries by ascending row (insertion sort on indices —
    // columns are short; this is a fixed, deterministic permutation of indices, NOT
    // a sort on floating data: ties are impossible because each (urow,ucol) is
    // unique here). Keeps the upper-triangle CSC canonical for the etree pass.
    for (atx::usize c = 0; c < n_; ++c) {
      const atx::usize s = Kup_p_[c];
      const atx::usize e = Kup_p_[c + 1];
      for (atx::usize a = s + 1; a < e; ++a) {
        const atx::usize ri = Kup_i_[a];
        const atx::f64 vi = Kup_x_[a];
        atx::usize b = a;
        while (b > s && Kup_i_[b - 1] > ri) {
          Kup_i_[b] = Kup_i_[b - 1];
          Kup_x_[b] = Kup_x_[b - 1];
          --b;
        }
        Kup_i_[b] = ri;
        Kup_x_[b] = vi;
      }
    }
  }

  // -------------------------------------------------------------------------
  //  Elimination tree + per-column L nonzero counts (Lnz_) of the permuted upper
  //  triangle. Standard QDLDL etree: for each strict-upper entry (i, k) with i<k,
  //  walk i's current ancestors up to k, attaching unattached nodes' parent to k
  //  and counting one L nonzero per traversed node. Returns false on an out-of-
  //  range parent (pattern inconsistency).
  // -------------------------------------------------------------------------
  [[nodiscard]] bool build_etree() {
    etree_.assign(n_, kInvalid);
    Lnz_.assign(n_, 0);
    std::vector<atx::usize> visit(n_, kInvalid); // last column that visited node i

    for (atx::usize k = 0; k < n_; ++k) {
      visit[k] = k;
      const atx::usize cs = Kup_p_[k];
      const atx::usize ce = Kup_p_[k + 1];
      for (atx::usize idx = cs; idx < ce; ++idx) {
        atx::usize i = Kup_i_[idx];
        if (i >= k) {
          continue; // diagonal or (defensively) lower — not a strict-upper entry
        }
        // Walk ancestors of i until we reach a node already visited by column k.
        while (visit[i] != k) {
          if (etree_[i] == kInvalid) {
            etree_[i] = k; // attach i to k
          }
          Lnz_[i]++;       // one more L nonzero in column i
          visit[i] = k;
          i = etree_[i];
          if (i >= n_) {
            return false; // parent out of range ⇒ pattern inconsistency
          }
        }
      }
    }
    return true;
  }

  // ---- Dimension --------------------------------------------------------
  atx::usize n_ = 0;

  // ---- Permutation (old -> new) -----------------------------------------
  std::vector<atx::usize> perm_; // perm_[old] = new position (solve() applies it both ways)

  // ---- Permuted upper-triangle CSC (Kp = P K Pᵀ, upper part) -------------
  std::vector<atx::usize> Kup_p_; // column pointers (n+1)
  std::vector<atx::usize> Kup_i_; // row indices (ascending within a column)
  std::vector<atx::f64> Kup_x_;   // values

  // ---- Symbolic structure ------------------------------------------------
  std::vector<atx::usize> etree_; // elimination tree parent (kInvalid = root)
  std::vector<atx::usize> Lnz_;   // per-column L nonzero count

  // ---- L factor (unit lower triangular, permuted ordering) + D ----------
  std::vector<atx::usize> Lp_; // L column pointers (n+1)
  std::vector<atx::usize> Li_; // L row indices
  std::vector<atx::f64> Lx_;   // L values (strict lower; diagonal implicit = 1)
  std::vector<atx::f64> D_;    // D diagonal (mixed sign — quasi-definite)
  std::vector<atx::f64> Dinv_; // precomputed D⁻¹ (division-free solve)

  bool symbolic_ready_ = false;
  bool numeric_ready_ = false;
};

} // namespace atx::engine::risk
