#pragma once

// atx::engine::risk — qp_factor_admm: the FACTOR-SPACE ADMM for the production constrained
// QP (Lane 6). Same problem as ConstrainedQpSolver,
//
//     minimize_w   ½ wᵀ(2λV + 2C_imp)w + q_effᵀw + κ Σ|w − w_prev|
//     subject to   l ≤ A w ≤ u,   Σ|w| ≤ G,   Σ|w − w_prev| ≤ T
//
// with V = X F Xᵀ + D, solved WITHOUT the factor-augmented KKT.
//
// ===========================================================================
//  Why (the per-iteration floor of the augmented solver)
// ===========================================================================
//  The augmented solver (qp_solver.hpp) carries y = Xᵀw as K extra variables with K dense
//  equality rows, splits the gross/turnover L1 with 2M aux columns, and factors the
//  (n + R̃)-dim KKT once. At M = 3000, K = 64 that LDLᵀ holds ~958k nonzeros, one solve
//  costs ~4.4 ms, and a warm production solve needs ~500 of them (≈ 3.5 s). The fill is a
//  property of the dense factor rows, not of the ordering (an ordering that defers the
//  dense rows only reached 661k).
//
//  Here the splitting keeps the risk EXACT in the x-update and moves only the constraints
//  into z:
//    * box rows (every row of A with exactly one nonzero: position caps, %ADV / %shares
//      folds, discretize pins) and the gross L1 budget form ONE block z_b = w projected
//      onto {lo ≤ z ≤ hi, Σ|z| ≤ G}: a soft-threshold-then-clamp at a scalar τ found by a
//      safeguarded Newton search (deterministic, O(M) per step);
//    * the remaining (dense) rows of A — dollar-neutral, factor exposure, beta, group,
//      sector-net — keep their box clamp z_d = clamp(Â_d w + y/ρ, l̂, û) (Â_d row-scaled to
//      unit ∞-norm);
//    * the turnover L1 (budget and/or κ penalty) is a second identity block z_t whose
//      update is the prox of κ‖·‖₁ + the L1-ball indicator around w_prev.
//  The x-update matrix is then  H = Δ + W Wᵀ  with Δ diagonal (2λD + 2c + σ + ρ_b + ρ_t)
//  and W = [√(2λ) X L_F | Â_dᵀ diag(√ρ_d)]  (M × (K + R_d), L_F = chol(F)), solved by
//  Woodbury through a (K + R_d)² capacitance: O(M(K + R_d)) per iteration, no M×M matrix
//  and no sparse factorization. A ρ change rebuilds the capacitance (O(M(K + R_d)²)).
//
//  The ρ schedule, over-relaxation, equality-row ρ boost, deterministic early exit and warm
//  start are the AdmmSchedule semantics of the augmented solver, applied to this splitting.
//
// ===========================================================================
//  Polish (OSQP §4, adapted) and the feasibility gate
// ===========================================================================
//  After the loop the active set is read off the final z (the clamps and soft-thresholds
//  land EXACTLY on their bounds): names fixed at a box bound, at 0 (gross soft zone) or at
//  w_prev (turnover soft zone); the signs of the free names; the dense rows at a bound; the
//  gross / turnover budgets when their τ is active. ONE equality-constrained QP over the
//  free names is solved exactly (Woodbury again, plus an n_e × n_e Schur complement). The
//  polished book is accepted only if it passes the feasibility gate and its objective is
//  no worse than the ADMM book's (to a 1e-9 + 1e-7·|f| slack, the iterate's own accuracy).
//  The gate checks, in original units: each box bound and dense row to cfg.feas_tol, and
//  the gross / turnover sums to (M + 1)·feas_tol — the tolerance the augmented gate
//  implies (it checks each of the M split rows and the budget row at feas_tol).
//
// ===========================================================================
//  Scope and determinism
// ===========================================================================
//  Cones (tracking error, sector SOC, robust alpha) are NOT handled: factor_admm_eligible()
//  is false for them and ConstrainedQpSolver keeps its augmented path. Every loop is order-
//  fixed; the τ searches are capped; the early exit is tested only every check_every
//  iterations. Same inputs ⇒ the same iteration count ⇒ a byte-identical book.
//
//  Warm start layout (distinct from the augmented one): x_full = w (length M); y_full =
//  [y_box (M) ; y_dense (R_d) ; y_turn (M, iff turnover)] in original units.

#include <algorithm> // std::max, std::min
#include <cmath>     // std::fabs, std::sqrt, std::isfinite, std::copysign
#include <limits>    // std::numeric_limits
#include <span>      // std::span
#include <sstream>   // gate diagnostics
#include <string>    // std::string
#include <utility>   // std::move
#include <vector>    // std::vector

#include <Eigen/Cholesky>
#include <Eigen/Dense>

#include "atx/core/error.hpp"         // Result, Ok, Err, ErrorCode
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/risk/admm_schedule.hpp" // AdmmSchedule, WarmStart, adapt_rho, residuals_converged
#include "atx/engine/risk/constraints.hpp"   // MaterializedConstraints
#include "atx/engine/risk/factor_model.hpp"  // FactorModel
#include "atx/engine/risk/qp_augment.hpp"    // kAugInf (the ±∞ bound sentinel)

