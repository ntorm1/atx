#pragma once

// Theo vocabulary + `TheoEngine` (THEO-7): the theo-vol overlay measure beside
// the served mark.
//
// A `PricedSurface` serves ONE number per (K, T, side): the market mark. Theo
// composes a SECOND number, `theo_vol` / `theo_price`, from that same served
// mark plus zero or more `ITheoOverlay` vol-space adjustments (fair-vol
// models, event/earnings adjustments, realized-vol blends -- Tasks 8+). Theo
// is an overlay MEASURE, never a competing mark: with zero overlays engaged,
// every `TheoValue` this engine produces is IDENTICAL, bit-for-bit, to what
// the surface itself would report --
//
//   theo_vol   == surface.iv(K, T)
//   theo_price == surface.fair_value(K, T, side).value()
//   edge_vol   == 0.0
//
// This identity holds BY CONSTRUCTION, not by tolerance: `market_vol` is read
// straight from `surface.iv(K, T)`, `theo_vol` starts at `market_vol` and is
// only ever ADDED to by a clamped overlay `dvol` (a sum over zero overlays
// adds nothing at all), and whenever `theo_vol` lands back on `market_vol`
// exactly (trivially true with no overlays, since no addition ever happens)
// the engine reuses the ALREADY-COMPUTED `market_price` -- literally the same
// `double` `surface.fair_value(K, T, side)` produced, not a second,
// independently-rounded American solve at "the same" vol. This zero-overlay
// case is the ONLY one the identity contract covers exactly.
//
// Once an overlay actually moves `theo_vol` away from `market_vol`, the
// engine pays for a fresh, COLD Andersen-Lake reprice at `theo_vol`, over the
// surface's own carry (`pricing().S`, `rate_at(T)`, `q_eff_at(T)`,
// `pricing().method`, `pricing().al_opts`). On a COLD-tier surface
// (`QueryPricingTier::LegacyCompatible`/`ColdReference`) that is the same
// route `fair_value` itself takes, so the two prices stay a clean, carry-
// consistent family as overlays are dialed up from zero. On a FAST-tier
// surface (`RepresentativeFast`/`CarryBank`), `market_price` is served
// through the cached Chebyshev-correction route
// (`PricedSurface::price_resolved`'s `Configured` branch), which is only an
// APPROXIMATION of the cold solve this engine reprices `theo_price` with --
// so `market_price - theo_price` carries a genuine route residual (typically
// small, concentrated at short tenors) that does NOT vanish as `dvol -> 0`
// the way it would on a cold surface; it simply stops being COMPUTED at
// `dvol == 0`, where the identity short-circuit above takes over instead.
// `TheoFlagBits::FastTierRoute` is set on exactly the queries where that
// residual is live (nonzero net `dvol`, fast-tier surface), naming it rather
// than leaving it silently folded into `edge_vol`'s price-space sibling.
// `edge_vol` itself (the VOL-space edge) is unaffected either way -- it is
// `market_vol - theo_vol`, computed entirely from `iv()` reads and overlay
// `dvol`, with no pricer call in it at all.
//
// `TheoConfig::price_theo = false` opts into a cheap, vol-space-only
// screening sheet: it never pays for the extra American solve at a shifted
// `theo_vol`. When the net overlay adjustment is exactly zero it can still
// fill `theo_price` for free (reusing `market_price`, per the identity
// above); once an overlay actually moves `theo_vol`, `theo_price` is left
// `NaN` rather than silently substituting the wrong-vol market price.
//
// Batch-first, allocation-free hot path: `value_into` chunks its query span
// into runs of at most `kTheoMaxBatch`, and drives each engaged overlay once
// per chunk over a fixed `std::array<OverlayAdjust, kTheoMaxBatch>` scratch
// buffer -- no heap allocation regardless of query count. The scalar `value`
// is a thin n=1 wrapper over the same path, so there is exactly one
// implementation to reason about.
//
// Tier-B: reachable by explicit include, deliberately outside the
// `atx/vol/vol.hpp` umbrella (mirrors realized_vol.hpp / breakeven.hpp).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "atx/vol/api/fitting/aggregate_arity.hpp" // TheoConfig field-count drift pin
#include "atx/vol/api/core/types.hpp"                  // Side, Result, Status

