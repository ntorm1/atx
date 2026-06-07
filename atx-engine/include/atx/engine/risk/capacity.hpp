#pragma once

// atx::engine::risk — the capacity curve `capacity_curve` (P4-10a).
//
// ===========================================================================
//  What this unit is — RenTech §9.6: report capacity, not just return
// ===========================================================================
//  For a fitted book `weights`, capacity_curve sweeps a grid of AUM levels and
//  reports, per AUM, the NET frictionless edge AFTER market-impact erosion. As
//  the book is scaled to a larger AUM each name trades a larger share of its ADV,
//  so the √-impact cost grows and the net edge falls — eventually crossing zero.
//  That zero-crossing is the CAPACITY of the book: the AUM at which √-impact has
//  eroded the gross edge to nothing. A strategy that reports return without
//  reporting capacity hides the fact that the return does not survive being sized
//  up; this unit makes that trade-off explicit (§5 sketch + §8 P4-10).
//
// ===========================================================================
//  §0-G — ONE COST SURFACE (the hard constraint)
// ===========================================================================
//  The √-impact MUST read the SAME exec::ExecutionSimulator cost model the rest
//  of the engine uses — there is NO second/duplicate cost path here. The sim's
//  temporary √-impact formula (exec/execution_sim.hpp, struct ImpactCfg) is
//      temp_i = Y * sigma_i * part_i^delta        // fraction of notional
//  with part_i = shares_i / ADV_i. This unit reads the sim's OWN coefficients via
//  the additive, read-only ExecutionSimulator::impact_cfg() accessor (mirroring
//  commission_cfg()) and applies them at research cadence WITHOUT replaying the
//  bar-by-bar queue/settle loop. The COEFFICIENTS are the sim's; only the sizing
//  arithmetic lives here.
//
// ===========================================================================
//  Scope — capacity is scoped to the √-impact (temporary-impact) term
// ===========================================================================
//  Per RT §9.6 / the §5 comment ("√-impact erodes net edge"), capacity is bounded
//  by the SIZE-DEPENDENT cost term: the temporary √-impact. Commission (≈scale-
//  free in bps — the same fractional charge at any AUM) and slippage are
//  INTENTIONALLY NOT folded into the capacity erosion: they do not bound capacity
//  because they do not grow with participation the way √-impact does. Folding a
//  scale-free term in would only shift the whole curve by a constant and obscure
//  the size-dependent erosion this unit exists to measure.
//
// ===========================================================================
//  The capacity model (decidable contract)
// ===========================================================================
//  For a book `weights` (length M, universe-aligned; a NaN or 0 weight contributes
//  nothing), over a `PanelView panel` (OHLCV-only, newest-first; row 0 = current):
//
//  1. Gross frictionless edge (the thing impact erodes), in bps:
//       gross_edge_bps = 1e4 * mean_{r in [0,W_edge)} ( Σ_i w[i] * ret_i(r) )
//     ret_i(r) = close(r,i)/close(r+1,i) − 1 is the P4-6 per-step return (reused
//     via detail::step_return). PIT/structural: reads only rows >= r (history). A
//     NaN return drops that (r,i) term from the cross-sectional sum.
//  2. Per-name √-impact cost at a given `aum`:
//       price_i    = close(0,i)                          (current mark)
//       notional_i = aum * |w[i]|                        (book gross-scaled to aum)
//       shares_i   = notional_i / price_i
//       ADV_i      = mean_{r in [0,W_adv)} close(r,i)*volume(r,i)  (dollar ADV)
//       part_i     = shares_i / ADV_i
//       sigma_i    = popstd_{r in [0,W_vol)} ret_i(r)    (return volatility)
//       temp_i     = Y * sigma_i * part_i^delta          (sim's OWN coefficients)
//     Cost in bps as a fraction of gross notional (notional_i ∝ |w[i]|, so the
//     notional-weighted mean impact is):
//       cost_bps(aum) = 1e4 * Σ_i |w[i]| * temp_i
//  3. Net edge: net_edge_bps(aum) = gross_edge_bps − cost_bps(aum).
//  4. One CapacityPoint{aum, net_edge_bps} per aum_grid entry, in grid order.
//
//  CONTRACT pins: net edge is non-increasing across ascending AUM (cost grows);
//  a positive-edge book crosses zero on a wide grid (the capacity point); at
//  AUM→0 net ≈ gross (impact→0); a larger sim ImpactCfg.Y -> strictly larger cost
//  (proves the one-cost-surface read); same inputs -> bit-identical output.
//
// ===========================================================================
//  Windows (Phase-5 calibrates these; named constants, NOT magic numbers)
// ===========================================================================
//  kAdvWindow = 20 (the P4-6 adv20 convention), kVolWindow = 60 (the P4-6 vol
//  convention). The edge window uses ALL usable trailing returns (kEdgeWindow is
//  the panel's available return history, capped by rows()-1). EVERY window is
//  GUARDED against the panel's available rows: a return-based window over [0,W)
//  needs row W to exist (close(W-1)/close(W) is the oldest term), so W is clamped
//  to rows()-1. A short test panel therefore never reads OOB (PanelView ABORTS in
//  debug, reads stale ring in release — these guards must hold under NDEBUG).
//
// ===========================================================================
//  Determinism / cost ethos
// ===========================================================================
//  NO RNG. Every reduction runs in canonical ascending (row, instrument) order.
//  This is a COLD, research-cadence call (one sweep per fitted book), so a few
//  scratch allocations and re-scans across the AUM grid are fine — there is no
//  hot-path concern. Same (weights, panel, sim, aum_grid) -> byte-identical out.

