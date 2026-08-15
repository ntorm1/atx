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

#include "atx/vol/api/analytics/scenario_grid.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"         // AmericanGreeks, american_price, AlOpts
#include "atx/vol/api/backtest/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/api/pricing/pricing_executor.hpp" // pricing_executor(): the shared P1.4 pool

#include "analytics/scenario_grid_detail.hpp" // detail::scenario_grid_product_is_representable
#include "pricing/american_boundary.hpp" // amer:: boundary seam (A7 spot-axis reuse)
#include "pricing/deriv_ref_bridge.hpp" // Task F-8: deriv_price_shocked_on_ref

namespace atx::vol {

namespace {

constexpr double kNaNv = std::numeric_limits<double>::quiet_NaN();
constexpr std::size_t kNoIndex = (std::numeric_limits<std::size_t>::max)();

template <class T> [[nodiscard]] bool vector_count_is_representable(std::size_t count) noexcept {
  return count <= std::vector<T>{}.max_size();
}

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
  if (!detail::scenario_grid_product_is_representable(n_spot, n_vol)) {
    return Err(ErrorCode::InvalidArgument,
               "scenario_grid: spot_pct x vol_bump cell count overflows size_t");
  }
  const std::size_t n_cells = n_spot * n_vol;
  if (!vector_count_is_representable<double>(n_cells) ||
      !vector_count_is_representable<std::uint8_t>(n_cells)) {
    return Err(ErrorCode::InvalidArgument,
               "scenario_grid: result cell count exceeds vector element capacity");
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

  // Compact successful-unique columns are stable because Portfolio contracts and
  // this scan are both deterministic. Failed uniques never need an Exact-price
  // slot: their positions are excluded from every cell.
  std::vector<std::size_t> exact_col_by_unique(n_unique, kNoIndex);
  std::vector<std::size_t> ok_unique;
  ok_unique.reserve(n_ok);
  for (std::size_t u = 0; u < n_unique; ++u) {
    if (uni_ok[u] != 0u) {
      exact_col_by_unique[u] = ok_unique.size();
      ok_unique.push_back(u);
    }
  }

  // ── Per-unique Exact-route base state (resolve + P0 once per unique). ────────
  // Only built when routing can actually fire (some cell exceeds a radius); with
  // routing fully disabled (inf/inf) this stays empty and the grid is pure Taylor,
  // byte-identical to C3.1. Resolving/pricing the base ONCE here amortizes P0 across
  // every Exact cell (each cell then costs one shocked solve per unique).
  const double rad_spot = spec.taylor_radius_spot;
  const double rad_vol = spec.taylor_radius_vol;
  // The routing predicate factors by axis. Build deterministic compact indexes:
  //   * exact_cols stores only vol columns containing at least one Exact cell;
  //   * exact_row_offset + vol_exact_rank maps each Exact cell to a dense row in
  //     the same row-major order as the public result.
  // This avoids a dense n_cells-sized size_t map while still making every Phase A
  // destination a pure function of its task and spot indexes.
  std::vector<std::uint8_t> spot_exact(n_spot, 0u);
  bool spot_axis_exact = false;
  for (std::size_t i = 0; i < n_spot; ++i) {
    if (std::abs(spec.spot_pct[i]) > rad_spot) {
      spot_exact[i] = 1u;
      spot_axis_exact = true;
    }
  }

  std::vector<std::size_t> vol_exact_rank(n_vol, kNoIndex);
  std::size_t n_exact_vol = 0;
  for (std::size_t j = 0; j < n_vol; ++j) {
    if (std::abs(spec.vol_bump[j]) > rad_vol) {
      vol_exact_rank[j] = n_exact_vol++;
    }
  }

  std::vector<std::size_t> exact_cols;
  exact_cols.reserve(spot_axis_exact ? n_vol : n_exact_vol);
  for (std::size_t j = 0; j < n_vol; ++j) {
    if (spot_axis_exact || vol_exact_rank[j] != kNoIndex) {
      exact_cols.push_back(j);
    }
  }

  std::vector<std::size_t> exact_row_offset(n_spot, 0u);
  std::size_t n_exact_cells = 0;
  for (std::size_t i = 0; i < n_spot; ++i) {
    exact_row_offset[i] = n_exact_cells;
    const std::size_t row_width = spot_exact[i] != 0u ? n_vol : n_exact_vol;
    // n_exact_cells is mathematically <= the already-checked n_cells; keep the
    // guard local so a future routing change cannot invalidate that proof.
    if (row_width > n_cells - n_exact_cells) {
      return Err(ErrorCode::Internal, "scenario_grid: compact Exact row count overflow");
    }
    n_exact_cells += row_width;
  }
  const bool any_exact = n_exact_cells != 0u;

  if (!detail::scenario_grid_product_is_representable(n_exact_cells, n_ok)) {
    return Err(ErrorCode::InvalidArgument,
               "scenario_grid: Exact cells x successful uniques overflows size_t");
  }
  const std::size_t n_exact_price_slots = n_exact_cells * n_ok;
  if (!vector_count_is_representable<double>(n_exact_price_slots)) {
    return Err(ErrorCode::InvalidArgument,
               "scenario_grid: compact Exact-price scratch exceeds vector element capacity");
  }
  if (!detail::scenario_grid_product_is_representable(exact_cols.size(), n_ok)) {
    return Err(ErrorCode::InvalidArgument,
               "scenario_grid: Exact columns x successful uniques task count overflows size_t");
  }
  const std::size_t n_exact_tasks = exact_cols.size() * n_ok;
  if (any_exact && !vector_count_is_representable<UniExact>(n_ok)) {
    return Err(ErrorCode::InvalidArgument,
               "scenario_grid: Exact unique state exceeds vector element capacity");
  }

  const std::span<const OptionContract> contracts = portfolio.contracts();
  std::vector<UniExact> uni;
  if (any_exact) {
    uni.assign(n_ok, UniExact{});
    for (std::size_t ok_col = 0; ok_col < n_ok; ++ok_col) {
      const std::size_t u = ok_unique[ok_col];
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
      UniExact &ue = uni[ok_col];
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
      const Result<double> p0 =
          american_price(pc.S, oc.K, oc.T, rp.sigma, rp.rate, rp.q_eff, oc.side, pc.method,
                         std::optional<AlOpts>{pc.al_opts});
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
  r.pnl.assign(n_cells, 0.0);
  r.route.assign(n_cells, static_cast<std::uint8_t>(ScenarioRoute::Taylor));
  r.n_ok = n_ok;
  r.n_failed = n_unique - n_ok;
  r.n_exact_price_scratch_slots = n_exact_price_slots;

  // ── Per-cell fill (parallel over cells, serial within a cell). ──────────────
  // Each cell writes its own disjoint slots and reduces over positions in fixed
  // input order, so the whole matrix is bit-identical for any n_threads. A cell is
  // Exact when its shock exceeds a radius; otherwise Taylor. dr/dt are the same
  // scalar in every cell and do not gate routing. `exact_fallback[row]` records only
  // Exact-cell fallbacks; summing it after the parallel section (integer add,
  // order-independent) keeps n_exact_fallback_lanes deterministic across threads.
  const double dr = spec.dr;
  const double dt = spec.dt;
  const double *spot_pct = spec.spot_pct.data();
  const double *vol_bump = spec.vol_bump.data();
  double *pnl = r.pnl.data();
  std::uint8_t *route = r.route.data();
  std::vector<std::size_t> exact_fallback(n_exact_cells, 0u);
  std::size_t *fallback = exact_fallback.data();

  const auto is_exact = [&](std::size_t i, std::size_t j) noexcept {
    return spot_exact[i] != 0u || vol_exact_rank[j] != kNoIndex;
  };
  const auto exact_row_of = [&](std::size_t i, std::size_t j) noexcept {
    const std::size_t col_rank = spot_exact[i] != 0u ? j : vol_exact_rank[j];
    return exact_row_offset[i] + col_rank;
  };

  // ── Phase A (A7 / GR-P3-S): the shocked reprices, HOISTED out of the cell loop. ──
  //
  // Pre-A7 this ran inside the per-cell body: one cold `american_price` per
  // (Exact cell x Ok unique), so every cell re-solved every unique's exercise
  // boundary from a Barone-Adesi-Whaley seed even though — for a put — the boundary
  // is the SAME object in every cell of a vol column (see `boundary_spot_invariant`).
  //
  // The fan-out is over (successful unique x Exact-bearing vol column). Each task
  // solves that unique's boundary once and prices every Exact spot shock in that
  // column against it. Tasks write disjoint compact (Exact row, successful unique)
  // slots, so the matrix is
  // bit-identical at any n_threads AND the solve count is a property of the grid
  // shape rather than of the thread partition (pinned by
  // ScenarioGrid.ExactArmSolveCountIsThreadInvariant). The (unique x column) grain is
  // also wider than the old per-cell grain on any realistic book.
  //
  // `pprime_all` is compact [Exact cell][successful unique] scratch. Exact rows keep
  // public row-major order via exact_row_of(); successful-unique columns keep
  // Portfolio contract order. Taylor cells and failed uniques therefore consume no
  // slots, while each destination remains deterministic and single-writer.
  std::vector<double> pprime_all; // NaN => fallback lane
  if (any_exact) {
    pprime_all.assign(n_exact_price_slots, kNaN);
    if (n_exact_tasks != 0u) {
      double *pp = pprime_all.data();
      pricing_executor().run_blocks(n_exact_tasks, spec.n_threads, [&](std::size_t t) {
        const std::size_t ok_col = t / exact_cols.size();
        const std::size_t compact_j = t % exact_cols.size();
        const std::size_t j = exact_cols[compact_j];
        const UniExact &ue = uni[ok_col];
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
              if (!is_exact(i, j)) {
                continue; // Taylor cell — no reprice is owed
              }
              const double Sp = ue.S * (1.0 + sp);
              if (!(Sp > 0.0)) {
                continue; // american_price would return InvalidArgument -> fallback lane
              }
              const double v =
                  amer::al_put_price_from_boundary(bnd, ws, Sp, ue.K, Tp, sig, rp_rate, ue.q_eff);
              if (std::isfinite(v)) {
                pp[exact_row_of(i, j) * n_ok + ok_col] = v;
              }
            }
            return;
          }
          // Collapsed / table-missing / frozen-sweep NotConverged: fall through so the
          // lane takes american_price's own error handling (Err -> NaN -> fallback),
          // unchanged.
        }

        // Cold arm: the pre-A7 path, one `american_price` per Exact cell of this column.
        for (std::size_t i = 0; i < n_spot; ++i) {
          const double sp = spot_pct[i];
          if (!is_exact(i, j)) {
            continue;
          }
          const double v = shocked_price_cold(ue, ue.S * (1.0 + sp), sig, rp_rate, Tp);
          if (std::isfinite(v)) {
            pp[exact_row_of(i, j) * n_ok + ok_col] = v;
          }
        }
      });
    }
  }

