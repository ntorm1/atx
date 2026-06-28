#pragma once

// atx::engine::cost — Cost-aware decision knobs (Sprint S6-4).
//
// ===========================================================================
//  What this header does (C8 — cost is a decision input)
// ===========================================================================
//  Maps a CalibratedCost (S6-1) into the THREE existing decision points so the
//  discovery + combination pipeline prices turnover honestly:
//
//    1. the optimizer's turnover penalty κ  (risk::OptimizerConfig::turnover_penalty),
//    2. the gate's max_turnover ceiling     (combine::GateConfig::max_turnover),
//    3. a factory fitness cost-floor        (CostKnobs::fitness_cost_floor),
//
//  plus a cost_adjusted_fitness down-ranking that subtracts a turnover-
//  proportional cost penalty from raw fitness — so a high-turnover net loser
//  ranks below a low-turnover winner even when its RAW fitness is higher.
//
//  EMIT-ONLY (additive). This unit returns the κ scalar, a populated GateConfig,
//  and a fitness floor; it does NOT edit optimizer.hpp / gate.hpp / fitness.hpp —
//  the CALLER feeds the emitted values into those existing decision points.
//
// ===========================================================================
//  ONE COST MODEL (C6) — reuses the simulator's functional form
// ===========================================================================
//  round_trip_cost_bps does NOT introduce a second impact formula. It applies the
//  SAME structural laws the execution simulator charges (execution_sim.hpp:148):
//
//      temp = Y · σ · p^δ        (temporary impact, folded into the fill price)
//      perm = 0.5 · γ · σ · p    (permanent impact, shifts the mark)
//
//  to the CALIBRATED coefficients (cc.impact) at a representative (participation,
//  sigma). It is the same model evaluated at one point — not a new model.
//
// ===========================================================================
//  AS-BUILT RECONCILIATION (recorded in the S6-4 ledger row)
// ===========================================================================
//  The plan sketched cost_aware_knobs(cc, Market, horizon) with a
//  typical_participation(mkt) helper. The as-built Market exposes only stats(id)
//  for a KNOWN id — there is no public universe iterator to enumerate the names
//  and compute a "typical" participation/σ. So the universe-representative
//  participation and sigma are taken as EXPLICIT scalars: the caller supplies the
//  Market → (ref_participation, ref_sigma) reduction. This header takes those two
//  scalars rather than a Market handle.

#include <algorithm> // std::clamp
#include <cmath>     // std::pow (the temporary-impact exponent term)

#include "atx/core/types.hpp" // atx::f64

#include "atx/engine/combine/cost_util.hpp"  // combine::cost_adjusted_fitness, kFitnessCostScale (S4-1 leaf)
#include "atx/engine/combine/gate.hpp"       // combine::GateConfig (max_turnover knob)
#include "atx/engine/combine/metrics.hpp"    // combine::AlphaMetrics (fitness / turnover)
#include "atx/engine/cost/calibration.hpp"   // cost::CalibratedCost (the calibrated coeffs)
#include "atx/engine/exec/execution_sim.hpp" // exec::ImpactCfg, exec::SlippageCfg (the sim's forms)

