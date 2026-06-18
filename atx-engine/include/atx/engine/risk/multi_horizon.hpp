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

#include <algorithm>  // std::min
#include <cmath>      // std::fabs, std::isnan
#include <functional> // std::function (sources_at / model_at callbacks)
#include <limits>     // std::numeric_limits (NaN aim seed)
#include <span>       // std::span
#include <utility>    // std::move, std::pair
#include <vector>     // std::vector (result books + trajectory)

#include "atx/core/error.hpp" // Result, Ok, Err, ATX_TRY
#include "atx/core/types.hpp" // f64, usize

#include "atx/engine/risk/constraints.hpp"  // ConstraintSet, MaterializedConstraints
#include "atx/engine/risk/factor_model.hpp" // FactorModel
#include "atx/engine/risk/garleanu_pedersen.hpp" // gp_aim_and_value (GP closed form + value matrix, S8.7)
#include "atx/engine/risk/horizon.hpp"      // SignalHorizon, HorizonForecast, forecast_trajectory
#include "atx/engine/risk/multi_period.hpp" // RebalanceSchedule, book::CostInputs
#include "atx/engine/risk/optimizer.hpp" // OptimizerConfig, PortfolioOptimizer (minimal dispatch)
#include "atx/engine/risk/qp_solver.hpp" // QpConfig, QpProblem, ConstrainedQpSolver (aug dispatch)

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
  // contract and the boundary pin (R7). `[[nodiscard]] const`.
  [[nodiscard]] atx::core::Result<MultiHorizonResult>
  run(const RebalanceSchedule &sched, const std::function<HorizonSources(atx::usize s)> &sources_at,
      const std::function<const FactorModel &(atx::usize s)> &model_at,
      const book::CostInputs &cost) const {
    namespace co = atx::core;
    // --- validate at the boundary -------------------------------------------
    if (cfg.trade_rate <= 0.0 || cfg.trade_rate > 1.0) {
      return co::Err(co::ErrorCode::InvalidArgument,
                     "MultiHorizonOptimizer::run: trade_rate must be in (0, 1]");
    }
    // stacked_mpc dispatch (S8.7): true ⇒ the TRUE O(N·H) joint multi-period QP over the
    // whole trajectory (benched, not the default); false ⇒ the shipped GP aim-collapse +
    // cost-to-go fold. The schedule walk below is shared; only the per-period inner solve
    // toward the aim differs (solve_toward_aim vs solve_stacked_mpc), selected per period.

    MultiHorizonResult out;
    out.books.reserve(sched.periods.size());
    out.turnover.reserve(sched.periods.size());
    out.cost_bps.reserve(sched.periods.size());

    std::vector<atx::f64> w_prev; // EMPTY at s=0 ⇒ a flat all-zero previous book.
    for (atx::usize s = 0; s < sched.periods.size(); ++s) {
      const atx::usize pit = sched.periods[s];
      const FactorModel &V = model_at(pit);
      const atx::usize m = V.n_instruments();

      // (1) trajectory → (2) GP aim alpha ᾱ = A_xf f_t (length M; NaN names preserved).
      // ᾱ is the decay-weighted return-space aim (the horizon-average of the decayed
      // trajectory rows; a persistent source keeps more of its α_t — Eq. 15's persistence
      // weighting). It is the q = −ᾱ linear term and the A_xf f_t of the GP closed form.
      const HorizonSources hs = sources_at(pit);
      ATX_TRY(HorizonForecast traj,
              forecast_trajectory(std::span<const HorizonSource>(hs.pairs), m, cfg.horizon));
      const std::vector<atx::f64> aim = gp_aim(traj, m);

      // (3) inner solve toward the aim, with the GP cost-to-go (scalar-Λ reduction, S8.7):
      //   * stacked_mpc == false (shipped): solve_toward_aim toward ᾱ with the cost-to-go
      //     tail folded into the QP objective (the MPC trick; A_xx = 2λV ⇒ q = −ᾱ, P = 2λV).
      //   * stacked_mpc == true (benched): the geometric horizon-blend stand-in (NOT a true
      //     joint O(N·H) QP — that is the recorded lift, sprint-1 §0.6).
      ATX_TRY(std::vector<atx::f64> target,
              cfg.stacked_mpc ? solve_stacked_mpc(traj, V, w_prev, cost)
                              : solve_toward_aim(aim, V, w_prev, cost));

      // (4) first-move execution — GP partial step + accounting (S7 walk, verbatim).
      std::vector<atx::f64> book = blend_toward(w_prev, target, cfg.trade_rate);
      out.turnover.push_back(l1_diff(book, w_prev));
      out.cost_bps.push_back(out.turnover.back() * cost.round_trip_cost_bps);
      out.books.push_back(book);
      w_prev = std::move(book);
    }
    return co::Ok(std::move(out));
  }

  // GP aim alpha = the horizon AVERAGE of the trajectory rows (D9). NaN-aware: a name
  // finite at ≥1 horizon averages over its finite contributions; a name NaN at EVERY
  // horizon stays NaN (never coerced to 0). Averaging normalizes scale so the
  // identity/H=1 single-source case yields aim == α_t EXACTLY (the byte pin). STATIC +
  // [[nodiscard]] so the unit test can exercise it directly.
  [[nodiscard]] static std::vector<atx::f64> gp_aim(const HorizonForecast &traj, atx::usize m) {
    std::vector<atx::f64> aim(m, std::numeric_limits<atx::f64>::quiet_NaN());
    const atx::usize rows = traj.alpha.size(); // == H+1
    // Order-fixed reduction: name i ascending, then horizon h ascending (R1).
    for (atx::usize i = 0; i < m; ++i) {
      atx::f64 acc = 0.0;
      atx::usize n_finite = 0;
      for (atx::usize h = 0; h < rows; ++h) {
        const atx::f64 a = traj.alpha[h][i];
        if (std::isnan(a)) {
          continue; // no opinion at this horizon — not added (mirrors S1-3 NaN policy)
        }
        acc += a;
        ++n_finite;
      }
      // PRESERVE no-opinion: all-horizon-NaN name stays NaN. Else the horizon average.
      if (n_finite > 0) {
        aim[i] = acc / static_cast<atx::f64>(n_finite);
      }
    }
    return aim;
  }

