#pragma once

// Volatility derivatives — variance/vol swaps, the model-free variance strip,
// the Carr-Lee volatility strike, aged-trade dispatch, and the running
// realized-variance tracker.
//
// Ported from the C `ats-vol` library (ats_vol_derivatives.{h,c},
// ats_vol_var_strip.c, ats_vol_vol_carr_lee.c, ats_vol_realized_tracker.c —
// Sprint 22). The pricing layer sits on top of a fitted vol surface (any type
// answering `iv(k_log, T)`, or a `PricedSurface` through the E6 overloads), the
// curve set (atx/vol/rates_curve.hpp), and the Black-76 kernel
// (atx/vol/black76.hpp).
//
// What this port ships (well past the C's v22 first cut -- the production
// sprint below added the distribution engine, greeks, dated fixings and the
// Richardson error estimate on top of it):
//   - Realized-variance tracker (RealizedTracker): a scalar state machine that
//     ingests spots and maintains the running Sigma r_i^2 and annualized
//     decimal variance for daily mark-to-market on aged swaps.
//   - Dated, idempotent fixings (RealizedTracker::observe_dated, Task 8): a
//     timestamped observe for the backtest engine's daily-fixing driver.
//     Enforces STRICTLY ASCENDING fixing timestamps -- a stale or replayed
//     ts_ns returns AlreadyExists and mutates nothing, including
//     last_fixing_ts_ns() -- so a re-delivered snapshot can never double-count
//     a fixing.
//   - Variance-swap fair strike via the model-free OTM option-strip formula
//     K_var(T) = (2/T) integral OTM(K) / (df K^2) dK (Demeterfi-Derman-Kamal-Zou
//     in log-strike form, composite Simpson), with a Richardson half-grid
//     quadrature-error estimate (Task 1: |I_h - I_2h|/15, populating
//     DerivQuote::integration_error_est) whenever the resolved node count is
//     4m+1 -- every quality tier default and the E2 adaptive-wing rescale land
//     there. The exact resolved log-strike grid is also recorded
//     (strip_k_lo_used / strip_k_hi_used / strip_nodes_used) so a caller, or
//     deriv_greeks' own bump pinning, can reproduce it exactly.
//   - Volatility-swap fair strike via the Carr-Lee model-free straddle formula
//     K_vol(T) ~= sqrt(2 pi / T) * C_ATMF(T) / (F * df) (DerivConfig::
//     carr_lee_form == Naive, the v1.1 default), or the Remark 6.4/6.5
//     convexity refinement against the strip's own K_var (Task C-5,
//     carr_lee_form == Refined -- planned 2.0 default).
//   - Aged-trade dispatch: the variance leg blends accrued realized variance
//     with future implied variance under the standard
//     (n_done/n_total)*RV_done + (n_future/n_total)*K_var_future convention;
//     the vol-swap dispatch handles inception (n_done == 0) and at-expiry
//     (n_done == n_total).
//   - Lognormal RV distribution engine (Task 2, detail/rv_lognormal.hpp:
//     Gauss-Hermite for smooth payoffs, a split-domain Gauss-Legendre rule for
//     kinked ones) plus a vol-of-vol knob (Task 3, DerivConfig::vol_of_vol):
//     the future realized-variance leg is modeled as lognormal with mean
//     K_var_future and log-stdev xi*sqrt(T), xi either the caller's own number
//     or auto-calibrated so the lognormal's E[sqrt(W)] reproduces the
//     surface's own Carr-Lee K_vol exactly (DerivQuote::vol_of_vol_used,
//     DerivFlags::VolOfVolCalibrated). This is the one shared foundation the
//     three distribution-model products below all price against.
//   - Capped variance swap (Task 4, engine RvDistributionProxy or Auto):
//     E[min(V,C)] for the blended variance V = a + b*W, modeling the future
//     leg W as lognormal with mean K_var_future and log-stdev xi*sqrt(T) (xi
//     from DerivConfig::vol_of_vol, explicit or auto-calibrated). The cap
//     option value b*E[(W-K_c)+] comes from the closed-form
//     atx::vol::detail::lognormal_call. A contract already accrued past the
//     cap (a >= C) prices deterministically -- pinned, no model, no strip.
//   - Capped volatility swap (Task 5, engine RvDistributionProxy or Auto):
//     E[min(sqrt V, c)] for the same blended variance V = a + b*W, c a
//     decimal VOL cap (C = c^2 its variance-units image). min(sqrt V, c) is
//     KINKED in W, so unlike the capped variance swap this is not a
//     closed-form call: the domain is split at the kink's standard-normal
//     coordinate z*, the smooth piece integrated by
//     atx::vol::detail::lognormal_truncated_expect (GL-64), and the tail
//     probability above the kink closed analytically via
//     atx::vol::detail::norm_cdf. Same pin/fully-aged/model-path structure as
//     the capped variance swap.
//   - Mid-life vol-swap dispatch (Task 6, engine Auto or RvDistributionProxy):
//     E[sqrt(a + b*W)] for the intermediate age regime (0 < n_done <
//     n_total), a = w_done*rv_done_dec, b = w_future, W lognormal at the
//     strip's own mean and log-stdev xi*sqrt(T). sqrt(a+b*W) is SMOOTH (no
//     kink, a/b >= 0), so this is priced by plain Gauss-Hermite
//     (atx::vol::detail::lognormal_expect), not the capped pricers'
//     split-domain quadrature. An explicit VolCarrLee engine on a mid-life
//     contract is InvalidArgument (Carr-Lee cannot blend an accrued leg); an
//     explicit RvDistributionProxy on an UNAGED contract runs this same
//     formula end to end (a = 0, b = 1) instead of Carr-Lee; fully-aged is
//     unaffected by engine (exact, no model needed).
//   - Finite-difference greeks for every kind (Task 7, deriv_greeks): delta /
//     gamma / vega / volga / vanna / theta / rho / charm, each bump repriced
//     through deriv_price itself so a product's greeks and its mark can never
//     come from two different engines. Spot bumps are sticky-strike (forwards
//     scale, the surface is re-read at the original absolute strike); an
//     auto-calibrated vol-of-vol is resolved once at the center and pinned
//     into the bumps; fully-aged contracts skip bumping entirely.
//
// Reserved for follow-on work. Two flavors:
//   - ACTIVELY REJECTED (return ErrorCode::NotImplemented, mirroring the C's
//     ATS_VOL_ERR_UNSUPPORTED): the RV distribution-affine / Monte-Carlo QE
//     pricing engines (DerivEngine::RvDistributionAffine / McQe), and the
//     discrete-monitoring full-Monte-Carlo correction
//     (DerivDiscreteCorrection::FullMc) -- checked up front by every pricer,
//     regardless of aging state.
//   - DECLARED, UNENFORCED: DerivMarkingConvention::CboeVarianceFuture. The
//     enum value and the DerivContract::marking field it lives on both exist,
//     but no pricing path reads `marking` yet -- a CboeVarianceFuture contract
//     prices identically to an Otc one today rather than failing loud. A
//     caller relying on CBOE variance-future conventions must not assume this
//     field does anything yet.
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
// vol_swap_fair_strike, deriv_price, deriv_greeks) are stateless pure functions
// of a fixed surface + curve set — safe to call concurrently from any number of
// threads. `deriv_greeks` builds its bumped curve sets as function-local copies,
// so it never writes through the caller's CurveSet.
// RealizedTracker is "single thread (mutates internal state)": a daily update
// takes exclusive access.

