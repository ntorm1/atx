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

#include "atx/vol/american.hpp" // AmericanGreeks, AmericanMethod, AlOpts, american_price/greeks
#include "atx/vol/simd/cpu.hpp"       // SimdIsa (call-local resolved price route)
#include "atx/vol/surface_parity.hpp" // SliceContext
#include "atx/vol/types.hpp"          // Result, Status, Side
#include "atx/vol/vol_curve.hpp"      // CurveSurface, VolCurveKind

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
  PricedSurface(PricedSurface &&) noexcept = default;
  PricedSurface &operator=(PricedSurface &&) noexcept = default;
  PricedSurface(const PricedSurface &) = delete;
  PricedSurface &operator=(const PricedSurface &) = delete;

  // Assemble from a fitted `CurveSurface` (moved in), its per-slice context, and
  // the pricing scalars.
  //
  // Errors: InvalidArgument if the surface is empty, `context` length != slice
  // count, S <= 0, r non-finite, or the slice T's are not strictly ascending
  // (the forward interpolation and no-extrapolation guards assume ascending T).
  [[nodiscard]] static Result<PricedSurface>
  create(CurveSurface &&surface, std::vector<SliceContext> context, const PricingContext &pricing);

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

  // Faster Greeks via the analytic Andersen-Lake path (american_greeks_al): five
  // boundary solves instead of greeks()'s seven. price + delta/gamma/vega/rho/vanna/
  // volga are bit-identical to greeks(); theta/charm are the exact continuation-region
  // PDE (so NOT bit-reproducible across an archive round-trip — hence opt-in, off the
  // bit-stable greeks() default). The pricer enables it via PriceOptions.
  [[nodiscard]] Result<AmericanGreeks> greeks_analytic(double K, double T, Side side) const;

  // American delta ONLY at (K, T, side) via `american_delta` — the single axis the
  // strike-from-delta solver consumes, at ~1-2 boundary solves instead of greeks()'s
  // seventeen (bit-identical value; same S/sigma/carry plumbing as greeks()).
  [[nodiscard]] Result<double> delta(double K, double T, Side side) const;

  // American vega ONLY at (K, T, side) — the single axis a vega-neutral sizing
  // call (e.g. the dispersion book build) needs, WITHOUT greeks_analytic()'s full
  // 5-solve AmericanGreeks bundle. Mirrors delta()'s structure exactly: same
  // resolve(K,T) validation/error mapping, routed to the AndersenLake-native
  // `american_vega_al` on the AL path (~0-2 boundary solves instead of 5), with
  // the existing non-AL-method FD fallback. The API contract is exact equality
  // to this dedicated reference; equality to a full analytic bundle is not a
  // general cross-method guarantee. (C1.7)
  [[nodiscard]] Result<double> vega(double K, double T, Side side) const;

  // ── Fused resolution + single-point / batch evaluation (P1.1) ──────────────
  //
  // Every query method above independently re-does the SAME resolution: validate
  // (K, T), interpolate the T-bracket forward/carry, form k = ln(K / F(T)), and
  // read the surface IV. `resolve` does that work ONCE; `evaluate` /
  // `evaluate_batch` price + Greek off a single resolution, and the six public
  // query methods are themselves reimplemented on top of `resolve` so there is
  // exactly one resolution code path (no six copies).

  // Which outputs a fused evaluation should populate. Bitmask; combine with `|`.
  // FirstOrder/SecondOrder retain their original FULL American Greeks bundle.
  // Delta and Vega use the dedicated scalar references; a bundle bit dominates
  // either dedicated bit when combined, so no duplicate axis solve is run.
  enum class EvalField : std::uint32_t {
    None = 0,
    Iv = 1u << 0,          // European-equivalent implied vol (always free once resolved)
    Price = 1u << 1,       // American mark (fair_value)
    FirstOrder = 1u << 2,  // delta, gamma, vega, theta, rho
    SecondOrder = 1u << 3, // vanna, volga, charm
    Delta = 1u << 4,       // dedicated american_delta route
    Vega = 1u << 5,        // dedicated american_vega_al/reference route
  };
  // DEFERRED (invariant #4.9 — explicit, not silent): the Delta/Vega selective
  // bits above and the matching EvaluationSoA delta/vega columns are complete
  // PricedSurface-LAYER primitives. Wiring portfolio-level callers to request
  // only these axes selectively is deferred to WP9 staging (sprint §6); until
  // then the primitives are exercised by the batch tests, not a portfolio path.

  // The point resolved once: validate + T-bracket + forward/carry + ln(K/F) +
  // surface IV. Everything downstream consumes this; nothing re-resolves. `valid`
  // is false (all other fields left 0) for a non-finite/non-positive K or T.
  struct ResolvedSurfacePoint {
    double K{0.0};
    double T{0.0};
    double forward{0.0};
    double q_eff{0.0};
    double k_log{0.0};
    double sigma{0.0};
    double rate{0.0};
    bool valid{false};
  };

  // Resolve validate + T-bracket + forward/carry + log(K/F) + surface IV, ONCE.
  // noexcept + allocation-free. Bit-identical resolution to what every query
  // method computed individually before P1.1.
  [[nodiscard]] ResolvedSurfacePoint resolve(double K, double T) const noexcept;

  // One fused single-point evaluation. `iv` and `price` are bit-identical to
  // `iv(K,T)` / `fair_value(K,T,side).value()`; `greeks` is bit-identical to
  // `greeks(K,T,side).value()` (or `greeks_analytic` when `analytic` is set).
  // `status` is Ok, or the propagated pricer error (e.g. the negative-carry
  // Unsupported corner), in which case the numeric fields are NaN.
  //
  // Field routing: requesting FirstOrder|SecondOrder runs the Greek bundle; since
  // american_greeks_fd().price IS the fair value (bit-identical), requesting
  // Greeks yields `price` FOR FREE (no extra american_price solve). Requesting
  // only Price runs american_price alone. Requesting neither does NO pricer solve
  // (Iv-only is one resolution and nothing else). `iv` is always populated when
  // the point is valid (it is free from the resolution).
  struct FusedResult {
    double iv{0.0};
    double price{0.0};
    AmericanGreeks greeks{}; // populated when FirstOrder|SecondOrder requested
    Status status{};         // default-constructed == Ok
  };

  [[nodiscard]] FusedResult evaluate(double K, double T, Side side, EvalField fields,
                                     bool analytic) const;

  // Caller-provided, one-entry-per-query output spans for `evaluate_batch`.
  // On the legacy path (no Delta/Vega bit), the original iv/price/status sizing
  // contract is unchanged and `greeks` may be empty when no bundle is requested.
  // On a dedicated-only selective path (Delta and/or Vega, with neither the
  // FirstOrder nor SecondOrder bundle), status and each requested numeric column
  // must be query-sized; unrequested numeric spans may be empty or query-sized
  // and stay untouched. Combined bundle/selective masks retain the legacy
  // iv/price sizing contract and additionally mirror requested axes.
  struct EvaluationSoA {
    std::span<double> iv{};
    std::span<double> price{};
    std::span<AmericanGreeks> greeks{}; // empty if Greeks not requested
    std::span<Status> status{};
    std::span<double> delta{}; // nullable; written only when Delta requested
    std::span<double> vega{};  // nullable; written only when Vega requested
  };

  // Fused batch/ladder evaluation of a (K, T, side) vector. When consecutive
  // entries share a bit-identical `T` (a strike ladder), the T-bracket and carry
  // are resolved ONCE and reused across the run — the per-entry result is
  // bit-identical to `evaluate` because the reused carry equals the per-entry
  // interpolation exactly (T compared by raw bits, never a tolerance). Writes
  // only into `out`'s caller-provided spans; the valid/hot path allocates nothing.
  // `resolved_price_isa` is call-local and affects only the price-only resolved
  // American batch; Auto preserves the measured scalar shipment gate.
  // @return InvalidArgument on a K/T/side length mismatch or an out-span sized
  //         neither 0 (where permitted) nor the query count, when any input span
  //         overlaps any output span, or when any nonempty output spans overlap
  //         each other. Overlap is rejected before any write.
  [[nodiscard]] Status evaluate_batch(std::span<const double> K, std::span<const double> T,
                                      std::span<const Side> side, EvalField fields, bool analytic,
                                      EvaluationSoA out,
                                      simd::SimdIsa resolved_price_isa = simd::SimdIsa::Auto) const;

  // ── Term carry accessors (the query re-pricing forward / effective yield) ──
  //
  // The interpolated term forward F(T) and effective carry q_eff(T): clamp to the
  // endpoint slice outside [T_0, T_last], linearly interpolate between. Both
  // return 0 for a non-finite / non-positive T. Match `VolaSession::forward_at` /
  // `q_eff_at` exactly.
  [[nodiscard]] double forward_at(double T) const noexcept;
  [[nodiscard]] double q_eff_at(double T) const noexcept;
  // Per-expiry rate derived from each stored curve's discount factor. Old
  // archives fall back to `PricingContext::r` if a slice carries invalid df/T.
  [[nodiscard]] double rate_at(double T) const noexcept;

  // ── Introspection ──────────────────────────────────────────────────────────

  [[nodiscard]] const CurveSurface &surface() const noexcept { return surface_; }
  [[nodiscard]] std::span<const SliceContext> context() const noexcept { return ctx_; }
  [[nodiscard]] const PricingContext &pricing() const noexcept { return pricing_; }
  [[nodiscard]] std::size_t n_slices() const noexcept { return surface_.n_slices(); }
  [[nodiscard]] std::uint32_t uid() const noexcept { return pricing_.uid; }

  // The curve kind of slice `i` (ascending T). Precondition: i < n_slices().
  [[nodiscard]] VolCurveKind kind_at(std::size_t i) const noexcept;

