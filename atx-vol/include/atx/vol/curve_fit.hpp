#pragma once

// Curve-agnostic surface fit driver — the generalization of `run_surface_parity`
// (which is hardwired to eSSVI) to ANY `VolCurveKind`.
//
// `run_surface_parity` de-Americanizes and fits every expiry into an eSSVI
// `VolSurface`. `fit_curve_surface` does the same de-Am + q_eff-bridge chain walk,
// but fits each expiry's slice through the uniform `fit_slice_curve` dispatch
// (Convex-QP dense / eSSVI / raw-SVI) and assembles a polymorphic `CurveSurface`.
// It scores the SAME re-Americanized per-expiry parity (`chain_parity`) so the
// reported quality is directly comparable across curve kinds.
//
// This is the path that finally lets `VolaSession` / `PricerFitter` SERVE the
// arb-free convex dense fit (the 99.5%-in-band SPY curve) — previously reachable
// only from bench code. For `VolCurveKind::Essvi` a caller should keep using
// `run_surface_parity` directly (it carries the calendar-repair machinery); this
// driver is the vehicle for the other kinds and for the CurveSelector.
//
// Stateless / pure: the returned report OWNS its `CurveSurface` by value (move it
// out). Safe to call concurrently on distinct underlyings.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "atx/vol/parity.hpp"         // ParityReport
#include "atx/vol/surface_parity.hpp" // SliceContext, SurfaceParityInputs
#include "atx/vol/types.hpp"          // Result
#include "atx/vol/universe.hpp"       // Underlying
#include "atx/vol/vol_curve.hpp"      // CurveSurface, CurveConfig

namespace atx::vol {

// The assembled polymorphic-surface bundle. `surface` OWNS the fitted curves;
// the vectors are parallel per fitted slice (ascending T), mirroring
// `SurfaceParityReport` so `VolaSession` consumes either interchangeably.
struct CurveSurfaceReport {
  CurveSurface surface;
  std::vector<SliceContext> context;    // per fitted slice, ascending T
  std::vector<ParityReport> per_expiry; // re-Americanized metrics (‖ context)
  double worst_frac_within_bidask{0.0};
  std::size_t n_slices{0};
  std::uint32_t n_score_inversions{0};
};

// De-Americanize + fit each expiry chain of `under` into a `CurveSurface` of the
// configured kind, scoring re-Americanized parity per expiry.
//
// Per chain (ascending T): `resolve_chain_forward` (borrow ⇒ term forward F),
// the q_eff bridge (S·e^{(r−q_eff)T} == F), `build_observations_european` (the
// de-Americanized fit input — same recipe the 99.5% bench uses), then
// `fit_slice_curve`. A slice with too few usable strikes, or that fails to
// de-Americanize / fit, is SKIPPED (not fatal). Parity is scored off the fitted
// slice's own `iv(k)` re-Americanized on the slice carry.
//
// @return InvalidArgument if S <= 0 or r non-finite; NotFound if `under` has no
//         chains or not a single slice fit; any fitter/pricer error propagated.
[[nodiscard]] Result<CurveSurfaceReport>
fit_curve_surface(const Underlying &under, const SurfaceParityInputs &in, const CurveConfig &cfg);

} // namespace atx::vol
