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
//   - Single-name dividend adjustment (Task F-6): the ISDA/MCA convention
//     r_i = ln(S_i / (S_{i-1} - D_i)), selected per tracker through
//     RealizedTracker::set_dividend_adjustment and fed through the three-argument
//     observe_dated. Index legs keep the raw close-to-close return, which stays
//     the default. This is what `RealizedVarianceSpec::include_dividend_adjustment`
//     now means; it was a reserved, unsettable field before F-6.
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
//   - Gamma swap (Task F-2, PV-F1 / LIT-7, DerivKind::GammaSwap): Lee's
//     weighted-variance strip, sharing the SAME grid/span/clamp/kink
//     resolution as the variance strip (the 1/K weight cancels against the
//     log-strike Jacobian, so the per-node integrand is just the undiscounted
//     OTM price -- see `strip_fair_value_core`, derivatives.cpp). Aged blend,
//     PV, and FD greeks dispatch identically to VarSwap; analytic greeks are
//     deferred (P-4's `AnalyticStrip` scope stays VarSwap-only). Exact under
//     zero carry only; see `DerivKind::GammaSwap`'s own doc for the
//     first-order-in-(r-q)*T approximation this ships for r != q.
//
// Reserved for follow-on work. Two flavors:
//   - ACTIVELY REJECTED (return ErrorCode::NotImplemented, mirroring the C's
//     ATS_VOL_ERR_UNSUPPORTED): the RV distribution-affine / Monte-Carlo QE
//     pricing engines (DerivEngine::RvDistributionAffine / McQe), the
//     discrete-monitoring full-Monte-Carlo correction
//     (DerivDiscreteCorrection::FullMc), and DerivMarkingConvention::Cboe
//     VarianceFuture -- all checked up front by every pricer, regardless of
//     aging state.
//
// The marking convention moved into that list in Task F-5, and the move is a
// BEHAVIOUR CHANGE worth stating: it used to be "DECLARED, UNENFORCED", i.e.
// a CboeVarianceFuture-marked contract priced identically to an Otc one and
// returned a confident OTC number instead of failing loud. A caller who was
// relying on that (there is no correct reason to have been) now gets
// NotImplemented from validate_deriv_dispatch on every kind and both lanes.
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
#include <optional>
#include <span>

#include "atx/vol/detail/aggregate_arity.hpp" // DerivConfig/DerivQuote field-count drift pins
#include "atx/vol/rates_curve.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

// E6: used only by const-reference in the PricedSurface-native overloads below,
// so the heavy definition stays out of this header.
class PricedSurface;

// ── Enums ────────────────────────────────────────────────────────────────

// Product kind. CappedVarSwap is priced via the lognormal RV distribution
// model (Task 4); CappedVolSwap via the same model's split-domain quadrature
// (Task 5). GammaSwap (Task F-2, PV-F1 / LIT-7) is a WEIGHTED-variance swap
// -- Lee's w(y) = y/Y0 weight function, lambda_yy = 2/(Y0*K) -- priced via
// the SAME model-free OTM-strip machinery as VarSwap (shared grid/span/
// clamp/kink resolution; the 1/K weight cancels against the log-strike
// Jacobian, so the strip's per-node integrand is just the undiscounted OTM
// price, not price/K). See `var_swap_fair_strike`'s doc and
// `strip_fair_value_core` (derivatives.cpp) for the shared strip, and the
// EXACTNESS CAVEAT below.
//
// EXACTNESS CAVEAT (Lee, "Weighted Variance Swaps," EQF 2010): the vanilla
// (single-expiry) OTM-strip replication of a weighted-variance claim is
// EXACT only under zero carry (r == q) -- see `GammaSwap.FlatZeroCarryExact`.
// Under r - q != 0 the log-contract's usual "delta term vanishes" identity
// (the reason VarSwap's own strip is carry-independent) no longer holds for
// a NON-log weight, and the true model-free hedge needs a continuum of
// expiries, not this task's scope. This library ships the standard
// single-expiry form (what every desk actually trades), which is a FIRST-
// ORDER-IN-(r-q)*T approximation to the exact weighted-variance expectation
// -- `K_gamma_shipped - K_gamma_exact ~= sigma_atm^2 * (r-q) * T / 2` for a
// flat surface (closed-form re-derivation, `GammaSwap.CarryApproximation
// ClosedForm`, derivatives_test.cpp). `GammaSwap.SkewOrdering` is the other
// discriminator (fails at exactly K_gamma - K_var == 0 under reversion).
// `GammaSwap.MCOracle` (an independent seeded-MC oracle) does NOT quantify
// this gap: at GBM/flat-vol precision the gamma-vs-variance discrimination
// signal and the single-expiry approximation bias above are the SAME order,
// O((r-q)*T)*sigma^2, so "passes" and "discriminates" cannot both hold at any
// feasible path count (Task F-2 fix round 1 / I-2 -- see MCOracle's own
// comment in derivatives_test.cpp). It is kept as a coarse calibration check
// only -- a wrong outer scale or broken quadrature still fails it. Analytic
// greeks are
// DEFERRED for this kind (P-4's `AnalyticStrip` scope stays VarSwap-only,
// unchanged by this task) -- `DerivGreekMethod::FiniteDifference` (the
// default) is correct and unaffected.
//
// CORRIDOR VARIANCE SWAP (Task F-3, PV-F3 / LIT-7, DerivKind::CorridorVarSwap).
// The same model-free variance strip, with the replicating weight
// 1{K in C}/K^2 instead of 1/K^2 -- so the ONLY difference from `VarSwap` is
// the INTEGRATION DOMAIN: the strip integrates over
// [ln(corridor_lo/F), ln(corridor_hi/F)] intersected with the span the tier /
// adaptive-wing policy resolved, and the corridor edges are therefore Simpson
// PANEL BOUNDARIES (C-3's split machinery, run on the restricted interval --
// see `strip_fair_value_core`, derivatives.cpp) rather than points a panel
// straddles. Both bounds are ABSOLUTE STRIKES on `DerivContract`, and `0` on a
// side means UNBOUNDED there, so `corridor_lo == corridor_hi == 0` reproduces
// `VarSwap`'s own quote on the same nodes with the same weights
// (`Corridor.FullCorridorIdentity`).
//
// REALIZED LEG (the convention this kind's acceptance criteria require stated
// in the header). Fixing i counts toward the corridor accrual iff its
// PREVIOUS CLOSE -- S_{i-1}, the spot the return r_i = ln(S_i/S_{i-1}) is
// measured FROM, not the spot it ends at -- lies in [corridor_lo,
// corridor_hi], CLOSED on both ends. That is the standard listed convention
// (the barrier is tested on information available before the return is
// realized, so the indicator is predictable and the accrual is a martingale
// increment); it is deliberately NOT the gamma swap's own convention, which
// weights by the spot AT the return (`RealizedVarianceSpec::sum_weighted_sq_
// log_returns_done`). `RealizedTracker::create_corridor` applies it;
// `RealizedVarianceSpec::n_obs_in_corridor` / `sum_sq_log_returns_in_corridor`
// / `rv_corridor_done_dec` carry the result.
//
// The CONDITIONAL variant (normalize by the in-corridor count rather than by
// the contract's total fixing count) is NOT a separate kind: one accrual
// yields both numbers, and the conditional one is published as
// `DerivQuote::conditional_corridor_var_dec`. See that field's own doc for
// exactly which quantity it is and, as importantly, which it is not.
// OPTIONS ON REALIZED VARIANCE (Task F-5, PV-F5 / LIT-5, DerivKind::
// VarianceCall / VariancePut). A European call or put on the SAME blended
// realized variance V = a + b*W the capped kinds already price -- a =
// w_done*rv_done_dec the accrued leg, b = w_future, W the future leg modeled
// lognormal with mean K_var_future and log-stdev xi*sqrt(T). The payoff is
// (V - K)+ for a call and (K - V)+ for a put, K being `DerivContract::
// strike_dec` read as an OPTION STRIKE in annualized decimal VARIANCE units.
// `cap_dec` names nothing here and must be 0 (validate_deriv_dispatch, the
// same rule every other uncapped kind follows). Engines: Auto or
// RvDistributionProxy.
//
// Both close in closed form against the displaced lognormal:
//   E[(V-K)+] = b * E[(W - (K-a)/b)+]   -- atx::vol::detail::lognormal_call
//   E[(K-V)+] = b * E[((K-a)/b - W)+]   -- atx::vol::detail::lognormal_put
// The put is priced by its OWN closed form rather than by rearranging the
// call through put-call parity: parity differences two nearly-equal numbers
// for a deep in-the-money call and loses digits exactly where the put's value
// is smallest. Parity is used as the ORACLE instead (`VarOption.PutCallParity`,
// deriv_distribution_test.cpp), which is the stronger arrangement -- it tests
// two independent formulas against each other rather than one formula against
// itself.
//
// WHAT `DerivQuote::fair_strike_dec` MEANS HERE, because it is not what the
// name says on every other kind: it is the option's UNDISCOUNTED PREMIUM,
// E[payoff], in decimal variance units -- and `pv` is `df * notional *` that,
// with NO strike subtraction, because K is already inside the payoff. There is
// no strike that prices an option to PV = 0, so "fair strike" has no meaning
// for these two kinds; the premium is the headline number a quote carries and
// it keeps `fair_strike_dec == undiscounted_expectation_dec`, the invariant
// every other kind already satisfies.
//
// E[V] is readable as `accrued_component_dec + future_component_dec` ON THE
// PATHS THAT RESOLVED A FUTURE LEG -- the model path (where it is a + b*m) and
// the fully-aged path (where b == 0 and E[V] == a exactly). It is NOT readable
// that way on the PUT-PIN path below: no strip runs there, so
// `future_component_dec` is 0 in this file's standing "0.0 means NOT COMPUTED"
// sense (the same convention `uncapped_var_dec` and `cap_option_value_dec`
// already use), while the true E[V] is a + b*m with b > 0. A caller that needs
// E[V] on a pinned put must price the underlying variance separately; gate on
// `DerivFlags::OptionPinned`.
//
// EXITS mirror the capped-variance structure, with one asymmetry worth stating
// because it is easy to get backwards. FULLY AGED (b == 0) is deterministic
// for both: V == a exactly, so the payoff is max(a-K,0) / max(K-a,0) with no
// strip and no model. A PUT additionally pins when a >= K even mid-life --
// V >= a >= K makes the payoff identically 0, so it is a valid quote request
// at T == 0 and must not pay for (or fail on) a strip it does not need. That is
// the same structure `DerivFlags::CapPinned` records for the capped kinds, and
// it gets its own flag for the same reason: `DerivFlags::OptionPinned`.
// (`CapPinned` itself is NOT reused -- it is documented as always accompanied
// by `CapApplied` and names a cap these kinds do not carry.) A CALL has NO
// corresponding pin: a >= K makes exercise certain but the VALUE is still
// a + b*m - K, which needs the strip's m. `lognormal_call` already returns
// m - k for k <= 0, so that case needs no branch at all -- it is the ordinary
// model path with a negative effective strike.
//
// MODEL RISK (LIT-5), stated in the header because a caller cannot see it in
// the numbers. Realized variance has a right tail materially FATTER than
// lognormal -- variance-of-variance clusters, and the empirical RV
// distribution is closer to an inverse-gamma / affine-jump law than to the
// two-parameter lognormal this engine assumes. The house model therefore
// UNDERPRICES out-of-the-money variance CALLS, and the error grows with
// moneyness: a strike far above K_var is priced off exactly the region where
// the lognormal is thinnest. It is a genuine model choice and not a
// calibration artifact -- xi is calibrated to the surface's own Carr-Lee
// convexity, i.e. to a sqrt-moment, which pins the middle of the distribution
// and says nothing about its tail. `DerivEngine::RvDistributionAffine` and
// `DerivEngine::McQe` remain reserved for exactly this: they are the escape
// hatch for a caller who needs the tail priced rather than assumed. Until one
// of them ships, treat a far-OTM variance-call mark from this library as a
// LOWER BOUND. Variance PUTS are affected in the opposite, much milder
// direction (the left tail of RV is bounded below by 0 and the lognormal
// respects that), which is why this note names calls specifically.
enum class DerivKind : std::uint8_t {
  VarSwap = 1,
  VolSwap = 2,
  CappedVarSwap = 3,
  CappedVolSwap = 4,
  GammaSwap = 5,
  CorridorVarSwap = 6,
  VarianceCall = 7,
  VariancePut = 8,
};