private:
  PricedSurface(CurveSurface &&surface, std::vector<SliceContext> &&ctx,
                const PricingContext &pricing) noexcept;

  // The interpolated (forward, q_eff) at T — the session's exact clamp-outside /
  // linear-between mechanic. Precondition: ctx_ non-empty, ascending T.
  struct ForwardCarry {
    double forward{0.0};
    double q_eff{0.0};
    double rate{0.0};
  };
  [[nodiscard]] ForwardCarry interp_forward(double T) const noexcept;

  // Resolve a point given an already-interpolated carry (the ladder-reuse path).
  // Bit-identical to `resolve(K, T)` when `fc == interp_forward(T)`.
  [[nodiscard]] ResolvedSurfacePoint resolve_with_carry(double K, double T,
                                                        ForwardCarry fc) const noexcept;

  // Shared price/Greek routing for `evaluate` / `evaluate_batch` off one resolved
  // point — the single place the field bitmask drives the pricer calls.
  [[nodiscard]] FusedResult evaluate_resolved(const ResolvedSurfacePoint &p, Side side,
                                              EvalField fields, bool analytic) const;

  CurveSurface surface_;          // fitted curves (any kind), ascending T
  std::vector<SliceContext> ctx_; // per-slice carry (‖ surface_ slices)
  PricingContext pricing_;        // cold re-pricing scalars
  bool term_rates_{false};        // any slice df differs from scalar-r df
};