#include <cstdint>
#include <limits>
#include <span>

#include "atx/vol/detail/aggregate_arity.hpp" // DerivConfig field-count drift pin
#include "atx/vol/rates_curve.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

// E6: used only by const-reference in the PricedSurface-native overloads below,
// so the heavy definition stays out of this header.
class PricedSurface;

// ── Enums ────────────────────────────────────────────────────────────────

// Product kind. CappedVarSwap is priced via the lognormal RV distribution
// model (Task 4); CappedVolSwap via the same model's split-domain quadrature
// (Task 5).
enum class DerivKind : std::uint8_t {
  VarSwap = 1,
  VolSwap = 2,
  CappedVarSwap = 3,
  CappedVolSwap = 4,
};

// Pricing engine selector. Values >= RvDistributionAffine are reserved;
// RvDistributionProxy is also reserved EXCEPT as the distribution-model
// dispatch target for DerivKind::CappedVarSwap (Task 4),
// DerivKind::CappedVolSwap (Task 5), and DerivKind::VolSwap (Task 6 --
// mid-life always, plus an unaged contract priced end to end through the
// model instead of Carr-Lee), all of which Auto also routes to as well.
enum class DerivEngine : std::uint8_t {
  Auto = 0,
  StripLogContract = 1,
  VolCarrLee = 2,
  RvDistributionProxy = 3,   // CappedVarSwap/CappedVolSwap/VolSwap only; reserved otherwise
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
  // Broadie-Jain (2008) diffusion-drift term: K_var_future +=
  // (T_resid/n_remaining) * (r_bar - q_bar - K_var_future/2)^2, additive and
  // keyed off the FUTURE leg's own remaining fixing count (n_remaining =
  // n_obs_total - n_obs_done), NOT the contract's total observation count.
  // Magnitude is a fraction of a variance point for a daily-monitored
  // (n ~ 252) contract at typical rate/carry differentials -- e.g. ~0.036 var
  // pts at sigma=20%, r-q=5%, T=1Y, n=252 (task-C-1-report.md). Does NOT cover
  // the residual O(1/n) JUMP term (Broadie-Jain sec 4); jump-diffusion
  // discrete-monitoring bias needs the FullMc engine below (reserved).
  Diffusion1OverN = 1,
  FullMc = 2,  // reserved
};

// Marking convention. CBOE variance-future marking is reserved.
enum class DerivMarkingConvention : std::uint8_t {
  Otc = 1,
  CboeVarianceFuture = 2,  // reserved
};

