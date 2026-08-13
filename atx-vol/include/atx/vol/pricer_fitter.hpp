#pragma once

// PricerFitter — the atx-vol library lifecycle in one object: fit a surface from
// an OptionChain, OWN it (unique_ptr<FittedSurface>), and price the chain in
// parallel with per-field output flags.
//
//   OptionChain chain = OptionChain::from_frame(frame, r, spot).value();
//   PricerFitter fitter{ PricerConfig{ .preset = FitPreset::Robust } };
//   fitter.fit(chain);                                   // stores the surface
//   auto val = fitter.value_chain(chain,                 // parallel valuation
//       OutputField::ModelPrice | OutputField::BidIV | OutputField::AskIV);
//   // ... a tick arrives ...
//   chain.update_quotes(ids, bids, asks);                // replace bid/ask by id
//   auto bands = fitter.value_chain(chain, OutputField::Bands);   // re-price
//
// The fitter composes the validated primitives — `VolaSession` (fit + cached
// query), `american_implied_vol` (cold IV inversion), `VolSurface` — so every
// number it produces is bit-consistent with those. It adds the two things the
// facade needs on top: ownership of the fitted surface, and a deterministic
// multi-threaded evaluator over the whole chain.
//
// Thread-safety: `fit` mutates (stores the surface) and needs exclusive access.
// `value_chain` is const and internally parallel; concurrent `value_chain` calls
// on one fitter are safe (all state read is immutable after `fit`).
//
// "Internally parallel" means it dispatches onto the PROCESS-GLOBAL pricing pool
// (detail/pricing_executor.hpp), not onto threads the fitter owns: concurrent
// `value_chain` calls therefore share one core budget rather than each spawning a
// fan, `cfg.n_threads` is a request clamped down to that pool, and a call issued
// from inside another pool dispatch runs inline — with the result unchanged in
// every case. The pool's ordering rule reaches callers here too: set its topology
// with `configure_pricing_executor` before the first pricing or fitting call
// builds it, after which configuration is refused with AlreadyExists.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "atx/core/error.hpp"          // Error
#include "atx/vol/american.hpp"        // AmericanGreeks
#include "atx/vol/chain.hpp"           // OptionChain, OptionId
#include "atx/vol/curve_selector.hpp"  // SelectorConfig, SelectorResult
#include "atx/vol/fit_policy.hpp"      // FitContext, FitPolicyConfig, FitDecision
#include "atx/vol/detail/prepared_policy.hpp" // PreparedObservationPolicy
#include "atx/vol/rates_curve.hpp"     // DividendEvent
#include "atx/vol/session.hpp"         // VolaSession, FitPreset, SessionDiagnostics
#include "atx/vol/surface_policy.hpp"  // explicit mark/risk purpose and quality policy
#include "atx/vol/types.hpp"           // Result, Status, Side
#include "atx/vol/vol_curve.hpp"       // CurveConfig, VolCurveKind

