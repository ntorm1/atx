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
// among those slice T's and, for a T strictly between two slices, LINEARLY
// interpolates `forward` and `q_eff`; a T at or beyond an endpoint clamps to that
// endpoint slice. That interpolated (F(T), q_eff(T)) drives both the surface's
// log-moneyness and the American re-pricing carry, exactly as the de-Am q_eff
// bridge intends (S*e^{(r-q_eff)T} == F).
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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/american.hpp"        // AmericanGreeks (query return)
#include "atx/vol/calib.hpp"           // CalibOpts
#include "atx/vol/correction.hpp"      // CorrectionCache, AmericanCorrectionCaches
#include "atx/vol/curve.hpp"           // DividendEvent
#include "atx/vol/data.hpp"            // QuoteFrame (from_frame input)
#include "atx/vol/deamer.hpp"          // DeAmOptions
#include "atx/vol/parity.hpp"          // ParityReport
#include "atx/vol/priced_surface.hpp"  // PricedSurface, PricingContext (to_priced_surface)
#include "atx/vol/surface_parity.hpp"  // SliceContext, run_surface_parity
#include "atx/vol/types.hpp"           // Result, Side
#include "atx/vol/universe.hpp"        // Underlying (build input)
#include "atx/vol/vol_curve.hpp"       // CurveConfig, CurveSurface, VolCurveKind
#include "atx/vol/vol_surface.hpp"     // VolSurface

namespace atx::vol {

// Market/pricing snapshot a session is built from. Maps 1:1 onto
// `SurfaceParityInputs` when driving `run_surface_parity`; the same fields are
// retained so the const queries can re-price off the fitted surface.
struct SessionInputs {
  double S{0.0};                          // spot (> 0); the OpraPanel implied_spot when built from a frame
  double r{0.0};                          // continuously-compounded rate (finite)
  std::vector<DividendEvent> cash_divs;   // discrete cash-dividend schedule
  std::int64_t now_ts_ns{0};              // valuation timestamp (epoch ns)
  DeAmOptions deam{};                     // borrow-implication + pricer method / AL opts
  CalibOpts calib{};                      // per-slice curve-fit policy
  // Curve family to fit. Essvi (default) is byte-identical to the historical
  // eSSVI path (run_surface_parity, with calendar repair). ConvexDense / Svi fit
  // through the curve-agnostic driver (fit_curve_surface) and are served via the
  // session's polymorphic-surface override — this is how PricerFitter reaches the
  // 99.5%-in-band convex dense fit. The convex knobs live in curve.convex, the
  // eSSVI/SVI knobs in curve.parametric (calib mirrors curve.parametric for the
  // default path).
  CurveConfig curve{VolCurveKind::Essvi};
  double band_k{1.0};                     // minimum-edge band multiplier (parity)
  // Build a per-side Chebyshev correction cache over the chain's (k, T, sigma)
  // box and route every American inversion / re-pricing through the fast cached
  // pricer (Black-76 + correction). ON by default (the SOTA hot path); the
  // round-trip stays self-consistent because the same cache prices both legs.
  // Falls back to the cold Andersen-Lake path automatically if a cache fails to
  // build. Set false to force the reference cold path (e.g. for a cold-vs-cached
  // benchmark).
  bool use_correction_cache{true};
  // Post-assembly calendar-arbitrage repair (see surface_parity.hpp
  // CalendarRepair). None (default) checks only — the raw independent-per-slice
  // surface may cross in the wings. Project makes the produced surface
  // calendar-arb-free (Vola's headline property): a backbone theta-bump on any
  // slice whose wing crosses, with the per-expiry parity then scored off the
  // repaired surface so the reported quality is what the surface serves.
  CalendarRepair calendar_repair{CalendarRepair::None};
};

// ── Named calibration presets ─────────────────────────────────────────────
//
// A single high-level choice that bundles the whole fit policy (Andersen-Lake
// preset + IV-inversion tolerance, borrow ATM-pair count, correction cache, and
// calendar-arb repair) into one name, so a caller need not hand-assemble the
// DeAmOptions / CalibOpts / cache / repair knobs and know which combination is
// coherent. `apply_fit_preset` sets ONLY those policy fields, preserving the
// market snapshot (S, r, cash_divs, now_ts_ns) the caller has already filled.
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
  // Low-latency: Fast + calendar repair + cache; a fast arb-free build feeding
  // the cheap cached query hot path. Same policy as Robust today; named
  // separately so the HFT query path can diverge as it grows (Sprint 4).
  Hft = 3,
};

