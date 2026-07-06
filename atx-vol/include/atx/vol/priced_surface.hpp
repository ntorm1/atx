#pragma once

// PricedSurface — a self-contained, serialization-ready fitted volatility surface
// that "slots into a pricer" and reproduces a VolaSession's theo values.
//
// A `VolaSession` is a heavy, live fitted handle: it owns the fitted curves, the
// per-slice re-pricing context, the market snapshot, AND (on the eSSVI path) a
// pair of Chebyshev correction caches. Most of that is scaffolding for the FIT.
// The minimal state needed to REPRICE — to answer iv / fair_value / greeks at an
// arbitrary (K, T, side) — is much smaller and fully value-typed:
//
//   * a `CurveSurface`     — the polymorphic fitted curves (any VolCurveKind), the
//                            SAME container the session serves its model IV from;
//   * a `SliceContext` per  — the term (forward, q_eff) the query re-prices on, the
//     slice                  session's forward-interpolation coordinates;
//   * a `PricingContext`   — the scalars the cold re-pricing needs (spot, rate,
//                            pricer method, Andersen-Lake preset, uid).
//
// A PricedSurface is exactly this bundle, and its query methods reproduce the
// session's COLD served path bit-for-bit:
//
//   fair_value(K, T, side) = american_price(S, K, T, surface.iv(k, T), r,
//                                            q_eff(T), side, method, al_opts)
//
// where k = ln(K / F(T)) and (F, q_eff) are the session's exact
// clamp-outside / linear-between forward interpolation. This is the path the
// ConvexDense (index / SPY) surface is served on — `VolaSession::fair_value`
// takes the cold Andersen-Lake branch whenever a polymorphic override is present
// (see session.hpp `served_cache`) — so a PricedSurface snapshot of such a
// session prices IDENTICALLY to the live session, reproducing its board accuracy.
//
// ## Why this type exists (vs. serializing a session)
//
// A session cannot be trivially serialized — its correction caches are large,
// carry-baked Chebyshev tensors that would have to be rebuilt to re-price anyway,
// and its VolSurface placeholder is unused on the override path. PricedSurface is
// the small, pure, cache-free residue that fully determines the served theo. It
// is the currency of `surface_archive` (fit -> PricedSurface -> serialize ->
// deserialize -> PricedSurface -> price), and it is copy-free to construct from a
// live session via `VolaSession::to_priced_surface`.
//
// ## Thread-safety
//
// Immutable after `create`. All query methods are const, allocation-free reads of
// value state — safe to call concurrently on one PricedSurface from any number of
// threads (the underlying CurveSurface and stateless cold pricer are both
// concurrent-const-safe).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/american.hpp"        // AmericanGreeks, AmericanMethod, AlOpts, american_price/greeks
#include "atx/vol/surface_parity.hpp"  // SliceContext
#include "atx/vol/types.hpp"           // Result, Status, Side
#include "atx/vol/vol_curve.hpp"       // CurveSurface, VolCurveKind

namespace atx::vol {

// The cold re-pricing scalars — everything `american_price` / `american_greeks`
// need beyond the surface's model IV. Trivially copyable so the archive stores it
// verbatim. `al_opts` is the RESOLVED Andersen-Lake preset the session priced with
// (a session resolves a nullopt preset to the fast preset at build time, so the
// stored value is never ambiguous). `method` is the cold pricer (Andersen-Lake).
struct PricingContext {
  double S{0.0};                                       // spot (> 0)
  double r{0.0};                                       // continuously-compounded rate
  std::int64_t now_ts_ns{0};                           // valuation timestamp (epoch ns)
  AmericanMethod method{AmericanMethod::AndersenLake}; // cold pricer
  AlOpts al_opts{};                                    // resolved AL accuracy preset
  std::uint32_t uid{0};                                // underlying id (informational)
};

// A fitted, cache-free, serialization-ready surface. Move-only (owns a move-only
// `CurveSurface`). Construct via `create` (validating) or receive one from
// `VolaSession::to_priced_surface` / `SurfaceArchive::map_symbol`.
class PricedSurface {
 public:
  PricedSurface(PricedSurface&&) noexcept = default;
  PricedSurface& operator=(PricedSurface&&) noexcept = default;
  PricedSurface(const PricedSurface&) = delete;
  PricedSurface& operator=(const PricedSurface&) = delete;