namespace atx::vol {

// ── Output-field selector ──────────────────────────────────────────────────
//
// Which per-option outputs a chain valuation should populate. Bitmask; combine
// with `|`. Unrequested scalar columns stay empty; a requested-but-failed cell
// is NaN, so a bad quote never sinks the rest of the valuation.
enum class OutputField : std::uint32_t {
  None = 0,
  ModelPrice = 1u << 0, // re-Americanized model fair value at (K, T, side)
  ModelIV = 1u << 1,    // surface European-equivalent IV at (K, T)
  BidIV = 1u << 2,      // American implied vol of the bid (on the fit's carry)
  AskIV = 1u << 3,      // American implied vol of the ask
  MidIV = 1u << 4,      // American implied vol of the mid
  Greeks = 1u << 5,     // full AmericanGreeks bundle at the model IV
  Prices = ModelPrice | ModelIV,
  Bands = BidIV | AskIV | MidIV,
  All = ModelPrice | ModelIV | BidIV | AskIV | MidIV | Greeks,
};

[[nodiscard]] constexpr OutputField operator|(OutputField a, OutputField b) noexcept {
  return static_cast<OutputField>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr OutputField operator&(OutputField a, OutputField b) noexcept {
  return static_cast<OutputField>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr bool has(OutputField set, OutputField flag) noexcept {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(flag)) != 0u;
}

// ── SoA valuation result ────────────────────────────────────────────────────
//
// One row per queried OptionId (row i <-> ids[i]). Only the requested columns
// are populated (others empty); requested-but-failed cells are NaN.
struct ChainValuation {
  std::vector<OptionId> ids;
  std::vector<double> model_price;
  std::vector<double> model_iv;
  std::vector<double> bid_iv;
  std::vector<double> ask_iv;
  std::vector<double> mid_iv;
  std::vector<AmericanGreeks> greeks;
  OutputField filled{OutputField::None};

  // NaN provenance for requested bid/ask IV bands. `unset` means inversion
  // was not attempted because the quote side was non-positive/non-finite or
  // the contract itself was degenerate. `iv_fail` means a finite positive
  // quote was attempted but could not be inverted. Both leave the output NaN.
  std::size_t n_bid_unset{0};
  std::size_t n_ask_unset{0};
  std::size_t n_bid_iv_fail{0};
  std::size_t n_ask_iv_fail{0};

  [[nodiscard]] std::size_t size() const noexcept { return ids.size(); }

  // Row index for an id (linear scan), or nullopt.
  [[nodiscard]] std::optional<std::size_t> row_of(OptionId id) const;
};

// ── Fallback ladder ─────────────────────────────────────────────────────────
//
// Curve families a failed `primary` is retried with, in order, for any board the
// policy routed (a caller-pinned curve is never substituted). A profile is a
// latency prior, not permission to drop an underlier, so every kind declares at
// least one rung and no rung repeats its own primary. `FitDecision::used_fallback`
// and `::primary_curve` record what happened. Exposed so the routing contract is
// inspectable, not just observable after a failure.
//
// BORROW with NO lifetime bound, unusually for this library: the returned span
// names a function-local `static constexpr` ladder with process lifetime, so it
// is immortal, never invalidated, and safe to hold or read from any thread. It is
// also read-only and never aliases caller storage. Nothing here needs copying out.
[[nodiscard]] std::span<const VolCurveKind> fallback_curve_rungs(VolCurveKind primary) noexcept;

// ── Independent-failure merge seam ──────────────────────────────────────────
//
// Merge a candidate session's non-geometric failure context — carry
// confidence, inversion certification, expiry-coverage gaps (CarryGap), and
// the ConvexDense served-price bound self-check (PriceBounds, oracle I-2) —
// into the independent oracle's geometric digest. Strictly fail-closed:
// OR-only on the failure bits and additive-only on the counts; it can never
// clear a failure the geometric validator already found. Used by BOTH
// PricerFitter::fit()'s candidate validation and refit_risk_slice, so a
// successful incremental publish cannot launder a fit-time Degraded reason
// into clean Healthy (§5.2). Exposed so the seam → admission contract is
// directly testable without engineering a board that defeats the fail-closed
// QP (call `finalize_validation_digest` afterwards to re-stamp the id).
void merge_session_failure_context(const SessionDiagnostics &diagnostics,
                                   ValidationDigest &digest) noexcept;

// ── Fit policy ──────────────────────────────────────────────────────────────
//
// Thin bundle over the session's fit knobs plus the evaluation thread count.
// Every field defaults so `PricerConfig{}` is a valid market-maker config. With
// no pinned curve, the unified policy routes dense ETF/index boards to the HFT
// linear-variance path, sparse boards to SVI, event boards to C8, and validates
// ambiguous boards out of sample.
struct PricerConfig {
  // Preset used for pinned curves and as the fallback policy for a forced
  // cross-validation. Auto policy may choose a profile-specific effective
  // preset; set Hft explicitly to retain the legacy hard-pinned dense route.
  FitPreset preset{FitPreset::Robust};
  // Curve family + per-kind knobs. std::nullopt (the default) lets the profile
  // policy route confident boards and sends ambiguous boards through `selector`.
  // Set it explicitly to pin a curve.
  std::optional<CurveConfig> curve{};
  // Search policy used only when `curve` is std::nullopt. Production defaults
  // to a single broad-coverage eSSVI candidate, which removes the historical
  // unbounded five-family search. Explicit research callers may assign
  // SelectorConfig{} or their own bounded ladder.
  SelectorConfig selector{production_selector_config()};
  // Unified profile/session/event auto-selection policy and per-snapshot hints.
  FitPolicyConfig policy{};
  FitContext context{};
  // Family-neutral publication gate. Default is the Mark-serving contract: it
  // admits the healthy real-world surfaces a mark consumer serves (relaxing full
  // expiry coverage, front-expiry, consecutive-gap, calendar-arb, and the
  // strike/calendar shape invariants) while still rejecting garbage via the
  // consumer-independent numerical-sanity gates. Strict risk admission is the
  // explicit opt-in -- assign `risk_admission_policy()` -- per WP12 staging.
  FitAdmissionPolicy admission{};
  // Optional overrides for the preset's cold-fit diagnostic/quality-speed knobs.
  // nullopt => use the preset default, with one deliberate exception: a board
  // the LIBRARY routed (no `curve` pin and not the preset-pinned Hft dense
  // route) ALWAYS scores parity, whatever the admission policy consumes (W3-A,
  // principle P4). A published surface must carry evidence of how well the
  // family the library chose actually fits; a floor-free Mark policy left that
  // evidence unmeasured on every non-eSSVI route, which reported zeroed rather
  // than poor diagnostics. A CALLER-pinned curve under a floor-free Mark
  // admission keeps the opt-out -- an explicit pin includes its latency budget.
  // A Mark bid/ask quality floor and Quote/Risk admission retain scoring by
  // default. An explicit false skips the second de-Am diagnostic pass on every
  // route and therefore fails closed when admission requires that evidence.
  // false for `enforce_calendar_floor` maximizes raw in-band fit quality by
  // fitting dense slices independently.
  std::optional<bool> use_correction_cache{};
  // Explicit query-time pricing contract. LegacyCompatible preserves historical
  // serving, ColdReference forces cold Andersen-Lake/FD, RepresentativeFast uses
  // one term-wide carry surrogate, and CarryBank interpolates a bounded bank.
  // Fast tiers are opt-in and independent of the cache-build override above.
  QueryPricingTier query_pricing_tier{QueryPricingTier::LegacyCompatible};
  std::optional<bool> score_parity{};
  std::optional<bool> enforce_calendar_floor{};
  std::optional<bool> use_deam_cache_for_fit{};
  // Optional overrides for the polymorphic-fit observation-preparation policy and
  // its fit-inversion audit. nullopt (default) => no override, bit-identical to
  // the session default (Configured / preset-derived audit). Set fit_prep_policy
  // to LegacyEssviCompatibility to keep thin single-name expiries the strict
  // usable-row floor would starve (see SessionInputs::fit_prep_policy);
  // audit_fit_inversions gates the lenient path's cold-reference fit audit.
  std::optional<PreparedObservationPolicy> fit_prep_policy{};
  std::optional<bool> audit_fit_inversions{};
  // W2-B: the family-neutral preparation decision (detail/prepared_policy.hpp),
  // copied onto `SessionInputs::prep`. Preparation strictness used to be an
  // accident of curve-family choice — the auto-router sends illiquid small caps
  // to SVI, and SVI drew the STRICTEST quote filter while eSSVI drew the
  // permissive one — so a thin board could not ask for the population it needs
  // without also changing the family it fits with. Naming `prep.strictness`
  // decides that independently of `curve`, and the two rescue tri-states are the
  // only route to `SurfaceParityInputs::per_slice_legacy_prep_fallback` and
  // `CalibOpts::per_slice_linear_fallback`, which were otherwise unreachable
  // from this config.
  //
  // Every field defaults to `Auto`; see `resolve_preparation_policy` for what
  // that resolves to per fit lane. `Auto` reproduces the historical preparation
  // except that the Legacy-prep rescue is ON for non-eSSVI families.
  //
  // The legacy `fit_prep_policy` above still works and is folded in as
  // `PrepStrictness::Permissive` when `prep.strictness` is left at `Auto`.
  PreparationPolicyRequest prep{};
  // Opt into the cross-pair warm start for the term-borrow carry solve
  // (DeAmOptions::warm_start_carry): seeds each near-ATM pair's borrow
  // fixed-point from the previous pair's converged state, cutting the
  // American-solve count in the de-Am hot path. nullopt => no override
  // (bit-identical cold carry solve).
  std::optional<bool> warm_start_carry{};
  // Optional per-slice cap applied before American-IV de-Am inversion. nullopt
  // => use the preset default; 0 => no cap.
  std::optional<std::uint32_t> max_obs_per_slice{};
  // Optional cap on the number of OTM strikes de-Americanized per expiry in the
  // legacy observation prep (CalibOpts::max_deam_strikes_per_expiry). nullopt =>
  // preset default (0 = unlimited). Setting e.g. 64 for a pinned SplineVol fit
  // bounds the per-strike inversion strip -- a 29-knot cubic spline is fully
  // determined by a moneyness-spread subset -- cutting fit latency on wide,
  // liquid boards without touching the forward/borrow carry solve. The subsample
  // pins both wing extremes + a dense near-ATM core, so the served fit and its
  // in-bidask/calendar-arb quality are unchanged.
  std::optional<std::uint32_t> max_deam_strikes_per_expiry{};
  // Reuse raw European IV when the estimated OTM early-exercise premium is at
  // most this fraction of the NBBO spread. nullopt => preset default.
  std::optional<double> max_otm_shortcut_premium_spread_frac{};
  // Worker count for value_chain. 0 => std::thread::hardware_concurrency();
  // 1 => serial. A per-call `value_chain(..., n_threads)` overrides this.
  unsigned n_threads{0};
  // Worker count for a non-eSSVI fit's independent per-expiry preparation.
  // 0 => machine/env auto; 1 => serial. Distinct from value-chain evaluation
  // threads so an outer board scheduler can suppress nested fan-out.
  unsigned fit_workers{0};
  // Publish a structured carry/de-Am/fit/audit/calendar wall-time breakdown in
  // the built session diagnostics. Disabled by default to avoid clock reads on
  // production fits.
  bool collect_stage_timings{false};
  // Extra discrete cash dividends the surface build should honour. Usually left
  // empty — the chain's `MarketEnv` supplies the dividend schedule; a non-empty
  // value here overrides the env's divs.
  std::vector<DividendEvent> cash_divs{};
  // V2 product policy — an EXPLICIT opt-in (MERGE, §9 routing seam). Naming
  // either field requests the v2 dual pipeline: a market-following mark plus an
  // independently admitted risk surface (`SurfaceOutputs::MarketMarkAndRisk`,
  // `FitQualityMode::Balanced` when only the other field is named; a legacy
  // `preset` still supplies the unnamed one through `map_legacy_fit_preset`).
  // Quality changes the work budget, never the mandatory admission floor.
  //
  // Leaving BOTH unset is the legacy request: ONE surface, `preset`-budgeted,
  // selector-routed and admitted by `admission` — the WP12 contract that the
  // default serves a MARK and that strict risk admission is the explicit opt-in
  // (`risk_admission_policy()`, or a v2 request naming a Risk output). A legacy
  // preset therefore never IMPLICITLY promotes a caller to a risk request.
  std::optional<FitQualityMode> quality_mode{};
  std::optional<SurfaceOutputs> outputs{};
  // Only consulted by a v2 request (above); a legacy request has no risk output
  // to admit or fall back from.
  RiskAdmission risk_admission{RiskAdmission::Required};
  SurfaceFallback fallback{SurfaceFallback::LastKnownGood};
};

enum class ExpiryBuildOutcome : std::uint8_t {
  Missing = 0,
  Fitted = 1,
  DuplicateMaturity = 2,
};

enum class SurfaceBuildStage : std::uint8_t {
  Selection = 0,
  InputValidation = 1,
  Build = 2,
  Admission = 3,
  Publication = 4,
};

struct ExpiryBuildReport {
  std::size_t expiry_index{0u};
  double maturity{0.0};
  ExpiryBuildOutcome outcome{ExpiryBuildOutcome::Missing};
  std::size_t n_used{0u};
  // Task 3 (mark-domain-robustness observability), ADDITIVE ONLY — never
  // widen `ExpiryBuildOutcome` itself, it has live consumers that switch over
  // its exact three values (e.g. spy_fit_rca.cpp). This is the FIT DRIVER's
  // own, finer-grained reason a chain never reached `expiries()`/`parity()`
  // (`ExpiryFitOutcome`: Fitted/FittedFallbackCurve/FittedLegacyPrep/
  // CarryFailed/PrepStarved/PrepFailed/FitFailed/Skipped/PrepUncovered
  // (Task 1: admitted rows fail k-coverage)/FitRefusedCalendar (Task 6: the
  // ConvexDense calendar-floor refusal behind an UNCOVERED prev, which used to
  // arrive here as an anonymous `FitFailed`), surface_parity.hpp), read off
  // `VolaSession::expiry_fit_reports()` and
  // indexed by chain position -- see `completed_attempt_report`
  // (pricer_fitter.cpp) for where this is populated. Meaningful only when
  // `outcome == Missing`; left at its default (`Fitted`, the enum's own 0
  // value) everywhere else, including on a `Fitted`/`DuplicateMaturity`
  // `ExpiryBuildReport` and on any report built OUTSIDE
  // `completed_attempt_report` (`failed_attempt_report`,
  // `duplicate_maturity_report`) where no session -- and therefore no rich
  // report -- exists at all.
  ExpiryFitOutcome fit_outcome{ExpiryFitOutcome::Fitted};
};

struct SurfaceBuildAttemptReport {
  CurveConfig curve{};
  SurfaceBuildStage stage{SurfaceBuildStage::Build};
  bool build_succeeded{false};
  SurfaceAdmissionEvidence evidence{};
  SurfaceAdmissionDecision admission{};
  std::vector<ExpiryBuildReport> expiries{};
  std::optional<atx::core::Error> failure{};
};

// ── T7a: the substitution seam's missing comparison ─────────────────────────
//
// When independent risk admission rejects a BUILT primary and the
// validation-rejection ladder (or the strict convex-dense recovery rung)
// adopts a substitute, both fitted records are live at the adoption site — and
// until this record existed, one was served without the two ever being
// compared. Downstream admission scores boards on worst_frac_within_bidask, a
// MIN over each candidate's OWN fitted slices, and a min is gameable by
// fitting less: drop the hard slices and the worst improves (measured: on the
// sp100 corpus, every substituted board whose worst improved also lost quote
// coverage, 20/20). This record scores both candidates on the SAME quote
// population so that claim is measurable per board.
//
// POPULATION (common support): every well-formed two-sided quote — finite
// strike/bid/ask, bid > 0, ask > 0, ask >= bid, the band-shape screen of
// parity.cpp's `quote_scorable` with no model/vol terms — on the slices the
// PRIMARY fitted. It is the primary's support at slice granularity, defined
// from the CHAIN rather than either session's prepared observations, so
// neither candidate's own preparation can shape the population it is judged
// on. A candidate that cannot price a population quote (e.g. its build dropped
// the slice; the served surface would still be queried there through
// interpolation) scores NOT within band: an unpriceable serve is a failure,
// not an exclusion. In-band uses the parity harness's inclusive predicate
// (bid <= fair_value <= ask) via the session's own serving API `fair_value` —
// the same artifact fit() goes on to publish, not a re-derivation.
struct FallbackComparisonSlice {
  double maturity{0.0};
  std::size_t n_scored{0u};          // common-support quotes on this slice
  std::size_t primary_within{0u};    // primary's in-band count on them
  std::size_t substitute_within{0u}; // substitute's in-band count on the SAME quotes
  std::size_t primary_obs{0u};       // primary's own fit support (SliceContext::n_used)
  std::size_t substitute_obs{0u};    // substitute's; 0 == it dropped this slice
};

enum class FallbackAdoptionPath : std::uint8_t {
  ValidationLadder = 0, // family-substitution rung (fallback_curve_rungs)
  StrictRecovery = 1,   // strict convex-dense refit rung
};

struct FallbackComparisonRecord {
  FallbackAdoptionPath path{FallbackAdoptionPath::ValidationLadder};
  CurveConfig primary_curve{};    // the oracle-rejected candidate
  CurveConfig substitute_curve{}; // the admitted, adopted candidate
  // The oracle verdict bits that killed the primary (its ValidationDigest
  // failure mask at rejection; InvalidDomain carries a folded policy refusal)
  // and the publish-gate verdict paired with it (I2).
  ValidationFailure primary_failures{ValidationFailure::None};
  SurfaceAdmissionDecision primary_admission{};
  // Which record fit() served. Today adoption always serves the substitute;
  // the field exists so any future policy at this seam (demotion, refusal)
  // remains distinguishable in persisted provenance.
  bool substitute_served{true};
  // Board-level tallies over the common support (sums of the slice rows).
  std::size_t n_scored{0u};
  std::size_t primary_within{0u};
  std::size_t substitute_within{0u};
  // The gameable statistic, recomputed honestly: min over scored slices of
  // within/n_scored — BOTH candidates on the SAME denominator.
  double primary_worst_slice_frac{0.0};
  double substitute_worst_slice_frac{0.0};
  // Own-support obs totals (support shrinkage is visible here and per slice).
  std::size_t primary_obs_total{0u};
  std::size_t substitute_obs_total{0u};
  std::vector<FallbackComparisonSlice> slices{};
};

// Complete primary + fallback history for one `fit` call. A report moves to
// `published_report()` atomically with the admitted surface. Failed calls are
// visible only through `last_attempt_report()` and cannot mutate published state.
struct SurfaceBuildReport {
  std::vector<SurfaceBuildAttemptReport> attempts{};
  CurveConfig primary_curve{};
  CurveConfig published_curve{};
  bool used_fallback{false};
  bool published{false};
  bool retained_last_known_good{false};
  std::optional<ExpiryId> refit_expiry{};
  std::optional<std::uint64_t> source_quote_revision{};
  bool warm_started{false};
  // T7a. Present IFF this fit adopted a substitute after independent risk
  // admission rejected a BUILT primary (see FallbackComparisonRecord above).
  // Absent means no such substitution happened — never a zeroed record, so
  // absence of a comparison is structurally distinguishable from a comparison
  // that scored zero (W3-A).
  std::optional<FallbackComparisonRecord> validation_fallback_comparison{};
};

struct FitSnapshotProvenance {
  std::uint64_t chain_instance_id{0u};
  std::uint64_t board_revision{0u};
  Uid uid{kInvalidUid};
  std::vector<std::uint64_t> expiry_revisions{};
};

struct ExpiryRefitDiagnostics {
  ExpiryId expiry_id{0u};
  std::uint64_t source_quote_revision{0u};
  VolCurveKind curve_kind{VolCurveKind::Essvi};
  bool warm_started{false};
  std::size_t n_used{0u};
  std::size_t n_dropped{0u};
  std::optional<FitDiag> parametric_fit{};
  SurfaceAdmissionDecision admission{};
};

// ── Fitted surface handle (the owned fit output) ────────────────────────────
//
// Wraps a `VolaSession` (the fitted VolSurface + per-slice carry + correction
// caches). Every query is a const read; move-only (heavy fitted state).
class FittedSurface {
public:
  FittedSurface(FittedSurface &&) noexcept = default;
  FittedSurface &operator=(FittedSurface &&) noexcept = default;
  FittedSurface(const FittedSurface &) = delete;
  FittedSurface &operator=(const FittedSurface &) = delete;

  [[nodiscard]] double iv(double K, double T) const { return sess_.iv(K, T); }
  [[nodiscard]] Result<double> fair_value(double K, double T, Side s) const {
    return sess_.fair_value(K, T, s);
  }
  [[nodiscard]] Result<AmericanGreeks> greeks(double K, double T, Side s) const {
    return sess_.greeks(K, T, s);
  }
  [[nodiscard]] const VolaSession &session() const noexcept { return sess_; }
  [[nodiscard]] const SessionDiagnostics &diagnostics() const noexcept {
    return sess_.diagnostics();
  }
  [[nodiscard]] SurfacePurpose purpose() const noexcept { return purpose_; }
  [[nodiscard]] FitQualityMode quality_mode() const noexcept { return quality_mode_; }
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }

private:
  friend class PricerFitter;
  explicit FittedSurface(VolaSession &&sess, SurfacePurpose purpose, FitQualityMode quality_mode,
                         std::uint64_t generation)
      : sess_(std::move(sess)), purpose_(purpose), quality_mode_(quality_mode),
        generation_(generation) {}
  VolaSession sess_;
  SurfacePurpose purpose_{SurfacePurpose::Risk};
  FitQualityMode quality_mode_{FitQualityMode::Balanced};
  std::uint64_t generation_{};
};

// One immutable publication snapshot. Shared const generation leases keep both
// surfaces alive across subsequent fitter publications, so a retained bundle
// can never dangle while readers finish pricing the old generation.
struct FitPhaseTimings {
  double market_mark_build_ms{};
  double risk_build_ms{};
  double risk_validation_ms{};
  double total_ms{};
  double incremental_input_ms{};
  double incremental_refit_ms{};
  double incremental_validation_ms{};
  double incremental_publish_ms{};
  double incremental_total_ms{};
};

struct SurfaceBundle {
  std::shared_ptr<const FittedSurface> market_mark{};
  std::shared_ptr<const FittedSurface> risk{};
  SurfaceHealth market_mark_health{.purpose = SurfacePurpose::MarketMark};
  SurfaceHealth risk_health{};
  FitPhaseTimings timings{};
  std::uint64_t candidate_generation{};