namespace atx::vol {

class PricedSurface; // priced_surface.hpp -- the served mark theo overlays beside
class EventSchedule; // event_vol.hpp -- optional earnings-event context
struct RvPanel;      // realized_vol.hpp (Task 1) -- optional realized-vol context

// One theo query: an American option identified by absolute strike, tenor
// (year-fraction), and side. Mirrors the (K, T, side) triple every
// `PricedSurface` query method takes.
struct TheoQuery {
  double strike{0.0};
  double tenor_years{0.0};
  Side side{Side::Call};
};

// Bit flags recorded per query in `TheoValue::flags`. Combine with `|`;
// `flags` itself is a plain `std::uint32_t` (not this enum), so no operator
// overloads are needed to set/test bits -- just `static_cast<std::uint32_t>`.
enum class TheoFlagBits : std::uint32_t {
  None = 0,
  // Reserved: Task 7 does not set this bit. Intended for a future overlay (or
  // the engine itself) to flag a query outside the surface's fitted tenor
  // domain (`PricedSurface::extrapolates_tenor`); left unset here rather than
  // guessed at without a consumer or a test to pin its exact trigger.
  Extrapolated = 1u << 0,
  // Set when ANY engaged overlay's `dvol` for this query was clamped to
  // +/- `TheoConfig::max_abs_dvol` before being folded into `theo_vol`.
  OverlayClamped = 1u << 1,
  // Set by an overlay (via `OverlayAdjust::flags`, OR'd into the query's
  // `TheoValue::flags` by the engine) that degrades gracefully on missing or
  // out-of-domain model input -- e.g. `RvBlendOverlay` with no `ctx.rv`, or
  // `EventVarOverlay` with no `ctx.events` (Task 8+). The overlay zeroes its
  // `dvol` for the affected queries and returns OK rather than failing the
  // whole batch: a model that cannot speak to a query is a data condition,
  // not a bug -- see the fail-loud contrast in `ITheoOverlay::adjust`'s doc,
  // which IS a bug (non-finite dvol/band).
  ModelMissing = 1u << 2,
  // Set on a query when the net applied overlay `dvol` is nonzero AND the
  // surface is fast-tier CONFIGURED (`PricedSurface::query_pricing_tier()`
  // is `RepresentativeFast` or `CarryBank`) -- read ONCE per `value_into`
  // call, not per query. (final-review M5: softened from "means the residual
  // is live" -- it is a CONFIGURATION signal, not a per-query guarantee. A
  // fast-tier-configured surface still cold-falls-back to a real Andersen-
  // Lake solve for any individual query that lands outside its certified
  // correction box (`PricedSurface::QueryPricingRoute::ColdFallback`), and
  // for those queries `market_price` already IS that cold solve, so the
  // residual this bit describes is exactly zero there -- over-flagging only,
  // never under.) Where the residual IS live: `market_price - theo_price`
  // (the PRICE-space edge) carries the fast tier's cached-correction route
  // residual against `theo_price`'s cold Andersen-Lake reprice, on top of
  // whatever the overlay itself intended. For the exact per-query answer,
  // call `PricedSurface::query_pricing_route(K, T, side, execution)`
  // directly. `edge_vol` (the VOL-space edge) is unaffected either way --
  // see the header banner.
  FastTierRoute = 1u << 3,
};

// One theo evaluation. Zero net overlay adjustment: `theo_vol == market_vol`,
// `theo_price == market_price`, `edge_vol == 0.0`, bit-for-bit against the
// served `PricedSurface` (see the header banner's identity contract).
struct TheoValue {
  double theo_vol{0.0};     // de-Americanized vol space, same space as surface iv()
  double theo_price{0.0};   // American premium at theo_vol (surface carry inputs)
  double market_vol{0.0};   // served surface iv(K,T)
  double market_price{0.0}; // served surface fair_value
  double edge_vol{0.0};     // market_vol - theo_vol  (>0 => market rich vs theo)
  double band_vol{0.0};     // half-width uncertainty band on theo_vol
  std::uint32_t flags{0};   // TheoFlagBits, OR-combined
};

// Non-owning market-state bundle a query evaluates against (SurfaceSet
// convention: the caller keeps every pointee alive for the call). `surface`
// is REQUIRED; `events`/`rv` are optional context for overlays that consume
// them (Task 8+) -- the engine itself only forwards the bundle, it never
// dereferences `events` or `rv`.
struct TheoContext {
  const PricedSurface *surface{nullptr}; // required
  const EventSchedule *events{nullptr};  // optional
  const RvPanel *rv{nullptr};            // optional
};

// One overlay's additive vol-space adjustment for one query: `dvol` shifts
// `theo_vol` (clamped to +/- `TheoConfig::max_abs_dvol` by the engine before
// accumulation); `band` contributes to `theo_vol`'s uncertainty band in
// quadrature (`band_vol = max(band_floor_vol, sqrt(Sum band_i^2))`); `flags`
// (`TheoFlagBits`, OR-combined) is OR'd by the engine into the query's
// `TheoValue::flags` alongside whatever it sets itself (`OverlayClamped`,
// `FastTierRoute`) -- an overlay degrading gracefully on missing/out-of-
// domain input (Task 8+) sets `ModelMissing` here, leaving `dvol`/`band` at
// their zero defaults, rather than failing the whole batch.
struct OverlayAdjust {
  double dvol{0.0};
  double band{0.0};
  std::uint32_t flags{0};
};

// One theo overlay: a pure function of `(ctx, query)` to a vol-space
// adjustment. Batch-first -- a scalar caller wraps a single-element span.
class ITheoOverlay {
public:
  virtual ~ITheoOverlay() = default;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  // Fill `out[i]` with `queries[i]`'s adjustment, for every i. `out.size() ==
  // queries.size()` is the caller's (TheoEngine's) contract to uphold; an
  // implementation may assume it. A non-OK return propagates: the engine
  // fails the WHOLE query batch rather than serving a partially-adjusted
  // result (an overlay that can degrade gracefully instead zeroes its `dvol`
  // for the affected queries and returns OK -- see `ModelMissing` above).
  [[nodiscard]] virtual Status adjust(const TheoContext &ctx, std::span<const TheoQuery> queries,
                                      std::span<OverlayAdjust> out) const = 0;
};

// ── First real overlays (Task 8) ───────────────────────────────────────────

// theo vol level lean: pull ATM level toward an RV-anchored forecast, damped
// in tenor. `dvol = w(T) * (rv_anchor - market_vol)`, `w(T) = weight *
// exp(-T / tenor_damp_years)`, `rv_anchor = ctx.rv->vol[rv_window_idx]`.
// Research doc S6.2: IV is the strongest single RV predictor but premium-
// biased, so this is the standard desk fair-vol core, not a replacement of
// the market level. DESIGNATED INITIALIZERS ONLY (see `TheoConfig`'s
// construction contract above -- the same rationale applies to every
// aggregate this module pins). Arity-pinned (3).
struct RvBlendConfig {
  double weight{0.35};           // 0 = identity; 1 = full RV anchor at the short end
  double tenor_damp_years{1.0};  // weight *= exp(-tenor/tenor_damp_years)
  std::uint8_t rv_window_idx{1}; // RvPanel window used as anchor (default 21d)
};

// Drift pin: RvBlendConfig has exactly THREE fields -- see TheoConfig's pin
// above for why this exists.
static_assert(detail::aggregate_arity_is_v<RvBlendConfig, 3>,
              "RvBlendConfig field count changed: update this pin, and confirm every "
              "construction site still uses designated initializers.");

// Builds the RV-blend overlay. `Err(InvalidArgument)` if `cfg.weight` is not
// finite, or `cfg.tenor_damp_years` is not finite and > 0 (it is a divisor).
// A query missing `ctx.rv`, whose `rv_window_idx` is out of range for
// `RvPanel::vol`, or whose selected RV slot is not finite (a window shorter
// than 2 available bars -- see `RvPanel`'s doc), degrades to `dvol = 0` with
// `ModelMissing` set rather than failing the batch.
[[nodiscard]] Result<std::unique_ptr<ITheoOverlay>> make_rv_blend_overlay(RvBlendConfig cfg = {});

// event variance swap: strip the market's implied event move, re-inject our
// own forecast. Research doc S6.3, over the existing `event_vol.hpp` FLEX
// censoring/recombination machinery: with `n = events->count_between(now,
// expiry)` events between the surface's valuation instant and the query's
// expiry, `dvol = event_recombined_vol(atm_cen, T, n, emove_forecast) -
// market_vol`, where `atm_cen` is the market's censored ATM vol after
// stripping `n` events' worth of `emove_market` out of its total variance
// (`censored_total_variance(market_vol^2 * T, n, emove_market)`). A query
// with no events before its expiry (`n == 0`) is a no-op by construction --
// both the strip and the re-inject terms vanish. DESIGNATED INITIALIZERS
// ONLY. Arity-pinned (2).
struct EventVarConfig {
  double emove_forecast{0.0}; // our per-event daily move forecast (0 disables)
  double emove_market{0.0};   // market-implied move to strip (from implied_emove_joint)
};

// Drift pin: EventVarConfig has exactly TWO fields -- see TheoConfig's pin
// above for why this exists.
static_assert(detail::aggregate_arity_is_v<EventVarConfig, 2>,
              "EventVarConfig field count changed: update this pin, and confirm every "
              "construction site still uses designated initializers.");

// Builds the event-variance overlay. `Err(InvalidArgument)` if
// `cfg.emove_forecast` or `cfg.emove_market` is not finite or negative (an
// eMove is a move-vol magnitude; never intentionally negative). A query
// missing `ctx.events`, or whose `market_vol`/`tenor_years` are not finite
// and > 0 (the surface extrapolates or the query is otherwise degenerate --
// `event_recombined_vol` itself requires `T > 0`), degrades to `dvol = 0`
// with `ModelMissing` set rather than failing the batch.
[[nodiscard]] Result<std::unique_ptr<ITheoOverlay>> make_event_var_overlay(EventVarConfig cfg);

// ── ML seam: fair-vol model overlay (Task 9) ────────────────────────────────
//
// `IFairVolModel` is the interface an offline-trained fair-vol model
// implements to plug into `TheoEngine` as an overlay. The feature vector it
// consumes is a FIXED, VERSIONED layout -- any offline trainer must
// produce/consume this exact ordering, hence `kFairVolFeatureSchemaV1` is
// threaded through both the model and its loader rather than left implicit.
//
// As of this sprint, the label-factory TSV (`examples/bev_label_factory.cpp`,
// `bev_label_factory --events <tsv>`) emits the full `kFairVolFeatureSchemaV1`
// feature block beside the target (`log_ratio = ln(sigma_be /
// sigma_entry_iv)`) and its join keys (`entry_ts_ns`, `uid`, `strike`,
// `expiry_ns`, `side`) -- a trainer reads that ONE file directly, with no
// offline join against the surface corpus or a separately-computed RV
// history required. `n_events_to_expiry` is NaN when the driver was run
// without `--events` (no calendar supplied at all -- distinct from a
// loaded-and-empty calendar, which counts 0). `rv_21d`/`rv_63d` are
// close-to-close realized vol over spot-mirror bars (O=H=L=C=spot,
// `RvEstimator::CloseToClose`) rather than real OHLC, until real OHLC lands
// in the corpus this driver walks.

// Feature vector contract for fair-vol models. Fixed order, versioned; any
// offline trainer must produce/consume this exact layout when assembling
// training rows -- see the ML seam banner above for what the label-factory
// TSV supplies.
inline constexpr std::size_t kFairVolFeatureCount = 8;
inline constexpr std::uint32_t kFairVolFeatureSchemaV1 = 1;
// [0] log_moneyness = ln(K/F)      [1] tenor_years
// [2] market_vol                   [3] rv_21d
// [4] rv_63d                       [5] iv_minus_rv = market_vol - rv_21d
// [6] n_events_to_expiry           [7] delta_abs (surface analytic |delta|)

// A fair-vol model: any implementation, trained however, that maps the fixed
// `kFairVolFeatureCount`-wide feature row to a log fair/market-vol ratio.
// Batch-first (mirrors `ITheoOverlay::adjust`'s batch shape) so a heavier
// model (e.g. a tree ensemble) can amortize per-call overhead; the shipped
// v1 linear model's per-row cost is trivial either way.
class IFairVolModel {
public:
  virtual ~IFairVolModel() = default;

