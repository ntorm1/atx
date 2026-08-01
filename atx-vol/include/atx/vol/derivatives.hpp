#pragma once

// Volatility derivatives — variance/vol swaps, the model-free variance strip,
// the Carr-Lee volatility strike, aged-trade dispatch, and the running
// realized-variance tracker.
//
// Ported from the C `ats-vol` library (ats_vol_derivatives.{h,c},
// ats_vol_var_strip.c, ats_vol_vol_carr_lee.c, ats_vol_realized_tracker.c —
// Sprint 22). The pricing layer sits on top of the fitted vol surface
// (atx/vol/surface.hpp), the curve set (atx/vol/curve.hpp), and the Black-76
// kernel (atx/vol/black76.hpp).
//
// What this port ships (matching the C's v22 first cut):
//   - Realized-variance tracker (RealizedTracker): a scalar state machine that
//     ingests spots and maintains the running Sigma r_i^2 and annualized
//     decimal variance for daily mark-to-market on aged swaps.
//   - Variance-swap fair strike via the model-free OTM option-strip formula
//     K_var(T) = (2/T) integral OTM(K) / (df K^2) dK (Demeterfi-Derman-Kamal-Zou
//     in log-strike form, composite Simpson).
//   - Volatility-swap fair strike via the Carr-Lee model-free straddle formula
//     K_vol(T) ~= sqrt(2 pi / T) * C_ATMF(T) / (F * df).
//   - Aged-trade dispatch: the variance leg blends accrued realized variance
//     with future implied variance under the standard
//     (n_done/n_total)*RV_done + (n_future/n_total)*K_var_future convention;
//     the vol-swap dispatch handles inception (n_done == 0) and at-expiry
//     (n_done == n_total).
//   - Capped variance swap (Task 4, engine RvDistributionProxy or Auto):
//     E[min(V,C)] for the blended variance V = a + b*W, modeling the future
//     leg W as lognormal with mean K_var_future and log-stdev xi*sqrt(T) (xi
//     from DerivConfig::vol_of_vol, explicit or auto-calibrated). The cap
//     option value b*E[(W-K_c)+] comes from the closed-form
//     atx::vol::detail::lognormal_call. A contract already accrued past the
//     cap (a >= C) prices deterministically -- pinned, no model, no strip.
//
// Reserved for follow-on work (return ErrorCode::NotImplemented, mirroring the
// C's ATS_VOL_ERR_UNSUPPORTED): capped volatility swaps, the RV
// distribution-affine / Monte-Carlo QE engines, and mid-life vol-swap dispatch
// (intermediate n_done).
//
// Conventions (unchanged from the C):
//   - Decimal variance internally: 0.04 <-> 20 vol <-> 400 variance points.
//   - Annualization defaults to 252 (equity trading days).
//   - Vol-swap notional is the product's "vega" — payoff per 1.00 of vol.
//
// Error channel: the C returned a negative-integer AtsVolStatus; this port
// routes expected failures through atx::core::Result<T> / Status. The C's
// NaN-as-not-estimated sentinel on DerivQuote::integration_error_est is
// retained verbatim so callers can gate on (x == x).
//
// Thread-safety: the pricing entries (var_swap_fair_strike,
// vol_swap_fair_strike, deriv_price) are stateless pure functions of a fixed
// surface + curve set — safe to call concurrently from any number of threads.
// RealizedTracker is "single thread (mutates internal state)": a daily update
// takes exclusive access.

#include <cstdint>
#include <span>

#include "atx/vol/curve.hpp"
#include "atx/vol/surface.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

// E6: used only by const-reference in the PricedSurface-native overloads below,
// so the heavy definition stays out of this header.
class PricedSurface;

// ── Enums ────────────────────────────────────────────────────────────────

// Product kind. CappedVolSwap is reserved (dispatch returns NotImplemented);
// CappedVarSwap is priced via the lognormal RV distribution model (Task 4).
enum class DerivKind : std::uint8_t {
  VarSwap = 1,
  VolSwap = 2,
  CappedVarSwap = 3,
  CappedVolSwap = 4,  // reserved
};

// Pricing engine selector. Values >= RvDistributionAffine are reserved;
// RvDistributionProxy is also reserved EXCEPT as the distribution-model
// dispatch target for DerivKind::CappedVarSwap (Task 4), which Auto routes to
// as well.
enum class DerivEngine : std::uint8_t {
  Auto = 0,
  StripLogContract = 1,
  VolCarrLee = 2,
  RvDistributionProxy = 3,   // CappedVarSwap only; reserved for every other kind
  RvDistributionAffine = 4,  // reserved
  McQe = 5,                  // reserved
};