#include <cmath>   // std::isnan, std::pow (part^delta), std::sqrt, std::fabs
#include <span>    // std::span (weights + aum_grid inputs)
#include <vector>  // std::vector (cold-path scratch + output)

#include "atx/core/types.hpp" // atx::f64, atx::usize

#include "atx/engine/exec/execution_sim.hpp" // ExecutionSimulator, ImpactCfg (§0-G)
#include "atx/engine/loop/panel_types.hpp"   // PanelView
#include "atx/engine/risk/exposures.hpp"     // detail::step_return (the P4-6 return)
#include "atx/engine/risk/fwd.hpp"           // CapacityPoint fwd decl

namespace atx::engine::risk {

// ===========================================================================
//  CapacityPoint — one (AUM, net-edge) sample on the capacity curve.
//  Matches the fwd.hpp forward declaration (a struct).
// ===========================================================================
struct CapacityPoint {
  atx::f64 aum;          // the swept AUM (book gross-scaled to this dollar size)
  atx::f64 net_edge_bps; // gross frictionless edge minus √-impact cost, in bps
};

namespace detail {

// Per-name cost-model windows. Phase-5 calibrates these; named so they are not
// magic numbers. kAdvWindow / kVolWindow mirror the P4-6 adv20 / vol conventions.
inline constexpr atx::usize kCapacityAdvWindow = 20U; // dollar-ADV lookback (adv20)
inline constexpr atx::usize kCapacityVolWindow = 60U; // return-volatility lookback

// Greatest return-window length W usable from row 0: a return at row r needs row
// r+1 to exist, so the oldest term ret(W-1) needs row W -> W <= rows()-1. Returns
// 0 when fewer than 2 rows exist (no valid return at all). Caps any requested
// window so a short panel never reads OOB (PanelView aborts in debug).
[[nodiscard]] inline atx::usize usable_return_window(const PanelView &panel,
                                                     atx::usize want) noexcept {
  const atx::usize rows = panel.rows();
  if (rows < 2U) {
    return 0U; // need at least two rows for one per-step return
  }
  const atx::usize avail = rows - 1U; // ret(0..avail-1) are addressable
  return (want < avail) ? want : avail;
}

// Population stddev of the newest `w` per-step returns of instrument i, skipping
// NaN terms (a NaN return drops that observation). Returns 0 when fewer than two
// valid returns remain (no spread to measure) — keeps sigma finite and non-NaN.
[[nodiscard]] inline atx::f64 return_volatility(const PanelView &panel, atx::usize i,
                                                atx::usize w) noexcept {
  atx::f64 sum = 0.0;
  atx::usize n = 0U;
  for (atx::usize r = 0U; r < w; ++r) { // ascending row -> order-fixed reduction
    const atx::f64 ret = step_return(panel, r, i);
    if (!std::isnan(ret)) {
      sum += ret;
      ++n;
    }
  }
  if (n < 2U) {
    return 0.0; // degenerate: no (or single) observation -> no measurable vol
  }
  const atx::f64 mean = sum / static_cast<atx::f64>(n);
  atx::f64 ss = 0.0;
  for (atx::usize r = 0U; r < w; ++r) {
    const atx::f64 ret = step_return(panel, r, i);
    if (!std::isnan(ret)) {
      const atx::f64 d = ret - mean;
      ss += d * d;
    }
  }
  return std::sqrt(ss / static_cast<atx::f64>(n)); // population std
}

// Dollar ADV of instrument i: mean over the newest `w` rows of close*volume.
// Skips rows whose close or volume is NaN. Returns 0 when no valid row exists (a
// zero ADV is guarded by the caller -> that name contributes no impact).
[[nodiscard]] inline atx::f64 dollar_adv(const PanelView &panel, atx::usize i,
                                         atx::usize w) noexcept {
  atx::f64 sum = 0.0;
  atx::usize n = 0U;
  for (atx::usize r = 0U; r < w; ++r) {
    const atx::f64 c = panel.close(r, i);
    const atx::f64 v = panel.volume(r, i);
    if (!std::isnan(c) && !std::isnan(v)) {
      sum += c * v;
      ++n;
    }
  }
  return (n == 0U) ? 0.0 : sum / static_cast<atx::f64>(n);
}

// Gross frictionless edge in bps: 1e4 * mean over the edge window of the
// cross-sectional book return Σ_i w[i]*ret_i(r). A NaN weight or NaN return drops
// that term. Rows with no contributing term are skipped (do not bias the mean).
[[nodiscard]] inline atx::f64 gross_edge_bps(std::span<const atx::f64> weights,
                                             const PanelView &panel,
                                             atx::usize w_edge) noexcept {
  if (w_edge == 0U) {
    return 0.0; // no usable return history -> no measurable edge
  }
  const atx::usize n_inst = panel.instruments();
  atx::f64 sum_rows = 0.0;
  atx::usize n_rows = 0U;
  for (atx::usize r = 0U; r < w_edge; ++r) { // ascending row -> order-fixed
    atx::f64 row_ret = 0.0;
    bool any = false;
    for (atx::usize i = 0U; i < n_inst && i < weights.size(); ++i) { // ascending inst
      const atx::f64 wi = weights[i];
      if (std::isnan(wi) || wi == 0.0) {
        continue; // dead name contributes nothing (no NaN leak)
      }
      const atx::f64 ret = step_return(panel, r, i);
      if (!std::isnan(ret)) {
        row_ret += wi * ret;
        any = true;
      }
    }
    if (any) {
      sum_rows += row_ret;
      ++n_rows;
    }
  }
  return (n_rows == 0U) ? 0.0 : 1.0e4 * (sum_rows / static_cast<atx::f64>(n_rows));
}

// √-impact cost in bps at `aum`: 1e4 * Σ_i |w[i]| * (Y * sigma_i * part_i^delta),
// reading the sim's OWN ImpactCfg coefficients (§0-G). A dead/NaN weight, a
// non-positive price, or a zero ADV makes a name contribute nothing (no NaN/Inf
// leak, no div-by-zero). part^delta is std::pow.
[[nodiscard]] inline atx::f64 impact_cost_bps(std::span<const atx::f64> weights,
                                              const PanelView &panel,
                                              const exec::ImpactCfg &impact, atx::f64 aum,
                                              atx::usize w_adv, atx::usize w_vol) noexcept {
  const atx::usize n_inst = panel.instruments();
  atx::f64 cost = 0.0;
  for (atx::usize i = 0U; i < n_inst && i < weights.size(); ++i) { // ascending inst
    const atx::f64 wi = weights[i];
    if (std::isnan(wi) || wi == 0.0) {
      continue; // dead name -> no notional, no impact (no NaN leak)
    }
    const atx::f64 abs_w = std::fabs(wi);
    const atx::f64 price = panel.close(0U, i); // current mark (row 0 = newest)
    if (std::isnan(price) || price <= 0.0) {
      continue; // unpriced / non-positive mark -> cannot size shares; skip
    }
    const atx::f64 adv = dollar_adv(panel, i, w_adv);
    if (adv <= 0.0) {
      continue; // no traded value -> participation undefined; contributes nothing
    }
    const atx::f64 notional = aum * abs_w;        // book gross-scaled to this aum
    const atx::f64 shares = notional / price;     // share count at the current mark
    const atx::f64 part = shares / adv;           // participation = shares / ADV
    const atx::f64 sigma = return_volatility(panel, i, w_vol);
    if (part <= 0.0 || sigma <= 0.0) {
      continue; // zero participation or zero vol -> temp = 0 (matches the sim)
    }
    const atx::f64 temp = impact.Y * sigma * std::pow(part, impact.delta);
    cost += abs_w * temp; // notional-weighted (notional_i ∝ |w[i]|)
  }
  return 1.0e4 * cost;
}

} // namespace detail

// ===========================================================================
//  capacity_curve — sweep AUM through the sim's √-impact, report net edge (P4-10a).
//
//  For each `aum_grid` entry (in grid order) returns one CapacityPoint{aum,
//  net_edge_bps} where net_edge_bps = gross_edge_bps − impact_cost_bps(aum). The
//  gross edge is computed ONCE (AUM-independent); only the impact cost depends on
//  AUM. Reads the sim's own ImpactCfg coefficients (§0-G — one cost surface). An
//  empty aum_grid yields an empty vector; a NaN/0-weight name contributes nothing;
//  a short panel degenerates to zero edge/cost (windows clamp; no OOB/div-by-zero).
//  PURE given (weights, panel, sim, aum_grid); NO RNG; bit-deterministic.
// ===========================================================================
[[nodiscard]] inline std::vector<CapacityPoint>
capacity_curve(std::span<const atx::f64> weights, const PanelView &panel,
               const exec::ExecutionSimulator &sim, std::span<const atx::f64> aum_grid) {
  std::vector<CapacityPoint> out;
  out.reserve(aum_grid.size());

  // Clamp every window to the panel's usable trailing history so a short panel
  // never reads OOB. The edge window uses ALL usable returns; ADV/vol use the
  // P4-6 conventions, each clamped (ADV is row-based; vol is return-based).
  const atx::usize w_edge = detail::usable_return_window(panel, panel.rows());
  const atx::usize w_vol = detail::usable_return_window(panel, detail::kCapacityVolWindow);
  const atx::usize w_adv_cap = (panel.rows() < detail::kCapacityAdvWindow)
                                   ? panel.rows()
                                   : detail::kCapacityAdvWindow;

  // Gross edge is AUM-independent — compute once, reuse for every grid point.
  const atx::f64 gross = detail::gross_edge_bps(weights, panel, w_edge);
  const exec::ImpactCfg &impact = sim.impact_cfg(); // §0-G: the sim's OWN coefficients

  for (const atx::f64 aum : aum_grid) {
    const atx::f64 cost = detail::impact_cost_bps(weights, panel, impact, aum, w_adv_cap, w_vol);
    out.push_back(CapacityPoint{aum, gross - cost});
  }
  return out;
}

} // namespace atx::engine::risk
