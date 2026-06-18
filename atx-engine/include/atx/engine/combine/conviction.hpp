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
//  SCAFFOLD (S10-0): this header currently forward-declares the spine types only;
//  their full definitions and the conviction() function land in S10-1.

#include "atx/core/types.hpp" // atx::f64, atx::u8 (enum underlying type)

namespace atx::engine::combine {

// =====================================================================
//  Scoped enum — forward declaration with explicit underlying type
// =====================================================================

// Step-3 explainability verdict from the RenTech signal-vetting pipeline:
// Explained (a known economic mechanism), PartlyExplained, or HeadScratcher
// (works out-of-sample but no mechanism). HeadScratcher discounts conviction
// rather than rejecting the alpha. Full definition in combine/conviction.hpp (S10-1).
enum class ExplainFlag : atx::u8;

// =====================================================================
//  Conviction config + result — forward declarations (S10-1)
// =====================================================================

// Fixed, named weights/floors governing the conviction reduction (no RNG).
// Full definition in combine/conviction.hpp (S10-1).
struct ConvictionConfig;

// Continuous confidence in [0,1] plus the component breakdown that produced it.
// Full definition in combine/conviction.hpp (S10-1).
struct ConvictionScore;

} // namespace atx::engine::combine