  [[nodiscard]] bool has(SurfacePurpose purpose) const noexcept {
    return purpose == SurfacePurpose::MarketMark ? market_mark != nullptr : risk != nullptr;
  }
};

// ── PricerFitter ────────────────────────────────────────────────────────────
class PricerFitter {
public:
  explicit PricerFitter(PricerConfig cfg = {}) : cfg_(std::move(cfg)) {}

  // Fit the surface from `chain` and STORE it as unique_ptr<FittedSurface>,
  // replacing any prior fit. Maps the config onto SessionInputs and drives
  // `VolaSession::build`. Propagates the build error (the prior surface is left
  // intact on failure).
  //
  // `session_overlay`, when set, is invoked on the fully-resolved
  // `SessionInputs` immediately before the (first) build call — e.g. a caller
  // layering a per-symbol override (`apply_symbol_config`) onto the EXACT
  // inputs this fit uses, without duplicating the preset/curve/profile
  // resolution above. Applied once; a fallback-ladder retry (triggered only
  // for an auto-routed, unpinned curve) does not re-invoke it. A caller that
  // wants its overlaid curve pin immune to the fallback ladder must also set
  // `PricerConfig::curve` so the ladder's own auto-routed guard sees it.
  [[nodiscard]] Status fit(const OptionChain &chain,
                           const std::function<void(SessionInputs &)> &session_overlay = {});

