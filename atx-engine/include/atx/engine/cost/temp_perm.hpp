#pragma once

// atx::engine::cost — Temp/perm split observer + cost throttle (Sprint S6-2).
//
// ===========================================================================
//  What this header is — a PROOF harness, not a second cost model (C3 + C6)
// ===========================================================================
//  S6-2's job is to PROVE the C3 hard exit criterion empirically: the execution
//  simulator's TEMPORARY impact lives only in the fill PRICE and never leaks into
//  the forward MARK. The mark moves by the PERMANENT component alone, and a later
//  no-trade bar leaves the mark untouched (temp does not grow / re-accrue).
//
//  Critically, this header adds NO impact math (C6: one cost model). It DRIVES the
//  existing exec::ExecutionSimulator + engine::Market and READS the marks/fill the
//  sim produces — it never re-derives Y·σ·partᵟ or 0.5·γ·σ·part. The expected
//  values are computed in the TEST from the documented sim formula; this header
//  contains no impact coefficients.
//
//  Three pieces:
//    * simulate_round_trip — drives ONE trade through sim+market and captures the
//      mark before the fill, the fill price, the mark after the fill, and the mark
//      on a later no-trade bar. The differential the C3 proof asserts on.
//    * fit_split_ratio    — a reporting helper: the median |perm_move|/|temp_move|
//      across observations (perm magnitude relative to temp magnitude).
//    * should_trade       — a RenTech-style throttle: decline a trade whose modeled
//      edge does not clear its modeled round-trip cost.
//
// ===========================================================================
//  Borrow-lifetime discipline (the one foot-gun this harness must respect)
// ===========================================================================
//  settle_pending returns a span into a sim-owned scratch buffer valid ONLY until
//  the next queue()/settle_pending() call. simulate_round_trip therefore COPIES
//  fills[0].price/fee to f64 IMMEDIATELY, before the second settle_pending call
//  invalidates the span.

#include <cmath>  // std::abs (round-trip magnitudes)
#include <span>   // std::span (round-trip observation view + order input)
#include <vector> // std::vector (per-trip ratio scratch)

#include "atx/core/datetime.hpp" // atx::core::time::Timestamp
#include "atx/core/macro.hpp"    // ATX_CHECK (full-fill / no-trade-bar guards)
#include "atx/core/types.hpp"    // atx::f64

#include "atx/engine/eval/stats_ext.hpp"     // eval::median (order-fixed split ratio)
#include "atx/engine/exec/execution_sim.hpp" // ExecutionSimulator
#include "atx/engine/exec/payloads.hpp"      // OrderPayload
#include "atx/engine/loop/market.hpp"        // Market