  // The feature schema this model was trained against (`kFairVolFeatureSchemaV1`
  // for every model this module currently ships). `make_fair_vol_model_overlay`
  // checks this against `kFairVolFeatureSchemaV1` at construction (it assembles
  // exactly that fixed layout and nothing else) and refuses a mismatch --
  // a NEW model implementation cannot silently receive a feature block laid
  // out for a schema it wasn't trained against.
  [[nodiscard]] virtual std::uint32_t feature_schema() const noexcept = 0;

  // Predicts `y = ln(sigma_fair / market_vol)` per row, from `features_row_major`
  // (row `i`'s features occupy `[i*kFairVolFeatureCount, (i+1)*kFairVolFeatureCount)`
  // in the fixed order documented above). `Err(InvalidArgument)` if
  // `features_row_major.size() != n_rows * kFairVolFeatureCount` or
  // `log_ratio_out.size() != n_rows` -- a schema/size mismatch is the caller's
  // bug, not a data condition, so this is NOT the graceful-degrade path (that
  // lives in the overlay, per-row, on missing ctx.rv/ctx.events -- see
  // `make_fair_vol_model_overlay`). A model MAY still return a non-finite `y`
  // for an individual row (an out-of-domain feature combination); the overlay,
  // not this interface, is responsible for turning that into `ModelMissing`
  // rather than ever emitting NaN into `TheoValue`.
  [[nodiscard]] virtual Status predict(std::span<const double> features_row_major,
                                       std::size_t n_rows,
                                       std::span<double> log_ratio_out) const = 0;
};

// v1 model: linear on the fixed schema above, `y = b0 + sum_i b_i * x_i`.
// Coefficients loaded from a TSV: a `# schema=<n>` comment line (any leading
// comment line, matched in file order) declaring the feature schema the file
// was fit against, then `kFairVolFeatureCount + 1` whitespace-separated
// values (intercept first, then one coefficient per feature in the fixed
// order above) spread across the remaining non-comment, non-blank content.
//
// Errors (ParseError family, matching this module's own schema/coefficient-
// file precedent -- `SurfaceArchiveV2::open`'s "schema hash mismatch",
// `backtest_db.cpp`'s "schema mismatch"): `Err(IoError)` if the path can't be
// opened; `Err(ParseError)` if no `# schema=<n>` line is found, the declared
// schema isn't `kFairVolFeatureSchemaV1`, the coefficient value count isn't
// exactly `kFairVolFeatureCount + 1`, or any coefficient token fails to parse
// as a finite double.
[[nodiscard]] Result<std::unique_ptr<IFairVolModel>>
load_linear_fair_vol_model(std::string_view coef_tsv_path);

// Builds the model-driven fair-vol overlay: assembles the
// `kFairVolFeatureCount`-wide feature row from `ctx` (surface + rv + events)
// and the query, calls `model->predict`, and converts the returned log-ratio
// `y` to `dvol = market_vol * (exp(y) - 1)`. Band contribution is
// `|dvol| * 0.5` -- a placeholder pending quantile heads on the model
// interface (residual work, not this task's scope).
//
// `Err(InvalidArgument)` if `model` is null, or if `model->feature_schema()`
// isn't `kFairVolFeatureSchemaV1` -- both checked HERE, at construction (the
// overlay never re-checks either per call). The schema check exists because
// this overlay assembles exactly the `kFairVolFeatureSchemaV1` layout above
// and nothing else; a model trained against any other schema would otherwise
// silently receive a feature block laid out for a schema it never saw. At
// query time, this overlay
// fails OPEN, never closed (theo must always serve): missing `ctx.rv` or
// `ctx.events`, a per-row surface read that comes back non-finite/invalid
// (out-of-domain K/T, a rejected `delta()` call), or a non-finite predicted
// `y`/`dvol` for a row all degrade THAT row to `dvol = 0` with `ModelMissing`
// set, exactly like `RvBlendOverlay`/`EventVarOverlay` (Task 8) -- never a
// batch-wide `Err`. A non-OK `model->predict` return (a schema/size
// mismatch -- the caller's bug, not a data condition, see `IFairVolModel::
// predict`'s doc) DOES propagate and fail the whole `adjust()` call, mirroring
// the engine's own fail-loud contrast between a broken component and a
// degraded data condition.
[[nodiscard]] Result<std::unique_ptr<ITheoOverlay>>
make_fair_vol_model_overlay(std::shared_ptr<const IFairVolModel> model);

// Engine knobs. DESIGNATED INITIALIZERS ONLY (mirrors `BevReplayConfig`'s
// construction contract, breakeven.hpp): construct as
// `TheoConfig{.max_abs_dvol = 0.10}`, never positionally -- a positional
// initializer silently rebinds the moment a field is inserted rather than
// appended. `aggregate_arity_is_v` below pins the field count at THREE so an
// insertion (not an append) turns red at compile time instead of quietly
// rebinding an existing call site.
struct TheoConfig {
  double band_floor_vol{0.002}; // minimum band: label-noise floor (Derman-Kamal)
  double max_abs_dvol{0.15};    // per-overlay clamp; clamping sets OverlayClamped
  bool price_theo{true};        // false: skip American reprice (vol-space-only sheet)
};

// Drift pin: TheoConfig has exactly THREE fields. Adding, removing, or
// splitting one breaks this line -- the point is to force whoever changes the
// struct to read the construction contract above instead of appending a knob
// "for compatibility" with positional initializers that were never a
// supported form here.
static_assert(detail::aggregate_arity_is_v<TheoConfig, 3>,
              "TheoConfig field count changed: update this pin, and confirm every "
              "construction site still uses designated initializers.");

// `value_into`'s per-chunk overlay scratch is a fixed-capacity
// `std::array<OverlayAdjust, kTheoMaxBatch>` -- no heap allocation regardless
// of query-span length. A query span longer than this is processed in
// multiple chunks (see `value_into`), each engaged overlay called once per
// chunk.
inline constexpr std::size_t kTheoMaxBatch = 256;

// Composes zero or more `ITheoOverlay`s over a `PricedSurface`'s served mark
// into a theo sheet. See the header banner for the identity contract this
// class exists to uphold.
class TheoEngine {
public:
  // Validates `overlays` (no null entries) and `cfg` (`max_abs_dvol > 0`,
  // `band_floor_vol >= 0`); `Err(InvalidArgument)` on either violation.
  [[nodiscard]] static Result<TheoEngine>
  create(std::vector<std::unique_ptr<ITheoOverlay>> overlays, const TheoConfig &cfg = {});