  // Refit one expiry from the chain's current quotes. The first safe tranche is
  // exact eSSVI only: preparation, parity refresh, full admission, and
  // publication operate on a private clone. Any failure retains the last-known-
  // good publication and updates only last_attempt_report().
  [[nodiscard]] Result<ExpiryRefitDiagnostics> refit_expiry(const OptionChain &chain,
                                                            ExpiryId expiry_id);

  // Copy-on-write update of one fitted risk expiry. The updated chain is used
  // to rebuild and certify that expiry's European observations. A carry move
  // requires a full fit; otherwise only the local slice and its adjacent
  // calendar pairs are refit before an independently validated generation is
  // atomically published. The prior generation remains served on every error.
  [[nodiscard]] Result<FitDiag> refit_risk_slice(const OptionChain &chain, std::size_t slice_idx);

  // True iff the config's default-purpose surface is served (see surface()).
  [[nodiscard]] bool fitted() const noexcept;
  // Compatibility accessor, fail-closed on purpose: when the active config
  // requests a Risk output, only the admitted risk surface answers (nullptr
  // while risk is rejected/unserved — the LinearVariance market mark is never
  // silently substituted for it). A mark-only request (explicit outputs or the
  // legacy HFT mapping) receives its market surface. Purpose-specific state is
  // always available via risk_surface() / market_mark_surface().
  //
  // BORROW, AND NOT A SHARE. These three return the RAW pointer out of a
  // `std::shared_ptr<const FittedSurface>` the fitter holds; the fitter (via that
  // shared_ptr) is the owner and the pointer carries no ownership of its own. It
  // is valid only while the fitter still serves that generation: `fit` and
  // `refit_risk_slice` atomically REPUBLISH, rebinding the shared_ptr, and once
  // the last share drops the surface is destroyed under any raw pointer still
  // held. A caller that must keep a surface alive across a refit — or hand one to
  // another thread — takes `bundle()` instead, which copies the shared_ptrs and
  // pins that exact generation. THREADING follows the header's contract above:
  // `fit`/`refit_risk_slice` are writers needing exclusive access, so read these
  // only when no publish can be racing; the pointed-to `FittedSurface` is itself
  // immutable once published and safe for concurrent readers. Destroying the
  // fitter invalidates every raw pointer it handed out.
  [[nodiscard]] const FittedSurface *surface() const noexcept;
  [[nodiscard]] const FittedSurface *risk_surface() const noexcept { return risk_surface_.get(); }
  [[nodiscard]] const FittedSurface *market_mark_surface() const noexcept {
    return market_mark_surface_.get();
  }
  [[nodiscard]] SurfaceBundle bundle() const noexcept {
    return SurfaceBundle{market_mark_surface_, risk_surface_, market_mark_health_,
                         risk_health_,         timings_,      candidate_generation_};
  }

