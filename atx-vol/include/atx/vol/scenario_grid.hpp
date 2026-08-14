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
//
// ## Thread-safety (plan 4.7)
//
// `scenario_grid` is a pure function of its inputs: it reads the caller's
// positions and the base `SurfaceSet` (both concurrent-const-safe) and returns a
// freshly built matrix, holding no state of its own between calls. Two grids may
// therefore run concurrently over the SAME book and surfaces.
//
// The fan-outs described above are dispatched onto the PROCESS-GLOBAL pricing pool
// (detail/pricing_executor.hpp), not onto threads this entry creates, so two
// concurrent grids share one core budget rather than oversubscribing — and a grid
// issued from INSIDE another pool dispatch runs inline. Determinism is unaffected
// either way, per the section above. The pool's own caller-facing rule applies
// here as everywhere: choose its topology with `configure_pricing_executor` before
// the first pricing call builds it, after which configuration is refused with
// AlreadyExists.

#include <cmath> // std::isfinite: the deriv leg's NaN gating
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "atx/vol/american.hpp"         // AmericanGreeks
#include "atx/vol/deriv_book.hpp"       // DerivPosition (Tier-A, closure-safe)
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

  // ── Vol-derivative leg (Task F-8, GK-G4) ─────────────────────────────────
  //
  // EMPTY on every result from the option-only overload, so nothing an existing
  // caller reads moves: `pnl` stays the OPTION book's total and this is the
  // swap book's, per cell, same row-major layout. A caller wanting the whole
  // book adds them. `deriv_route` is the per-cell route tag for this leg,
  // routed by the SAME radii as the option leg.
  std::vector<double> deriv_pnl;
  std::vector<std::uint8_t> deriv_route;
  std::size_t n_deriv_ok{0};      // deriv positions contributing to every cell
  std::size_t n_deriv_failed{0};  // deriv positions whose base greek solve failed
  // Deriv positions that PRICED but carry "not computed" (NaN) in a sensitivity
  // this grid's own shocks would read -- a contract too short to roll against a
  // non-zero `dt`, or a smile shock against a book priced without
  // `smile_greeks`. Excluded from every cell and counted HERE rather than in
  // `n_deriv_failed`, because the two have different fixes: a failed solve is a
  // broken position, this is a grid asking for an axis nothing measured.
  //
  // Counted rather than contributed as 0.0. A zero would read as "measured, and
  // this position has no exposure to that axis", which is exactly the confusion
  // Task F-7 round 1 removed one layer down.
  //
  // ok + failed + missing_sensitivity == deriv_book.size(), always.
  std::size_t n_deriv_missing_sensitivity{0};
  // (cell x position) Exact reprices that Erred or went non-finite and fell
  // back to that position's Taylor leg for that cell. The cell's route stays
  // Exact, mirroring the option leg's own fallback accounting.
  std::size_t n_deriv_exact_fallback_lanes{0};

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

// ── Vol-derivative leg (Task F-8, GK-G4) ────────────────────────────────────
//
// The shocks a swap book sees that an option book does not. `spot_pct` /
// `vol_bump` / `dr` / `dt` are the grid's own, shared with the option leg; these
// two are SCALARS applied to every cell, exactly like `dr`/`dt`, because a
// smile rotation shifts the whole surface rather than indexing a cell axis.
//
// A variance swap's defining risk is the SHAPE of the smile -- the strip
// integrates every strike, so a rotation moves K_var even when the ATM vol does
// not budge -- which is why these exist at all and why the option leg has no
// counterpart. Both are zero by default, and at zero the deriv Taylor cell is
// term-for-term the option kernel.
struct ScenarioDerivSpec {
  double d_skew{0.0};       // absolute smile-slope shift, vol per unit k = ln(K/F)
  double d_convexity{0.0};  // absolute smile-curvature shift, vol per unit k^2
};

