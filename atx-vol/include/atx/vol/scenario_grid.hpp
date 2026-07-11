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
// ## Failed lanes
//
// A unique contract whose Greek solve fails (uid missing from `base`, degenerate
// contract, or a numeric error) is EXCLUDED from every cell and counted ONCE in
// `n_failed`; the surviving book is priced exactly as if that contract were absent.
// No NaN ever enters a cell total.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "atx/vol/american.hpp"         // AmericanGreeks
#include "atx/vol/portfolio_pricer.hpp" // Position, SurfaceSet
#include "atx/vol/types.hpp"            // Result

namespace atx::vol {

// The scenario shocks. `spot_pct` / `vol_bump` are the two grid axes; `dr` / `dt`
// are scalars applied to every cell (see the header conventions).
struct ScenarioGridSpec {
  std::vector<double> spot_pct; // relative spot moves (fraction of S), e.g. {-0.10 .. +0.10}
  std::vector<double> vol_bump; // absolute vol-point shifts, e.g. {-0.05 .. +0.05}
  double dr{0.0};               // rate bump (absolute), applied to every cell
  double dt{0.0};               // time roll (years), applied to every cell
  unsigned n_threads{1};        // fan-out for the Greek solve AND the cell fill
  bool analytic_greeks{false};  // route the Greek solve through the AL analytic path
};

// Per-cell route tag (stored as std::uint8_t in the result). Only `Taylor` ships
// in C3.1; `Exact` is reserved for C3.2's large-bump full re-solve.
enum class ScenarioRoute : std::uint8_t {
  Taylor = 0,
  Exact = 1,
};

// The grid result. `pnl` / `route` are row-major over [i_spot * n_vol + i_vol].
struct ScenarioGridResult {
  std::size_t n_spot{0};
  std::size_t n_vol{0};
  std::vector<double> pnl;         // book-total P&L per cell (position-scaled)
  std::vector<std::uint8_t> route; // ScenarioRoute per cell (all Taylor on this path)
  std::size_t n_ok{0};             // UNIQUE contracts whose Greek solve succeeded
  std::size_t n_failed{0};         // UNIQUE contracts excluded from every cell

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
// @return InvalidArgument if either axis is empty, or the propagated dedup/price
//         error. An empty book yields an all-zero grid (n_ok = n_failed = 0).
[[nodiscard]] Result<ScenarioGridResult> scenario_grid(const std::vector<Position> &book,
                                                       const SurfaceSet &base,
                                                       const ScenarioGridSpec &spec);

} // namespace atx::vol
