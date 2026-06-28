// cost/capacity.hpp — S6-3: per-alpha + per-mega-book capacity wrappers and
// the capacity POINT (the AUM where net_edge_bps first crosses zero).
//
// C6 — ONE cost model: every public function delegates to risk::capacity_curve,
// which reads sim.impact_cfg().  No impact coefficients live here.
//
// Namespace: atx::engine::cost
// Header-only, #pragma once.
#pragma once

#include <cmath>  // std::pow, std::exp, std::log (log-spaced AUM grid)
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

// ---------------------------------------------------------------------------
//  compute_capacity_vector — the per-alpha capacity-AUM vector (P7-S4-2).
//
//  Returns, for each alpha a in [0, streams.n_alphas()) in ASCENDING order, the
//  capacity AUM of that alpha's LAST-period target book — the AUM where the net
//  frictionless edge crosses zero (capacity_point of the swept net-edge curve).
//  This is the real per-name capacity vector that replaces the constant-1.0 stub
//  in the driver's decorrelate_weights blend (S4 proves the math; S7 wires the
//  driver call site — this header adds NO new cost model, REUSING capacity_for_alpha
//  -> risk::capacity_curve and capacity_point verbatim).
//
//  AUM grid: a log-spaced grid of kCapacityAumGridPoints points from
//  0.01*target_aum to 10*target_aum (two decades each side of the operational AUM
//  — wide enough to bracket the zero-crossing in typical cases). The grid is a pure
//  deterministic function of target_aum (same target_aum -> same grid -> same
//  output); ascending alpha order, no RNG -> bit-deterministic.
//
//  Per-alpha capacity AUM semantics (the downstream cap_scale contract):
//    * +inf  — the grid never crossed zero (the book has spare capacity at 10x the
//              target). The caller's cap_scale = clamp(capacity/floor, 0, 1)
//              clamps +inf to 1.0 (no penalty) — the existing contract.
//    * 0     — the last-period book has no positive frictionless edge
//              (capacity_point returns grid[0] which, on a positive grid, is only
//              reached when net edge is already <= 0 at the smallest AUM; for a
//              dead book gross<=0 so cap_scale -> ~0, fading it out).
//    * finite in (0, +inf) — the interpolated zero-crossing AUM.
//
//  LIMITATION (documented, inherited from capacity_for_alpha): the book proxy is
//  the LAST-period target weights (streams.positions(a, n_periods-1)), NOT a
//  realized-PnL edge estimate. The p6-S6-1 realized-edge refinement is a driver-
//  layer concern, out of scope for this engine-layer helper (see the S4 plan
//  "capacity_for_alpha uses last-period weights" guardrail).
//
//  PRECONDITION: streams.n_periods() > 0 (ATX_CHECK in capacity_for_alpha). A
//  non-positive target_aum yields a degenerate grid (all points <= 0); the curve
//  then has no positive edge and capacity_point returns grid[0] (<= 0) — the caller
//  must pass target_aum > 0 (the driver guards this, as it does for the stub path).
//  PURE given (streams, panel, sim, target_aum); NO RNG; thread-safe (no shared
//  state). Header-only inline — a cold, research-cadence call.
// ---------------------------------------------------------------------------
inline constexpr atx::usize kCapacityAumGridPoints = 20U; // log-spaced grid resolution

[[nodiscard]] inline std::vector<atx::f64>
compute_capacity_vector(const alpha::AlphaStreams& streams, const PanelView& panel,
                        const exec::ExecutionSimulator& sim, atx::f64 target_aum) {
  // Log-spaced AUM grid from 0.01*target_aum to 10*target_aum, ascending. Built
  // once and reused for every alpha (the grid depends only on target_aum). For a
  // single grid point the loop degenerates to the lone endpoint.
  std::vector<atx::f64> aum_grid;
  aum_grid.reserve(kCapacityAumGridPoints);
  const atx::f64 lo = 0.01 * target_aum;
  const atx::f64 hi = 10.0 * target_aum;
  if (kCapacityAumGridPoints == 1U) {
    aum_grid.push_back(lo);
  } else {
    // Geometric spacing: aum_k = lo * (hi/lo)^(k/(N-1)). Computed via exp/log so the
    // ratio is exact at the endpoints (k=0 -> lo, k=N-1 -> hi) for lo > 0.
    const atx::f64 log_lo = std::log(lo);
    const atx::f64 log_hi = std::log(hi);
    const atx::f64 denom = static_cast<atx::f64>(kCapacityAumGridPoints - 1U);
    for (atx::usize k = 0U; k < kCapacityAumGridPoints; ++k) {
      const atx::f64 frac = static_cast<atx::f64>(k) / denom;
      aum_grid.push_back(std::exp(log_lo + frac * (log_hi - log_lo)));
    }
  }

  const atx::usize n_alphas = streams.n_alphas();
  std::vector<atx::f64> out;
  out.reserve(n_alphas);
  for (atx::usize a = 0U; a < n_alphas; ++a) { // ascending alpha order (determinism)
    const std::vector<risk::CapacityPoint> curve =
        capacity_for_alpha(streams, a, panel, sim, std::span<const atx::f64>{aum_grid});
    out.push_back(capacity_point(std::span<const risk::CapacityPoint>{curve}));
  }
  return out;
}

} // namespace atx::engine::cost
