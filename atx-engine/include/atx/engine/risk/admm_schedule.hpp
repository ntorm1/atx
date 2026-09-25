#pragma once

// atx::engine::risk — admm_schedule: the DETERMINISTIC adaptive-ρ schedule and warm-start
// descriptor for ConstrainedQpSolver (Lane 6).
//
// ===========================================================================
//  Why a schedule (and why it stays deterministic)
// ===========================================================================
//  A fixed ρ is the single largest accuracy lever left in the fixed-iteration ADMM: the
//  factor-definition rows y − Xᵀw = 0 are equalities whose duals converge slowly at ρ = 1,
//  and problems whose primal/dual residuals are badly balanced stall. OSQP (Stellato et al.
//  2020, §5.2) fixes both with (a) a per-row ρ that is 10³× larger on equality rows and
//  (b) an adaptive ρ ← ρ·sqrt((‖r_p‖/max(‖Ax‖,‖z‖)) / (‖r_d‖/max(‖Px‖,‖Aᵀy‖,‖q‖))).
//
//  OSQP adapts on a WALL-CLOCK fraction of the factorization time, which is not
//  reproducible. Here the adaptation happens only at FIXED iteration indices
//  (`refactor_at`), and the new ρ is snapped to a power of two, so the chosen ρ sequence is
//  a pure function of the inputs and tiny floating-point wobble in the residual ratio
//  cannot flip the refactor decision near a boundary. The KKT pattern never changes (only
//  the −1/ρ diagonal), so a refactor is ONE numeric LDLᵀ over the cached symbolic phase.
//
//  The optional early exit is also deterministic: residuals are tested only every
//  `check_every` iterations against fixed tolerances, so the exit iteration is a pure
//  function of the inputs (same inputs ⇒ same iteration count ⇒ byte-identical book).

#include <array>   // std::array (refactor points)
#include <cmath>   // std::sqrt, std::log2, std::exp2, std::round, std::isfinite
#include <span>    // std::span (warm-start)

#include "atx/core/types.hpp" // f64, usize

namespace atx::engine::risk {

struct AdmmSchedule {
  // Iteration indices (1-based: "after this many iterations") at which ρ may adapt.
  // Entries larger than cfg.iters are never reached; zero entries are ignored.
  std::array<atx::usize, 3> refactor_at{25, 75, 150};
  bool pow2_round = true;       // snap every adapted ρ to 2^round(log2 ρ)
  atx::f64 rho_min = 1e-6;      // clamp for the adapted ρ
  atx::f64 rho_max = 1e6;
  atx::f64 eq_rho_scale = 1e3;  // ρ multiplier on equality rows (l == u), OSQP ρ_eq
  atx::f64 adapt_ratio = 5.0;   // refactor only if ρ_new/ρ ≥ this or ≤ 1/this
  atx::f64 relax_alpha = 1.6;   // ADMM over-relaxation α ∈ (0, 2) (OSQP default 1.6)
  bool early_exit = false;      // deterministic residual exit at `check_every` multiples
  atx::usize check_every = 25;
  atx::f64 eps_abs = 1e-9;
  atx::f64 eps_rel = 1e-9;
};

// Warm start for the next solve: the previous QpResult's full augmented primal/dual
// (x_full / y_full, ORIGINAL units). Non-owning; must outlive the solve call.
struct WarmStart {
  std::span<const atx::f64> x0;
  std::span<const atx::f64> y0;
  // Optional: the previous scheduled solve's cert.rho_final. > 0 ⇒ the scheduled ADMM
  // starts from it instead of cfg.rho (the adapted ρ carries over day to day, so a warm
  // solve does not re-spend its first refactor points re-discovering it). 0 ⇒ cfg.rho.
  // Ignored by the unscheduled path.
  atx::f64 rho = 0.0;
};

// ∞-norm residual summary the ρ adaptation and the early exit consume (scaled units).
struct AdmmResidualNorms {
  atx::f64 prim = 0.0;     // ‖Ãx − z‖∞
  atx::f64 dual = 0.0;     // ‖Px + q + Ãᵀy‖∞
  atx::f64 ax = 0.0;       // ‖Ãx‖∞
  atx::f64 z = 0.0;        // ‖z‖∞
  atx::f64 px = 0.0;       // ‖Px‖∞
  atx::f64 aty = 0.0;      // ‖Ãᵀy‖∞
  atx::f64 q = 0.0;        // ‖q‖∞
};

// 2^round(log2 ρ) for finite ρ > 0; returns ρ unchanged otherwise.
[[nodiscard]] inline atx::f64 round_pow2(atx::f64 rho) noexcept {
  if (!(rho > 0.0) || !std::isfinite(rho)) {
    return rho;
  }
  return std::exp2(std::round(std::log2(rho)));
}

// The OSQP residual-balancing update, clamped to [rho_min, rho_max] and optionally
// snapped to a power of two. A zero/degenerate residual leaves ρ unchanged.
[[nodiscard]] inline atx::f64 adapt_rho(atx::f64 rho, const AdmmResidualNorms &r,
                                        const AdmmSchedule &s) noexcept {
  constexpr atx::f64 kTiny = 1e-30;
  const atx::f64 prim_scale = (r.ax > r.z ? r.ax : r.z);
  atx::f64 dual_scale = (r.px > r.aty ? r.px : r.aty);
  dual_scale = (dual_scale > r.q ? dual_scale : r.q);
  const atx::f64 num = r.prim / (prim_scale + kTiny);
  const atx::f64 den = r.dual / (dual_scale + kTiny);
  if (!(num > 0.0) || !(den > 0.0) || !std::isfinite(num) || !std::isfinite(den)) {
    return rho;
  }
  atx::f64 next = rho * std::sqrt(num / den);
  next = next < s.rho_min ? s.rho_min : (next > s.rho_max ? s.rho_max : next);
  return s.pow2_round ? round_pow2(next) : next;
}

// True iff `it_done` (iterations completed) is one of the schedule's refactor points.
[[nodiscard]] inline bool is_refactor_point(const AdmmSchedule &s, atx::usize it_done) noexcept {
  for (const atx::usize k : s.refactor_at) {
    if (k != 0U && k == it_done) {
      return true;
    }
  }
  return false;
}

// OSQP termination test on the residual summary (absolute + relative tolerances).
[[nodiscard]] inline bool residuals_converged(const AdmmResidualNorms &r,
                                              const AdmmSchedule &s) noexcept {
  const atx::f64 prim_scale = (r.ax > r.z ? r.ax : r.z);
  atx::f64 dual_scale = (r.px > r.aty ? r.px : r.aty);
  dual_scale = (dual_scale > r.q ? dual_scale : r.q);
  return r.prim <= s.eps_abs + s.eps_rel * prim_scale &&
         r.dual <= s.eps_abs + s.eps_rel * dual_scale;
}

} // namespace atx::engine::risk
