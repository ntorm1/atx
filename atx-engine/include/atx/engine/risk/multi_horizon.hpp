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
//  Recorded lift (stacked_mpc)
// ===========================================================================
//  cfg.stacked_mpc == true requests a TRUE stacked multi-period MPC QP (optimize the
//  whole trajectory jointly, not the aim-collapse). That is an atx-core lift and is NOT
//  implemented here ⇒ Err(NotImplemented). The shipped path is the GP aim-collapse.

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
  bool stacked_mpc = false;     // false ⇒ GP aim-collapse (shipped); true ⇒ Err(NotImplemented)
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
    if (cfg.stacked_mpc) {
      return co::Err(co::ErrorCode::NotImplemented,
                     "MultiHorizonOptimizer::run: stacked MPC QP is a recorded atx-core lift");
    }

    MultiHorizonResult out;
    out.books.reserve(sched.periods.size());
    out.turnover.reserve(sched.periods.size());
    out.cost_bps.reserve(sched.periods.size());

    std::vector<atx::f64> w_prev; // EMPTY at s=0 ⇒ a flat all-zero previous book.
    for (atx::usize s = 0; s < sched.periods.size(); ++s) {
      const atx::usize pit = sched.periods[s];
      const FactorModel &V = model_at(pit);
      const atx::usize m = V.n_instruments();

      // (1) trajectory → (2) GP aim alpha (length M; NaN names preserved).
      const HorizonSources hs = sources_at(pit);
      ATX_TRY(HorizonForecast traj,
              forecast_trajectory(std::span<const HorizonSource>(hs.pairs), m, cfg.horizon));
      const std::vector<atx::f64> aim = gp_aim(traj, m);

      // (3) inner solve toward the aim — dispatch on the constraint set.
      ATX_TRY(std::vector<atx::f64> target, solve_toward_aim(aim, V, w_prev, cost));

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
  [[nodiscard]] static std::vector<atx::f64> gp_aim(const HorizonForecast &traj, atx::usize M) {
    std::vector<atx::f64> aim(M, std::numeric_limits<atx::f64>::quiet_NaN());
    const atx::usize rows = traj.alpha.size(); // == H+1
    // Order-fixed reduction: name i ascending, then horizon h ascending (R1).
    for (atx::usize i = 0; i < M; ++i) {
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
  // the aim. The QP minimizes ½wᵀ(2λV)w + qᵀw, so q = −aim drives the constrained
  // Markowitz book. The QP has NO NaN-exclusion path, so a NaN aim name (no opinion)
  // maps to a 0 linear coefficient — it carries no return tilt but the constraints still
  // bound it. (The materialized GrossNet.gross_leverage carries the gross L1 budget; the
  // capacity-clip is the minimal-dispatch concern — the augmented path honors whatever
  // gross the caller set in cfg.constraints, mirroring the S1-2 QP contract.)
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  solve_augmented(const std::vector<atx::f64> &aim, const FactorModel &V,
                  std::span<const atx::f64> w_prev) const {
    namespace co = atx::core;
    const atx::usize m = V.n_instruments();
    ATX_TRY(MaterializedConstraints C, cfg.constraints.materialize(V.exposures(), w_prev, m));

    std::vector<atx::f64> q(m, 0.0);
    for (atx::usize i = 0; i < m; ++i) {
      q[i] = std::isnan(aim[i]) ? 0.0 : -aim[i]; // NaN aim ⇒ no linear tilt (0 coefficient)
    }
    const ConstrainedQpSolver solver{cfg.qp};
    const QpProblem prob{V, cfg.risk_aversion, std::span<const atx::f64>(q), C};
    return solver.solve(prob);
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
