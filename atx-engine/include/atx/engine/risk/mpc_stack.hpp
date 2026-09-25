#pragma once

// atx::engine::risk — mpc_stack: the TRUE stacked multi-period MPC QP (Lane 6), H ≤ 3.
//
// ===========================================================================
//  Problem (Gârleanu-Pedersen objective, finite horizon, constrained)
// ===========================================================================
//  Given the pre-trade book w_0 and per-period return forecasts α_1..α_H (length M each),
//  solve jointly for the H books w_1..w_H:
//
//    minimize  Σ_{h=1..H} [ ρ̄^h (−α_hᵀw_h + ½γ w_hᵀΣw_h) + ρ̄^{h−1} ½ Δ_hᵀΛΔ_h ]
//    subject to  l ≤ A w_h ≤ u,  Σ|w_h| ≤ G  (every period)
//
//  with Σ = XFXᵀ + D (the factor model, never densified), Λ = diag(λ_i) the quadratic
//  trade cost, Δ_h = w_h − w_{h−1} and ρ̄ = 1 − ρ. Only the FIRST move w_1 is traded
//  (receding horizon); w_2..w_H are the plan. With no constraints and
//  α_h = B(I − Φ)^{h−1} f this is EXACTLY the H-step Gârleanu-Pedersen backward recursion
//  (gp_riccati with cfg.horizon = H), which the tests pin.
//
// ===========================================================================
//  Formulation — one AugmentedQp through the standard ConstrainedQpSolver pipeline
// ===========================================================================
//  Columns (fixed order):  [ w_1..w_H (HM) | y_1..y_H (HK) | d_1..d_H (HM) | s_1..s_H (HM) ]
//    y_h = Xᵀw_h (factor block),  d_h = Δ_h (trade block),  s_h ≥ |w_h| (gross, iff G set)
//  P = blkdiag(ρ̄^h γD, ρ̄^h γF, ρ̄^{h−1}Λ, 0)   q = [−ρ̄^h α_h ; 0 ; 0 ; 0]
//  Rows (fixed order):
//    (0) per h: K rows   y_h − Xᵀw_h = 0
//    (1) per h: M rows   d_h − w_h + w_{h−1} = 0   (h = 1: d_1 − w_1 = −w_0)
//    (2) per h: R rows   l ≤ A w_h ≤ u
//    (3) per h (iff G ≥ 0): w − s ≤ 0, −w − s ≤ 0, s ≥ 0, Σs ≤ G
//  The solver returns the first M entries of x ⇒ QpResult::book == w_1; the full stacked
//  primal is QpResult::x_full. Every coupling term is sparse (the inter-period rows carry
//  2 nnz each), so the solve is O(H·(MK + nnz A)) per ADMM iteration.
//
//  Scope: linear rows + the gross L1 budget. Turnover budgets/penalties, the impact
//  surrogate and every cone (tracking, sector SOC, robust) are rejected with
//  Err(InvalidArgument) rather than silently dropped — the trade cost here IS Λ.
//
//  Determinism: a pure assembler (fixed triplet order) + the deterministic solver.

#include <cmath>   // std::isfinite
#include <cstddef> // std::ptrdiff_t
#include <span>    // std::span
#include <utility> // std::move
#include <vector>  // std::vector

#include <Eigen/SparseCore>

#include "atx/core/error.hpp"         // Result, Ok, Err
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/types.hpp"         // f64, usize

#include "atx/engine/risk/admm_schedule.hpp" // AdmmSchedule, WarmStart
#include "atx/engine/risk/constraints.hpp"   // MaterializedConstraints
#include "atx/engine/risk/factor_model.hpp"  // FactorModel
#include "atx/engine/risk/qp_augment.hpp"    // AugmentedQp, kAugInf
#include "atx/engine/risk/qp_solver.hpp"     // ConstrainedQpSolver, QpProblem, QpResult

