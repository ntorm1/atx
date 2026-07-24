#pragma once

// MULTI-EXPIRY de-Americanized volatility-SURFACE parity — the calendar +
// time-interpolation acceptance layer above the single-expiry harness
// (vola_parity.hpp).
//
// Where `run_expiry_parity` proves the Vola Dynamics workflow for ONE expiry
// (de-Americanize -> fit a 3-parameter eSSVI slice -> re-Americanize -> score),
// this module lifts that to a whole `Underlying`: it de-Americanizes and fits
// EVERY expiry chain, assembles the fitted slices into one ascending-T eSSVI
// `VolSurface`, and then proves the two properties a surface (as opposed to a
// bag of independent slices) must additionally satisfy:
//
//   1. CALENDAR NO-ARBITRAGE — total variance w(k, T) is non-decreasing in T at
//      every sampled log-moneyness (arb.hpp's `arb_check_calendar`).
//   2. TIME-INTERPOLATION PARITY — because each fitted eSSVI slice reproduces
//      its S3/SSVI truth, the surface's linear-in-total-variance interpolation
//      between adjacent slices reproduces the truth slices' own linear-in-w
//      interpolation. This is the "interpolation parity with Vola" property;
//      the acceptance test checks it against the s3_iv reference directly.
//
// ## Reuse of the single-expiry pattern
//
// The per-expiry work mirrors `vola_parity.cpp` exactly: de_americanize_chain
// -> rebuild the aligned observation set on the de-Am forward / q_eff ->
// essvi_fit_slice -> the natural-form `EssviParams` slice. `run_surface_parity`
// re-does that per chain (vola_parity returns only metrics, never the slice) so
// it can WRITE each fitted slice into the surface. The q_eff bridge
// (q_eff = r - ln(F/S)/T) is used throughout, exactly as in the single-expiry
// harness.
//
// Stateless and pure — a single call owns only its local scratch; the returned
// report OWNS the fitted `VolSurface` by value (move it out). Safe to call
// concurrently on distinct underlyings.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "atx/core/error.hpp"          // ErrorCode (ExpiryFitReport)
#include "atx/vol/calib.hpp"           // CalibOpts
#include "atx/vol/correction.hpp"      // AmericanCorrectionCaches (cert-carry resolve)
#include "atx/vol/curve.hpp"           // DividendEvent
#include "atx/vol/deamer.hpp"          // DeAmOptions
#include "atx/vol/parity.hpp"          // ParityReport
#include "atx/vol/prepared_policy.hpp" // PreparedObservationPolicy
#include "atx/vol/types.hpp"           // Result
#include "atx/vol/universe.hpp"        // Underlying
#include "atx/vol/vol_surface.hpp"     // VolSurface

