#pragma once

// atx::engine::risk — MultiHorizonOptimizer: the constrained MULTI-HORIZON receding-
// horizon driver (P2-S1-4). The INTEGRATIVE CAPSTONE of sprint S1.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  It mirrors the as-built S7 MultiPeriodOptimizer schedule walk (multi_period.hpp)
//  EXACTLY, but replaces the single-period inner solve with the S1 stack:
//
//    1. forecast_trajectory (S1-3) — project the CURRENT cross-section α_t of each
//       signal source forward over a decay horizon into an (H+1)×M trajectory.
//    2. gp_aim (D9) — collapse the trajectory to a single Gârleanu-Pedersen AIM alpha
//       = the horizon AVERAGE of the trajectory rows (NaN-aware; §gp_aim below).
//    3. Inner solve toward the aim — DISPATCH (§Dispatch below):
//         * MINIMAL constraints (GrossNet + optional PositionCap, nothing else) ⇒ the
//           as-built PortfolioOptimizer::solve, configured IDENTICALLY to the S7 driver
//           (R7 — the boundary pin reduces this to MultiPeriodOptimizer byte-for-byte).
//         * AUGMENTED constraints (factor/group/beta/turnover present) ⇒ materialize the
//           S1-1 ConstraintSet and solve the S1-2 ConstrainedQpSolver with q = −aim.
//    4. First-move execution — Gârleanu-Pedersen partial-step toward the target, push
//       the realized book, thread w_prev forward, charge turnover/cost — EXACTLY as the
//       S7 driver does (blend_toward / l1_diff replicated verbatim below).
//
// ===========================================================================
//  The S7 boundary pin (R7) — the load-bearing invariant
// ===========================================================================
//  With H == 1 and ONE SignalHorizon::identity() source per period, the trajectory is
//  constant == α_t at both rows, so gp_aim's average is EXACTLY α_t (bit-identical:
//  (a+a)/2 == a and a/1 == a in IEEE-754 round-to-nearest). The minimal-constraint
//  dispatch then builds the SAME OptimizerConfig the S7 driver builds and calls the
//  SAME PortfolioOptimizer::solve — and because that solver is SCALE-INVARIANT in α and
//  the aim equals α_t EXACTLY (not merely proportionally), the whole book chain is
//  BYTE-IDENTICAL to MultiPeriodOptimizer over the same schedule/alpha/model/cost. The
//  blend_toward rate==1.0 verbatim-assign branch preserves the signed zero the solver's
//  dollar-neutral demean can emit, so the pin holds at the bit level (not just ≈).
//
// ===========================================================================
//  gp_aim — the horizon AVERAGE (D9), and WHY average not sum
// ===========================================================================
//  aim[i] = (1 / n_finite_i) · Σ_{h : traj.alpha[h][i] finite} traj.alpha[h][i], where
//  n_finite_i is the count of horizons at which name i has a finite forecast. A name
//  that is NaN at EVERY horizon (no source ever had an opinion) stays NaN — never
//  coerced to 0 — so the downstream optimizer's NaN→0/excluded path stays correct.
//  AVERAGING (not summing) is the D9 decision: it NORMALIZES the trajectory scale so the
//  degenerate identity/H=1 case collapses to aim == α_t EXACTLY (the byte-pin), while
//  persistence-weighting still emerges — decay is already baked into each traj.alpha[h],
//  so a slow-decay (persistent) source keeps more of its α_t across the horizon and thus
//  earns a larger horizon-average than a fast-decay source. (A SUM would inflate the aim
//  by ~H and break the bit-identity with α_t, defeating the boundary pin.)
//
// ===========================================================================
//  Dispatch — minimal vs augmented (§0.5 / D7, MANDATORY for R7)
// ===========================================================================
//  The minimal path exists so the as-built engine solver (PortfolioOptimizer) is reused
//  VERBATIM whenever the constraint set carries nothing it cannot express (it natively
//  does dollar-neutral + gross + per-name cap + κ-turnover). Only when a factor/group/
//  beta/turnover constraint is present do we fall to the S1-2 ADMM QP. The aug path maps
//  a NaN aim entry to 0 in q (the QP has NO NaN-exclusion path, unlike PortfolioOptimizer
//  — a NaN aim name simply has "no opinion", which for the linear term qᵀw means a 0
//  coefficient; the constraints still bound that name).
//
// ===========================================================================
//  Determinism + allocation
// ===========================================================================
//  NO map / clock / RNG. forecast_trajectory, gp_aim, blend_toward, l1_diff and both
//  inner solvers are order-fixed and fixed-iteration, so the whole book chain is
//  byte-deterministic (R1). Allocations are the result vectors + the per-period
//  trajectory / aim / target / book — at rebalance cadence, never on a hot tick path.
//
// ===========================================================================
//  GP cost-to-go (scalar-Λ reduction) + stacked-MPC stand-in (S8.7)
// ===========================================================================
//  The shipped (default) path is the GP AIM-COLLAPSE with the cost-to-go tail under the
//  SCALAR-Λ reduction (sprint-1 plan §0.6 / §0.4): ᾱ = the decay-weighted return-space aim
//  (gp_aim); the single-period QP carries the GP value-function tail
//  +½(w−aim_pos)ᵀA_xx(w−aim_pos) folded into P/q (the MPC trick). With Λ = λΣ the value
//  matrix is A_xx = 2λV (the PLAIN one-period Hessian — NO Riccati is solved), so the fold
//  is q = −ᾱ with P = 2λV UNCHANGED (garleanu_pedersen.hpp); it is byte-identical to the
//  pre-S8.7 augmented book and the boundary pin holds (R10). The unconstrained closed form
//  aim_pos = (2λV)⁻¹ᾱ is the no-solver fast path. (The full matrix-Riccati A_xx is the
//  recorded lift, not shipped.)
//
//  cfg.stacked_mpc == true requests the GEOMETRIC HORIZON-BLEND (S8.7): a single-period
//  dispatch solve toward the GP geometric-trade-rate-weighted blend of the per-horizon
//  forecasts. It is NOT a true O(N·H) joint stacked QP (no stacked {w_0..w_H} variables /
//  inter-period coupling — the true joint QP is the recorded lift, sprint-1 §0.6); it is a
//  real, benched STAND-IN — NO LONGER Err(NotImplemented) — but NOT the default. On a
//  constant (identity-decay) trajectory it COINCIDES with the aim-collapse byte-for-byte.

