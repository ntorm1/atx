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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/american.hpp"         // AmericanGreeks, american_price, AlOpts
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/pricing_executor.hpp" // pricing_executor(): the shared P1.4 pool

#include "american_boundary.hpp" // amer:: boundary seam (A7 spot-axis reuse)

namespace atx::vol {

namespace {

constexpr double kNaNv = std::numeric_limits<double>::quiet_NaN();

// Effective deliverable, mirroring portfolio_pricer.cpp `eff_multiplier`: a
// non-finite / non-positive multiplier defaults to 100. The Taylor path already
// carries this weight baked into its position-scaled greeks (PortfolioPricer::price
// scales by qty * eff_multiplier), so the Exact path MUST apply the identical weight
// to its (P' - P0) so the two routes agree inside the radius.
[[nodiscard]] double eff_multiplier(double m) noexcept {
  return (std::isfinite(m) && m > 0.0) ? m : 100.0;
}

// Shocked-reprice guards (C3.2). A sub-floor sigma or a past-expiry T is clamped so
// the pricer stays in its valid domain; both collapse toward intrinsic, matching the
// degenerate-input policy american_price already applies internally.
inline constexpr double kSigmaFloor = 1.0e-4; // min sigma' after a large negative vol bump
inline constexpr double kMinT = 1.0e-6;       // min T' after a dt time roll past expiry

// Per-unique base state for the Exact route: the once-resolved base surface point,
// the base price P0, and the pricer scalars the shocked reprice reuses. Built only
// for uniques whose Greek solve succeeded (uni_ok); `ready` records that the base
// reprice itself produced a finite P0.
struct UniExact {
  double S{0.0};
  double K{0.0};
  double T{0.0};
  double sigma{0.0};
  double rate{0.0};
  double q_eff{0.0};
  double P0{0.0};
  Side side{Side::Call};
  AmericanMethod method{AmericanMethod::AndersenLake};
  AlOpts al_opts{};
  bool ready{false};
  // A7 (GR-P3-S): may this unique's shocked exercise boundary be solved ONCE per
  // vol column and reused across the whole spot axis? See `boundary_spot_invariant`.
  bool reuse_boundary{false};
  amer::AlScheme sch{}; // == scheme_from_opts(al_opts), resolved once per unique
};

// A7: is this unique's SHOCKED internal-put boundary independent of the spot shock?
//
// american.cpp's S-independence seam: the Andersen-Lake boundary is a function of
// (K, T, sigma, r, q) and NOT of the spot — al_solve_put is exactly
// al_solve_put_boundary + al_put_price_from_boundary(.., S, ..). Within ONE grid,
// K / q / T' = T-dt / r' = r+dr are the same in every cell and only sigma moves with
// the vol axis, so a PUT (internal-put strike = K) has one boundary per vol column.
//
// A CALL is the McDonald-Schroder internal put P(K, S', q, r'): its internal-put
// STRIKE is the shocked spot S', so its boundary moves with the spot axis too. Its
// boundary is homogeneity-reusable in R but only to a few ULP in IEEE, which would
// shift served values — so calls deliberately keep the cold per-cell solve.
//
// Every condition here is cell-INVARIANT (Tp/rp are grid scalars); the per-cell
// preconditions andersen_lake_core also checks (S' > 0, sigma' > 1e-8) are re-checked
// at the point of use, and any lane that misses one falls back to `american_price`
// itself, so the fast path is entered only where it reproduces it bit-for-bit.
[[nodiscard]] bool boundary_spot_invariant(const UniExact &ue, double Tp, double rp) noexcept {
  return ue.method == AmericanMethod::AndersenLake && ue.side == Side::Put && ue.K > 0.0 &&
         Tp > 1.0e-12 && std::isfinite(rp) && std::isfinite(ue.q_eff) &&
         detail::classify_regime(/*rate=*/rp, /*yield=*/ue.q_eff) ==
             detail::ExerciseRegime::American;
}

// The pre-A7 shocked reprice, verbatim: one cold `american_price` per (cell, unique).
// This is the oracle the reuse path must reproduce, and the fallback for every lane
// the reuse path declines.
[[nodiscard]] double shocked_price_cold(const UniExact &ue, double Sp, double sig, double rp,
                                        double Tp) noexcept {
  const Result<double> px = american_price(Sp, ue.K, Tp, sig, rp, ue.q_eff, ue.side, ue.method,
                                           std::optional<AlOpts>{ue.al_opts});
  return (px && std::isfinite(*px)) ? *px : kNaNv;
}

} // namespace

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
  // Exact-route per-position bookkeeping: the unique index this position reprices
  // through, and its weight qty * eff_multiplier (the SAME scale the Taylor greeks
  // already carry — see eff_multiplier above).
  std::vector<std::uint32_t> pos_uni(n_pos, 0u);
  std::vector<double> pos_w(n_pos, 0.0);