namespace atx::vol {

// Calendar-arbitrage repair policy applied to the ASSEMBLED surface before the
// per-expiry parity is scored.
//
// Independent per-slice eSSVI fits + wing extrapolation can cross in total
// variance (the flagged Vola-parity gap): the raw surface is not guaranteed
// calendar-arb-free. Two repair strategies, at opposite ends of the
// quality-vs-strictness trade-off:
//
//  * `MonotoneFit` — the QUALITY-PRESERVING calendar-constrained fit. Slices are
//    fit short→long with a calendar floor w_i(k) >= w_{i-1}(k) enforced over the
//    near-money region: a theta floor on the ATM level PLUS an active set of
//    one-sided floor pseudo-observations (each currently-violating grid point is
//    added as a heavily-weighted obs targeting w_{i-1}(k), then the slice is
//    refit, iterated until no violation remains). Because the floor is enforced
//    by least squares over just the violating points, the fit lifts only where it
//    must and gives up the MINIMAL error needed to stay monotone — unlike a
//    global theta bump. MEASURED (XOM OPRA slice): clears the near-money window
//    (|k|<=0.6) 26 -> 0 violations at held quality (fair-value-in-bid-ask
//    98.5% -> 98.5%, reduced chi2 0.207 -> 0.209). Deep-wing crossings (|k| out
//    to 3, ~20 sigma, no quotes) are left free — economically irrelevant
//    extrapolation; use `Project` if a strict full-grid guarantee is required.
//    This is the "parity with Vola" surface: calendar-arb-free where it trades,
//    at held fit quality.
//
//  * `Project` — STRICT but quality-COSTLY. Runs the post-hoc backbone
//    projection (`arb_project_calendar_essvi`) + residual damping over the whole
//    check grid, guaranteeing calendar-arb-free over |k|<=3. MEASURED TRADE-OFF
//    (XOM OPRA slice): the projection lifts a crossing slice's theta to cover a
//    far-wing crossing, moving the whole slice (ATM included) off market — mean
//    fair-value-in-bid-ask 98.5% -> ~20%, reduced chi2 0.21 -> ~750. Reserve it
//    for callers that require strict wing no-arb and accept the fit cost.
//
// Parity is scored off the FINAL surface in every mode, so the reported quality
// is what the surface actually serves.
//
// `None` (the default) leaves the surface untouched and only CHECKS calendar
// arbitrage — byte-identical to the historical behaviour.
enum class CalendarRepair : std::uint8_t {
  None = 0,        // check only; leave the assembled surface untouched (default)
  MonotoneFit = 1, // sequential theta-floor fit: ATM-monotone, quality-preserving
  Project = 2,     // post-hoc projection to strict |k|<=3 arb-free (quality cost)
};

// Opt-in wall/worker-time breakdown for one surface build. The default build
// path leaves `collected=false` and every duration zero; callers must request
// collection explicitly, so production fits pay no steady-clock calls unless
// they are being measured. Carry/observation durations are sums across expiry
// workers, while total_wall_ms is elapsed wall time for the complete session
// build. That distinction makes parallel efficiency observable.
struct SurfaceFitStageTimings {
  double carry_solve_ms{0.0};
  double observation_deam_ms{0.0};
  double slice_fit_ms{0.0};
  double audit_ms{0.0};
  double calendar_validation_ms{0.0};
  // V2 blind-spot closure (WS-V). Two session-level fit costs that live in
  // VolaSession::build — OUTSIDE run_surface_parity / fit_curve_surface — so they
  // were previously invisible to this breakdown:
  //   correction_cache_ms  — per-board correction-cache rebuild (build_session_caches,
  //                          ~192 AL boundary solves/board). Stamped on both routes.
  //   input_diagnostics_ms — the eSSVI certification/diagnostics de-Am recompute
  //                          (collect_input_diagnostics -> build_observations_european),
  //                          a full duplicate board de-Am. eSSVI route only (0 for curve).
  // The *_solves are the AL-boundary-solve ledger deltas attributable to each stage
  // (atx::vol::counters::ledger). EXACT under serial / single-board fit (the e2e
  // bench, populate at fit_workers=1); under concurrent populate they are a global
  // ledger delta over the build window and read as an upper attribution.
  double correction_cache_ms{0.0};
  double input_diagnostics_ms{0.0};
  std::uint64_t correction_cache_solves{0u};
  std::uint64_t input_diagnostics_solves{0u};
  double total_wall_ms{0.0};
  bool collected{false};
};

// Market/pricing context for a whole-surface parity run. `cash_divs` is held by
// value so the call owns a span-friendly copy of the dividend schedule, shared
// across every expiry (each chain supplies its own T / expiry_ns).
struct SurfaceParityInputs {
  double S{0.0}; // spot (> 0)
  double r{0.0}; // continuously-compounded rate (finite)
  // Optional vectors aligned with `Underlying::chains`. Empty preserves the
  // legacy scalar `r` path bit-for-bit; otherwise each expiry uses its own
  // continuously-compounded zero rate.
  std::vector<double> expiry_rate_T;
  std::vector<double> expiry_rates;
  std::vector<DividendEvent> cash_divs;        // discrete cash-dividend schedule
  std::int64_t now_ts_ns{0};                   // valuation timestamp (epoch ns)
  DeAmOptions deam{};                          // borrow-implication / pricer policy
  CalibOpts calib{};                           // per-slice curve-fit policy
  double band_k{1.0};                          // minimum-edge band multiplier (parity)
  CalendarRepair repair{CalendarRepair::None}; // post-assembly calendar-arb repair

