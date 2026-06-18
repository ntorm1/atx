#pragma once

// atx::engine::combine — conviction score: continuous per-alpha confidence (S10-1).
//
// ===========================================================================
//  What this unit is (RenTech gap G1)
// ===========================================================================
//  The AlphaGate (combine/gate.hpp) is a BINARY screen — an alpha is either in
//  the pool or out of it. The RenTech mapping (research/rentech-structure-...md,
//  finding G1) shows interpretability modulated SIZE, not membership: a signal
//  that worked but was not understood ("head-scratcher") was traded at REDUCED
//  size rather than discarded. This unit turns the gate's pass/fail into a
//  continuous conviction in [0,1] that downstream sizing (S10-2 fractional-Kelly)
//  scales position by. It does NOT replace the gate — an alpha must still be
//  admitted; conviction only graduates "admitted at full size" into "admitted at
//  a size proportional to confidence".
//
//  The score is a PURE function of already-computed evaluation outputs — it
//  recomputes nothing. Inputs: the deflated-Sharpe result (eval/deflated_sharpe),
//  the PBO result (eval/pbo), an out-of-sample / in-sample Sharpe stability ratio
//  the caller already holds, and an ExplainFlag (the step-3 "can we explain it?"
//  signal from the RenTech 3-step pipeline). See S10-1 for the full contract.
//
// ===========================================================================
//  What this unit is NOT
// ===========================================================================
//  * NOT an admission decision — it never rejects; that is the gate's job. A
//    HeadScratcher alpha is sized DOWN, not out (discount, not veto).
//  * NOT a recompute of DSR / PBO / stability — every term is read off an input
//    the caller already produced. No RNG, no allocation, no I/O.
//  * NOT annualization or any unit conversion — DSR and PBO are probabilities in
//    [0,1] and the stability ratio is dimensionless; conviction is a pure blend.

#include "atx/core/types.hpp" // atx::f64, atx::u8 (enum underlying type)

namespace atx::engine::eval {
// Consumed (read-only) evaluation outputs — defined in their own headers; only
// forward-declared here so the combine public surface does not drag the eval
// spine into every includer. The .cpp includes the full definitions.
struct DsrResult;
struct PboResult;
} // namespace atx::engine::eval

namespace atx::engine::combine {

// =====================================================================
//  Scoped enum — step-3 explainability verdict
// =====================================================================

// Step-3 explainability verdict from the RenTech signal-vetting pipeline:
// Explained (a known economic mechanism), PartlyExplained, or HeadScratcher
// (works out-of-sample but no mechanism). HeadScratcher DISCOUNTS conviction
// rather than rejecting the alpha ("trade unexplained at reduced size"). The
// underlying type is u8 to match the scaffold forward declaration in this header.
enum class ExplainFlag : atx::u8 {
  Explained,       // a known economic mechanism backs the signal — full size
  PartlyExplained, // partial mechanism — modest size haircut
  HeadScratcher,   // works OOS, no mechanism — trade, but at reduced size
};

// =====================================================================
//  Conviction config — fixed, named weights/floors (no RNG)
// =====================================================================

// The blend weights and explainability multipliers. Defaults are the published
// S10-1 values. INVARIANT: w_dsr + w_pbo + w_stability == 1.0 so `base` (their
// weighted sum of three terms each in [0,1]) is itself in [0,1] — the .cpp
// ATX_ASSERTs this in debug, since a caller that reweights must keep the sum 1.
struct ConvictionConfig {
  atx::f64 w_dsr = 0.40;                   // weight on deflated-Sharpe probability
  atx::f64 w_pbo = 0.35;                   // weight on (1 - PBO)
  atx::f64 w_stability = 0.25;             // weight on clamped OOS/IS Sharpe ratio
  atx::f64 partly_explained_mult = 0.75;   // PartlyExplained conviction multiplier
  atx::f64 head_scratcher_discount = 0.50; // HeadScratcher multiplier ("reduced size")
};

// =====================================================================
//  Conviction result — final score + the component breakdown
// =====================================================================

// Continuous confidence in [0,1] plus the (clamped) component terms and the
// explainability multiplier that produced it. Returning the breakdown — not just
// the scalar — lets a report attribute WHY an alpha is sized the way it is. The
// five fields are deterministic functions of the inputs (byte-identical run to
// run). Trivial aggregate (Rule of Zero); owns nothing.
struct ConvictionScore {
  atx::f64 score;          // final conviction in [0,1] = base * explain_mult
  atx::f64 dsr_term;       // clamped dsr probability used (clamp(dsr.dsr,0,1))
  atx::f64 pbo_term;       // clamped (1 - pbo) used (clamp(1-pbo.pbo,0,1))
  atx::f64 stability_term; // clamped OOS/IS ratio used (clamp(ratio,0,1))
  atx::f64 explain_mult;   // explainability multiplier applied (in [0,1])
};

// =====================================================================
//  conviction — the continuous [0,1] confidence (S10-1)
// =====================================================================

// Blend already-computed evaluation outputs into a single conviction:
//   base  = w_dsr*clamp(dsr.dsr) + w_pbo*clamp(1-pbo.pbo) + w_stability*clamp(ratio)
//   score = base * explain_mult                                  (in THIS order)
// where explain_mult is 1.0 (Explained), cfg.partly_explained_mult, or
// cfg.head_scratcher_discount per `explain`.
//
//   * dsr            — eval::DsrResult; only its `dsr` field (a probability in
//                      [0,1]) is read. Higher DSR ⇒ less selection bias ⇒ more
//                      conviction.
//   * pbo            — eval::PboResult; only its `pbo` field (probability of
//                      backtest overfitting in [0,1]) is read. The term is
//                      (1 - pbo), so higher PBO ⇒ LOWER conviction.
//   * oos_is_ratio   — caller-held OOS/IS Sharpe stability ratio. Clamped to
//                      [0,1]: ratio >= 1 (OOS as good as IS) ⇒ 1.0; ratio <= 0
//                      (OOS broke down) ⇒ 0.
//   * explain        — step-3 verdict; modulates SIZE, never membership.
//   * cfg            — weights/discounts; defaults sum w_dsr+w_pbo+w_stability=1.
//
// PRECONDITION (fail-closed): dsr.dsr, pbo.pbo, and oos_is_ratio must all be
// finite — a NaN input ATX_ASSERTs (aborts) in debug rather than poisoning the
// score. PURE, no RNG, order-fixed reduction; the result is byte-identical run to
// run. Guaranteed in [0,1] since base ∈ [0,1] (weights sum to 1, terms clamped)
// and explain_mult ∈ [0,1].
[[nodiscard]] ConvictionScore conviction(const eval::DsrResult &dsr, const eval::PboResult &pbo,
                                         atx::f64 oos_is_ratio, ExplainFlag explain,
                                         const ConvictionConfig &cfg = {}) noexcept;

} // namespace atx::engine::combine