// Populate the fit-policy fields of `in` for `preset` (Andersen-Lake opts,
// iv_tol, n_atm, use_correction_cache, calendar_repair), leaving the market
// snapshot fields untouched. Idempotent; safe to call before `build`/`from_frame`.
void apply_fit_preset(SessionInputs& in, FitPreset preset) noexcept;

// Convenience: a SessionInputs preconfigured for `preset` with the market
// snapshot filled. Equivalent to setting the four snapshot fields then calling
// `apply_fit_preset`.
[[nodiscard]] SessionInputs make_session_inputs(FitPreset preset, double S,
                                                double r,
                                                std::int64_t now_ts_ns = 0);

// Aggregate surface-quality summary, distilled from the per-expiry parity
// reports and the per-slice context at build time.
struct SessionDiagnostics {
  double worst_frac_within_bidask{0.0};   // min over expiries of frac in bid-ask
  double mean_frac_within_bidask{0.0};    // mean over expiries
  double mean_chi2_reduced{0.0};          // mean reduced chi-square (vol space)
  double mean_rmse_vol{0.0};              // mean RMSE(model vol - mkt vol)
  bool   calendar_arb_free{false};        // surface calendar no-arb check
  std::size_t n_calendar_viol_pre{0};     // calendar violations BEFORE any repair
  std::size_t n_slices{0};                // fitted slice count
  std::size_t n_quotes{0};                // sum of per-slice n_used
};

// Stateful surface handle. Construct with `build` / `from_frame`; then query.
class VolaSession {
 public:
  // Move-only: the fitted surface is heavy state, not a value to copy. Declaring
  // the moves implicitly deletes the copy operations (that is intentional).
  VolaSession(VolaSession&&) noexcept = default;
  VolaSession& operator=(VolaSession&&) noexcept = default;

  // ── Construction ─────────────────────────────────────────────────────────

  // Build from an already-installed `Underlying`. Drives `run_surface_parity`
  // (S <= 0 / non-finite r => InvalidArgument; no chains or no usable slice =>
  // NotFound) and retains its fitted surface, per-slice context, per-expiry
  // parity, and pricing inputs. Any parity-harness error is propagated.
  [[nodiscard]] static Result<VolaSession> build(const Underlying& under,
                                                 const SessionInputs& in);

  // Build directly from an in-memory quote frame: install it into a local
  // `Universe` (`data_install`), resolve the resulting `Underlying`, then
  // `build`. Propagates any install / build error.
  [[nodiscard]] static Result<VolaSession> from_frame(const QuoteFrame& frame,
                                                      const SessionInputs& in);

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
  [[nodiscard]] Result<AmericanGreeks> greeks(double K, double T,
                                              Side side) const;

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
  [[nodiscard]] Status fair_value_ladder(double T,
                                         std::span<const double> strikes,
                                         std::span<const Side> sides,
                                         std::span<double> out) const;

  // Greeks-ladder analogue of `fair_value_ladder`. `out[i]` receives the full
  // AmericanGreeks bundle; a per-strike failure leaves that slot value-
  // initialized with a NaN price (`out[i].price`). Same structural-error and
  // shared-context contract.
  [[nodiscard]] Status greeks_ladder(double T, std::span<const double> strikes,
                                     std::span<const Side> sides,
                                     std::span<AmericanGreeks> out) const;

  // ── Incremental update (tick-to-quote) ─────────────────────────────────────
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
  // @return InvalidArgument for an out-of-range `slice_idx`, empty `new_obs`, or
  //         a surface with no eSSVI slice at that index; the fit's own error
  //         (Unavailable on a degenerate slice) otherwise; else Ok(FitDiag).
  [[nodiscard]] Result<FitDiag> refit_slice(std::size_t slice_idx,
                                            std::span<const FitObs> new_obs);

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

  [[nodiscard]] const VolSurface& surface() const noexcept { return surface_; }

  // Per fitted slice, ascending T. Parallel to `parity()`.
  [[nodiscard]] std::span<const SliceContext> expiries() const noexcept {
    return ctx_;
  }

  // Per-expiry re-Americanized parity, ascending T (parallel to `expiries()`).
  [[nodiscard]] std::span<const ParityReport> parity() const noexcept {
    return parity_;
  }

  [[nodiscard]] const SessionDiagnostics& diagnostics() const noexcept {
    return diag_;
  }

  // ── Term carry accessors (the query re-pricing forward / effective yield) ──
  //
  // The interpolated term forward F(T) and effective carry q_eff(T) at an
  // arbitrary T — the SAME clamp-outside / linear-interp-between-slices mechanic
  // the const queries use to re-price (see the forward-interpolation note above).
  // Exposed so a batch / whole-chain evaluator can resolve the per-expiry carry
  // (e.g. to invert bid/ask to American IV on the fit's own carry) without a
  // refit. Both return 0 for a non-finite / non-positive T (no slice to locate).
  [[nodiscard]] double forward_at(double T) const noexcept;
  [[nodiscard]] double q_eff_at(double T) const noexcept;

