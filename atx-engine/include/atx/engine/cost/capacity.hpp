// cost/capacity.hpp — S6-3: per-alpha + per-mega-book capacity wrappers and
// the capacity POINT (the AUM where net_edge_bps first crosses zero).
//
// C6 — ONE cost model: every public function delegates to risk::capacity_curve,
// which reads sim.impact_cfg().  No impact coefficients live here.
//
// Namespace: atx::engine::cost
// Header-only, #pragma once.
#pragma once

#include <limits> // std::numeric_limits
#include <span>   // std::span
#include <vector> // std::vector

#include "atx/core/macro.hpp" // ATX_CHECK
#include "atx/core/types.hpp" // atx::f64, atx::usize

#include "atx/engine/alpha/streams.hpp"    // alpha::AlphaStreams
#include "atx/engine/exec/execution_sim.hpp" // exec::ExecutionSimulator
#include "atx/engine/loop/panel_types.hpp"   // PanelView
#include "atx/engine/risk/capacity.hpp"      // risk::capacity_curve, risk::CapacityPoint

namespace atx::engine::cost {

// ---------------------------------------------------------------------------
//  capacity_for_book — sweep the P4 combined-book weights through the
//  calibrated sim.  This is a thin C6 wrapper: all impact math lives in
//  risk::capacity_curve.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<risk::CapacityPoint>
capacity_for_book(std::span<const atx::f64> combined_weights, const PanelView& panel,
                  const exec::ExecutionSimulator& sim,
                  std::span<const atx::f64> aum_grid) {
  return risk::capacity_curve(combined_weights, panel, sim, aum_grid); // C6
}

// ---------------------------------------------------------------------------
//  capacity_for_alpha — per-alpha capacity using the LAST-period target
//  weights from the streams.  Delegates to risk::capacity_curve (C6).
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::vector<risk::CapacityPoint>
capacity_for_alpha(const alpha::AlphaStreams& streams, atx::usize alpha_idx,
                   const PanelView& panel, const exec::ExecutionSimulator& sim,
                   std::span<const atx::f64> aum_grid) {
  ATX_CHECK(streams.n_periods() > 0); // deref guard sits outside the condition
  const auto w = streams.positions(alpha_idx, streams.n_periods() - 1U);
  return risk::capacity_curve(w, panel, sim, aum_grid); // C6
}

// ---------------------------------------------------------------------------
//  capacity_point — the AUM where net_edge_bps crosses zero.
//
//  The curve must be monotone NON-INCREASING in AUM (concave impact); this
//  function asserts that invariant (C4 sanity guard).  The zero-crossing is
//  found by bracketing adjacent samples and linearly interpolating in AUM.
//
//  Returns:
//    +inf  — if the curve is empty OR net edge never reaches zero on the grid.
//    curve[0].aum — if the very first point is already <= 0 (capacity at or
//                   below the smallest sampled AUM).
//    linear interpolation in (curve[i-1].aum, curve[i].aum) — first bracket.
// ---------------------------------------------------------------------------
[[nodiscard]] inline atx::f64 capacity_point(std::span<const risk::CapacityPoint> curve) {
  if (curve.empty()) {
    return std::numeric_limits<atx::f64>::infinity();
  }

  // C4 monotonicity guard: non-increasing up to a small FP tolerance.
  for (atx::usize i = 1U; i < curve.size(); ++i) {
    ATX_CHECK(curve[i].net_edge_bps <= curve[i - 1U].net_edge_bps + 1e-6);
  }

  // Already non-positive at the smallest AUM on the grid.
  if (curve[0U].net_edge_bps <= 0.0) {
    return curve[0U].aum;
  }

  // Find the first bracket where net edge crosses from positive to <= 0.
  for (atx::usize i = 1U; i < curve.size(); ++i) {
    if (curve[i].net_edge_bps <= 0.0) {
      // ne[i-1] > 0 >= ne[i]: interpolate the zero-crossing AUM linearly.
      const auto& a = curve[i - 1U];
      const auto& b = curve[i];
      const atx::f64 t = a.net_edge_bps / (a.net_edge_bps - b.net_edge_bps); // in (0,1]
      return a.aum + t * (b.aum - a.aum);
    }
  }

  // Net edge stays positive across the entire grid.
  return std::numeric_limits<atx::f64>::infinity();
}

} // namespace atx::engine::cost
