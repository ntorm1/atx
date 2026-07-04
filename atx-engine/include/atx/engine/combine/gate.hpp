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
//  admit() checks conditions in THIS fixed order; the verdict is the FIRST
//  one that fails (so a candidate failing several conditions has ONE deterministic
//  verdict — the earliest in the order):
//    1. metrics.fitness      >= cfg.min_fitness      else RejectFitness   (WQ-aligned primary gate)
//    2. metrics.sharpe       >= cfg.min_sharpe       else RejectSharpe    (low sanity floor; DSR is the sig gate)
//    3. metrics.turnover     <= cfg.max_turnover     else RejectTurnover  (ceiling)
//    3b.metrics.holding_days >= cfg.min_holding_days else RejectTurnover  (S4-2 floor; inert at min_holding_days=0)
//    3c.defl.dsr             >= cfg.min_dsr          else RejectDsr            (S1-1; inert at min_dsr=0)
//    3d.defl.pbo             <= cfg.max_pbo          else RejectPbo            (S1-2; inert at max_pbo=1)
//    3e.defl.split_stable    (when require_split_stable) else RejectSplitUnstable (S1-3; inert when flag false)
//    4. corr_to_pool         <= cfg.max_pool_corr    else RejectCorrelated
//    else                                                 Accept
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

  // Cost / holding-period fields (S4 plumbing; inert at the 0.0 default — read only when > 0.0).
  atx::f64 rt_cost_bps      = 0.0; // round-trip cost in bps; 0 => frictionless (no cost gate)
  atx::f64 min_holding_days = 0.0; // holding-period floor in periods; 0 => inert

  // Deflation / selection-bias fields (S1 plumbing; inert at the stated defaults).
  // These give the STATELESS gate (the library / standalone caller) the same DSR /
  // PBO / split-half honesty the factory tier already enforces via FactoryConfig.
  // Each is read ONLY when non-inert, so the default off-path is byte-identical to
  // the pre-S1 verdict (see AlphaGate::admit S1-1/S1-2/S1-3 guards).
  atx::f64 min_dsr              = 0.0;   // DSR floor; 0.0 => inert (DSR ∈ [0,1] always ≥ 0)
  atx::f64 max_pbo             = 1.0;   // PBO ceiling; 1.0 => inert (PBO ∈ [0,1] never > 1)
  bool     require_split_stable = false; // require split-half sign agreement; false => inert
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
  // S1: deflation / selection-bias rejects. APPENDED at the END — the enumerator
  // order is a FROZEN reject-histogram index, so new buckets must extend the tail
  // (indices 5,6,7) and never renumber an existing value. Pinned by the
  // gate_verdict_histogram_layout static_assert (S1-5).
  RejectDsr,           // S1: holdout DSR below cfg.min_dsr floor
  RejectPbo,           // S1: run-level PBO above cfg.max_pbo ceiling
  RejectSplitUnstable, // S1: split-half holdout halves disagree in Sharpe sign
};

// ===========================================================================
//  GateDeflation — the per-candidate deflation / selection-bias scalars the S1
//  gate screens against (DSR floor, PBO ceiling, split-half stability).
//
//  WHY A SEPARATE STRUCT (not fields on AlphaMetrics): AlphaMetrics is serialized
//  VERBATIM into the library segment record (library/record.hpp::AlphaDirEntry,
//  pinned by static_assert(sizeof(AlphaMetrics)==56) and embedded in the segment
//  CRC / manifest version_id). Growing AlphaMetrics would change the on-disk format
//  and break byte-identity of every library golden/digest — forbidden by the p7
//  determinism contract. These deflation scalars are an ADMISSION-TIME screening
//  input, not durable record metadata, so they live in this small NON-serialized
//  POD that the caller fills (from holdout_dsr() / pbo_cscv / split_floor_ok) and
//  passes alongside the metrics. The inert sentinels match the plan's intent: an
//  UNSET instance can never trip a gate at the inert GateConfig default.
//    dsr = 1.0  : DSR max => clears any min_dsr ∈ [0,1]
//    pbo = 0.0  : PBO min => never exceeds any max_pbo ∈ [0,1]
//    split_stable = false : gated only when require_split_stable is explicitly set
// ===========================================================================
struct GateDeflation {
  atx::f64 dsr          = 1.0;   // Deflated Sharpe Ratio ∈ [0,1]
  atx::f64 pbo          = 0.0;   // Probability of Backtest Overfitting ∈ [0,1]
  bool     split_stable = false; // both holdout halves share the full-sample Sharpe sign
};

