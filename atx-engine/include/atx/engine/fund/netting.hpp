#pragma once

// atx::engine::fund — Internal-crossing Netting (P2-S2-4): the honest crossing
// measurement. Sum the per-sleeve target DELTAS into ONE fund net trade; offsetting
// sleeve flow crosses internally, so the fund trades the NET, not the gross.
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  S2-2's MetaAllocator gives per-sleeve capital weights c_s; each sleeve carries a
//  target book w_s (at period t) and its prior book (at t−1, both over the SAME
//  universe M). Trading the sleeves SEPARATELY would move Σ_i Σ_s |c_s·Δw_{s,i}| of
//  gross turnover. But the fund holds ONE consolidated book W = Σ_s c_s·w_s, so the
//  ACTUAL trade per name is the netted delta Σ_s c_s·Δw_{s,i}: when sleeve A buys a
//  name that sleeve B sells, the flow crosses INTERNALLY and never hits the market.
//  This unit measures, in ONE pass (§4.4):
//    fund_book            = W = Σ_s c_s·w_s                  (the consolidated book)
//    turnover_gross       = Σ_i Σ_s |c_s·Δw_{s,i}|           (sleeves traded separately)
//    turnover_net         = Σ_i |Σ_s c_s·Δw_{s,i}|          ≤ gross (triangle, R3)
//    crossing_benefit_bps = (gross − net)·round_trip_cost_bps ≥ 0 (R3)
//    crossed_fraction     = (gross − net)/gross ∈ [0,1]      (the internal-cross rate)
//
//  The benefit is priced through the SAME calibrated book::CostInputs the sleeves and
//  the single-fund path use — one cost model, no second calibration. We ship the LINEAR
//  round_trip_cost_bps form; the convex-impact crossing variant (pricing each name's
//  turnover through a nonlinear a|q|+b|q|^{3/2} impact term) is a RECORDED refinement
//  (§4.4.1), NOT in this unit.
//
// ===========================================================================
//  The invariants this unit must NOT violate (R3)
// ===========================================================================
//  turnover_net ≤ turnover_gross ALWAYS — the triangle inequality |Σ d| ≤ Σ |d| holds
//  by construction (net sums the signed deltas, gross sums their magnitudes). And
//  crossing_benefit_bps ≥ 0 ALWAYS, since (gross − net) ≥ 0 and round_trip_cost_bps ≥ 0.
//  A violation would be a cost-model bug; here they hold structurally and the tests
//  ASSERT them on every fixture.
//
// ===========================================================================
//  Determinism (R1) / no-look-ahead (R2)
// ===========================================================================
//  Order-fixed: ascending name i (outer), ascending sleeve s (inner); no RNG, no clock,
//  no std::unordered_*. Same inputs ⇒ byte-identical NetResult. net_fund_book consults
//  ONLY the current + prior sleeve books passed in — structurally a same-timestamp
//  aggregation of already-known targets, no realized return / future bar (§0.9). It is a
//  PURE function of its arguments.

#include <span>   // std::span (sleeve books / prev / capital)
#include <vector> // std::vector (fund_book)

#include "atx/core/error.hpp" // Result, ErrorCode
#include "atx/core/types.hpp" // f64

#include "atx/engine/risk/multi_period.hpp" // book::CostInputs (the ONE calibrated cost model)

namespace atx::engine::fund {

// ===========================================================================
//  NetResult — the netted fund book + gross/net turnover + the priced crossing benefit.
// ===========================================================================
struct NetResult {
  // W = Σ_s c_s·w_s, length M (order-fixed; empty when there are no sleeves / no names).
  std::vector<atx::f64> fund_book;
  // Σ_i Σ_s |c_s·Δw_{s,i}| — turnover if the sleeves traded SEPARATELY (no crossing).
  atx::f64 turnover_gross = 0.0;
  // Σ_i |Σ_s c_s·Δw_{s,i}| — the netted fund trade ≤ gross (the triangle inequality, R3).
  atx::f64 turnover_net = 0.0;
  // (gross − net)·round_trip_cost_bps — the saving from internal crossing, ≥ 0 (R3).
  atx::f64 crossing_benefit_bps = 0.0;
  // (gross − net)/gross — the internal-cross rate ∈ [0,1] (upper bound holds since
  // turnover_net ≤ turnover_gross; 0 when gross == 0). Report metric.
  atx::f64 crossed_fraction = 0.0;
};

// ===========================================================================
//  net_fund_book — net the sleeve books at ONE period into the fund order; measure the
//  crossing benefit.
//
//  sleeve_books : sleeve s's target book w_s at period t, one span per sleeve, EACH
//                 length M (== sleeve_books[0].size()). S == sleeve_books.size().
//  sleeve_prev  : sleeve s's book at t−1, one span per sleeve, EACH length M. An EMPTY
//                 sleeve_prev means "first move from a flat book" (all prev = 0).
//  c            : per-sleeve capital weights (length S == sleeve_books.size()).
//  cost         : the calibrated cost adapter; the benefit is priced via
//                 cost.round_trip_cost_bps (bps per unit turnover) — the SAME field the
//                 sleeves / single-fund path use. REUSED, never redefined.
//
//  Boundary validation (before compute) ⇒ Err(InvalidArgument):
//    * sleeve_books.size() == c.size() (== S);
//    * every sleeve_books[s].size() == M (consistent length);
//    * sleeve_prev is EITHER empty OR size() == S with every sleeve_prev[s].size() == M.
//  Degenerate (valid, NOT an error): S == 0 ⇒ Ok with empty fund_book and all-zero
//  turnovers/benefit; likewise M == 0.
//
//  NaN policy (matching S2-3 cross_sleeve_risk.hpp): a non-finite input (NaN in
//  sleeve_books / sleeve_prev / c) PROPAGATES into fund_book and the turnovers — it is
//  NOT rejected (this unit does not scrub inputs; that is caller responsibility). One
//  consequence: a NaN turnover makes the t_gross > 0.0 test false, so crossed_fraction
//  falls to 0.0 rather than NaN.
//
//  Invariants (R3, hold by construction): turnover_net ≤ turnover_gross (triangle) and
//  crossing_benefit_bps ≥ 0. PIT-safe by construction (§0.9): a same-timestamp
//  aggregation of already-known targets — NO future data. Order-fixed (R1).
// ===========================================================================
[[nodiscard]] atx::core::Result<NetResult>
net_fund_book(std::span<const std::span<const atx::f64>> sleeve_books,
              std::span<const std::span<const atx::f64>> sleeve_prev,
              std::span<const atx::f64> c, const book::CostInputs& cost);

} // namespace atx::engine::fund