  // Enable the structured SurfaceFitStageTimings report. False is the hot-path
  // default and performs no stage clock reads.
  bool collect_stage_timings{false};

  // Worker count for the per-chain de-Americanization pre-pass in
  // `fit_curve_surface` (S0-1). Matches the `parallel_for` contract exactly:
  // 0 => std::thread::hardware_concurrency(); 1 => serial (recovers the
  // pre-S0-1 single-threaded path bit-for-bit); N => N workers. The fit itself
  // (calendar-floor dependent) always stays sequential; only the cold per-chain
  // de-Am (resolve_chain_forward + build_observations_european) fans out, over
  // disjoint per-chain output slots, so the result is bit-identical for any
  // worker count. `run_surface_parity` (the eSSVI path) does not read this
  // field and is unaffected.
  unsigned fit_workers{0};

  // Score the re-Americanized per-expiry parity diagnostic (a SECOND cold
  // de-Am of the board via `build_parity_data` in `fit_curve_surface`; S0-2).
  // true (default) = bit-identical to the historical served path -- production
  // still gets the quality diagnostic. false = skip it: the fitted surface is
  // UNCHANGED (parity is a pure diagnostic), but `SurfaceParityReport::per_expiry`
  // /`CurveSurfaceReport::per_expiry` are zeroed `ParityReport{}` (n == 0) and
  // `worst_frac_within_bidask` resolves to 0.0 -- so a caller that only needs
  // the fitted surface avoids the redundant second de-Am pass. Only consulted
  // by `fit_curve_surface`; `run_surface_parity` (the eSSVI path) always scores
  // parity and does not read this field.
  bool score_parity{true};

  // Enforce the previous expiry's total-variance curve as a floor while fitting
  // each later ConvexDense/SVI slice. true preserves the robust served dense
  // path; false fits each slice independently, which maximizes penny-tight SPY
  // in-band coverage and avoids the quality cost of cross-expiry lifting. Only
  // consulted by `fit_curve_surface`; eSSVI calendar handling remains governed
  // by `repair`.
  bool enforce_calendar_floor{true};

  // Route the fit-observation American-IV de-Am through the supplied correction
  // caches instead of cold Andersen-Lake. Default false preserves the reference
  // dense fit; HFT can opt in after measuring the penny-tight SPY quality trade.
  bool use_deam_cache_for_fit{false};

  // Correction caches for the CERTIFICATION-carry resolve exported through
  // `CurveSurfaceReport::input_certification` (perf C1 review fix). The
  // certification layer historically resolved carry with the CALLER's
  // `SessionInputs::deam` — whose caches the session never populates — while
  // `deam.caches` here may carry the session-built hot-path caches. nullopt
  // (default) => the fit's own carry resolve already ran with exactly the
  // certification caches; reuse it. Set => when it differs from `deam.caches`,
  // the prepass re-resolves the certification carry with these caches
  // substituted (same per-chain parallel task), reproducing the historical
  // serial certification pass bit-for-bit. Only consulted by
  // `fit_curve_surface`; `run_surface_parity` does not read this field.
  std::optional<AmericanCorrectionCaches> deam_cert_caches{};

