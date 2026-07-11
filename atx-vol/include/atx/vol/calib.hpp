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
#include <vector>

#include "atx/vol/american.hpp"     // AlOpts (de-Am Andersen-Lake accuracy preset)
#include "atx/vol/correction.hpp"   // AmericanCorrectionCaches (optional de-Am hot path)
#include "atx/vol/types.hpp"        // Side, Result
#include "atx/vol/universe.hpp"     // Chain (SoA quote layout)
#include "atx/vol/vol_surface.hpp"  // ResidualBasisKind (reused, not redefined)

namespace atx::vol {

// ── Calibration policy enums ─────────────────────────────────────────────

// eSSVI ρ-handling mode (AtsVolEssviRhoMode). PerSlice is the shipping
// default; Shared / TermStructure are reserved for later work.
enum class EssviRhoMode : std::uint8_t {
  PerSlice = 0,       // one ρ per expiry (default)
  Shared = 1,         // one ρ for the whole surface
  TermStructure = 2,  // ρ(T) piecewise (future)
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
  Interval = 1,
};

// Calibration price-target anchor (AtsVolCalibAnchorKind). The observation
// builder writes the chosen target into `FitObs::mid`; the loss + Jacobian
// consume that field unchanged.
enum class CalibAnchorKind : std::uint8_t {
  Mid = 0,  // target = ½·(bid + ask) (default)
  Bid = 1,  // target = bid
  Ask = 2,  // target = ask
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
struct CalibOpts {
  // IRLS / Newton.
  std::uint16_t max_outer_iter{4};   // IRLS reweighting passes
  std::uint16_t max_inner_iter{12};  // Newton steps per pass
  double tol_param{1.0e-9};          // convergence on parameter norm
  double tol_residual{1.0e-10};      // convergence on residual change

  // Robust loss.
  double huber_k{1.5};  // Huber threshold in vega-weighted residual std-devs

  // Quote filtering (read by the observation builder / accept predicate).
  double min_vega_weight{1.0e-6};  // drop below this weight_sigma = (vega/spread)²
  double max_spread_vol{0.05};     // drop quotes with spread/vega above this
  double max_weight{1.0e3};        // upper clip on vega-spread weights
  // 0 = use every surviving observation. Positive = cap the per-slice
  // de-Americanized fit population before the expensive American-IV inversion,
  // selecting adaptive knots by normalized total-variance interpolation error.
  // This is a cold-start latency knob for very dense index boards; the default
  // preserves the historical full-board fit exactly.
  std::uint32_t max_obs_per_slice{0};
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
  // Proposal guards. Ultra-short, very low-vega and far-wing observations
  // bypass the raw-European OTM shortcut and go directly to inversion.
  double min_otm_shortcut_T{7.0 / 365.25};
  double min_otm_shortcut_vega{1.0e-4};
  double max_otm_shortcut_abs_k{0.50};

  // Warm-start regularization.
  double prior_strength{0.0};  // shrinkage toward θ_prev (0 = none, 1 = strong)

  // eSSVI dispatch / fallback.
  EssviRhoMode essvi_rho_mode{EssviRhoMode::PerSlice};
  OptimizationLevel optimization_level{OptimizationLevel::Trading};
  double essvi_fallback_rmse_threshold{0.01};  // vol pts; > this ⇒ fall back to SVI
  std::uint32_t n_butterfly_grid{200};         // k-grid resolution for arb checks

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
  double morozov_tau{1.1};  // τ multiplier for the Morozov stop

  // Run the static-arb validators at the end and bail on a violation.
  bool validate_no_arb{true};