  // Assemble from a fitted `CurveSurface` (moved in), its per-slice context, and
  // the pricing scalars.
  //
  // Errors: InvalidArgument if the surface is empty, `context` length != slice
  // count, S <= 0, r non-finite, or the slice T's are not strictly ascending
  // (the forward interpolation and no-extrapolation guards assume ascending T).
  [[nodiscard]] static Result<PricedSurface> create(CurveSurface&& surface,
                                                    std::vector<SliceContext> context,
                                                    const PricingContext& pricing);

  // ── Queries (const; reproduce the session's cold served path) ──────────────

  // European-equivalent implied vol at absolute strike K and year-fraction T. NaN
  // outside the surface's no-extrapolation domain or for non-finite/non-positive
  // K/T. Identical to `VolaSession::iv` on the override path.
  [[nodiscard]] double iv(double K, double T) const noexcept;

  // Total variance w(k, T) = sigma^2 * T at (K, T). Same domain / NaN semantics.
  [[nodiscard]] double total_variance(double K, double T) const noexcept;

  // Re-Americanized model fair value at (K, T, side): price the surface's model IV
  // on the interpolated carry via cold `american_price`. Bit-identical to
  // `VolaSession::fair_value` when the session serves cold (override present).
  // InvalidArgument for non-finite/non-positive K/T; any pricer error propagated.
  [[nodiscard]] Result<double> fair_value(double K, double T, Side side) const;

  // Model Greeks + price at (K, T, side) via cold `american_greeks` (null cache),
  // bit-identical to `VolaSession::greeks` on the override path.
  [[nodiscard]] Result<AmericanGreeks> greeks(double K, double T, Side side) const;

  // ── Term carry accessors (the query re-pricing forward / effective yield) ──
  //
  // The interpolated term forward F(T) and effective carry q_eff(T): clamp to the
  // endpoint slice outside [T_0, T_last], linearly interpolate between. Both
  // return 0 for a non-finite / non-positive T. Match `VolaSession::forward_at` /
  // `q_eff_at` exactly.
  [[nodiscard]] double forward_at(double T) const noexcept;
  [[nodiscard]] double q_eff_at(double T) const noexcept;

  // ── Introspection ──────────────────────────────────────────────────────────

  [[nodiscard]] const CurveSurface& surface() const noexcept { return surface_; }
  [[nodiscard]] std::span<const SliceContext> context() const noexcept { return ctx_; }
  [[nodiscard]] const PricingContext& pricing() const noexcept { return pricing_; }
  [[nodiscard]] std::size_t n_slices() const noexcept { return surface_.n_slices(); }
  [[nodiscard]] std::uint32_t uid() const noexcept { return pricing_.uid; }

  // The curve kind of slice `i` (ascending T). Precondition: i < n_slices().
  [[nodiscard]] VolCurveKind kind_at(std::size_t i) const noexcept;

 private:
  PricedSurface(CurveSurface&& surface, std::vector<SliceContext>&& ctx,
                const PricingContext& pricing) noexcept;

  // The interpolated (forward, q_eff) at T — the session's exact clamp-outside /
  // linear-between mechanic. Precondition: ctx_ non-empty, ascending T.
  struct ForwardCarry { double forward{0.0}; double q_eff{0.0}; };
  [[nodiscard]] ForwardCarry interp_forward(double T) const noexcept;

  CurveSurface surface_;              // fitted curves (any kind), ascending T
  std::vector<SliceContext> ctx_;     // per-slice carry (‖ surface_ slices)
  PricingContext pricing_;            // cold re-pricing scalars
};

}  // namespace atx::vol