  // Observation-preparation policy for the polymorphic fit pre-pass
  // (`fit_curve_surface` -> `run_deam_prepass`). Configured (default) applies
  // CalibOpts through the shared, always-audited calibration builder and is
  // bit-identical to the historical curve-driver path — the strict usable-row
  // floor can starve thin single-name expiries. LegacyEssviCompatibility uses
  // the permissive eSSVI cold-driver predicate (the policy the served eSSVI
  // `run_surface_parity` path prepares under), keeping those thin expiries;
  // under it the pre-pass audits fitted inversions per `deam.audit_fit_inversions`.
  // Consulted by `fit_curve_surface` always, and by `run_surface_parity` only
  // when `essvi_serve_configured_prep` is TRUE (FT-C8 flag-guarded rollout below).
  PreparedObservationPolicy fit_prep_policy{PreparedObservationPolicy::Configured};

  // FT-C8 flag-guarded rollout: whether the served eSSVI path (`run_surface_parity`)
  // prepares its observation population under the configured `fit_prep_policy`
  // (the full CalibOpts filter cascade — kill-mask flags, max_spread_vol,
  // max_spread_to_mid_pct, min_vega_weight, anchors, max_obs_per_slice) instead
  // of the permissive `LegacyEssviCompatibility` predicate the canonical default
  // family historically fit under (strike>0 + quote_valid only). The default eSSVI
  // family otherwise fits a filter-free population while every other family gets
  // the configured cascade — exactly the family-dependent policy the facade denies.
  // DEFAULT FALSE => byte-identical to the historical served-eSSVI Legacy prep;
  // the flip to TRUE is gated behind the XOM+SPY in-band%/chi2/vol-RMSE A/B.
  bool essvi_serve_configured_prep{false};

  // W3.3 (F3): opt-in per-slice Legacy-prep rescue (thin-slice recovery). When
  // TRUE and the primary `fit_prep_policy` is Configured, a slice whose
  // Configured preparation starves below the usable-row floor with an EXPECTED
  // error (NotFound / Unavailable — genuinely thin, non-positive forward, or
  // audit-rejected rows) or that produces fewer than the floor is RE-PREPARED
  // under LegacyEssviCompatibility (the permissive eSSVI cold-driver predicate)
  // with `deam.audit_fit_inversions` forced on, so the rescued rows are still
  // repriced-audited (correctness-first serving, charter §8.1). A HARD
  // preparation error (Internal / InvalidArgument / OutOfRange / …) is a real
  // defect: it is never rescued and is retained on the prepass slot so the driver
  // can surface it truthfully. This is the root-cause
  // fix for the failed-board cohort ("80% failure"): the majority-route thin
  // single-name expiries that the strict Configured funnel starved are recovered
  // instead of dropping the whole surface. DEFAULT FALSE => byte-identical to the
  // historical drop-the-slice behavior. Only consulted by `fit_curve_surface`.
  bool per_slice_legacy_prep_fallback{false};

