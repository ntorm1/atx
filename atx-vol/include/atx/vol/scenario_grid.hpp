#pragma once

// scenario_grid — a 2-D (spot% × vol) second-order Taylor P&L scenario matrix
// over a PortfolioPricer book.
//
// ## What it does
//
// Given a book of `Position`s, a base `SurfaceSet` (one surface per underlying),
// and a grid of relative-spot and absolute-vol shocks (plus a single rate and
// time-roll scalar threaded through every cell), `scenario_grid` returns a
// full-book P&L matrix. The per-unique Greek bundle is computed EXACTLY ONCE — one
// deduped `PortfolioPricer::price` solve against `base` — and each grid cell is
// then reconstructed ANALYTICALLY to second order from that single bundle. There
// is no per-cell re-solve on this (Taylor) path, so a whole 11×11 grid costs about
// one full-Greeks price call (the cheap arithmetic per cell is negligible beside
// the cold Andersen-Lake solve).
//
// This is the SAME Taylor expansion `pnl_explain` decomposes a base→shifted move
// with (portfolio_pricer.cpp `scatter_pnl_rows`): delta/gamma on the spot move,
// vega/volga on the vol move, vanna on the cross, theta on the time roll, rho on
// the rate move, charm on the spot×time cross. Here the cell value IS the
// `explained` sum — there is no realized reprice to difference against, so the
// unexplained residual is n/a on this path (C3.2 adds an exact re-solve route for
// large bumps, tagged via the per-cell `route` field).
//
// ## Conventions
//
//   * `spot_pct[i]` is a FRACTION of each surface's OWN spot: dS = spot_pct[i] *
//     S_of_uid. One grid therefore prices a heterogeneous multi-underlying book on
//     a common relative-move axis (a −10%..+10% shock hits every name's own spot).
//   * `vol_bump[j]` is an ABSOLUTE vol-point shift added to sigma (e.g. +0.05).
//   * `dr` / `dt` are single SCALARS applied to every cell — the flagship grid is
//     the 2-D spot×vol matrix; a rate bump / time roll shifts the whole surface
//     uniformly, so they are threaded through, not spread onto their own axes.
//   * All P&L is POSITION-scaled (qty * multiplier * per-share), matching the
//     PriceFrame / PnlFrame columns.
//
// ## Determinism
//
// The one Greek solve is bit-identical across thread counts (PortfolioPricer's
// guarantee), and the cell fill is parallelized over CELLS with a serial,
// fixed-position-order reduction inside each cell (mirroring `reduce_pnl_totals`),
// so every cell — and thus the whole matrix — is bit-identical at any `n_threads`.
//
// The Exact route's shocked reprices (C3.2) run in a preceding pass fanned over
// (unique × vol column) rather than over cells (A7): a PUT's exercise boundary does
// not depend on the spot shock, so one solve serves a whole column. Each task writes
// a disjoint set of (cell, unique) slots, so both the values AND the number of
// boundary solves are properties of the grid shape, not of the thread partition.
//
// ## Failed lanes
//
// A unique contract whose Greek solve fails (uid missing from `base`, degenerate
// contract, or a numeric error) is EXCLUDED from every cell and counted ONCE in
// `n_failed`; the surviving book is priced exactly as if that contract were absent.
// No NaN ever enters a cell total.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/vol/american.hpp"         // AmericanGreeks
#include "atx/vol/portfolio_pricer.hpp" // Position, SurfaceSet
#include "atx/vol/types.hpp"            // Result

