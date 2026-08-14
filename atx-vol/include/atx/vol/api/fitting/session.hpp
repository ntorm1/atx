#pragma once

// VolaSession — a stateful, composable surface HANDLE for the American-equity
// options pipeline (a la Vola Dynamics' surface handle).
//
// `run_surface_parity` (surface_parity.hpp) is a one-shot: it de-Americanizes
// and fits every expiry, assembles an ascending-T eSSVI `VolSurface`, and scores
// re-Americanized parity. It is the right primitive but the wrong *shape* for a
// caller that wants to build once from a market snapshot and then repeatedly ask
// "what is the implied vol / fair value / Greeks at an arbitrary (K, T)?".
//
// VolaSession wraps that one-shot into a handle: `build` runs the parity harness,
// KEEPS the fitted surface and the per-slice re-pricing context, and remembers
// the pricing inputs (S, r, pricer method, AL opts). The const query methods then
// answer (K, T) cheaply with NO refit — interpolating the forward/carry in T,
// reading the surface's own linear-in-total-variance vol, and re-Americanizing on
// the interpolated carry.
//
// ## Forward interpolation (the load-bearing query mechanic)
//
// The surface stores each slice in LOG-MONEYNESS k = ln(K / F_slice), so a query
// at an absolute strike K must use the forward AT the queried T. `build` records
// each fitted slice's (T, forward, q_eff) in ascending T; a query at T locates T
// among those slice T's and geometrically interpolates market-implied `forward`.
// Interior q_eff is derived from that forward and interpolated discount state,
// preserving S*e^{(r-q_eff)T} == F. Exact pillars retain calibrated carry. In
// either tail, endpoint q_eff/r are held flat and F is derived at the query T.
//
// ## Ownership / move semantics
//
// The fitted `VolSurface` is held by value; VolaSession is therefore MOVE-ONLY
// (moves defaulted, copy implicitly deleted — a session is a heavy fitted object,
// not a value to duplicate). Rule of Zero otherwise: every member is a RAII value.
//
// ## Thread-safety
//
// `build`/`from_frame` are the only mutating entries and each returns a fresh,
// fully-constructed session. After construction every query method is a const,
// allocation-free read of immutable state — safe to call concurrently on one
// session from any number of threads (matching the underlying `VolSurface` and
// stateless-pricer contracts).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"        // AmericanGreeks (query return)
#include "atx/vol/api/fitting/calib.hpp"           // CalibOpts
#include "atx/vol/api/fitting/correction.hpp"      // CorrectionCache, AmericanCorrectionCaches
#include "atx/vol/api/marketdata/data.hpp"            // QuoteFrame (from_frame input)
#include "atx/vol/api/fitting/deamer.hpp"          // DeAmOptions
#include "atx/vol/api/fitting/aggregate_arity.hpp" // SessionInputs field-count drift pin
#include "atx/vol/api/analytics/event_vol.hpp"       // EventSchedule (SessionInputs::events), implied_emove
#include "atx/vol/api/fitting/parity.hpp"          // ParityReport
#include "atx/vol/api/fitting/prepared_policy.hpp" // PreparedObservationPolicy
#include "atx/vol/api/backtest/priced_surface.hpp"  // PricedSurface, PricingContext (to_priced_surface)
#include "atx/vol/api/fitting/projection.hpp"      // InterpMode (SessionInputs::interp, ShapeBlend eval)
#include "atx/vol/api/backtest/query_pricing.hpp"   // QueryPricingTier
#include "atx/vol/api/pricing/rates_curve.hpp"     // DividendEvent
#include "atx/vol/api/fitting/surface_parity.hpp"  // SliceContext, run_surface_parity
#include "atx/vol/api/core/types.hpp"           // Result, Side
#include "atx/vol/api/marketdata/universe.hpp"        // Underlying (build input)
#include "atx/vol/api/fitting/vol_curve.hpp"       // CurveConfig, CurveSurface, VolCurveKind
#include "atx/vol/api/fitting/vol_surface.hpp"     // VolSurface
#include "atx/vol/api/core/vol_time.hpp"        // TimeSpec (SessionInputs::time)