  const double *pprime_base = pprime_all.empty() ? nullptr : pprime_all.data();

  pricing_executor().run_blocks(n_cells, spec.n_threads, [&](std::size_t c) {
    const std::size_t i_spot = c / n_vol;
    const std::size_t j_vol = c % n_vol;
    const double sp = spot_pct[i_spot];
    const double dvol = vol_bump[j_vol];
    const bool exact = is_exact(i_spot, j_vol);

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
    const std::size_t exact_row = exact_row_of(i_spot, j_vol);
    const double *pprime = n_ok == 0u ? nullptr : pprime_base + exact_row * n_ok;
    std::size_t nfb = 0;
    for (std::size_t ok_col = 0; ok_col < n_ok; ++ok_col) {
      if (!std::isfinite(pprime[ok_col])) {
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
      const std::size_t ok_col = exact_col_by_unique[u];
      if (std::isfinite(pprime[ok_col])) {
        acc += (pprime[ok_col] - uni[ok_col].P0) * pos_w[p];
      } else {
        const double dS = sp * pos_spot[p];
        acc += scenario_taylor_leg(pos_greeks[p], dS, dvol, dt, dr);
      }
    }
    pnl[c] = acc;
    route[c] = static_cast<std::uint8_t>(ScenarioRoute::Exact);
    fallback[exact_row] = nfb;
  });

  std::size_t n_fallback = 0;
  for (const std::size_t v : exact_fallback) {
    n_fallback += v;
  }
  r.n_exact_fallback_lanes = n_fallback;

  return r;
}

// ── Vol-derivative leg (Task F-8, GK-G4) ────────────────────────────────────

namespace {

// Fill `out`'s deriv columns in place. Kept OUT of the option entry above and
// bolted on by the overload below rather than woven into it: the option leg is
// then not merely "unchanged", it is the same call, and
// `DerivLegLeavesTheOptionLegByteIdentical` can say so against a byte compare
// instead of against a reading of the diff.
[[nodiscard]] Result<void> fill_deriv_leg(ScenarioGridResult &out,
                                          const std::vector<DerivPosition> &deriv_book,
                                          const SurfaceSet &base, const ScenarioGridSpec &spec,
                                          const ScenarioDerivSpec &deriv_spec) {
  if (deriv_book.empty()) {
    return core::Ok();  // no leg, no columns -- distinguishable from a flat one
  }
  const std::size_t n_spot = spec.spot_pct.size();
  const std::size_t n_vol = spec.vol_bump.size();
  const std::size_t n_cells = n_spot * n_vol;
  const std::size_t n_pos = deriv_book.size();
  if (!detail::scenario_grid_product_is_representable(n_cells, n_pos)) {
    return Err(ErrorCode::InvalidArgument,
               "scenario_grid: deriv cell x position count overflows size_t");
  }

  // One greek solve for the whole deriv book, mirroring the option leg's single
  // `PortfolioPricer::price`. `smile_greeks` costs four extra repricings per
  // position and is only worth paying for when a smile shock is actually set --
  // but without it `skew_vega` is NaN, which would poison the entire Taylor
  // cell rather than just its smile term.
  DerivGreekBumps bumps{};
  bumps.smile_greeks = (deriv_spec.d_skew != 0.0) || (deriv_spec.d_convexity != 0.0);
  bumps.time_years = (spec.dt > 0.0) ? spec.dt : bumps.time_years;
  const DerivConfig cfg{};
  ATX_TRY(const DerivPriceFrame frame,
          price_deriv_book(base, std::span<const DerivPosition>{deriv_book}, cfg, true, bumps));
  if (frame.rows.size() != n_pos) {
    return Err(ErrorCode::Internal, "scenario_grid: deriv frame lost a row");
  }

  std::vector<std::uint8_t> pos_ok(n_pos, 0u);
  std::vector<double> pos_spot(n_pos, kNaNv);
  for (std::size_t p = 0; p < n_pos; ++p) {
    if (frame.rows[p].status != PriceStatus::Ok) {
      continue;
    }
    const SurfaceRef surf = base.find(deriv_book[p].uid);
    if (surf == nullptr || !(surf->pricing().S > 0.0)) {
      continue;  // Ok implies a surface, but a cell must never see a NaN spot
    }
    pos_ok[p] = 1u;
    pos_spot[p] = surf->pricing().S;
  }

  // Which axes this grid will actually read. A position that priced but carries
  // "not computed" in one of those slots cannot be scenario'd along it, and is
  // excluded HERE -- before any cell is touched -- rather than allowed to
  // propagate a NaN into a total this result promises is finite. See
  // `scenario_deriv_greeks_sufficient` for why the predicate is the exact
  // mirror of the Taylor kernel's own shock gating.
  bool any_spot = false;
  for (const double s : spec.spot_pct) {
    any_spot = any_spot || (s != 0.0);
  }
  bool any_vol = false;
  for (const double v : spec.vol_bump) {
    any_vol = any_vol || (v != 0.0);
  }
  const bool has_dt = spec.dt != 0.0;
  const bool has_dr = spec.dr != 0.0;

  std::size_t n_missing = 0;
  for (std::size_t p = 0; p < n_pos; ++p) {
    if (pos_ok[p] == 0u) {
      continue;
    }
    if (!scenario_deriv_greeks_sufficient(frame.rows[p].greeks, any_spot, any_vol, has_dt, has_dr,
                                          deriv_spec)) {
      pos_ok[p] = 0u;
      ++n_missing;
    }
  }

  std::size_t n_ok = 0;
  for (const std::uint8_t v : pos_ok) {
    n_ok += v;
  }
  out.n_deriv_ok = n_ok;
  out.n_deriv_failed = n_pos - n_ok - n_missing;
  out.n_deriv_missing_sensitivity = n_missing;

  out.deriv_pnl.assign(n_cells, 0.0);
  out.deriv_route.assign(n_cells, static_cast<std::uint8_t>(ScenarioRoute::Taylor));
  std::size_t n_fallback = 0;

  // Serial over cells AND over positions inside each cell. The option leg fans
  // its Exact reprices over the shared pool because a PUT's exercise boundary
  // is reusable down a whole spot column; a deriv reprice shares nothing across
  // cells, so a fan-out here would buy throughput at the cost of the
  // fixed-order reduction the result's determinism contract rests on. Cost is
  // therefore (Exact cells x Ok positions) strip integrations, stated plainly
  // rather than hidden -- a caller wanting a large Exact deriv grid should size
  // it knowingly.
  for (std::size_t c = 0; c < n_cells; ++c) {
    const std::size_t i_spot = c / n_vol;
    const std::size_t j_vol = c % n_vol;
    const double spot_pct = spec.spot_pct[i_spot];
    const double dvol = spec.vol_bump[j_vol];
    const bool exact = std::abs(spot_pct) > spec.taylor_radius_spot ||
                       std::abs(dvol) > spec.taylor_radius_vol;
    out.deriv_route[c] =
        static_cast<std::uint8_t>(exact ? ScenarioRoute::Exact : ScenarioRoute::Taylor);

    double total = 0.0;
    for (std::size_t p = 0; p < n_pos; ++p) {
      if (pos_ok[p] == 0u) {
        continue;
      }
      const double dS = spot_pct * pos_spot[p];
      const double taylor =
          scenario_deriv_taylor_leg(frame.rows[p].greeks, dS, dvol, spec.dt, spec.dr,
                                    deriv_spec.d_skew, deriv_spec.d_convexity);
      if (!exact) {
        total += taylor;
        continue;
      }
      detail::DerivShock shock{};
      shock.spot_rel = spot_pct;
      shock.vol_shift = dvol;
      shock.skew_shift = deriv_spec.d_skew;
      shock.convexity_shift = deriv_spec.d_convexity;
      shock.rate_shift = spec.dr;
      shock.time_roll = spec.dt;
      const SurfaceRef surf = base.find(deriv_book[p].uid);
      // The row's own centre quote is the pin source, so the shocked reprice
      // differs from the base by a change of price rather than by a re-resolved
      // grid or a re-calibrated vol-of-vol -- and the grid pays nothing for it,
      // because `price_deriv_book` already computed that quote.
      const auto q = detail::deriv_price_shocked_on_ref(
          surf, deriv_book[p].contract, cfg, std::nullopt, shock, &frame.rows[p].greeks.quote);
      // A shocked solve that Erred or went non-finite falls back to this
      // position's Taylor leg for this cell; the cell's route STAYS Exact, and
      // the substitution is counted rather than absorbed -- same accounting the
      // option leg's `n_exact_fallback_lanes` keeps.
      if (!q.has_value() || !std::isfinite(q->pv)) {
        ++n_fallback;
        total += taylor;
        continue;
      }
      total += deriv_book[p].qty * q->pv - frame.rows[p].pv;
    }
    // The header promises no NaN reaches a cell total. That promise was FALSE
    // before this round -- a VolSwap row under the default `ScenarioDerivSpec{}`
    // returned nine NaN cells alongside `n_deriv_ok = 1` -- so it is now
    // enforced rather than asserted. Every path into `total` is guarded above
    // (unusable sensitivities excluded before the loop, non-finite Exact
    // reprices falling back to Taylor), so this can only fire on a defect in
    // that guarding, which is exactly when a caller should hear about it loudly
    // instead of receiving a NaN matrix stamped Ok.
    if (!std::isfinite(total)) {
      return Err(ErrorCode::Internal,
                 "scenario_grid: deriv cell " + std::to_string(c) + " went non-finite");
    }
    out.deriv_pnl[c] = total;
  }

  out.n_deriv_exact_fallback_lanes = n_fallback;
  return core::Ok();
}

}  // namespace

Result<ScenarioGridResult> scenario_grid(const std::vector<Position> &book,
                                         const std::vector<DerivPosition> &deriv_book,
                                         const SurfaceSet &base, const ScenarioGridSpec &spec,
                                         const ScenarioDerivSpec &deriv_spec) {
  ATX_TRY(ScenarioGridResult out, scenario_grid(book, base, spec));
  ATX_TRY_VOID(fill_deriv_leg(out, deriv_book, base, spec, deriv_spec));
  return core::Ok(std::move(out));
}

} // namespace atx::vol
