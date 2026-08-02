#pragma once

// Shared calibration infrastructure — the options, observations, and quote
// filter that every per-parametrization calibrator (eSSVI, SVI, C8, CStar)
// builds on.
//
// Ported from the C `ats-vol` library:
//   - `AtsVolCalibOpts` + the calibration enums + `ats_vol_calib_default_opts`
//     (include/ats_calibrate.h, src/ats_calibrate.c);
//   - `AtsVolSviObs`, `ats_vol_svi_build_observations`,
//     `ats_vol_calib_obs_accepted`, `AtsVolSviFitDiag`
//     (src/ats_calibrate_svi.h, src/ats_calibrate_svi.c).
//
// The refactor to the atx house style (.agents/cpp/agent.md) drops the C's
// negative-integer `AtsVolStatus` channel for `Result<T>`, replaces the raw
// SoA scratch buffers with `std::vector`, and promotes the C's `uint8_t`
// boolean knobs to `bool` and its `uint8_t`-tagged enums to `enum class`.
//
// The *shared* objective every calibrator minimizes is
//
//      minimize  Σ_i  w_i · ( σ_model(k_i; θ) − σ_market_i )²
//
// over the observation set this module builds. `CalibOpts` carries the LM /
// IRLS / filter policy; `build_observations` applies the quote-filter cascade
// and yields the per-slice `FitObs` rows; `obs_accepted` is the O(1) predicate
// that reports whether a single (strike, side) tuple would survive that same
// cascade (used by benches to score the exact population the LM anchored to).
//
// ## Scope of this port
//
// This header is the *foundation* the parametrization-specific calibrators
// (ported later) `#include`. It deliberately ports only the load-bearing core
// of `AtsVolCalibOpts` that the LM core and the observation builder actually
// read. The deep-research knobs are omitted — see the PORT NOTE at the end of
// the options struct for the exact list.
//
// ## Thread-safety
//
// Every type here is a trivially-copyable value (or a `std::vector` owner).
// `build_observations` / `obs_accepted` are pure reads of the supplied `Chain`
// ("many readers OR one writer", matching the C chain contract) — safe to call
// concurrently against a fixed chain from any number of threads.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/american.hpp"    // AlOpts (de-Am Andersen-Lake accuracy preset)
#include "atx/vol/correction.hpp"  // AmericanCorrectionCaches (optional de-Am hot path)
#include "atx/vol/types.hpp"       // Side, Result
#include "atx/vol/universe.hpp"    // Chain (SoA quote layout)
#include "atx/vol/vol_surface.hpp" // ResidualBasisKind (reused, not redefined)