// Integration/accuracy tier. Drives the default log-strike grid for the strip.
enum class DerivQuality : std::uint8_t {
  Fast = 1,
  Standard = 2,
  High = 3,
  Audit = 4,
};

// Discrete-monitoring correction for the future implied-variance leg.
enum class DerivDiscreteCorrection : std::uint8_t {
  None = 0,
  Diffusion1OverN = 1,
  FullMc = 2,  // reserved
};

// Marking convention. CBOE variance-future marking is reserved.
enum class DerivMarkingConvention : std::uint8_t {
  Otc = 1,
  CboeVarianceFuture = 2,  // reserved
};

// Provenance / diagnostic bitmask carried on DerivQuote::flags (mirrors
// AtsVolDerivFlags exactly).
enum class DerivFlags : std::uint32_t {
  None = 0u,
  Aged = 1u << 0,
  FullyAged = 1u << 1,
  ModelProxy = 1u << 2,
  StripTruncatedLeft = 1u << 3,
  StripTruncatedRight = 1u << 4,
  VolCarrLee = 1u << 5,
  DiscreteCorrApplied = 1u << 6,
  LowT = 1u << 7,
  // Set when a discount factor could not be resolved at the contract maturity
  // and df = 1.0 was substituted (typical case: T == 0 at expiry). Callers
  // using PV must check for this.
  DfFallback = 1u << 8,
  // Set when DerivConfig::vol_of_vol == 0 (auto-calibrate) and the auto path
  // produced xi -- including the degenerate xi = 0 outcome (no Carr-Lee
  // convexity on this surface/tenor). NOT set on an explicit cfg.vol_of_vol,
  // and not set when no distribution model ran at all (this task ships the
  // knob + resolver; Tasks 4-6 are the first callers that can raise it).
  VolOfVolCalibrated = 1u << 9,
  // Set when a capped product's cap option value was actually subtracted
  // from the uncapped expectation (Task 4: the distribution-model path, or
  // the pinned deterministic path). NOT set when the cap cannot bind at all
  // (e.g. the fully-aged deterministic leg with accrued < cap -- there the
  // capped and uncapped answers are identical by construction).
  CapApplied = 1u << 10,
  // Set when the accrued leg alone already reached or exceeded the cap
  // (w_done*rv_done_dec >= cap_dec): the quote is pinned at
  // df*N*(cap_dec - strike_dec) with no strip call and no T > 0 requirement
  // (works at expiry). Always accompanied by CapApplied.
  CapPinned = 1u << 11,
};

[[nodiscard]] constexpr DerivFlags operator|(DerivFlags a, DerivFlags b) noexcept {
  return static_cast<DerivFlags>(static_cast<std::uint32_t>(a) |
                                 static_cast<std::uint32_t>(b));
}
[[nodiscard]] constexpr DerivFlags operator&(DerivFlags a, DerivFlags b) noexcept {
  return static_cast<DerivFlags>(static_cast<std::uint32_t>(a) &
                                 static_cast<std::uint32_t>(b));
}
constexpr DerivFlags& operator|=(DerivFlags& a, DerivFlags b) noexcept {
  a = a | b;
  return a;
}
[[nodiscard]] constexpr bool has_flag(DerivFlags value, DerivFlags flag) noexcept {
  return (value & flag) != DerivFlags::None;
}

// ── Realized variance state ──────────────────────────────────────────────

// Snapshot of the running realized-variance accumulator. Suitable for direct
// use as an immutable contract field or as a copy-out of a RealizedTracker
// (AtsRealizedVarianceSpec in the C).
struct RealizedVarianceSpec {
  double annualization = 252.0;               // default 252 trading days
  std::uint32_t n_obs_total = 0;              // contract observation count
  std::uint32_t n_obs_done = 0;               // observations realized so far
  double sum_sq_log_returns_done = 0.0;       // raw running Sigma r_i^2
  double rv_done_dec = 0.0;                    // annualized decimal variance to date
  bool include_dividend_adjustment = false;   // reserved; unused in this port
};

// Mutable realized-variance accumulator. Caller owns; not thread-safe.
//
// Constructed through create() so the annualization / observation-count
// invariants are validated once at the boundary (the C's *_init returned
// ATS_VOL_ERR_INVALID for the same preconditions). Rule of Zero.
class RealizedTracker {
public:
  // Build a tracker. @return InvalidArgument if annualization <= 0 or
  // n_obs_total == 0 (mirrors ats_vol_realized_tracker_init).
  [[nodiscard]] static Result<RealizedTracker> create(double annualization,
                                                       std::uint32_t n_obs_total);