// Which Carr-Lee K_vol approximation the ATMF-straddle formula (and the
// vol-of-vol auto-calibration that inverts it, resolve_vol_of_vol) resolve
// against. Task C-5; see task-C-5-report.md for the from-paper derivation.
//
//   Naive    -> K_vol ~= sqrt(2 pi / T) * C_ATMF(T) / (F * df) (Carr & Lee
//               2009, "Robust Replication of Volatility Derivatives", Prop.
//               6.1 bound (a) / Remark 6.3) -- the ATMF-straddle
//               approximation the paper explicitly declines to endorse
//               (Remark 6.5). Under equity skew it is biased LOW (LIT-4:
//               >40 vol bp at 6M, Heston BCC calibration, the paper's Sec.
//               6.5 numerical example) because it reads only the ATMF vol
//               and never sees the rest of the smile. v1.1 DEFAULT, for
//               behavior compatibility with every quote struck before this
//               knob existed.
//   Refined  -> the Remark 6.4/6.5 convexity refinement, evaluated against
//               the variance strip's OWN K_var instead of a second naive
//               proxy. Recovers PART of the naive-vs-sqrt(K_var) convexity
//               gap without ever crossing the Jensen bound VOL0 <= VAR0
//               (Prop. 6.1(c)) in the regime this task's tests cover. Costs
//               one extra strip evaluation at the standalone Carr-Lee entry
//               (vol_swap_fair_strike) -- the distribution-model callers
//               (resolve_vol_of_vol's 3 call sites) already have the strip's
//               K_var in hand, so refining there is free. Planned 2.0
//               default; see CHANGELOG.md for the migration note.
enum class CarrLeeForm : std::uint8_t {
  Naive = 0,
  Refined = 1,
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
  // Set when the variance strip's grid spacing (dk) exceeds
  // sigma_atm*sqrt(T)/4 -- the resolution floor `var_swap_fair_strike`
  // enforces (C-2 / PV-2), mirroring the span policy's own vol-scaled
  // widening but for RESOLUTION rather than COVERAGE. Fires in exactly two
  // cases: (a) the floor had to raise the node count (an unpinned grid,
  // typically a short-tenor/low-vol quote whose tier default is coarser than
  // its own vol scale calls for), or (b) a caller-pinned `strip_nodes`
  // leaves the grid under-resolved -- a pin is never overridden (pin
  // semantics are load-bearing for deriv_greeks' grid pinning), so that case
  // is flagged instead of silently corrected. Absent whenever dk already
  // satisfies the floor, including every tier default at a long-enough tenor.
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
  // Set when the variance strip's resolved span extended beyond the wing trust
  // band (DerivConfig::wing_clamp_k) so tail nodes were read at the band-edge
  // vol rather than the surface's extrapolated wing. STRUCTURAL, not numeric:
  // it says "flat-vol tails were in effect", not "the value moved" — on a flat
  // smile the clamp moves nothing and the flag still fires. Absent whenever the
  // clamp is disabled (wing_clamp_k < 0) or the whole span fits inside the
  // band. atx extension: not mirrored in AtsVolDerivFlags.
  WingClamped = 1u << 12,
  // Set when the variance strip's quadrature read a non-finite or non-
  // positive surface IV at one or more nodes STRICTLY INSIDE the grid (PV-4)
  // -- as opposed to `StripTruncatedLeft`/`Right`, which cover only the two
  // ENDPOINT nodes of the whole span. An interior bad node still contributes
  // 0 to the integral (unchanged); this flag is purely informational,
  // recording that it happened. `var_swap_fair_strike` still returns a quote
  // whenever the count is at or below `max(2, n_nodes/100)` -- past that
  // threshold it returns `Internal` instead (see that function's doc): a
  // surface with that many holes across its middle is broken, not sparse.
  // atx extension: not mirrored in AtsVolDerivFlags.
  InteriorBadNodes = 1u << 13,
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

  // Timestamped observe for daily-fixing drivers (the backtest). Same accrual
  // arithmetic as observe(); additionally enforces STRICTLY ASCENDING fixing
  // timestamps so a re-delivered snapshot cannot double-count a fixing:
  // ts_ns <= last_fixing_ts_ns() returns AlreadyExists and mutates nothing.
  // Ordering is validated FIRST -- a rejected call (stale or replayed ts)
  // leaves every field (including last_fixing_ts_ns()) untouched, even when
  // the underlying observe(spot) would itself have failed.
  [[nodiscard]] Status observe_dated(std::int64_t ts_ns, double spot);

  // Timestamp of the last accepted observe_dated() call, or
  // numeric_limits<int64_t>::min() before the first one.
  [[nodiscard]] std::int64_t last_fixing_ts_ns() const noexcept { return last_fixing_ts_ns_; }

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
  std::int64_t last_fixing_ts_ns_ = std::numeric_limits<std::int64_t>::min();
};

// ── Contract / config / quote ────────────────────────────────────────────