namespace atx::vol {

// ── Calibration policy enums ─────────────────────────────────────────────

// eSSVI ρ-handling mode (AtsVolEssviRhoMode). PerSlice is the shipping
// default; Shared / TermStructure are reserved for later work.
enum class EssviRhoMode : std::uint8_t {
  PerSlice = 0,      // one ρ per expiry (default)
  Shared = 1,        // one ρ for the whole surface
  TermStructure = 2, // ρ(T) piecewise (future)
};

// Optimization level, sized to economic precision (AtsVolOptimizationLevel).
// The active level selects one of the per-level iteration caps below.
//
//   QuickMark : sparse names, hot-quote refresh, pre-open / unstable chains.
//   Trading   : default live-quoting / signal-generation cadence (default).
//   Risk      : end-of-day, scheduled book snapshots, Greek/PnL explain.
//   Reference : research-grade, regression fixtures, model comparison.
//   ColdFast  : single-thread cold-fit fast path (hard iteration caps).
enum class OptimizationLevel : std::uint8_t {
  QuickMark = 0,
  Trading = 1,
  Risk = 2,
  Reference = 3,
  ColdFast = 4,
};

// Calibration loss function (AtsVolCalibLossKind). Mid = squared distance from
// the symmetric midpoint (default); Interval = zero residual when the model
// price lands inside [bid, ask], quadratic penalty outside.
enum class CalibLossKind : std::uint8_t {
  Mid = 0,
  Interval = 1, // persisted vocabulary; parametric fit rejects until implemented
};

// Calibration price-target anchor (AtsVolCalibAnchorKind). The observation
// builder writes the chosen target into `FitObs::mid`; the loss + Jacobian
// consume that field unchanged.
enum class CalibAnchorKind : std::uint8_t {
  Mid = 0, // target = ½·(bid + ask) (default)
  Bid = 1, // target = bid
  Ask = 2, // target = ask
};

// ── Calibration options ──────────────────────────────────────────────────

// The load-bearing subset of `AtsVolCalibOpts`. Every member is initialized to
// the value returned by the C `ats_vol_calib_default_opts()` — so a
// default-constructed `CalibOpts` equals `calib_default_opts()`.
//
// PORT NOTE — the following `AtsVolCalibOpts` research knobs are intentionally
// OMITTED (the core LM + obs-builder do not read them; downstream calibrators
// only need the set below):
//   - economic-precision controller : price_noise_ticks, spread_vol_fraction,
//                                      max_residual_ticks,
//                                      marginal_improvement_ticks;
//   - Andersen-Lake correction cache : amer_correction_call / _put;
//   - candidate selection            : residual_candidate_select,
//                                      candidate_safety_pp,
//                                      candidate_trigger_pct;
//   - eSSVI-vs-SVI fallback policy    : fallback_use_quality_score;
//   - tenor-bucketed filters (F.2)   : tenor_buckets (AtsVolTenorBuckets) — the
//                                      builder here uses the scalar
//                                      max_spread_vol / min_vega_weight only;
//   - source-vol seed / fallback      : use_source_vol_seed,
//                                      fallback_local_anchored;
//   - loss-aware selector (Phase L)   : selector_loss_aware,
//                                      selector_safety_pp_weighted;
//   - Fengler overlay (Phase M)       : fengler_n_basis, fengler_max_proj_iters,
//                                      fengler_ridge;
//   - diagnostics / logging           : verbose, diag (AtsVolCalibDiag*), and
//                                      all _pad_* alignment fields.
//
// PORT NOTE — a few C header comments disagree with the actual
// `ats_vol_calib_default_opts()` body; the *impl* wins here (verified against
// src/ats_calibrate.c): max_iter_cold_fast = 10 (comment said 5),
// wing_floor_alpha = 0.0 (comment said 0.05), morozov_stop = false (comment
// said 1/on), max_spread_to_mid_pct = 0.60 (comment said 0.40).

// Compatibility-sentinel defaults for the two options that are validated as
// "unchanged until the feature ships". Defined once here so the CalibOpts member
// initializers and validate_calib_options share a single source of truth and
// cannot silently drift apart.
inline constexpr double kDefaultEssviFallbackRmse = 0.01;
inline constexpr std::uint32_t kDefaultButterflyGrid = 200u;

struct CalibOpts {
  // IRLS / Newton.
  std::uint16_t max_outer_iter{4};  // IRLS reweighting passes
  std::uint16_t max_inner_iter{12}; // Newton steps per pass
  double tol_param{1.0e-9};         // convergence on parameter norm
  double tol_residual{1.0e-10};     // convergence on residual change

  // Robust loss.
  double huber_k{1.5}; // Huber threshold in vega-weighted residual std-devs