  // Per-UNIQUE outcome: every position that shares a contract carries the same
  // status, so counting Ok uniques here (not positions) yields the unique-contract
  // n_ok / n_failed the API reports.
  std::vector<std::uint8_t> uni_ok(n_unique, 0u);

  for (std::size_t i = 0; i < n_pos; ++i) {
    const bool ok = frame.status[i] == PriceStatus::Ok;
    pos_ok[i] = ok ? 1u : 0u;
    pos_uni[i] = portfolio.contract_ix(i);
    uni_ok[pos_uni[i]] = ok ? 1u : 0u;
    if (!ok) {
      continue;
    }
    pos_w[i] = positions[i].qty * eff_multiplier(positions[i].multiplier);
    const SurfaceRef surf = base.find(positions[i].contract.uid);
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

  // ── Per-unique Exact-route base state (resolve + P0 once per unique). ────────
  // Only built when routing can actually fire (some cell exceeds a radius); with
  // routing fully disabled (inf/inf) this stays empty and the grid is pure Taylor,
  // byte-identical to C3.1. Resolving/pricing the base ONCE here amortizes P0 across
  // every Exact cell (each cell then costs one shocked solve per unique).
  const double rad_spot = spec.taylor_radius_spot;
  const double rad_vol = spec.taylor_radius_vol;
  bool any_exact = false;
  for (std::size_t i = 0; i < n_spot && !any_exact; ++i) {
    any_exact = std::abs(spec.spot_pct[i]) > rad_spot;
  }
  for (std::size_t j = 0; j < n_vol && !any_exact; ++j) {
    any_exact = std::abs(spec.vol_bump[j]) > rad_vol;
  }

  const std::span<const OptionContract> contracts = portfolio.contracts();
  std::vector<UniExact> uni;
  if (any_exact) {
    uni.assign(n_unique, UniExact{});
    for (std::size_t u = 0; u < n_unique; ++u) {
      if (uni_ok[u] == 0u) {
        continue; // already a n_failed lane; excluded from every cell
      }
      const OptionContract &oc = contracts[u];
      const SurfaceRef surf = base.find(oc.uid);
      if (surf == nullptr) {
        continue; // defensive: uni_ok implies a registered surface
      }
      const PricedSurface::ResolvedSurfacePoint rp = surf->resolve(oc.K, oc.T);
      if (!rp.valid) {
        continue; // ready stays false => Exact cells fall back to Taylor for this unique
      }
      const PricingContext &pc = surf->pricing();
      UniExact &ue = uni[u];
      ue.S = pc.S;
      ue.K = oc.K;
      ue.T = oc.T;
      ue.sigma = rp.sigma;
      ue.rate = rp.rate;
      ue.q_eff = rp.q_eff;
      ue.side = oc.side;
      ue.method = pc.method;
      ue.al_opts = pc.al_opts;
      // Base price per share: reproduces surf->fair_value(K,T,side) exactly (pinned
      // by ScenarioGrid.BaseRepriceMatchesFairValue). Computed once, shared by all
      // Exact cells for this unique.
      const Result<double> p0 = american_price(pc.S, oc.K, oc.T, rp.sigma, rp.rate, rp.q_eff,
                                               oc.side, pc.method, std::optional<AlOpts>{pc.al_opts});
      if (p0 && std::isfinite(*p0)) {
        ue.P0 = *p0;
        ue.ready = true;
      }
      // A7: resolve the AL scheme once (andersen_lake_core does scheme_from_opts on
      // every call) and decide, from the CELL-INVARIANT shocked contract, whether the
      // spot axis can share one boundary solve.
      ue.sch = amer::scheme_from_opts(std::optional<AlOpts>{pc.al_opts});
      const double Tp_grid = std::max(oc.T - spec.dt, kMinT);
      const double rate_grid = rp.rate + spec.dr;
      ue.reuse_boundary = boundary_spot_invariant(ue, Tp_grid, rate_grid);
    }
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

  // ── Per-cell fill (parallel over cells, serial within a cell). ──────────────
  // Each cell writes its own disjoint slots and reduces over positions in fixed
  // input order, so the whole matrix is bit-identical for any n_threads. A cell is
  // Exact when its shock exceeds a radius; otherwise Taylor. dr/dt are the same
  // scalar in every cell and do not gate routing. `cell_fallback[c]` records the
  // per-cell exact-solve fallbacks; summing it after the parallel section (integer
  // add, order-independent) keeps n_exact_fallback_lanes deterministic across threads.
  const double dr = spec.dr;
  const double dt = spec.dt;
  const double *spot_pct = spec.spot_pct.data();
  const double *vol_bump = spec.vol_bump.data();
  double *pnl = r.pnl.data();
  std::uint8_t *route = r.route.data();
  std::vector<std::size_t> cell_fallback(n_cells, 0u);
  std::size_t *fallback = cell_fallback.data();

  const auto is_exact = [&](double sp, double dvol) noexcept {
    return (std::abs(sp) > rad_spot) || (std::abs(dvol) > rad_vol);
  };

  // ── Phase A (A7 / GR-P3-S): the shocked reprices, HOISTED out of the cell loop. ──
  //
  // Pre-A7 this ran inside the per-cell body: one cold `american_price` per
  // (Exact cell x Ok unique), so every cell re-solved every unique's exercise
  // boundary from a Barone-Adesi-Whaley seed even though — for a put — the boundary
  // is the SAME object in every cell of a vol column (see `boundary_spot_invariant`).
  //
  // The fan-out is now over (unique x VOL COLUMN): task t = u*n_vol + j solves that
  // unique's boundary once and prices every spot shock of column j against it. Task t
  // writes only slots (i, j, u), which are disjoint across tasks, so the matrix is
  // bit-identical at any n_threads AND the solve count is a property of the grid
  // shape rather than of the thread partition (pinned by
  // ScenarioGrid.ExactArmSolveCountIsThreadInvariant). The (unique x column) grain is
  // also wider than the old per-cell grain on any realistic book.
  //
  // `pprime_all` costs one double per (cell, unique) — i.e. one double per shocked
  // solve the grid was already going to perform — so it cannot dominate the work it
  // indexes. Allocated only when routing can actually fire.
  std::vector<double> pprime_all; // [cell][unique] row-major; NaN => fallback lane
  if (any_exact) {
    pprime_all.assign(n_cells * n_unique, kNaN);
    double *pp = pprime_all.data();
    pricing_executor().run_blocks(n_unique * n_vol, spec.n_threads, [&](std::size_t t) {
      const std::size_t u = t / n_vol;
      const std::size_t j = t % n_vol;
      if (uni_ok[u] == 0u) {
        return; // excluded unique; leaves NaN, exactly as the pre-A7 loop did
      }
      const UniExact &ue = uni[u];
      if (!ue.ready) {
        return; // base reprice failed -> NaN -> counted once per Exact cell in Phase B
      }
      const double dvol = vol_bump[j];
      const double sig = std::max(ue.sigma + dvol, kSigmaFloor);
      const double rp_rate = ue.rate + dr;
      const double Tp = std::max(ue.T - dt, kMinT);

      // Reuse arm: ONE boundary for the whole column. al_solve_put_boundary +
      // al_put_price_from_boundary IS al_solve_put's American branch, and both take
      // bnd/ws by const reference on the price side, so each spot shock reproduces
      // `american_price` bit-for-bit (ScenarioGrid.ExactCellsMatchColdPerCellOracleBitwise).
      if (ue.reuse_boundary && sig > 1.0e-8 && Tp > 1.0e-12) {
        amer::AlBoundary bnd;
        amer::AlWorkspace ws;
        if (amer::al_solve_put_boundary(ue.K, Tp, sig, rp_rate, ue.q_eff, ue.sch, bnd, ws) ==
            amer::AlSolveStatus::Ok) {
          for (std::size_t i = 0; i < n_spot; ++i) {
            const double sp = spot_pct[i];
            if (!is_exact(sp, dvol)) {
              continue; // Taylor cell — no reprice is owed
            }
            const double Sp = ue.S * (1.0 + sp);
            if (!(Sp > 0.0)) {
              continue; // american_price would return InvalidArgument -> fallback lane
            }
            const double v =
                amer::al_put_price_from_boundary(bnd, ws, Sp, ue.K, Tp, sig, rp_rate, ue.q_eff);
            if (std::isfinite(v)) {
              pp[(i * n_vol + j) * n_unique + u] = v;
            }
          }
          return;
        }
        // Collapsed / table-missing: fall through so the lane takes american_price's
        // own error handling (Err -> NaN -> fallback), unchanged.
      }

      // Cold arm: the pre-A7 path, one `american_price` per Exact cell of this column.
      for (std::size_t i = 0; i < n_spot; ++i) {
        const double sp = spot_pct[i];
        if (!is_exact(sp, dvol)) {
          continue;
        }
        const double v = shocked_price_cold(ue, ue.S * (1.0 + sp), sig, rp_rate, Tp);
        if (std::isfinite(v)) {
          pp[(i * n_vol + j) * n_unique + u] = v;
        }
      }
    });
  }

  const double *pprime_base = pprime_all.empty() ? nullptr : pprime_all.data();

  pricing_executor().run_blocks(n_cells, spec.n_threads, [&](std::size_t c) {
    const std::size_t i_spot = c / n_vol;
    const std::size_t j_vol = c % n_vol;
    const double sp = spot_pct[i_spot];
    const double dvol = vol_bump[j_vol];
    const bool exact = (std::abs(sp) > rad_spot) || (std::abs(dvol) > rad_vol);

    if (!exact) {
      // Taylor cell: second-order reconstruction from the one Greek bundle.
      double acc = 0.0;
      for (std::size_t p = 0; p < n_pos; ++p) {
        if (pos_ok[p] == 0u) {
          continue;
        }
        const double dS = sp * pos_spot[p];
        acc += scenario_taylor_leg(pos_greeks[p], dS, dvol, dt, dr);
      }
      pnl[c] = acc;
      route[c] = static_cast<std::uint8_t>(ScenarioRoute::Taylor);
      return;
    }

    // Exact cell — Phase A already filled this cell's row of shocked reprices (one
    // per Ok unique, P0 amortized). A unique whose base wasn't ready or whose shocked
    // solve failed left NaN there; count those once for this cell. The tally is an
    // integer add over a fixed index range, so it is order-independent and matches
    // the pre-A7 in-line count lane for lane.
    const double *pprime = pprime_base + c * n_unique;
    std::size_t nfb = 0;
    for (std::size_t u = 0; u < n_unique; ++u) {
      if (uni_ok[u] == 0u) {
        continue; // excluded unique; its positions are gated by pos_ok below
      }
      if (!std::isfinite(pprime[u])) {
        ++nfb;
      }
    }

    // Phase B: reduce over positions in fixed input order. A position whose unique
    // repriced falls to (P' - P0) * weight; a fallback position takes its Taylor leg.
    double acc = 0.0;
    for (std::size_t p = 0; p < n_pos; ++p) {
      if (pos_ok[p] == 0u) {
        continue;
      }
      const std::uint32_t u = pos_uni[p];
      if (std::isfinite(pprime[u])) {
        acc += (pprime[u] - uni[u].P0) * pos_w[p];
      } else {
        const double dS = sp * pos_spot[p];
        acc += scenario_taylor_leg(pos_greeks[p], dS, dvol, dt, dr);
      }
    }
    pnl[c] = acc;
    route[c] = static_cast<std::uint8_t>(ScenarioRoute::Exact);
    fallback[c] = nfb;
  });

  std::size_t n_fallback = 0;
  for (const std::size_t v : cell_fallback) {
    n_fallback += v;
  }
  r.n_exact_fallback_lanes = n_fallback;

  return r;
}

} // namespace atx::vol
