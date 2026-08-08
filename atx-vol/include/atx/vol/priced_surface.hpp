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
// A PricedSurface is this bundle plus, when explicitly prepared, transient query
// acceleration state. Its default and ColdReference query methods reproduce the
// session's COLD served path bit-for-bit:
//
//   fair_value(K, T, side) = american_price(S, K, T, surface.iv(k, T), r,
//                                            q_eff(T), side, method, al_opts)
//
// where k = ln(K / F(T)) and (F, q_eff) are the session's exact
// flat-carry tails / log-state-between forward interpolation. This is the path the
// ConvexDense (index / SPY) surface is served on — `VolaSession::fair_value`
// takes the cold Andersen-Lake branch whenever a polymorphic override is present
// (see session.hpp `served_cache`) — so a PricedSurface snapshot of such a
// session prices IDENTICALLY to the live session, reproducing its board accuracy.
//
// ## Why this type exists (vs. serializing a session)
//
// A session cannot be trivially serialized — its correction caches are large,
// carry-baked Chebyshev tensors that would have to be rebuilt to re-price anyway,
// and its VolSurface placeholder is unused on the override path. The archived
// PricedSurface payload is the small, pure, cache-free residue that fully
// determines the served theo. Optional query caches are rebuilt from that payload
// by `with_query_pricing`; they are never part of the wire format. PricedSurface
// is the currency of `surface_archive` (fit -> PricedSurface -> serialize ->
// deserialize -> PricedSurface -> price), and it is copy-free to construct from a
// live session via `VolaSession::to_priced_surface`.
//
// ## Thread-safety
//
// Immutable after `create` or the rvalue-only `with_query_pricing` preparation.
// All query methods are const, allocation-free reads of value state — safe to call
// concurrently on one published PricedSurface from any number of threads (the
// underlying CurveSurface, transient caches, and cold fallback are
// concurrent-const-safe).
//
// The one entry that reaches SHARED state is `with_query_pricing`: building a
// multi-center accelerator fans its independent center builds through the
// PROCESS-GLOBAL pricing pool (detail/pricing_executor.hpp) rather than spawning
// its own threads. Two caller-facing consequences, both from that header's
// Thread-safety section. First, that pool's topology must be set with
// `configure_pricing_executor` BEFORE the first call that builds it — a
// preparation is exactly such a call, and afterwards configuration is refused with
// AlreadyExists. Second, a preparation issued from INSIDE another pool dispatch
// runs fully inline instead of self-oversubscribing, and the result is
// bit-identical either way. Serving from an already-prepared surface touches no
// global state.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/american.hpp" // AmericanGreeks, AmericanMethod, AlOpts, american_price/greeks
#include "atx/vol/query_pricing.hpp"  // QueryPricingRoute, QueryPricingTier
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

class PricedSurface;
// WS-S S2 seam: the zero-copy read view (priced_surface_view.hpp) evaluates the
// same cold path as PricedSurface and must be able to mint a genuine FullGreekSeed
// (its constructor is private). Forward-declared here so it can be befriended
// below without pulling the view header into this widely-included one.
class PricedSurfaceView;
class PortfolioPricer;

// Immutable proof that one exact PricedSurface instance evaluated one exact
// contract through a specified full-Greek route. Construction is private: a
// caller may copy or move a genuine seed, but cannot fabricate or alter its
// provenance or numeric payload.
class FullGreekSeed final {
public:
  [[nodiscard]] std::uint32_t uid() const noexcept { return uid_; }
  [[nodiscard]] double K() const noexcept { return K_; }
  [[nodiscard]] double T() const noexcept { return T_; }
  [[nodiscard]] Side side() const noexcept { return side_; }
  [[nodiscard]] std::uint64_t surface_instance_id() const noexcept { return surface_instance_id_; }
  [[nodiscard]] bool analytic_greeks() const noexcept { return analytic_greeks_; }
  [[nodiscard]] QueryExecution query_execution() const noexcept { return query_execution_; }
  [[nodiscard]] double iv() const noexcept { return iv_; }
  [[nodiscard]] const AmericanGreeks &greeks() const noexcept { return greeks_; }

private:
  friend class PricedSurface;
  friend class PricedSurfaceView; // WS-S S2: the view mints seeds on the same cold path
  friend class PortfolioPricer;   // exports exact target-risk rows from its fused P&L solve

  FullGreekSeed(std::uint32_t uid, double K, double T, Side side, std::uint64_t surface_instance_id,
                bool analytic_greeks, QueryExecution query_execution, double iv,
                const AmericanGreeks &greeks) noexcept;