namespace atx::vol {

struct CanonicalPreparedExpiry;
class PricerFitter;

// Market/pricing snapshot a session is built from. Maps 1:1 onto
// `SurfaceParityInputs` when driving `run_surface_parity`; the same fields are
// retained so the const queries can re-price off the fitted surface.
//
// CONSTRUCTION CONTRACT (v1, plan item 4.2) — DESIGNATED INITIALIZERS ONLY.
// Build one with `make_session_inputs` / `apply_fit_preset` or set fields by
// name; never `SessionInputs{a, b, c, ...}`. `curve_pinned` used to carry an
// "appended for positional aggregate source compatibility" note, meaning it was
// parked at the very end — two screens away from the `curve` field it is a
// qualifier ON — solely so positional initializers kept compiling. It now sits
// directly beneath `curve`, and the field-count `static_assert` below makes a
// silent re-append a compile error.
//
// session.hpp is Tier-A (frozen v1 surface): this reorder is the LAST layout
// change allowed here. After v1 the order is fixed and new knobs append at the
// end WITHOUT any positional-compatibility promise.
struct SessionInputs {
  double S{0.0}; // spot (> 0); the OpraPanel implied_spot when built from a frame
  double r{0.0}; // continuously-compounded rate (finite)
  // The three vectors carry an explicit `{}` so that EVERY member has a default
  // member initializer: without one, naming any later field in a designated
  // initializer trips -Wmissing-field-initializers, and this struct's contract is
  // designated init. Value-identical to the implicit default construction.
  std::vector<double> expiry_rate_T{};    // empty => legacy scalar r
  std::vector<double> expiry_rates{};     // aligned with expiry_rate_T
  std::vector<DividendEvent> cash_divs{}; // discrete cash-dividend schedule
  std::int64_t now_ts_ns{0};              // valuation timestamp (epoch ns)
  DeAmOptions deam{};                   // borrow-implication + pricer method / AL opts
  CalibOpts calib{};                    // per-slice curve-fit policy
  // Curve family to fit. Essvi (default) is byte-identical to the historical
  // eSSVI path (run_surface_parity, with calendar repair). ConvexDense / Svi fit
  // through the curve-agnostic driver (fit_curve_surface) and are served via the
  // session's polymorphic-surface override — this is how PricerFitter reaches the
  // 99.5%-in-band convex dense fit. The convex knobs live in curve.convex, the
  // eSSVI/SVI knobs in curve.parametric (calib mirrors curve.parametric for the
  // default path).
  CurveConfig curve{VolCurveKind::Essvi};
  // Qualifies `curve` above: true only when the caller selected `curve`
  // explicitly and the fitter will not substitute another family. A pinned
  // non-eSSVI LegacyCompatible/Cold surface may omit a correction cache when
  // neither fit nor queries can read it; an auto-routed surface retains the cache
  // because its fallback ladder can still publish eSSVI. Runtime policy only; not
  // archived.
  bool curve_pinned{false};
  double band_k{1.0}; // minimum-edge band multiplier (parity)
  // Build a per-side Chebyshev correction cache over the chain's (k, T, sigma)
  // box and route every American inversion / re-pricing through the fast cached
  // pricer (Black-76 + correction). ON by default (the SOTA hot path); the
  // round-trip stays self-consistent because the same cache prices both legs.
  // Falls back to the cold Andersen-Lake path automatically if a cache fails to
  // build. A pinned polymorphic curve also elides construction when its fit and
  // query policies cannot consume the cache. Set false to force the reference
  // cold path (e.g. for a cold-vs-cached benchmark).
  bool use_correction_cache{true};
  // Explicit query-serving tier. LegacyCompatible preserves the historical
  // cached-eSSVI/cold-override behavior; ColdReference forces Andersen-Lake/FD;
  // RepresentativeFast uses one term-wide carry surrogate; CarryBank builds a
  // bounded post-fit fixed-carry bank and interpolates adjacent entries. The two
  // fast tiers are opt-in because their marks/Greeks can differ slightly from
  // the cold reference.
  QueryPricingTier query_pricing_tier{QueryPricingTier::LegacyCompatible};
  // ConvexDense/SVI cold-start controls. `score_parity=false` skips the redundant
  // diagnostic re-Americanization pass after fitting; the surface is unchanged,
  // but per-expiry parity diagnostics are intentionally zeroed. Disabling the
  // calendar floor fits slices independently, recovering the tightest SPY
  // bid/ask fit at the cost of cross-expiry no-arb enforcement.
  bool score_parity{true};
  bool enforce_calendar_floor{true};
  bool use_deam_cache_for_fit{false};
  // Observation-preparation policy for the polymorphic (ConvexDense / Svi /
  // SplineVol) fit path, mapped onto `SurfaceParityInputs::fit_prep_policy` in
  // `VolaSession::build`. Configured (default) is bit-identical to the historical
  // curve-driver preparation; LegacyEssviCompatibility uses the permissive eSSVI
  // cold-driver predicate, which keeps thin single-name expiries that the strict
  // usable-row floor would otherwise starve. Only affects a polymorphic-curve
  // build; the default eSSVI path (run_surface_parity) does not read it.
  //
  // LEGACY SPELLING (W2-B). This is the family-coupled half of what `prep` below
  // now expresses: it can only say "permissive on the polymorphic lane". It is
  // retained because live callers set it, and it is folded into the request as
  // `PrepStrictness::Permissive` whenever `prep.strictness` is left at `Auto`.
  // An explicit `prep.strictness` always wins. Prefer `prep` for new code.
  PreparedObservationPolicy fit_prep_policy{PreparedObservationPolicy::Configured};
  // Post-assembly calendar-arbitrage repair (see surface_parity.hpp
  // CalendarRepair). None (default) checks only — the raw independent-per-slice
  // surface may cross in the wings. Project makes the produced surface
  // calendar-arb-free (Vola's headline property): a backbone theta-bump on any
  // slice whose wing crosses, with the per-expiry parity then scored off the
  // repaired surface so the reported quality is what the surface serves.
  CalendarRepair calendar_repair{CalendarRepair::None};
  // Worker budget for the independent per-expiry preparation phase used by
  // non-eSSVI curve families. 0 => machine/env auto; 1 => serial. Orchestrators
  // that already parallelize across boards set this to 1 to avoid nested H^2
  // fan-out. The eSSVI path is sequential and ignores this field.
  unsigned fit_workers{0};
  // Opt-in structured fit-stage timing. False keeps the production hot path
  // free of steady-clock calls; true publishes the report in diagnostics().
  bool collect_stage_timings{false};
  // Cross-expiry interpolation mode for arbitrary-T queries served off this
  // session's surface. PiecewiseTotalVariance (default) is bit-identical to
  // current behavior; ShapeBlend is the FLEX-style vol-multiple blend. Only
  // applies to the default eSSVI surface (`curve` == Essvi, the shipped
  // inserted-slice path); a session built with a polymorphic curve override
  // (ConvexDense / Svi) always serves PiecewiseTotalVariance-equivalent
  // cross-expiry interpolation regardless of this field, since ShapeBlend is
  // specific to VolSurface's inserted-slice mechanism (see
  // InterpMode::ShapeBlend).
  InterpMode interp{InterpMode::PiecewiseTotalVariance};
  // Symbol earnings-event schedule (nullable; default null => bit-identical
  // serving, exactly like `interp` above). When non-null, `build` solves an
  // implied eMove from the two fitted expiries bracketing the next
  // scheduled event (SpiderRock event_vol.hpp `implied_emove`; see
  // `SessionDiagnostics::implied_emove` for the exact bracketing rule and
  // its NaN-on-failure convention) and, ONLY on a successful solve, threads
  // (events, the solved eMove) through every query's model_w/model_iv so
  // cross-expiry interpolation censors/re-adds the earnings-variance term
  // (`event_vol.hpp` `event_aware_w`, via `w_on_inserted_slice`'s
  // event-aware overload) instead of the plain blend. Applies under EITHER
  // `interp` value (see `w_on_inserted_slice`'s doc for what gets
  // event-censored in each mode). A failed solve (`implied_emove` left NaN)
  // serves EXACTLY as if `events` were null -- no error, no fabricated
  // event contribution. Restricted to the default eSSVI path, same as
  // ShapeBlend above: a session built with a polymorphic curve override
  // (ConvexDense / Svi) leaves `implied_emove` at its NaN default and never
  // consults `events`. NOT restricted by `time.convention`: the solve reads
  // each fitted slice's stamped `EssviParams::expiry_ns` (the real listed
  // expiry, a plain UTC instant `run_surface_parity` copies from
  // `Chain::expiry_ns` -- independent of T convention) rather than deriving
  // an instant from T, so it runs identically under Calendar365 and VolTime
  // -- see `time`'s doc below.
  std::shared_ptr<const EventSchedule> events{};
  // The session's retained copy of the T convention its chains were built
  // under (see vol_time.hpp `TimeSpec`; default Calendar365 is BIT-IDENTICAL
  // to the historical `year_fraction`-derived `Chain::T`). Chain::T itself
  // always comes from the FRAME's own convention (`QuoteFrame::time`, read by
  // `data_install`); this field is the session-side mirror, kept verbatim on
  // the built session (`inputs().time`) as the single stored source of truth
  // for any T-derivation performed after the fit. `from_frame` ENFORCES the
  // two agree — it returns InvalidArgument on `in.time != frame.time` rather
  // than build a mixed-convention session (copy the frame's TimeSpec, e.g.
  // `OpraPanel::time`, into this field). `build(under, in)` installs nothing
  // itself (`under` is pre-installed), so there the match with whatever frame
  // produced `under` remains a documented caller contract. NOTE: events-
  // censoring (`events` above) is NOT restricted by this field -- the eMove
  // solve reads each fitted slice's stamped `EssviParams::expiry_ns`
  // (`run_surface_parity`'s copy of `Chain::expiry_ns`, a plain UTC instant)
  // instead of deriving one from T, so a `VolTime`-shaped T no longer risks
  // mis-bucketing a nearby event the way the historical `ns_from_year_
  // fraction` (Calendar365-inverse) synthesis could. That synthesis is
  // still used as a fallback for any slice that was never stamped
  // (`expiry_ns == 0`) -- see `solve_implied_emove`'s doc (session.cpp).
  TimeSpec time{};
  // W2-B: the EXPLICIT preparation-strictness + thin-slice-rescue decision (see
  // detail/prepared_policy.hpp). Every field defaults to `Auto`, which
  // `VolaSession::build` resolves against the fit LANE the curve family selects
  // — `PreparationLane::EssviDriver` for `curve.kind == Essvi`,
  // `PreparationLane::PolymorphicDriver` otherwise — via
  // `resolve_preparation_policy`. All-`Auto` reproduces the historical
  // preparation on both lanes EXCEPT for one deliberate change: the per-slice
  // Legacy-prep rescue (`SurfaceParityInputs::per_slice_legacy_prep_fallback`)
  // is now ON for the polymorphic lane, which is a pure recovery path — it can
  // only add slices whose strict preparation starved, never remove one.
  //
  // Appended last (post-v1); no positional-compatibility promise, per the
  // construction contract above.
  PreparationPolicyRequest prep{};
};

// Drift pin (plan item 4.2). SessionInputs has exactly TWENTY-FOUR fields.
// Adding, removing or splitting one breaks this line, which is the point: it
// forces whoever changes the struct to read the construction contract above
// instead of appending a knob "for compatibility" with positional initializers
// that are no longer part of the API.
static_assert(detail::aggregate_arity_is_v<SessionInputs, 24>,
              "SessionInputs field count changed: update this pin, and confirm every "
              "construction site still initializes by field name.");

// ── Named calibration presets ─────────────────────────────────────────────
//
// A single high-level choice that bundles the whole fit policy (Andersen-Lake
// preset + IV-inversion tolerance, borrow ATM-pair count, correction cache, and
// calendar-arb repair) into one name, so a caller need not hand-assemble the
// DeAmOptions / CalibOpts / cache / repair knobs and know which combination is
// coherent. `apply_fit_preset` sets policy fields, preserving the market
// snapshot (S, r, cash_divs, now_ts_ns) the caller has already filled.
enum class FitPreset : std::uint8_t {
  // Surface-fit hot path: fast Andersen-Lake preset (al_fast_opts), inversion
  // tol matched to its accuracy floor, one ATM borrow pair, correction cache on.
  // Cold whole-surface fit ~0.36 s. The session's historical auto-default.
  Fast = 0,
  // Reference fidelity: the ACCURATE Andersen-Lake preset (al_default_opts),
  // tight inversion tol, three ATM borrow pairs, cache on. Slower cold build.
  Accurate = 1,
  // Market-maker default: Fast + calendar-arb repair (MonotoneFit) — the surface
  // is calendar-arb-free near-money at held fit quality (see surface_parity.hpp
  // CalendarRepair). The recommended production preset for a quoting desk.
  Robust = 2,
  // Low-latency dense index cold-start: adaptive-knot linear total variance,
  // selective fast de-Am, no diagnostic parity pass, and independent slices.
  // Optimizes SPY-style penny-tight fit latency and in-band coverage.
  Hft = 3,
  // C3 (populate tier): the RIGHT-SIZED bulk-populate preset. Keeps Robust's
  // eSSVI FIT quality (MonotoneFit calendar repair, 3 ATM carry pairs, parity
  // scored) but drops the de-Americanization / correction-cache-sampling / baked
  // cold-mark Andersen-Lake preset from Robust's al_default_opts {12,24,8,1e-10}
  // (~200 us "pseudo-accurate", ~1e-5) to the fast preset al_fast_opts
  // {7,16,4,1e-8} (~47 us, ~1e-3) with the inversion tol matched to its accuracy
  // floor. Rationale (docs/al-preset-ladder.md sec 5-6, the K1 audit): inversion /
  // cache sampling only need ~1e-4 PRICE accuracy vs the ~1e-2 surface RMSE, and
  // the Robust default was never validated as the populate choice — it over-pays
  // 4-8x on every de-Am solve, cache sample, and cache-miss cold mark. Robust
  // stays the FINAL-FIT / CERTIFICATION / oracle preset; Populate is for the
  // populate / cache-sampling lane only. Economic parity vs Robust (surface RMSE,
  // arb, served-coverage) is gated on real OPRA boards (C3 characterization).
  Populate = 4,
  // Perf Phase 2b: the OPT-IN bulk-populate throughput tier. Identical to Populate
  // in every field that sets the fitted surface's QUALITY (Robust-grade eSSVI,
  // MonotoneFit calendar repair, 3 ATM carry pairs, parity scored, audited
  // inversions, correction-cache-served fit) and identical in what it BAKES into
  // the stored pricing config. It changes exactly one thing: the Andersen-Lake rung
  // the FIT's de-Americanization / IV inversion / cache sampling run on, from
  // al_fast_opts {7,16,4} to al_bulk_opts {7,8,2, price 32} — the ladder's `ql_fast`
  // (docs/al-preset-ladder.md §4-5, which names it for exactly this tier).
  //
  // WHY IT EXISTS. Phase-2b step-1 measurement (al_probe.hpp) on two 102-symbol
  // production dates: the AL boundary solve is ~84% of a `populate` board fit and
  // its Jacobi-Newton + fixed-point sweeps alone are ~71%, against ~6.7% for the
  // early-exercise premium quadrature. `ql_fast` cuts the sweep work per solve 4x
  // on the fit's own de-Am plane, which is where ~49% of the fit sits.
  //
  // NOT a default, and never the certification / oracle / serving preset: Populate
  // stays the reproducible production tier and Robust the oracle. Appended last so
  // every existing enumerator keeps its persisted value.
  Bulk = 5,
};

// Populate the fit-policy fields of `in` for `preset` (Andersen-Lake opts,
// iv_tol, n_atm, use_correction_cache, calendar_repair), leaving the market
// snapshot fields untouched. Idempotent; safe to call before `build`/`from_frame`.
void apply_fit_preset(SessionInputs &in, FitPreset preset) noexcept;

// Convenience: a SessionInputs preconfigured for `preset` with the market
// snapshot filled. Equivalent to setting the four snapshot fields then calling
// `apply_fit_preset`.
[[nodiscard]] SessionInputs make_session_inputs(FitPreset preset, double S, double r,
                                                std::int64_t now_ts_ns = 0);

enum class ParityDiagnosticState : std::uint8_t {
  NotScored = 0,
  Disabled = 1,
  Failed = 2,
  Valid = 3,
};

// Timing and outcome counters for the local one-expiry update path. Durations
// are wall-clock milliseconds from the most recent attempt.
struct IncrementalRefitDiagnostics {
  std::size_t attempts{0};
  std::size_t committed{0};
  std::size_t rolled_back{0};
  std::size_t last_slice_index{0};
  VolCurveKind last_kind{VolCurveKind::Essvi};
  std::size_t last_adjacent_pairs_checked{0};
  double last_fit_ms{0.0};
  double last_calendar_ms{0.0};
  double last_validation_ms{0.0};
  double last_total_ms{0.0};
  bool last_committed{false};
};

// Aggregate surface-quality summary, distilled from the per-expiry parity
// reports and the per-slice context at build time. `parity_state` distinguishes
// a genuine zero score from an opt-out or a scoring failure.
struct SessionDiagnostics {
  double worst_frac_within_bidask{0.0}; // min over expiries of frac in bid-ask
  double mean_frac_within_bidask{0.0};  // mean over expiries
  double mean_chi2_reduced{0.0};        // mean reduced chi-square (vol space)
  double mean_rmse_vol{0.0};            // mean RMSE(model vol - mkt vol)
  // T5 item 3 — the ABSOLUTE de-Am -> fit -> re-Americanize round trip, in vol
  // points, against the raw American mid (ParityReport::rmse_round_trip_vol).
  // `mean` is over scored expiries, `max` is the board's worst single quote.
  // Every other quality number on this struct is spread-normalised and so
  // cannot report a large error on a wide board; these two can, which is why
  // they are the ones to read on a thin board.
  double mean_round_trip_vol{0.0};
  double max_round_trip_vol{0.0};
  bool calendar_arb_free{false};        // surface calendar no-arb check
  std::size_t n_calendar_viol_pre{0};   // calendar violations BEFORE any repair;
                                        // on a FAILED check, stamped with sentinel
                                        // 1 (calendar_arb_free=false) — nonzero
                                        // means "found violations OR check failed"
  std::size_t n_slices{0};              // fitted slice count
  std::size_t n_quotes{0};              // sum of per-slice n_used
  ParityDiagnosticState parity_state{ParityDiagnosticState::NotScored};
  // T4 escalation (T10c): banded parity-evidence counters, rolled up from each
  // expiry's ParityReport (n_parity_scored = Σ n, n_parity_in_band = Σ
  // n_within, n_parity_out_of_band = their difference). The averaging above
  // cannot tell ABSENCE OF EVIDENCE from EVIDENCE OF ALL-OUT: a
  // default-constructed ParityReport (n == 0) enters mean/worst as
  // frac_fv_within_bidask == 0 while parity_state can stay Valid, reading
  // exactly like a surface that reprices nothing in band. With the counters,
  // n_parity_scored == 0 says "nothing was measured" and n_parity_scored > 0
  // with n_parity_in_band == 0 says "everything measured missed" — structurally
  // different states. Additive observability only: no admission, score, or
  // chosen_kind reads them.
  std::size_t n_parity_scored{0};
  std::size_t n_parity_in_band{0};
  std::size_t n_parity_out_of_band{0};
  // SpiderRock-style band-violation stats, rolled up from each expiry's
  // ParityReport::band (record-only; not used to gate slice selection).
  std::size_t n_bid_miss{};             // sum over slices
  std::size_t n_ask_miss{};             // sum over slices
  double max_prc_err{};                 // max over slices (premium units)
  SurfaceFitStageTimings fit_timings{}; // zero/uncollected unless explicitly requested
  // eMove implied from the two fitted expiries bracketing the first
  // scheduled event in `(now_ts_ns, last fitted expiry]` (SessionInputs::
  // events), via `implied_emove` on those two expiries' own ATM total
  // variances (event_vol.hpp). NaN (never a fabricated 0 -- mirrors the
  // conservative calendar-guard convention just above, `n_calendar_viol_pre`)
  // when: `events` is null; the session used a polymorphic curve override
  // (ConvexDense / Svi -- events is eSSVI-default only, see
  // SessionInputs::events); there are fewer than two fitted expiries; no
  // scheduled event falls in that window; the event has no fitted expiry
  // strictly before it to bracket against; or `implied_emove`'s own solve
  // failed (no identification, or a negative-beyond-tolerance e^2). A
  // finite value here is the ONLY condition under which queries route
  // through the event-aware blend (see SessionInputs::events).
  double implied_emove{std::numeric_limits<double>::quiet_NaN()};
  // E3b / AN-P1-3. WHICH solve produced `implied_emove`: `Joint` when the
  // identified {eMove, st, lt, decay} fit over ALL fitted slices ran
  // (`implied_emove_joint`, event_vol.hpp), `TwoPillar` when the slice set did
  // not support it and the pre-E3b bracketing solve was used instead. Only
  // meaningful when `implied_emove` is finite; on a declined solve it keeps its
  // `TwoPillar` default and means nothing.
  EmoveMethod emove_method{EmoveMethod::TwoPillar};
  // FIX-E I-2. The joint fit's OUTCOME CODE. Without it the status channel at
  // the session boundary is incomplete: `emove_method` alone cannot distinguish
  // a converged joint answer from a fallback, nor say WHY the fallback
  // happened. Read it together with `emove_method`:
  //   * `Joint` + `Ok`/`Minimum`    — the identified joint fit converged;
  //   * `TwoPillar` + `Ok`          — the joint fit was never attempted (the
  //                                   slice set does not identify it);
  //   * `TwoPillar` + anything else — the joint fit RAN and was REJECTED, and
  //                                   this names the failure (`MaxSteps` /
  //                                   `LeftBound` / `RightBound` /
  //                                   `CenterFlat` / `Degenerate`).
  // Only meaningful when `implied_emove` is finite.
  EmoveFitCode emove_fit_code{EmoveFitCode::Ok};
  std::size_t n_carry_slices{0};    // slices with a resolved carry diagnostic
  std::size_t n_carry_confident{0}; // carry slices clearing confidence gates
  // Expiries the fit DROPPED because carry could not be resolved (confidence
  // gate / no quotable pair / degenerate forward). A surface missing expiries
  // must surface the gap (§5.2); risk admission maps a non-zero count to a
  // Degraded health state with a CarryGap reason.
  std::size_t n_carry_skipped_expiries{0};
  // Decision B: expiries ADMITTED with a carry DERIVED from the board's
  // borrow-vs-T term structure (interpolated / flat-extended from the confident
  // expiries) rather than solved from their own co-terminal pairs. A fallback
  // carry is honest — it does NOT count toward n_carry_confident — so it is
  // surfaced through the same publish-with-Degraded CarryGap reason as a skipped
  // expiry, NOT the hard InsufficientData reject (the whole point of Decision B
  // is to keep the term structure, published Degraded, rather than drop it).
  std::size_t n_carry_fallback_expiries{0};
  // Expiries the fit DROPPED because the fit-inversion audit starved the
  // slice below the usable-observation floor (eSSVI aligned-obs path under
  // deam.audit_fit_inversions). The audit-created analogue of a carry skip;
  // risk admission surfaces it through the same CarryGap reason.
  std::size_t n_audit_starved_expiries{0};
  // T6 (§5.2). Expiries the fit dropped because PREPARATION starved the slice
  // below the usable-row floor even after every armed rescue
  // (`ExpiryFitOutcome::PrepStarved`).
  //
  // This is the largest of the expiry-loss classes and it was the only one with
  // no route to admission. Measured over 2,707 chain outcomes on the whole
  // lqbench corpus, 30.2% were starved against 4.7% carry-failed, and on a
  // 40-board SERVING sample the carry failures were exactly zero. So a board
  // could lose a third of its expiries to thin preparation and still publish
  // `Healthy`, because the two gaps that DID reach the digest — the carry gate
  // and the fit-inversion audit — had not fired. That is precisely the hidden-
  // gap class `CarryGap` exists to close, one field over.
  //
  // It merges onto the SAME `CarryGap` bit rather than a new one, for the two
  // reasons that bit already documents (`pricer_fitter.cpp`): the fact is
  // identical in kind — the served surface is missing expiries the board has,
  // while the surviving slices passed the full geometric contract — and a new
  // bit would invalidate every persisted Degraded+CarryGap provenance in the
  // archive and the surface DB. The established semantics therefore apply
  // unchanged: alone it publishes Degraded and still serves; combined with any
  // other failure it rejects. Measured to add ZERO rejections on lqbench
  // 2026-08-03, where the served reason mask takes exactly two values —
  // CarryGap on 188 boards and empty on 15 — so no board can acquire the
  // "CarryGap plus something else" combination from this change.
  std::size_t n_prep_starved_expiries{0};
  // T6d. Expiries the fit dropped because the admitted rows fail Task 1's
  // k-coverage predicate (`ExpiryFitOutcome::PrepUncovered`) -- enough rows to
  // clear the count floor, but all on one wing or with a central hole, so a
  // fit would serve the missing region extrapolated. The same hidden-gap class
  // as `n_prep_starved_expiries` one comment up: the served surface is missing
  // expiries the board has while every surviving slice passed the full
  // geometric contract. It therefore merges onto the SAME CarryGap bit, for
  // the same two reasons (identical in kind; a new bit would invalidate every
  // persisted Degraded+CarryGap provenance). Produced by the ConvexDense
  // driver only -- every other family's admission has no coverage refusal, so
  // this stays 0 there and the merge term is inert.
  std::size_t n_prep_uncovered_expiries{0};
  // ConvexDense-served call-price bound self-check violations (oracle finding
  // I-2): the independent risk-surface oracle only reconstructs prices from
  // w=sigma^2*T via Black, which is always in-bounds by construction and
  // cannot see a served call_price() the fit clamped into range before
  // forming w. This is `arb_check_price_bounds` run over the session's own
  // served CurveSurface (0 for a non-ConvexDense session). Like a carry gap,
  // this is the one fitter self-report the geometric oracle trusts, and only
  // to ADD a ValidationFailure::PriceBounds failure — never to clear one
  // (merge_session_failure_context, pricer_fitter.cpp).
  std::size_t n_price_bound_violations{0};
  double min_carry_effective_pairs{0.0};
  double max_carry_dispersion{0.0};
  double max_carry_leave_one_out{0.0};
  std::size_t n_inversion_slices{0};
  std::size_t n_iv_proposed{0};
  std::size_t n_iv_audited{0};
  std::size_t n_iv_fallback{0};
  std::size_t n_iv_rejected_residual{0};
  double max_iv_proposal_residual_half_spreads{0.0};
  bool carry_confident{false}; // true iff every fitted slice is confident
  // True iff every fitted slice's inversions ran the AUDITED route (the fit
  // rows themselves, not just a diagnostic re-run), every accepted node passed
  // the cold-reference residual budget, and tolerated node drops stayed under
  // calib.max_certified_deam_drop_fraction. Non-AndersenLake methods have no
  // audit and are never certified (see deam_inversion_certified).
  bool inversion_certified{false};
  IncrementalRefitDiagnostics incremental{};
};

// Compact, persistence-friendly carry summary. Raw quotes and the individual
// CarryPairDiagnostic vector are deliberately not retained by a session.
struct SessionCarryDiagnostics {
  std::size_t n_candidates{0};
  std::size_t n_attempted{0};
  std::size_t n_solved{0};
  std::size_t n_retained{0};
  double effective_pair_count{0.0};
  double dispersion{0.0};
  double max_leave_one_out_shift{0.0};
  double confidence_half_width{0.0};
  double max_pcp_residual{0.0};
  bool available{false};
  bool confident{false};
  // Decision B provenance: Solved for a directly-inferred carry; a TermStructure*
  // value marks a slice whose carry was borrowed from the board term structure.
  CarrySource source{CarrySource::Solved};
};

// Parallel to expiries(). DeAmAuditDiagnostics contains counts/quantiles only;
// neither the source observations nor per-row IVs survive session construction.
struct SessionSliceDiagnostics {
  double T{0.0};
  SessionCarryDiagnostics carry{};
  DeAmAuditDiagnostics inversion{};
  bool inversion_available{false};
  bool inversion_certified{false};
};

// Stateful surface handle. Construct with `build` / `from_frame`; then query.
class VolaSession {
public:
  // Move-only: the fitted surface is heavy state, not a value to copy. Declaring
  // the moves implicitly deletes the copy operations (that is intentional).
  VolaSession(VolaSession &&) noexcept = default;
  VolaSession &operator=(VolaSession &&) noexcept = default;