  // Wing-residual layer (Sprint 11). Disabled by default; a strict superset of
  // backbone-only when enabled.
  bool residual_disable{true};
  ResidualBasisKind residual_basis_kind{ResidualBasisKind::None};  // C default 0
  std::uint8_t residual_n_basis_terms{0};  // 5..16 for C2Bspline; 0 ⇒ fitter default
  double residual_ridge_factor{0.0};       // 0 ⇒ fitter default (1e-3)

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
};

// The calibration defaults (`ats_vol_calib_default_opts`). Equal to a
// default-constructed `CalibOpts`; provided for call-site symmetry with the
// C API and with `filter_default_opts()`.
[[nodiscard]] CalibOpts calib_default_opts() noexcept;

// ── Per-slice fit observation ────────────────────────────────────────────

// One observation per (strike, side) survivor of the quote filter (ports the
// meaningful fields of `AtsVolSviObs`; the C's IRLS `_scratch_resid_sigma` and
// `_pad07` are dropped — the fitters keep their own scratch).
struct FitObs {
  double k{0.0};                // log-moneyness log(K/F)
  double sigma_mkt{0.0};        // observed IV (annualized lognormal)
  double w_mkt{0.0};            // total variance sigma_mkt² · T
  double weight_w{0.0};         // w-space weight: vega² / spread² / (2σT)²
  double active_weight_w{0.0};  // IRLS-mutable copy of weight_w (seeded equal)
  double K{0.0};                // strike (raw)
  double F{0.0};                // forward at this slice
  double df{0.0};               // discount factor for T
  double mid{0.0};              // anchor-aware target price (bid / mid / ask)
  double spread{0.0};           // ask − bid (price units)
  double vega{0.0};             // B76 vega at sigma_mkt
  double noise_sigma{0.0};      // spread / vega — σ-equivalent of the half-spread
  Side side{Side::Call};
};

// Diagnostics returned by a per-slice fit (ports `AtsVolSviFitDiag`).
struct FitDiag {
  double rmse_vol_vega_weighted{0.0};
  double max_residual_vol{0.0};
  std::uint16_t outer_iters{0};
  std::uint16_t inner_iters_total{0};
  std::uint32_t n_quotes_used{0};
};

struct InversionRouteDiagnostics {
  std::uint32_t n_proposed{0};
  std::uint32_t n_audited{0};
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
};

// The output of `build_observations`: the surviving rows plus the count of
// quotes rejected by the filter cascade (`out_n_dropped` in the C).
struct ObsSet {
  std::vector<FitObs> obs;
  std::uint32_t n_dropped{0};
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
[[nodiscard]] Result<ObsSet> build_observations(const Chain &chain, double F,
                                                double T, double df,
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
// uses, orders of magnitude faster on a wide board. Default-empty => cold
// (bit-identical to the historical behaviour).
//
// `al_opts` / `iv_tol` / `iv_max_iter` tune the COLD Andersen-Lake inversion (the
// path taken when `caches` is empty). They default to the ACCURATE preset
// (nullopt = al_default_opts, 1e-7 / 64) so an unchanged caller is bit-identical.
// The fast-preset served path (session Fast/Hft) passes its `DeAmOptions`
// al_opts (al_fast_opts) + iv_tol here so the per-strike de-Am honors the SAME
// fast-cold accuracy as the borrow solve — the surface only needs ~1e-4 price
// accuracy (RMSE ~1e-2), and machine-precision cold AL was ~5x wasted cost.
[[nodiscard]] Result<ObsSet>
build_observations_european(const Chain &chain, double S, double r, double F, double T, double df,
                            const CalibOpts &opts, const AmericanCorrectionCaches &caches = {},
                            const std::optional<AlOpts> &al_opts = std::nullopt,
                            double iv_tol = 1.0e-7, std::uint16_t iv_max_iter = 64,
                            AmericanMethod method = AmericanMethod::AndersenLake);

// O(1) calibrator-population predicate (ports `ats_vol_calib_obs_accepted`):
// would the (strike_idx, side) tuple survive the same cascade as one row of
// `build_observations`? The `max_spread_to_mid_pct` filter is mirrored; the
// tenor-bucket path is not ported, so the scalar `max_spread_vol` /
// `min_vega_weight` are used (identical to a legacy-bucket build).
//
// @return Ok(iv) — the inverted IV the calibrator would anchor to — when the
//         tuple is accepted; NotFound when it is rejected; InvalidArgument if
//         `strike_idx` is out of range or F/T/df ≤ 0.
[[nodiscard]] Result<double> obs_accepted(const Chain &chain,
                                          std::uint16_t strike_idx, Side side,
                                          double F, double T, double df,
                                          const CalibOpts &opts);

}  // namespace atx::vol