  std::uint32_t uid_;
  double K_;
  double T_;
  Side side_;
  std::uint64_t surface_instance_id_;
  bool analytic_greeks_;
  QueryExecution query_execution_;
  double iv_;
  AmericanGreeks greeks_;
};

// ── K4 first-order tier selector (WS-L L4 wiring) ────────────────────────────
//
// Which σ/rate-bumped boundary solves a Greek request needs. Defaults to the full
// analytic bundle, so EVERY existing call site is byte-unchanged. A reduced request
// threads straight into `american_greeks_al`'s `need_vega`/`need_rho`/`need_charm`
// (american.hpp:422): `vega=false` drops the σ± solves feeding vega/volga/vanna;
// `rho=false` drops the r± solves feeding rho; `charm=false` drops the wide speed
// stencils. Solve counts (BoundarySolves ledger): full = 5, {vega only, i.e.
// rho=charm=false} = 3, {none} = 1 (base). The columns a reduced request DOES return
// are BIT-IDENTICAL to the full bundle (same base boundary + σ± stencils —
// docs/seams/laned-greeks.md K4 guarantee); the unrequested greeks come back 0. This
// is the missing K4 batch-arm seam L4 wires (PM standing license, sprint §4 L4 + §5
// contention-note (2)); the cached correction and FD fallback routes ignore it and
// stay the full oracle (a correctness-preserving superset — the rare guard corners,
// not the backtest's analytic AL hot path).
//
// Namespace scope (not nested in PricedSurface) so the `= {}` default argument on the
// query methods does not trip clang's "default member initializer needed within the
// enclosing class outside a member function" rule; PricedSurface aliases it back as
// `PricedSurface::GreekNeeds`, the name every caller uses.
struct GreekNeeds {
  bool vega{true};
  bool rho{true};
  bool charm{true};
  [[nodiscard]] constexpr bool full() const noexcept { return vega && rho && charm; }
  [[nodiscard]] friend constexpr bool operator==(GreekNeeds a, GreekNeeds b) noexcept {
    return a.vega == b.vega && a.rho == b.rho && a.charm == b.charm;
  }
};

// ── Strip-carry hoisting (Task P-1) ─────────────────────────────────────────
//
// A quadrature strip (the var-swap/vol-swap/greeks node loop in
// derivatives.cpp) queries one surface at ONE constant T across up to ~2000
// log-moneyness nodes. `iv(K, T)` re-derives T's forward/carry AND the
// surface's own T-bracket on EVERY call -- pure waste when T does not move
// between calls, exactly the ladder-reuse case `evaluate_batch` already
// exploits for a run of bit-identical-T queries (see its "T-bracket and carry
// are resolved ONCE and reused ... bit-identical" comment below).
// `strip_carry_at` / `iv_with_carry` generalize that same hoist to a caller
// that cannot present its whole ladder as one span up front (a quadrature
// node loop interleaves the surface read with outer pricing arithmetic per
// node, and a greek stencil re-queries at a second, ROLLED T for its theta/
// charm repricings).
//
// `SurfaceStripCarry` is namespace-scope (not nested in `PricedSurface`)
// because `PricedSurfaceView` (priced_surface_view.hpp) produces and consumes
// the identical snapshot for its own zero-copy queries, and `SurfaceRef`
// (portfolio_pricer.hpp) forwards to whichever of the two a borrowed handle
// wraps -- one shared value type keeps that forwarding a single overload
// pair instead of a tagged union of two structurally-identical per-class
// types. Fields are an opaque snapshot: construct via `strip_carry_at` and
// consume via `iv_with_carry`; do not construct, compare, or mutate them
// directly, and do not reuse a token across a different T or a different
// surface instance.
struct SurfaceStripCarry {
  double T{0.0};
  double forward{0.0};
  double q_eff{0.0};
  double rate{0.0};
  std::size_t bracket_lo{0};
  std::size_t bracket_hi{0};
  double bracket_upper_weight{0.0};
  bool valid{false};
};

// A fitted, serialization-ready surface with optional transient query caches.
// Move-only (owns a move-only `CurveSurface`). Construct via `create` (validating)
// or receive one from `VolaSession::to_priced_surface` /
// `SurfaceArchiveV2::reconstruct_symbol`.
class PricedSurface {
public:
  ~PricedSurface();
  PricedSurface(PricedSurface &&) noexcept;
  PricedSurface &operator=(PricedSurface &&) noexcept;
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