  [[nodiscard]] const PricerConfig &config() const noexcept { return cfg_; }
  void set_threads(unsigned n) noexcept { cfg_.n_threads = n; }

  // The curve-selection outcome from the most recent `fit`, when the config left
  // `curve` unset (auto-select). Empty if `curve` was pinned or no fit has run.
  // Lets a caller see WHICH curve the library chose for this board (and the
  // per-candidate out-of-sample scores).
  [[nodiscard]] const std::optional<SelectorResult> &selection() const noexcept {
    return served_selection_;
  }
  [[nodiscard]] const std::optional<SelectorResult> &candidate_selection() const noexcept {
    return selection_;
  }

  // Profile/features/effective preset+curve decision from the most recent auto
  // fit. Unlike selection(), this is populated for the O(N) direct routes too.
  [[nodiscard]] const std::optional<FitDecision> &decision() const noexcept {
    return served_decision_;
  }
  [[nodiscard]] const std::optional<FitDecision> &candidate_decision() const noexcept {
    return decision_;
  }

  [[nodiscard]] const std::optional<SurfaceBuildReport> &published_report() const noexcept {
    return published_report_;
  }

  [[nodiscard]] const std::optional<SurfaceBuildReport> &last_attempt_report() const noexcept {
    return last_attempt_report_;
  }