namespace atx::vol {

namespace detail {

// Allocation-free sizing seam used by scenario_grid for every multiplied shape
// (result cells, compact exact-price lanes, and executor tasks). Public only so
// overflow boundaries can be pinned without attempting pathological allocations.
[[nodiscard]] constexpr bool scenario_grid_product_is_representable(std::size_t lhs,
                                                                    std::size_t rhs) noexcept {
  return lhs == 0u || rhs <= (std::numeric_limits<std::size_t>::max)() / lhs;
}

} // namespace detail

// ── Taylor↔Exact routing (C3.2) ──────────────────────────────────────────────
//
// A cell whose shock magnitude exceeds a MEASURED Taylor-valid radius is re-solved
// EXACTLY (a no-refit, sticky-strike vol-bump reprice) instead of reconstructed from
// the single second-order Taylor bundle. A cell routes Exact when
//
//     |spot_pct[i]| > taylor_radius_spot  ||  |vol_bump[j]| > taylor_radius_vol
//
// `dr` / `dt` are small scalars applied uniformly to every cell and do NOT
// participate in routing (documented deviation from a full radius test — they shift
// the whole surface, not a per-cell axis magnitude).
//
// The defaults below are the MEASURED PER-AXIS radii: the largest PURE-spot (resp.
// PURE-vol) bump at which the worst-case per-share Taylor residual over the synthetic
// eSSVI board stays <= $0.005 (both land at 3% / 3 vol-pts; the residual jumps past
// the band at the next step — see task-c3.2-report.md req 4). They are pinned by
// ScenarioGrid.DefaultRadiiPinned so a silent change fails a test. NOTE: the bound is
// per-axis — a Taylor cell at the DOUBLE CORNER (|spot| and |vol| both at the radius)
// combines both residuals plus the vanna cross-term and, measured over the FULL eSSVI
// board (wing strikes 80/120, tenor extremes 0.05/0.45 — the same board
// MeasureTaylorRadius uses), can reach ~$0.0111. $0.0125 (worst + ~13% headroom,
// rounded) is the pinned §9.3 gate tolerance (ScenarioGrid.TaylorExactAgreeInsideRadius
// — see task-c3.2-report.md, "Fix: M1 gate board widening"). Setting a radius to
// `std::numeric_limits<double>::infinity()` DISABLES routing on that axis: `|x| > inf`
// is always false, so an inf/inf spec reproduces the C3.1 all-Taylor grid BYTE-for-byte
// (pinned by ScenarioGrid.InfiniteRadiusIsByteIdenticalToTaylorOnly).
//
// ## Exact cell semantics — sticky-strike, NO smile roll
//
// Per unique contract, the BASE surface is resolved ONCE (rp = surface.resolve(K,T))
// and the base price is P0 = american_price(S, K, T, rp.sigma, rp.rate, rp.q_eff,
// side, ...) with (S, method, al_opts) from surface.pricing() — bit-identical to
// surface.fair_value(K,T,side). The shocked reprice HOLDS the base-resolved sigma and
// bumps only the pricer inputs:
//
//     S'     = S * (1 + spot_pct[i])
//     sigma' = max(rp.sigma + vol_bump[j], kSigmaFloor)   // floor documented in .cpp
//     r'     = rp.rate + dr
//     T'     = max(T - dt, kMinT)                          // clamp documented in .cpp
//     q'     = rp.q_eff                                    // carry held (sticky strike)
//     P'     = american_price(S', K, T', sigma', r', q', side, ...)
//
// This is a STICKY-STRIKE vol bump: the surface is NOT re-fit and the smile is NOT
// rolled to the new spot — the cell answers "reprice THIS contract at base-resolved
// sigma + bump under the shocked market inputs", the honest scenario counterpart of
// the Taylor bundle. Cell P&L is the SAME position-scaled, fixed-input-order sum as
// the Taylor path, with the per-position leg replaced by (P' - P0) * qty *
// eff_multiplier (P' shared across all positions on one unique; P0 amortized).
//
// A unique whose shocked solve Errs / returns non-finite (e.g. a negative-carry
// corner surfaced by dr, or a boundary collapse) FALLS BACK to its Taylor leg for
// that cell — the cell's route STAYS Exact — and is counted once in
// ScenarioGridResult::n_exact_fallback_lanes. No NaN ever enters a cell total.
inline constexpr double kDefaultTaylorRadiusSpot = 0.03; // fraction of spot (MEASURED, req 4)
inline constexpr double kDefaultTaylorRadiusVol = 0.03;  // absolute vol points (MEASURED, req 4)

// The scenario shocks. `spot_pct` / `vol_bump` are the two grid axes; `dr` / `dt`
// are scalars applied to every cell (see the header conventions).
struct ScenarioGridSpec {
  std::vector<double> spot_pct; // relative spot moves (fraction of S), e.g. {-0.10 .. +0.10}
  std::vector<double> vol_bump; // absolute vol-point shifts, e.g. {-0.05 .. +0.05}
  double dr{0.0};               // rate bump (absolute), applied to every cell
  double dt{0.0};               // time roll (years), applied to every cell
  unsigned n_threads{1};        // fan-out for the Greek solve AND the cell fill
  bool analytic_greeks{false};  // route the Greek solve through the AL analytic path
  // Exact re-solve routing radii (C3.2). A cell whose |spot_pct| exceeds
  // `taylor_radius_spot` OR whose |vol_bump| exceeds `taylor_radius_vol` is re-solved
  // exactly. Default = the measured radii (routing ON). Set either to infinity() to
  // disable routing on that axis (reproduces C3.1's all-Taylor grid byte-identically).
  double taylor_radius_spot{kDefaultTaylorRadiusSpot};
  double taylor_radius_vol{kDefaultTaylorRadiusVol};
};

// Per-cell route tag (stored as std::uint8_t in the result). `Taylor` cells are
// reconstructed analytically from the one Greek bundle; `Exact` cells (C3.2, large
// bumps) are re-solved per unique via `american_price` (see the routing comment).
enum class ScenarioRoute : std::uint8_t {
  Taylor = 0,
  Exact = 1,
};

// The grid result. `pnl` / `route` are row-major over [i_spot * n_vol + i_vol].
struct ScenarioGridResult {
  std::size_t n_spot{0};
  std::size_t n_vol{0};
  std::vector<double> pnl;         // book-total P&L per cell (position-scaled)
  std::vector<std::uint8_t> route; // ScenarioRoute per cell
  std::size_t n_ok{0};             // UNIQUE contracts whose Greek solve succeeded
  std::size_t n_failed{0};         // UNIQUE contracts excluded from every cell
  // (cell × unique) exact re-solves that Erred / went non-finite and fell back to the
  // unique's Taylor leg for that cell (the cell's route stays Exact). Counted once per
  // failing unique per Exact cell; 0 whenever routing is disabled or every solve holds.
  std::size_t n_exact_fallback_lanes{0};
  // Number of doubles in the compact Exact-price scratch. This is exactly
  // (Exact cells × Greek-successful unique contracts), not (all cells × all
  // uniques); exposed as a stable working-set diagnostic for production-sized grids.
  std::size_t n_exact_price_scratch_slots{0};