// A vol-derivative contract (AtsVolDerivContract). `cap_dec` activates for
// CappedVarSwap (annualized decimal VARIANCE cap, e.g. (2.5*0.20)^2 = 0.25)
// and for CappedVolSwap (a decimal VOL cap instead, e.g. 2.5*0.20 = 0.50):
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
  // Which Carr-Lee K_vol approximation feeds the standalone vol-swap entry
  // and the vol-of-vol auto-calibration above (see CarrLeeForm). Naive is
  // the v1.1 default (behavior-compatible with every pre-C-5 quote);
  // Refined pulls in the strip's own K_var for a smaller convexity bias.
  CarrLeeForm carr_lee_form = CarrLeeForm::Naive;
  // Wing trust band for the variance strip's SURFACE READS, in absolute
  // log-forward-moneyness. The fit pipeline certifies a surface only on
  // |k| <= 0.5 (RiskSurfaceValidationConfig::k_min/k_max); beyond it a
  // parametric eSSVI/SVI slice serves an unbounded linear-in-|k| extrapolation
  // no quote ever disciplined, and the strip's 1/K weighting turns that
  // fiction into fair-strike level and daily mark noise (the sp100-2026 XOM
  // 3M strike read ~38 vol against a ~30 ATM, with ~98% of its day-to-day
  // variance sourced beyond |k| = 0.25). Nodes beyond the band keep their true
  // strikes but read the BAND-EDGE vol — flat-vol tails, the standard desk
  // discipline for un-quoted wings — so the strip stays complete (no
  // truncation bias) while the uncertified region loses its say. The span,
  // node count, and truncation flags are untouched: this clamps reads, not
  // the grid. `DerivFlags::WingClamped` records that tails were in effect.
  //
  //   0    -> the certified validation band, strip::kCertifiedWingHalfBand
  //           (= 0.5, kept equal to RiskSurfaceValidationConfig{}.k_max).
  //   > 0  -> explicit half-band; reads clamped to [-wing_clamp_k, +wing_clamp_k].
  //   < 0  -> OFF: read the raw surface everywhere (pre-clamp behavior; the
  //           escape hatch for a surface whose wings ARE quote-disciplined).
  //   NaN  -> InvalidArgument.
  double wing_clamp_k = 0.0;
  // Reserved — must be left at 0.
  double abs_price_tol = 0.0;
  double rel_price_tol = 0.0;
  std::uint32_t flags_request = 0;
};

