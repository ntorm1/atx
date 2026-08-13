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

// T10b (plan D5): what the per-slice FIT itself reported, for one committed
// slice. Parallel to `context` / `per_expiry` (one entry per committed slice,
// ascending T) — the same shape `input_certification` already uses.
//
// D5 is "served-but-bad surfaces are structurally unreportable". T10 built the
// `FitDiag` out-param that makes a fit describable and stated plainly that no
// production call site passed one, so the switch was inert. This is the sink
// that makes it observable: `fit_curve_surface` now passes a real struct on both
// the primary and the LinearVariance-fallback fit and parks the result here, on
// the same public report `context` / `per_expiry` / `expiry_reports` ride out on.
//
// READ THE FAMILY BEFORE READING THE VERDICT. Coverage is deliberately PARTIAL
// and differs per family, because only what a fitter actually computes is
// reported. Every unpopulated field keeps its "not known" default — never a
// value that reads as success — but those defaults are only honest if the reader
// knows which family produced them, which is why `kind` is stored beside the
// diagnostic instead of being left to be inferred from the CurveConfig:
//
//   ConvexDense     termination = Converged (the one family that fails closed,
//                   so a returned fit is certified), final_grad_norm = its
//                   scaled KKT stationarity norm, n_quotes_used, outer_iters
//   Svi             the fitter's own FitDiag, plus termination from its IRLS
//                   stop and projection from the Mingone/Lee gate
//   C8              termination + final_grad_norm from the LM optimality
//                   certificate (T10b), n_quotes_used, inner_iters_total,
//                   reverted_to_seed
//   Essvi           the fitter's own FitDiag ONLY. `essvi_fit_slice` populates
//                   five fields and none of the T10 ones, so `termination` reads
//                   Unknown and `projection` reads NotRun on EVERY eSSVI slice.
//                   That is a reporting gap in the eSSVI fitter, NOT a
//                   converged-clean verdict, and must not be counted as one.
//   LinearVariance
//   / SplineVol     n_quotes_used only
//
// So `Unknown` never means "failed to converge", and a disengaged
// `final_grad_norm` never means "zero gradient". Both mean the fitter was not
// asked, or does not know.
struct SliceFitDiagnostics {
  std::size_t chain_index{0};
  double maturity{0.0};
  // The family ACTUALLY served for this slice, which is not always
  // `CurveConfig::kind`: an expiry recovered by the opt-in per-slice fallback is
  // a LinearVariance curve under a different configured kind, and reading its
  // diagnostic against the configured family would misattribute the table above.
  VolCurveKind kind{VolCurveKind::ConvexDense};
  FitDiag diag{};

