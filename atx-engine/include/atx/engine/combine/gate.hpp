#pragma once

// atx::engine::combine — AlphaGate: orthogonality + quality gates (P4-3).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  The stateless screen that decides whether a candidate alpha is ADMITTED into
//  the pool. An alpha is admitted only if it clears the standalone quality floors
//  (Sharpe / fitness / turnover) AND is sufficiently DIVERSIFYING — its maximum
//  |pairwise-complete Pearson| correlation to any already-accepted pool member is
//  <= max_pool_corr. The diversification gate is the operational core of the
//  weak-signal thesis: a pool of individually-mediocre-but-uncorrelated alphas
//  combines into a strong signal, but a pool of correlated copies does not. The
//  combiner (P4-4) consumes the admitted pool the gate produces.
//
// ===========================================================================
//  §5.2 algorithm — FIXED-ORDER, deterministic verdict
// ===========================================================================
//  admit() checks four conditions in THIS fixed order; the verdict is the FIRST
//  one that fails (so a candidate failing several conditions has ONE deterministic
//  verdict — the earliest in the order):
//    1. metrics.fitness  >= cfg.min_fitness    else RejectFitness   (WQ-aligned primary gate)
//    2. metrics.sharpe   >= cfg.min_sharpe     else RejectSharpe    (low sanity floor; DSR is the sig gate)
//    3. metrics.turnover <= cfg.max_turnover   else RejectTurnover
//    4. corr_to_pool     <= cfg.max_pool_corr  else RejectCorrelated
//    else                                           Accept
//  corr_to_pool = max_j |pairwise_complete_corr(candidate, member_j)| over the
//  pool ("max" = the strictest member; it is a MAGNITUDE gate, so a perfectly
//  anti-correlated member with |corr| = 1 is just as disqualifying as a perfect
//  copy). An EMPTY pool ⇒ corr_to_pool = 0, so the first floor-clearing alpha is
//  always admitted.
//
//  LAZY-CORRELATION OPTIMIZATION (mandated): corr_to_pool is computed ONLY after
//  checks 1–3 pass. A floor-rejected candidate never pays the O(|pool|·T)
//  correlation cost. This is observationally identical to the eager fixed order
//  (the verdict of a candidate that fails a floor is decided before the corr gate
//  is ever consulted), so the optimization does not change any verdict.
//
//  The pairwise-complete NaN policy (and the degenerate-pair → 0 convention) is
//  shared verbatim with the combiner via combine/correlation.hpp ("one helper").
//
// ===========================================================================
//  Statelessness
// ===========================================================================
//  AlphaGate holds only its GateConfig (value, copied). admit() is const and
//  pure: it reads the candidate metrics, the candidate PnL stream, and the pool's
//  member PnL rows — it mutates nothing and inserts nothing (the caller inserts on
//  an Accept verdict). The verdict→action mapping is the CALLER's; where a caller
//  switches on GateVerdict it must handle every enumerator (no `default`).

#include <span> // std::span (candidate PnL view)

#include "atx/core/macro.hpp" // ATX_ASSERT (debug length precondition)
#include "atx/core/types.hpp" // atx::f64, atx::u8, atx::usize

#include "atx/engine/combine/correlation.hpp" // pairwise_complete_corr (shared §3.3 helper)
#include "atx/engine/combine/cost_util.hpp"   // combine::cost_adjusted_fitness, kFitnessCostScale
#include "atx/engine/combine/metrics.hpp"     // AlphaMetrics (the floored fields)
#include "atx/engine/combine/store.hpp"       // AlphaStore, AlphaId (the accepted pool)

namespace atx::engine::combine {

// ===========================================================================
//  GateConfig — admission thresholds (§4). Defaults are the plan's published
//  values (BRAIN "gold standard" fitness floor; WQ §6.5 cost-gate turnover).
// ===========================================================================
struct GateConfig {
  atx::f64 min_sharpe = 0.25;   // standalone-Sharpe sanity floor (statistical-significance gate is DSR)
  atx::f64 min_fitness = 1.0;   // BRAIN "gold standard for submission" (WQ §4.4)
  atx::f64 max_turnover = 0.70; // generous default; cost-gate (WQ §6.5)
  atx::f64 max_pool_corr = 0.7; // reject if too correlated with an accepted alpha

