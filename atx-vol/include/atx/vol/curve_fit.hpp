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

#include "atx/vol/calib.hpp"          // FitObs, DeAmAuditDiagnostics
#include "atx/vol/deamer.hpp"         // CarryDiagnostics
#include "atx/vol/parity.hpp"         // ParityReport
#include "atx/vol/surface_parity.hpp" // SliceContext, SurfaceParityInputs
#include "atx/vol/types.hpp"          // Result
#include "atx/vol/universe.hpp"       // Underlying
#include "atx/vol/vol_curve.hpp"      // CurveSurface, CurveConfig

namespace atx::vol {

// Per-slice de-Am INPUT certification data captured by the parallel prepass
// (`run_deam_prepass`, curve_fit.cpp) — the same carry resolution +
// de-Americanized-observation audit that `VolaSession::build`'s certification
// layer used to re-derive with a SECOND, serial `resolve_chain_forward` +
// `build_observations_european` pass (perf finding C1). Parallel to
// `context`/`per_expiry` (one entry per committed slice, same order); every
// field is exactly what the prepass task for that slice's chain already
// computed, moved out — never approximated or resampled.
struct SliceInputCertification {
  // Certification carry: resolved with the CALLER's caches
  // (`SurfaceParityInputs::deam_cert_caches` — what the historical serial
  // certification pass used), NOT necessarily the fit's own (possibly
  // session-cached) resolve. `carry_available` is false when that
  // certification resolve failed (a cached fit resolve can succeed where the
  // certification resolve does not) — mirrors the old serial pass leaving the
  // slice's carry diagnostics unavailable.
  CarryDiagnostics carry{};
  bool carry_available{false};
  DeAmAuditDiagnostics inversion{};
  std::vector<FitObs> obs;                // fit rows (ObsSet::obs)
  std::vector<double> source_mids;        // ‖ obs; raw chain.mids at (K, side)
  std::vector<std::uint8_t> source_flags; // ‖ obs; raw chain.flags at (K, side)
  std::vector<double> chain_mids;         // full-chain snapshot (incremental cache)
  std::vector<std::uint8_t> chain_flags;
  std::vector<double> chain_bids;
  std::vector<double> chain_asks;
  std::vector<std::int64_t> chain_ts;
};

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
  // Expiries dropped because carry resolution failed (confidence gate / no
  // quotable pair / degenerate forward) — surfaced, never silently skipped.
  std::size_t n_carry_skipped{0};
  // Slices whose primary-curve fit failed but were recovered via the opt-in
  // per-slice LinearVariance fallback (`CalibOpts::per_slice_linear_fallback`).
  // Always 0 when the flag is off (the byte-identical default path).
  std::size_t n_slice_linear_fallback{0};
  // Perf C1: per-slice input certification, ‖ context/per_expiry. Lets
  // `VolaSession::build` construct `SessionSliceDiagnostics` + the incremental
  // observation cache directly, without a second serial de-Am pass.
  std::vector<SliceInputCertification> input_certification;
  SurfaceFitStageTimings fit_timings{};
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