namespace atx::engine::risk {

// The solver knobs the factor-space path reads (a subset of QpConfig, passed by value so
// this header does not depend on qp_solver.hpp).
struct FactorAdmmConfig {
  atx::usize iters = 300;   // iteration cap (the schedule's early exit may end sooner)
  atx::f64 rho = 1.0;       // base ρ when no warm ρ is supplied
  atx::f64 sigma = 1e-6;    // proximal regularization
  atx::f64 feas_tol = 1e-6; // gate tolerance (per row; sums get (M + 1)·feas_tol)
  bool polish = true;
};

struct FactorAdmmOutput {
  std::vector<atx::f64> w;    // length M
  std::vector<atx::f64> dual; // [y_box (M) ; y_dense (R_d) ; y_turn (M iff turnover)], original units
  atx::usize iters = 0;
  atx::f64 rho = 0.0;
  atx::f64 prim_res = 0.0;    // worst constraint violation of w (original units)
  atx::f64 dual_res = 0.0;    // ‖Pw + q_eff + Ãᵀy‖∞ at the ADMM iterate (scaled rows)
  bool polished = false;
};

// True when the factor-space path can solve C: no cone blocks. (Elasticity metadata is not
// part of the exact surface and is ignored, as in the augmented solve.)
[[nodiscard]] inline bool factor_admm_eligible(const MaterializedConstraints &C) noexcept {
  return !C.tracking.active && !C.sector_risk.active &&
         !(C.robust.active && C.robust.kappa > 0.0);
}

namespace detail {

namespace cl = atx::core::linalg;

inline constexpr atx::usize kFaTauIters = 100; // cap on each τ search (Newton/bisection)

// The compiled problem: box block, dense rows, budgets, Hessian diagonal, factor loading.
struct FaProblem {
  atx::usize m = 0;
  std::vector<atx::f64> lo, hi;   // per-name box (±kAugInf when absent)
  std::vector<atx::u8> box_on;    // 1 ⇒ name i is in the z_b block (a finite bound or gross)
  std::vector<atx::u8> box_eq;    // 1 ⇒ lo == hi (a pin: ρ boost)
  std::vector<Eigen::Index> dense_rows; // row indices of C.A kept as dense rows
  cl::MatX ad;                    // R_d × M, row-scaled to unit ∞-norm
  std::vector<atx::f64> e;        // R_d row scales (Â_d = E A_d)
  std::vector<atx::f64> dl, du;   // scaled dense-row bounds
  std::vector<atx::u8> d_eq;      // dense equality rows
  bool gross = false;
  atx::f64 gross_budget = 0.0;
  bool turn = false;
  atx::f64 turn_budget = kAugInf; // kAugInf ⇒ no hard budget
  atx::f64 kappa = 0.0;           // turnover penalty
  std::vector<atx::f64> wprev;    // turnover / impact reference (zeros when absent)
  std::vector<atx::f64> free_lo, free_hi; // ±kAugInf (the turnover block has no box)
  std::vector<atx::u8> all_on;            // every name (the turnover block)
  std::vector<atx::f64> pd;       // P diagonal: 2λD + 2c
  cl::VecX q;                     // q_eff = q − 2c∘w_prev
  cl::MatX xl;                    // √(2λ) X L_F  (M × K; 0 columns when λ == 0)
};

[[nodiscard]] inline atx::f64 fa_div_bound(atx::f64 b, atx::f64 a) noexcept {
  if (b <= -kAugInf) {
    return a > 0.0 ? -kAugInf : kAugInf;
  }
  if (b >= kAugInf) {
    return a > 0.0 ? kAugInf : -kAugInf;
  }
  return b / a;
}

[[nodiscard]] inline atx::core::Result<FaProblem> fa_compile(const FactorModel &V,
                                                             atx::f64 lambda,
                                                             std::span<const atx::f64> q,
                                                             const MaterializedConstraints &C) {
  namespace co = atx::core;
  FaProblem f;
  const atx::usize m = V.n_instruments();
  const auto em = static_cast<Eigen::Index>(m);
  f.m = m;
  f.lo.assign(m, -kAugInf);
  f.hi.assign(m, kAugInf);

  // (1) Classify the rows of A by nonzero count in ONE column-major pass (A is dense
  //     column-major; a row-wise scan would stride through it).
  const Eigen::Index r = C.A.rows();
  std::vector<atx::usize> nnz(static_cast<atx::usize>(r), 0U);
  std::vector<Eigen::Index> col(static_cast<atx::usize>(r), -1);
  std::vector<atx::f64> val(static_cast<atx::usize>(r), 0.0);
  if (r > 0) {
    for (Eigen::Index j = 0; j < em; ++j) {
      const atx::f64 *cj = C.A.data() + j * r;
      for (Eigen::Index i = 0; i < r; ++i) {
        if (cj[i] != 0.0) {
          ++nnz[static_cast<atx::usize>(i)];
          col[static_cast<atx::usize>(i)] = j;
          val[static_cast<atx::usize>(i)] = cj[i];
        }
      }
    }
  }
  for (Eigen::Index i = 0; i < r; ++i) {
    const auto iu = static_cast<atx::usize>(i);
    if (nnz[iu] == 0U) {
      if (C.l[i] > 0.0 || C.u[i] < 0.0) {
        return co::Err(co::ErrorCode::InvalidArgument,
                       "factor-space QP: an all-zero constraint row excludes w = 0");
      }
      continue;
    }
    if (nnz[iu] == 1U) {
      const auto j = static_cast<atx::usize>(col[iu]);
      const atx::f64 a = val[iu];
      const atx::f64 b1 = fa_div_bound(C.l[i], a);
      const atx::f64 b2 = fa_div_bound(C.u[i], a);
      f.lo[j] = std::max(f.lo[j], std::min(b1, b2));
      f.hi[j] = std::min(f.hi[j], std::max(b1, b2));
      continue;
    }
    f.dense_rows.push_back(i);
  }
  for (atx::usize j = 0; j < m; ++j) {
    if (f.lo[j] > f.hi[j]) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "factor-space QP: contradictory single-name bounds on name " +
                         std::to_string(j));
    }
  }

  // (2) Dense rows, row-scaled to unit ∞-norm.
  const auto rd = static_cast<Eigen::Index>(f.dense_rows.size());
  f.ad.resize(rd, em);
  f.e.assign(static_cast<atx::usize>(rd), 1.0);
  f.dl.assign(static_cast<atx::usize>(rd), 0.0);
  f.du.assign(static_cast<atx::usize>(rd), 0.0);
  f.d_eq.assign(static_cast<atx::usize>(rd), 0U);
  for (Eigen::Index k = 0; k < rd; ++k) {
    const Eigen::Index i = f.dense_rows[static_cast<atx::usize>(k)];
    const auto ku = static_cast<atx::usize>(k);
    f.ad.row(k) = C.A.row(i);
    const atx::f64 mx = f.ad.row(k).cwiseAbs().maxCoeff();
    const atx::f64 s = (mx > 0.0) ? 1.0 / mx : 1.0;
    f.e[ku] = s;
    f.ad.row(k) *= s;
    f.dl[ku] = (C.l[i] <= -kAugInf) ? -kAugInf : C.l[i] * s;
    f.du[ku] = (C.u[i] >= kAugInf) ? kAugInf : C.u[i] * s;
    f.d_eq[ku] = (C.l[i] == C.u[i] && std::fabs(C.l[i]) < kAugInf) ? 1U : 0U;
  }