  [[nodiscard]] std::size_t n_cells() const noexcept { return pnl.size(); }
};

// Second-order Taylor P&L for ONE leg whose greeks `g` are POSITION-SCALED
// (qty * multiplier * per-share), under a shock of (dS absolute spot move, dvol
// absolute vol points, dt years, dr rate). The `price` field of `g` is ignored.
//
// The term set and left-to-right operation order are PINNED to
// portfolio_pricer.cpp's `scatter_pnl_rows` (delta·dS, ½·gamma·dS², vega·dvol,
// ½·volga·dvol², vanna·dS·dvol, theta·dt, rho·dr, charm·dS·dt), so a grid cell
// reconstructs `pnl_explain`'s `explained` sum term-for-term. This is the single
// source of truth for the Taylor kernel: the grid impl and its tests both call it.
[[nodiscard]] inline double scenario_taylor_leg(const AmericanGreeks &g, double dS, double dvol,
                                                double dt, double dr) noexcept {
  const double pd = g.delta * dS;
  const double pg = 0.5 * g.gamma * dS * dS;
  const double pv = g.vega * dvol;
  const double pvol = 0.5 * g.volga * dvol * dvol;
  const double pvanna = g.vanna * dS * dvol;
  const double pth = g.theta * dt;
  const double prho = g.rho * dr;
  const double pcharm = g.charm * dS * dt;
  return pd + pg + pv + pvol + pvanna + pth + prho + pcharm;
}

// Build the Taylor scenario grid: dedup `book` and price it against `base` ONCE
// (one Greek solve), then reconstruct every (spot_pct[i], vol_bump[j]) cell
// analytically to second order. `spec.dr` / `spec.dt` are applied to every cell.
//
// @return InvalidArgument if either axis is empty or a result/scratch/task shape
//         overflows size_t (or exceeds a vector's element capacity), otherwise the
//         propagated dedup/price error. An empty book yields an all-zero grid
//         (n_ok = n_failed = 0).
[[nodiscard]] Result<ScenarioGridResult> scenario_grid(const std::vector<Position> &book,
                                                       const SurfaceSet &base,
                                                       const ScenarioGridSpec &spec);

} // namespace atx::vol