  // ── Construction ─────────────────────────────────────────────────────────

  // Build from an already-installed `Underlying`. Drives `run_surface_parity`
  // (S <= 0 / non-finite r => InvalidArgument; no chains or no usable slice =>
  // NotFound) and retains its fitted surface, per-slice context, per-expiry
  // parity, and pricing inputs. Any parity-harness error is propagated.
  [[nodiscard]] static Result<VolaSession> build(const Underlying &under, const SessionInputs &in);

  // Build directly from an in-memory quote frame: install it into a local
  // `Universe` (`data_install`), resolve the resulting `Underlying`, then
  // `build`. Propagates any install / build error.
  [[nodiscard]] static Result<VolaSession> from_frame(const QuoteFrame &frame,
                                                      const SessionInputs &in);

  // Deep copy for copy-on-write publication. The fitted polymorphic curves and
  // correction caches are independently owned by the clone, so a local refit
  // can mutate the candidate while readers retain an immutable prior generation.
  [[nodiscard]] VolaSession clone() const;

  // ── Queries (const, no refit; see the forward-interpolation note above) ────

  // Interpolated European-equivalent implied vol at absolute strike K and
  // year-fraction T. NaN if K/T are non-finite or non-positive, or where the
  // surface declines to extrapolate (past the last slice, or > 50% below the
  // first).
  [[nodiscard]] double iv(double K, double T) const;