  // Single-query convenience wrapper over `value_into` (n=1) -- one
  // implementation, not a parallel code path.
  [[nodiscard]] Result<TheoValue> value(const TheoContext &ctx, const TheoQuery &q) const;

  // Batch evaluation into a caller-owned, pre-sized `out` span.
  //
  // Validates, BEFORE any mutation of `out`: `out.size() == qs.size()`
  // (`Err(InvalidArgument)` otherwise, `out` left byte-for-byte untouched),
  // and `ctx.surface != nullptr` (`Err(InvalidArgument)` otherwise).
  //
  // A query the surface cannot price, a non-OK overlay `adjust`, or an
  // overlay returning a non-finite `dvol`/`band` (a broken overlay is a bug,
  // not a data condition -- this fails loud, naming the overlay and query
  // index, rather than silently clamping NaN/Inf into `theo_vol`), fails the
  // WHOLE call (`Err` propagated unchanged) -- deterministic all-or-nothing
  // semantics; entries already written into `out` before the failing query
  // are left as computed (not rolled back), exactly as
  // `PricedSurface::evaluate_batch` behaves on a per-element pricer error.
  //
  // No allocation: overlay scratch is a fixed `std::array<OverlayAdjust,
  // kTheoMaxBatch>`, reused across chunks of at most `kTheoMaxBatch` queries.
  [[nodiscard]] Status value_into(const TheoContext &ctx, std::span<const TheoQuery> qs,
                                  std::span<TheoValue> out) const;

private:
  explicit TheoEngine(std::vector<std::unique_ptr<ITheoOverlay>> ovs, const TheoConfig &c);

  std::vector<std::unique_ptr<ITheoOverlay>> overlays_;
  TheoConfig cfg_;
};

// Allocating convenience over `value_into` (Task 10; mirrors
// `compute_surface_analytics`'s shape, analytics.hpp): resolves every query in
// `queries` into a freshly-allocated `std::vector<TheoValue>` sized to
// `queries.size()`, via exactly one `value_into` call -- field-for-field
// identical to what a caller's own `std::vector<TheoValue>
// out(queries.size()); engine.value_into(ctx, queries, out);` would produce
// (there is no second implementation to drift out of step). Prefer
// `value_into` directly on a hot path with a caller-owned, reused buffer; this
// exists for callers -- a one-off screening sheet, a script, a test -- that
// would otherwise hand-roll the same allocate-then-fill pattern. Errors
// propagate unchanged from `value_into` (out-of-size never happens here: the
// vector is sized exactly to `queries.size()` before the call).
[[nodiscard]] Result<std::vector<TheoValue>> compute_theo_sheet(const TheoContext &ctx,
                                                                const TheoEngine &engine,
                                                                std::span<const TheoQuery> queries);

} // namespace atx::vol