// Drift pin: DerivConfig has exactly THIRTEEN fields (v1.1 appended
// carr_lee_form, task C-5). Adding, removing, or splitting one breaks this
// line -- update the count, and confirm every construction site still uses
// `DerivConfig{}` + designated field assignment (the only form used anywhere
// in this codebase today; there is no positional brace-init to protect).
static_assert(detail::aggregate_arity_is_v<DerivConfig, 13>,
              "DerivConfig field count changed: update this pin.");

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
  // The strip's variance in decimal variance units. ITS MEANING IS PER-KIND,
  // and the two readings are NOT interchangeable:
  //   * var-swap dispatch, the unaged vol swap's best-effort Carr-Lee
  //     diagnostic, and BOTH capped paths carry the strip's RAW FUTURE-LEG
  //     value K_var(T) -- no accrued leg, no discrete correction, no cap
  //     haircut. (`future_component_dec` beside it IS discrete-corrected and
  //     weight-scaled; these two deliberately differ.)
  //   * the MID-LIFE vol swap (the distribution model) carries the BLENDED
  //     TOTAL variance a + b*m that it actually prices sqrt() of: a =
  //     w_done*rv_done_dec, b = w_future, m the strip mean AFTER any
  //     Diffusion1OverN correction. That is the model's own input, and the
  //     number `convexity_adjustment_dec` beside it is formed from
  //     (sqrt(a+b*m) - fair_strike_dec).
  // 0.0 means NO STRIP RAN -- fully-aged legs, cap pins, and the standalone
  // Carr-Lee vol-strike entry -- never "the strip integrated to zero".
  double uncapped_var_dec = 0.0;
  double accrued_component_dec = 0.0;     // RV_done * n_done/n_total
  double future_component_dec = 0.0;      // K_var_future * n_future/n_total
  // sqrt(K_var) - K_vol (uncapped vol swap); sqrt(a+b*m) - fair_strike_dec
  // for a capped vol swap's model path (Task 5) -- the same "sqrt of the
  // blended-variance mean minus the priced vol strike" diagnostic, just with
  // the capped strike on the right. Left at the struct default 0.0 on paths
  // that never build a sqrt(E[V]) to compare against: the capped-var-swap
  // paths, and the capped-vol-swap pin/fully-aged paths -- those never form
  // the a+b*m blend (no strip runs), so sqrt(a+b*m) is simply not computed
  // there, not "computed and zero".
  double convexity_adjustment_dec = 0.0;
  // Strip-engine error estimate; NaN = not estimated (see file header).
  // `var_swap_fair_strike` populates it via a Richardson half-grid estimate
  // (|I_h - I_2h|/15) whenever the strip's node count is 4m+1 — every quality
  // tier default, and the adaptive-wing rescale, land there; a caller-pinned
  // `strip_nodes` that isn't 4m+1 leaves it NaN. Stays 0.0 on the paths the C
  // left at its memset zero (e.g. the standalone vol-swap Carr-Lee entry,
  // which runs no strip).
  double integration_error_est = 0.0;
  // The vol-of-vol actually used to price this quote: DerivConfig::vol_of_vol
  // resolved -- the caller's explicit xi, or the Carr-Lee auto-calibrated one
  // (flagged VolOfVolCalibrated). Populated by exactly the paths that run a
  // distribution model over the future variance: the mid-life vol swap and
  // both capped kinds' model branches.
  //
  // NaN, not 0, when NO distribution model ran -- the var-swap strip, the
  // unaged/fully-aged vol-swap branches, and the capped pin / fully-aged exits
  // all leave it at the struct default (kQuietNaN, curve.hpp). A caller gates
  // on (x == x) exactly as with integration_error_est above. 0.0 is a REAL
  // resolved value (a surface with no Carr-Lee convexity calibrates to xi = 0,
  // i.e. RV collapses to its own mean), never "not computed".
  double vol_of_vol_used = kQuietNaN;
  // Cap option value subtracted from the uncapped expectation to get the
  // capped one. Units follow the product: for CappedVarSwap (Task 4) this is
  // b*E[(W-K_c)+] in VARIANCE units, closed-form via
  // atx::vol::detail::lognormal_call; for CappedVolSwap (Task 5) it is
  // E[sqrt(V)] - E[min(sqrt(V),c)] in VOL units, the difference of two
  // lognormal_truncated_expect calls over the same [-8,8] domain (the capped
  // side additionally closes its tail analytically past the kink, so this is
  // NOT a same-nodes-exact identity — expect ~1e-9, not machine epsilon,
  // agreement against an independent smooth-integrand oracle). 0.0 for
  // uncapped kinds, for a capped quote where the cap cannot bind (fully-aged,
  // accrued < cap), AND for a pinned quote (accrued >= cap) -- the pin path
  // deliberately skips the strip, so the true haircut against the (unpriced)
  // uncapped expectation is never computed there; this is "not computed", not
  // "computed and zero". NEVER NaN -- unlike vol_of_vol_used, a caller should
  // not have to gate on (x == x) to read this one.
  double cap_option_value_dec = 0.0;
  // The EXACT log-strike grid the variance strip integrated on, as resolved
  // after the quality tier, any caller pin, and the E2 adaptive-wing rescale
  // have all had their say. Populated by `var_swap_fair_strike` whenever the
  // strip actually runs, and carried through every dispatch path that runs one;
  // left at NaN / 0 ("no strip ran") on the paths that never integrate --
  // fully-aged legs, cap pins, and the standalone Carr-Lee vol strike.
  //
  // These exist so a caller can REPRODUCE a quote's grid exactly by feeding
  // them back as DerivConfig::k_min_log / k_max_log / strip_nodes. That is what
  // `deriv_greeks` does: the adaptive rescale rounds its node count with a
  // ceil(), so a bumped surface can land on a DIFFERENT node count than the
  // center, and the finite differences would then straddle a step
  // discontinuity in the quadrature -- contaminating gamma/volga/vanna with an
  // artifact of the grid rather than a property of the price. Pinning the
  // center's grid into every bumped evaluation removes that failure mode by
  // construction.
  double strip_k_lo_used = kQuietNaN;
  double strip_k_hi_used = kQuietNaN;
  std::uint32_t strip_nodes_used = 0;
  DerivFlags flags = DerivFlags::None;
};

// ── Carr-Lee convexity refinement (detail) ─────────────────────────────────