  // Interpolated total variance w(k, T) = sigma^2 * T at (K, T). Same domain /
  // NaN semantics as `iv`.
  [[nodiscard]] double total_variance(double K, double T) const;

  // Re-Americanized model fair value at (K, T, side): price the interpolated vol
  // on the interpolated carry via `american_price`. InvalidArgument if K/T are
  // non-finite or non-positive; any pricer error is propagated.
  [[nodiscard]] Result<double> fair_value(double K, double T, Side side) const;

  // Model Greeks + price at (K, T, side) via `american_greeks`, using this
  // side's correction cache when the session built one (else the cold Black-76-
  // leg path). InvalidArgument if K/T are non-finite or non-positive; any pricer
  // error is propagated.
  [[nodiscard]] Result<AmericanGreeks> greeks(double K, double T, Side side) const;

  // Per-query route introspection for the serve path (PR-C1), mirroring the
  // archived PricedSurface's QueryPricingRoute. Reports, WITHOUT pricing, which
  // route fair_value/greeks would take at (K, T, side):
  //   ColdReference    — no usable correction (cold-configured tier, override
  //                      surface, or a side with no built cache): the cold pricer.
  //   RepresentativeFast / CarryBank — the query is inside the correction box and
  //                      is served from the (single / carry-bank) cached graph.
  //   ColdFallback     — a usable cache exists but the query is OUTSIDE its box
  //                      (fitted vol above the sigma ceiling, or T/moneyness past
  //                      the padded box): the serve path falls back to the cold
  //                      Andersen-Lake pricer instead of a box-edge-clamped mark.
  // Returns ColdReference for an invalid (non-finite / non-positive) K or T (the
  // domain on which fair_value itself returns InvalidArgument).
  [[nodiscard]] QueryPricingRoute query_route(double K, double T, Side side) const noexcept;