private:
  // -------------------------------------------------------------------------
  //  Inner solve toward the aim alpha — the §0.5 / D7 DISPATCH (MANDATORY for R7).
  // -------------------------------------------------------------------------
  // MINIMAL constraints (no fexp/grp/beta/turn) ⇒ reuse PortfolioOptimizer::solve,
  // configured IDENTICALLY to MultiPeriodOptimizer's inner oc (so the boundary pin is
  // byte-identical). AUGMENTED ⇒ materialize the S1-1 ConstraintSet and run the S1-2 QP.
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve_toward_aim(const std::vector<atx::f64> &aim, const FactorModel &V,
                   std::span<const atx::f64> w_prev, const book::CostInputs &cost) const {
    if (is_minimal_constraint_set()) {
      return solve_minimal(aim, V, w_prev, cost);
    }
    return solve_augmented(aim, V, w_prev);
  }

  // A constraint set is MINIMAL when it carries ONLY GrossNet (+ optional PositionCap):
  // exactly the algebra PortfolioOptimizer expresses natively (dollar-neutral + gross +
  // per-name cap). Any factor/group/beta/turnover row needs the augmented QP.
  [[nodiscard]] bool is_minimal_constraint_set() const noexcept {
    return !cfg.constraints.fexp && !cfg.constraints.grp && !cfg.constraints.beta &&
           !cfg.constraints.turn;
  }

  // Minimal dispatch: build the SAME OptimizerConfig MultiPeriodOptimizer builds (R7).
  // name_cap defaults to gross_leverage when no PositionCap is set so the cap can never
  // bind (PortfolioOptimizer skips the clip when cap ≥ gross). κ ← cost.kappa and the
  // gross is capacity-clipped — IDENTICAL to the S7 driver's inner oc construction.
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve_minimal(const std::vector<atx::f64> &aim, const FactorModel &V,
                std::span<const atx::f64> w_prev, const book::CostInputs &cost) const {
    OptimizerConfig oc;
    oc.risk_aversion = cfg.risk_aversion;
    oc.gross_leverage = cfg.constraints.gross.gross_leverage;
    oc.dollar_neutral = cfg.constraints.gross.dollar_neutral;
    oc.name_cap = cfg.constraints.pos ? cfg.constraints.pos->name_cap
                                      : cfg.constraints.gross.gross_leverage; // can't bind
    oc.max_iters = cfg.prox_max_iters;
    oc.turnover_penalty = cost.kappa; // mirror MultiPeriodOptimizer (κ ← calibrated cost)
    if (cfg.capacity_bound_gross) {
      oc.gross_leverage = std::min(oc.gross_leverage, cost.capacity_gross);
    }
    const PortfolioOptimizer opt{oc};
    return opt.solve(std::span<const atx::f64>(aim), V, w_prev);
  }

  // Augmented dispatch: materialize the S1-1 constraints and solve the S1-2 QP toward
  // the aim with the GP Riccati COST-TO-GO TAIL folded in (S8.7, the MPC trick). The QP
  // minimizes ½wᵀPw + qᵀw. GP's value function appends −½(w−aim_pos)ᵀA_xx(w−aim_pos) to
  // the maximize objective ⇒ +½(w−aim_pos)ᵀA_xx(w−aim_pos) in our minimize QP. With the
  // scalar-Λ value matrix A_xx = 2λV (the curvature build_augmented already emits as
  // P = blkdiag(2λD,2λF,0)) this expands to ½wᵀ(2λV)w − (A_xx·aim_pos)ᵀw + const, and the
  // documented GP identity A_xx·aim_pos == ᾱ (gp_aim_and_value: aim_pos = (2λV)⁻¹ᾱ) makes
  // the cost-to-go LINEAR term exactly q = −ᾱ — folding into q, with P UNCHANGED at 2λV
  // (R5: no new factorization; the Riccati "solve" is the cached factor capacitance, only
  // applied). So the fold is q = −ᾱ, matching the existing 2λ scaling. We route q through
  // gp_aim_and_value so the value-function structure is EXPLICIT and the A_xx·aim_pos==ᾱ
  // identity is computed, not assumed; the resulting q (= −ᾱ) is byte-identical to the
  // pre-S8.7 q = −aim, so the boundary pin / R3 augmented books are untouched (R10).
  //
  // The QP has NO NaN-exclusion path, so a NaN aim name (no opinion) maps to a 0 linear
  // coefficient — it carries no return tilt but the constraints still bound it. (The
  // materialized GrossNet.gross_leverage carries the gross L1 budget; the capacity-clip is
  // the minimal-dispatch concern — the augmented path honors whatever gross the caller set
  // in cfg.constraints, mirroring the S1-2 QP contract.)
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve_augmented(const std::vector<atx::f64> &aim, const FactorModel &V,
                  std::span<const atx::f64> w_prev) const {
    const atx::usize m = V.n_instruments();
    ATX_TRY(MaterializedConstraints C, cfg.constraints.materialize(V.exposures(), w_prev, m));

    // GP cost-to-go fold: q = −ᾱ (the value-function LINEAR term; A_xx = 2λV stays in P).
    // gp_aim_and_value echoes ᾱ on alpha_bar (and computes aim_pos = (2λV)⁻¹ᾱ, the
    // closed-form/oracle target — unused on the constrained path but the witness that the
    // A_xx·aim_pos == ᾱ identity holds). NaN ᾱ ⇒ 0 coefficient (no-opinion name).
    ATX_TRY(GpAimValue gp, gp_aim_and_value(std::span<const atx::f64>(aim), V, cfg.risk_aversion));
    std::vector<atx::f64> q(m, 0.0);
    for (atx::usize i = 0; i < m; ++i) {
      q[i] = std::isnan(gp.alpha_bar[i]) ? 0.0 : -gp.alpha_bar[i]; // q = −ᾱ (NaN ⇒ 0)
    }
    const ConstrainedQpSolver solver{cfg.qp};
    const QpProblem prob{V, cfg.risk_aversion, std::span<const atx::f64>(q), C};
    return solver.solve(prob);
  }

  // -------------------------------------------------------------------------
  //  solve_stacked_mpc (S8.7) — the GEOMETRIC HORIZON-BLEND, a benched alternative to the
  //  uniform-average aim-collapse (NOT the default).
  //
  //  HONEST SCOPE: this is NOT a true O(N·H) joint stacked-MPC QP. It does NOT introduce
  //  stacked {w_0..w_H} decision variables, inter-period turnover-coupling rows, or a joint
  //  constraint surface. It is a single-period dispatch solve toward a GEOMETRICALLY
  //  horizon-weighted return-space aim — the tell is the linearity collapse below (there is
  //  no joint coupling to break that linearity). The TRUE O(N·H) joint stacked-MPC QP (the
  //  full {w_0..w_H} program with inter-period coupling) remains the RECORDED LIFT
  //  (sprint-1 plan §0.6); this benched blend is the shipped stand-in.
  //
  //  The aim-collapse path solves ONE single-period QP toward the decay-weighted aim ᾱ
  //  (the UNIFORM horizon-AVERAGE of the trajectory rows). This path instead weights the
  //  per-horizon forecasts traj.alpha[h] by the GP geometric trade-rate weights
  //  ω_h ∝ (1−a)^h (a = trade_rate ∈ (0,1]): a slow (full-discount) step keeps far-horizon
  //  foresight; a == 1 (full step) ⇒ ω_0 == 1, ω_{h>0} == 0, the myopic period-0 forecast.
  //  We blend in RETURN space — ᾱ_stacked = Σ_h ω_h alpha[h] (per-name re-normalized by the
  //  SEEN weight, NaN-aware) — then drive ᾱ_stacked through the SAME dispatch (minimal fast
  //  path OR augmented QP), which applies the (2λV)⁻¹ Markowitz map. By linearity of
  //  (2λV)⁻¹ this EQUALS blending the per-horizon position targets m_h = (2λV)⁻¹alpha[h]
  //  (Σω_h m_h = (2λV)⁻¹ Σω_h alpha[h]) — that linearity is precisely why this is a
  //  single-aim blend and NOT a joint QP. DISTINCT from the aim-collapse (geometric vs
  //  uniform weights) — yet on a CONSTANT trajectory (identity decay: every alpha[h] == α_t)
  //  BOTH weightings give ᾱ_stacked == α_t, so this path COINCIDES with the aim-collapse
  //  (the documented agreement case). The constraint surface and the first-move/threading
  //  are identical to the shipped path.
  //  Deterministic (R1): order-fixed geometric weights + order-fixed per-name reduction +
  //  the cached-Cholesky V⁻¹ in the dispatch. (The blend is in return space, so this path
  //  is well-defined at λ == 0 too — the dispatch's λ=0 pure-alpha branch then applies.)
  // -------------------------------------------------------------------------
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve_stacked_mpc(const HorizonForecast &traj, const FactorModel &V,
                    std::span<const atx::f64> w_prev, const book::CostInputs &cost) const {
    const atx::usize m = V.n_instruments();
    const atx::usize rows = traj.alpha.size(); // H+1

    // Per-horizon return-space targets blended by the GP geometric trade-rate weights
    // ω_h ∝ (1−a)^h (normalized). a == 1 (full step) ⇒ ω_0 == 1, ω_{h>0} == 0 ⇒ the
    // stacked aim is the myopic period-0 forecast (a full-step GP holds nothing back).
    const atx::f64 a = cfg.trade_rate;
    std::vector<atx::f64> weight(rows, 0.0);
    atx::f64 pw = 1.0; // (1−a)^h, h ascending (order-fixed)
    for (atx::usize h = 0; h < rows; ++h) {
      weight[h] = pw;
      pw *= (1.0 - a);
    }
    // ω_0 == 1 always. Build the return-space stacked aim ᾱ_stacked =
    // Σ_h ω_h · alpha[h], NaN-aware per name (a name finite at ≥1 horizon blends its
    // finite contributions; an all-NaN name stays NaN — mirrors gp_aim's policy, R8).
    std::vector<atx::f64> alpha_stacked(m, std::numeric_limits<atx::f64>::quiet_NaN());
    for (atx::usize i = 0; i < m; ++i) {
      atx::f64 acc = 0.0;
      atx::f64 wseen = 0.0;
      bool any = false;
      for (atx::usize h = 0; h < rows; ++h) { // order-fixed: name i, then horizon h
        const atx::f64 ah = traj.alpha[h][i];
        if (std::isnan(ah)) {
          continue;
        }
        acc += weight[h] * ah;
        wseen += weight[h];
        any = true;
      }
      // Re-normalize by the SEEN weight so a name with holes is not down-scaled by the
      // missing horizons (and a constant trajectory collapses to α_t exactly: Σω·α/Σω).
      if (any && wseen > 0.0) {
        alpha_stacked[i] = acc / wseen;
      }
    }

    // Drive the SAME dispatch toward the stacked return-space aim — identical constraint
    // surface, first-move execution, and threading as the shipped aim-collapse path.
    return solve_toward_aim(alpha_stacked, V, w_prev, cost);
  }

  // -------------------------------------------------------------------------
  //  blend_toward / l1_diff — REPLICATED VERBATIM from multi_period.hpp.
  //
  //  The byte-identity of the boundary pin (R7) DEPENDS on these being the exact same
  //  arithmetic the S7 driver uses. In particular blend_toward SPECIAL-CASES rate==1.0
  //  to assign target[i] VERBATIM: the algebraic form 0.0 + 1.0·(target[i] − 0.0) would
  //  flush a −0.0 target weight to +0.0 (IEEE: 0.0 + −0.0 == +0.0), and the solver's
  //  dollar-neutral demean can emit −0.0, so the verbatim assignment is what preserves
  //  the signed zero and makes a full step byte-identical to the solver output.
  //  (Source of truth: atx/engine/risk/multi_period.hpp, MultiPeriodOptimizer.)
  // -------------------------------------------------------------------------
  [[nodiscard]] static std::vector<atx::f64>
  blend_toward(std::span<const atx::f64> w_prev, std::span<const atx::f64> target, atx::f64 rate) {
    std::vector<atx::f64> book(target.size(), 0.0);
    for (atx::usize i = 0; i < target.size(); ++i) {
      const atx::f64 p = i < w_prev.size() ? w_prev[i] : 0.0;
      book[i] = (rate == 1.0) ? target[i] : (p + rate * (target[i] - p));
    }
    return book;
  }

  // Σ_i |a[i] − b[i]| in ascending i; an empty b is the flat all-zero book ⇒ Σ_i |a[i]|.
  [[nodiscard]] static atx::f64 l1_diff(std::span<const atx::f64> a,
                                        std::span<const atx::f64> b) noexcept {
    atx::f64 s = 0.0;
    for (atx::usize i = 0; i < a.size(); ++i) {
      const atx::f64 bi = i < b.size() ? b[i] : 0.0;
      s += std::fabs(a[i] - bi);
    }
    return s;
  }
};

} // namespace atx::engine::risk