// THE two per-kind payoff-shape questions, each stated ONCE (Task F-5, pre-
// feature refactor). Both used to be hand-written `kind ==` chains, and the
// capped one had THREE independent copies of the same disjunction:
// `validate_deriv_dispatch` (derivatives.cpp, the cap_dec scope rule),
// `is_capped_kind` (backtest.cpp, the swap-lot cap_dec rule AND the settlement
// haircut), and an inline expression in `validate_restrike_spec`
// (strategy.cpp). Three copies of one rule is the shape that produced F-3's
// own Critical C-1: two lanes reaching different verdicts about one contract.
// They now CALL one function, so divergence is impossible by construction
// rather than merely detectable.
//
// They live HERE, beside the enum, rather than in `backtest.hpp` beside
// `engine_supports_swap_kind`, because they answer a question about the
// PRODUCT (what does this contract's payoff look like) and not about the
// ENGINE (can the backtest loop carry one). `derivatives.cpp` is also the
// lowest layer that needs them, and it cannot include `backtest.hpp` without
// inverting the dependency.
//
// Both are exhaustive `switch`es with NO `default:`, for exactly the reason
// `engine_supports_swap_kind`'s own doc gives: a `kind ==` chain assigns a
// future enumerator to the negated branch SILENTLY, whereas `-Wswitch -WX`
// turns this into a compile error and forces the author to choose. The
// trailing `return false;` covers an out-of-enum value only, matching what the
// `==` chains these replace already did for one.

// True for the kinds whose `cap_dec` is meaningful (and required > 0); false
// for every other kind, which must leave `cap_dec` at 0.
[[nodiscard]] constexpr bool deriv_kind_is_capped(DerivKind kind) noexcept {
  switch (kind) {
  case DerivKind::CappedVarSwap:
  case DerivKind::CappedVolSwap:
    return true;
  case DerivKind::VarSwap:
  case DerivKind::VolSwap:
  case DerivKind::GammaSwap:
  case DerivKind::CorridorVarSwap:
  // Task F-5: an option on variance carries an option STRIKE (`strike_dec`),
  // never a cap. `cap_dec` must stay 0, which is exactly what falling here
  // means.
  case DerivKind::VarianceCall:
  case DerivKind::VariancePut:
    return false;
  }
  return false;  // out-of-enum value: refuse, matching the `==` chains replaced
}

// True for the kinds whose settled terminal rate is a VOL -- sqrt of the
// accrued variance -- rather than a variance. Read by the backtest engine's
// `swap_terminal_value`, which settles `qty * notional * (terminal -
// strike_dec)`: a LINEAR payoff in the terminal rate. A kind whose payoff is
// not linear in it has no honest answer here, and must be refused by
// `engine_supports_swap_kind` (backtest.hpp) before it can reach that code.
[[nodiscard]] constexpr bool deriv_kind_settles_in_vol(DerivKind kind) noexcept {
  switch (kind) {
  case DerivKind::VolSwap:
  case DerivKind::CappedVolSwap:
    return true;
  case DerivKind::VarSwap:
  case DerivKind::CappedVarSwap:
  case DerivKind::GammaSwap:
  case DerivKind::CorridorVarSwap:
  // Task F-5: the option kinds land here because their UNDERLYING is a
  // variance, but read the contract above before relying on that. Their payoff
  // is (V-K)+ / (K-V)+, which is NOT linear in the terminal rate, so
  // `swap_terminal_value`'s `qty * notional * (terminal - strike_dec)`
  // settlement would pay a swap's linear P&L on an option. That is why
  // `engine_supports_swap_kind` (backtest.hpp) refuses both kinds outright:
  // this predicate's answer for them is never reached, and must not become
  // reachable without `swap_terminal_value` growing an option branch first.
  case DerivKind::VarianceCall:
  case DerivKind::VariancePut:
    return false;
  }
  return false;  // out-of-enum value: refuse, matching the `==` chains replaced
}

// Pricing engine selector. Values >= RvDistributionAffine are reserved;
// RvDistributionProxy is also reserved EXCEPT as the distribution-model
// dispatch target for DerivKind::CappedVarSwap (Task 4),
// DerivKind::CappedVolSwap (Task 5), DerivKind::VolSwap (Task 6 -- mid-life
// always, plus an unaged contract priced end to end through the model instead
// of Carr-Lee), and DerivKind::VarianceCall/VariancePut (Task F-5), all of
// which Auto also routes to as well.
enum class DerivEngine : std::uint8_t {
  Auto = 0,
  StripLogContract = 1,
  VolCarrLee = 2,
  // CappedVarSwap/CappedVolSwap/VolSwap/VarianceCall/VariancePut only;
  // reserved otherwise.
  RvDistributionProxy = 3,
  // Reserved -- AND the documented escape hatch from the lognormal RV tail
  // assumption the proxy engine makes; see DerivKind::VarianceCall's model-risk
  // note above.
  RvDistributionAffine = 4,
  McQe = 5,  // reserved; same escape-hatch role as RvDistributionAffine
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
  // pts at sigma=20%, r-q=5%, T=1Y, n=252 (CHANGELOG's C-1 entry has the full
  // worked comparison against the pre-fix formula). Does NOT cover
  // the residual O(1/n) JUMP term (Broadie-Jain sec 4); jump-diffusion
  // discrete-monitoring bias needs the FullMc engine below (reserved).
  Diffusion1OverN = 1,
  FullMc = 2,  // reserved
};

// Marking convention. CBOE variance-future marking is reserved, and ENFORCED
// as reserved since Task F-5: `validate_deriv_dispatch` (derivatives.cpp)
// returns NotImplemented for it on every kind, through both the `deriv_price`
// lane and the book-memo lane. Before F-5 nothing read this field at all, so
// the listed convention silently priced as OTC.
enum class DerivMarkingConvention : std::uint8_t {
  Otc = 1,
  CboeVarianceFuture = 2,  // reserved: NotImplemented, never priced as Otc
};

// Which Carr-Lee K_vol approximation the ATMF-straddle formula (and the
// vol-of-vol auto-calibration that inverts it, resolve_vol_of_vol) resolve
// against. Task C-5; see Carr & Lee (2009) Remarks 6.4/6.5, cited in full
// below, for the from-paper derivation.
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

// How the variance strip serves reads BEYOND the surface's certified wing
// trust band (Task F-1, FIT-F1 / PV-6 / LIT-6). `DerivConfig::wing_clamp_k`
// resolves WHERE the band sits; this selects WHAT gets served past it.
//
// BACKGROUND. Jiang & Tian (2007) document flat-vol wing extrapolation --
// freezing the read at the band-edge vol -- as standard index-methodology
// practice (LIT-1; also CBOE's VIX convention), and it is what `FlatClamp`
// below does. It is deliberately CONSERVATIVE: on a name with material equity
// skew, JT's own numbers put a smile-consistent (non-flat) wing treatment
// versus flat extrapolation at roughly -198bp to +79bp on a VIX-style index,
// the SIGN depending on the regime (a steep near-the-money skew whose wings
// keep rising understates K_var under flat truncation; a wing that flattens
// or reverses can overstate it) -- flat clamp is not a bug, it is one
// defensible point on that range, and this sprint's own desk ruling (sp100
// XOM, Task C-6) kept it the v1.1 default for mark stability. On the specific
// regime this knob targets -- sigma_atm*sqrt(T) large enough that the
// certified band sits inside roughly 2 standard deviations of the smile
// (sigma_atm*sqrt(T) gtrsim 0.083, i.e. the band edge at |k|=0.5 is within
// ~2sigma) -- flat clamp's understatement is the systematic, predictable
// side of that range, because the strip's own fitted wings (eSSVI's phi
// ceiling caps total-variance slope at <= 2 by construction -- Lee 2004's
// moment-formula bound, LIT-6; ConvexDense's tails are power-law, not flat)
// are already Lee-consistent and have never been allowed to say so.
//
//   FlatClamp -> beyond the band, freeze the read at the band-edge vol (JT
//               practice, unchanged from pre-F-1 behaviour). v1.1 DEFAULT --
//               bit-identical to every quote struck before this knob
//               existed; see the header note on `DerivConfig::wing_mode`.
//   LeeSlopeExtrapolation -> beyond the band, serve TOTAL VARIANCE
//               continuing at the fitted slice's OWN slope at the band edge,
//               clamped to Lee's [0, 2-eps] moment bound (eps = 1e-3) so an
//               ill-behaved or misconfigured fit can never smuggle a
//               moment-violating wing through this path even though the
//               reads it starts from are already almost always compliant.
//               Continuous AND slope-matched (C1) at the band edge whenever
//               the clamp does not bind -- no band-edge kink for the C-3
//               quadrature split to worry about in this mode (see
//               `var_swap_fair_strike`, derivatives.cpp). This is the
//               mode that recovers part of the JT understatement above by
//               trusting the fit exactly as far as Lee's own bound allows.
//   Raw       -> no clamp: read the raw (possibly uncertified) surface at
//               every node, exactly as `wing_clamp_k < 0` has always meant.
//               Provided as an explicit alternative to that sign convention;
//               behaves identically to it regardless of `wing_clamp_k`'s own
//               value (see `resolve_wing_clamp`, derivatives.cpp).
//
// Per-caller, not global: this makes the level-fidelity trade a CHOICE a
// caller states, rather than a fixed desk-wide ruling every book inherits.
enum class StripWingMode : std::uint8_t {
  FlatClamp = 0,
  LeeSlopeExtrapolation = 1,
  Raw = 2,
};