  // Provenance follows the same fail-closed default-purpose routing as
  // surface(). The purpose-specific overload is required for a dual request:
  // mark and risk may retain different last-known-good generations.
  [[nodiscard]] const std::optional<FitSnapshotProvenance> &published_provenance() const noexcept;
  [[nodiscard]] const std::optional<FitSnapshotProvenance> &
  published_provenance(SurfacePurpose purpose) const noexcept;

  // Price the chain's options for the requested `fields`, fanned out across
  // `n_threads` workers (0 => cfg.n_threads; final 0 => hardware_concurrency,
  // 1 => serial). DETERMINISTIC: the result is bit-identical for any thread
  // count (disjoint output slots, pure const reads).
  //
  // The purpose-less overload prices the config's default purpose FAIL-CLOSED:
  // a config that requested a Risk output is answered by the admitted risk
  // surface or with Unavailable — never by the market mark. Pass
  // SurfacePurpose::MarketMark explicitly to price the mark interpolant.
  //
  // @return Unavailable if the requested surface is unserved; InvalidArgument
  //         if `chain` differs from the fitted chain instance/uid; otherwise
  //         Ok(valuation).
  [[nodiscard]] Result<ChainValuation> value_chain(const OptionChain &chain, OutputField fields,
                                                   unsigned n_threads = 0) const;
  [[nodiscard]] Result<ChainValuation> value_chain(const OptionChain &chain, OutputField fields,
                                                   SurfacePurpose purpose,
                                                   unsigned n_threads = 0) const;

