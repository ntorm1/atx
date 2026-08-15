#pragma once

// Two-date P&L attribution for a vol-derivative position (Task F-8, GK-G5 /
// GK-C2). Tier-B: additive, depends on `derivatives.hpp` for `DerivGreeks`.
//
// WHY THIS EXISTS. A swap position's daily mark move had no explain at all.
// `swap_pnl` reported the number and nothing about where it came from, which
// makes a bad day indistinguishable from a bad model -- jointly with carry
// theta this was the largest attribution hole in the library.
//
// ── WHAT THIS IS, AND WHAT IT REFUSES TO BE ────────────────────────────────
//
// This is arithmetic over quantities the CALLER measured. It holds no surface,
// prices nothing, and calls no pricer: every sensitivity comes in through
// `DerivGreeks`, every market move comes in as a pair of dated observables.
// That is deliberate. An explain that repriced internally would answer with
// the same numbers it was checking, and its residual would measure nothing.
//
//   dPV = carry + realized + vol_level + skew + convexity + discount + residual
//
// and `residual` is DEFINED as the leftover, so the sum identity is a
// tautology -- true by construction, and worth asserting only as a check that
// the assembly has no sign or transcription error. The content of this type is
// entirely in whether the named components are SMALL leftovers on a world
// where the answer is independently known. `deriv_pnl_test.cpp` establishes
// that against closed-form var-swap fixtures whose PV is written out by hand,
// not against a repricing.
//
// ── THE TERMS, AND HOW THEY AVOID DOUBLE-COUNTING ──────────────────────────
//
//   carry     = greeks.theta_zero_fixing * dt_years
//     The "nothing happened" move: one more fixing lands at a LITERAL ZERO
//     return, the calendar rolls, and the discount factor accretes. Note what
//     this already contains -- on a variance swap one day of FUTURE implied
//     variance has been replaced by a zero, so `carry` is already carrying the
//     `-K_var_future/n_total` leg. That is why `realized` below is the actual
//     fixing on its own and NOT `(r^2 - K_var_future)`: pairing a
//     realized-minus-implied surprise with a zero-fixing carry subtracts the
//     implied leg twice. (The task brief specified the paired form; it is
//     inconsistent with `theta_zero_fixing` and would leave a residual of
//     exactly `fixing_weight * K_var_future`, which
//     `RealizedTermPairsWithZeroFixingCarry` measures rather than asserts
//     away.)
//
//   realized  = fixing_weight * realized_var_dec
//     The step's actual annualized realized variance, priced at the position's
//     sensitivity to one fixing. See `var_swap_fixing_weight` for the linear
//     (variance) case; a vol swap's fixing sensitivity is not linear and a
//     caller must supply its own local weight.
//
//   vol_level = greeks.vega * (to.sigma_atm - from.sigma_atm)
//   skew      = greeks.skew_vega * (to.skew_slope - from.skew_slope)
//   convexity = greeks.convexity_vega * (to.smile_curvature - from.smile_curvature)
//     The three smile moves, each against the matching F-7 sensitivity. The
//     slope and curvature conventions are `DerivGreeks::skew_vega`'s: vol per
//     unit k and per unit k^2, with k = ln(K/F). A caller measuring slope in
//     any other units gets a wrong number here, not a flagged one.
//
//   discount  = greeks.rho * (to.zero_rate - from.zero_rate)
//     The discount CURVE moving. The roll DOWN the curve is already inside
//     `carry` (theta rolls T and re-reads the curve), so this term is the rate
//     level change only and the two do not overlap.
//
// Everything else -- gamma against the spot move, second-order vol, a change
// in the strip's own resolution, a pricer boundary crossed between the two
// dates -- lands in `residual` by design. A first-order explain that silently
// absorbed its own higher-order terms would be a worse instrument than one
// whose leftover is visible.
//
// ── NaN IS A FIRST-CLASS ANSWER ────────────────────────────────────────────
//
// `theta_zero_fixing` is legitimately NaN whenever the roll could not happen
// (a contract shorter than the bump, `carry_theta` off, a dispatch-engine
// boundary the centre was never in, a failed strip). `skew_vega` and
// `convexity_vega` are NaN unless `DerivGreekBumps::smile_greeks` was on.
// Those are "not computed", not "zero", and this type keeps them distinct: an
// unavailable sensitivity yields a NaN component, poisons `residual`, and
// raises the matching `DerivPnlFlags` bit. Nothing here substitutes a zero,
// because a zero would read as a measurement.

#include <cstdint>

#include "atx/vol/api/pricing/derivatives.hpp" // DerivGreeks, DerivContract, kQuietNaN
#include "atx/vol/api/core/types.hpp"          // Result