  // ── Strike-ladder queries (SoA; reprice a whole expiry in one call) ────────
  //
  // The HFT / market-maker shape: given one expiry `T` and a struct-of-arrays
  // ladder (`strikes[i]`, `sides[i]`), write the re-Americanized fair value /
  // Greeks for every strike into `out[i]`. The per-expiry context — the
  // forward/carry interpolation in T and this side's correction-cache pointers —
  // is resolved ONCE and reused across the whole ladder. Each entry is
  // bit-identical to the scalar `fair_value`/`greeks` at the same (K, T, side).
  //
  // This is an ERGONOMICS + robustness primitive, not a throughput trick: the
  // shared-context amortization is negligible against the per-strike cached
  // pricer, so MEASURED latency is ~1x the scalar loop (opra_parity_bench:
  // ~6.2 us/option either way — the correction-eval cost dominates). Its value
  // is the single-call chain reprice with SoA output and per-strike NaN
  // isolation; a genuine latency win needs a cheaper per-strike pricer (or
  // vector transcendentals, unavailable under clang-cl — see README SIMD note),
  // not a layout change.
  //
  // A per-strike failure (non-positive/non-finite K, or a non-finite price) is
  // written as NaN into that slot and does not abort the ladder — a bad quote in
  // a chain must not sink the rest of the reprice. The call returns
  // InvalidArgument only for a structural error: non-finite/non-positive `T`, or
  // `strikes`/`sides`/`out` of unequal length.
  [[nodiscard]] Status fair_value_ladder(double T, std::span<const double> strikes,
                                         std::span<const Side> sides, std::span<double> out) const;