  // Quote filtering (read by the observation builder / accept predicate).
  double min_vega_weight{1.0e-6}; // drop below this weight_sigma = (vega/spread)²
  double max_spread_vol{0.05};    // drop quotes with spread/vega above this
  double max_weight{1.0e3};       // finite positive upper clip on stored w-space weights
  // 0 = use every surviving observation. Positive = cap the per-slice
  // de-Americanized fit population before the expensive American-IV inversion,
  // selecting adaptive knots by normalized total-variance interpolation error.
  // This is a cold-start latency knob for very dense index boards. The default
  // keeps the full population; adjacent-strike warm starts are controlled below.
  std::uint32_t max_obs_per_slice{0};
  // Seed each call/put inversion from the previous accepted strike of the same
  // side. Disable only to run the cold reference path for an A/B comparison.
  bool warm_start_deam_adjacent_strikes{true};
  // Share one retained nine-node sigma interpolation of the transformed
  // Andersen-Lake boundary across a sufficiently wide expiry/side ladder.
  // An embedded five-node estimator and accurate cold sentinels certify the
  // side before any proposal is admitted; ineligible or failed lanes remain on
  // the scalar inverter. Disable for a cold-reference A/B comparison.
  bool use_shared_boundary_deam{true};
  // 0 = unlimited = current behavior. Positive = cap the number of OTM strikes
  // per expiry that the LEGACY (`LegacyEssviCompatibility`) observation-prep
  // path de-Americanizes. The legacy prep inverts one (cold-ish) Andersen-Lake
  // solve per OTM strike to build the fit strip; on a liquid name a single wide
  // expiry can carry ~600 strikes → ~600 inversions, yet a 29-knot SplineVol
  // curve is fully constrained by ~40-60 well-spread observations, so most of
  // that work is wasted. When an expiry has MORE candidate strikes than this cap
  // (counted AFTER the cheap validity filter but BEFORE the expensive de-Am
  // inversion), only a deterministic moneyness-SPREAD subset of size `cap` is
  // de-Americanized: the near-ATM core is kept densely (most signal + vega),
  // both extreme wings are pinned (the spline's outer knots stay constrained),
  // and the intermediate strikes are thinned by an even moneyness stride. The
  // dropped candidates are recorded with the `ObservationCap` rejection reason
  // and never inverted. This is a cold-start LATENCY knob only: the forward /
  // borrow carry solve (near-ATM pair set) is untouched, and when scoring is
  // enabled parity still scores the full original chain, so accuracy is
  // unaffected. If an expiry has <= `cap` candidates the prep is BIT-IDENTICAL
  // to the uncapped path. Only the legacy prep honors this field.
  std::uint32_t max_deam_strikes_per_expiry{0};
  // 0 = always de-Americanize each fit row with the configured American-IV
  // solver. Positive = allow an HFT shortcut for OTM rows whose BAW-estimated
  // early-exercise premium at the raw Black-76 IV is at most this fraction of
  // the bid/ask spread; those rows reuse the raw European IV and avoid the cold
  // Andersen-Lake inversion. The default preserves the historical full solve.
  double max_otm_shortcut_premium_spread_frac{0.0};
  // A shortcut, correction-cache result, or fast Andersen-Lake result is only
  // a proposal.  Reprice it with the cold accurate Andersen-Lake map and accept
  // it only inside this fraction of one half-spread; otherwise fall back to an
  // accurate inversion.  This is a hard residual ceiling, not a speed knob.
  double max_inversion_residual_half_spreads{0.25};
  // Diagnostic reference switch. A direct accurate Andersen-Lake inversion is
  // cold-polished against the audit map already, so its second full reprice is
  // redundant. Fast/cache/shortcut and accurate-fallback proposals stay audited.
  bool audit_accurate_inversions{false};
  // Proposal guards. Ultra-short, very low-vega and far-wing observations
  // bypass the raw-European OTM shortcut and go directly to inversion.
  double min_otm_shortcut_T{7.0 / 365.25};
  double min_otm_shortcut_vega{1.0e-4};
  double max_otm_shortcut_abs_k{0.50};
  // Inversion certification tolerates DROPPED nodes (failed inversion or an
  // over-budget residual — both excluded from the fit set and counted in
  // diagnostics) up to this fraction of the rows entering the de-Am stage.
  // Beyond it the slice's certificate is refused: fail-closed at the cap, not
  // at the first bad quote (a lone crossed-into-intrinsic deep quote must not
  // reject an entire risk generation; see deamer.hpp's documented node-drop
  // semantics). Conservative default: one bad node in ten usable ones.
  double max_certified_deam_drop_fraction{0.10};

  // Warm-start regularization.
  double prior_strength{0.0}; // shrinkage toward θ_prev (0 = none, 1 = strong)

  // eSSVI dispatch / fallback.
  EssviRhoMode essvi_rho_mode{EssviRhoMode::PerSlice};
  OptimizationLevel optimization_level{OptimizationLevel::Trading};
  // The defaults are compatibility sentinels. Non-default values are rejected
  // until quality-driven fallback and configurable arb grids are implemented.
  double essvi_fallback_rmse_threshold{kDefaultEssviFallbackRmse};
  std::uint32_t n_butterfly_grid{kDefaultButterflyGrid};

  // Per-level iteration caps. The active `optimization_level` selects one; the
  // legacy max_outer_iter / max_inner_iter apply when the per-level cap is 0.
  std::uint16_t max_iter_quick_mark{8};
  std::uint16_t max_iter_trading{35};
  std::uint16_t max_iter_risk{100};
  std::uint16_t max_iter_reference{250};
  std::uint16_t max_iter_cold_fast{10};

  // Wing-floor weight (Sprint 07 B2). Floors each per-obs weight at
  // α · max(weight) so deep wings keep a non-zero gradient. 0 = disabled.
  double wing_floor_alpha{0.0};

  // Lee-bound projection (B3): project (θ, φ, ρ) onto Lee's admissible cone
  // after each accepted step.
  bool lee_bound_project{true};

  // Morozov discrepancy stop (B6): halt the LM once the weighted residual norm
  // falls below τ · noise_estimate.
  bool morozov_stop{false};
  double morozov_tau{1.1}; // τ multiplier for the Morozov stop

  // Run the static-arb validators at the end and bail on a violation.
  bool validate_no_arb{true};

  // FT-C9a: EXPLICIT opt-in for the alternate eSSVI surface drivers
  // (essvi_calib_surface[_sequential]) to run the surface-level theta-scale
  // calendar projection (arb_project_calendar_essvi) as a post-assembly repair.
  // This is the quality-destroying "Project"-style theta bump the README warns
  // about — it moves the ATM total-variance level to remove calendar crossings.
  // It was previously folded into `validate_no_arb` (default true) and ran
  // silently. DEFAULT FALSE => the alternate driver leaves the fitted ATM term
  // structure untouched (the canonical PricerFitter/run_surface_parity path uses
  // its own MonotoneFit/Project policy and is unaffected by this flag).
  bool essvi_alt_driver_theta_project{false};

