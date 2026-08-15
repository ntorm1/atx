#include "oracle_conventions.hpp"

#include <cmath>

// STAGE-2 HYPOTHESIS MAP. Every mapping below is an initial hypothesis chosen
// to stand the tool up; charter stage 3 (iteration 0) round-trips SpiderRock's
// own numbers through candidate conventions and REWRITES this file against the
// measured residual floor. Each hypothesis is annotated with the alternative
// iteration 0 must test. Nothing outside this TU may depend on these choices.

namespace atx::vol::oracle {

namespace {

// HYPOTHESIS: SpiderRock `years` is an ACT/365 calendar year fraction.
// Alternatives for iteration 0: ACT/252 (business), ACT/365.25.
constexpr double kDaysPerYear = 365.0;

// HYPOTHESIS: ve/rh/ph/vo/va are quoted per POINT (per 0.01 of vol / rate /
// yield), i.e. engine derivative x 0.01. Alternatives: per 1.00 (no scaling),
// and for vo a per-point^2 (x 1e-4) double scaling.
constexpr double kPerPoint = 0.01;

} // namespace

EnginePricingInputs mode_a_inputs(const OracleRow &row) noexcept {
  EnginePricingInputs in;
  in.spot = row.uprc;    // HYPOTHESIS: uPrc is the pricing spot (mid underlier)
  in.strike = row.strike;
  in.years = row.years;  // their own year fraction, used verbatim in Mode A
  in.sigma = row.sr_vol; // Mode A prices AT SpiderRock's own vol
  in.rate = row.rate;    // HYPOTHESIS: continuously-compounded
  // HYPOTHESIS: sdiv is a continuous dividend/borrow yield -> the pricer's q.
  // ddiv (discrete dividend stream) is NOT applied in stage 2: whether it is
  // escrowed into spot, folded into an effective q, or already inside sdiv is
  // exactly the kind of question iteration 0's round-trip resolves.
  in.carry = row.sdiv;
  in.side = row.side;
  return in;
}

double dte_days(double years) noexcept { return years * kDaysPerYear; }

double price_to_oracle_units(double engine_price) noexcept {
  // HYPOTHESIS: srPrc is a per-share premium in the same currency unit the
  // engine prices in (identity map). Alternative: per-contract (x100).
  return engine_price;
}

OracleUnitGreeks to_oracle_units(const AmericanGreeks &g, double dp_dq) noexcept {
  OracleUnitGreeks out;
  out.de = g.delta; // HYPOTHESIS: spot delta, unscaled
  out.ga = g.gamma; // HYPOTHESIS: d(delta)/dS, unscaled
  // HYPOTHESIS: th is theta per CALENDAR DAY (engine theta is per year).
  // Alternative: per business day (/252).
  out.th = g.theta / kDaysPerYear;
  out.ve = g.vega * kPerPoint;
  out.rh = g.rho * kPerPoint;
  // HYPOTHESIS: ph ("phi") is the dividend-yield rho, dP/dq per point. NaN
  // propagates when the carry route was unavailable — the caller skips the
  // ph observation for that row.
  out.ph = dp_dq * kPerPoint;
  // HYPOTHESIS (charter): vo = volga, va = vanna, both per point. The reversed
  // assignment is the first alternative iteration 0 must test.
  out.vo = g.volga * kPerPoint;
  out.va = g.vanna * kPerPoint;
  // HYPOTHESIS: deDecay is charm (delta decay) per calendar day.
  out.de_decay = g.charm / kDaysPerYear;
  return out;
}

} // namespace atx::vol::oracle