  // Greeks-ladder analogue of `fair_value_ladder`. `out[i]` receives the full
  // AmericanGreeks bundle; a per-strike failure leaves that slot value-
  // initialized with a NaN price (`out[i].price`). Same structural-error and
  // shared-context contract.
  [[nodiscard]] Status greeks_ladder(double T, std::span<const double> strikes,
                                     std::span<const Side> sides,
                                     std::span<AmericanGreeks> out) const;

  // Fused ladder evaluation for callers that need more than one model column.
  // Carry is resolved once for the expiry and surface IV once per strike. When
  // `greeks_out` is requested, its American price also supplies `price_out`, so
  // the cached correction graph is differentiated only once per contract.
  // A price-only uncached Andersen-Lake ladder uses the qualified eight-node
  // sigma-boundary interpolation in bounded side-specific blocks (real-SPY max
  // difference 3.8e-5/share versus scalar cold). Any rejected block falls back
  // to scalar cold pricing; cached prices and every Greek route are unchanged.
  // Every output span is optional (empty means unrequested); each nonempty span
  // must match `strikes.size()`. Invalid strikes are isolated as NaNs. The hot
  // path is allocation-free and safe for concurrent calls on a built session.
  [[nodiscard]] Status evaluate_ladder(double T, std::span<const double> strikes,
                                       std::span<const Side> sides, std::span<double> iv_out,
                                       std::span<double> price_out,
                                       std::span<AmericanGreeks> greeks_out) const;

  // ── Incremental update (tick-to-quote) ─────────────────────────────────────
  //
  // COMPATIBILITY-ONLY UNSAFE PRIMITIVE. This entry predates facade-owned
  // transactionality. It accepts caller-constructed rows, performs no canonical
  // preparation/provenance check, does not run surface admission, and mutates
  // this session directly. New code must use PricerFitter::refit_expiry. The
  // declaration remains temporarily for source compatibility only.
  //
  // Warm-start refit of ONE already-fitted expiry from a fresh observation set —
  // the market-maker's re-quote path. When a chain re-prints, the desk does not
  // want a whole-surface rebuild; it wants that one expiry's eSSVI slice nudged
  // to the new quotes, in microseconds, reusing the prior fit.
  //
  // `slice_idx` indexes the ascending-T fitted slices (parallel to `expiries()`
  // / `parity()`). `new_obs` is the rebuilt w-space observation set for that
  // expiry — produce it with `build_observations(chain, F, T, df, ...)` on the
  // fresh quotes (F/T/df are `expiries()[slice_idx]`'s forward / T / carry).
  //
  // The fit WARM-STARTS from the slice's current eSSVI params: the whole cube
  // (level/curvature/skew) seeds from the prior fit, so the LM converges in far
  // fewer iterations than the cold seed at the same quality (see essvi_calib.hpp
  // `warm`; measured in opra_parity_bench). If `in.calib.prior_strength > 0` a
  // Tikhonov term also
  // shrinks toward the prior, stabilising thin/noisy updates. The refit is
  // calendar-floored at the previous slice's ATM level so the term structure
  // stays monotone through the update.
  //
  // On success the refit slice REPLACES the old one in the surface (its expiry
  // identity preserved), the slice's `n_used` is refreshed, and the surface-
  // level calendar-arb flag in `diagnostics()` is re-evaluated; every subsequent
  // query (`iv`/`fair_value`/`greeks`/ladders) reflects the new slice with no
  // further refit. The returned `FitDiag` carries the refit's fit quality
  // (RMSE, iteration counts). NOTE: the per-expiry re-Americanized bid-ask
  // `parity()` fracs are NOT re-scored (the raw market quotes are not retained);
  // fit quality is in the returned diag. On a fit failure the surface is left
  // untouched and the fit error is propagated.
  //
  // For ConvexDense/SVI/C8, the replacement is fit on a staged local candidate,
  // checked only against its previous/next shared-k calendar pairs, independently
  // shape-validated, and atomically published. A failed fit or admission leaves
  // every served value unchanged; timing/outcome counters are in
  // `diagnostics().incremental`.
  //
  // @return InvalidArgument for an out-of-range `slice_idx`, empty `new_obs`, or
  //         a surface with no eSSVI slice at that index; the fit's own error
  //         (Unavailable on a degenerate slice) otherwise; else Ok(FitDiag).
  [[nodiscard]] Result<FitDiag> refit_slice(std::size_t slice_idx, std::span<const FitObs> new_obs);

  // Fast path for quote updates that changed uncertainty but not price. The
  // original, already-certified European IVs are reused exactly and only their
  // spread-derived weights are refreshed. A price/flag/shape change returns an
  // error so the caller can fall back to full de-Americanization.
  [[nodiscard]] Result<std::vector<FitObs>> cached_refit_observations(const Chain &chain,
                                                                      std::size_t slice_idx) const;