namespace atx::engine::cost {

// ===========================================================================
//  Tunable constants (latitude; the INVARIANTS the tests pin are not).
// ===========================================================================

/// Basis-points-per-unit-fraction conversion (a fraction 0.01 == 100 bps).
inline constexpr atx::f64 kBpsPerUnit = 1.0e4;

/// Fitness-penalty scale: cost_penalty = turnover · rt_cost_bps · kFitnessScale,
/// in raw-fitness units. Chosen so a realistic per-trip cost (a few bps) applied
/// to a high-turnover alpha (≈0.9) materially erodes a fitness ≈ O(1): at
/// rt_cost_bps = 8, turnover 0.9 costs 0.72 fitness units, turnover 0.1 costs
/// 0.08 — a >0.6-unit spread that flips a thin RAW-fitness lead. Documented, not
/// fitted (the down-rank flip is the only invariant the test pins).
/// S4-1: the actual 0.1 constant lives in combine/cost_util.hpp (kFitnessCostScale)
/// so gate.hpp, library.hpp, and this header all share ONE definition. This alias
/// keeps the public name cost::kFitnessScale unchanged (existing call sites unchanged).
inline constexpr atx::f64 kFitnessScale = combine::kFitnessCostScale;

/// Gate-map sensitivity: how fast the max_turnover ceiling decays with cost. The
/// ceiling is 0.70 · horizon / (horizon + kGateCostSensitivity · rt_cost_bps),
/// so a costlier universe (larger rt_cost_bps) shrinks the ceiling and a longer
/// horizon (slower rebalancing → the round trip is paid less often) widens it.
inline constexpr atx::f64 kGateCostSensitivity = 0.10;

/// The default gate ceiling (combine::GateConfig::max_turnover default, gate.hpp:70)
/// — the map clamps the cost-tightened ceiling AT MOST to this.
inline constexpr atx::f64 kGateCeiling = 0.70;

/// A sane floor for the gate ceiling: even a very costly universe still admits
/// some turnover (a strategy that never trades is not the goal). Clamp >= this.
inline constexpr atx::f64 kGateFloor = 0.05;

// ===========================================================================
//  CostKnobs — the three emitted decision knobs (definition for fwd.hpp:147).
// ===========================================================================
//  kappa              → feeds risk::OptimizerConfig::turnover_penalty.
//  gate               → a GateConfig with a cost-tightened max_turnover.
//  fitness_cost_floor → the round-trip cost in bps, a factory fitness floor.
struct CostKnobs {
  atx::f64 kappa;
  combine::GateConfig gate;
  atx::f64 fitness_cost_floor;
};

// ---------------------------------------------------------------------------
//  round_trip_cost_bps — representative MODELED round-trip cost in bps.
// ---------------------------------------------------------------------------
/// Applies the simulator's temp / perm forms (C6) to the CALIBRATED coefficients
/// at a representative (participation, sigma), plus the calibrated fixed-bps
/// slippage. A round trip pays the temporary impact AND the slippage on the way
/// IN and again on the way OUT, plus the one-time permanent footprint:
///
///   temp      = Y · σ · p^δ                       (one-way temporary impact, fraction)
///   slip_frac = slippage.bps / 1e4                (one-way slippage, fraction)
///   perm      = 0.5 · γ · σ · p                   (one-time permanent footprint, fraction)
///   rt_frac   = 2·(temp + slip_frac) + perm
///   rt_cost_bps = rt_frac · 1e4
///
/// MONOTONE in every cost coefficient (Y, γ, slippage.bps) and in (p, σ).
[[nodiscard]] inline atx::f64 round_trip_cost_bps(const CalibratedCost& cc,
                                                  atx::f64 participation,
                                                  atx::f64 sigma) {
  const exec::ImpactCfg& imp = cc.impact;
  const atx::f64 temp = imp.Y * sigma * std::pow(participation, imp.delta);
  const atx::f64 perm = 0.5 * imp.gamma * sigma * participation;
  const atx::f64 slip_frac = cc.slippage.bps / kBpsPerUnit;
  const atx::f64 rt_frac = 2.0 * (temp + slip_frac) + perm;
  return rt_frac * kBpsPerUnit;
}

// ---------------------------------------------------------------------------
//  max_turnover_for — the cost → gate-ceiling map.
// ---------------------------------------------------------------------------
/// Monotone-DECREASING in rt_cost_bps and INCREASING in horizon_days, clamped to
/// [kGateFloor, kGateCeiling]. A costlier universe tightens the ceiling; a longer
/// rebalance horizon (the round trip is paid less often) relaxes it. A non-
/// positive horizon degenerates to the floor (no horizon ⇒ no relaxation).
/// noexcept: arithmetic + std::clamp (both noexcept); no allocation, no throw path.
[[nodiscard]] inline atx::f64 max_turnover_for(atx::f64 rt_cost_bps,
                                               atx::f64 horizon_days) noexcept {
  if (horizon_days <= 0.0) {
    return kGateFloor;
  }
  const atx::f64 denom = horizon_days + kGateCostSensitivity * rt_cost_bps;
  const atx::f64 raw = kGateCeiling * horizon_days / denom;
  return std::clamp(raw, kGateFloor, kGateCeiling);
}

// ---------------------------------------------------------------------------
//  gate_config_for_cost — derive a GateConfig with a cost-tightened max_turnover.
// ---------------------------------------------------------------------------
/// Returns a GateConfig whose max_turnover is the cost-tightened ceiling from
/// max_turnover_for(rt_cost_bps, horizon_days).  All other fields keep their
/// defaults so the caller receives a minimal override: only turnover is cost-driven.
/// Sprint 7 calls this during config construction; the default GateConfig::max_turnover
/// (0.70) is UNCHANGED for callers that do not use this helper.
/// noexcept: max_turnover_for is noexcept; GateConfig aggregate-init is trivial.
[[nodiscard]] inline combine::GateConfig gate_config_for_cost(
    atx::f64 rt_cost_bps, atx::f64 horizon_days) noexcept {
  combine::GateConfig cfg{};
  cfg.max_turnover = max_turnover_for(rt_cost_bps, horizon_days);
  return cfg;
}

// ---------------------------------------------------------------------------
//  cost_aware_knobs — map calibrated cost → the three decision knobs.
// ---------------------------------------------------------------------------
/// MONOTONE: a costlier calibration (larger round-trip cost) yields a LARGER κ
/// and a TIGHTER gate.max_turnover. The other GateConfig fields keep their
/// defaults (this unit prices only turnover).
///   kappa              := rt_cost_bps / 1e4        (a per-unit-turnover penalty)
///   gate.max_turnover  := max_turnover_for(rt_cost_bps, horizon_days)
///   fitness_cost_floor := rt_cost_bps
[[nodiscard]] inline CostKnobs cost_aware_knobs(const CalibratedCost& cc,
                                                atx::f64 ref_participation,
                                                atx::f64 ref_sigma,
                                                atx::f64 horizon_days) {
  const atx::f64 rt_cost_bps = round_trip_cost_bps(cc, ref_participation, ref_sigma);
  combine::GateConfig gate{}; // defaults; only max_turnover is cost-driven.
  gate.max_turnover = max_turnover_for(rt_cost_bps, horizon_days);
  return CostKnobs{rt_cost_bps / kBpsPerUnit, gate, rt_cost_bps};
}

// ---------------------------------------------------------------------------
//  cost_adjusted_fitness — turnover-proportional down-ranking (C8).
// ---------------------------------------------------------------------------
/// Net fitness = raw fitness − a turnover-proportional cost penalty (in fitness
/// units): cost_penalty(turnover, rt_cost_bps) = turnover · rt_cost_bps ·
/// kFitnessScale. A high-turnover alpha pays more, so two alphas with the same
/// raw fitness are split by turnover, and a high-turnover net loser ranks below a
/// low-turnover winner even when its RAW fitness is higher.
/// S4-1: delegates to combine::cost_adjusted_fitness (combine/cost_util.hpp) so the
/// formula and the 0.1 scale live in ONE place. Public signature + behaviour unchanged.
[[nodiscard]] inline atx::f64 cost_adjusted_fitness(const combine::AlphaMetrics& m,
                                                    atx::f64 rt_cost_bps) noexcept {
  return combine::cost_adjusted_fitness(m.fitness, m.turnover, rt_cost_bps);
}

} // namespace atx::engine::cost