  // (3) Budgets.
  f.gross = C.gross_l1_budget >= 0.0;
  f.gross_budget = f.gross ? C.gross_l1_budget : 0.0;
  f.turn = C.has_turnover || C.turnover_penalty > 0.0;
  f.turn_budget = C.has_turnover ? C.turnover_budget : kAugInf;
  f.kappa = C.turnover_penalty;
  f.wprev.assign(m, 0.0);
  if (f.turn) {
    for (atx::usize i = 0; i < m && i < C.turnover_ref.size(); ++i) {
      f.wprev[i] = C.turnover_ref[i];
    }
  }
  if (f.turn) {
    f.free_lo.assign(m, -kAugInf);
    f.free_hi.assign(m, kAugInf);
    f.all_on.assign(m, 1U);
  }
  f.box_on.assign(m, 0U);
  f.box_eq.assign(m, 0U);
  for (atx::usize j = 0; j < m; ++j) {
    const bool bounded = f.lo[j] > -kAugInf || f.hi[j] < kAugInf;
    f.box_on[j] = (f.gross || bounded) ? 1U : 0U;
    f.box_eq[j] = (f.lo[j] == f.hi[j]) ? 1U : 0U;
  }

  // (4) Hessian: diagonal 2λD + 2c (√-impact surrogate, same fold as build_augmented) and
  //     the factor loading √(2λ) X L_F with F = L_F L_Fᵀ.
  const bool has_impact = C.impact.active && !C.impact.coeff.empty();
  const cl::VecX &D = V.specific_var();
  f.pd.assign(m, 0.0);
  f.q.resize(em);
  const atx::f64 two_lambda = 2.0 * lambda;
  for (atx::usize i = 0; i < m; ++i) {
    const atx::f64 ci =
        (has_impact && i < C.impact.coeff.size() && C.impact.coeff[i] > 0.0) ? C.impact.coeff[i]
                                                                             : 0.0;
    f.pd[i] = two_lambda * D[static_cast<Eigen::Index>(i)] + 2.0 * ci;
    f.q[static_cast<Eigen::Index>(i)] = q[i] - 2.0 * ci * f.wprev[i];
  }
  if (lambda > 0.0 && V.n_factors() > 0U) {
    const Eigen::LLT<cl::MatX> llt(V.factor_cov());
    if (llt.info() != Eigen::Success) {
      return co::Err(co::ErrorCode::InvalidArgument, "factor-space QP: F is not SPD");
    }
    const cl::MatX lf = llt.matrixL();
    f.xl.noalias() = V.exposures() * lf;
    f.xl *= std::sqrt(two_lambda);
  } else {
    f.xl.resize(em, 0);
  }
  return co::Ok(std::move(f));
}

// Soft-threshold at τ, then clamp to [lo, hi].
[[nodiscard]] inline atx::f64 fa_soft_clamp(atx::f64 v, atx::f64 tau, atx::f64 lo,
                                            atx::f64 hi) noexcept {
  const atx::f64 a = std::fabs(v) - tau;
  const atx::f64 s = (a > 0.0) ? std::copysign(a, v) : 0.0;
  return std::min(std::max(s, lo), hi);
}

// Σ|soft_clamp(v_i, τ)| over the `on` names and the count of names in the linear regime
// (soft value strictly inside (lo, hi) and nonzero): −slope of the sum in τ.
inline void fa_l1_eval(const cl::VecX &v, const std::vector<atx::f64> &lo,
                       const std::vector<atx::f64> &hi, const std::vector<atx::u8> &on,
                       atx::f64 tau, atx::f64 &sum, atx::f64 &slope) noexcept {
  sum = 0.0;
  slope = 0.0;
  for (Eigen::Index i = 0; i < v.size(); ++i) {
    const auto iu = static_cast<atx::usize>(i);
    if (on[iu] == 0U) {
      continue;
    }
    const atx::f64 a = std::fabs(v[i]) - tau;
    if (a <= 0.0) {
      const atx::f64 z = std::min(std::max(0.0, lo[iu]), hi[iu]);
      sum += std::fabs(z);
      continue;
    }
    const atx::f64 s = std::copysign(a, v[i]);
    if (s <= lo[iu]) {
      sum += std::fabs(lo[iu]);
    } else if (s >= hi[iu]) {
      sum += std::fabs(hi[iu]);
    } else {
      sum += a;
      slope += 1.0;
    }
  }
}