namespace detail {

// Carr-Lee (Carr & Lee 2009, "Robust Replication of Volatility Derivatives",
// https://math.uchicago.edu/~rl/rrvd.pdf, Remark 6.4/6.5) convexity
// refinement of the naive ATMF-straddle K_vol approximation, adapted to this
// codebase's ANNUALIZED decimal convention. task-C-5-report.md has the full
// from-paper re-derivation; in short, the paper states Remark 6.4 for
// UN-annualized total-horizon quantities (IV0, VAR0, VOL0, each scaling like
// sigma*sqrt(T)) --
//
//   VOL0 ~= IV0 * (1 + (VAR0^2 - IV0^2) / (8 + 2*IV0^2))
//
// -- and naively dropping T when restating it for this codebase's annualized
// K_vol/K_var (a "summary fidelity" transcription error, not a paper error)
// silently assumes T == 1 always. Substituting IV0 = k_vol_naive*sqrt(T),
// VAR0^2 = k_var*T and dividing back through by sqrt(T) gives the
// annualization-consistent form actually implemented here:
//
//   K_vol_refined = k_vol_naive *
//       (1 + T*(k_var - k_vol_naive^2) / (8 + 2*T*k_vol_naive^2))
//
// which collapses to the (T-dropped) paraphrase exactly at T == 1 and to
// k_vol_naive exactly whenever k_var == k_vol_naive^2 (no convexity to
// recover -- CarrLee.RefinementVanishesOnFlat pins this).
//
// Review fix I-1 (C-5): k_vol_naive occupies Remark 6.5's IV0 slot, but the
// paper's IV0 is the ATM IMPLIED VOL (sigma_atmf) itself -- "a simple
// approximation using ATM implied volatility and the variance swap value."
// k_vol_naive (carr_lee_k_vol's output) is a SEPARATE, already-approximate
// stand-in for sigma_atmf: the ATMF-straddle closed form is
// g_hat(sigma_atmf*sqrt(T)) = sigma_atmf*sqrt(T) -
// (sigma_atmf*sqrt(T))^3/24 + O(sigma_atmf^5) (Taylor-expanding the erf-
// based straddle formula), so annualized, k_vol_naive is biased LOW
// relative to sigma_atmf by sigma_atmf^3*T/24 -- a SEPARATE, UN-corrected
// approximation layer this refinement does not touch. On this task's own
// T=0.5/sigma_atmf=0.20 fixture that residual is ~1.667 vol bp, ROUGHLY 1.8x
// LARGER than the +0.906 vol bp the refinement itself adds
// (task-C-5-report.md's measurement table): a Refined strike still lands
// strictly below sigma_atmf, let alone the paper's true VOL0. "Recovers
// part of the convexity gap" describes the K_var-vs-k_vol_naive^2 gap this
// formula targets, not the total distance from k_vol_naive to fair value.
//
// The substitution is FORCED, not an oversight: CarrLee.RefinementVanishes
// OnFlat pins refined == naive BIT-EXACT whenever k_var == k_vol_naive^2,
// which only holds with k_vol_naive itself (not sigma_atmf) as the base
// point -- substituting sigma_atmf would move Refined by ~1.7 vol bp even
// on a perfectly flat surface and break that pin. Fixing the proxy itself
// (feeding refine_carr_lee_k_vol a true ATM-implied-vol argument) is a
// different, un-scoped change: it would move the Naive default too, which
// the v1.1 behavior-compatibility contract forbids without a version bump.
//
// This is a LOCAL/leading-order approximation (the paper does not endorse
// it -- Remark 6.5), valid in the small-correction regime every intended
// caller here operates in; it is not a globally bound-respecting formula; a
// pathologically large T or skew could in principle push it past
// sqrt(k_var). resolve_vol_of_vol's existing ratio-in-[0,1) guard already
// absorbs that case (degrades to xi = 0, the same "no usable convexity"
// outcome an untestable input already produces) rather than propagating a
// bad value, so no additional clamp is added here.
//
// Precondition (caller-enforced, not checked -- an unconditionally noexcept
// leaf like this file's other detail:: primitives): T > 0; k_vol_naive and
// k_var finite and >= 0.
[[nodiscard]] double refine_carr_lee_k_vol(double k_vol_naive, double k_var,
                                           double T) noexcept;

}  // namespace detail

// ── Fair-strike resolvers ────────────────────────────────────────────────

// Variance-swap fair strike via OTM option-strip integration (engine
// STRIP_LOG_CONTRACT). Pure future expectation; ignores any rv_spec accrual.
//
// `SurfaceT` is any fitted-surface type answering `iv(k_log, T)` — that query is
// the whole requirement. The forward is resolved from `curves.forward`
// (linear interpolation in T, clamped) and the discount factor from
// `curves.yield`.
//
// @return InvalidArgument for T <= 0; NotImplemented if a reserved config
//         field is non-zero; OutOfRange if the forward/discount cannot be
//         resolved (F <= 0 or df <= 0); Internal if more than
//         max(2, n_nodes/100) nodes STRICTLY INSIDE the grid read a
//         non-finite/non-positive surface IV (PV-4) -- a handful is recorded
//         via DerivFlags::InteriorBadNodes and still priced, but a surface
//         with a mid-grid hole that wide is broken, not sparse.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote>
var_swap_fair_strike(const SurfaceT& surface, const CurveSet& curves, double T,
                     const DerivConfig& cfg = DerivConfig{});

// Volatility-swap fair strike via the Carr-Lee model-free straddle formula
// (engine VOL_CARR_LEE). Same error contract as var_swap_fair_strike; also
// returns OutOfRange if the ATMF implied vol is non-finite or non-positive.
//
// `cfg.carr_lee_form` (Task C-5, default Naive) selects the naive formula
// above or the Remark 6.4/6.5 convexity refinement (detail::
// refine_carr_lee_k_vol). Naive runs NO strip (integration_error_est stays
// NaN, uncapped_var_dec stays 0.0, matching every pre-C-5 caller exactly).
// Refined needs the strip's own K_var, so it pays for one var_swap_fair_
// strike evaluation and propagates that strip's error contract too (Internal
// on an unusably holey surface, etc.) -- an opt-in cost, never paid unless
// the caller asks for it.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote>
vol_swap_fair_strike(const SurfaceT& surface, const CurveSet& curves, double T,
                     const DerivConfig& cfg = DerivConfig{});

// ── Unified product price (handles aged + dispatch) ──────────────────────