  // Observe a single spot. The first call records the seed (no return
  // computed); each subsequent call updates Sigma r^2, n_done, and rv_done_dec.
  //
  // @return InvalidArgument for spot <= 0, or when all n_obs_total returns have
  //         already been observed.
  [[nodiscard]] Status observe(double spot);

  // Feed spots in order; stops early on the first invalid spot and propagates
  // its error (mirrors ats_vol_realized_tracker_observe_batch).
  [[nodiscard]] Status observe_batch(std::span<const double> spots);

  // Immutable spec view for use as a contract field (returned by value; the C
  // copied it out).
  [[nodiscard]] RealizedVarianceSpec snapshot() const noexcept { return rv_; }

  // Last observed spot (0.0 before the first observe). Exposed for callers
  // driving a spot path who need the running previous mark.
  [[nodiscard]] double prev_spot() const noexcept { return prev_spot_; }
  [[nodiscard]] bool have_prev() const noexcept { return have_prev_; }

private:
  RealizedTracker() = default;  // via create()

  double prev_spot_ = 0.0;
  bool have_prev_ = false;
  RealizedVarianceSpec rv_{};
};

// ── Contract / config / quote ────────────────────────────────────────────

// A vol-derivative contract (AtsVolDerivContract). `cap_dec` activates for
// CappedVarSwap (annualized decimal VARIANCE cap, e.g. (2.5*0.20)^2 = 0.25):
// deriv_price requires cap_dec > 0 on a capped kind (else InvalidArgument)
// and rejects a non-zero cap_dec on an uncapped kind (also InvalidArgument).
struct DerivContract {
  DerivKind kind = DerivKind::VarSwap;
  double maturity_t = 0.0;   // years until expiry
  double strike_dec = 0.0;   // K_var or K_vol
  double cap_dec = 0.0;      // capped kinds only; see above
  double notional = 0.0;     // N_var or N_vol
  RealizedVarianceSpec rv_spec{};
  DerivMarkingConvention marking = DerivMarkingConvention::Otc;
};

// Pricing configuration (AtsVolDerivConfig). The reserved fields
// (abs_price_tol / rel_price_tol / flags_request) must be left at 0; a
// non-zero value returns NotImplemented from every derivatives entry, so a
// forward-looking caller cannot silently depend on a knob the engine ignores.
struct DerivConfig {
  DerivEngine engine = DerivEngine::Auto;
  DerivQuality quality = DerivQuality::Standard;
  DerivDiscreteCorrection discrete_correction_mode = DerivDiscreteCorrection::None;
  double k_min_log = 0.0;         // log-strike grid lower (0 -> quality default)
  double k_max_log = 0.0;         // log-strike grid upper (0 -> quality default)
  std::uint32_t strip_nodes = 0;  // 0 -> quality default
  // E2 / AN-P1-2 adaptive wing width, in σ√T units — the same policy knob
  // `RndConfig::width_sigmas` has always had on the density route (FIX-E M-6:
  // E2 changed THIS route's span policy without giving it the knob).
  //
  //   0        -> the shared default, strip::kDefaultWidthSigmas = 6.
  //   > 0      -> span floor widened to `width_sigmas·σ_atm·√T`, and the
  //               truncation flags measure against that same requirement.
  //   < 0      -> vol scaling OFF. The span stays exactly at the tier default
  //               (or at an explicit [k_min_log, k_max_log]) AND the wings are
  //               no longer judged against a vol-scaled requirement. This is the
  //               escape hatch for a caller who genuinely wants an
  //               exactly-specified strip: before it existed, pinning the bounds
  //               got you the strip you asked for but permanently flagged it
  //               truncated.
  double width_sigmas = 0.0;
  // Annualized lognormal vol of the FUTURE realized-variance leg (the
  // "vol-of-vol" driving the RV distribution models Tasks 4-6 add: capped
  // swaps and mid-life vol-swap dispatch need a distribution over the future
  // variance, not just its mean, and this is the one free parameter of the
  // lognormal RV model those engines assume).
  //
  //   0    -> auto-calibrate from the surface's OWN Carr-Lee convexity at the
  //           contract tenor: pick xi so the lognormal E[sqrt(W)] reproduces
  //           the Carr-Lee K_vol exactly (see resolve_vol_of_vol,
  //           derivatives.cpp anon namespace). No convexity on the surface ->
  //           xi = 0 (RV collapses to its own mean, i.e. no vol-of-vol).
  //   > 0  -> used as-is; the caller's own calibration wins.
  //   < 0  -> InvalidArgument (a vol-of-vol cannot be negative).
  double vol_of_vol = 0.0;
  // Reserved — must be left at 0.
  double abs_price_tol = 0.0;
  double rel_price_tol = 0.0;
  std::uint32_t flags_request = 0;
};