  // Attach transient, archive-independent American correction state before
  // publishing the surface to concurrent readers. The method is rvalue-only so
  // no live immutable surface can change route under a caller. LegacyCompatible
  // and ColdReference remain cache-free; the two fast tiers require Andersen-
  // Lake and return InvalidArgument rather than silently serving another method.
  [[nodiscard]] Result<PricedSurface> with_query_pricing(QueryPricingTier tier) &&;

  // ── Queries (const; reproduce the session's cold served path) ──────────────

  // Every American price/Greek entry point accepts a call-local execution
  // contract. Configured follows the immutable surface's prepared tier;
  // ColdReference bypasses its transient accelerator without mutating or
  // reconstructing the surface. IV and total variance are tier-independent.

  // European-equivalent implied vol at absolute strike K and year-fraction T. NaN
  // only for non-finite/non-positive K/T (or an empty surface) -- there is no
  // fitted-range gate here. Outside the fitted tenor range this EXTRAPOLATES
  // (`CurveSurface::w`: flat vol short of the front pillar; flat total variance,
  // i.e. zero forward variance, at or past the last one). It does NOT return NaN
  // the way `VolSurface` and the demoted per-family containers do outside
  // THEIR fitted range; a caller that needs that stricter guarantee must
  // check K/T against `context()` itself. Identical to `VolaSession::iv` on
  // the override path.
  [[nodiscard]] double iv(double K, double T) const noexcept;

  // Total variance w(k, T) = sigma^2 * T at (K, T). Same domain / extrapolation
  // semantics as `iv` above.
  [[nodiscard]] double total_variance(double K, double T) const noexcept;

  // Re-Americanized model fair value at (K, T, side). Cold tiers price the
  // surface's model IV on interpolated carry via `american_price`; fast tiers use
  // their certified cached surrogate and fall back to the same cold route outside
  // its box. InvalidArgument for non-finite/non-positive K/T; any pricer error is
  // propagated.
  [[nodiscard]] Result<double>
  fair_value(double K, double T, Side side,
             QueryExecution execution = QueryExecution::Configured) const;

  // K4 first-order tier selector — namespace-scope type (see above), aliased here so
  // callers keep writing `PricedSurface::GreekNeeds`.
  using GreekNeeds = ::atx::vol::GreekNeeds;

  // Model Greeks + price at (K, T, side). Cold tiers use `american_greeks` with no
  // cache. Fast tiers differentiate the cached surrogate directly and retain the
  // cold route as their certified-box fallback.
  [[nodiscard]] Result<AmericanGreeks>
  greeks(double K, double T, Side side,
         QueryExecution execution = QueryExecution::Configured) const;

  // Faster Greeks via the analytic Andersen-Lake path (american_greeks_al): five
  // boundary solves instead of greeks()'s seven. price + delta/gamma/vega/rho/vanna/
  // volga are bit-identical to greeks(); theta/charm are the exact continuation-region
  // PDE (so NOT bit-reproducible across an archive round-trip — hence opt-in, off the
  // bit-stable greeks() default). The pricer enables it via PriceOptions. On a
  // fast query tier both methods intentionally return the same cached jet, so the
  // analytic flag has no effect while the cache route is active.
  // `needs` narrows the analytic bundle (K4 tier): the DEFAULT `{}` (all true) is the
  // full 5-solve bundle, BIT-IDENTICAL to the pre-L4 maskless call; a reduced request
  // (e.g. {vega=true,rho=false,charm=false} for a mark+vega friction consumer) skips
  // the r±/charm solves and returns those columns 0. Only honored on the cold analytic
  // AL route; the fast cached-surrogate tier returns its full internally-consistent jet.
  [[nodiscard]] Result<AmericanGreeks>
  greeks_analytic(double K, double T, Side side,
                  QueryExecution execution = QueryExecution::Configured,
                  GreekNeeds needs = {}) const;

  // Produce an immutable full-Greek handoff seed through one fused surface
  // resolution and one full American-Greek evaluation. The seed is valid only
  // for this exact surface instance, raw (uid,K,T,side), analytic route, and
  // effective execution route. Configured and ColdReference are equivalent only
  // when this surface's LegacyCompatible/ColdReference tier resolves both to the
  // cold route; prepared fast tiers keep them distinct. @return the underlying
  // evaluation error on an invalid or unsupported query.
  [[nodiscard]] Result<FullGreekSeed>
  full_greek_seed(double K, double T, Side side, bool analytic,
                  QueryExecution execution = QueryExecution::Configured) const;