// Which numerical scheme `deriv_greeks` uses for delta / gamma / vega / vanna
// / volga (Task P-4, GK-P). Theta / theta_carry / theta_zero_fixing are
// UNAFFECTED by this knob -- each rolls `contract.maturity_t` and so prices
// "genuinely new information" a closed form cannot shortcut (see
// `DerivGreekBumps::method`'s own doc) -- and rho is ALREADY the closed form
// `-T*PV` on every path regardless (Task P-2, GK-C3). charm IS affected
// (Review fix round 2, I-2): it differences an FD-rolled delta at `T - dt`
// (method-independent) against `g.delta` AT `T`, which IS the value this
// knob selects -- see `deriv_greeks`'s own charm comment, derivatives.cpp,
// and `AnalyticGreeks.*`'s charm gate, deriv_greeks_test.cpp, for the
// measured magnitude (small: ~3.7e-4 relative on the skew-unaged fixture).
//
//   FiniteDifference -> every affected greek comes from a bumped repricing
//               through `deriv_price` (the pre-P-4 behaviour, unchanged bit
//               for bit). Works for every `DerivKind`.
//   AnalyticStrip -> `DerivKind::VarSwap` (uncapped, any age) WITH
//               `DerivConfig::discrete_correction_mode == None` differentiates
//               the model-free strip's own closed form instead of repricing
//               it under a bump -- see `deriv_analytic_greeks.hpp`
//               (derivatives.cpp) for the full derivation. Requested on any
//               OTHER kind (`VolSwap`, `CappedVarSwap`, `CappedVolSwap` --
//               each prices through a genuinely nonlinear model layer on top
//               of the strip that admits no such shortcut) OR with the
//               `Diffusion1OverN` discrete-monitoring correction ON (its
//               addend is QUADRATIC in K_var, so the raw-strip closed form
//               this file differentiates does not reproduce it -- Review
//               fix round 1, C-1) OR with `DerivConfig::wing_mode ==
//               StripWingMode::LeeSlopeExtrapolation` (Task F-1: the closed
//               form's wing term hard-codes the FlatClamp/Raw "clamp the grid
//               position once, then shift" identity -- `deriv_analytic_
//               greeks.hpp`'s own header note -- and has no chain-rule term
//               for a band-edge slope that is ITSELF resolved by a finite
//               difference of the surface; extending it is new derivation
//               work, not this task's scope, so LeeSlope falls back to FD
//               like the other excluded cases rather than silently
//               differentiate the wrong wing shape) falls back to
//               FiniteDifference SILENTLY: the fallback only ever picks
//               which NUMERICAL METHOD computes a greek, and never changes
//               what dispatch error a given (kind, engine) combination
//               raises -- an invalid engine/kind pairing fails exactly the
//               same way under either method (PV-5's dispatch matrix,
//               `deriv_price`, runs identically either way; only
//               `deriv_greeks`'s POST-price greek computation branches on
//               this).
enum class DerivGreekMethod : std::uint8_t {
  FiniteDifference = 0,
  AnalyticStrip = 1,
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
  // band. Task F-1: fires under `DerivConfig::wing_mode ==
  // StripWingMode::FlatClamp` (the v1.1 default) exactly as it always has;
  // `LeeSlopeExtrapolation` raises `WingExtrapolated` instead on the same
  // structural condition, never this one. atx extension: not mirrored in
  // AtsVolDerivFlags.
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
  // Task F-1: the `WingClamped` sibling for `DerivConfig::wing_mode ==
  // StripWingMode::LeeSlopeExtrapolation` -- same STRUCTURAL condition (the
  // resolved span extended beyond the wing trust band, so nodes beyond it
  // contributed under the Lee-slope extrapolation formula rather than the raw
  // surface), fired on that mode instead of `WingClamped`. Says nothing about
  // whether the band-edge slope itself needed clamping to Lee's [0, 2-eps]
  // bound -- that has no flag of its own; a caller who needs to know can
  // compare the served K_var against a `Raw` quote on the same inputs. Absent
  // under `FlatClamp`/`Raw` (those raise `WingClamped` or nothing,
  // respectively), and absent whenever the whole span fits inside the band.
  // atx extension: not mirrored in AtsVolDerivFlags.
  WingExtrapolated = 1u << 14,
  // Task F-4 (PV-F4 / LIT-7): `forward_var_fair_strike` found the T2 strip's
  // total variance BELOW the T1 strip's by more than the two legs' own
  // combined accuracy -- a calendar-arbitrageable surface at the strip level,
  // where the forward variance the caller asked for does not exist.
  //
  // DELIVERY. This flag is raised on an ERROR path (`ErrorCode::Internal`),
  // and an `Err` carries no `DerivQuote` to read it off. It is therefore
  // delivered through `forward_var_fair_strike`'s `diagnostic_out`
  // out-parameter, which that entry fills on EVERY return path -- see its doc
  // for the full ruling. A caller that passes `nullptr` gets the `Internal`
  // status and no flag; there is no other channel, and in particular the
  // error MESSAGE is not one (this sprint has twice been burned by treating an
  // error string as a machine-readable signal).
  //
  // Never set on a successful quote: a surface whose forward variance is
  // merely negative WITHIN that accuracy is served as exactly 0.0 with no
  // flag, because at that magnitude "inverted" and "flat" are the same
  // observation. atx extension: not mirrored in AtsVolDerivFlags.
  CalendarInconsistent = 1u << 15,
  // Task F-5 fix round 1: a variance PUT whose accrued leg alone already reached
  // its strike (a >= K). V >= a >= K makes (K-V)+ identically 0, so the quote is
  // pinned at premium 0 with no strip call and no T > 0 requirement -- exactly
  // the structure `CapPinned` records for the capped kinds, which is why that
  // flag is NOT reused here: `CapPinned` is documented as always accompanied by
  // `CapApplied` and names a cap these kinds do not carry.
  //
  // WHY IT NEEDS A FLAG AT ALL, rather than the header simply saying no flag is
  // set: a pinned put and a genuinely near-worthless one both quote ~0, and
  // distinguishing them otherwise takes an inference across three fields
  // (`ModelProxy` absent, `FullyAged` absent, `vol_of_vol_used` NaN). "Dead by
  // accrual" and "cheap by model" are different facts for anything marking or
  // risking the position, and the first one is knowable exactly.
  //
  // Never set on a CALL: a >= K makes exercise certain but leaves the value
  // a + b*m - K, which still needs the strip -- see DerivKind::VarianceCall.
  // atx extension: not mirrored in AtsVolDerivFlags.
  OptionPinned = 1u << 16,
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
  // Task F-6 (PV feature list / LIT-9): the SINGLE-NAME return convention.
  // false (the default) is the INDEX convention -- r_i = ln(S_i / S_{i-1}),
  // the raw close-to-close return, which is what every index variance swap
  // pays and what this accumulator has always computed. true is the
  // single-name convention -- r_i = ln(S_i / (S_{i-1} - D_i)), the prior close
  // reduced by the cash dividend going ex on day i, so a stock's mechanical
  // ex-div drop is not booked as realized variance (ISDA/MCA; the JPM worked
  // example is ln(94/95), not ln(94/100)).
  //
  // WHY THIS FIELD CARRIES THE INDEX-VS-SINGLE-NAME DISTINCTION: nothing else
  // in the deriv types does. `DerivContract` has no underlier-class
  // discriminator and `DerivKind` enumerates PRODUCTS, not underliers, so this
  // boolean is the whole of that convention. It travels on the snapshot into
  // `DerivContract::rv_spec` precisely so a consumer of an accrued leg can see
  // which convention produced it.
  //
  // Set it through `RealizedTracker::set_dividend_adjustment` (which refuses
  // once accrual has started -- see there); a hand-built spec sets it directly
  // and owns the consistency of whatever produced its sums. It was declared
  // "reserved; unused in this port" from the C port until F-6, during which
  // time NO code path could set it on a tracker at all.
  bool include_dividend_adjustment = false;
  // Task F-2 (PV-F1 / LIT-7): the gamma-swap (weighted-variance) accrued-leg
  // accumulator, appended -- Lee's w(y) = y/Y0 weight applied to each daily
  // realized return: raw running Sigma (S_i/S0)*r_i^2, S0 the tracker's own
  // seed spot (the first observe() call), S_i the spot AT the return i was
  // measured. Written by `RealizedTracker::observe` alongside the plain
  // accumulator above; a hand-built spec (tests, swap_leg.cpp's strategy-side
  // mirror) that never populates this leaves it at 0.0, exactly like
  // `sum_sq_log_returns_done`'s own "not populated" convention.
  double sum_weighted_sq_log_returns_done = 0.0;
  // Annualized decimal gamma-weighted variance to date: annualization *
  // sum_weighted_sq_log_returns_done / n_obs_done. `price_gamma_swap`
  // (derivatives.cpp) reads THIS field for the accrued leg, exactly as
  // `price_var_swap` reads `rv_done_dec` -- the two are NOT interchangeable
  // (the same "meaning is per-kind" convention DerivQuote::uncapped_var_dec
  // documents).
  double rv_gamma_done_dec = 0.0;
  // Task F-2 fix round 1 (C-1 Critical): the seed spot S0 that anchors the
  // weight above -- Lee's w(y) = y/Y0 -- so a consumer that must combine
  // `rv_gamma_done_dec` (anchored at S0, the tracker's FIRST observe() call)
  // with a future leg anchored at TODAY's spot (`curves.spot`) has the factor
  // it needs to put both legs on the same anchor before combining them:
  // `curves.spot / gamma_seed_spot`. Before this field existed, S0 was
  // accumulation-time-only state private to RealizedTracker (see its old
  // `s0_` member) on the theory that "no pricer reads it back off the
  // snapshot" -- `price_gamma_swap`'s aged blend (derivatives.cpp) falsified
  // that theory: it silently mixed a same-count-weighted blend of two
  // DIFFERENT anchors, which is only equal to the correct blend at
  // `gamma_seed_spot == curves.spot`. 0.0 means "no accrual yet recorded"
  // (RealizedTracker before its first observe(), or a hand-built spec that
  // never populates this), exactly like `rv_gamma_done_dec`'s own
  // "not populated" convention.
  double gamma_seed_spot = 0.0;
  // Task F-3 (PV-F3 / LIT-7): the corridor-variance accrued leg, appended as a
  // THIRD per-kind accumulator alongside the plain and gamma ones above. THE
  // MEMBERSHIP RULE, stated once and shared by every writer: fixing i counts
  // iff its PREVIOUS CLOSE S_{i-1} lies in the contract's
  // [corridor_lo, corridor_hi] (closed both ends; 0 on a side means unbounded
  // there) -- see `DerivKind::CorridorVarSwap`'s own doc for why the
  // predictable, previous-close convention is the right one and how it differs
  // from the gamma weight beside it.
  //
  // How many of the `n_obs_done` fixings passed that test. Needed by the
  // CONDITIONAL corridor variance (`DerivQuote::conditional_corridor_var_dec`),
  // which normalizes by this count instead of by `n_obs_done`; the two numbers
  // come from ONE accrual, which is why the conditional variant is a quote
  // field and not a second DerivKind. Never exceeds `n_obs_done`
  // (`price_corridor_var_swap` rejects a spec where it does).
  std::uint32_t n_obs_in_corridor = 0;
  // Raw running Sigma r_i^2 restricted to those fixings. Same "written by the
  // tracker, read back only by its own writers" role
  // `sum_sq_log_returns_done` has: no pricer reads it.
  double sum_sq_log_returns_in_corridor = 0.0;
  // Annualized decimal corridor variance to date, on the CONTRACT's clock:
  // annualization * sum_sq_log_returns_in_corridor / n_obs_done -- the
  // denominator is the count of fixings OBSERVED, not the count that passed
  // the corridor test, so that the ordinary n_done/n_total aged blend lands
  // exactly on annualization * Sigma_{in C} r^2 / n_total. This is the field
  // `price_corridor_var_swap` (derivatives.cpp) reads for the accrued leg,
  // exactly as `price_var_swap` reads `rv_done_dec`; the three are NOT
  // interchangeable. 0.0 with `n_obs_in_corridor == 0` is a REAL value ("no
  // fixing has been inside the corridor yet"), not "not populated".
  double rv_corridor_done_dec = 0.0;
};