// The smallest τ ≥ tau_min with Σ|soft_clamp(v, τ)| ≤ budget (budget ≥ kAugInf ⇒ tau_min).
// The sum is piecewise linear and non-increasing in τ. Bracketed search: a Newton step from
// the infeasible end (lands exactly on the root when no breakpoint intervenes, else crosses
// at least one), else a Newton step back from the feasible end, else bisection; every 4th
// step is a bisection so the bracket always shrinks geometrically. Capped at kFaTauIters
// (order-fixed ⇒ deterministic); on the cap the feasible end is returned.
[[nodiscard]] inline atx::f64 fa_find_tau(const cl::VecX &v, const std::vector<atx::f64> &lo,
                                          const std::vector<atx::f64> &hi,
                                          const std::vector<atx::u8> &on, atx::f64 budget,
                                          atx::f64 tau_min) noexcept {
  if (budget >= kAugInf) {
    return tau_min;
  }
  atx::f64 s_lo = 0.0;
  atx::f64 sl_lo = 0.0;
  fa_l1_eval(v, lo, hi, on, tau_min, s_lo, sl_lo);
  if (s_lo <= budget) {
    return tau_min;
  }
  atx::f64 t_hi = tau_min;
  for (Eigen::Index i = 0; i < v.size(); ++i) {
    if (on[static_cast<atx::usize>(i)] != 0U) {
      t_hi = std::max(t_hi, std::fabs(v[i]));
    }
  }
  atx::f64 s_hi = 0.0;
  atx::f64 sl_hi = 0.0;
  fa_l1_eval(v, lo, hi, on, t_hi, s_hi, sl_hi);
  if (s_hi > budget) {
    return t_hi; // the box alone exceeds the budget: the least-L1 point is the best we can do
  }
  atx::f64 t_lo = tau_min;
  const atx::f64 tol = 1e-15 * (1.0 + budget);
  for (atx::usize it = 0; it < kFaTauIters; ++it) {
    atx::f64 c = 0.5 * (t_lo + t_hi);
    if (it % 4U != 3U) {
      const atx::f64 from_lo = (sl_lo > 0.0) ? t_lo + (s_lo - budget) / sl_lo : t_hi;
      const atx::f64 from_hi = (sl_hi > 0.0) ? t_hi - (budget - s_hi) / sl_hi : t_lo;
      if (from_lo > t_lo && from_lo < t_hi) {
        c = from_lo;
      } else if (from_hi > t_lo && from_hi < t_hi) {
        c = from_hi;
      }
    }
    atx::f64 s_c = 0.0;
    atx::f64 sl_c = 0.0;
    fa_l1_eval(v, lo, hi, on, c, s_c, sl_c);
    if (std::fabs(s_c - budget) <= tol) {
      return c;
    }
    if (s_c > budget) {
      t_lo = c;
      s_lo = s_c;
      sl_lo = sl_c;
    } else {
      t_hi = c;
      s_hi = s_c;
      sl_hi = sl_c;
    }
    if (t_hi - t_lo <= 1e-16 * (1.0 + t_hi)) {
      break;
    }
  }
  return t_hi;
}

// Woodbury solver for H = diag(delta) + W Wᵀ (W is M × r).
struct FaWoodbury {
  cl::VecX dinv;
  cl::MatX w;
  cl::MatX g; // Δ⁻¹ W
  Eigen::LLT<cl::MatX> cap;
  cl::VecX t;

  [[nodiscard]] bool build(const cl::VecX &delta, cl::MatX wmat) {
    dinv = delta.cwiseInverse();
    w = std::move(wmat);
    g = dinv.asDiagonal() * w;
    const Eigen::Index r = w.cols();
    cl::MatX c = cl::MatX::Identity(r, r);
    if (r > 0) {
      c.noalias() += w.transpose() * g;
    }
    cap.compute(c);
    t.resize(r);
    return cap.info() == Eigen::Success;
  }

  // out = H⁻¹ b.
  void solve(const cl::VecX &b, cl::VecX &out) {
    out = dinv.cwiseProduct(b);
    if (w.cols() == 0) {
      return;
    }
    t.noalias() = w.transpose() * out;
    cap.solveInPlace(t);
    out.noalias() -= g * t;
  }
};

// The ADMM state (scaled dense rows; box/turn blocks in w units).
struct FaState {
  cl::VecX w, zb, yb, zd, yd, zt, yt;
  atx::f64 tau_b = 0.0; // last box/gross threshold (τ > 0 ⇒ gross active)
  atx::f64 tau_t = 0.0; // last turnover threshold
};

// P w = pd∘w + xl (xlᵀ w).
inline void fa_apply_p(const FaProblem &f, const cl::VecX &w, cl::VecX &out) {
  out.resize(w.size());
  for (Eigen::Index i = 0; i < w.size(); ++i) {
    out[i] = f.pd[static_cast<atx::usize>(i)] * w[i];
  }
  if (f.xl.cols() > 0) {
    const cl::VecX t = f.xl.transpose() * w;
    out.noalias() += f.xl * t;
  }
}

// Worst violation of w in original units (box / dense rows at feas_tol scale; the L1 sums
// are returned separately so the gate can apply its (M + 1)·feas_tol sum tolerance).
struct FaViolation {
  atx::f64 row = 0.0;   // worst box / dense-row violation
  atx::f64 sums = 0.0;  // worst gross / turnover budget excess
  atx::f64 total = 0.0; // Σ of every positive violation (the polish acceptance bound)
  Eigen::Index which = -1; // dense row index into C.A (−1 ⇒ box or budget)
};

[[nodiscard]] inline FaViolation fa_violation(const FaProblem &f, const cl::VecX &w) {
  FaViolation v;
  for (atx::usize i = 0; i < f.m; ++i) {
    const atx::f64 wi = w[static_cast<Eigen::Index>(i)];
    const atx::f64 viol = std::max(f.lo[i] - wi, wi - f.hi[i]);
    v.row = std::max(v.row, viol);
    v.total += std::max(0.0, viol);
  }
  if (f.ad.rows() > 0) {
    const cl::VecX a = f.ad * w;
    for (Eigen::Index k = 0; k < a.size(); ++k) {
      const auto ku = static_cast<atx::usize>(k);
      // back to original units: a_orig = a / e
      const atx::f64 ao = a[k] / f.e[ku];
      const atx::f64 lo = (f.dl[ku] <= -kAugInf) ? -kAugInf : f.dl[ku] / f.e[ku];
      const atx::f64 hi = (f.du[ku] >= kAugInf) ? kAugInf : f.du[ku] / f.e[ku];
      const atx::f64 viol = std::max(lo - ao, ao - hi);
      v.total += std::max(0.0, viol);
      if (viol > v.row) {
        v.row = viol;
        v.which = f.dense_rows[ku];
      }
    }
  }
  if (f.gross) {
    atx::f64 g = 0.0;
    for (Eigen::Index i = 0; i < w.size(); ++i) {
      g += std::fabs(w[i]);
    }
    v.sums = std::max(v.sums, g - f.gross_budget);
    v.total += std::max(0.0, g - f.gross_budget);
  }
  if (f.turn && f.turn_budget < kAugInf) {
    atx::f64 t = 0.0;
    for (atx::usize i = 0; i < f.m; ++i) {
      t += std::fabs(w[static_cast<Eigen::Index>(i)] - f.wprev[i]);
    }
    v.sums = std::max(v.sums, t - f.turn_budget);
    v.total += std::max(0.0, t - f.turn_budget);
  }
  return v;
}