// The default config: STANDARD quality, AUTO engine, no discrete correction,
// OTC marking (ats_vol_deriv_default_config).
[[nodiscard]] inline DerivConfig deriv_default_config() noexcept {
  return DerivConfig{};
}

// Pricing result (AtsVolDerivQuote).
struct DerivQuote {
  double fair_strike_dec = 0.0;           // K_var or K_vol
  double fair_strike_points = 0.0;        // var pts or vol pts
  double pv = 0.0;                        // contract PV today
  double undiscounted_expectation_dec = 0.0;
  double uncapped_var_dec = 0.0;          // populated when the strip ran
  double accrued_component_dec = 0.0;     // RV_done * n_done/n_total
  double future_component_dec = 0.0;      // K_var_future * n_future/n_total
  double convexity_adjustment_dec = 0.0;  // sqrt(K_var) - K_vol (vol swap only)
  // Strip-engine error estimate; NaN = not estimated (see file header).
  // `var_swap_fair_strike` populates it via a Richardson half-grid estimate
  // (|I_h - I_2h|/15) whenever the strip's node count is 4m+1 — every quality
  // tier default, and the adaptive-wing rescale, land there; a caller-pinned
  // `strip_nodes` that isn't 4m+1 leaves it NaN. Stays 0.0 on the paths the C
  // left at its memset zero (e.g. the standalone vol-swap Carr-Lee entry,
  // which runs no strip).
  double integration_error_est = 0.0;
  // The vol-of-vol actually used to price this quote (DerivConfig::vol_of_vol
  // resolved: the explicit value, or the auto-calibrated xi). NaN, not 0 --
  // this task adds the config knob and the resolver but wires no distribution
  // model into a pricing path yet, so every quote built by THIS task's code
  // leaves it at the struct default (kQuietNaN, curve.hpp) and a caller can
  // gate on (x == x) exactly as with integration_error_est above. Tasks 4-6
  // populate a real value once a distribution model actually runs.
  double vol_of_vol_used = kQuietNaN;
  // Cap option value b*E[(W-K_c)+] subtracted from the uncapped expectation
  // to get E[min(V,C)] (Task 4). 0.0 for uncapped kinds and for a capped
  // quote where the cap cannot bind (fully-aged, accrued < cap); NEVER NaN --
  // unlike vol_of_vol_used, a caller should not have to gate on (x == x) to
  // read this one.
  double cap_option_value_dec = 0.0;
  DerivFlags flags = DerivFlags::None;
};

// ── Unit conversions ─────────────────────────────────────────────────────

[[nodiscard]] constexpr double var_dec_to_points(double var_dec) noexcept {
  return 1.0e4 * var_dec;
}
[[nodiscard]] constexpr double var_points_to_dec(double var_points) noexcept {
  return 1.0e-4 * var_points;
}
[[nodiscard]] constexpr double vol_dec_to_points(double vol_dec) noexcept {
  return 1.0e2 * vol_dec;
}
[[nodiscard]] constexpr double vol_points_to_dec(double vol_points) noexcept {
  return 1.0e-2 * vol_points;
}

// ── Fair-strike resolvers ────────────────────────────────────────────────

// Variance-swap fair strike via OTM option-strip integration (engine
// STRIP_LOG_CONTRACT). Pure future expectation; ignores any rv_spec accrual.
//
// `SurfaceT` is the fitted surface (EssviSurface or SviSurface); only its
// `iv(k_log, T)` query is used. The forward is resolved from `curves.forward`
// (linear interpolation in T, clamped) and the discount factor from
// `curves.yield`.
//
// @return InvalidArgument for T <= 0; NotImplemented if a reserved config
//         field is non-zero; OutOfRange if the forward/discount cannot be
//         resolved (F <= 0 or df <= 0).
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote>
var_swap_fair_strike(const SurfaceT& surface, const CurveSet& curves, double T,
                     const DerivConfig& cfg = DerivConfig{});