  // Cost / holding-period fields (S4 plumbing; inert at 0 — nothing reads them yet).
  atx::f64 rt_cost_bps      = 0.0; // round-trip cost in bps; 0 => frictionless (no cost gate)
  atx::f64 min_holding_days = 0.0; // holding-period floor in periods; 0 => inert
};

// ===========================================================================
//  GateVerdict — admission outcome. The underlying type MUST match the
//  forward declaration in combine/fwd.hpp (`enum class GateVerdict : atx::u8;`).
//  Enumerator order is FROZEN: it is used as a stable array index for the reject
//  histogram, so values must not be renumbered. It does NOT reflect the runtime
//  check order — admission now tests fitness BEFORE the raw-Sharpe sanity floor
//  (RejectFitness can fire before RejectSharpe). Callers that map a verdict to an
//  action must switch EXHAUSTIVELY (no `default`) so a future enumerator forces a
//  compile error rather than silent fall-through.
// ===========================================================================
enum class GateVerdict : atx::u8 {
  Accept,
  RejectSharpe,
  RejectFitness,
  RejectTurnover,
  RejectCorrelated,
};

// ===========================================================================
//  AlphaGate — stateless admission screen (holds only its config).
// ===========================================================================
struct AlphaGate {
  GateConfig cfg;

  // Admit iff metrics clear the floors AND max |corr-to-pool| <= max_pool_corr.
  // `candidate_pnl` is the candidate's realized-PnL stream (length == the pool's
  // n_periods() once the pool is non-empty); it is correlated pairwise-complete
  // against each accepted member's PnL row. Returns the §5.2 fixed-order verdict.
  // PURE + const: reads only; the caller inserts on Accept.
  [[nodiscard]] GateVerdict admit(const AlphaMetrics &metrics,
                                  std::span<const atx::f64> candidate_pnl,
                                  const AlphaStore &pool) const noexcept {
    // §5.2 checks 1–3: the standalone quality floors, in fixed order. First
    // failing condition is the verdict (deterministic).
    // Fitness (WQ-aligned) is the dominant primary gate; sharpe is a low
    // sanity floor only (the statistical-significance gate is DSR, factory-side).
    //
    // S4-1: when rt_cost_bps > 0 use cost-adjusted fitness so the floor is net-of-cost.
    // At rt_cost_bps == 0 (default) the branch is not taken and eff_fitness ==
    // metrics.fitness exactly — byte-identical to the pre-S4-1 path.
    const atx::f64 eff_fitness =
        (cfg.rt_cost_bps > 0.0)
            ? combine::cost_adjusted_fitness(metrics.fitness, metrics.turnover, cfg.rt_cost_bps)
            : metrics.fitness;
    if (eff_fitness < cfg.min_fitness) {
      return GateVerdict::RejectFitness;
    }
    if (metrics.sharpe < cfg.min_sharpe) {
      return GateVerdict::RejectSharpe;
    }
    if (metrics.turnover > cfg.max_turnover) {
      return GateVerdict::RejectTurnover;
    }
    // §5.2 check 4 (LAZY): only now — after the floors pass — pay the
    // O(|pool|·T) correlation cost. Empty pool ⇒ corr_to_pool = 0 (the loop
    // body never runs), so the first alpha clears the diversification gate.
    const atx::f64 corr_to_pool = max_abs_corr_to_pool(candidate_pnl, pool);
    if (corr_to_pool > cfg.max_pool_corr) {
      return GateVerdict::RejectCorrelated;
    }
    return GateVerdict::Accept;
  }

private:
  // max_j |pairwise_complete_corr(candidate, member_j)| over the pool. The MAX
  // (strictest member) is a magnitude gate: |corr| treats a perfect copy and a
  // perfect anti-correlation as equally disqualifying. Empty pool ⇒ 0.
  [[nodiscard]] static atx::f64 max_abs_corr_to_pool(std::span<const atx::f64> candidate_pnl,
                                                     const AlphaStore &pool) noexcept {
    // Every pool member's PnL row has length pool.n_periods(), so the candidate
    // MUST match that length to be a coherent paired-observation stream. An empty
    // pool short-circuits (the loop never runs — nothing to correlate against).
    // pairwise_complete_corr is OOB-safe regardless (it truncates to the overlap
    // in release); this assert makes the contract violation loud in debug.
    ATX_ASSERT(pool.n_periods() == 0U || candidate_pnl.size() == pool.n_periods());
    atx::f64 worst = 0.0;
    const atx::usize n = pool.n_alphas();
    for (atx::usize i = 0U; i < n; ++i) {
      const std::span<const atx::f64> member = pool.pnl(AlphaId{static_cast<atx::u32>(i)});
      const atx::f64 c = pairwise_complete_corr(candidate_pnl, member);
      const atx::f64 mag = (c < 0.0) ? -c : c; // |corr| (magnitude gate)
      if (mag > worst) {
        worst = mag;
      }
    }
    return worst;
  }
};

} // namespace atx::engine::combine