  // American delta ONLY at (K, T, side) via `american_delta` — the single axis the
  // strike-from-delta solver consumes, at ~1-2 boundary solves instead of greeks()'s
  // seventeen (bit-identical value; same S/sigma/carry plumbing as greeks()).
  [[nodiscard]] Result<double> delta(double K, double T, Side side,
                                     QueryExecution execution = QueryExecution::Configured) const;

  // American vega ONLY at (K, T, side) — the single axis a vega-neutral sizing
  // call (e.g. the dispersion book build) needs, WITHOUT greeks_analytic()'s full
  // 5-solve AmericanGreeks bundle. Mirrors delta()'s structure exactly: same
  // resolve(K,T) validation/error mapping, routed to the AndersenLake-native
  // `american_vega_al` on the AL path (~0-2 boundary solves instead of 5), with
  // the existing non-AL-method FD fallback. The API contract is exact equality
  // to this dedicated reference; equality to a full analytic bundle is not a
  // general cross-method guarantee. (C1.7)
  [[nodiscard]] Result<double> vega(double K, double T, Side side,
                                    QueryExecution execution = QueryExecution::Configured) const;

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

  // One fused single-point evaluation. `iv` and `price` match `iv(K,T)` /
  // `fair_value(K,T,side).value()`; `greeks` matches
  // `greeks(K,T,side).value()` (or `greeks_analytic` when `analytic` is set).
  // On an active fast route, `analytic` is deliberately ignored because the
  // cached surrogate supplies a single internally consistent price/Greek jet.
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

  // (GreekNeeds is declared earlier, before greeks(), so greeks_analytic() can name it.)
  [[nodiscard]] FusedResult evaluate(double K, double T, Side side, EvalField fields, bool analytic,
                                     QueryExecution execution = QueryExecution::Configured,
                                     GreekNeeds needs = {}) const;

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
  // `needs` narrows the analytic Greek bundle when FirstOrder/SecondOrder is requested
  // (K4 tier). DEFAULT `{}` (all true) is the full 5-solve bundle, BIT-IDENTICAL to the
  // pre-L4 maskless batch; the reduced requested columns stay bit-identical and the
  // unrequested greeks come back 0. Ignored on the price-only / cached-correction / FD
  // routes (those stay the full oracle).
  [[nodiscard]] Status evaluate_batch(std::span<const double> K, std::span<const double> T,
                                      std::span<const Side> side, EvalField fields, bool analytic,
                                      EvaluationSoA out,
                                      simd::SimdIsa resolved_price_isa = simd::SimdIsa::Auto,
                                      QueryExecution execution = QueryExecution::Configured,
                                      GreekNeeds needs = {}) const;

  // ── Term carry accessors (the query re-pricing forward / effective yield) ──
  //
  // F(T) is geometrically interpolated between pillars; q_eff(T) is derived from
  // F(T), rate_at(T), spot, and T. In either tail, endpoint q_eff/r remain flat
  // and F is derived at the query T. Exact pillars retain calibrated state.
  // Both return 0 for a non-finite / non-positive T and
  // match `VolaSession::forward_at` / `q_eff_at` economically.
  [[nodiscard]] double forward_at(double T) const noexcept;
  [[nodiscard]] double q_eff_at(double T) const noexcept;
  // Per-expiry rate derived from each stored curve's discount factor. Old
  // archives fall back to `PricingContext::r` if a slice carries invalid df/T.
  [[nodiscard]] double rate_at(double T) const noexcept;

  // ── Strip-carry hoisting (Task P-1; see SurfaceStripCarry above) ───────────

  // Resolve T's forward/carry and this surface's own T-bracket ONCE. `valid`
  // is false (every other field left at its sentinel) for a non-finite/
  // non-positive T or an empty surface -- `iv_with_carry` then reads that as
  // "no opinion" and returns NaN, mirroring `iv(K,T)`'s own T-invalid path.
  [[nodiscard]] SurfaceStripCarry strip_carry_at(double T) const noexcept;

  // Surface IV at absolute strike K, off a carry already resolved by
  // `strip_carry_at`. Bit-identical to `iv(K, carry.T)` -- `interp_forward`/
  // `CurveSurface::bracket` are pure functions of T over this surface's
  // immutable state, so resolving once and reusing computes exactly what a
  // fresh per-call resolve would have. NaN for a non-finite/non-positive K or
  // an invalid carry (see above).
  [[nodiscard]] double iv_with_carry(double K, const SurfaceStripCarry &carry) const noexcept;