// ── EvalField bitmask operators ──────────────────────────────────────────────
// Provided so the enum arithmetic type-checks under /W4 /WX (a scoped enum has no
// implicit integer conversions). `has_field` is the "is this bit set" test.
[[nodiscard]] constexpr PricedSurface::EvalField operator|(PricedSurface::EvalField a,
                                                           PricedSurface::EvalField b) noexcept {
  return static_cast<PricedSurface::EvalField>(static_cast<std::uint32_t>(a) |
                                               static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr PricedSurface::EvalField operator&(PricedSurface::EvalField a,
                                                           PricedSurface::EvalField b) noexcept {
  return static_cast<PricedSurface::EvalField>(static_cast<std::uint32_t>(a) &
                                               static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr PricedSurface::EvalField operator^(PricedSurface::EvalField a,
                                                           PricedSurface::EvalField b) noexcept {
  return static_cast<PricedSurface::EvalField>(static_cast<std::uint32_t>(a) ^
                                               static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr PricedSurface::EvalField operator~(PricedSurface::EvalField a) noexcept {
  return static_cast<PricedSurface::EvalField>(~static_cast<std::uint32_t>(a));
}
constexpr PricedSurface::EvalField &operator|=(PricedSurface::EvalField &a,
                                               PricedSurface::EvalField b) noexcept {
  a = a | b;
  return a;
}
constexpr PricedSurface::EvalField &operator&=(PricedSurface::EvalField &a,
                                               PricedSurface::EvalField b) noexcept {
  a = a & b;
  return a;
}
[[nodiscard]] constexpr bool has_field(PricedSurface::EvalField set,
                                       PricedSurface::EvalField bit) noexcept {
  return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(bit)) != 0u;
}

} // namespace atx::vol