  // Wing-residual layer (Sprint 11). Disabled by default; a strict superset of
  // backbone-only when enabled.
  bool residual_disable{true};
  ResidualBasisKind residual_basis_kind{ResidualBasisKind::None}; // C default 0
  std::uint8_t residual_n_basis_terms{0}; // 5..16 for C2Bspline; 0 ⇒ fitter default
  double residual_ridge_factor{0.0};      // 0 ⇒ fitter default (1e-3)

  // Loss function + price-target anchor (2026-05-02 SPY volar-parity review).
  CalibLossKind loss_kind{CalibLossKind::Mid};
  CalibAnchorKind anchor_kind{CalibAnchorKind::Mid};

  // Asymmetric ρ in the eSSVI backbone (Sprint 15). Off ⇒ symmetric 3D LM.
  bool essvi_asymmetric_rho{false};

  // Robustness guards (Sprint 24). `min_obs_per_slice` gates the fitter;
  // `max_post_fit_sigma` rejects a blown-up slice; `max_spread_to_mid_pct` is
  // an extra observation-builder filter on (ask − bid) / mid (0 disables it).
  std::uint32_t min_obs_per_slice{4};
  double max_post_fit_sigma{2.0};
  double max_spread_to_mid_pct{0.60};

  // Opt-in per-slice LinearVariance fallback (coverage recovery). When TRUE and
  // the configured curve kind is NOT already LinearVariance, a per-slice fit
  // failure in the curve-agnostic driver (`fit_curve_surface`) is retried for
  // THAT slice with `VolCurveKind::LinearVariance` before the slice is dropped.
  // A thin per-expiry-sparse name whose primary (e.g. SplineVol) fit needs more
  // usable de-Americanized rows than the funnel yields can then still produce a
  // served linear-in-variance slice from >=2 nodes, instead of dropping the whole
  // board. The fallback slice goes through the SAME `fit_slice_curve` admission
  // (>=2 nodes + the union-grid calendar floor against the prior slice) as any
  // LinearVariance slice — no numerical-sanity check is bypassed. Heterogeneous
  // (SplineVol + LinearVariance) slices in one surface are already supported.
  // DEFAULT FALSE => byte-identical to the historical drop-the-slice behavior;
  // ConvexDense / Svi / eSSVI paths stay bit-identical.
  bool per_slice_linear_fallback{false};
};

// The calibration defaults (`ats_vol_calib_default_opts`). Equal to a
// default-constructed `CalibOpts`; provided for call-site symmetry with the
// C API and with `filter_default_opts()`.
[[nodiscard]] CalibOpts calib_default_opts() noexcept;

// Validate public calibration policy before it reaches a fitting driver.
// Persisted but unimplemented policies are rejected instead of being silently
// ignored. `max_weight` must be finite and strictly positive.
// @return InvalidArgument for malformed values/enums; NotImplemented for a
//         recognized policy this build cannot execute truthfully.
[[nodiscard]] Status validate_calib_options(const CalibOpts &opts) noexcept;

// ── Per-slice fit observation ────────────────────────────────────────────

// One observation per (strike, side) survivor of the quote filter (ports the
// meaningful fields of `AtsVolSviObs`; the C's IRLS `_scratch_resid_sigma` and
// `_pad07` are dropped — the fitters keep their own scratch).
struct FitObs {
  double k{0.0};               // log-moneyness log(K/F)
  double sigma_mkt{0.0};       // observed IV (annualized lognormal)
  double w_mkt{0.0};           // total variance sigma_mkt² · T
  double weight_w{0.0};        // w-space weight: vega² / spread² / (2σT)²
  double active_weight_w{0.0}; // IRLS-mutable copy of weight_w (seeded equal)
  double K{0.0};               // strike (raw)
  double F{0.0};               // forward at this slice
  double df{0.0};              // discount factor for T
  double mid{0.0};             // anchor-aware target price (bid / mid / ask)
  double spread{0.0};          // ask − bid (price units)
  double vega{0.0};            // B76 vega at sigma_mkt
  double noise_sigma{0.0};     // spread / vega — σ-equivalent of the half-spread
  Side side{Side::Call};
  // Stable source identity survives sorting, observation caps, and de-Am.
  std::uint32_t source_strike_index{0};
  // European-equivalent IV recovered from the raw symmetric midpoint. This is
  // the parity-scoring market IV even when `mid` is anchored to bid or ask.
  double score_sigma_mkt{0.0};
};

enum class ObsRejectionReason : std::uint8_t {
  None = 0,
  InvalidStrike,
  QuoteFlag,
  InvalidBidAsk,
  InvalidMid,
  SpreadToMid,
  RawIvFailure,
  RawIvOutOfBand,
  SpreadVol,
  LowVegaWeight,
  ObservationCap,
  Deamericanization,
  EuropeanPrice,
};

struct ObsProvenance {
  std::uint32_t source_strike_index{0};
  Side side{Side::Call};
  ObsRejectionReason rejection{ObsRejectionReason::None};
};

// Diagnostics returned by a per-slice fit (ports `AtsVolSviFitDiag`).
struct FitDiag {
  double rmse_vol_vega_weighted{0.0};
  double max_residual_vol{0.0};
  std::uint16_t outer_iters{0};
  std::uint16_t inner_iters_total{0};
  std::uint32_t n_quotes_used{0};
  // Butterfly no-arb diagnostic (Task C2.5): summed per-slice butterfly
  // violations observed on the research/surface-driver path. Closed-form
  // Martini-Mingone tally for raw-SVI slices; 0 by construction for eSSVI. This
  // is a DIAGNOSTIC COUNT ONLY — the surface drivers do not reject on it (the
  // per-slice serving gates in `fit_slice_curve` do the rejecting).
  std::uint32_t n_butterfly_viol{0};
};

struct InversionRouteDiagnostics {
  std::uint32_t n_proposed{0};
  // Logical certifications. A successful direct accurate Andersen-Lake solve
  // counts here without repeating its identical forward map.
  std::uint32_t n_audited{0};
  // Actual independent cold Andersen-Lake reference reprices performed.
  std::uint32_t n_reference_reprices{0};
  std::uint32_t n_accepted{0};
  std::uint32_t n_fallback{0};
  double p50_residual_half_spreads{0.0};
  double p95_residual_half_spreads{0.0};
  double max_residual_half_spreads{0.0};
};

// Per-observation de-Americanization audit summary.  The route buckets are
// mutually exclusive for the initial proposal; accurate fallbacks are counted
// on the originating route and in n_accurate_fallback.
struct DeAmAuditDiagnostics {
  InversionRouteDiagnostics shortcut{};
  InversionRouteDiagnostics cache{};
  InversionRouteDiagnostics fast{};
  InversionRouteDiagnostics accurate{};
  std::uint32_t n_forced_short_tenor{0};
  std::uint32_t n_forced_low_vega{0};
  std::uint32_t n_forced_far_wing{0};
  std::uint32_t n_accurate_fallback{0};
  std::uint32_t n_rejected_residual{0};
  // Row-level ledger for the fit's price-to-IV inversion stage (route counters
  // can double count a fallback row across two routes; these never do). For an
  // American contract this is the de-Am inversion; for an already-European
  // contract it is the direct Black-76 inversion plus explicit Black-76
  // reprice. `n_deam_rows` counts rows entering that certification stage and
  // `n_deam_accepted` counts rows surviving into the European observation set.
  // The historical field names remain stable for persisted diagnostics.
  std::uint32_t n_deam_rows{0};
  std::uint32_t n_deam_accepted{0};
  // W3.1 shared-boundary route. Boundary work is constant per eligible side
  // (nine build nodes) plus bounded cold sentinels and selective scalar lanes.
  std::uint32_t n_shared_boundary_lanes{0};
  std::uint32_t n_shared_call_lanes{0};
  std::uint32_t n_shared_put_lanes{0};
  // R-32 — EXACT semantics, because this counter reads like a total and is not
  // one. It counts ONLY the nine Chebyshev BUILD solves per eligible side (so a
  // two-sided board reads 18), and nothing else. It EXCLUDES all cold boundary
  // work done while certifying the side: each sentinel's `american_implied_vol`
  // runs a root-find whose every residual evaluation is its own cold solve, and
  // each sentinel's confirming `american_price` is one more. Those are real
  // boundary solves — they are visible in the global `counters::BoundarySolves`,
  // just not here — so this counter UNDERSTATES the route's true boundary work,
  // by a factor driven by the sentinels' inversion iteration counts rather than
  // by any fixed multiple. Both parts stay O(1) per side (nine nodes plus at
  // most three bounded sentinels), which is the property the route claims; the
  // counter is a build-node tally, not a boundary-work budget. Tests pin these
  // values, so the semantics are fixed: measure real work with the global
  // counter, not this one.
  std::uint32_t n_shared_boundary_solves{0};
  std::uint32_t n_shared_sentinel_reprices{0};
  std::uint32_t n_shared_scalar_fallback_lanes{0};
};

// The output of `build_observations`: the surviving rows plus the count of
// quotes rejected by the filter cascade (`out_n_dropped` in the C).
struct ObsSet {
  std::vector<FitObs> obs;
  // One preferred-leg record per source strike, in source strike order.
  std::vector<ObsProvenance> provenance;
  std::uint32_t n_dropped{0};
  // Independent raw-mid American-IV inversions performed only for parity
  // scoring semantics (anchor, cap warm-start, or OTM shortcut).
  std::uint32_t n_score_inversions{0};
  DeAmAuditDiagnostics deam_audit{};
};

// ── Observation builder + accept predicate ───────────────────────────────

// Build the observation set for one chain's slice, applying the quote-filter
// cascade exactly as the C `ats_vol_svi_build_observations`:
//
//   for each strike K (K > 0), pick the preferred leg (call if K ≥ F, else
//   put), read bid/ask/flags at chain_index(strike_idx, side), and DROP the
//   row when any of the following holds:
//     1. any kill-mask flag is set
//        (Locked/Crossed/Stale/Halted/WideSpread/Penny/LowVega);
//     2. bid ≤ 0 or ask ≤ bid;
//     3. mid ≤ 0;
//     4. (ask − bid) / mid > max_spread_to_mid_pct   (when the cap > 0);
//     5. IV inversion of the raw mid fails, or the IV ∉ (0.005, 5.0);
//     6. spread_vol = (ask − bid) / vega > max_spread_vol   (when vega > 1e-12);
//     7. weight_sigma = vega² / spread² < min_vega_weight.
//   The non-preferred leg is silently skipped (NOT counted as a drop).
//
// The stored `FitObs::mid` is the anchor target (bid / mid / ask per
// `opts.anchor_kind`); the IV inversion always uses the raw symmetric mid.
//
// @return InvalidArgument if F ≤ 0 or T ≤ 0, or if the chain's per-side SoA
//         arrays are shorter than 2·n_strikes (malformed chain);
//         NotFound (maps the C ERR_NO_DATA) if fewer than 5 rows survive;
//         otherwise Ok with the surviving observations and the drop count.
// For an explicitly European chain, every surviving Black-76 IV is repriced
// against its raw midpoint and recorded in `ObsSet::deam_audit`; a row outside
// `max_inversion_residual_half_spreads` is dropped before the usable-row floor.
[[nodiscard]] Result<ObsSet> build_observations(const Chain &chain, double F, double T, double df,
                                                const CalibOpts &opts);

// De-Americanized ("European-equivalent") observation builder. Runs the SAME
// filter cascade as `build_observations`, but each surviving leg's American
// premium is stripped to its European equivalent before it is stored: the mid is
// inverted to a European-equivalent lognormal vol via `american_implied_vol` on
// the carry (S, r, q_eff = r − ln(F/S)/T), and `FitObs::{mid, sigma_mkt, w_mkt,
// vega, weight_w}` are restated in European terms (mid = Black-76 price at that
// vol). `k`, `K`, `F`, `df`, `spread`, `side` are unchanged.
//
// This is the correct input for the convex dense-slice fit (`fit_convex_slice`),
// which folds put→call via EUROPEAN put-call parity `C = P + df·(F−K)` and whose
// model IV is re-Americanized downstream: feeding it raw American mids leaves the
// (large, put-side) early-exercise premium in and biases the near-money put wing
// systematically high. With European inputs the fit→re-Americanize round-trips.
//
// @return same error contract as `build_observations`; additionally, a leg whose
//         de-Americanization fails or leaves the band is counted as a drop.
// `caches` (optional) routes the per-strike American de-Americanization through
// the cached hot path (Black-76 + Chebyshev correction) instead of the cold
// Andersen-Lake solve — the SAME accurate, self-consistent de-Am the eSSVI path
// uses, orders of magnitude faster on a wide board. Default-empty selects the
// accurate cold map; adjacent-strike seeds may move the converged result by a
// few ULPs while remaining inside the documented economic tolerance.
//
// `al_opts` / `iv_tol` / `iv_max_iter` tune the COLD Andersen-Lake inversion (the
// path taken when `caches` is empty). They default to the ACCURATE preset
// (nullopt = al_default_opts, 1e-7 / 64).
// With `opts.use_shared_boundary_deam`, a sufficiently wide positive-rate,
// non-short expiry/side uses one retained sigma-boundary interpolant before
// scalar inversion. An embedded estimator and bounded accurate sentinels must
// clear the economic price/IV budgets; failed sides and individual lanes fall
// back to this same scalar path in deterministic source order.
// The fast-preset served path (session Fast/Hft) passes its `DeAmOptions`
// al_opts (al_fast_opts) + iv_tol here so the per-strike de-Am honors the SAME
// fast-cold accuracy as the borrow solve — the surface only needs ~1e-4 price
// accuracy (RMSE ~1e-2), and machine-precision cold AL was ~5x wasted cost.
[[nodiscard]] Result<ObsSet>
build_observations_european(const Chain &chain, double S, double r, double F, double T, double df,
                            const CalibOpts &opts, const AmericanCorrectionCaches &caches = {},
                            const std::optional<AlOpts> &al_opts = std::nullopt,
                            double iv_tol = 1.0e-7, std::uint16_t iv_max_iter = 64,
                            AmericanMethod method = AmericanMethod::AndersenLake,
                            bool prepare_scoring = true);

// F1 (R-01p2) shared-boundary de-Am LANE BATCH — the NEW entry point that lets
// the Legacy/eSSVI prepare path de-Americanize a slice through the SAME retained
// sigma-boundary interpolant `build_observations_european` uses (one boundary
// solve per slice-side across strikes) instead of a per-row scalar
// `american_implied_vol`. It is the exact machinery the Configured builder runs
// internally, re-exported so the Legacy driver can share it.
//
// Contract (mirrors §5.3/§8.1 — "a shortcut may PROPOSE an answer; it may not
// certify its own answer"): for every `rows[i]` the batch solves AND certifies to
// the economic price/IV budget (a nine-node interpolant plus bounded accurate
// cold-Andersen-Lake sentinels that must clear |ΔIV| ≤ 1e-4 and the per-lane
// price budget), the row's European-equivalent IV is written into
// `rows[i].score_sigma_mkt`. Every other row is left `kUnscoredIv` (NaN). The
// per-row scalar inverter (`european_equiv_iv` / `american_implied_vol`) stays the
// numerical SOURCE OF TRUTH and PARITY ORACLE: the caller MUST invert every
// unscored row scalar (byte-identical to the pre-batch path), and the batch never
// forces a route on the caller.
//
// Each `rows[i]` must carry the raw American observation the batch needs — `mid`
// (the OTM-leg American premium to de-Americanize), `K`, `side`, `spread`
// (ask−bid, which drives the per-lane price budget). The Black-76 upper-bound seed
// (`sigma_mkt`) and B76 `vega` are (re)derived INTERNALLY exactly as
// `build_observations` derives them (`sigma_mkt = implied_vol(mid, F, K, T, df,
// side)`), so the caller need not pre-seed them; a row whose European seed
// inversion fails or leaves the band is left unscored (scalar fallback). `F`/`df`
// are the shared per-slice forward/discount; the carry is q_eff = r − ln(F/S)/T,
// exactly as the Configured builder forms it, so the two paths de-Americanize on
// the identical forward. Engagement honours `opts.use_shared_boundary_deam` and
// the same guards `build_observations_european` applies (Mid anchor, Andersen-
// Lake, r ≥ 0, a wide-enough positive-rate side); when any guard fails the batch
// writes nothing and returns 0 (all rows fall to scalar). `audit` accrues the
// same DeAmAuditDiagnostics the internal path records (may be a throwaway).
//
// @return the number of rows certified (i.e. `score_sigma_mkt` written).
[[nodiscard]] std::size_t
shared_boundary_deam_batch(std::span<FitObs> rows, double S, double r, double F, double T, double df,
                           const CalibOpts &opts, const AmericanCorrectionCaches &caches = {},
                           const std::optional<AlOpts> &al_opts = std::nullopt,
                           double iv_tol = 1.0e-7, std::uint16_t iv_max_iter = 64,
                           AmericanMethod method = AmericanMethod::AndersenLake,
                           DeAmAuditDiagnostics *audit = nullptr);

// Inversion certificate over one slice's price-to-IV audit (charter
// §5.3/§8.1: "a cache or shortcut may propose an answer; it may not certify
// its own answer"; residual budgets bind on ACCEPTED nodes). True iff
//   1. every route accepted no more proposals than it audited — a method with
//      no cold-reference audit (e.g. AmericanMethod::Baw) can never certify;
//   2. at least one row entered the certification stage and at least one survived; and
//   3. the dropped-row fraction (failed inversion / over-budget residual —
//      dropped nodes never reach the fit set) is within `max_drop_fraction`.
// Fail-closed on a non-finite or negative budget.
[[nodiscard]] bool deam_inversion_certified(const DeAmAuditDiagnostics &audit,
                                            double max_drop_fraction) noexcept;

namespace detail {

// W3.1 shared-boundary per-lane price-acceptance gate, exposed for direct test
// (internal: not part of the supported API surface).
//
// A shared lane's accepted sigma is the root of the NINE-node interpolated price
// map. Two distinct errors separate that map's value from the true American
// price at the same sigma:
//   * `price - mid`  — the root-find residual the lane actually converged to;
//   * `price - embedded` — the 9-vs-5 Richardson gap, which is this route's only
//     ESTIMATE of the nine-node map's own interpolation error.
// The quantity the sprint bounds is the true price error, and by the triangle
// inequality
//     |price_true(sigma) - mid| <= |price_true(sigma) - price| + |price - mid|
//                              ~= |price - embedded|          + |price - mid|,
// so the SUM is what must clear `budget`. Gating each term against `budget`
// independently would only prove the sum is within 2 x budget — a bound the
// sprint never claimed. Returns true iff the lane is acceptable.
//
// Fail-closed: a non-finite input or a non-positive budget is never acceptable.
[[nodiscard]] bool shared_lane_residual_within_budget(double price, double mid, double embedded,
                                                      double budget) noexcept;

// W3.1 shared-boundary per-lane root-finding bracket, exposed for direct test
// (internal: not part of the supported API surface).
//
// Brackets a root of the nine-node interpolated price map in sigma. The caller
// establishes the invariant `f_lo < 0 <= f_hi` on `[lo, hi]` before the first
// step; every `update` call with a FINITE `residual` preserves it. Evaluator-
// agnostic on purpose: the lane loop supplies residuals from the interpolant,
// and the unit test supplies them from a closed-form price, so the test drives
// the SAME stepping logic production runs.
//
// Precondition on `update`: `residual` must be finite. A non-finite residual is
// not sign-tested (`NaN < 0.0` is false), so it falls into the `f_hi = residual`
// branch and writes NaN over the invariant regardless of its true sign. The sole
// production caller (`iterate_shared_lanes`, calib.cpp) already checks
// `std::isfinite(residual)` before calling `update` and never calls it
// otherwise; any other caller (this type is an exposed `detail` type, not
// enforced by the compiler) must do the same.
//
// Termination is on bracket WIDTH (`hi - lo <= solve_tol`), because that is what
// `finalize_shared_lane` re-tests before accepting a lane -- so the width, not the
// residual, is the quantity a step must contract.
struct SharedLaneBracket {
  // Steps after which the secant is abandoned and every remaining step bisects
  // unconditionally. This is the constructive iteration bound (see next_sigma):
  // Illinois converges this map in ~5 steps and at worst 11 measured, so at 24
  // the backstop is >2x clear of the real workload and never fires in practice;
  // once it does fire, each step halves, and the widest bracket the route admits
  // (w0 <= kObsIvMax - kSharedMinSigma = 4.99) needs at most
  // ceil(log2(4.99 / 1e-9)) = 33 halvings against the tightest solve_tol. So a
  // lane terminates within 24 + 33 = 57 evaluations, inside the max_iter = 64 the
  // production route passes.
  static constexpr std::uint16_t kMaxSecantSteps = 24u;

