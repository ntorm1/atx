#pragma once

// atx::engine::risk — reference_data: the CAPACITY REFERENCE inputs for the
// %ADV / %shares-outstanding position-box caps (S8.4).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  ParticipationCap / OwnershipCap (constraints.hpp) bound a single name's
//  weight as a fraction of its tradable liquidity / float, expressed back in
//  PORTFOLIO-WEIGHT units (|w_i| is a fraction of NAV). Both conversions need the
//  same per-name reference panel — ADV, shares outstanding, and the current mark
//  — plus the book NAV and the participation horizon. CapacityRef bundles exactly
//  that, as borrowed spans (the caller owns the storage; this is a cold-path
//  rebalance-cadence view, never copied into the hot loop).
//
//      |w_i| ≤ ρ · H · ADV_i · price_i / NAV       (participation, ρ=adv_frac)
//      |w_i| ≤ κ · shares_out_i · price_i / NAV     (ownership,    κ=shares_frac)
//
//  ADV_i is a SHARE average daily volume and price_i its mark, so ADV_i·price_i is
//  the dollar ADV; ρ·H scales it to the allowed dollar participation over H days;
//  dividing by NAV expresses the bound as a portfolio weight. shares_out_i·price_i
//  is the name's market float in dollars; κ caps the fraction of it the book may
//  hold, again over NAV. These fold (elementwise min) into the diagonal position
//  box in ConstraintSet::materialize — the cheapest constraint in the QP (R4: the
//  box rows are diagonal, never an M×M block).
//
// ===========================================================================
//  Determinism / allocation (R1)
// ===========================================================================
//  A pure data view — no RNG, no clock, no allocation. The spans are read in
//  ascending index order by materialize(); same inputs ⇒ byte-identical caps.

#include <span> // std::span (borrowed per-name reference panels)

#include "atx/core/types.hpp" // f64

namespace atx::engine::risk {

// The per-name capacity reference panel + book scalars the participation /
// ownership caps convert through. All spans are length-M, universe-aligned and
// BORROWED (the caller owns the storage). An EMPTY span ⇒ the corresponding cap
// cannot be evaluated (materialize treats it as not-binding for that name — the
// fold leaves the existing PositionCap / other caps to govern).
struct CapacityRef {
  std::span<const atx::f64> adv;        // per-name average daily volume (shares)
  std::span<const atx::f64> shares_out; // per-name shares outstanding (float)
  std::span<const atx::f64> price;      // per-name current mark (price per share)
  atx::f64 nav = 0.0;                   // book net asset value (dollars); |w_i| is a fraction of NAV
  atx::f64 horizon_days = 0.0;          // H — participation horizon in days (scales ρ·ADV)
};

} // namespace atx::engine::risk