// Price any vol-derivative contract, blending accrued realized variance with
// the future implied leg under the standard aged convention.
//
// Variance-swap dispatch handles all three age regimes through the linear
// variance blend. Vol-swap dispatch handles all three age regimes too:
// n_done == 0 (pure future leg, Carr-Lee by default) and n_done == n_total
// (pure realized leg, sqrt(rv_done_dec)) are exact/closed-form; intermediate
// n_done (Task 6) prices E[sqrt(a+b*W)] via the same lognormal RV
// distribution model as the capped swaps (a = w_done*rv_done_dec, b =
// w_future, W's mean from the strip at the residual maturity). An explicit
// VolCarrLee engine on a mid-life vol swap is InvalidArgument (Carr-Lee
// cannot blend an accrued leg); an explicit RvDistributionProxy on an unaged
// vol swap runs the distribution model end to end (a = 0, b = 1) instead of
// Carr-Lee, and on a fully-aged one is a no-op (the exact branch already has
// nothing for the model to add).
// CappedVarSwap and CappedVolSwap are both priced via the lognormal RV
// distribution model (Tasks 4/5, engine Auto or RvDistributionProxy only --
// StripLogContract/VolCarrLee on a capped kind return InvalidArgument):
// CappedVarSwap in closed form (b*E[(W-K_c)+]), CappedVolSwap by split-domain
// quadrature (E[min(sqrt(V),c)] is kinked, so no closed form applies).
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote>
deriv_price(const SurfaceT& surface, const CurveSet& curves,
            const DerivContract& contract, const DerivConfig& cfg = DerivConfig{});

// The three templates above have NO definition in this header — the bodies live
// in derivatives.cpp, which explicitly instantiates them for a fixed supported
// set. `VolSurface` is the Tier-A member of that set, and the one to reach for:
// it is the calibration-grade surface container this library's own arbitrage
// validators, projection spine and archive are written against, and it answers
// `iv(k_log, T)`, which is this template's entire requirement. The set also
// carries the two per-family containers demoted to `detail/` by S4-T21, for
// source compatibility with callers that predate the demotion — being reachable
// through a Tier-A template does not promote them, and Tier-A code should not
// name them.
//
// A caller supplying a NEW `SurfaceT` needs an instantiation added beside those.
// New code should not need one: the modern fitted pipeline produces a
// `PricedSurface`, whose entry points are the non-templated overloads below.

// ── Finite-difference greeks ─────────────────────────────────────────────

// Spot-based sensitivity block, same conventions as the option pipeline's
// AmericanGreeks (portfolio_pricer.hpp): delta = dPV/dS, gamma = d2PV/dS2,
// vega = dPV/dsigma (parallel surface shift, per 1.00 vol), volga = d2PV/dsigma2,
// vanna = d2PV/dSdsigma, theta = dPV/dt (calendar, PV units per year, holding
// the realized accrual fixed), rho = dPV/dr, charm = d2PV/dSdt. NaN = not
// computed (see DerivGreekBumps::second_order and the theta note below).
//
// All sensitivities are NOTIONAL-scaled, because PV is: a var swap's delta is
// dollars per 1.00 of spot on the whole contract, not per unit of variance.
struct DerivGreeks {
  double pv = 0.0;
  double delta = 0.0, gamma = 0.0, vega = 0.0, volga = 0.0, vanna = 0.0;
  double theta = 0.0, rho = 0.0, charm = 0.0;
  DerivQuote quote{};  // the center (unbumped) quote
};

// Bump sizes for `deriv_greeks`. The defaults are the ones the whole test
// matrix is calibrated against; they are exposed so a caller pricing a
// pathologically short tenor can widen them.
struct DerivGreekBumps {
  double spot_rel = 1.0e-4;          // relative S bump (central)
  double vol_abs = 1.0e-4;           // absolute parallel sigma bump (central)
  double rate_abs = 1.0e-4;          // absolute zero-rate bump (one-sided)
  double time_years = 1.0 / 365.25;  // theta roll (one-sided, T decreasing)
  // vanna + charm, the only greeks needing evaluations of their own (4 spot x
  // vol crosses + 2 rolled spot bumps = 6 extra repricings); both are NaN when
  // this is off. gamma and volga fall out of the SAME stencils delta and vega
  // already pay for, so they are always computed and this knob does not gate
  // them.
  bool second_order = true;
};