// Drift pin: RealizedVarianceSpec has exactly TWELVE fields (v1.1 appended
// sum_weighted_sq_log_returns_done / rv_gamma_done_dec, Task F-2; v1.2
// appended gamma_seed_spot, Task F-2 fix round 1 / C-1; v1.3 appended
// n_obs_in_corridor / sum_sq_log_returns_in_corridor / rv_corridor_done_dec,
// Task F-3). Following the same
// `aggregate_arity_is_v` convention as `DerivConfig`/`DerivQuote` above.
// Every construction site in this codebase already uses `RealizedVarianceSpec{}`
// plus designated/named-field assignment (never positional brace-init), so
// this raises no migration burden; it only catches a FUTURE append that
// forgets to update this line.
static_assert(detail::aggregate_arity_is_v<RealizedVarianceSpec, 12>,
              "RealizedVarianceSpec field count changed: update this pin.");

// Mutable realized-variance accumulator. Caller owns; not thread-safe.
//
// Constructed through create() so the annualization / observation-count
// invariants are validated once at the boundary (the C's *_init returned
// ATS_VOL_ERR_INVALID for the same preconditions). Rule of Zero.
class RealizedTracker {
public:
  // Build a tracker. @return InvalidArgument if annualization <= 0 or
  // n_obs_total == 0 (mirrors ats_vol_realized_tracker_init). The corridor is
  // UNBOUNDED on both sides, so every fixing counts toward the corridor
  // accumulators too and they track the plain ones exactly -- the same
  // identity `Corridor.FullCorridorIdentity` pins on the pricing side.
  [[nodiscard]] static Result<RealizedTracker> create(double annualization,
                                                       std::uint32_t n_obs_total);

  // Task F-3: build a tracker that maintains the CORRIDOR accumulators against
  // [corridor_lo, corridor_hi] (absolute strikes; 0 on a side means unbounded
  // there, exactly as on `DerivContract`). A separate named entry rather than
  // a defaulted-argument overload, so a caller that means "corridor" has to
  // say so and a caller that does not cannot acquire one by accident.
  //
  // @return everything `create` does, plus InvalidArgument for a non-finite or
  //         negative bound, or for a bounded pair with lo >= hi (a zero- or
  //         negative-width corridor accrues identically nothing, which is a
  //         caller error rather than a product).
  [[nodiscard]] static Result<RealizedTracker> create_corridor(double annualization,
                                                                std::uint32_t n_obs_total,
                                                                double corridor_lo,
                                                                double corridor_hi);

  // Observe a single spot. The first call records the seed (no return
  // computed) AND anchors the gamma-weight S0 (Task F-2) at that same spot,
  // writing it to the snapshot's `gamma_seed_spot` (Task F-2 fix round 1 /
  // C-1) so a caller can rescale a future leg onto this same anchor; each
  // subsequent call updates Sigma r^2, n_done, rv_done_dec, and the
  // gamma-weighted Sigma (S_i/S0)*r^2 / rv_gamma_done_dec alongside it.
  //
  // Task F-3: each subsequent call also tests the return's PREVIOUS CLOSE (the
  // spot the PREVIOUS call recorded, not the one being passed now) against the
  // corridor and, when it is inside, adds r^2 to
  // `sum_sq_log_returns_in_corridor` and one to `n_obs_in_corridor`. Note the
  // deliberate asymmetry with the gamma weight on the line beside it, which
  // reads the spot AT the return: the two conventions differ because one is a
  // WEIGHT (Lee's y/Y0, evaluated where the variance is earned) and the other
  // is a PREDICTABLE INDICATOR (the corridor test must not peek at the return
  // it gates).
  //
  // @return InvalidArgument for spot <= 0, when all n_obs_total returns have
  //         already been observed, or -- Task F-6 -- when this tracker is
  //         dividend-adjusted (see `set_dividend_adjustment`), because this
  //         entry has no channel to carry a dividend and would silently accrue
  //         the INDEX convention onto a snapshot advertising the single-name
  //         one. A dividend-adjusted tracker is driven through the three-argument
  //         `observe_dated` only.
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

  // Task F-6 (PV feature list / LIT-9): the same dated observe, additionally
  // carrying the CASH DIVIDEND going ex on this fixing's day. On a
  // dividend-adjusted tracker the return is formed against the reduced prior
  // close, r_i = ln(S_i / (S_{i-1} - D_i)), so the mechanical ex-div drop is
  // not booked as realized variance; `ex_div_cash == 0` reproduces the
  // two-argument entry bit for bit, and that is exactly what the two-argument
  // entry forwards.
  //
  // A SEPARATE OVERLOAD, never a defaulted third argument on the declaration
  // above: a default would make every existing two-argument call ambiguous
  // against it, and folding the two declarations into one defaulted signature
  // would change an already-published signature under the v1.x additive-only
  // freeze.
  //
  // ONLY the return's DENOMINATOR moves. The gamma weight still reads the
  // just-observed close and the corridor indicator still tests the RAW previous
  // close (`Corridor.TrackerCountsTheRawPreviousCloseUnderADividend` pins
  // this): a corridor is a barrier in traded price space, and the dividend
  // adjustment is a return-construction device, not a restatement of where the
  // stock traded. `prev_spot()` likewise keeps reporting the raw close -- the
  // adjustment is per-return, so folding it into the stored mark would
  // double-count it on the NEXT return.
  //
  // @return everything the two-argument entry does, plus InvalidArgument when:
  //         ex_div_cash is negative or non-finite; ex_div_cash > 0 on a tracker
  //         that is NOT dividend-adjusted (a knob that would do nothing here is
  //         a caller error, not a silent no-op -- the rule `cap_dec` and the
  //         corridor bounds already follow); ex_div_cash > 0 on the SEEDING
  //         call, which forms no return for it to adjust; or ex_div_cash >=
  //         prev_spot(), which would make the adjusted prior close zero or
  //         negative and the return undefined.
  //
  // NOT WIRED TO THE BACKTEST. The swap lane's fixing driver
  // (`observe_swap_fixing`, backtest.cpp) is a separate transcription of this
  // arithmetic and passes no dividend at all; it stays on the index convention
  // until a corporate-actions feed exists to source D from. `FinancingConfig::
  // share_dividends` (backtest.hpp) is the OPTION lane's hedge-share cash
  // ledger and is not that feed.
  [[nodiscard]] Status observe_dated(std::int64_t ts_ns, double spot, double ex_div_cash);

  // Task F-6: select the SINGLE-NAME return convention on this tracker, i.e.
  // set `snapshot().include_dividend_adjustment`. Until this entry existed the
  // flag had no writer anywhere in the tree and was unreachable through the
  // public API despite shipping as a documented field.
  //
  // WHY A MODE SETTER AND NOT A `create_single_name` FACTORY. `create_corridor`
  // is a factory because it VALIDATES -- its bounds have an invariant that must
  // be checked once at the boundary. A boolean convention has no invariant to
  // validate, so the factory's whole rationale is absent, and a factory per
  // mode would multiply combinatorially against the corridor entry (the
  // corridor x single-name product is real and is reachable here by
  // composition, at no API cost). What a factory WOULD have bought -- the
  // convention being immutable for the tracker's accumulating lifetime -- is
  // bought instead by the refusal below, which closes the only window in which
  // a flip could corrupt anything.
  //
  // @return InvalidArgument once this tracker has observed ANYTHING (`have_prev()`),
  //         because a mid-stream flip would leave Sigma r^2 an undecomposable mix
  //         of adjusted and unadjusted returns while the snapshot advertised a
  //         single convention for all of them. Configure, then accumulate.
  [[nodiscard]] Status set_dividend_adjustment(bool on);

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

  // Task F-6: the ONE place a return is formed, so the dividend-adjusted and
  // unadjusted paths cannot drift apart -- every public observe entry funnels
  // here, the unadjusted ones passing ex_div_cash = 0.
  [[nodiscard]] Status observe_impl(double spot, double ex_div_cash);

  double prev_spot_ = 0.0;
  // Task F-2 fix round 1 (C-1 Critical): the gamma-weight anchor S0 used to
  // live here, private, on the theory that "no pricer reads it back off the
  // snapshot" -- `price_gamma_swap`'s aged blend needed exactly this value to
  // combine the accrued and future legs on a common anchor, so it now lives
  // in the snapshot itself as `rv_.gamma_seed_spot`; see that field's comment.
  bool have_prev_ = false;
  RealizedVarianceSpec rv_{};
  std::int64_t last_fixing_ts_ns_ = std::numeric_limits<std::int64_t>::min();
  // Task F-3: the corridor this tracker tests each fixing's previous close
  // against, in absolute strikes with 0 == unbounded (validated once, in
  // `create_corridor`). Accumulation-time-only state -- unlike F-2's seed
  // spot, no pricer needs to read it back off the snapshot, because the
  // corridor a quote is priced under comes from the CONTRACT
  // (`DerivContract::corridor_lo/corridor_hi`), which is the authority. A
  // tracker configured against one corridor and a contract priced against
  // another is a caller-side mismatch this class cannot detect and does not
  // pretend to.
  double corridor_lo_ = 0.0;
  double corridor_hi_ = 0.0;
};

// ── Contract / config / quote ────────────────────────────────────────────

// A vol-derivative contract (AtsVolDerivContract). `cap_dec` activates for
// CappedVarSwap (annualized decimal VARIANCE cap, e.g. (2.5*0.20)^2 = 0.25)
// and for CappedVolSwap (a decimal VOL cap instead, e.g. 2.5*0.20 = 0.50):
// deriv_price requires cap_dec > 0 on a capped kind (else InvalidArgument)
// and rejects a non-zero cap_dec on an uncapped kind (also InvalidArgument).
//
// UNIT NOTE (PV-7): the aged-trade blend weighs the accrued and future legs
// by RAW OBSERVATION COUNT -- n_done/n_total and n_future/n_total (n_future =
// rv_spec.n_obs_total - rv_spec.n_obs_done) -- never by calendar time
// directly. That is only the right blend when the observation schedule
// tracks calendar time at roughly the annualization's own cadence, i.e.
// n_future ~= rv_spec.annualization * T_resid, T_resid being `maturity_t`
// itself (the same residual year-fraction the future leg's K_var_future is
// priced over); the default annualization = 252 implicitly assumes one
// observation per trading day. Keeping `rv_spec` and `maturity_t` staged
// consistently with that assumption -- e.g. after a roll or re-strike that
// changes maturity_t but not n_obs_total/n_obs_done to match -- is entirely
// the caller's responsibility; nothing here cross-validates n_future against
// maturity_t.
//
// CORRIDOR NOTE (Task F-3): `corridor_lo`/`corridor_hi` activate for
// CorridorVarSwap and must be left at 0 on every other kind (else
// InvalidArgument -- the same "a knob that does nothing here is a caller
// error, not a silent no-op" rule `cap_dec` already follows). They are
// ABSOLUTE STRIKES, so their log-moneyness image ln(bound/F) is RE-RESOLVED
// at every pricing against that pricing's own forward: a corridor is a fixed
// barrier in price space, and a spot move genuinely changes how much of the
// contract's variance is inside it. That is the economically right reading
// AND what makes `deriv_greeks`' spot bumps carry the corridor's own
// sensitivity instead of freezing it (see `strip_fair_value_core`'s note on
// why `DerivQuote::strip_k_lo_used` reports the PRE-corridor span for this
// kind).
// OPTION NOTE (Task F-5): on `DerivKind::VarianceCall`/`VariancePut`,
// `strike_dec` is the OPTION strike in annualized decimal variance (the K in
// (V-K)+ / (K-V)+), not a swap's break-even level, and `cap_dec` must be 0 like
// every other uncapped kind. See `DerivKind::VarianceCall`'s own doc for what
// the resulting quote's `fair_strike_dec` and `pv` then mean, which differs
// from every swap kind.
struct DerivContract {
  DerivKind kind = DerivKind::VarSwap;
  double maturity_t = 0.0;   // years until expiry
  double strike_dec = 0.0;   // K_var or K_vol; the OPTION strike on F-5's kinds
  double cap_dec = 0.0;      // capped kinds only; see above
  double notional = 0.0;     // N_var or N_vol
  RealizedVarianceSpec rv_spec{};
  DerivMarkingConvention marking = DerivMarkingConvention::Otc;
  // CorridorVarSwap only; absolute strikes, 0 == unbounded on that side. See
  // the CORRIDOR NOTE above and `DerivKind::CorridorVarSwap`.
  double corridor_lo = 0.0;
  double corridor_hi = 0.0;
};