  // ── Introspection ──────────────────────────────────────────────────────────

  [[nodiscard]] const CurveSurface &surface() const noexcept { return surface_; }
  // BORROW of the per-slice carry vector this surface owns. `ctx_` is built once
  // by the private constructor and never rewritten — query-tier preparation
  // touches the transient caches, not the carry — so the span is valid for the
  // surface's lifetime and is safe for concurrent const readers, exactly like the
  // query entries above. `PricedSurface` is MOVE-ONLY, so the only invalidations
  // are destruction and move-assignment (a plain move keeps the elements'
  // addresses; a moved-from surface's span must not be read). Copy the contexts
  // out if they must outlive the surface.
  [[nodiscard]] std::span<const SliceContext> context() const noexcept { return ctx_; }
  [[nodiscard]] const PricingContext &pricing() const noexcept { return pricing_; }
  [[nodiscard]] std::size_t n_slices() const noexcept { return surface_.n_slices(); }
  [[nodiscard]] std::uint32_t uid() const noexcept { return pricing_.uid; }
  // Never-reused process-local value identity. Plain moves transfer it, move
  // assignment replaces it, and successful query-tier preparation refreshes it.
  // Retained valuation caches use this to reject same-address pointee ABA.
  [[nodiscard]] std::uint64_t instance_id() const noexcept { return instance_id_; }
  [[nodiscard]] QueryPricingTier query_pricing_tier() const noexcept { return query_pricing_tier_; }
  [[nodiscard]] std::size_t query_cache_pair_count() const noexcept;

  // Route that a valid point would take. Fast-configured points outside the
  // certified cache box report ColdFallback. Invalid K/T also report fallback
  // under a fast tier and ColdReference otherwise. A forced-cold execution
  // always reports ColdReference.
  [[nodiscard]] QueryPricingRoute
  query_pricing_route(double K, double T, Side side,
                      QueryExecution execution = QueryExecution::Configured) const noexcept;

  // The curve kind of slice `i` (ascending T). Precondition: i < n_slices().
  [[nodiscard]] VolCurveKind kind_at(std::size_t i) const noexcept;

private:
  PricedSurface(CurveSurface &&surface, std::vector<SliceContext> &&ctx,
                const PricingContext &pricing, std::vector<double> &&slice_rates,
                bool term_rates) noexcept;

  // The interpolated forward and identity-preserving effective carry at T.
  // Precondition: ctx_ non-empty, ascending T.
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
  [[nodiscard]] ResolvedSurfacePoint
  resolve_with_carry_and_bracket(double K, double T, ForwardCarry fc,
                                 CurveSurface::Bracket bracket) const noexcept;

  // Shared price/Greek routing for `evaluate` / `evaluate_batch` off one resolved
  // point — the single place the field bitmask drives the pricer calls.
  [[nodiscard]] FusedResult evaluate_resolved(const ResolvedSurfacePoint &p, Side side,
                                              EvalField fields, bool analytic,
                                              QueryExecution execution, GreekNeeds needs = {}) const;

  [[nodiscard]] Result<double> price_resolved(const ResolvedSurfacePoint &p, Side side,
                                              QueryExecution execution) const;
  [[nodiscard]] Result<AmericanGreeks> greeks_resolved(const ResolvedSurfacePoint &p, Side side,
                                                       bool analytic, QueryExecution execution,
                                                       GreekNeeds needs = {}) const;
  [[nodiscard]] Result<double> delta_resolved(const ResolvedSurfacePoint &p, Side side,
                                              QueryExecution execution) const;
  [[nodiscard]] Result<double> vega_resolved(const ResolvedSurfacePoint &p, Side side,
                                             QueryExecution execution) const;

  struct QueryAccelerator;

  CurveSurface surface_;          // fitted curves (any kind), ascending T
  std::vector<SliceContext> ctx_; // per-slice carry (‖ surface_ slices)
  PricingContext pricing_;        // cold re-pricing scalars
  // Construction-time rates decoded from each slice discount factor. This
  // removes the former two logarithms per off-pillar term-rate query.
  std::vector<double> slice_rates_; // one per surface slice
  bool term_rates_{false};          // any material departure from scalar r
  QueryPricingTier query_pricing_tier_{QueryPricingTier::LegacyCompatible};
  std::unique_ptr<QueryAccelerator> query_accelerator_{};
  std::uint64_t instance_id_{0};
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