#include <functional> // std::function (sources_at / model_at callbacks)
#include <span>       // std::span
#include <utility>    // std::pair
#include <vector>     // std::vector (result books + trajectory)

#include "atx/core/error.hpp" // Result
#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/risk/constraints.hpp" // ConstraintSet (MultiHorizonConfig member)
#include "atx/engine/risk/fwd.hpp"         // FactorModel (fwd — passed by ref through the callbacks)
#include "atx/engine/risk/horizon.hpp"     // SignalHorizon, HorizonForecast (HorizonSource / gp_aim)
#include "atx/engine/risk/multi_period.hpp" // RebalanceSchedule, book::CostInputs (run() signature)
#include "atx/engine/risk/qp_solver.hpp"    // QpConfig (MultiHorizonConfig::qp member)
// S8.8a: the method BODIES live in src/risk/multi_horizon.cpp; the heavy dispatch
// includes (factor_model, garleanu_pedersen, optimizer) are pulled there, not here.

namespace atx::engine::risk {

// ===========================================================================
//  HorizonSources — the per-period signal pack: each source carries its CURRENT
//  cross-section α_t and the SignalHorizon governing its forward decay. This is the
//  exact pair type forecast_trajectory consumes (S1-3), bundled for the run() callback.
// ===========================================================================
using HorizonSource = std::pair<std::span<const atx::f64>, SignalHorizon>;
struct HorizonSources {
  std::vector<HorizonSource> pairs;
};

// ===========================================================================
//  MultiHorizonConfig — the driver knobs (pure configuration).
// ===========================================================================
struct MultiHorizonConfig {
  atx::f64 risk_aversion = 1.0; // λ (passed to both dispatch paths)
  ConstraintSet constraints{};  // the S1-1 algebra; a MINIMAL set ⇒ the S7 reduction (R7)
  QpConfig qp{};                // ADMM knobs — used ONLY on the augmented path
  atx::usize horizon = 1;       // H (forward lookahead periods); H=1 + identity ⇒ S7 boundary
  atx::f64 trade_rate = 1.0;    // Gârleanu-Pedersen partial step ∈ (0,1]; 1 ⇒ full step
  bool stacked_mpc = false;     // false ⇒ GP aim-collapse + cost-to-go (shipped); true ⇒ stacked O(N·H) path (benched)
  atx::usize prox_max_iters = 64;   // PortfolioOptimizer max_iters for the minimal dispatch
  bool capacity_bound_gross = true; // capacity-clip the gross on the dispatch path (mirror S7)
};

// ===========================================================================
//  MultiHorizonResult — the realized book chain + per-period turnover/cost (S7 shape).
// ===========================================================================
struct MultiHorizonResult {
  std::vector<std::vector<atx::f64>> books;
  std::vector<atx::f64> turnover;
  std::vector<atx::f64> cost_bps;
};

// ===========================================================================
//  MultiHorizonOptimizer — the constrained multi-horizon receding-horizon driver.
// ===========================================================================
class MultiHorizonOptimizer {
public:
  MultiHorizonConfig cfg;