// Drift pin: DerivContract has exactly NINE fields (v1.3 appended
// corridor_lo / corridor_hi, Task F-3). This struct had no pin before F-3 --
// P-4's fix round audited its 7 fields by hand against the analytic-greek
// scope predicate, and a hand audit is exactly what a pin makes unnecessary
// the next time. Same contract as the `DerivConfig` pin above.
//
// A SCOPE-GATED field -- one meaningful on some kinds that must be zero on
// others, as `cap_dec` and the corridor bounds both are -- belongs in exactly
// ONE place: `validate_deriv_dispatch` (derivatives.cpp), which BOTH the
// `deriv_price` lane and the P-6 book-memo lane call. F-3's review measured
// what not knowing that costs: the corridor rule went into what was then a
// hand-synchronised copy of that validation, and the memo lane silently priced
// a contract the other lane rejected, at 2.56x. This pin is what routes the
// next appender to the single validator instead of to a second copy.
static_assert(detail::aggregate_arity_is_v<DerivContract, 9>,
              "DerivContract field count changed: update this pin. If the new field is "
              "SCOPE-GATED (legal on some kinds, must be zero on others), enforce it in "
              "validate_deriv_dispatch -- the ONE validator both the deriv_price and the "
              "book-memo lane call -- never in a per-lane copy. Also re-audit every "
              "predicate that projects a contract onto a scope decision "
              "(analytic_scope_from_cfg / var_swap_memo_eligible / "
              "validate_var_swap_shared_scope).");

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
  // log-forward-moneyness. The fit pipeline certifies a surface's no-
  // arbitrage properties only over the band its OWN build quality mode's
  // independent risk validator actually samples -- Latency |k| <= 0.35,
  // Balanced |k| <= 0.50, Accuracy |k| <= 0.60
  // (`atx::vol::certified_wing_half_band`, surface_policy.hpp; the fitter's
  // own `risk_validation_config`, pricer_fitter.cpp). Beyond ITS certified
  // band a parametric eSSVI/SVI slice serves an unbounded linear-in-|k|
  // extrapolation no quote ever disciplined, and the strip's 1/K weighting
  // turns that fiction into fair-strike level and daily mark noise (the
  // sp100-2026 XOM 3M strike read ~38 vol against a ~30 ATM, with ~98% of its
  // day-to-day variance sourced beyond |k| = 0.25). Nodes beyond the band keep
  // their true strikes but read the BAND-EDGE vol under the v1.1 default --
  // flat-vol tails, the standard desk discipline for un-quoted wings -- so
  // the strip stays complete (no truncation bias) while the uncertified
  // region loses its say. What exactly gets served past the band is
  // `wing_mode`'s decision (Task F-1); THIS field only decides where the band
  // itself sits. The span, node count, and truncation flags are untouched:
  // this clamps reads, not the grid. `DerivFlags::WingClamped` (FlatClamp) /
  // `WingExtrapolated` (LeeSlopeExtrapolation) record that tails were in
  // effect.
  //
  //   0    -> the SURFACE's own certified band when the caller states one --
  //           the `PricedSurface`/`SurfaceRef`-native entry points below
  //           (`var_swap_fair_strike` etc.) take a `surface_certified_wing_
  //           band` argument for exactly this (FIT-C7 / Task C-6) -- else
  //           the mode-blind default `strip::kCertifiedWingHalfBand` (= 0.5,
  //           the BALANCED band; kept equal to the default
  //           `RiskSurfaceValidationConfig{}.k_max`). A surface priced
  //           through a path with no such argument (the templated legacy
  //           VolSurface/eSSVI/SVI containers, or a caller that does not
  //           state the surface's quality mode) always resolves the
  //           mode-blind default, regardless of the surface's true mode.
  //   > 0  -> explicit half-band; reads clamped to [-wing_clamp_k, +wing_clamp_k].
  //           Wins over any surface-carried band.
  //   < 0  -> OFF: read the raw surface everywhere (pre-clamp behavior; the
  //           escape hatch for a surface whose wings ARE quote-disciplined).
  //           Wins over any surface-carried band.
  //   NaN  -> InvalidArgument.
  double wing_clamp_k = 0.0;
  // Task F-1 (FIT-F1 / PV-6 / LIT-6): what the variance strip serves for a
  // node beyond `wing_clamp_k`'s resolved band -- see `StripWingMode`'s own
  // doc for the full FlatClamp / LeeSlopeExtrapolation / Raw contract and the
  // Jiang-Tian bias framing. `FlatClamp` is the v1.1 DEFAULT: every quote
  // struck before this field existed keeps pricing bit-for-bit identically,
  // because it is the literal zero value a value-initialized `DerivConfig{}`
  // (or any struct predating this field) already carries. `Raw` behaves
  // identically to `wing_clamp_k < 0` regardless of this field's own value --
  // see `resolve_wing_clamp`, derivatives.cpp.
  StripWingMode wing_mode = StripWingMode::FlatClamp;
  // Reserved — must be left at 0.
  double abs_price_tol = 0.0;
  double rel_price_tol = 0.0;
  std::uint32_t flags_request = 0;
};

// Drift pin: DerivConfig has exactly FOURTEEN fields (v1.1 appended
// wing_mode, task F-1). Adding, removing, or splitting one breaks this
// line -- update the count, and confirm every construction site still uses
// `DerivConfig{}` + designated field assignment (the only form used anywhere
// in this codebase today; there is no positional brace-init to protect).
static_assert(detail::aggregate_arity_is_v<DerivConfig, 14>,
              "DerivConfig field count changed: update this pin.");

// The default config: STANDARD quality, AUTO engine, no discrete correction,
// OTC marking (ats_vol_deriv_default_config).
[[nodiscard]] inline DerivConfig deriv_default_config() noexcept {
  return DerivConfig{};
}

// Pricing result (AtsVolDerivQuote).
struct DerivQuote {
  // K_var or K_vol on every SWAP kind. On `DerivKind::VarianceCall`/
  // `VariancePut` (Task F-5) it is instead the option's UNDISCOUNTED PREMIUM in
  // decimal variance units, and `pv` is `df * notional *` this with NO strike
  // subtraction -- no strike prices an option to PV = 0, so a break-even level
  // does not exist for those two kinds. The invariant
  // `fair_strike_dec == undiscounted_expectation_dec` holds on all of them
  // regardless; E[V] on an option quote is
  // `accrued_component_dec + future_component_dec` EXCEPT on the put-pin path,
  // where no strip ran and `future_component_dec` is 0 in the "not computed"
  // sense -- gate on `DerivFlags::OptionPinned`, and see
  // `DerivKind::VarianceCall`'s own doc.
  double fair_strike_dec = 0.0;
  double fair_strike_points = 0.0;        // var pts or vol pts
  double pv = 0.0;                        // contract PV today
  double undiscounted_expectation_dec = 0.0;
  // The strip's variance in decimal variance units. ITS MEANING IS PER-KIND,
  // and the two readings are NOT interchangeable:
  //   * var-swap dispatch, the unaged vol swap's best-effort Carr-Lee
  //     diagnostic, BOTH capped paths, and BOTH variance-option paths (Task
  //     F-5) carry the strip's RAW FUTURE-LEG value K_var(T) -- no accrued leg,
  //     no discrete correction, no cap haircut, no option payoff.
  //     (`future_component_dec` beside it IS discrete-corrected and
  //     weight-scaled; these two deliberately differ.)
  //   * the MID-LIFE vol swap (the distribution model) carries the BLENDED
  //     TOTAL variance a + b*m that it actually prices sqrt() of: a =
  //     w_done*rv_done_dec, b = w_future, m the strip mean AFTER any
  //     Diffusion1OverN correction. That is the model's own input, and the
  //     number `convexity_adjustment_dec` beside it is formed from
  //     (sqrt(a+b*m) - fair_strike_dec).
  //   * GammaSwap dispatch (Task F-2 cleanup round, m-13) carries the
  //     strip's RAW, TODAY-ANCHORED future-leg value K_gamma(T) -- like
  //     var-swap dispatch above, no accrued leg, no rescale. This is the
  //     ONE field on a GammaSwap quote that is NOT seed-anchored:
  //     `fair_strike_dec`, `undiscounted_expectation_dec`, and
  //     `future_component_dec` are all rescaled onto
  //     `RealizedVarianceSpec::gamma_seed_spot` when an anchor exists
  //     (`price_gamma_swap`, derivatives.cpp -- the C-1/C-3 anchor
  //     invariant), but this field deliberately is not, mirroring var-swap
  //     dispatch's own "raw future leg, not part of the blend" convention.
  //     A consumer that combines `uncapped_var_dec` with any of those three
  //     on a GammaSwap quote is mixing anchors -- the same defect class as
  //     C-1/C-3/C-4, one level out in the quote rather than the spec.
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
  // The wing trust half-band `resolve_wing_clamp` (derivatives.cpp) actually
  // used for this quote's strip -- DerivConfig::wing_clamp_k resolved against
  // whatever certified band the surface itself carried, per FIT-C7 (Task C-6).
  // 0.0 means the clamp resolved OFF (an explicit negative wing_clamp_k); NaN
  // means no strip ran, same "not computed" convention as strip_k_lo_used
  // beside it. Populated and carried alongside the grid fields above by every
  // dispatch path that runs a strip.
  double resolved_wing_clamp = kQuietNaN;
  DerivFlags flags = DerivFlags::None;
  // Task F-3 (PV-F3): the CONDITIONAL corridor variance -- the realized
  // corridor variance to date normalized by the number of fixings that were
  // actually INSIDE the corridor rather than by the number observed:
  //
  //   annualization * Sigma_{i in C} r_i^2 / n_obs_in_corridor
  //   == rv_corridor_done_dec * n_obs_done / n_obs_in_corridor
  //
  // Published as a field rather than as a second `DerivKind` because both
  // numbers come from ONE accrual: `fair_strike_dec` blends the
  // n_done/n_total-weighted (UNCONDITIONAL) accrual with the corridor strip,
  // and this is the same accrual re-normalized. Populated ONLY on
  // CorridorVarSwap dispatch; NaN, not 0, everywhere else and whenever
  // `n_obs_in_corridor == 0` (nothing to condition on) -- a caller gates on
  // (x == x), the same convention `vol_of_vol_used` uses.
  //
  // WHAT IT IS NOT, stated because the difference is easy to miss: this is a
  // PURELY REALIZED quantity. It is NOT a forward-looking conditional fair
  // strike -- that would need E[corridor variance] / E[time in corridor], and
  // the denominator is an expected OCCUPATION TIME, which no single-expiry
  // option strip replicates (unlike the numerator, which is exactly what this
  // kind's strip prices). Dividing this quote's `fair_strike_dec` by a
  // corridor-time estimate is the caller's own modelling decision, not
  // something this library has done for them.
  double conditional_corridor_var_dec = kQuietNaN;
  // Task F-4 (PV-F4): the two var-swap legs `forward_var_fair_strike`
  // differenced, in ANNUALIZED decimal variance -- K_var(T1) and K_var(T2),
  // each as its own strip reported it, under the SHARED policy resolution both
  // legs were priced with.
  //
  // Appended rather than folded onto `accrued_component_dec` /
  // `future_component_dec`, which mean "realized leg" / "implied leg of an
  // aged blend" on every other kind: a forward-start strike has no accrued
  // leg, and overloading those two would have made the same double mean two
  // incompatible things depending on how the quote was produced -- the exact
  // defect class this sprint's C-1..C-4 spent four Criticals on.
  //
  // NaN, not 0, on every quote no forward-start entry produced (the same
  // convention `vol_of_vol_used` and `conditional_corridor_var_dec` use; a
  // caller gates on (x == x)). On a forward-start quote both are finite and
  // the identity
  //     fair_strike_dec == (leg_T2_var_dec*T2 - leg_T1_var_dec*T1)/(T2 - T1)
  // holds exactly as computed, EXCEPT on the documented clamp branch where a
  // within-accuracy negative numerator is served as 0.0 -- publishing both
  // legs is what lets a caller recompute the raw quotient and see that.
  double leg_T1_var_dec = kQuietNaN;
  double leg_T2_var_dec = kQuietNaN;
};