// Second-order Taylor P&L for ONE deriv position whose greeks `g` are
// POSITION-SCALED, under the same shock the option kernel takes plus the two
// smile shifts.
//
// The first eight terms are `scenario_taylor_leg`'s, in ITS left-to-right order,
// so the two legs of one grid reconstruct the same expansion; the two smile
// terms are appended. `g.pv` and the carry-theta fields are ignored -- this is a
// market-move expansion, and the fixing rollover a swap also earns over `dt` is
// `DerivPnlExplain`'s job, not a scenario cell's.
//
// ── EVERY TERM IS SHOCK-GATED, AND THAT IS ONE RULE, NOT TEN ───────────────
//
// A term whose SHOCK is exactly zero contributes exactly zero WITHOUT READING
// its sensitivity. `NaN * 0.0` is NaN, not 0, and a `DerivGreeks` legitimately
// carries "not computed" as NaN in SIX of these ten slots on documented
// conditions:
//
//   theta, charm             a contract shorter than `bumps.time_years`
//   vanna, charm             `DerivGreekBumps::second_order` off
//   skew_vega, convexity_vega`DerivGreekBumps::smile_greeks` off (the default)
//
// so an unguarded product poisons the WHOLE cell -- all ten terms -- on a grid
// that never asked for that axis. Task F-7 round 1 guarded the two smile terms
// after `price_deriv_book` stopped fabricating a 0.0 skew vega for memoized
// VarSwap rows. That guard was correct and incomplete: the same argument covers
// theta, charm and vanna verbatim, and a rule applied to two of six slots is
// the shape this sprint keeps re-finding. It is now the single rule for all
// ten, expressed identically at each term rather than as a special case beside
// eight bare multiplies.
//
// This is bit-identical to an unguarded product whenever the sensitivity is
// finite: the only value it changes is a `-0.0` term (a negative sensitivity
// times a zero shock) into `+0.0`, which can only alter a sum in which EVERY
// term is `-0.0` -- and that sum is zero either way.
//
// A NON-ZERO shock against a NaN sensitivity still propagates NaN, deliberately.
// The caller asked for an axis nothing measured, and there is no answer to give;
// `scenario_grid`'s deriv leg detects that case BEFORE pricing and excludes the
// position rather than letting the NaN reach a cell (see
// `ScenarioGridResult::n_deriv_missing_sensitivity`).
[[nodiscard]] inline double scenario_deriv_taylor_leg(const DerivGreeks &g, double dS, double dvol,
                                                      double dt, double dr, double d_skew,
                                                      double d_convexity) noexcept {
  const double pd = (dS == 0.0) ? 0.0 : g.delta * dS;
  const double pg = (dS == 0.0) ? 0.0 : 0.5 * g.gamma * dS * dS;
  const double pv = (dvol == 0.0) ? 0.0 : g.vega * dvol;
  const double pvol = (dvol == 0.0) ? 0.0 : 0.5 * g.volga * dvol * dvol;
  const double pvanna = (dS == 0.0 || dvol == 0.0) ? 0.0 : g.vanna * dS * dvol;
  const double pth = (dt == 0.0) ? 0.0 : g.theta * dt;
  const double prho = (dr == 0.0) ? 0.0 : g.rho * dr;
  const double pcharm = (dS == 0.0 || dt == 0.0) ? 0.0 : g.charm * dS * dt;
  const double pskew = (d_skew == 0.0) ? 0.0 : g.skew_vega * d_skew;
  const double pconv = (d_convexity == 0.0) ? 0.0 : g.convexity_vega * d_convexity;
  return pd + pg + pv + pvol + pvanna + pth + prho + pcharm + pskew + pconv;
}

// Which `DerivGreeks` slots a grid carrying these shocks will actually read.
// The mirror of the gating above, and deliberately the same disjunctions: a
// term is read iff its shock is non-zero, so this is the exact predicate under
// which a NaN sensitivity would reach a cell.
//
// `any_spot` / `any_vol` are "does ANY cell on that axis carry a non-zero
// shock", because one grid shares one included/excluded verdict across all its
// cells -- a position that contributes to some cells and not others would make
// the matrix unreadable as a surface.
[[nodiscard]] inline bool scenario_deriv_greeks_sufficient(const DerivGreeks &g, bool any_spot,
                                                           bool any_vol, bool has_dt, bool has_dr,
                                                           const ScenarioDerivSpec &d) noexcept {
  const auto ok = [](double v) noexcept { return std::isfinite(v); };
  if (any_spot && !(ok(g.delta) && ok(g.gamma))) return false;
  if (any_vol && !(ok(g.vega) && ok(g.volga))) return false;
  if (any_spot && any_vol && !ok(g.vanna)) return false;
  if (has_dt && !ok(g.theta)) return false;
  if (any_spot && has_dt && !ok(g.charm)) return false;
  if (has_dr && !ok(g.rho)) return false;
  if (d.d_skew != 0.0 && !ok(g.skew_vega)) return false;
  if (d.d_convexity != 0.0 && !ok(g.convexity_vega)) return false;
  return true;
}

// The same grid over an option book AND a vol-derivative book.
//
// The option leg is bit-identical to the overload above given the same `book` /
// `base` / `spec` -- it is literally the same code path -- so this is an
// additive route, not a replacement. The deriv leg prices each position's
// greeks ONCE against `base` (`price_deriv_book`) and reconstructs each cell
// from that one bundle, routing to an exact reprice on the same radii the
// option leg uses.
//
// ## Exact deriv cell semantics -- sticky-strike, NO smile roll
//
// `detail::deriv_price_shocked_on_ref` reprices the contract with the base
// surface read through the shocked overlay: the surface is NOT re-fit, and the
// smile is NOT rolled to the new spot. Identical in spirit to the option leg's
// Exact cell, and identical in mechanism to every `deriv_greeks` spot bump --
// which is what makes a Taylor cell and an Exact cell two views of one model
// rather than two models.
//
// Two deliberate differences from the option leg's Exact cell, stated because
// they are differences: the rate shock is applied as an exact discount rescale
// rather than a curve bump (every kind here has `rho = -T*pv` analytically, so
// the rescale IS the reprice), and `dt` rolls the CALENDAR ONLY -- no fixing is
// injected, matching `DerivGreeks::theta`.
//
// `smile_greeks` is forced ON for the deriv greek solve whenever either smile
// shock is non-zero, because a NaN `skew_vega` would otherwise poison the whole
// Taylor cell rather than the term the caller asked for.
//
// @return the same error contract as the overload above. A deriv position whose
//         greek solve fails is excluded from every cell and counted in
//         `n_deriv_failed`; no NaN enters a cell total.
[[nodiscard]] Result<ScenarioGridResult> scenario_grid(const std::vector<Position> &book,
                                                       const std::vector<DerivPosition> &deriv_book,
                                                       const SurfaceSet &base,
                                                       const ScenarioGridSpec &spec,
                                                       const ScenarioDerivSpec &deriv_spec = {});

} // namespace atx::vol