// Volatility-swap fair strike via the Carr-Lee model-free straddle formula
// (engine VOL_CARR_LEE). Same error contract as var_swap_fair_strike; also
// returns OutOfRange if the ATMF implied vol is non-finite or non-positive.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote>
vol_swap_fair_strike(const SurfaceT& surface, const CurveSet& curves, double T,
                     const DerivConfig& cfg = DerivConfig{});

// ── Unified product price (handles aged + dispatch) ──────────────────────

// Price any vol-derivative contract, blending accrued realized variance with
// the future implied leg under the standard aged convention.
//
// Variance-swap dispatch handles all three age regimes through the linear
// variance blend. Vol-swap dispatch supports n_done == 0 (pure future leg,
// Carr-Lee) and n_done == n_total (pure realized leg, sqrt(rv_done_dec));
// intermediate n_done returns NotImplemented (the unbiased mid-life
// expectation needs a distribution engine this port does not ship).
// CappedVarSwap is priced via the lognormal RV distribution model (Task 4,
// engine Auto or RvDistributionProxy only -- StripLogContract/VolCarrLee on
// a capped kind return InvalidArgument). CappedVolSwap still returns
// NotImplemented.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote>
deriv_price(const SurfaceT& surface, const CurveSet& curves,
            const DerivContract& contract, const DerivConfig& cfg = DerivConfig{});

// Only the two shipped surface parametrizations are instantiated (mirrors
// surface.cpp). Keeps the template bodies out of this header.
extern template Result<DerivQuote> var_swap_fair_strike<EssviSurface>(
    const EssviSurface&, const CurveSet&, double, const DerivConfig&);
extern template Result<DerivQuote> var_swap_fair_strike<SviSurface>(
    const SviSurface&, const CurveSet&, double, const DerivConfig&);
extern template Result<DerivQuote> vol_swap_fair_strike<EssviSurface>(
    const EssviSurface&, const CurveSet&, double, const DerivConfig&);
extern template Result<DerivQuote> vol_swap_fair_strike<SviSurface>(
    const SviSurface&, const CurveSet&, double, const DerivConfig&);
extern template Result<DerivQuote> deriv_price<EssviSurface>(
    const EssviSurface&, const CurveSet&, const DerivContract&, const DerivConfig&);
extern template Result<DerivQuote> deriv_price<SviSurface>(
    const SviSurface&, const CurveSet&, const DerivContract&, const DerivConfig&);

// ── E6 / AN-W: PricedSurface-native entry points ────────────────────────────
//
// The templates above are stranded on the LEGACY calibration-grade surface types
// (`EssviSurface` / `SviSurface`, surface.hpp). The modern fitted pipeline
// produces a `PricedSurface`, so reaching `var_swap_fair_strike` from it meant
// hand-converting slices — which is why this whole module was reachable only
// from its own unit test.
//
// These overloads take a `PricedSurface` and NO `CurveSet`: the surface already
// carries its own per-expiry forwards and discount factors, and using them is
// the only way the strip's k = 0 is the surface's OWN ATM forward. The carry is
// read off the fitted pillars (`context()` forwards, `rate_at`) and interpolated
// between them by the same shared convention the strip integrates under
// (`strip_grid.hpp`, E2).
//
// FITTED-RANGE ONLY. `T` must lie within `[context().front().T,
// context().back().T]`; outside it these return `OutOfRange`. This is a real
// restriction and it is deliberate: past the end pillars the strip's forward
// clamps flat while `PricedSurface::forward_at` keeps extrapolating
// economically, so the two would disagree and bias K_var with no signal. A
// caller who genuinely wants an extrapolated tenor supplies its own `CurveSet`
// through the templated overload above and owns that choice explicitly.
//
// Numeric behaviour is otherwise unchanged: identical grid, identical adaptive
// span, identical Simpson quadrature, identical flags.
//
// @return the same error contract as the templated overloads, plus
//         InvalidArgument when the surface carries no usable fitted pillar and
//         OutOfRange when `T` falls outside the fitted pillar range.
[[nodiscard]] Result<DerivQuote> var_swap_fair_strike(const PricedSurface& surface, double T,
                                                      const DerivConfig& cfg = DerivConfig{});

[[nodiscard]] Result<DerivQuote> vol_swap_fair_strike(const PricedSurface& surface, double T,
                                                      const DerivConfig& cfg = DerivConfig{});

[[nodiscard]] Result<DerivQuote> deriv_price(const PricedSurface& surface,
                                             const DerivContract& contract,
                                             const DerivConfig& cfg = DerivConfig{});

}  // namespace atx::vol