// Finite-difference greeks for any vol-derivative contract.
//
// Every bump reprices through `deriv_price`, so each product / age / cap
// regime gets its greeks from exactly the path that produced its mark — a
// capped swap differentiates its own cap model, a mid-life vol swap its own
// distribution engine, and no greek can silently come from a different pricer
// than the PV it hedges.
//
// Bump mechanics:
//   - Spot is STICKY-STRIKE: `CurveSet::spot` and every `ForwardPoint::F`
//     scale by (1 +/- h) while the surface is read at k + ln(1 +/- h), i.e. the
//     vol is re-read at the ORIGINAL absolute strike. `curves.spot` is the
//     divisor, so it must be > 0.
//   - Vol is a PARALLEL additive shift of `iv(k,T)`; the curves are untouched.
//   - Rate rebuilds the yield curve with every zero rate shifted by dr,
//     sampled at the forward pillars' Ts plus the contract's own T. The
//     FORWARD curve is deliberately held fixed: F is fitted independently
//     upstream, so this reports the pure discounting exposure.
//   - Theta rolls `contract.maturity_t` down by dt with the realized spec
//     untouched, i.e. calendar time passes and nothing new is realized.
//
// THE CENTER'S NUMERICAL SCHEME IS PINNED INTO EVERY BUMP. Two things about
// the pricing are resolved FROM THE SURFACE, so both move when the surface is
// bumped — and a finite difference that lets them move is measuring a change of
// scheme, not a derivative:
//   - The STRIP GRID. The E2 adaptive-wing rescale sizes the span to the
//     tenor's own sigma*sqrt(T) and rounds the node count with a ceil(), so a
//     vol bump can push a bumped evaluation onto a different node count than
//     the center. The differences would then straddle a step discontinuity in
//     the quadrature and contaminate gamma / volga / vanna. The center's own
//     resolved grid (DerivQuote::strip_k_lo_used / strip_k_hi_used /
//     strip_nodes_used) is therefore pinned into all bumped evaluations through
//     the ordinary explicit-pin config path, so every evaluation integrates the
//     identical grid. Side effect, deliberate and harmless: with the span
//     pinned, a vol-UP bump raises the width the truncation test measures
//     against, so bumped quotes can carry StripTruncated* flags the center does
//     not. The stencils read only PV, never flags.
//   - The AUTO-CALIBRATED VOL-OF-VOL. When the center reports a
//     `vol_of_vol_used`, that xi is pinned too; otherwise vega would
//     double-count the drift of the calibration itself, mixing the model's
//     response to the vol shift with the model re-parametrizing itself. A
//     calibrated xi of exactly 0 is pinned as the smallest positive double
//     rather than as 0, because 0 is the config's "auto-calibrate" selector —
//     every consumer of xi reaches the same limit at a denormal as at zero, so
//     this pins the value without re-selecting the auto path.
// The center is then REPRICED under that pinned config and it is that value the
// stencils difference; the `pv` and `quote` reported back are the original
// center quote, priced exactly as `deriv_price` would have.
//
// FULLY-AGED CONTRACTS SKIP ALL BUMPING. Nothing is left to realize, so PV is a
// fixed settlement amount under a pure discount, PV(t) = e^{-r(T-t)}*X. Every
// market greek is exactly 0, and the two time greeks are analytic and mutually
// consistent — dPV/dr = -(T-t)*PV and dPV/dt = +r*PV are one statement
// differentiated two ways, so rho = -T*PV and theta = r*PV (r read off the
// curve at maturity). At T == 0 the discount is gone and both are 0. The one
// quote where this identity does not describe the PV is a DerivFlags::DfFallback
// one (no discount factor resolved, df = 1 substituted); the flag on `quote` is
// how a caller detects that.
//
// THETA/CHARM ARE NaN WHEN `maturity_t <= bumps.time_years`. The roll would
// land at or past expiry, where an un-aged var/vol swap has no future leg left
// to price (the pricers return InvalidArgument for T <= 0). Reporting "not
// computed" beats failing the whole greek block over one stencil that cannot
// exist.
//
// @return the error of the first failing evaluation — the center quote's, or a
//         bumped one's (a bumped failure is a real failure: the same contract
//         priced under a marginally different market must not be silently
//         dropped). Plus InvalidArgument when a bump size is non-positive or
//         `curves.spot` is not > 0.
template <class SurfaceT>
[[nodiscard]] Result<DerivGreeks>
deriv_greeks(const SurfaceT& surface, const CurveSet& curves,
             const DerivContract& contract, const DerivConfig& cfg = DerivConfig{},
             const DerivGreekBumps& bumps = DerivGreekBumps{});

// Instantiated in derivatives.cpp over the same supported set as its siblings
// (see the note above `deriv_price`): `VolSurface` plus the two demoted
// containers this public header deliberately does not name.

// ── E6 / AN-W: PricedSurface-native entry points ────────────────────────────
//
// The modern fitted pipeline produces a `PricedSurface`, not one of the
// containers the templates above are instantiated for, so reaching
// `var_swap_fair_strike` from it meant hand-converting slices — which is why
// this whole module was once reachable only from its own unit test.
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

// Same contract as the templated `deriv_greeks` above, differentiating the
// PricedSurface-native `deriv_price`. The fitted-range gate runs ONCE, on
// `contract.maturity_t`: the theta roll then reuses that same carry CurveSet
// with a shorter contract T rather than re-deriving carry, so a contract
// sitting exactly on the front pillar rolls into the curve's flat-extrapolated
// tail instead of failing OutOfRange.
//
// CAVEAT on a contract at or very near the FRONT fitted pillar: below it the
// carry CurveSet clamps the forward flat while `PricedSurface::forward_at`
// would keep extrapolating economically — the very divergence `carry_from`'s
// range gate exists to refuse. The gate cannot see the rolled T, and theta
// divides the resulting PV difference by dt (~1/365), so it AMPLIFIES that
// divergence by ~365x. Theta on a front-pillar contract is therefore the one
// output here to treat as indicative; price the roll against a surface whose
// front pillar is genuinely shorter than the contract if it must be traded on.
[[nodiscard]] Result<DerivGreeks> deriv_greeks(const PricedSurface& surface,
                                               const DerivContract& contract,
                                               const DerivConfig& cfg = DerivConfig{},
                                               const DerivGreekBumps& bumps = DerivGreekBumps{});

}  // namespace atx::vol