  // ── D4: what the served reduced chi-square was actually scored against ───
  //
  // The dof handed to `reduced_chi_square` for this slice — the fitted curve's
  // own `IVolCurve::dof()`. It used to be hardcoded to 3, which was right only
  // for eSSVI and made every other family's served chi2 optimistic, because
  // chi2_reduced is chi2/(N - dof) and too small a dof inflates the denominator.
  // Zero when parity was not scored for this slice.
  std::size_t chi2_dof{0};
  // True when the scored population could NOT support that dof (N <= dof), so a
  // TRUE reduced chi-square is undefined for this slice. An INTERPOLATING family
  // reaches this by construction — LinearVariance's dof is its node count.
  //
  // When set, `ParityReport::chi2_reduced` holds chi2/N (re-scored with dof = 0),
  // NOT chi2/(N - dof). It is deliberately not blanked to zero: an exact zero
  // chi-square reads as a PERFECT fit, and zeroing it would re-create the W3-A
  // all-zero diagnostics blackout on exactly the auto-routed LinearVariance
  // route that invariant was written to close.
  //
  // THIS IS THE FLAG THAT KEEPS THAT NUMBER HONEST. Without it chi2/N is
  // indistinguishable from a genuine reduced chi-square and silently flatters an
  // interpolating curve. The band evidence in the same `ParityReport` does not
  // depend on dof and is fully measured either way.
  bool chi2_dof_underdetermined{false};
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
  // W3.3 (F3): slices whose Configured preparation STARVED below the usable-row
  // floor and were recovered by re-preparing under LegacyEssviCompatibility (the
  // permissive eSSVI cold-driver predicate), gated on
  // `SurfaceParityInputs::per_slice_legacy_prep_fallback`. The rescued slice
  // truthfully carries `LegacyEssviCompatibility` provenance with a default
  // (never-certified) de-Am audit — certification must not claim Configured-grade
  // de-Am for it. Always 0 when the flag is off (byte-identical default path).
  std::size_t n_slices_legacy_rescued{0};
  // W1-B (F21): slices whose primary preparation starved under the OTM-only leg
  // rule and were recovered by re-preparing with `CalibOpts::itm_leg_fallback`,
  // gated on `SurfaceParityInputs::{per_slice,board_starved}_itm_leg_fallback`.
  // Orthogonal to `n_slices_legacy_rescued`: the leg rule and the preparation
  // policy are independent relaxations and a slice can carry both, so the two
  // counters may each include the same slice. Always 0 when both flags are off
  // (byte-identical default path).
  std::size_t n_slices_itm_rescued{0};
  // W3.3 (F3): expiries that stayed below the usable-row floor even after any
  // rescue attempt — truthfully thin (not a defect), surfaced so admission can
  // distinguish a genuinely sparse board from one starved by a preparation
  // funnel that a permissive policy would have kept.
  std::size_t n_slices_starved{0};
  // Task 1 (k-coverage): expiries refused because their admitted fit rows do
  // not straddle ATM or leave a central k-hole wider than the cap
  // (ExpiryFitOutcome::PrepUncovered). Surfaced, never silent.
  std::size_t n_slices_uncovered{0};
  // Task 3, re-gated by Task 6 (D1): slices refused because the previous
  // slice's calendar floor bound them only where that slice had no admitted
  // data (the seed-ratchet shape; kCalendarFloorUnsupportedMsg), reported as
  // `ExpiryFitOutcome::FitRefusedCalendar`. Truncation follows; surfaced, never
  // silent. Expected ZERO on healthy boards: the support band is armed ONLY
  // when the previous COMMITTED slice fails Task 1's k-coverage predicate, a
  // shape Task 1 already refuses at prep — so through `fit_curve_surface` this
  // counter is a defense-in-depth tripwire for prep-bypassing paths, and any
  // nonzero value on a dense board is itself the signal.
  // Caveat: under the opt-in `CalibOpts::per_slice_linear_fallback` (off by
  // default; the populate path never enables it), a refusal is first retried
  // as a LinearVariance slice, and a successful recovery does NOT increment
  // this counter — the refusal is absorbed, not surfaced. That fallback's
  // union-grid floor also applies w_prev unconditionally (no floor_support_k
  // analog), so "every refusal is counted" holds only with the flag off.
  std::size_t n_slice_calendar_unsupported{0};
  // Perf C1: per-slice input certification, ‖ context/per_expiry. Lets
  // `VolaSession::build` construct `SessionSliceDiagnostics` + the incremental
  // observation cache directly, without a second serial de-Am pass.
  std::vector<SliceInputCertification> input_certification;
  // T10b (D5): per-committed-slice fit diagnostics, ‖ `context` / `per_expiry`.
  // Empty only when no slice was committed. See `SliceFitDiagnostics` for the
  // per-family coverage table — the defaults are "not known", never "clean".
  std::vector<SliceFitDiagnostics> slice_diagnostics;
  // W3.4 (F4): per-expiry build outcome for EVERY chain walked (‖ under.chains,
  // in chain order — NOT the fitted-slice order of context/per_expiry). Always
  // populated so admission can distinguish a thin/absent expiry from a defect
  // (`ExpiryFitReport` / `ExpiryFitOutcome` are defined in surface_parity.hpp).
  std::vector<ExpiryFitReport> expiry_reports;
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