// The inert default deflation input: passing this (or nothing) to admit() leaves
// the three S1 screens dormant, so the verdict is byte-identical to the pre-S1 gate.
inline constexpr GateDeflation kInertDeflation{};

// ===========================================================================
//  AlphaGate — stateless admission screen (holds only its config).
// ===========================================================================
struct AlphaGate {
  GateConfig cfg;

  // Admit iff metrics clear the floors AND the deflation screens AND max
  // |corr-to-pool| <= max_pool_corr. `candidate_pnl` is the candidate's realized-PnL
  // stream (length == the pool's n_periods() once the pool is non-empty); it is
  // correlated pairwise-complete against each accepted member's PnL row. `defl`
  // carries the per-candidate DSR / PBO / split-half scalars the S1 deflation gates
  // screen against; it DEFAULTS to the inert instance, so every pre-S1 caller (which
  // omits it) gets a byte-identical verdict. Returns the §5.2 fixed-order verdict.
  // PURE + const: reads only; the caller inserts on Accept.
  [[nodiscard]] GateVerdict admit(const AlphaMetrics &metrics,
                                  std::span<const atx::f64> candidate_pnl, const AlphaStore &pool,
                                  const GateDeflation &defl = kInertDeflation) const noexcept {
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
    // S4-2: holding-period floor — a turnover-side guard (reuses RejectTurnover;
    // holding is 1/turnover, so the two checks are adjacent). The guard is inert
    // at the default min_holding_days=0.0 (the > 0.0 condition is false) so this
    // branch is never taken and verdicts are byte-identical to pre-S4-2.
    if (cfg.min_holding_days > 0.0 && metrics.holding_days < cfg.min_holding_days) {
      return GateVerdict::RejectTurnover;
    }
    // S1 deflation / selection-bias screens. These are CHEAP scalar comparisons, so
    // they slot in BEFORE the O(|pool|·T) correlation sweep (a deflation-rejected
    // candidate never pays the corr cost) and AFTER the standalone quality floors
    // (they are quality floors of the same kind). Each is INERT at its GateConfig
    // default, so the off-path is byte-identical to the pre-S1 verdict; each fires
    // only when the caller sets a non-inert bar AND supplies the matching `defl`.
    //
    // S1-1: DSR (Deflated Sharpe Ratio) floor — the statistical-significance gate.
    // DSR ∈ [0,1] = P(true SR > SR*_N | data) under N-trial selection bias. Inert at
    // min_dsr=0.0 (DSR is always ≥ 0, so the test never fires); fires only when the
    // caller sets a positive bar (e.g. the FactoryConfig-aligned min_dsr=0.5).
    if (cfg.min_dsr > 0.0 && defl.dsr < cfg.min_dsr) {
      return GateVerdict::RejectDsr;
    }
    // S1-2: PBO (Probability of Backtest Overfitting) ceiling — run-level overfit
    // screen. PBO ∈ [0,1]: → 0 a persistent edge; → 0.5 the IS winner is OOS noise.
    // Inert at max_pbo=1.0 (PBO never strictly exceeds 1.0, so the test never fires).
    if (cfg.max_pbo < 1.0 && defl.pbo > cfg.max_pbo) {
      return GateVerdict::RejectPbo;
    }
    // S1-3: split-half stability — reject a single-regime artifact. split_stable ==
    // both halves of the holdout PnL share the full-sample Sharpe sign. Inert at the
    // default require_split_stable=false (boolean guard; never fires). When active,
    // rejects a candidate whose halves contradict (strong H1, dead/negative H2 is the
    // canonical failure mode).
    if (cfg.require_split_stable && !defl.split_stable) {
      return GateVerdict::RejectSplitUnstable;
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