  // ── Serialization snapshot ─────────────────────────────────────────────────
  //
  // Distil this live session into a small, cache-free, value-typed `PricedSurface`
  // — the currency of `surface_archive`. The snapshot owns a DEEP COPY of the
  // fitted curves (the session keeps serving), the per-slice re-pricing context,
  // and the resolved pricing scalars (S, r, method, Andersen-Lake preset, uid).
  //
  // Its `fair_value`/`greeks` reproduce THIS session's COLD served path exactly:
  // for a polymorphic-override session (ConvexDense / Svi) that is the very path
  // the session prices on, so the snapshot is bit-identical to the live session.
  // For the eSSVI default path the snapshot rebuilds the fitted eSSVI slices into a
  // uniform `CurveSurface` and prices them COLD (the session's own cold fallback);
  // the surface's model IV is preserved, and cold is the accurate reference (the
  // live session's fast cached value differs only by the cache's carry surrogate).
  //
  // @return InvalidArgument if the session has no fitted slice (never for a
  //         successfully built session); otherwise the snapshot.
  [[nodiscard]] Result<PricedSurface> to_priced_surface() const;

  // ── Introspection ──────────────────────────────────────────────────────────
  //
  // BORROW CONTRACT for every view this section hands out — `surface()`,
  // `curve_override()`, `expiries()`, `parity()`, `slice_diagnostics()`,
  // `diagnostics()` and `inputs()`. The SESSION owns all of that storage; the
  // views own none of it, and each is valid exactly as long as this session
  // object is. INVALIDATION is simple here because the session is immutable after
  // construction (Thread-safety, above: `build`/`from_frame` are the only
  // mutating entries and each returns a FRESH session rather than rewriting one),
  // so nothing short of destroying the session invalidates a view — with one
  // consequence worth stating: a refit produces a NEW session, so views taken
  // from the old one keep naming the old object and must be re-taken. The session
  // is MOVE-ONLY (Ownership, above), so there is no copy whose storage a stale
  // view could be confused with; a plain move keeps element addresses valid, and
  // a moved-from session's views must not be read. Because the state is
  // immutable, holding these views across concurrent const readers is safe.
  // Anything that must outlive the session gets copied out, not borrowed.
  [[nodiscard]] const VolSurface &surface() const noexcept { return surface_; }

  // The optional polymorphic-surface override (ConvexDense / Svi / C8):
  // nullptr on the default eSSVI path (queries read surface()), non-null when
  // this session was built with a non-Essvi curve family. Read-only escape
  // hatch for a curve-family-aware caller that must inspect the SERVED
  // slices' own fit structure (e.g. the independent risk-surface validator's
  // node-k grid densification, oracle finding I-3) without the session
  // itself becoming curve-family-aware anywhere else.
  [[nodiscard]] const CurveSurface *curve_override() const noexcept {
    return curve_override_.has_value() ? &*curve_override_ : nullptr;
  }

  // Per fitted slice, ascending T. Parallel to `parity()`.
  [[nodiscard]] std::span<const SliceContext> expiries() const noexcept { return ctx_; }

  // Per-expiry re-Americanized parity, ascending T (parallel to `expiries()`).
  [[nodiscard]] std::span<const ParityReport> parity() const noexcept { return parity_; }

  // Per-expiry FIT-driver outcome for EVERY chain walked (‖ under.chains, in
  // CHAIN order -- NOT the fitted-slice order of expiries()/parity() above;
  // same alignment contract as `ExpiryFitReport` itself, surface_parity.hpp).
  // Task 3 (mark-domain-robustness): retained past the fit so the admission
  // layer's build report can spell WHY a chain is missing from the served
  // surface (CarryFailed/PrepStarved/PrepFailed/PrepUncovered (Task 1)/
  // FitFailed/FitRefusedCalendar (Task 6)/Skipped) instead of
  // collapsing every non-fit reason into one coarse outcome. Empty when this
  // session was NOT built through `run_surface_parity`/`fit_curve_surface`
  // (e.g. a session assembled by some other construction path never sets it).
  [[nodiscard]] std::span<const ExpiryFitReport> expiry_fit_reports() const noexcept {
    return expiry_fit_reports_;
  }

  [[nodiscard]] const SessionDiagnostics &diagnostics() const noexcept { return diag_; }

  [[nodiscard]] std::span<const SessionSliceDiagnostics> slice_diagnostics() const noexcept {
    return slice_diag_;
  }

  // Effective, fully-resolved fit inputs retained by the session. Quality
  // scoring uses these so a direct/fallback OOS refit sees the same preset,
  // quote filter, dividends, carry, and pricer policy as the shipped surface.
  //
  // `deam.caches` is always EMPTY here: those pointers are a build-time borrow of
  // caller-owned CorrectionCaches with no lifetime contract past `build`, so the
  // session releases them rather than publishing a pointer it cannot vouch for.
  // The session's own caches are `correction_caches()` / `correction_caches_at`.
  [[nodiscard]] const SessionInputs &inputs() const noexcept { return in_; }

  // ── Term carry accessors (the query re-pricing forward / effective yield) ──
  //
  // The interpolated term forward F(T) and coherent effective carry q_eff(T) at
  // an arbitrary T — the same clamp-outside / log-state-between-slices mechanic
  // the const queries use to re-price (see the forward-interpolation note above).
  // Exposed so a batch / whole-chain evaluator can resolve the per-expiry carry
  // (e.g. to invert bid/ask to American IV on the fit's own carry) without a
  // refit. Both return 0 for a non-finite / non-positive T (no slice to locate).
  [[nodiscard]] double forward_at(double T) const noexcept;
  [[nodiscard]] double q_eff_at(double T) const noexcept;
  [[nodiscard]] double rate_at(double T) const noexcept;

  // The per-side correction caches this session built (null pointers where a side
  // is on the cold path). Lets a batch / whole-chain evaluator route its
  // American-IV inversions through the SAME cached hot path (`american_price_cached`
  // = Black-76 + Chebyshev correction) that `fair_value` uses — orders of magnitude
  // faster than a cold Andersen-Lake solve per residual, and self-consistent with
  // the session's own re-pricing.
  [[nodiscard]] AmericanCorrectionCaches correction_caches() const noexcept {
    return in_.query_pricing_tier == QueryPricingTier::ColdReference ? AmericanCorrectionCaches{}
                                                                     : query_caches();
  }

  // T-aware query-cache selection. In the CarryBank tier the session owns a
  // bounded bank of fixed-(r,q) cache pairs built after fitting; this selects
  // the closest carry center while preserving fixed-carry Greek semantics.
  // Outside that tier this is identical to correction_caches().
  [[nodiscard]] AmericanCorrectionCaches correction_caches_at(double T) const noexcept;
  // Fixed-carry interpolation for CarryBank. Adjacent bank entries are chosen
  // in expiry order, then the query carry is projected onto their (r,q) segment
  // to obtain one clamped, call-constant weight. Other tiers return a single
  // endpoint (or an unusable blend for their cold path).
  [[nodiscard]] CorrectionBlend correction_blend_at(double T, Side side) const noexcept;
  [[nodiscard]] std::size_t query_cache_bank_size() const noexcept {
    return query_cache_bank_.size();
  }

private:
  friend class PricerFitter;

  [[nodiscard]] VolaSession clone_for_refit() const;
  [[nodiscard]] Result<FitDiag> apply_prepared_essvi_refit(std::size_t slice_idx,
                                                           const CanonicalPreparedExpiry &prepared);
  [[nodiscard]] Status refresh_refit_diagnostics();
  void build_fast_query_cache_bank(const Underlying &under);

