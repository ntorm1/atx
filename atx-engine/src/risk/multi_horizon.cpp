#include "atx/engine/risk/multi_horizon.hpp"

// atx::engine::risk — MultiHorizonOptimizer bodies (S8.7 receding-horizon driver).
// S8.8a header/source split: the schedule walk, the GP aim-collapse + cost-to-go fold,
// the stacked-MPC geometric blend, and the boundary-pin blend/turnover arithmetic live
// here so the 7 dependents of multi_horizon.hpp no longer re-parse them (and the heavy
// dispatch includes — factor_model, garleanu_pedersen, optimizer — stay in this TU).
// PURE refactor — byte-identical (R10): the math, reduction order, and the rate==1.0
// signed-zero special case are unchanged.

#include <algorithm> // std::min
#include <cmath>     // std::fabs, std::isnan
#include <limits>    // std::numeric_limits (NaN aim seed)
#include <utility>   // std::move

#include "atx/engine/risk/factor_model.hpp"      // FactorModel (exposures / n_instruments)
#include "atx/engine/risk/garleanu_pedersen.hpp" // gp_aim_and_value (GP closed form, S8.7)
#include "atx/engine/risk/optimizer.hpp" // OptimizerConfig, PortfolioOptimizer (minimal dispatch)

namespace atx::engine::risk {

atx::core::Result<MultiHorizonResult>
MultiHorizonOptimizer::run(const RebalanceSchedule &sched,
                           const std::function<HorizonSources(atx::usize s)> &sources_at,
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
// identity/H=1 single-source case yields aim == α_t EXACTLY (the byte pin).
std::vector<atx::f64> MultiHorizonOptimizer::gp_aim(const HorizonForecast &traj, atx::usize m) {
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

// MINIMAL constraints (no fexp/grp/beta/turn) ⇒ reuse PortfolioOptimizer::solve,
// configured IDENTICALLY to MultiPeriodOptimizer's inner oc (so the boundary pin is
// byte-identical). AUGMENTED ⇒ materialize the S1-1 ConstraintSet and run the S1-2 QP.
atx::core::Result<std::vector<atx::f64>>
MultiHorizonOptimizer::solve_toward_aim(const std::vector<atx::f64> &aim, const FactorModel &V,
                                        std::span<const atx::f64> w_prev,
                                        const book::CostInputs &cost) const {
  if (is_minimal_constraint_set()) {
    return solve_minimal(aim, V, w_prev, cost);
  }
  return solve_augmented(aim, V, w_prev);
}

// A constraint set is MINIMAL when it carries ONLY GrossNet (+ optional PositionCap):
// exactly the algebra PortfolioOptimizer expresses natively (dollar-neutral + gross +
// per-name cap). Any factor/group/beta/turnover row needs the augmented QP.
bool MultiHorizonOptimizer::is_minimal_constraint_set() const noexcept {
  return !cfg.constraints.fexp && !cfg.constraints.grp && !cfg.constraints.beta &&
         !cfg.constraints.turn;
}

// Minimal dispatch: build the SAME OptimizerConfig MultiPeriodOptimizer builds (R7).
// name_cap defaults to gross_leverage when no PositionCap is set so the cap can never
// bind (PortfolioOptimizer skips the clip when cap ≥ gross). κ ← cost.kappa and the
// gross is capacity-clipped — IDENTICAL to the S7 driver's inner oc construction.
atx::core::Result<std::vector<atx::f64>>
MultiHorizonOptimizer::solve_minimal(const std::vector<atx::f64> &aim, const FactorModel &V,
                                     std::span<const atx::f64> w_prev,
                                     const book::CostInputs &cost) const {
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

// Augmented dispatch: materialize the S1-1 constraints and solve the S1-2 QP toward the
// aim with the GP cost-to-go TAIL folded in (S8.7, the MPC trick). The QP minimizes
// ½wᵀPw + qᵀw; the scalar-Λ value matrix A_xx = 2λV makes the cost-to-go LINEAR term
// exactly q = −ᾱ (gp_aim_and_value computes the A_xx·aim_pos == ᾱ identity, so q = −ᾱ is
// byte-identical to the pre-S8.7 q = −aim). NaN ᾱ ⇒ 0 coefficient (no-opinion name).
atx::core::Result<std::vector<atx::f64>>
MultiHorizonOptimizer::solve_augmented(const std::vector<atx::f64> &aim, const FactorModel &V,
                                       std::span<const atx::f64> w_prev) const {
  const atx::usize m = V.n_instruments();
  ATX_TRY(MaterializedConstraints C, cfg.constraints.materialize(V.exposures(), w_prev, m));

  // GP cost-to-go fold: q = −ᾱ (the value-function LINEAR term; A_xx = 2λV stays in P).
  ATX_TRY(GpAimValue gp, gp_aim_and_value(std::span<const atx::f64>(aim), V, cfg.risk_aversion));
  std::vector<atx::f64> q(m, 0.0);
  for (atx::usize i = 0; i < m; ++i) {
    q[i] = std::isnan(gp.alpha_bar[i]) ? 0.0 : -gp.alpha_bar[i]; // q = −ᾱ (NaN ⇒ 0)
  }
  const ConstrainedQpSolver solver{cfg.qp};
  const QpProblem prob{V, cfg.risk_aversion, std::span<const atx::f64>(q), C};
  return solver.solve(prob);
}

// solve_stacked_mpc (S8.7) — the GEOMETRIC HORIZON-BLEND, a benched alternative to the
// uniform-average aim-collapse (NOT the default, NOT a true joint O(N·H) QP — the
// recorded lift). Blend the per-horizon forecasts by ω_h ∝ (1−a)^h (a = trade_rate),
// re-normalized per name by the SEEN weight, then drive the SAME dispatch. By linearity
// of (2λV)⁻¹ this equals blending the per-horizon position targets; on a constant
// (identity-decay) trajectory it COINCIDES with the aim-collapse byte-for-byte.
atx::core::Result<std::vector<atx::f64>>
MultiHorizonOptimizer::solve_stacked_mpc(const HorizonForecast &traj, const FactorModel &V,
                                         std::span<const atx::f64> w_prev,
                                         const book::CostInputs &cost) const {
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
  // ω_0 == 1 always. Build the return-space stacked aim ᾱ_stacked = Σ_h ω_h · alpha[h],
  // NaN-aware per name (a name finite at ≥1 horizon blends its finite contributions; an
  // all-NaN name stays NaN — mirrors gp_aim's policy, R8).
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

// blend_toward / l1_diff — REPLICATED VERBATIM from multi_period.hpp. The byte-identity
// of the boundary pin (R7) DEPENDS on these being the exact same arithmetic the S7
// driver uses. blend_toward SPECIAL-CASES rate==1.0 to assign target[i] VERBATIM: the
// algebraic form 0.0 + 1.0·(target[i] − 0.0) would flush a −0.0 target weight to +0.0
// (IEEE: 0.0 + −0.0 == +0.0), and the solver's dollar-neutral demean can emit −0.0, so
// the verbatim assignment is what preserves the signed zero and makes a full step
// byte-identical to the solver output.
std::vector<atx::f64> MultiHorizonOptimizer::blend_toward(std::span<const atx::f64> w_prev,
                                                          std::span<const atx::f64> target,
                                                          atx::f64 rate) {
  std::vector<atx::f64> book(target.size(), 0.0);
  for (atx::usize i = 0; i < target.size(); ++i) {
    const atx::f64 p = i < w_prev.size() ? w_prev[i] : 0.0;
    book[i] = (rate == 1.0) ? target[i] : (p + rate * (target[i] - p));
  }
  return book;
}

// Σ_i |a[i] − b[i]| in ascending i; an empty b is the flat all-zero book ⇒ Σ_i |a[i]|.
atx::f64 MultiHorizonOptimizer::l1_diff(std::span<const atx::f64> a,
                                        std::span<const atx::f64> b) noexcept {
  atx::f64 s = 0.0;
  for (atx::usize i = 0; i < a.size(); ++i) {
    const atx::f64 bi = i < b.size() ? b[i] : 0.0;
    s += std::fabs(a[i] - bi);
  }
  return s;
}

} // namespace atx::engine::risk