  // Walk the ascending schedule, threading w_prev from the prior REALIZED book. For
  // each as-of period: build the horizon trajectory, collapse it to the GP aim alpha,
  // solve toward the aim (minimal-constraint dispatch or augmented QP), partial-step
  // toward the target, and charge turnover/cost. See the header block for the full
  // contract and the boundary pin (R7). `[[nodiscard]] const`. Body in the .cpp (S8.8a).
  [[nodiscard]] atx::core::Result<MultiHorizonResult>
  run(const RebalanceSchedule &sched, const std::function<HorizonSources(atx::usize s)> &sources_at,
      const std::function<const FactorModel &(atx::usize s)> &model_at,
      const book::CostInputs &cost) const;

  // GP aim alpha = the horizon AVERAGE of the trajectory rows (D9). NaN-aware: a name
  // finite at ≥1 horizon averages over its finite contributions; a name NaN at EVERY
  // horizon stays NaN (never coerced to 0). Averaging normalizes scale so the
  // identity/H=1 single-source case yields aim == α_t EXACTLY (the byte pin). STATIC +
  // [[nodiscard]] so the unit test can exercise it directly. Body in the .cpp (S8.8a).
  [[nodiscard]] static std::vector<atx::f64> gp_aim(const HorizonForecast &traj, atx::usize m);

private:
  // Inner solve toward the aim alpha — the §0.5 / D7 DISPATCH (MANDATORY for R7).
  // MINIMAL constraints ⇒ PortfolioOptimizer::solve (S7-identical oc); AUGMENTED ⇒ the
  // S1-2 QP with the GP cost-to-go fold (q = −ᾱ). Bodies in src/risk/multi_horizon.cpp.
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve_toward_aim(const std::vector<atx::f64> &aim, const FactorModel &V,
                   std::span<const atx::f64> w_prev, const book::CostInputs &cost) const;

  // A constraint set is MINIMAL when it carries ONLY GrossNet (+ optional PositionCap).
  [[nodiscard]] bool is_minimal_constraint_set() const noexcept;

  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve_minimal(const std::vector<atx::f64> &aim, const FactorModel &V,
                std::span<const atx::f64> w_prev, const book::CostInputs &cost) const;

  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve_augmented(const std::vector<atx::f64> &aim, const FactorModel &V,
                  std::span<const atx::f64> w_prev) const;

  // solve_stacked_mpc (S8.7) — the GEOMETRIC HORIZON-BLEND benched alternative (NOT a
  // true joint O(N·H) QP; the recorded lift). A constant trajectory ⇒ coincides with the
  // uniform aim-collapse byte-for-byte.
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve_stacked_mpc(const HorizonForecast &traj, const FactorModel &V,
                    std::span<const atx::f64> w_prev, const book::CostInputs &cost) const;

  // blend_toward / l1_diff — REPLICATED VERBATIM from multi_period.hpp (the boundary-pin
  // arithmetic; blend_toward special-cases rate==1.0 to preserve a signed −0.0 target).
  [[nodiscard]] static std::vector<atx::f64>
  blend_toward(std::span<const atx::f64> w_prev, std::span<const atx::f64> target, atx::f64 rate);
  [[nodiscard]] static atx::f64 l1_diff(std::span<const atx::f64> a,
                                        std::span<const atx::f64> b) noexcept;
};

} // namespace atx::engine::risk