[[nodiscard]] inline bool fa_gate_ok(const FaProblem &f, const FaViolation &v,
                                     atx::f64 feas_tol) noexcept {
  return v.row <= feas_tol && v.sums <= feas_tol * static_cast<atx::f64>(f.m + 1U);
}

// ‖y‖∞ over every block in ORIGINAL units (dense-row duals un-scaled by e).
[[nodiscard]] inline atx::f64 fa_dual_inf(const FaProblem &f, const cl::VecX &yb,
                                          const cl::VecX &yd, const cl::VecX &yt) noexcept {
  atx::f64 y = 0.0;
  for (Eigen::Index i = 0; i < yb.size(); ++i) {
    y = std::max(y, std::fabs(yb[i]));
  }
  for (Eigen::Index k = 0; k < yd.size(); ++k) {
    y = std::max(y, std::fabs(yd[k] * f.e[static_cast<atx::usize>(k)]));
  }
  if (f.turn) {
    for (Eigen::Index i = 0; i < yt.size(); ++i) {
      y = std::max(y, std::fabs(yt[i]));
    }
  }
  return y;
}

// f(w) = ½wᵀPw + q_effᵀw + κΣ|w − w_prev|.
[[nodiscard]] inline atx::f64 fa_objective(const FaProblem &f, const cl::VecX &w) {
  cl::VecX pw;
  fa_apply_p(f, w, pw);
  atx::f64 obj = 0.5 * w.dot(pw) + f.q.dot(w);
  if (f.kappa > 0.0) {
    atx::f64 t = 0.0;
    for (atx::usize i = 0; i < f.m; ++i) {
      t += std::fabs(w[static_cast<Eigen::Index>(i)] - f.wprev[i]);
    }
    obj += f.kappa * t;
  }
  return obj;
}

// The z-updates (projections) from the relaxed targets. v_b / v_t are the targets plus y/ρ.
inline void fa_project(const FaProblem &f, const cl::VecX &vb, const cl::VecX &vd,
                       const cl::VecX &vt, atx::f64 rho_t, FaState &s) {
  // (a) box ∩ gross block.
  s.tau_b = f.gross ? fa_find_tau(vb, f.lo, f.hi, f.box_on, f.gross_budget, 0.0) : 0.0;
  for (atx::usize i = 0; i < f.m; ++i) {
    const auto ei = static_cast<Eigen::Index>(i);
    s.zb[ei] = (f.box_on[i] != 0U) ? fa_soft_clamp(vb[ei], s.tau_b, f.lo[i], f.hi[i]) : vb[ei];
  }
  // (b) dense rows: clamp.
  for (Eigen::Index k = 0; k < vd.size(); ++k) {
    const auto ku = static_cast<atx::usize>(k);
    s.zd[k] = std::min(std::max(vd[k], f.dl[ku]), f.du[ku]);
  }
  // (c) turnover: prox of κ‖·‖₁ + the L1-ball indicator, centred at w_prev.
  if (f.turn) {
    cl::VecX u(static_cast<Eigen::Index>(f.m));
    for (atx::usize i = 0; i < f.m; ++i) {
      u[static_cast<Eigen::Index>(i)] = vt[static_cast<Eigen::Index>(i)] - f.wprev[i];
    }
    const atx::f64 pen = f.kappa / rho_t;
    s.tau_t = fa_find_tau(u, f.free_lo, f.free_hi, f.all_on, f.turn_budget, pen);
    for (atx::usize i = 0; i < f.m; ++i) {
      const auto ei = static_cast<Eigen::Index>(i);
      s.zt[ei] = f.wprev[i] + fa_soft_clamp(u[ei], s.tau_t, -kAugInf, kAugInf);
    }
  }
}