namespace atx::engine::risk {

inline constexpr atx::usize kMpcMaxHorizon = 3;

// One stacked MPC instance. Spans are non-owning and must outlive the call.
struct MpcStackProblem {
  const FactorModel &V;                            // Σ = XFXᵀ + D (M names, K factors)
  atx::f64 gamma = 1.0;                            // γ > 0 (objective ½γ wᵀΣw)
  std::span<const atx::f64> impact_diag;           // Λ diagonal, length M, entries > 0
  std::span<const std::vector<atx::f64>> alpha;    // α_1..α_H, H ∈ [1, kMpcMaxHorizon]
  std::span<const atx::f64> w_prev;                // w_0 (length M); empty ⇒ the zero book
  const MaterializedConstraints &C;                // per-period rows (+ optional gross)
  atx::f64 rho = 0.0;                              // discount ρ ∈ [0, 1)
};

struct MpcStackResult {
  std::vector<std::vector<atx::f64>> path; // w_1..w_H (path[0] is the traded first move)
  QpResult qp;                             // the stacked solve (book == path[0])
};

namespace detail {

[[nodiscard]] inline atx::core::Status validate_mpc(const MpcStackProblem &p) {
  namespace co = atx::core;
  const atx::usize m = p.V.n_instruments();
  const atx::usize h = p.alpha.size();
  if (h == 0U || h > kMpcMaxHorizon) {
    return co::Err(co::ErrorCode::OutOfRange, "mpc_stack: horizon must be in [1, 3]");
  }
  if (!(p.gamma > 0.0) || !std::isfinite(p.gamma)) {
    return co::Err(co::ErrorCode::InvalidArgument, "mpc_stack: gamma must be finite and > 0");
  }
  if (!(p.rho >= 0.0 && p.rho < 1.0)) {
    return co::Err(co::ErrorCode::InvalidArgument, "mpc_stack: rho must lie in [0, 1)");
  }
  if (p.impact_diag.size() != m) {
    return co::Err(co::ErrorCode::InvalidArgument, "mpc_stack: impact_diag must be length M");
  }
  for (const atx::f64 l : p.impact_diag) {
    if (!(l > 0.0) || !std::isfinite(l)) {
      return co::Err(co::ErrorCode::InvalidArgument, "mpc_stack: impact_diag must be > 0");
    }
  }
  for (const std::vector<atx::f64> &a : p.alpha) {
    if (a.size() != m) {
      return co::Err(co::ErrorCode::InvalidArgument, "mpc_stack: each alpha must be length M");
    }
    for (const atx::f64 v : a) {
      if (!std::isfinite(v)) {
        return co::Err(co::ErrorCode::InvalidArgument, "mpc_stack: alpha must be finite");
      }
    }
  }
  if (!p.w_prev.empty() && p.w_prev.size() != m) {
    return co::Err(co::ErrorCode::InvalidArgument, "mpc_stack: w_prev must be empty or M");
  }
  const MaterializedConstraints &c = p.C;
  if (c.A.rows() > 0 && static_cast<atx::usize>(c.A.cols()) != m) {
    return co::Err(co::ErrorCode::InvalidArgument, "mpc_stack: A.cols() must equal M");
  }
  if (c.l.size() != c.A.rows() || c.u.size() != c.A.rows()) {
    return co::Err(co::ErrorCode::InvalidArgument, "mpc_stack: l/u must match A.rows()");
  }
  const bool cones = c.tracking.active || c.sector_risk.active ||
                     (c.robust.active && c.robust.kappa > 0.0);
  if (c.has_turnover || c.turnover_penalty > 0.0 || c.impact.active || cones) {
    return co::Err(co::ErrorCode::InvalidArgument,
                   "mpc_stack: only linear rows and the gross budget are supported "
                   "(turnover / impact / cone constraints are rejected, Λ is the trade cost)");
  }
  return co::Ok();
}

} // namespace detail

// Assemble the stacked AugmentedQp (see the header block). Precondition: validate_mpc ok.
[[nodiscard]] inline AugmentedQp build_mpc_stack(const MpcStackProblem &p) {
  namespace cl = atx::core::linalg;
  using Trip = Eigen::Triplet<atx::f64>;
  const atx::usize m = p.V.n_instruments();
  const atx::usize k = p.V.n_factors();
  const atx::usize hh = p.alpha.size();
  const atx::usize rr = static_cast<atx::usize>(p.C.A.rows());
  const bool gross = p.C.gross_l1_budget >= 0.0;
  const cl::MatX &X = p.V.exposures();
  const cl::VecX &D = p.V.specific_var();
  const cl::MatX &F = p.V.factor_cov();
  const atx::f64 rb = 1.0 - p.rho;

  AugmentedQp out;
  out.n_w = hh * m;
  out.n_y = hh * k;
  out.n_aux = hh * m + (gross ? hh * m : 0U);
  const atx::usize n = out.n_w + out.n_y + out.n_aux;
  const atx::usize y_off = hh * m;
  const atx::usize d_off = y_off + hh * k;
  const atx::usize s_off = d_off + hh * m;
  const auto wc = [&](atx::usize h, atx::usize i) { return static_cast<int>(h * m + i); };
  const auto yc = [&](atx::usize h, atx::usize a) { return static_cast<int>(y_off + h * k + a); };
  const auto dc = [&](atx::usize h, atx::usize i) { return static_cast<int>(d_off + h * m + i); };
  const auto sc = [&](atx::usize h, atx::usize i) { return static_cast<int>(s_off + h * m + i); };

  // Hessian + linear term, fixed order: every w diag, every y block, every d diag.
  std::vector<Trip> pt;
  pt.reserve(2U * hh * m + hh * k * k);
  out.q_aug = cl::VecX::Zero(static_cast<Eigen::Index>(n));
  atx::f64 disc = 1.0; // ρ̄^h, h = 1..H
  for (atx::usize h = 0; h < hh; ++h) {
    disc *= rb;
    for (atx::usize i = 0; i < m; ++i) {
      const auto ei = static_cast<Eigen::Index>(i);
      pt.emplace_back(wc(h, i), wc(h, i), disc * p.gamma * D[ei]);
      out.q_aug[wc(h, i)] = -disc * p.alpha[h][i];
    }
    for (atx::usize b = 0; b < k; ++b) {
      for (atx::usize a = 0; a < k; ++a) {
        pt.emplace_back(yc(h, a), yc(h, b),
                        disc * p.gamma *
                            F(static_cast<Eigen::Index>(a), static_cast<Eigen::Index>(b)));
      }
    }
    const atx::f64 cost_disc = disc / rb; // ρ̄^{h−1}
    for (atx::usize i = 0; i < m; ++i) {
      pt.emplace_back(dc(h, i), dc(h, i), cost_disc * p.impact_diag[i]);
    }
  }
  out.P.resize(static_cast<int>(n), static_cast<int>(n));
  out.P.setFromTriplets(pt.begin(), pt.end());

  const atx::usize rows =
      hh * (k + m + rr) + (gross ? hh * (3U * m + 1U) : 0U);
  out.l = cl::VecX::Zero(static_cast<Eigen::Index>(rows));
  out.u = cl::VecX::Zero(static_cast<Eigen::Index>(rows));
  std::vector<Trip> at;
  at.reserve(hh * (k * (m + 1U) + 2U * m + rr * m + (gross ? 7U * m : 0U)));
  atx::usize row = 0U;
  const auto bound = [&](atx::f64 lo, atx::f64 hi) {
    out.l[static_cast<Eigen::Index>(row)] = lo;
    out.u[static_cast<Eigen::Index>(row)] = hi;
    ++row;
  };
  for (atx::usize h = 0; h < hh; ++h) { // (0) y_h − Xᵀw_h = 0
    for (atx::usize a = 0; a < k; ++a) {
      for (atx::usize i = 0; i < m; ++i) {
        at.emplace_back(static_cast<int>(row), wc(h, i),
                        -X(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(a)));
      }
      at.emplace_back(static_cast<int>(row), yc(h, a), 1.0);
      bound(0.0, 0.0);
    }
  }
  for (atx::usize h = 0; h < hh; ++h) { // (1) d_h − w_h + w_{h−1} = 0
    for (atx::usize i = 0; i < m; ++i) {
      if (h > 0U) {
        at.emplace_back(static_cast<int>(row), wc(h - 1U, i), 1.0);
      }
      at.emplace_back(static_cast<int>(row), wc(h, i), -1.0);
      at.emplace_back(static_cast<int>(row), dc(h, i), 1.0);
      const atx::f64 rhs = (h == 0U && !p.w_prev.empty()) ? -p.w_prev[i] : 0.0;
      bound(rhs, rhs);
    }
  }
  for (atx::usize h = 0; h < hh; ++h) { // (2) l ≤ A w_h ≤ u
    for (atx::usize r = 0; r < rr; ++r) {
      const auto er = static_cast<Eigen::Index>(r);
      for (atx::usize j = 0; j < m; ++j) {
        const atx::f64 aij = p.C.A(er, static_cast<Eigen::Index>(j));
        if (aij != 0.0) {
          at.emplace_back(static_cast<int>(row), wc(h, j), aij);
        }
      }
      bound(p.C.l[er], p.C.u[er]);
    }
  }
  if (gross) { // (3) s_h ≥ |w_h|, Σ s_h ≤ G
    for (atx::usize h = 0; h < hh; ++h) {
      for (atx::usize i = 0; i < m; ++i) {
        at.emplace_back(static_cast<int>(row), wc(h, i), 1.0);
        at.emplace_back(static_cast<int>(row), sc(h, i), -1.0);
        bound(-kAugInf, 0.0);
      }
      for (atx::usize i = 0; i < m; ++i) {
        at.emplace_back(static_cast<int>(row), wc(h, i), -1.0);
        at.emplace_back(static_cast<int>(row), sc(h, i), -1.0);
        bound(-kAugInf, 0.0);
      }
      for (atx::usize i = 0; i < m; ++i) {
        at.emplace_back(static_cast<int>(row), sc(h, i), 1.0);
        bound(0.0, kAugInf);
      }
      for (atx::usize i = 0; i < m; ++i) {
        at.emplace_back(static_cast<int>(row), sc(h, i), 1.0);
      }
      bound(-kAugInf, p.C.gross_l1_budget);
    }
  }
  out.A_tilde.resize(static_cast<int>(rows), static_cast<int>(n));
  out.A_tilde.setFromTriplets(at.begin(), at.end());
  return out;
}

// Validate, assemble and solve the stacked MPC through `solver` (Ruiz → ADMM → polish →
// feasibility gate). `sched` (optional) selects the deterministic adaptive-ρ ADMM, which
// the many inter-period equality rows benefit from; `ws` warm-starts from a previous
// solve's x_full / y_full (same M, K, H and constraint layout).
[[nodiscard]] inline atx::core::Result<MpcStackResult>
solve_mpc_stack(const ConstrainedQpSolver &solver, const MpcStackProblem &p,
                const AdmmSchedule *sched = nullptr, const WarmStart *ws = nullptr) {
  ATX_TRY_VOID(detail::validate_mpc(p));
  const AugmentedQp aug = build_mpc_stack(p);
  const atx::usize m = p.V.n_instruments();
  const std::vector<atx::f64> q0(m, 0.0); // unused by solve_augmented_form (q lives in aug)
  const QpProblem qp{p.V, 0.0, std::span<const atx::f64>(q0), p.C};
  ATX_TRY(QpResult r, solver.solve_augmented_form(aug, qp, sched, ws));
  MpcStackResult out;
  out.path.resize(p.alpha.size());
  for (atx::usize h = 0; h < p.alpha.size(); ++h) {
    out.path[h].assign(r.x_full.begin() + static_cast<std::ptrdiff_t>(h * m),
                       r.x_full.begin() + static_cast<std::ptrdiff_t>((h + 1U) * m));
  }
  out.qp = std::move(r);
  return atx::core::Ok(std::move(out));
}

} // namespace atx::engine::risk