  double lo{0.0};
  double hi{0.0};
  double f_lo{0.0};
  double f_hi{0.0};
  // Which endpoint survived the previous update: +1 = hi, -1 = lo, 0 = no update
  // yet. Drives the Illinois deflation in `update`.
  std::int8_t retained{0};
  // Steps folded in so far; arms the bisection backstop above.
  std::uint16_t steps{0};

  // Next sigma to probe: the Illinois-modified regula-falsi step while it lands
  // strictly inside the bracket, else the midpoint.
  [[nodiscard]] double next_sigma() const noexcept;

  // Fold a probe `(sigma, residual)` into the bracket, keeping the side whose
  // sign it matches, then apply the Illinois deflation (see below).
  // PRECONDITION: `residual` must be finite -- the caller establishes this (see
  // the struct comment above).
  void update(double sigma, double residual) noexcept;
};

} // namespace detail

// O(1) calibrator-population predicate (ports `ats_vol_calib_obs_accepted`):
// would the (strike_idx, side) tuple survive the same cascade as one row of
// `build_observations`? The `max_spread_to_mid_pct` filter is mirrored; the
// tenor-bucket path is not ported, so the scalar `max_spread_vol` /
// `min_vega_weight` are used (identical to a legacy-bucket build).
//
// @return Ok(iv) — the inverted IV the calibrator would anchor to — when the
//         tuple is accepted; NotFound when it is rejected; InvalidArgument if
//         `strike_idx` is out of range or F/T/df ≤ 0.
[[nodiscard]] Result<double> obs_accepted(const Chain &chain, std::uint16_t strike_idx, Side side,
                                          double F, double T, double df, const CalibOpts &opts);

} // namespace atx::vol