namespace atx::engine::cost {

// ===========================================================================
//  RoundTrip — the marks + fill captured around a single simulated trade.
//
//  mark_before    : the reference mark just before the fill settles (== the sim's
//                   `ref` at fill time, since the fill reads the live mark).
//  fill_price     : the executed price (slippage + temporary impact, in the PRICE).
//  mark_after_fill: the mark immediately after the fill (moved by PERMANENT impact
//                   only — temporary impact does NOT touch the mark).
//  mark_next_bar  : the mark on a LATER no-trade bar (no new order, no price update)
//                   — must equal mark_after_fill (temp neither leaks nor grows).
//  fee            : the commission charged on the fill (for completeness).
// ===========================================================================
struct RoundTrip {
  atx::f64 mark_before;
  atx::f64 fill_price;
  atx::f64 mark_after_fill;
  atx::f64 mark_next_bar;
  atx::f64 fee;
};

// ===========================================================================
//  simulate_round_trip — drive ONE trade through the EXISTING sim + market.
//
//  Captures the mark before the fill, the fill price/fee, the mark after the fill,
//  and the mark on a later NO-TRADE bar. The order's queued_at must be strictly
//  before settle_t (firewall + latency). DOES NOT call market.update_prices on the
//  no-trade bar, so the mark retains ONLY the permanent shift from the fill — the
//  whole point of the leakage proof.
//
//  PRECONDITION: the order fills FULLY in one slice at settle_t (the caller sizes
//  ADV + bar volume so the volume cap does not bind). ATX_CHECK enforces a single
//  fill before the marks are read (a no-fill would silently read stale marks).
// ===========================================================================
[[nodiscard]] inline RoundTrip simulate_round_trip(exec::ExecutionSimulator &sim, Market &mkt,
                                                   const exec::OrderPayload &order,
                                                   atx::core::time::Timestamp settle_t,
                                                   atx::core::time::Timestamp next_bar_t) {
  const InstrumentId id = order.id;
  const atx::f64 mark_before = mkt.mark(id);

  const std::span<const exec::OrderPayload> one{&order, 1};
  sim.queue(one, order.queued_at);

  const std::span<const exec::FillPayload> fills = sim.settle_pending(settle_t, mkt);
  // The order MUST fill fully on this slice (caller's fixture guarantees the volume
  // cap does not bind). The span borrows the sim's scratch buffer — copy the money
  // fields out NOW, before the next settle_pending call invalidates it.
  ATX_CHECK(fills.size() == 1);
  // STRONGER gate: the SINGLE fill must be the WHOLE order, not a volume-capped
  // partial — a partial would silently make `part = |filled|/adv` smaller than the
  // caller's `|order qty|/adv` and corrupt every expected-value identity downstream.
  const atx::i64 filled_mag = (fills[0].qty < 0) ? -fills[0].qty : fills[0].qty;
  const atx::i64 order_mag = (order.qty < 0) ? -order.qty : order.qty;
  ATX_CHECK(filled_mag == order_mag);
  const atx::f64 fill_price = fills[0].price.to_double();
  const atx::f64 fee = fills[0].fee.to_double();

  const atx::f64 mark_after_fill = mkt.mark(id);

  // A LATER no-trade bar: no new order is queued and the mark is NOT refreshed, so
  // the only orders in flight are gone and the mark keeps just the permanent shift.
  // A temp-leakage bug would re-accrue temp here; a correct sim leaves it untouched.
  const std::span<const exec::FillPayload> none = sim.settle_pending(next_bar_t, mkt);
  ATX_CHECK(none.empty()); // no order is open — nothing may settle on the no-trade bar
  const atx::f64 mark_next_bar = mkt.mark(id);

  return RoundTrip{mark_before, fill_price, mark_after_fill, mark_next_bar, fee};
}

// ===========================================================================
//  fit_split_ratio — perm magnitude / temp magnitude, for reporting.
//
//  Per trip: perm_move = |mark_after_fill − mark_before| (the realized permanent
//  mark shift), temp_move = |fill_price − mark_before| (the realized temporary
//  cost in the price). ratio = perm_move / max(temp_move, eps). Returns the
//  order-fixed MEDIAN of the per-trip ratios (eval::median, deterministic — C5).
//  Empty input -> 0 (no observations). A real fixture (temp ≫ perm) yields a
//  small POSITIVE ratio.
// ===========================================================================
[[nodiscard]] inline atx::f64 fit_split_ratio(std::span<const RoundTrip> trips) {
  // eps guards a degenerate temp_move == 0 (a zero-participation trip); it never
  // bites on a real fill, which always pays some temporary impact.
  constexpr atx::f64 kEps = 1e-12;
  std::vector<atx::f64> ratios;
  ratios.reserve(trips.size());
  for (const RoundTrip &t : trips) {
    const atx::f64 perm_move = std::abs(t.mark_after_fill - t.mark_before);
    const atx::f64 temp_move = std::abs(t.fill_price - t.mark_before);
    const atx::f64 denom = (temp_move > kEps) ? temp_move : kEps;
    ratios.push_back(perm_move / denom);
  }
  return eval::median(std::span<const atx::f64>{ratios});
}

// ===========================================================================
//  should_trade — RenTech ~2-day-hold cost throttle.
//
//  Decline a trade whose modeled EDGE does not clear its modeled round-trip COST
//  (both in bps). `safety` (>= 1) inflates the cost hurdle: a larger safety demands
//  a larger edge before the trade is taken. A trade is taken iff the edge strictly
//  exceeds safety · cost.
// ===========================================================================
[[nodiscard]] inline bool should_trade(atx::f64 expected_edge_bps, atx::f64 predicted_cost_bps,
                                       atx::f64 safety = 1.0) noexcept {
  return expected_edge_bps > safety * predicted_cost_bps;
}

} // namespace atx::engine::cost
