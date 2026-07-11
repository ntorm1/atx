// scenario_grid implementation — one Greek solve + analytic per-cell Taylor fill.
//
// The design is deliberately built on the PUBLIC PortfolioPricer::price API so it
// touches no PortfolioPricer / PricingExecutor internals: `price` already dedups
// the book on (uid,K,T,side), solves each unique contract ONCE (the SOTA cold
// Andersen-Lake Greeks bundle), and returns per-position POSITION-SCALED greeks —
// exactly the bundle every cell reconstructs. The only extra machinery here is the
// per-cell analytic fill, fanned over the shared pricing_executor with a serial,
// fixed-position-order reduction inside each cell (so the matrix is bit-identical
// across thread counts, mirroring reduce_pnl_totals' discipline).

#include "atx/vol/scenario_grid.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"         // AmericanGreeks
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/pricing_executor.hpp" // pricing_executor(): the shared P1.4 pool

namespace atx::vol {

Result<ScenarioGridResult> scenario_grid(const std::vector<Position> &book, const SurfaceSet &base,
                                         const ScenarioGridSpec &spec) {
  const std::size_t n_spot = spec.spot_pct.size();
  const std::size_t n_vol = spec.vol_bump.size();
  if (n_spot == 0 || n_vol == 0) {
    return Err(ErrorCode::InvalidArgument,
               "scenario_grid: spot_pct and vol_bump must each have at least one value");
  }

  // ── One Greek solve: dedup + price the book against `base` exactly once. ────
  ATX_TRY(auto pf, Portfolio::create(book));
  const PortfolioPricer pricer(std::move(pf));
  PriceOptions popts;
  popts.n_threads = spec.n_threads;
  popts.analytic_greeks = spec.analytic_greeks; // prices_only stays false => FullGreeks
  ATX_TRY(auto frame, pricer.price(base, popts));

  const Portfolio &portfolio = pricer.portfolio();
  const std::span<const Position> positions = portfolio.positions();
  const std::size_t n_pos = positions.size();
  const std::size_t n_unique = portfolio.n_contracts();
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

  // ── Per-position precompute: Ok flag, per-surface spot, scaled greeks. ──────
  // A position is included in a cell only when its (unique) contract solved Ok;
  // its uid then has a registered surface, so `base.find(uid)->pricing().S` is a
  // finite spot. Failed lanes keep NaN spot but are gated out of every cell.
  std::vector<std::uint8_t> pos_ok(n_pos, 0u);
  std::vector<double> pos_spot(n_pos, kNaN);
  std::vector<AmericanGreeks> pos_greeks(n_pos); // position-scaled (qty*mult*per-share)

  // Per-UNIQUE outcome: every position that shares a contract carries the same
  // status, so counting Ok uniques here (not positions) yields the unique-contract
  // n_ok / n_failed the API reports. `uni_seen` guards the (unreachable) case of a
  // unique with no referencing position.
  std::vector<std::uint8_t> uni_ok(n_unique, 0u);

  for (std::size_t i = 0; i < n_pos; ++i) {
    const bool ok = frame.status[i] == PriceStatus::Ok;
    pos_ok[i] = ok ? 1u : 0u;
    uni_ok[portfolio.contract_ix(i)] = ok ? 1u : 0u;
    if (!ok) {
      continue;
    }
    const PricedSurface *surf = base.find(positions[i].contract.uid);
    // Ok implies the surface was found during the solve; guard defensively anyway.
    pos_spot[i] = (surf != nullptr) ? surf->pricing().S : kNaN;
    pos_greeks[i] = AmericanGreeks{frame.delta[i], frame.gamma[i], frame.vega[i],
                                   frame.theta[i], frame.rho[i],   frame.vanna[i],
                                   frame.volga[i], frame.charm[i], 0.0};
  }

  std::size_t n_ok = 0;
  for (const std::uint8_t v : uni_ok) {
    n_ok += v;
  }

  // ── Result shape. ──────────────────────────────────────────────────────────
  ScenarioGridResult r;
  r.n_spot = n_spot;
  r.n_vol = n_vol;
  const std::size_t n_cells = n_spot * n_vol;
  r.pnl.assign(n_cells, 0.0);
  r.route.assign(n_cells, static_cast<std::uint8_t>(ScenarioRoute::Taylor));
  r.n_ok = n_ok;
  r.n_failed = n_unique - n_ok;

  // ── Per-cell analytic fill (parallel over cells, serial within a cell). ─────
  // Each cell writes its own r.pnl[c] slot (disjoint), and reduces the leg Taylor
  // over positions in fixed input order, so the result is bit-identical for any
  // n_threads. dr/dt are the same scalar in every cell.
  const double dr = spec.dr;
  const double dt = spec.dt;
  const double *spot_pct = spec.spot_pct.data();
  const double *vol_bump = spec.vol_bump.data();
  double *pnl = r.pnl.data();

  pricing_executor().run_blocks(n_cells, spec.n_threads, [&](std::size_t c) {
    const std::size_t i_spot = c / n_vol;
    const std::size_t j_vol = c % n_vol;
    const double sp = spot_pct[i_spot];
    const double dvol = vol_bump[j_vol];
    double acc = 0.0;
    for (std::size_t p = 0; p < n_pos; ++p) {
      if (pos_ok[p] == 0u) {
        continue;
      }
      const double dS = sp * pos_spot[p];
      acc += scenario_taylor_leg(pos_greeks[p], dS, dvol, dt, dr);
    }
    pnl[c] = acc;
  });

  return r;
}

} // namespace atx::vol