  // The per-side correction caches this session built (null pointers where a side
  // is on the cold path). Lets a batch / whole-chain evaluator route its
  // American-IV inversions through the SAME cached hot path (`american_price_cached`
  // = Black-76 + Chebyshev correction) that `fair_value` uses — orders of magnitude
  // faster than a cold Andersen-Lake solve per residual, and self-consistent with
  // the session's own re-pricing.
  [[nodiscard]] AmericanCorrectionCaches correction_caches() const noexcept {
    return query_caches();
  }

 private:
  // The interpolated term forward and effective carry at a queried T.
  struct ForwardCarry {
    double forward{0.0};
    double q_eff{0.0};
  };

  // Constructed only via build/from_frame (VolSurface's default ctor is private,
  // so VolaSession is not default-constructible either). Takes ownership of the
  // (optional) per-side correction caches built during `build`, and the optional
  // polymorphic-surface override: empty for the default eSSVI path (queries read
  // `surface_`); populated for ConvexDense / Svi (queries read the override).
  VolaSession(VolSurface&& surface, std::vector<SliceContext>&& ctx,
              std::vector<ParityReport>&& parity, SessionInputs in,
              const SessionDiagnostics& diag,
              std::optional<CorrectionCache>&& corr_call,
              std::optional<CorrectionCache>&& corr_put,
              std::optional<CurveSurface>&& curve_override);

  // Non-owning per-side cache bundle for the query re-pricing (empty when the
  // caches were not built). Recomputed on each call so it survives a move.
  [[nodiscard]] AmericanCorrectionCaches query_caches() const noexcept {
    return AmericanCorrectionCaches{
        corr_call_ ? &*corr_call_ : nullptr,
        corr_put_ ? &*corr_put_ : nullptr};
  }

  // Locate T among the ascending slice T's and return the term forward / carry:
  // clamp to the endpoint slice outside [T_0, T_last]; linearly interpolate
  // between the two bracketing slices inside. Precondition: at least one slice
  // (guaranteed — build errors on an empty surface).
  [[nodiscard]] ForwardCarry interp_forward(double T) const noexcept;

  // Model-vol source, dispatching to the polymorphic override when present and to
  // the eSSVI VolSurface otherwise. Every query (iv / fair_value / greeks /
  // ladders) reads the model IV through here, so the convex/SVI surface flows
  // everywhere the eSSVI surface did with no other change.
  [[nodiscard]] double model_iv(double k_log, double T) const noexcept {
    return curve_override_ ? curve_override_->iv(k_log, T) : surface_.iv(k_log, T);
  }
  [[nodiscard]] double model_w(double k_log, double T) const noexcept {
    return curve_override_ ? curve_override_->w(k_log, T) : surface_.w(k_log, T);
  }

  // Correction cache to serve a query through, or nullptr for the cold (accurate)
  // Andersen-Lake path. The single-carry cache is a self-consistent DE-AM round-
  // trip surrogate; pricing an arbitrary MODEL IV through it is penny-inaccurate
  // on carry-distant expiries (its correction is baked at one representative
  // carry). So the high-accuracy override surface (ConvexDense) serves COLD — the
  // price the library produces then matches the fitted surface's ~99.5% board
  // accuracy — while the eSSVI default keeps the fast cached path unchanged
  // (byte-identical). Band-IV inversions in value_chain stay on the cache
  // regardless (they are diagnostic bands, and the cache is their forward map).
  [[nodiscard]] const CorrectionCache* served_cache(Side side) const noexcept {
    if (curve_override_.has_value()) {
      return nullptr;
    }
    const CorrectionCache* const cc = query_caches().for_side(side);
    return (cc != nullptr && cc->populated() && cc->side() == side) ? cc : nullptr;
  }

  VolSurface surface_;
  std::vector<SliceContext> ctx_;      // ascending T
  std::vector<ParityReport> parity_;   // ascending T (‖ ctx_)
  SessionInputs in_;
  SessionDiagnostics diag_;
  std::optional<CorrectionCache> corr_call_;  // empty => cold path for calls
  std::optional<CorrectionCache> corr_put_;   // empty => cold path for puts
  // Polymorphic-surface override (ConvexDense / Svi). Empty => default eSSVI path
  // (queries read surface_). Move-only, like the session itself.
  std::optional<CurveSurface> curve_override_;
};

}  // namespace atx::vol