// OSQP-style polish on the active set read off the final z. Returns false (and leaves
// w_out untouched) when the active set is inconsistent or the reduced system is singular.
[[nodiscard]] inline bool fa_polish(const FaProblem &f, const FaState &s, atx::f64 rho_t,
                                    cl::VecX &w_out) {
  const atx::usize m = f.m;
  std::vector<atx::u8> fixed(m, 0U);
  cl::VecX wfix = cl::VecX::Zero(static_cast<Eigen::Index>(m));
  std::vector<atx::f64> gsign(m, 0.0);
  std::vector<atx::f64> tsign(m, 0.0);
  const bool gross_act = f.gross && s.tau_b > 0.0;
  const atx::f64 pen = (f.turn && rho_t > 0.0) ? f.kappa / rho_t : 0.0;
  const bool turn_act = f.turn && f.turn_budget < kAugInf && s.tau_t > pen;

  auto fix = [&](atx::usize i, atx::f64 v) -> bool {
    const auto ei = static_cast<Eigen::Index>(i);
    if (fixed[i] != 0U) {
      return wfix[ei] == v;
    }
    fixed[i] = 1U;
    wfix[ei] = v;
    return true;
  };
  for (atx::usize i = 0; i < m; ++i) {
    const auto ei = static_cast<Eigen::Index>(i);
    if (f.box_on[i] != 0U) {
      const atx::f64 z = s.zb[ei];
      if (f.box_eq[i] != 0U || (f.lo[i] > -kAugInf && z == f.lo[i]) ||
          (f.hi[i] < kAugInf && z == f.hi[i])) {
        if (!fix(i, z)) {
          return false;
        }
      } else if (gross_act && z == 0.0) {
        if (!fix(i, 0.0)) {
          return false;
        }
      } else if (gross_act) {
        gsign[i] = (z > 0.0) ? 1.0 : -1.0;
      }
    }
    if (f.turn) {
      const atx::f64 d = s.zt[ei] - f.wprev[i];
      if (d == 0.0) {
        if (!fix(i, f.wprev[i])) {
          return false;
        }
      } else {
        tsign[i] = (d > 0.0) ? 1.0 : -1.0;
      }
    }
  }
  // A name fixed by one block must not carry a sign from another block that contradicts
  // the fixed value (e.g. gross sign + on a name fixed at a negative bound). Fixed names
  // leave the equality rows anyway; their contributions move to the right-hand side.
  std::vector<atx::usize> free_idx;
  for (atx::usize i = 0; i < m; ++i) {
    if (fixed[i] == 0U) {
      free_idx.push_back(i);
    }
  }
  const auto mf = static_cast<Eigen::Index>(free_idx.size());
  if (mf == 0) {
    w_out = wfix;
    return true;
  }
  // Equality rows over the free names: active dense rows, the gross budget, the turnover
  // budget. rhs moves the fixed names' contribution across.
  std::vector<cl::VecX> rows;
  std::vector<atx::f64> rhs;
  for (Eigen::Index k = 0; k < f.ad.rows(); ++k) {
    const auto ku = static_cast<atx::usize>(k);
    const atx::f64 z = s.zd[k];
    atx::f64 bound = 0.0;
    if (f.d_eq[ku] != 0U) {
      bound = f.dl[ku];
    } else if (f.dl[ku] > -kAugInf && z == f.dl[ku]) {
      bound = f.dl[ku];
    } else if (f.du[ku] < kAugInf && z == f.du[ku]) {
      bound = f.du[ku];
    } else {
      continue;
    }
    cl::VecX row(mf);
    for (Eigen::Index j = 0; j < mf; ++j) {
      row[j] = f.ad(k, static_cast<Eigen::Index>(free_idx[static_cast<atx::usize>(j)]));
    }
    rows.push_back(std::move(row));
    rhs.push_back(bound - f.ad.row(k).dot(wfix));
  }
  if (gross_act) {
    cl::VecX row(mf);
    atx::f64 fixed_l1 = 0.0;
    for (atx::usize i = 0; i < m; ++i) {
      if (fixed[i] != 0U) {
        fixed_l1 += std::fabs(wfix[static_cast<Eigen::Index>(i)]);
      }
    }
    for (Eigen::Index j = 0; j < mf; ++j) {
      row[j] = gsign[free_idx[static_cast<atx::usize>(j)]];
    }
    rows.push_back(std::move(row));
    rhs.push_back(f.gross_budget - fixed_l1);
  }
  if (turn_act) {
    cl::VecX row(mf);
    atx::f64 c = 0.0;
    for (atx::usize i = 0; i < m; ++i) {
      if (fixed[i] != 0U) {
        c += std::fabs(wfix[static_cast<Eigen::Index>(i)] - f.wprev[i]);
      }
    }
    for (Eigen::Index j = 0; j < mf; ++j) {
      const atx::usize i = free_idx[static_cast<atx::usize>(j)];
      row[j] = tsign[i];
      c -= tsign[i] * f.wprev[i];
    }
    rows.push_back(std::move(row));
    rhs.push_back(f.turn_budget - c);
  }

  // Linear term over the free names: q_eff + P·w_fixed + κ·tsign.
  cl::VecX pwf;
  fa_apply_p(f, wfix, pwf);
  cl::VecX g(mf);
  cl::VecX dfree(mf);
  cl::MatX xf(mf, f.xl.cols());
  for (Eigen::Index j = 0; j < mf; ++j) {
    const atx::usize i = free_idx[static_cast<atx::usize>(j)];
    const auto ei = static_cast<Eigen::Index>(i);
    g[j] = f.q[ei] + pwf[ei] + f.kappa * tsign[i];
    dfree[j] = f.pd[i];
    if (!(dfree[j] > 0.0)) {
      return false; // no strictly convex diagonal on a free name: skip the polish
    }
    if (f.xl.cols() > 0) {
      xf.row(j) = f.xl.row(ei);
    }
  }
  FaWoodbury pff;
  if (!pff.build(dfree, xf)) {
    return false;
  }
  cl::VecX w0;
  pff.solve(-g, w0);
  cl::VecX wf = w0;
  const auto ne = static_cast<Eigen::Index>(rows.size());
  if (ne > 0) {
    cl::MatX y(mf, ne);
    cl::VecX col;
    for (Eigen::Index k = 0; k < ne; ++k) {
      pff.solve(rows[static_cast<atx::usize>(k)], col);
      y.col(k) = col;
    }
    cl::MatX schur(ne, ne);
    cl::VecX r(ne);
    for (Eigen::Index a = 0; a < ne; ++a) {
      for (Eigen::Index b = 0; b < ne; ++b) {
        schur(a, b) = rows[static_cast<atx::usize>(a)].dot(y.col(b));
      }
      r[a] = rows[static_cast<atx::usize>(a)].dot(w0) - rhs[static_cast<atx::usize>(a)];
    }
    const Eigen::LDLT<cl::MatX> ldlt(schur);
    if (ldlt.info() != Eigen::Success) {
      return false;
    }
    const cl::VecX nu = ldlt.solve(r);
    wf.noalias() -= y * nu;
    for (Eigen::Index a = 0; a < ne; ++a) {
      const atx::f64 res =
          rows[static_cast<atx::usize>(a)].dot(wf) - rhs[static_cast<atx::usize>(a)];
      if (!std::isfinite(res) || std::fabs(res) > 1e-9 * (1.0 + std::fabs(rhs[static_cast<atx::usize>(a)]))) {
        return false; // rank-deficient active set: the equalities are not met
      }
    }
  }
  cl::VecX wp = wfix;
  for (Eigen::Index j = 0; j < mf; ++j) {
    wp[static_cast<Eigen::Index>(free_idx[static_cast<atx::usize>(j)])] = wf[j];
  }
  if (!wp.allFinite()) {
    return false;
  }
  w_out = std::move(wp);
  return true;
}

} // namespace detail