namespace atx::vol {

// Which components could not be computed, and therefore which NaNs in the
// result are "not measured" rather than "measured as NaN".
enum class DerivPnlFlags : std::uint32_t {
  None = 0u,
  MarkUnavailable = 1u << 0,       // either date's `pv` was not finite
  CarryUnavailable = 1u << 1,      // theta_zero_fixing NaN (see its own doc)
  RealizedUnavailable = 1u << 2,   // no fixing weight or no realized variance
  VolLevelUnavailable = 1u << 3,   // vega or an ATM vol not finite
  SkewUnavailable = 1u << 4,       // skew_vega NaN -- smile_greeks was off
  ConvexityUnavailable = 1u << 5,  // convexity_vega NaN -- smile_greeks was off
  DiscountUnavailable = 1u << 6,   // rho or a zero rate not finite
};

[[nodiscard]] constexpr DerivPnlFlags operator|(DerivPnlFlags a, DerivPnlFlags b) noexcept {
  return static_cast<DerivPnlFlags>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr DerivPnlFlags& operator|=(DerivPnlFlags& a, DerivPnlFlags b) noexcept {
  a = a | b;
  return a;
}
[[nodiscard]] constexpr bool has_flag(DerivPnlFlags value, DerivPnlFlags flag) noexcept {
  return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0u;
}

// One date's mark and the market state behind it. Every field is in the same
// convention on both dates; this type does not check that, because it cannot.
struct DerivPnlMark {
  double pv = kQuietNaN;               // the position's mark, same scaling both dates
  double sigma_atm = kQuietNaN;        // ATM implied vol at the contract's tenor
  double skew_slope = kQuietNaN;       // d(sigma)/dk at k = 0, k = ln(K/F)
  double smile_curvature = kQuietNaN;  // d2(sigma)/dk2-scale coefficient at k = 0
  double zero_rate = kQuietNaN;        // continuously-compounded zero to maturity
};

// Drift pin: five fields. A sixth observable means a sixth explain term or an
// explicit decision to leave it in the residual -- neither should be silent.
static_assert(detail::aggregate_arity_is_v<DerivPnlMark, 5>,
              "DerivPnlMark field count changed: update this pin.");

struct DerivPnlInputs {
  DerivPnlMark from{};
  DerivPnlMark to{};

  // AS OF `from`. Every sensitivity is evaluated at the start of the step, so
  // the explain is a forward prediction of the move rather than a hindsight
  // fit -- which is what makes a large residual informative.
  DerivGreeks greeks{};

  double dt_years = 0.0;

  // Annualized variance realized over (from, to], i.e. what one fixing of the
  // contract's own `rv_spec` observed. NaN when the step realized nothing
  // measurable (a stale or holiday date), which flags `realized` rather than
  // pricing it as a zero return -- those are different days.
  double realized_var_dec = kQuietNaN;

  // dPV per unit of annualized fixing variance. `var_swap_fixing_weight`
  // computes it for a linear variance leg; anything else is the caller's own
  // local sensitivity.
  double fixing_weight = kQuietNaN;
};

static_assert(detail::aggregate_arity_is_v<DerivPnlInputs, 6>,
              "DerivPnlInputs field count changed: update this pin.");

struct DerivPnlExplain {
  double d_pv = kQuietNaN;  // to.pv - from.pv
  double carry = kQuietNaN;
  double realized = kQuietNaN;
  double vol_level = kQuietNaN;
  double skew = kQuietNaN;
  double convexity = kQuietNaN;
  double discount = kQuietNaN;
  // d_pv minus the six components. NaN whenever any component is, so a
  // partially-attributed step cannot masquerade as a fully-explained one.
  double residual = kQuietNaN;
  DerivPnlFlags flags = DerivPnlFlags::None;
};

// Drift pin: nine fields. A new component is an appended field AND a new term
// in the identity -- both must move together or the residual stops meaning
// what its own doc says.
static_assert(detail::aggregate_arity_is_v<DerivPnlExplain, 9>,
              "DerivPnlExplain field count changed: update this pin.");

// dPV per unit of one fixing's annualized variance for a LINEAR variance leg:
// `df * notional / n_obs_total`. A variance swap's accrued leg is the running
// mean of the fixings, so one more fixing at annualized rate r^2 moves the
// undiscounted expectation by exactly `r^2 / n_obs_total` -- no model, no
// smile, just the contract's own averaging.
//
// NaN for any kind whose payoff is not linear in realized variance (VolSwap
// and both capped kinds), and for a contract with no fixing schedule. Those
// callers must supply their own local weight; returning a linear number for a
// concave payoff would be a wrong measurement rather than a missing one.
[[nodiscard]] double var_swap_fixing_weight(const DerivContract& contract, double df) noexcept;

// Attribute `to.pv - from.pv` across the six named components.
//
// Fails only on a caller error the attribution cannot be defined against: a
// non-finite or negative `dt_years`. Every other unusable input is reported
// through `DerivPnlFlags` and a NaN component, because "this step could not
// be attributed" is an answer a P&L report has to be able to print.
[[nodiscard]] Result<DerivPnlExplain> deriv_pnl_explain(const DerivPnlInputs& in);

}  // namespace atx::vol
