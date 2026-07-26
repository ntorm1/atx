#pragma once

// WS-P1v: the ONE laned-analytic-Greek batch kernel driver, shared by
// `PricedSurface::evaluate_batch` and `PricedSurfaceView::evaluate_batch`.
//
// WHY THIS FILE EXISTS. WS-P1a/P1b landed the laned AVX2 American-Greek dispatch on
// `PricedSurface::evaluate_batch` only. `PricedSurfaceView` — the zero-copy reader over
// a mapped `.atxvsa` record, i.e. the type the archive/`SurfaceDb` replay path serves —
// kept a pure scalar per-entry loop for Greeks, so a replayed book paid the per-contract
// `american_greeks_al` fan while a freshly-fit book rode the 4-lane kernel. That split
// was also the reason the `SurfaceArchiveV2` `expect_batch_bit_identical` golden had to
// be relaxed on the analytic route: view-batch was scalar, surface-batch was laned, so
// the two disagreed by the documented ~1e-13 AVX2-transcendental delta.
//
// Rather than copy the block, both types now call `laned_greek_run` with two lambdas
// that describe the ONLY thing that differs between them — how a query index resolves,
// and how a lane that cannot be laned falls back to that type's own scalar routing.
// Once resolved to (S, K, T, sigma, r, q, side) the kernel path is byte-for-byte
// identical, which is precisely why the archive bit-identity golden can be (and is)
// re-tightened.
//
// DETERMINISM CONTRACT (unchanged from priced_surface.cpp's original block): pack
// membership is fixed by the caller's entry order within the [begin, end) run and by
// `kGreekChunk`, never by any pricing-executor thread partition. The laned kernels are
// pack-composition invariant (a 1-lane pack returns exactly what a 4-lane pack returns),
// pinned by PricedSurface.EvaluateBatchLanedGreeksPackCompositionInvariant.