  // Price only `selected_ids`, preserving caller order and duplicates. Work
  // and output allocation are proportional to the selection; this is the
  // quote-update path for dirty options and does not snapshot or scan the full
  // board. Field and surface-purpose semantics are identical to the full-chain
  // overloads above.
  //
  // @return NotFound if any selected id is foreign or unknown; Unavailable if
  //         the requested surface is unserved; InvalidArgument if `chain`
  //         differs from the fitted chain instance/uid; otherwise Ok(valuation).
  [[nodiscard]] Result<ChainValuation> value_chain(const OptionChain &chain,
                                                   std::span<const OptionId> selected_ids,
                                                   OutputField fields,
                                                   unsigned n_threads = 0) const;
  [[nodiscard]] Result<ChainValuation> value_chain(const OptionChain &chain,
                                                   std::span<const OptionId> selected_ids,
                                                   OutputField fields, SurfacePurpose purpose,
                                                   unsigned n_threads = 0) const;

private:
  // Effective §9 request after the one-release legacy-preset mapping. Computed
  // in one place so fit(), the default-purpose accessors, and value_chain can
  // never disagree about whether the config requested a risk output.
  struct EffectiveRequest {
    SurfaceOutputs outputs{SurfaceOutputs::MarketMarkAndRisk};
    FitQualityMode quality_mode{FitQualityMode::Balanced};
  };
  [[nodiscard]] Result<ChainValuation> value_snapshot(const OptionChain &chain,
                                                      ChainSnapshot snapshot, OutputField fields,
                                                      SurfacePurpose purpose,
                                                      unsigned n_threads) const;
  [[nodiscard]] EffectiveRequest effective_request() const noexcept;
  // True iff the caller explicitly opted into the v2 dual mark/risk API (a
  // non-default quality_mode or outputs). A legacy request (default v2 fields)
  // runs main's single-surface transactional fit; a v2 request runs the branch
  // dual pipeline. Used by fit(), effective_request(), and refit_expiry().
  [[nodiscard]] bool is_v2_request() const noexcept;

  PricerConfig cfg_;
  std::shared_ptr<const FittedSurface> market_mark_surface_;
  std::shared_ptr<const FittedSurface> risk_surface_;
  SurfaceHealth market_mark_health_{.purpose = SurfacePurpose::MarketMark};
  SurfaceHealth risk_health_{};
  FitPhaseTimings timings_{};
  std::uint64_t candidate_generation_{};
  std::optional<SelectorResult> selection_;        // last auto-select outcome (if any)
  std::optional<SelectorResult> served_selection_; // selector that produced served risk
  std::optional<FitDecision> decision_;            // last unified policy outcome
  std::optional<SurfaceBuildReport> published_report_;
  std::optional<SurfaceBuildReport> last_attempt_report_;
  // A dual fit publishes and retains each purpose independently. Keeping one
  // provenance record would either make a newly published mark unpriceable or
  // misattribute a retained risk surface to the mark's newer chain snapshot.
  std::optional<FitSnapshotProvenance> market_mark_provenance_;
  std::optional<FitSnapshotProvenance> risk_provenance_;
  std::optional<FitDecision> served_decision_; // policy that produced served risk generation
};

} // namespace atx::vol