// Drift pin: DerivQuote has exactly NINETEEN fields (v1.1 appended
// resolved_wing_clamp, Task C-6; v1.3 appended conditional_corridor_var_dec,
// Task F-3; v1.3 appended leg_T1_var_dec + leg_T2_var_dec, Task F-4). See the
// DerivConfig pin above for the contract this protects.
static_assert(detail::aggregate_arity_is_v<DerivQuote, 19>,
              "DerivQuote field count changed: update this pin.");

// ── Carr-Lee convexity refinement (detail) ─────────────────────────────────

namespace detail {

// Carr-Lee (Carr & Lee 2009, "Robust Replication of Volatility Derivatives",
// https://math.uchicago.edu/~rl/rrvd.pdf, Remark 6.4/6.5) convexity
// refinement of the naive ATMF-straddle K_vol approximation, adapted to this
// codebase's ANNUALIZED decimal convention. The from-paper re-derivation
// follows in full below; in short, the paper states Remark 6.4 for
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
// LARGER than the +0.906 vol bp the refinement itself adds (CHANGELOG.md's
// measurement table for this task): a Refined strike still lands
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

// ── Forward-start variance (Task F-4: PV-F4 / FIT-F2 / LIT-7) ────────────

// The coarsest K_fwd `forward_var_fair_strike` will serve, in ANNUALIZED
// decimal variance units: 1e-3 is 0.25 vol points at a 20% forward vol (0.5 at
// 10%). When the two legs' combined noise floor divided by (T2 - T1) exceeds
// this, the call refuses rather than returning a number whose leading digits
// are quadrature and fit error. See the entry's TENOR SEPARATION note.
inline constexpr double kFwdVarNoiseCeilingVar = 1.0e-3;

// THE CANONICAL FORWARD-VARIANCE CONVENTION FOR THIS LIBRARY. Total variance
// is additive in time (LIT-7), so the fair strike of a variance swap starting
// at T1 and ending at T2 is the difference of the two strips' total variances
// per unit of elapsed time:
//
//     K_fwd = (K_var(T2)*T2 - K_var(T1)*T1) / (T2 - T1)
//
// with each K_var(T) the FULL-SMILE model-free strip this file already
// prices. That "full smile" is the whole point, and it is what makes this the
// canonical convention rather than one of two: `atx::vol::forward_vol`
// (analytics.hpp) answers the same question from the ATM total variance
// ALONE, which is a different quantity on any surface with skew -- the
// tradeable forward variance is an integral over the smile, not an ATM read.
// `forward_vol` is retained unchanged for its term-structure-diagnostic
// callers (analytics_aggregate's `forward_vol_segments`) and is documented
// there as the ATM diagnostic, NOT the pricing convention; it is not removed
// in 1.x (additive-only API).
//
// SHARED POLICY, PER-TENOR GRIDS. Both legs are priced under ONE resolution of
// the pricing policy: one `DerivConfig` object and one certified-wing-band
// argument reach both strips, so wing mode, wing trust band and `width_sigmas`
// are the same by construction -- there is no second resolution that could
// drift from the first, which is the only way the difference of two strips
// means anything. What DOES differ per tenor is the resolved grid: each leg's
// adaptive span and node budget follow its own sigma_atm*sqrt(T), exactly as a
// standalone `var_swap_fair_strike` at that tenor would. That is correct;
// forcing one grid on both legs would under-resolve one of them.
//
// HOW ACCURATE K_fwd ACTUALLY IS, and the two `DerivConfig` fields that move
// it -- `strip_nodes` and `width_sigmas`. Every figure below is measured on a
// flat-smile surface with six pillars from 0.10y to 1.00y at r = 4.3%, over all
// fifteen ordered tenor pairs (the `ForwardVar.FlatSurfaceExact` /
// `...DegradesAtHighVol` fixtures). Which field binds depends on the REGIME:
//
//   * NODE BUDGET, at ordinary vol (sigma = 0.20). The error here is Simpson
//     quadrature and falls ~16x per node doubling. The `High` and `Audit` tier
//     DEFAULTS reach 4.2e-17 and 1.2e-13. THE `Standard` TIER DEFAULT DOES NOT:
//     257 nodes give ~1.5e-10. A caller who needs 1e-10 at `Standard` must set
//     `DerivConfig::strip_nodes = 513`, which measures 9.4e-12. This is the one
//     accuracy surprise in the entry and it is stated here rather than left for
//     a caller to discover.
//
//   * VOLATILITY, and the knob for it is the SPAN, not the node budget. The
//     per-leg error also carries a SPAN-TRUNCATION term that scales with
//     sigma*sqrt(T) against the resolved span. At low vol it is negligible and
//     nearly tenor-invariant, so it cancels in (w2 - w1); as vol rises it grows
//     and becomes strongly tenor-dependent -- because the
//     `width_sigmas`*sigma*sqrt(T) widening engages on the LONG leg while the
//     short one still sits on the tier's span FLOOR, so the two legs end up with
//     very unequal COVERAGE (half-span / sigma*sqrt(T)): 11.50 short vs 6.00
//     long at sigma = 0.55 / `High`. Worst case over the fifteen pairs at each
//     tier's DEFAULT config: ~1.2e-10 (sigma = 0.55), ~3.3e-10 (0.70),
//     ~1.1e-09 (0.90).
//
//     THAT IS THE ACCURACY OF THE TIER'S DEFAULT SPAN, NOT OF THIS ENTRY. The
//     distinction is load-bearing because the two budgets behave oppositely:
//       - `strip_nodes` does NOT lift it. Truncation is not a discretisation
//         error, so the error plateaus: at sigma = 0.55 an 8193-node strip lands
//         within 8e-14 of a 4097-node one, both above 1e-10, and the fifteen-pair
//         worst case is 1.3245e-10 at BOTH 8193 and 16385 nodes.
//       - `width_sigmas` DOES, because it is the knob that buys coverage.
//         Measured at sigma = 0.55 on each tier's DEFAULT node budget:
//         `High` 1.2451e-10 (default 6) -> 2.7371e-11 (8) -> 1.0283e-11 (12);
//         `Audit` 2.8867e-10 -> 1.0500e-12 (8) -> 2.2873e-12 (12). One field,
//         no extra nodes, and both clear the 1e-10 bar.
//
//     `Standard` IS THE EXCEPTION AND NEEDS BOTH KNOBS. Its 257-node default
//     cannot pay for a wider span -- widening ALONE spreads the same nodes over
//     more range and makes it WORSE (2.4859e-10 at the default 6, 9.7861e-10 at
//     8). `width_sigmas = 8` WITH `strip_nodes = 2049` measures 7.4729e-12.
//
// ACCURACY FLOOR AND THE CALENDAR DETECTOR (FIT-F3). The numerator differences
// two nearly-equal total variances, so the answer is only as good as the legs
// are. Two error sources are named explicitly and combined into ONE noise
// floor in total-variance units:
//   * the library's calendar accuracy floor, `kCalendarTotalVarianceTol`
//     (types.hpp) -- the same 1e-7 in w the fit-side no-arb checks measure
//     against. A surface can PASS every fit-side calendar check and still
//     carry a w-decrease of that size; a detector with a tighter bar would
//     fire on good surfaces.
//   * each leg's OWN reported quadrature error, `integration_error_est`,
//     converted to total-variance units (err*T). Measured per call rather
//     than assumed, so a coarse tier widens the band and a fine one narrows
//     it (the same "anchor the gate to the reported error estimate, not to a
//     magic constant" rule Task F-1 established).
// Below that floor a negative numerator is indistinguishable from a flat term
// structure and is served as K_fwd == 0.0. Beyond it the surface is genuinely
// calendar-arbitrageable at strip level and the call FAILS LOUD with
// `ErrorCode::Internal` plus `DerivFlags::CalendarInconsistent`. This is the
// strip-level detector the fit-side lattice check cannot be: the fit checks
// w(k,T) pointwise on a sampled lattice, while a variance swap trades the
// whole integral, and the integral can invert while every sampled point holds.
//
// HOW THE ERROR-PATH FLAG REACHES YOU. `Result<DerivQuote>` carries no quote
// on an `Err`, so a flag "on the error-path diagnostic" has no channel in the
// return type. `diagnostic_out` is that channel: when non-null it is ASSIGNED
// ON EVERY RETURN PATH -- success, calendar failure, and every argument or
// leg failure -- so a caller reads the flag off it after any outcome. Paths
// that never priced a leg leave it default-constructed (`DerivFlags::None`,
// both leg fields NaN); paths that priced both legs fill the legs and the
// flags whether or not the call then succeeded. Passing `nullptr` (the
// default) is supported and simply forgoes the diagnostic.
//
// TENOR SEPARATION. T2 -> T1 divides the noise floor above by a vanishing
// number, so past some separation K_fwd is noise. Rather than a magic minimum
// gap, the entry refuses (`OutOfRange`) exactly when the noise floor divided
// by (T2 - T1) exceeds `kFwdVarNoiseCeilingVar` decimal variance units --
// i.e. when the answer cannot be resolved to that accuracy at this tenor
// separation, whatever combination of gap, tier and surface produced it.
//
// @return InvalidArgument when T1/T2 are not finite, T1 <= 0, or T2 <= T1
//         (0 has exactly one meaning here -- an invalid tenor -- and +Inf is
//         rejected, not admitted, by the finiteness test);
//         OutOfRange when the tenor separation cannot resolve K_fwd to
//         `kFwdVarNoiseCeilingVar`; Internal + `CalendarInconsistent` on a
//         genuinely inverted term structure; otherwise whatever error the
//         first failing leg's `var_swap_fair_strike` returned, unchanged.
//
// Quote fields: `fair_strike_dec`/`fair_strike_points` carry K_fwd;
// `leg_T1_var_dec`/`leg_T2_var_dec` carry the two legs; `flags` is the OR of
// both legs' provenance; `uncapped_var_dec` carries the forward TOTAL variance
// (K_fwd*(T2-T1)). `pv` stays 0: this is a fair strike, not a contract.
// Grid-provenance fields (`strip_k_lo_used` and siblings) report the T2 LEG's
// grid, since the two legs' grids genuinely differ -- `leg_T*_var_dec` plus a
// direct `var_swap_fair_strike` call is how a caller recovers the other.
template <class SurfaceT>
[[nodiscard]] Result<DerivQuote>
forward_var_fair_strike(const SurfaceT& surface, const CurveSet& curves, double T1,
                        double T2, const DerivConfig& cfg = DerivConfig{},
                        DerivQuote* diagnostic_out = nullptr);

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
  // Carry-theta diagnostics (Task C-10, GK-C2). `theta` above rolls ONLY the
  // CALENDAR (T -> T - dt) with `rv_spec` held byte-for-byte fixed, so it
  // silently omits the implied->realized fixing rollover -- the largest
  // deterministic daily P&L term on any unaged/mid-life swap (theta reports
  // ~0 on a fair-struck swap; the real daily mark move is the fixing roll,
  // not the calendar roll). These two fields price THAT roll too: a COPY of
  // `rv_spec` gets one additional fixing (n_done -> n_done+1) injected before
  // the SAME T -> T - dt roll `theta` already takes, so any DerivKind's own
  // aged-blend dispatch does the actual repricing -- nothing here duplicates
  // per-kind blend math.
  //   theta_carry: the injected fixing lands exactly AT today's model-free
  //     implied variance rate (K_var_future, resolved fresh via
  //     var_swap_fair_strike regardless of DerivKind -- the one process every
  //     product kind's future leg is struck against). On a fair-struck swap
  //     the blend does not move (fair stays fair), so this isolates the pure
  //     discounting drift `theta`'s calendar-only roll already carries.
  //   theta_zero_fixing: the injected fixing is a literal zero return
  //     (nothing traded overnight) -- the deterministic "nothing happened"
  //     mark move, i.e. the number a desk actually calls carry and the one to
  //     put in a daily P&L predict.
  // On the BUMPED path (not fully aged), both NaN when `theta` is
  // (maturity_t <= bumps.time_years), when `DerivGreekBumps::carry_theta` is
  // false, or when the injected fixing would cross a dispatch-ENGINE boundary
  // the center was never in (Task C-10 fix round 1, CRITICAL-1) -- today that
  // is exactly an unaged VolSwap priced under an explicit
  // `DerivEngine::VolCarrLee`: injecting a fixing makes it mid-life, and
  // `price_vol_swap` rejects VolCarrLee mid-life (Carr-Lee cannot blend an
  // accrued leg). Every other greek in the block is unaffected and still
  // computed -- only these two fields go NaN, never the whole
  // `Result<DerivGreeks>`.
  // FULLY AGED IGNORES `carry_theta`: both equal `theta` exactly on that
  // branch REGARDLESS of the knob -- the same PV = df*X identity theta/rho
  // already share there, since nothing is left to realize and there is no
  // fixing left to roll, so there is nothing for the knob to gate (the brief
  // states "Fully-aged: both = theta" unconditionally). A caller who sets
  // `carry_theta = false` specifically to suppress these two fields
  // everywhere must also check `has_flag(quote.flags, DerivFlags::FullyAged)`.
  //
  // PRICER-BOUNDARY CAVEAT (unaged VolSwap only, any engine): the center of an
  // unaged VolSwap prices via Carr-Lee, but the injected copy is mid-life and
  // therefore prices via the lognormal RV distribution model
  // (`price_vol_swap_distribution`) whenever that engine is legal for it --
  // so these two fields momentarily difference PVs from two DIFFERENT
  // pricers, unlike `theta` (which never changes `rv_spec` and so never
  // leaves Carr-Lee).
  // Under the DEFAULT auto-calibrated vol-of-vol this is a small, second-order
  // Jensen-gap effect (`resolve_vol_of_vol` calibrates the model to reproduce
  // Carr-Lee's K_vol exactly at the center, so the two engines agree at the
  // degenerate point by construction: ~0.1-0.2% of the carry signal on a
  // typical fixture). Under an EXPLICIT `cfg.vol_of_vol`, the distribution
  // model no longer agrees with Carr-Lee at all and the effect becomes
  // FIRST-ORDER, growing with the mismatch between the caller's xi and the
  // auto-calibrated one -- a caller who pins `vol_of_vol` explicitly on an
  // unaged VolSwap should treat these two fields as approximate, not exact.
  double theta_carry = kQuietNaN;
  double theta_zero_fixing = kQuietNaN;
  // Smile greeks (Task F-7, GK-G1/G2). `vega` above is a PARALLEL shift only,
  // but a variance swap's defining risk is the SHAPE of the smile: the strip
  // integrates every strike, so a rotation or a steepening of the smile moves
  // K_var even when the ATM vol does not budge. These two name that exposure.
  //
  // MONEYNESS CONVENTION. k = ln(K/F), the same log-forward-moneyness the
  // strip integrates in (detail/strip_grid.hpp) and the same coordinate
  // `SurfaceAnalytics::skew_slope` (analytics.hpp) reports d(sigma)/dk in. So
  // a perturbation of `s` here shifts the surface's OWN skew_slope by exactly
  // +s, in identical units.
  //   skew_vega       dPV/ds under  iv(k,T) -> max(iv(k,T) + s*k,       1e-4)
  //   convexity_vega  dPV/dc under  iv(k,T) -> max(iv(k,T) + c*k*k,     1e-4)
  // Both are PV per 1.00 of the coefficient (a full vol point of extra slope
  // per unit log-moneyness), which is a large perturbation -- a typical index
  // skew_slope is O(0.1) -- so a desk figure is `skew_vega * 0.01` for a
  // 1-vol-point-per-unit-k rotation. Both are notional-scaled like every other
  // field here.
  //
  // SIGN. s < 0 is the equity-like direction: it RAISES downside vols (k < 0)
  // and lowers upside ones, i.e. steepens the familiar equity skew. A long
  // variance swap is long every strike, so richer puts raise K_var and raise
  // PV -- PV rises as s falls, hence skew_vega < 0 on a long var swap. Pinned
  // and independently cross-checked by `SmileGreeks.SkewSignOnSkewFixture`
  // (deriv_greeks_test.cpp). c > 0 raises BOTH wings and lowers nothing, so it
  // raises K_var unambiguously: convexity_vega > 0 on a long var swap.
  //
  // WING-CLAMP SATURATION -- a modelling limit to read before using these.
  // The strip CLAMPS k into the resolved wing trust band before it ever calls
  // `iv` (see the wing-clamp discussion above `DerivConfig::wing_clamp_k`), so
  // the perturbation a node past the band actually receives is s*band, not
  // s*k: the linear term SATURATES at the band edge instead of growing. With
  // the default certified half-band of 0.5 and a 1Y 20-vol strip spanning
  // roughly +-1.2 in k, the outer wings therefore all receive the SAME shift.
  // These two numbers are consequently sensitivities of the CLAMPED surface
  // the strip actually prices -- which is the honest target, since that is the
  // surface `pv` came from -- and NOT of an unclamped analytic smile. A caller
  // wanting the unsaturated figure must widen `DerivConfig::wing_clamp_k` (or
  // select `StripWingMode::Raw`) for the greek call as well as the mark.
  //
  // NaN unless `DerivGreekBumps::smile_greeks` is on (4 extra repricings);
  // like every market greek here, both are exactly 0 on a fully-aged contract,
  // where nothing is left to realize for the smile to act on.
  double skew_vega = 0.0;
  double convexity_vega = 0.0;
  DerivQuote quote{};  // the center (unbumped) quote
};

