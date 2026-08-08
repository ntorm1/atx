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
// the engine reuses the ALREADY-COMPUTED `market_price` -- the surface's own
// `fair_value(K, T, side)` result -- as `theo_price`, rather than re-deriving
// it through a second, independently-rounded American solve. Only once an
// overlay actually moves `theo_vol` away from `market_vol` does the engine pay
// for a fresh American reprice, at `theo_vol`, over the surface's OWN carry
// (`pricing().S`, `rate_at(T)`, `q_eff_at(T)`, `pricing().method`,
// `pricing().al_opts`) -- the same inputs `PricedSurface::fair_value`
// reprices with internally (see priced_surface.cpp `price_resolved`), so the
// two curves stay carry-consistent as overlays are dialed up.
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

#include "atx/vol/detail/aggregate_arity.hpp" // TheoConfig field-count drift pin
#include "atx/vol/types.hpp"                  // Side, Result, Status

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
  // Reserved for overlays (Task 8-9): an overlay that degrades gracefully
  // (missing model input, stale event data, ...) zeroes its `dvol` and is
  // expected to signal that degradation here once `OverlayAdjust` grows a
  // flags field of its own. `OverlayAdjust` carries no such field today, so
  // Task 7's engine never sets this bit -- extending `OverlayAdjust` is that
  // future task's documented change, not this one's.
  ModelMissing = 1u << 2,
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
// quadrature (`band_vol = max(band_floor_vol, sqrt(Sum band_i^2))`).
struct OverlayAdjust {
  double dvol{0.0};
  double band{0.0};
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
  // A query the surface cannot price, or a non-OK overlay `adjust`, fails the
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

} // namespace atx::vol
