#pragma once

// margin.hpp — Reg-T short-option margin + scenario-grid portfolio margin
// (Task B2, backtest-production-lakehouse sprint).
//
// Two independent ways to answer "how much collateral must this book post":
//
//   * `regt_short_option_margin` — the FINRA/NYSE Regulation T formula for ONE
//     naked short listed option, evaluated per contract from its own spot,
//     strike, current premium and multiplier. Closed-form and cheap, so the
//     backtest engine (backtest.cpp) sums it over a book's short lots on every
//     recorded step to populate `BacktestResult::margin_required` — see that
//     field's own comment for the non-wire, empty-or-row-parallel convention it
//     follows (the same one `gross_vega_abs` / `nav_liquidation` already use).
//   * `scenario_margin` — a portfolio-level "worst realistic loss" figure:
//     revalue the WHOLE book over a small spot x vol shock grid and take the
//     worst (most negative) cell as the required collateral. Reuses
//     `scenario_grid.hpp`'s Taylor+exact-repricing machinery — see
//     `MarginScenarioSpec` below — rather than re-deriving a revaluation path
//     of its own; no pricing logic is duplicated here.
//
// `MarginModel` names which of the two produced a given number. It is not
// consumed by either function below — the two functions ARE the entry points
// — and exists purely so a caller/report can TAG a margin figure with its
// provenance (e.g. a tearsheet or a CLI flag choosing between them).
//
// Deliberately independent of the backtest engine: this header must not
// include backtest.hpp, and margin.cpp must not include it either — the
// engine calls INTO these pure functions, never the reverse. See
// margin_test.cpp for the hand-computed Reg-T cases and the scenario_margin
// fixture.

#include <cstdint>
#include <vector>

#include "atx/vol/api/backtest/portfolio_pricer.hpp" // PortfolioPricer, Portfolio, SurfaceSet, Position
#include "atx/vol/api/analytics/scenario_grid.hpp"    // scenario_grid, ScenarioGridSpec, kDefaultTaylorRadius*
#include "atx/vol/api/core/types.hpp"            // Side, Result

namespace atx::vol {

// Tags the model that produced a margin figure. Not consumed by anything in
// this header — see the file header above.
enum class MarginModel : std::uint8_t {
  RegT = 0,
  ScenarioGrid = 1,
};

// Regulation T margin for ONE short (naked) listed option contract, evaluated
// against its own spot/strike/premium/multiplier. The standard FINRA formula:
//
//   OTM   = max(K - S, 0)   for a call     (out-of-the-money amount, >= 0)
//         = max(S - K, 0)   for a put
//   floor = 0.10 * S        for a call     (the ONE call/put asymmetry)
//         = 0.10 * K        for a put
//   margin = max(0.20 * S - OTM, floor) * mult + premium
//
// `premium` is the CONTRACT's current dollar value (per-share mark * `mult`,
// NOT a bare per-share price) so it is dimensionally consistent with the
// `mult`-scaled first term — see margin_test.cpp's hand-computed cases for
// the exact arithmetic. `mult` is used exactly as given, with no defaulting:
// this is a pure formula function, and a book-level caller owns any
// multiplier-sanitization policy (the same division of responsibility
// scenario_grid.cpp's own `eff_multiplier` documents for ITS caller).
//
// Long / flat positions carry no Reg-T maintenance requirement (the premium
// is paid up front, no collateral is owed against it) — this function prices
// only the SHORT side's per-CONTRACT requirement; a book-level caller sums it
// over short lots only and scales by contracts held (see backtest.cpp's
// `book_margin_required`, which does exactly that for the engine's per-step
// column).
[[nodiscard]] double regt_short_option_margin(double spot, double strike, double premium,
                                              double mult, Side side) noexcept;

// The shock grid `scenario_margin` builds internally, reusing
// `scenario_grid.hpp`'s Taylor+exact-repricing machinery — `dr`/`dt`/
// `n_threads`/`analytic_greeks`/the routing radii all pass straight through
// to `ScenarioGridSpec` unchanged. See scenario_grid.hpp's own header for
// what each of those means; this type only fixes the SHAPE of the margin
// grid (a symmetric spot and vol shock around the base) rather than an
// arbitrary axis vector.
//
// Both axes are evaluated at {-shock, 0, +shock} (the base scenario is
// included, so the interior corner is still priced analytically like every
// other scenario_grid consumer, not just the two extremes).
//
// Defaults: +/-15% spot, per the sprint brief. The brief's own "vol +/-"
// clause was truncated before a magnitude — see the sprint's task-B2-brief.md
// line 9 verbatim — so +/-10 (absolute) vol points is used here as a
// documented, conservative, symmetric default; override `vol_shock` when a
// book wants a different PM-style width.
struct MarginScenarioSpec {
  double spot_shock{0.15}; // symmetric relative spot shock (fraction of each surface's own spot)
  double vol_shock{0.10};  // symmetric absolute vol-point shock (see the brief-truncation note above)
  double dr{0.0};
  double dt{0.0};
  unsigned n_threads{1};
  bool analytic_greeks{false};
  double taylor_radius_spot{kDefaultTaylorRadiusSpot};
  double taylor_radius_vol{kDefaultTaylorRadiusVol};
};

// Portfolio-level scenario margin: the worst (most negative) P&L cell of the
// spot x vol grid `spec` describes, revalued via `scenario_grid` against
// `base`, reported as a NON-NEGATIVE dollar requirement (the collateral
// needed to cover the worst modeled loss). A book that only ever GAINS across
// every scenario cell (e.g. a long-only book) has a margin requirement of
// 0.0, never a negative "credit".
//
// `pricer`'s book is extracted via `PortfolioPricer::portfolio().positions()`
// and handed to `scenario_grid` alongside `base` — a `PortfolioPricer` alone
// carries no market data (a `SurfaceSet` is supplied per pricing call, not
// stored), so `base` is the required second input. This function builds a
// `ScenarioGridSpec` from `spec` and calls `scenario_grid(book, base, ...)`
// wholesale; no revaluation logic is duplicated here.
//
// @return whatever `scenario_grid` itself returns: InvalidArgument on a
// malformed grid or an empty book/axis, otherwise the propagated pricer
// error.
[[nodiscard]] Result<double> scenario_margin(const PortfolioPricer &pricer, const SurfaceSet &base,
                                             const MarginScenarioSpec &spec);

} // namespace atx::vol