// Drift pin: DerivGreeks has exactly FOURTEEN fields (v1.1 appended
// theta_carry / theta_zero_fixing in Task C-10, then skew_vega /
// convexity_vega in Task F-7). See the DerivConfig pin above for the contract
// this protects.
static_assert(detail::aggregate_arity_is_v<DerivGreeks, 14>,
              "DerivGreeks field count changed: update this pin.");

// Bump sizes for `deriv_greeks`. The defaults are the ones the whole test
// matrix is calibrated against; they are exposed so a caller pricing a
// pathologically short tenor can widen them.
struct DerivGreekBumps {
  double spot_rel = 1.0e-4;          // relative S bump (central)
  double vol_abs = 1.0e-4;           // absolute parallel sigma bump (central)
  // UNUSED as of Task P-2 (GK-C3): rho is now the closed form -T*PV (see
  // deriv_greeks' own doc below) rather than a one-sided finite difference of
  // a rate-shifted reprice, so there is no rate bump left to size. Kept as a
  // field (not removed) so this v1.x struct stays additive-only / source-
  // compatible with any existing caller-set value, which is now simply
  // ignored; `bumps_valid` still requires it be > 0, matching every other
  // bump size's validation, even though nothing reads it downstream.
  double rate_abs = 1.0e-4;
  double time_years = 1.0 / 365.25;  // theta roll (one-sided, T decreasing)
  // vanna + charm, the only greeks needing evaluations of their own (4 spot x
  // vol crosses + 2 rolled spot bumps = 6 extra repricings); both are NaN when
  // this is off. gamma and volga fall out of the SAME stencils delta and vega
  // already pay for, so they are always computed and this knob does not gate
  // them.
  bool second_order = true;
  // Gate for DerivGreeks::theta_carry / theta_zero_fixing (Task C-10). Costs
  // ONE extra var_swap_fair_strike evaluation (resolving the model-free
  // implied variance rate the injected fixing is struck at -- see the header
  // note above DerivGreeks::theta_carry) plus TWO extra deriv_price
  // repricings (the T - dt roll with one fixing injected at that rate, and
  // again at a zero return), on top of the block's existing up-to-12 (Task
  // F-7: was written as 13 here, stale since Task P-2 removed the FD rate
  // bump without re-counting -- the measured bump-table count is pinned by
  // `SmileGreeks.OffByDefaultCostsNothing`, deriv_greeks_test.cpp). Skipped
  // for free (no extra evaluation at all) when `contract.rv_spec.n_obs_total
  // == 0` -- no fixing schedule exists to inject into, so both fields just
  // equal `theta`. Default true: theta_zero_fixing is the number a daily P&L
  // predict should read, and a caller should not have to opt in to see it.
  bool carry_theta = true;
  // Task P-4 / GK-P: opt-in closed-form delta/gamma/vega/vanna/volga for
  // `DerivKind::VarSwap` (see `DerivGreekMethod`'s own doc for scope and the
  // silent-fallback contract). Default `FiniteDifference` -- no mark move
  // for any existing caller; the flip to a different default is a migration
  // this library will evaluate no sooner than a 2.0 (mirrors
  // `CarrLeeForm::Refined`'s own "planned 2.0 default" precedent, not a
  // commitment that 2.0 makes it).
  DerivGreekMethod method = DerivGreekMethod::FiniteDifference;
  // Task F-7 smile bumps -- the coefficients of the two shape perturbations
  // `DerivGreeks::skew_vega` / `convexity_vega` differentiate (see those
  // fields for the k = ln(K/F) convention, the signs, and the wing-clamp
  // saturation caveat). Both are absolute vol per unit k / per unit k^2, and
  // both are CENTRAL differences, so each costs two repricings.
  //
  // 1e-3 rather than `vol_abs`'s 1e-4: these multiply k, and the strip's own
  // trusted band is |k| <= 0.5 by default, so a 1e-4 slope moves the far wing
  // by only 5e-5 vol -- within the strip quadrature's own noise on a Debug
  // build, which shows up as a stencil that fails to converge rather than as a
  // visibly wrong number. 1e-3 keeps the perturbation comfortably above that
  // floor while staying far inside the linear regime (verified by
  // `SmileGreeks.SkewSignOnSkewFixture`'s 100x bump-size independence check).
  double skew_abs = 1.0e-3;
  double convexity_abs = 1.0e-3;
  // OFF BY DEFAULT, unlike `second_order`/`carry_theta`. Costs FOUR extra
  // repricings (skew +/-, convexity +/-, all at zero spot and vol shift), and
  // unlike the carry thetas these are a portfolio-shaping diagnostic rather
  // than a number a daily P&L predict needs -- so a caller opts in. Both
  // fields are NaN when this is off, by the same arithmetic NaN propagation
  // `vanna` uses under `second_order == false`, never a false 0.0.
  //
  // Honoured under `DerivGreekMethod::AnalyticStrip` too: the closed form
  // (deriv_analytic_greeks.hpp) has no skew/convexity term, so these two are
  // computed by finite difference either way. An explicitly requested greek
  // silently coming back NaN because an unrelated method knob was set is the
  // worse failure; the cost is the same 4 repricings.
  bool smile_greeks = false;
};