  // W3.4 (F4): completeness contract. When TRUE, an expiry whose PREPARATION or
  // slice FIT fails with a HARD error code (anything other than NotFound /
  // Unavailable — i.e. a genuine defect such as a non-converged QP `Internal`)
  // makes the driver RETURN that error instead of silently dropping the slice and
  // publishing a partial surface as success (F-02). Genuinely thin expiries
  // (NotFound / Unavailable) are still dropped, preserving the deliberate
  // Mark-policy tolerance for sparse boards. DEFAULT FALSE => byte-identical to
  // the historical "drop the slice, fail only at zero slices" behavior; a
  // completeness-requiring consumer (risk admission, the recovery cohort) opts in.
  // Consulted by both `fit_curve_surface` and `run_surface_parity`.
  bool fail_board_on_hard_slice_error{false};
};

// Per-fitted-slice pricing context: everything the composable facade
// (session.hpp) needs to RE-PRICE an option off the assembled surface without
// re-running de-Americanization. `q_eff` is the effective carry that reproduces
// the term forward exactly (S*e^{(r-q_eff)T} == forward), so a single scalar
// drives both the surface's log-moneyness and the American re-pricing.
struct SliceContext {
  double T{0.0};            // year-fraction to expiry (== expiry_T[i])
  double forward{0.0};      // term forward F used for the fit / scoring
  double borrow{0.0};       // implied (or fixed) per-term borrow
  double q_eff{0.0};        // effective carry: r - ln(F/S)/T
  std::size_t n_used{0};    // strikes that survived to the fit
  std::size_t n_dropped{0}; // strikes skipped (bad quote / failed invert)
};

// W3.4 (F4): per-expiry FIT-driver outcome taxonomy (distinct from the
// admission-layer `ExpiryBuildReport` in pricer_fitter.hpp). One `ExpiryFitReport`
// is emitted for EVERY chain the driver walks (fitted or not), so a caller — and
// the admission layer — can tell a genuinely thin/absent expiry from a real
// defect instead of inferring "Missing" by maturity matching, and stop treating
// a partial fit (e.g. 1-of-24 slices after hard failures on the rest) as a clean
// success. Shared by both drivers (`run_surface_parity`, `fit_curve_surface`).
enum class ExpiryFitOutcome : std::uint8_t {
  Fitted = 0,          // the primary curve fit succeeded
  FittedFallbackCurve, // recovered via the per-slice LinearVariance fallback
  FittedLegacyPrep,    // fit succeeded on a Legacy-prep-rescued (thin) slice
  CarryFailed,         // carry / forward resolution failed — no slice
  PrepStarved,         // below the usable-row floor (thin) — no slice
  PrepFailed,          // HARD preparation error (defect) — `error` is set
  FitFailed,           // slice fit failed — `error` is set
  Skipped,             // degenerate maturity (T<=0) — never attempted
};

struct ExpiryFitReport {
  std::size_t chain_index{0};
  double maturity{0.0};
  ExpiryFitOutcome outcome{ExpiryFitOutcome::Skipped};
  std::size_t n_observations{0};
  // Meaningful for PrepFailed / FitFailed: the code the failed step returned.
  atx::core::ErrorCode error{atx::core::ErrorCode::Unknown};
  // Decision B: provenance of the carry (borrow) used for this expiry. `Solved`
  // for a directly-inferred carry; a TermStructure* value marks a slice admitted
  // by the board-level term-structure carry fallback (surfaced so admission can
  // distinguish a solved from a borrowed carry).
  CarrySource carry_source{CarrySource::Solved};
};

// Perf C1: per-fitted-slice de-Am INPUT certification captured by the eSSVI fit
// itself (`run_surface_parity` -> `prepare_expiry` under LegacyEssviCompatibility),
// ‖ `context`. The analogue of `CurveSurfaceReport::SliceInputCertification` for
// the eSSVI path: it lets `VolaSession::build` construct the per-slice
// `SessionSliceDiagnostics` + the incremental observation cache directly from the
// rows the FIT actually de-Americanized, instead of re-running a SECOND,
// independent Configured de-Am pass (finding 10). Every field is exactly what the
// fit's `PreparedSlice` produced for this slice — never resampled. NOTE (class
// accuracy-improving): the reused rows are the LegacyEssviCompatibility recipe
// (what the surface was fit from), NOT the Configured `build_observations_european`
// recipe the old certification pass ran; the certification/diagnostics therefore
// now describe the fit's own rows (more correct — the old pass audited rows the
// fit never used), and the incremental refit-seed store carries Legacy rows.
struct EssviInputCertification {
  DeAmAuditDiagnostics inversion{};       // the fit's own de-Am audit (deam_audit())
  std::vector<FitObs> obs;                // the fit rows (fit_observations())
  std::vector<double> source_mids;        // ‖ obs; raw chain.mids at (K, side)
  std::vector<std::uint8_t> source_flags; // ‖ obs; raw chain.flags at (K, side)
  std::vector<double> chain_mids;         // full-chain snapshot (incremental cache)
  std::vector<std::uint8_t> chain_flags;
  std::vector<double> chain_bids;
  std::vector<double> chain_asks;
  std::vector<std::int64_t> chain_ts;
};

// The whole-surface acceptance bundle. `surface` OWNS the fitted eSSVI surface
// (movable); the vectors are parallel per fitted slice (ascending T).
struct SurfaceParityReport {
  VolSurface surface;                   // fitted eSSVI surface (move it out)
  std::vector<double> expiry_T;         // per fitted slice, strictly ascending
  std::vector<ParityReport> per_expiry; // re-Americanized metrics per expiry
  std::vector<SliceContext> context;    // per-slice re-pricing context (‖ expiry_T)
  // Perf C1: the carry diagnostics the FIT's `resolve_chain_forward` produced
  // for this slice (‖ context), resolved with `in.deam` — including whatever
  // caches it carried. `VolaSession::build`'s certification layer reuses these
  // ONLY when the fit's caches equal the certification caches (the caller's
  // `SessionInputs::deam.caches`); otherwise it falls back to its own
  // cache-free recompute so certification stays bit-identical to the
  // historical serial pass (perf C1 review fix).
  std::vector<CarryDiagnostics> carry;
  // Perf C1: per-fitted-slice de-Am input certification captured by the fit,
  // ‖ context/carry. Empty unless populated by run_surface_parity; consumed by
  // VolaSession::build to skip the second (certification) de-Am pass.
  std::vector<EssviInputCertification> input_certification;
  double worst_frac_within_bidask{0.0}; // min over expiries of frac in bid-ask
  bool calendar_arb_free{false};        // arb.hpp calendar check on the surface
  std::size_t n_slices{0};              // fitted slice count (== expiry_T.size())
  std::size_t n_calendar_viol_pre{0};   // calendar violations BEFORE any repair
  // Expiries dropped because carry resolution failed (confidence gate / no
  // quotable pair / degenerate forward) — surfaced, never silently skipped.
  std::size_t n_carry_skipped{0};
  // Expiries dropped because the fit-inversion audit starved the slice below
  // the usable-observation floor (it would have fit but for audit drops) —
  // the audit-created analogue of a carry skip, surfaced the same way.
  std::size_t n_audit_starved{0};
  // W3.4 (F4): per-expiry build outcome for EVERY chain walked (‖ under.chains,
  // in chain order — NOT the fitted-slice order). Always populated. The trailing
  // `{}` gives it a default member initializer so the positional aggregate init
  // below (which stops at n_audit_starved) does not trip -Wmissing-field-init.
  std::vector<ExpiryFitReport> expiry_reports{};
  SurfaceFitStageTimings fit_timings{};
};

// De-Americanize + fit each expiry chain of `under`, assemble an ascending-T
// eSSVI `VolSurface`, run the calendar no-arbitrage check on it, and score
// per-expiry re-Americanized parity.
//
// For each chain in `under.chains` (stored ascending in T): de_americanize_chain
// (implied/fixed borrow + term forward F), rebuild the aligned observation set
// on (F, q_eff = r - ln(F/S)/T), fit the eSSVI slice, write it into the surface
// at the next ascending index, and score re-Americanized parity with a model IV
// read back from the assembled surface. Slices with fewer than five usable
// strikes (or that fail to de-Americanize / fit) are SKIPPED (not fatal); a run
// that produces zero slices is an error. After assembly the surface's calendar
// arbitrage is checked over k in [-3, 3] (25 grid points).
//
// @param under  the underlying whose chains are de-Americanized and fit.
// @param in     market/pricing context, borrow-implication + curve-fit policy.
// @return       the assembled-surface parity bundle, or an Error:
//                 InvalidArgument — S <= 0 or non-finite r;
//                 NotFound        — `under` carries no chains, or not a single
//                                   expiry produced a usable eSSVI slice.
//               Any surface-construction, curve-fitter, or parity/pricer error
//               is propagated.
[[nodiscard]] atx::core::Result<SurfaceParityReport>
run_surface_parity(const Underlying &under, const SurfaceParityInputs &in);

} // namespace atx::vol