#include "atx/vol/american.hpp"
#include "atx/vol/counters.hpp" // ATX_VOL_COUNT(SurfaceFullGreekRoutes)
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/simd/american_boundary_batch.hpp"

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace atx::vol::detail {

// Whether `laned_greek_run` is the right route for this batch request. Mirrors the
// predicate WS-P1a locked on PricedSurface (the ACCELERATOR guard is the caller's — the
// view carries no accelerator and is always eligible).
[[nodiscard]] inline bool laned_greek_route_selected(bool want_greeks, bool selective_only,
                                                     bool want_delta, bool want_vega, bool analytic,
                                                     bool t_valid, AmericanMethod method,
                                                     simd::SimdIsa isa) noexcept {
  return want_greeks && !selective_only && !want_delta && !want_vega && analytic && t_valid &&
         method == AmericanMethod::AndersenLake && simd::avx2_greeks_selected(isa);
}

// FIX-2/F2-B (rev-ws-g M1-5): the finite sweep behind the Greek Ok-stamp. It IS
// `greeks_all_finite` in portfolio_pricer.cpp — as of FIX-5/M3 that name delegates here
// instead of carrying a second copy. The semantics FIX-1 locked at the portfolio
// Ok-stamps (740b040 F2, 9c3e1d0 F3): price/delta/gamma/theta always,
// vega/volga/vanna, rho and charm only when the caller REQUESTED them. Guarding the full
// bundle instead would veto perfectly good lanes on a column that was never materialized
// (FIX-1/F3), and guarding the price alone lets a NaN greek out on an Ok lane (F2-B).
//
// FIX-3/F3-A: this and `normalize_unrequested_greeks` below are now the SINGLE definition
// of that stamp for every Greek route — the laned scatter here AND the scalar
// `evaluate_resolved` in priced_surface.cpp / priced_surface_view.cpp, which both include
// this header. They are deliberately not duplicated: F2-B guarded only the laned driver,
// which left the same lane certifiable Ok on one ISA and demoted on another. Any future
// change to these two functions must stay a change to BOTH routes at once, which is
// exactly what having one definition buys.
//
// FIX-5 (I1, I3, M3) — the census, because "the three portfolio Ok-stamps" was carried in
// four commit bodies and in this comment, and it was wrong. There are FOUR stamps in
// portfolio_pricer.cpp (seed-staging, marks, ADJOINT, non-adjoint greeks) — I1 found the
// adjoint one still on the pre-FIX-1 `isfinite(price)` predicate — and a second laned-Greek
// driver outside that file entirely, `american_greeks_batch` (src/american_batch.cpp),
// whose scalar and laned arms each carried their own predicate and disagreed by side (I3).
// Every one of them now routes through THESE two functions. The list of routes that must
// stay on this definition is: laned_greek_run's scatter; both evaluate_resolved
// implementations; all four portfolio_pricer stamps (via `greeks_all_finite`, which
// delegates here as of M3); and american_greeks_batch's two arms.
[[nodiscard]] inline bool requested_greeks_finite(const AmericanGreeks &g,
                                                  GreekNeeds needs) noexcept {
  bool ok = std::isfinite(g.price) && std::isfinite(g.delta) && std::isfinite(g.gamma) &&
            std::isfinite(g.theta);
  if (needs.vega) {
    ok = ok && std::isfinite(g.vega) && std::isfinite(g.volga) && std::isfinite(g.vanna);
  }
  if (needs.rho) {
    ok = ok && std::isfinite(g.rho);
  }
  if (needs.charm) {
    ok = ok && std::isfinite(g.charm);
  }
  return ok;
}

// FIX-1/F3's second half, applied here: an UNREQUESTED slot that came back non-finite is
// normalized to its canonical unmaterialized value 0.0 — exactly what the narrowed kernel
// documents it leaves there ("unrequested greeks are left 0 in out_greeks"). Relaxing the
// mask WITHOUT this would turn a spurious veto into a poisoned product downstream, because
// a consumer's `g.rho * dr` is NaN even when dr is 0.0. Restricted to the non-finite case,
// so every lane that is admitted today stays bit-for-bit identical.
inline void normalize_unrequested_greeks(AmericanGreeks &g, GreekNeeds needs) noexcept {
  if (!needs.vega) {
    if (!std::isfinite(g.vega)) {
      g.vega = 0.0;
    }
    if (!std::isfinite(g.volga)) {
      g.volga = 0.0;
    }
    if (!std::isfinite(g.vanna)) {
      g.vanna = 0.0;
    }
  }
  if (!needs.rho && !std::isfinite(g.rho)) {
    g.rho = 0.0;
  }
  if (!needs.charm && !std::isfinite(g.charm)) {
    g.charm = 0.0;
  }
}

// Drive entries [begin, end) — a single raw-bit-equal-T run — through the laned analytic
// Greek kernels, writing iv/price/greeks/status into `out`.
//
//   `resolve`  : (std::size_t e) -> ResolvedSurfacePoint  — the caller's own carry/bracket
//                resolution for entry `e` (PricedSurface uses resolve_with_carry_and_bracket,
//                PricedSurfaceView uses resolve_with_carry).
//   `scalar_at`: (std::size_t e, Side sd) -> FusedResult  — the caller's exact scalar route
//                for entry `e`, used for invalid resolutions and for the non-finite-lane
//                failure fallback so both stay byte-identical to the unlaned path.
//
// `S`, `al` and `needs` are this surface's own pricing context — NOT the more expensive
// defaults the higher-level american_greeks_batch forces — so the result matches this
// surface's scalar `american_greeks_al(..., al_opts)` lane for lane.
template <class ResolveFn, class ScalarFn>
void laned_greek_run(double S, std::size_t begin, std::size_t end, std::span<const Side> side,
                     const std::optional<AlOpts> &al, simd::SimdIsa isa, GreekNeeds needs,
                     PricedSurface::EvaluationSoA out, ResolveFn &&resolve, ScalarFn &&scalar_at) {
  constexpr std::size_t kGreekChunk = 128;
  // Two homogeneous-per-side packs (the kernels are side-specific); each flushes when
  // full. Buffers are per-side so a mixed put/call run lanes BOTH halves.
  double psS[kGreekChunk], psK[kGreekChunk], psT[kGreekChunk];
  double psSig[kGreekChunk], psR[kGreekChunk], psQ[kGreekChunk];
  AmericanGreeks psG[kGreekChunk];
  std::size_t psIdx[kGreekChunk];
  std::size_t psN = 0;
  double csS[kGreekChunk], csK[kGreekChunk], csT[kGreekChunk];
  double csSig[kGreekChunk], csR[kGreekChunk], csQ[kGreekChunk];
  AmericanGreeks csG[kGreekChunk];
  std::size_t csIdx[kGreekChunk];
  std::size_t csN = 0;

  const auto scatter = [&](Side sd, std::size_t e, const AmericanGreeks &g) {
    AmericanGreeks gg = g;
    normalize_unrequested_greeks(gg, needs);
    if (requested_greeks_finite(gg, needs)) {
      // american_greeks_al().price IS the American mark (cold-FD invariant), exactly as
      // evaluate_resolved returns g->price for a want_greeks lane.
      out.greeks[e] = gg;
      out.price[e] = gg.price;
      out.status[e] = atx::core::Ok();
      return;
    }
    if (!std::isfinite(gg.price)) {
      // Byte-identical failure fallback: reproduce the caller's exact Err + poison (the
      // kernel exposes no per-lane ErrorCode). UNCHANGED — this is the non-finite-MARK
      // case that has always taken this route.
      const auto fr = scalar_at(e, sd);
      out.iv[e] = fr.iv;
      out.price[e] = fr.price;
      out.greeks[e] = fr.greeks;
      out.status[e] = fr.status;
      return;
    }
    // FIX-2/F2-B (rev-ws-g M1-5): finite MARK, non-finite REQUESTED greek. This stamp used
    // to gate on isfinite(price) alone, so such a lane came back Ok carrying a NaN greek,
    // and every DIRECT evaluate_batch consumer — one that does not reach FIX-1's
    // portfolio-level sweeps — inherited it. Re-routing to `scalar_at` would not close it:
    // evaluate_resolved returns a default (Ok) status whenever greeks_resolved yields a
    // value, non-finite columns included. So the lane is NaN-isolated by status here,
    // exactly as FIX-1 demotes at the portfolio stamps, and the computed columns are left
    // in place for diagnosis — consumers gate on status.
    out.greeks[e] = gg;
    out.price[e] = gg.price;
    out.status[e] = atx::core::Err(atx::core::ErrorCode::Internal,
                                   "laned_greek_run: non-finite requested Greek on a "
                                   "finite-price lane");
  };

  const auto flush_put = [&]() {
    if (psN == 0) {
      return;
    }
    // Honor the K4 first-order tier exactly as the scalar route does: a reduced `needs`
    // skips the sigma+/-, r+/- and speed solves (a {delta} hedge caller pays ONE boundary
    // solve, not five) and leaves the unrequested greeks 0.
    (void)simd::american_put_greeks_batch(psS, psK, psT, psSig, psR, psQ, psN, al, psG, isa,
                                          needs.vega, needs.rho, needs.charm);
    for (std::size_t m = 0; m < psN; ++m) {
      scatter(Side::Put, psIdx[m], psG[m]);
    }
    psN = 0;
  };
  const auto flush_call = [&]() {
    if (csN == 0) {
      return;
    }
    (void)simd::american_call_greeks_batch(csS, csK, csT, csSig, csR, csQ, csN, al, csG, isa,
                                           needs.vega, needs.rho, needs.charm);
    for (std::size_t m = 0; m < csN; ++m) {
      scatter(Side::Call, csIdx[m], csG[m]);
    }
    csN = 0;
  };

  for (std::size_t e = begin; e < end; ++e) {
    const PricedSurface::ResolvedSurfacePoint p = resolve(e);
    if (p.valid && side[e] == Side::Put) {
      ATX_VOL_COUNT(SurfaceFullGreekRoutes); // one greek bundle, laned or patched
      psS[psN] = S;
      psK[psN] = p.K;
      psT[psN] = p.T;
      psSig[psN] = p.sigma;
      psR[psN] = p.rate;
      psQ[psN] = p.q_eff;
      psIdx[psN] = e;
      out.iv[e] = p.sigma; // IV is free from the resolution (matches evaluate_resolved)
      if (++psN == kGreekChunk) {
        flush_put();
      }
    } else if (p.valid && side[e] == Side::Call) {
      ATX_VOL_COUNT(SurfaceFullGreekRoutes); // one greek bundle, laned or patched
      csS[csN] = S;
      csK[csN] = p.K;
      csT[csN] = p.T;
      csSig[csN] = p.sigma;
      csR[csN] = p.rate;
      csQ[csN] = p.q_eff;
      csIdx[csN] = e;
      out.iv[e] = p.sigma; // IV is free from the resolution (matches evaluate_resolved)
      if (++csN == kGreekChunk) {
        flush_call();
      }
    } else {
      // Invalid resolutions: exact scalar routing, byte-identical to the unlaned path.
      const auto fr = scalar_at(e, side[e]);
      out.iv[e] = fr.iv;
      out.price[e] = fr.price;
      out.greeks[e] = fr.greeks;
      out.status[e] = fr.status;
    }
  }
  flush_put();
  flush_call();
}

} // namespace atx::vol::detail