// Drift pin: DerivGreekBumps has exactly TEN fields (v1.1 appended method in
// Task P-4, then skew_abs / convexity_abs / smile_greeks in Task F-7). See
// the DerivConfig pin above for the contract this protects.
static_assert(detail::aggregate_arity_is_v<DerivGreekBumps, 10>,
              "DerivGreekBumps field count changed: update this pin.");

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
//   - SMILE SHAPE (opt-in, `DerivGreekBumps::smile_greeks`) adds two more
//     surface perturbations at zero spot and vol shift: `iv(k,T) + s*k` and
//     `iv(k,T) + c*k*k`, each floored at 1e-4 so a large bump cannot drive a
//     wing node non-positive. See `DerivGreeks::skew_vega` for the k = ln(K/F)
//     convention, the sign, and the wing-clamp saturation caveat.
//   - Theta rolls `contract.maturity_t` down by dt with the realized spec
//     untouched, i.e. calendar time passes and nothing new is realized.
//   - Rate has NO bump: rho is the closed form below, not a stencil.
//
// RHO IS EXACTLY -T*PV, ANALYTICALLY, NOT JUST ON THE FULLY-AGED BRANCH
// (Task P-2, GK-C3). Every quote this library builds -- var swap, the
// unaged/mid-life vol swap (Carr-Lee or the lognormal RV distribution model),
// both capped kinds, at any age -- is assembled as `pv = df(r) * X`, where X
// (the fair strike / undiscounted expectation / cap option blend) is PROVABLY
// INDEPENDENT of the rate curve, not merely insensitive to it in practice:
//   - The variance strip's own OTM(K)/(df*K^2) integrand has its discount
//     factor cancel algebraically against the OTM price's own df (Demeterfi-
//     Derman-Kamal-Zou), so K_var never reads `curves.yield` at all.
//   - The Carr-Lee ATMF-straddle K_vol formula (`carr_lee_k_vol`) is a
//     function of the ATMF implied vol and T alone -- no discount factor
//     anywhere in it.
//   - The one place a rate-like quantity enters a non-df channel --
//     `DerivDiscreteCorrection::Diffusion1OverN`'s carry differential
//     (`resolve_carry_diff`, r_bar - q_bar) -- is read off ln(F/S)/T, the
//     FORWARD and spot, never `curves.yield`, so even that correction leaves
//     X untouched by a rate curve rebuild.
// So dPV/dr = X * d(df)/dr = -T * df(r) * X = -T * PV identically, exactly the
// same identity the fully-aged branch below already uses (the two are one
// statement, not two): the finite-difference r+ bump this used to cost was
// recomputing that identity to FD precision, one whole extra repricing per
// greek call (a second strip integration for var/capped swaps; a second
// Carr-Lee straddle plus its own diagnostic strip for an unaged vol swap),
// never discovering anything the closed form does not already say. T is
// clamped to >= 0 before the multiply, same as the fully-aged branch's own
// PV-9 clamp: a cap-pinned quote can succeed at T <= 0 without being
// FullyAged (only partially aged, e.g. an expired-but-not-rolled-off lot
// pinned at its cap), where `df` is unconditionally 1.0 and the true dPV/dr
// is 0, not a sign-flipped `-T*PV` (fix round 1, C-1).
// `Rho.AnalyticMatchesFD` (deriv_greeks_test.cpp) pins this identity against
// the FD bump this replaced, across every DerivKind and aging state, before
// the bump was ever deleted.
//
// FORWARD-CHANNEL RATE RISK IS DELIBERATELY NOT IN RHO, unchanged from every
// prior version of this stencil: F is fitted independently upstream of this
// library (see the spot-bump note above -- a spot bump moves F, but nothing
// here ever moves F in response to r), so rho reports the pure discounting
// exposure only, exactly as it always has. The switch to a closed form is
// honest AND free: it removes the FD bump's own truncation noise (rho now
// bit-identical across repeated calls and independent of `bumps.rate_abs`,
// which is accordingly unused -- see its own field doc) without changing
// what rho MEANS.
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
// how a caller detects that. theta_carry and theta_zero_fixing both equal this
// same theta exactly — nothing is left to realize, so there is no fixing roll
// left to price either.
//
// THETA/CHARM ARE NaN WHEN `maturity_t <= bumps.time_years`. The roll would
// land at or past expiry, where an un-aged var/vol swap has no future leg left
// to price (the pricers return InvalidArgument for T <= 0). Reporting "not
// computed" beats failing the whole greek block over one stencil that cannot
// exist. theta_carry / theta_zero_fixing share this same gate (same roll, same
// knob, `DerivGreekBumps::time_years`) on the bumped path -- see the field
// doc on `DerivGreeks::theta_carry` for their two additional NaN cases
// (`DerivGreekBumps::carry_theta` false; the injected fixing crossing a
// dispatch-engine boundary the center was never in) and for why fully-aged
// ignores all of this and always returns `theta`.
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
// `surface_certified_wing_band`: FIT-C7 (Task C-6). `DerivConfig::wing_clamp_k
// == 0` (the default) ordinarily resolves to the strip's mode-blind certified
// band (`strip::kCertifiedWingHalfBand`, = the BALANCED quality mode's own
// certified band) -- correct for a Balanced-quality surface, but a surface
// fit at a DIFFERENT quality mode certifies a DIFFERENT band (Latency ±0.35,
// Accuracy ±0.60: `atx::vol::certified_wing_half_band`, surface_policy.hpp).
// A caller who knows this PricedSurface's own build quality mode should
// resolve that band and pass it here; the strip then trusts the surface
// exactly where that mode's fit pipeline actually certified it instead of the
// mode-blind default. `std::nullopt` (the default) preserves prior behaviour
// exactly -- the mode-blind band, unless `cfg.wing_clamp_k` overrides it
// explicitly (unchanged >0/<0 semantics either way).
//
// @return the same error contract as the templated overloads, plus
//         InvalidArgument when the surface carries no usable fitted pillar and
//         OutOfRange when `T` falls outside the fitted pillar range.
[[nodiscard]] Result<DerivQuote>
var_swap_fair_strike(const PricedSurface& surface, double T,
                     const DerivConfig& cfg = DerivConfig{},
                     std::optional<double> surface_certified_wing_band = std::nullopt);

[[nodiscard]] Result<DerivQuote>
vol_swap_fair_strike(const PricedSurface& surface, double T,
                     const DerivConfig& cfg = DerivConfig{},
                     std::optional<double> surface_certified_wing_band = std::nullopt);

[[nodiscard]] Result<DerivQuote>
deriv_price(const PricedSurface& surface, const DerivContract& contract,
           const DerivConfig& cfg = DerivConfig{},
           std::optional<double> surface_certified_wing_band = std::nullopt);

// Forward-start variance on the modern fitted container -- the entry Task F-4's
// spec names, and the one to reach for. Full contract (canonical convention,
// shared policy resolution, accuracy floor, calendar detector, and how
// `diagnostic_out` delivers `DerivFlags::CalendarInconsistent` on the error
// path) is on the templated declaration above; only the PricedSurface-specific
// parts are restated here.
//
// BOTH tenors are gated against the surface's fitted pillar range by the SAME
// carry resolution -- one `CurveSet` built once from the surface's own pillars
// serves both legs, so the two strips cannot end up reading different forwards
// for the same T. `T1` below the front pillar or `T2` past the back one
// returns `OutOfRange`, for the reason the sibling overloads' FITTED-RANGE
// ONLY note gives: past the pillars the strip's flat-clamped forward and
// `PricedSurface::forward_at`'s economic extrapolation disagree, and a
// mis-centred k = 0 biases K_var silently.
//
// `surface_certified_wing_band` is resolved ONCE and applied to both legs --
// it is part of the shared policy, not a per-leg knob.
[[nodiscard]] Result<DerivQuote>
forward_var_fair_strike(const PricedSurface& surface, double T1, double T2,
                        const DerivConfig& cfg = DerivConfig{},
                        std::optional<double> surface_certified_wing_band = std::nullopt,
                        DerivQuote* diagnostic_out = nullptr);

// Same contract as the templated `deriv_greeks` above, differentiating the
// PricedSurface-native `deriv_price`. The fitted-range gate runs ONCE, on
// `contract.maturity_t`: the theta roll then reuses that same carry CurveSet
// with a shorter contract T rather than re-deriving carry, so a contract
// sitting exactly on the front pillar rolls into the curve's flat-extrapolated
// tail instead of failing OutOfRange.
//
// A contract at or very near the FRONT fitted pillar is handled correctly
// (GK-C8): when the theta roll's T - dt would land below the front pillar,
// the carry snapshot carries a second forward + rate pillar there too, read
// off the surface's own economic extrapolation (`PricedSurface::forward_at` /
// `rate_at`) rather than the flat clamps `resolve_forward` / `YieldCurve`
// would otherwise apply outside the pillar range. This mirrors the
// SurfaceRef bridge (`carry_from_ref`), which always carries that second
// pillar.
[[nodiscard]] Result<DerivGreeks>
deriv_greeks(const PricedSurface& surface, const DerivContract& contract,
            const DerivConfig& cfg = DerivConfig{},
            const DerivGreekBumps& bumps = DerivGreekBumps{},
            std::optional<double> surface_certified_wing_band = std::nullopt);

}  // namespace atx::vol