// Solve the QP through the factor-space ADMM. `sched` drives ρ adaptation / early exit;
// `ws` (optional) warm-starts from a previous factor-space solve (layout in the header).
// Err(InvalidArgument) on a malformed / infeasible set or when the final book fails the
// gate. Callers must check factor_admm_eligible(C) first (cones are not supported).
[[nodiscard]] inline atx::core::Result<FactorAdmmOutput>
solve_factor_admm(const FactorModel &V, atx::f64 lambda, std::span<const atx::f64> q,
                  const MaterializedConstraints &C, const FactorAdmmConfig &cfg,
                  const AdmmSchedule &sched, std::span<const atx::f64> x0 = {},
                  std::span<const atx::f64> y0 = {}, atx::f64 rho_warm = 0.0) {
  namespace co = atx::core;
  namespace cl = atx::core::linalg;
  using detail::FaProblem;
  using detail::FaState;
  if (!factor_admm_eligible(C)) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "solve_factor_admm: cone constraints need the augmented solver");
  }
  ATX_TRY(FaProblem f, detail::fa_compile(V, lambda, q, C));
  const auto m = static_cast<Eigen::Index>(f.m);
  const Eigen::Index rd = f.ad.rows();
  const atx::usize ny = f.m + static_cast<atx::usize>(rd) + (f.turn ? f.m : 0U);

  // ρ vectors.
  atx::f64 rho = (rho_warm > 0.0 && std::isfinite(rho_warm)) ? rho_warm : cfg.rho;
  cl::VecX rb(m);
  cl::VecX rdv(rd);
  auto set_rho = [&](atx::f64 base) {
    for (Eigen::Index i = 0; i < m; ++i) {
      const auto iu = static_cast<atx::usize>(i);
      rb[i] = (f.box_on[iu] == 0U) ? 0.0 : base * ((f.box_eq[iu] != 0U) ? sched.eq_rho_scale : 1.0);
    }
    for (Eigen::Index k = 0; k < rd; ++k) {
      rdv[k] = base * ((f.d_eq[static_cast<atx::usize>(k)] != 0U) ? sched.eq_rho_scale : 1.0);
    }
  };
  detail::FaWoodbury wb;
  auto factor = [&]() -> bool {
    cl::VecX delta(m);
    for (Eigen::Index i = 0; i < m; ++i) {
      delta[i] = f.pd[static_cast<atx::usize>(i)] + cfg.sigma + rb[i] + (f.turn ? rho : 0.0);
    }
    const Eigen::Index kf = f.xl.cols();
    cl::MatX w(m, kf + rd);
    if (kf > 0) {
      w.leftCols(kf) = f.xl;
    }
    for (Eigen::Index k = 0; k < rd; ++k) {
      w.col(kf + k) = f.ad.row(k).transpose() * std::sqrt(rdv[k]);
    }
    return wb.build(delta, std::move(w));
  };
  set_rho(rho);
  if (!factor()) {
    return co::Err(co::ErrorCode::InvalidArgument, "solve_factor_admm: capacitance not SPD");
  }

  // State + warm start.
  FaState s;
  s.w = cl::VecX::Zero(m);
  s.zb = cl::VecX::Zero(m);
  s.yb = cl::VecX::Zero(m);
  s.zd = cl::VecX::Zero(rd);
  s.yd = cl::VecX::Zero(rd);
  s.zt = cl::VecX::Zero(m);
  s.yt = cl::VecX::Zero(m);
  const bool seed_x = x0.size() == f.m;
  const bool seed_y = y0.size() == ny;
  if (seed_x) {
    for (Eigen::Index i = 0; i < m; ++i) {
      s.w[i] = x0[static_cast<atx::usize>(i)];
    }
  }
  if (seed_y) {
    for (Eigen::Index i = 0; i < m; ++i) {
      s.yb[i] = (f.box_on[static_cast<atx::usize>(i)] != 0U) ? y0[static_cast<atx::usize>(i)] : 0.0;
    }
    for (Eigen::Index k = 0; k < rd; ++k) {
      // scaled row dual ŷ = y / e (the row was scaled by e).
      s.yd[k] = y0[f.m + static_cast<atx::usize>(k)] / f.e[static_cast<atx::usize>(k)];
    }
    if (f.turn) {
      for (Eigen::Index i = 0; i < m; ++i) {
        s.yt[i] = y0[f.m + static_cast<atx::usize>(rd) + static_cast<atx::usize>(i)];
      }
    }
  }
  {
    // z seeded from the (possibly zero) w: the same projections the loop applies.
    const cl::VecX ad_w = f.ad * s.w;
    detail::fa_project(f, s.w, ad_w, s.w, rho, s);
  }

  const atx::f64 alpha = sched.relax_alpha;
  cl::VecX b(m);
  cl::VecX xt(m);
  cl::VecX axd(rd);
  cl::VecX zrb(m);
  cl::VecX zrd(rd);
  cl::VecX zrt(m);
  cl::VecX vb(m);
  cl::VecX vd(rd);
  cl::VecX vt(m);
  cl::VecX tmp_d(rd);
  cl::VecX pw(m);
  atx::usize done = 0;
  atx::f64 dual_norm = 0.0;
  for (atx::usize it = 0; it < cfg.iters; ++it) {
    // (1) x-update: H x̃ = σw − q + (ρ_b z_b − y_b) + Â_dᵀ(ρ_d z_d − y_d) + (ρ z_t − y_t).
    for (Eigen::Index i = 0; i < m; ++i) {
      b[i] = cfg.sigma * s.w[i] - f.q[i] + (rb[i] * s.zb[i] - s.yb[i]);
    }
    if (rd > 0) {
      tmp_d = rdv.cwiseProduct(s.zd) - s.yd;
      b.noalias() += f.ad.transpose() * tmp_d;
    }
    if (f.turn) {
      b += rho * s.zt - s.yt;
    }
    wb.solve(b, xt);
    // (2) relaxation + z-update + y-update.
    if (rd > 0) {
      axd.noalias() = f.ad * xt;
    }
    for (Eigen::Index i = 0; i < m; ++i) {
      zrb[i] = alpha * xt[i] + (1.0 - alpha) * s.zb[i];
      vb[i] = (rb[i] > 0.0) ? zrb[i] + s.yb[i] / rb[i] : zrb[i];
    }
    for (Eigen::Index k = 0; k < rd; ++k) {
      zrd[k] = alpha * axd[k] + (1.0 - alpha) * s.zd[k];
      vd[k] = zrd[k] + s.yd[k] / rdv[k];
    }
    if (f.turn) {
      for (Eigen::Index i = 0; i < m; ++i) {
        zrt[i] = alpha * xt[i] + (1.0 - alpha) * s.zt[i];
        vt[i] = zrt[i] + s.yt[i] / rho;
      }
    }
    s.w = alpha * xt + (1.0 - alpha) * s.w;
    detail::fa_project(f, vb, vd, vt, rho, s);
    for (Eigen::Index i = 0; i < m; ++i) {
      s.yb[i] += rb[i] * (zrb[i] - s.zb[i]);
    }
    for (Eigen::Index k = 0; k < rd; ++k) {
      s.yd[k] += rdv[k] * (zrd[k] - s.zd[k]);
    }
    if (f.turn) {
      for (Eigen::Index i = 0; i < m; ++i) {
        s.yt[i] += rho * (zrt[i] - s.zt[i]);
      }
    }
    done = it + 1U;

    const bool refactor = is_refactor_point(sched, done);
    const bool check = sched.early_exit && sched.check_every > 0U && done % sched.check_every == 0U;
    if (!refactor && !check) {
      continue;
    }
    // Residual summary (scaled dense rows).
    AdmmResidualNorms nr;
    const cl::VecX adw = f.ad * s.w;
    for (Eigen::Index i = 0; i < m; ++i) {
      if (rb[i] > 0.0) {
        nr.prim = std::max(nr.prim, std::fabs(s.w[i] - s.zb[i]));
        nr.ax = std::max(nr.ax, std::fabs(s.w[i]));
        nr.z = std::max(nr.z, std::fabs(s.zb[i]));
      }
    }
    for (Eigen::Index k = 0; k < rd; ++k) {
      nr.prim = std::max(nr.prim, std::fabs(adw[k] - s.zd[k]));
      nr.ax = std::max(nr.ax, std::fabs(adw[k]));
      nr.z = std::max(nr.z, std::fabs(s.zd[k]));
    }
    cl::VecX aty = s.yb;
    if (rd > 0) {
      aty.noalias() += f.ad.transpose() * s.yd;
    }
    if (f.turn) {
      for (Eigen::Index i = 0; i < m; ++i) {
        nr.prim = std::max(nr.prim, std::fabs(s.w[i] - s.zt[i]));
        nr.ax = std::max(nr.ax, std::fabs(s.w[i]));
        nr.z = std::max(nr.z, std::fabs(s.zt[i]));
      }
      aty += s.yt;
    }
    detail::fa_apply_p(f, s.w, pw);
    nr.dual = (pw + f.q + aty).lpNorm<Eigen::Infinity>();
    nr.px = pw.lpNorm<Eigen::Infinity>();
    nr.aty = aty.lpNorm<Eigen::Infinity>();
    nr.q = f.q.lpNorm<Eigen::Infinity>();
    dual_norm = nr.dual;
    if (check && residuals_converged(nr, sched)) {
      break;
    }
    if (refactor) {
      const atx::f64 next = adapt_rho(rho, nr, sched);
      if (next >= rho * sched.adapt_ratio || next * sched.adapt_ratio <= rho) {
        rho = next;
        set_rho(rho);
        if (!factor()) {
          return co::Err(co::ErrorCode::InvalidArgument,
                         "solve_factor_admm: capacitance not SPD after a rho change");
        }
      }
    }
  }

  FactorAdmmOutput out;
  out.iters = done;
  out.rho = rho;
  out.dual_res = dual_norm;
  cl::VecX w = s.w;
  detail::FaViolation viol = detail::fa_violation(f, w);
  if (cfg.polish) {
    cl::VecX wp;
    if (detail::fa_polish(f, s, rho, wp)) {
      const detail::FaViolation vp = detail::fa_violation(f, wp);
      // Acceptance. The polished book must be feasible to the per-row tolerance on EVERY
      // check (the sums included — no (M + 1) slack), and its objective must not exceed the
      // ADMM book's by more than what the ADMM book's own infeasibility can explain: by
      // first-order sensitivity the optimum is ≤ f(w_admm) + ‖y*‖∞·Σ violations, with the
      // ADMM dual (×2) standing in for y*. A polish on a wrong (over-constrained) active
      // set exceeds that bound and is rejected; the ADMM book is kept.
      const atx::f64 fa = detail::fa_objective(f, w);
      const atx::f64 fp = detail::fa_objective(f, wp);
      const atx::f64 slack = 1e-9 + 1e-7 * std::fabs(fa) +
                             2.0 * detail::fa_dual_inf(f, s.yb, s.yd, s.yt) * viol.total;
      if (vp.row <= cfg.feas_tol && vp.sums <= cfg.feas_tol && fp <= fa + slack) {
        w = std::move(wp);
        viol = vp;
        out.polished = true;
      }
    }
  }
  out.prim_res = std::max(0.0, std::max(viol.row, viol.sums));
  if (!detail::fa_gate_ok(f, viol, cfg.feas_tol)) {
    std::ostringstream msg;
    msg.precision(17);
    msg << "solve_factor_admm: book violates ";
    if (viol.row > cfg.feas_tol) {
      msg << (viol.which >= 0 ? "constraint row " + std::to_string(viol.which) : std::string("a box bound"))
          << " by " << viol.row;
    } else {
      msg << "an L1 budget by " << viol.sums;
    }
    msg << " tolerance=" << cfg.feas_tol << " after " << done
        << " iterations — the set may be infeasible OR the iteration cap is too low";
    return co::Err(co::ErrorCode::InvalidArgument, msg.str());
  }
  out.w.assign(w.data(), w.data() + w.size());
  out.dual.assign(ny, 0.0);
  for (atx::usize i = 0; i < f.m; ++i) {
    out.dual[i] = s.yb[static_cast<Eigen::Index>(i)];
  }
  for (Eigen::Index k = 0; k < rd; ++k) {
    out.dual[f.m + static_cast<atx::usize>(k)] = s.yd[k] * f.e[static_cast<atx::usize>(k)];
  }
  if (f.turn) {
    for (atx::usize i = 0; i < f.m; ++i) {
      out.dual[f.m + static_cast<atx::usize>(rd) + i] = s.yt[static_cast<Eigen::Index>(i)];
    }
  }
  return co::Ok(std::move(out));
}

} // namespace atx::engine::risk