  // The interpolated term forward and effective carry at a queried T.
  struct ForwardCarry {
    double forward{0.0};
    double q_eff{0.0};
    double rate{0.0};
  };

  // Constructed only via build/from_frame (VolSurface's default ctor is private,
  // so VolaSession is not default-constructible either). Takes ownership of the
  // (optional) per-side correction caches built during `build`, and the optional
  // polymorphic-surface override: empty for the default eSSVI path (queries read
  // `surface_`); populated for ConvexDense / Svi (queries read the override).
  VolaSession(VolSurface &&surface, std::vector<SliceContext> &&ctx,
              std::vector<ParityReport> &&parity, SessionInputs in, const SessionDiagnostics &diag,
              std::vector<SessionSliceDiagnostics> &&slice_diag,
              std::optional<CorrectionCache> &&corr_call, std::optional<CorrectionCache> &&corr_put,
              std::optional<CurveSurface> &&curve_override);

  // Non-owning per-side cache bundle for the query re-pricing (empty when the
  // caches were not built). Recomputed on each call so it survives a move.
  [[nodiscard]] AmericanCorrectionCaches query_caches() const noexcept {
    return AmericanCorrectionCaches{corr_call_ ? &*corr_call_ : nullptr,
                                    corr_put_ ? &*corr_put_ : nullptr};
  }

  // Locate T among the ascending slice T's and return the term forward / carry:
  // hold endpoint carry flat outside [T_0, T_last]; interpolate log-forward and
  // log-discount state between bracketing slices. Precondition: at least one slice
  // (guaranteed — build errors on an empty surface).
  [[nodiscard]] ForwardCarry interp_forward(double T) const noexcept;

  // Model-vol source, dispatching to the polymorphic override when present and to
  // the eSSVI VolSurface otherwise. Every query (iv / fair_value / greeks /
  // ladders) reads the model IV through here, so the convex/SVI surface flows
  // everywhere the eSSVI surface did with no other change. When there is no
  // polymorphic override and `in_.interp == ShapeBlend`, cross-expiry queries
  // route through `shape_blend_total_variance` instead of `surface_`'s own
  // linear-in-total-variance interpolation; PiecewiseTotalVariance (the
  // default) is unchanged and stays bit-identical to `surface_.iv`/`w`.
  [[nodiscard]] double model_iv(double k_log, double T) const noexcept;
  [[nodiscard]] double model_w(double k_log, double T) const noexcept;

  // ShapeBlend total variance at (k_log, T) off `surface_`, via the
  // projection-layer inserted-slice path (see InterpMode::ShapeBlend). The
  // session has no CurveSet of its own (forward/carry come from ctx_ via
  // interp_forward), so the handle skips the forward cache; only the slice
  // bracket is needed here. NaN if the inserted-slice handle fails to build
  // (never for a successfully built session's own surface_).
  [[nodiscard]] double shape_blend_total_variance(double k_log, double T) const noexcept;

  // True iff a query should route through the event-aware blend: `events`
  // is set AND the post-fit solve landed on a finite eMove (see
  // SessionDiagnostics::implied_emove -- NaN means "serve exactly as if
  // events were null", never a fabricated fallback value).
  [[nodiscard]] bool event_aware_active() const noexcept {
    return (in_.events != nullptr) && std::isfinite(diag_.implied_emove);
  }

  // Event-aware total variance at (k_log, T) off `surface_`, via the SAME
  // projection-layer inserted-slice path `shape_blend_total_variance` uses
  // (see there), but through `w_on_inserted_slice`'s event-aware overload
  // (events + the solved emove + `in_.now_ts_ns`) instead of the 3-arg
  // legacy call. `in_.interp` still selects which blend gets event-censored
  // (PiecewiseTotalVariance vs ShapeBlend -- see w_on_inserted_slice's doc).
  // Only called when `event_aware_active()` is true.
  [[nodiscard]] double event_aware_total_variance(double k_log, double T) const noexcept;

  // Correction cache to serve a query through, or nullptr for the cold
  // Andersen-Lake path. LegacyCompatible preserves the historical distinction
  // between cached eSSVI and cold polymorphic overrides. RepresentativeFast
  // explicitly serves the single-carry surrogate; CarryBank is handled by
  // correction_blend_at before this single-cache fallback is reached.
  [[nodiscard]] const CorrectionCache *served_cache(double T, Side side) const noexcept {
    if (in_.query_pricing_tier == QueryPricingTier::ColdReference ||
        (curve_override_.has_value() &&
         in_.query_pricing_tier == QueryPricingTier::LegacyCompatible)) {
      return nullptr;
    }
    const CorrectionCache *const cc = correction_caches_at(T).for_side(side);
    return (cc != nullptr && cc->populated() && cc->side() == side) ? cc : nullptr;
  }

  VolSurface surface_;
  std::vector<SliceContext> ctx_;    // ascending T
  std::vector<ParityReport> parity_; // ascending T (‖ ctx_)
  // Task 3 (mark-domain-robustness): ‖ under.chains, in CHAIN order (NOT ‖
  // ctx_/parity_ above -- see `expiry_fit_reports()`'s doc). Empty unless the
  // build path that produced this session set it.
  std::vector<ExpiryFitReport> expiry_fit_reports_;
  SessionInputs in_;
  SessionDiagnostics diag_;
  std::vector<SessionSliceDiagnostics> slice_diag_; // compact, ascending T
  std::optional<CorrectionCache> corr_call_;        // empty => cold path for calls
  std::optional<CorrectionCache> corr_put_;         // empty => cold path for puts
  struct QueryCacheBankEntry {
    double T{0.0};
    double rate{0.0};
    double q_eff{0.0};
    std::optional<CorrectionCache> call{};
    std::optional<CorrectionCache> put{};
  };
  std::vector<QueryCacheBankEntry> query_cache_bank_;
  // Polymorphic-surface override (ConvexDense / Svi). Empty => default eSSVI path
  // (queries read surface_). Move-only, like the session itself.
  std::optional<CurveSurface> curve_override_;
  struct IncrementalObservationStore {
    std::vector<std::vector<FitObs>> observations;
    std::vector<std::vector<double>> source_mids;
    std::vector<std::vector<std::uint8_t>> source_flags;
    std::vector<std::vector<double>> chain_mids;
    std::vector<std::vector<std::uint8_t>> chain_flags;
    // Full-chain bid/ask/timestamp snapshots: the robust carry weights consume
    // spreads (quality) and quote ages (freshness), and pair ELIGIBILITY
    // consumes bids/asks/mids/flags — so certified reuse must be able to prove
    // every carry coordinate unchanged, not just mids and flags (§14).
    std::vector<std::vector<double>> chain_bids;
    std::vector<std::vector<double>> chain_asks;
    std::vector<std::vector<std::int64_t>> chain_ts;
  };
  // Immutable and shared across copy-on-write generations. A spread-only refit
  // never changes certified prices/IVs, so cloning this history is wasted work.
  std::shared_ptr<const IncrementalObservationStore> incremental_observations_;
};

} // namespace atx::vol
