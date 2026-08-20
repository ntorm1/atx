#include "oracle_conventions.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

#include "atx/vol/api/pricing/greeks.hpp"

namespace atx::vol::oracle {

using atx::core::Ok;

namespace {

constexpr ConventionMap kBaseline{};

// Hard cut: this is the only production convention map, and it is PINNED from
// the deterministic Stage 3 sweep (`atx-vol-oracle-bench --convention-sweep`
// over the aggregate smoke+tune cohort) rather than hand-authored. Nothing here
// asserts the sweep's answer on trust: `convention_sweep_json` re-emits this map
// as `production_conventions` beside the winner the sweep just resolved, and the
// gate fails closed while the two differ, so every gate run re-derives the map
// and checks this literal against it.
//
// `days_per_year` is deliberately absent from the search: it is the scorecard's
// calendar DTE-banding day count, not a unit the sweep may pick (see the field's
// contract in oracle_conventions.hpp).
//
// Designated initializers: twelve consecutive doubles by position is a
// parameter-swap waiting to happen.
constexpr ConventionMap kWinner{
    .input_model = InputModel::DiscreteDividendPvSdivYield,
    // Resolved by the sweep's identity tie-break since MGTN's reclassification:
    // with the empirical table empty the two European rules route identical
    // root sets, every metric ties bit-for-bit, and the deterministic
    // source-then-identity ordering picks the lexicographically smaller
    // `european_cash_settled_index` — which is also the honest name for a
    // routing that now rests on contract facts alone.
    .exercise_style = ExerciseStyleRule::EuropeanCashSettledIndex,
    .price_scale = 1.0,
    .days_per_year = 365.0,
    .theta_days_per_year = 252.0,
    .delta_scale = 1.0,
    .gamma_scale = 1.0,
    .theta_scale = 1.0 / 252.0,
    .vega_scale = 0.01,
    .rho_scale = 0.01,
    .phi_scale = 0.01,
    .volga_source = GreekSource::Volga,
    .volga_scale = 0.0001,
    .vanna_source = GreekSource::Vanna,
    .vanna_scale = 0.01,
    .delta_decay_scale = 1.0 / 252.0,
};

// ── The exercise-style root tables ────────────────────────────────────────
//
// READ THIS BEFORE ADDING A NAME. These tables are a STAND-IN for an
// exercise-style / calcEngine column the store does not carry. A root belongs
// in kEuropeanIndexRoots only when its listing specification says the contract
// is European-exercise — a fact about the instrument, checkable against the
// exchange's contract spec, and not something this sweep fitted. A root that
// merely REPRODUCES European belongs in kEmpiricalEuropeanRoots, whose whole
// purpose is to keep "measured, unexplained" from being filed as "contract".
//
// CONTRACT FACTS, one line each:
//   SPX  Cboe S&P 500 Index option. Cash-settled, EUROPEAN exercise.
//   XSP  Cboe Mini-SPX (1/10 SPX). Cash-settled, EUROPEAN exercise.
//   MGTN Cboe Magnificent 10 Index option (options launched Dec 2025).
//        Cash-settled, EUROPEAN exercise by its listing specification:
//        https://cdn.cboe.com/resources/membership/MGTN-Index-Options-Contract-Specifications.pdf
//        RECLASSIFIED from kEmpiricalEuropeanRoots: it was routed on measured
//        reproduction alone while our ingest tagged it EQT / NMS / Stock and no
//        contract fact had been located; the Cboe contract spec above settles
//        it as a fact about the instrument, which is this table's bar.
//
// DELIBERATELY ABSENT, and why — the store's other index roots (RUT, NDX, OEX,
// XEO, MRUT, XND) are NOT here:
//   - OEX is the standing counterexample to a blanket "index root => European"
//     rule: Cboe's S&P 100 option is cash-settled but AMERICAN exercise. XEO
//     exists precisely because OEX is not European. A rule keyed on "it is an
//     index" would price OEX wrong in the other direction, which is why this
//     table is a list of named contracts and never a predicate on the root's
//     shape.
//   - RUT, NDX, XEO, MRUT and XND are European by contract spec, but NOT ONE OF
//     THEM APPEARS IN THE smoke OR tune COHORT (smoke is SPY; tune is SPY, QQQ,
//     SPX, XSP, GS, KLAC, BKNG, MGTN, MULL, DAVE). They are in the STORE, but
//     the only place they could be measured is holdout, and reading holdout to
//     decide a convention would destroy the one unbiased estimate this loop
//     will ever have of the change. Contract facts alone are not the bar this
//     axis is held to — the bar is a MEASURED reproduction — so they stay out
//     until a sanctioned cohort can show them. Adding one is one line here plus
//     a re-sweep; adding one on faith is what this comment exists to prevent.
constexpr std::array<std::string_view, 3> kEuropeanIndexRoots = {"SPX", "XSP", "MGTN"};

// MEASURED, UNEXPLAINED — the quarantine table, EMPTY today. MGTN lived here
// while its European behaviour was only a measured reproduction (ingest tags it
// EQT / NMS / Stock, and no column we read says European); the Cboe contract
// specification cited in kEuropeanIndexRoots settled it as a contract fact and
// it moved there. The table and the `..._plus_empirical` rule id survive so the
// NEXT measured, unexplained root has a home whose receipt says, in the rule's
// own identity, "we measured this and cannot explain it" — which must never be
// filed as "this is how the contract works". While this table is empty the two
// European rules route identical root sets and the sweep's identity tie-break
// deterministically prefers the plain `european_cash_settled_index` id.
constexpr std::array<std::string_view, 0> kEmpiricalEuropeanRoots{};

[[nodiscard]] bool contains_root(std::span<const std::string_view> roots,
                                 std::string_view underlier) noexcept {
  return std::find(roots.begin(), roots.end(), underlier) != roots.end();
}

[[nodiscard]] double greek_value(const AmericanGreeks &g, double dp_dq,
                                 GreekSource source) noexcept {
  switch (source) {
  case GreekSource::Delta:
    return g.delta;
  case GreekSource::Gamma:
    return g.gamma;
  case GreekSource::Theta:
    return g.theta;
  case GreekSource::Vega:
    return g.vega;
  case GreekSource::Rho:
    return g.rho;
  case GreekSource::CarryRho:
    return dp_dq;
  case GreekSource::Volga:
    return g.volga;
  case GreekSource::Vanna:
    return g.vanna;
  case GreekSource::Charm:
    return g.charm;
  }
  assert(false);
  return 0.0;
}

} // namespace

const ConventionMap &baseline_convention() noexcept { return kBaseline; }

const ConventionMap &winning_convention() noexcept { return kWinner; }

std::string_view input_model_id(InputModel model) noexcept {
  switch (model) {
  case InputModel::CurrentSpotSdivYield:
    return "uprc_spot__rate__sdiv_yield";
  case InputModel::DiscreteDividendPvSdivYield:
    return "discrete_forward_pv__rate__sdiv_yield";
  case InputModel::DiscreteForwardNetCarry:
    return "discrete_forward_net_carry__rate__sdiv_yield";
  case InputModel::DiscreteForwardRateSdivYield:
    return "discrete_forward__rate__sdiv_yield";
  case InputModel::DiscreteForwardNetRate:
    return "discrete_forward__rate_minus_sdiv__zero_carry";
  case InputModel::DiscreteForwardZeroRates:
    return "discrete_forward__zero_rate__zero_carry";
  case InputModel::DiscreteDividendPvNetRate:
    return "discrete_forward_pv__rate_minus_sdiv__zero_carry";
  case InputModel::DiscreteDividendPvRatePlusSdiv:
    return "discrete_forward_pv__rate_plus_sdiv__zero_carry";
  }
  assert(false);
  return "invalid";
}

std::string_view exercise_style_id(ExerciseStyleRule rule) noexcept {
  switch (rule) {
  case ExerciseStyleRule::AmericanAll:
    return "american_all";
  case ExerciseStyleRule::EuropeanCashSettledIndex:
    return "european_cash_settled_index";
  case ExerciseStyleRule::EuropeanCashSettledIndexPlusEmpirical:
    return "european_cash_settled_index_plus_empirical";
  }
  assert(false);
  return "invalid";
}

bool routes_european(std::string_view underlier, ExerciseStyleRule rule) noexcept {
  switch (rule) {
  case ExerciseStyleRule::AmericanAll:
    return false;
  case ExerciseStyleRule::EuropeanCashSettledIndex:
    return contains_root(kEuropeanIndexRoots, underlier);
  case ExerciseStyleRule::EuropeanCashSettledIndexPlusEmpirical:
    return contains_root(kEuropeanIndexRoots, underlier) ||
           contains_root(kEmpiricalEuropeanRoots, underlier);
  }
  assert(false);
  return false;
}

ExerciseStyle exercise_style_for(const OracleRow &row, const ConventionMap &map) noexcept {
  // The SEAM. An ingested style is fact and outranks every rule; the root list
  // answers only for a row that does not know its own exercise style, which
  // today is every row.
  if (row.ingested_exercise_style != ExerciseStyle::Unknown) {
    return row.ingested_exercise_style;
  }
  return routes_european(row.underlier, map.exercise_style) ? ExerciseStyle::European
                                                            : ExerciseStyle::American;
}

std::string_view greek_source_id(GreekSource source) noexcept {
  switch (source) {
  case GreekSource::Delta:
    return "delta";
  case GreekSource::Gamma:
    return "gamma";
  case GreekSource::Theta:
    return "theta";
  case GreekSource::Vega:
    return "vega";
  case GreekSource::Rho:
    return "rho";
  case GreekSource::CarryRho:
    return "carry_rho";
  case GreekSource::Volga:
    return "volga";
  case GreekSource::Vanna:
    return "vanna";
  case GreekSource::Charm:
    return "charm";
  }
  assert(false);
  return "invalid";
}

EnginePricingInputs mode_a_inputs(const OracleRow &row, const ConventionMap &map) noexcept {
  EnginePricingInputs in;
  const double forward = row.uprc * std::exp(row.rate * row.years) - row.ddiv;
  in.strike = row.strike;
  in.years = row.years;
  in.sigma = row.sr_vol;
  in.side = row.side;
  switch (map.input_model) {
  case InputModel::CurrentSpotSdivYield:
    in.spot = row.uprc;
    in.rate = row.rate;
    in.carry = row.sdiv;
    break;
  case InputModel::DiscreteDividendPvSdivYield:
    in.spot = forward * std::exp(-row.rate * row.years);
    in.rate = row.rate;
    in.carry = row.sdiv;
    break;
  case InputModel::DiscreteForwardNetCarry:
    in.spot = forward * std::exp(-(row.rate - row.sdiv) * row.years);
    in.rate = row.rate;
    in.carry = row.sdiv;
    break;
  case InputModel::DiscreteForwardRateSdivYield:
    in.spot = forward;
    in.rate = row.rate;
    in.carry = row.sdiv;
    break;
  case InputModel::DiscreteForwardNetRate:
    in.spot = forward;
    in.rate = row.rate - row.sdiv;
    in.carry = 0.0;
    break;
  case InputModel::DiscreteForwardZeroRates:
    in.spot = forward;
    in.rate = 0.0;
    in.carry = 0.0;
    break;
  case InputModel::DiscreteDividendPvNetRate:
    in.spot = forward * std::exp(-row.rate * row.years);
    in.rate = row.rate - row.sdiv;
    in.carry = 0.0;
    break;
  case InputModel::DiscreteDividendPvRatePlusSdiv:
    in.spot = forward * std::exp(-row.rate * row.years);
    in.rate = row.rate + row.sdiv;
    in.carry = 0.0;
    break;
  }
  return in;
}

EnginePricingInputs mode_a_inputs(const OracleRow &row) noexcept {
  return mode_a_inputs(row, winning_convention());
}

Result<AmericanGreeks> european_greeks(double S, double K, double T, double sigma, double r,
                                       double q, Side side, double *dp_dq_out) noexcept {
  // american_greeks_al's contract verbatim: which leg a row takes must not
  // change whether the row is admitted.
  if (!(S > 0.0) || !(K > 0.0) || !(T > 0.0) || !(sigma > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "european_greeks: S, K, T, sigma must be > 0");
  }
  const double m = std::exp((r - q) * T); // F/S
  const double F = S * m;
  const double df = std::exp(-r * T);
  const Black76Greeks bundle = black76_greeks(F, K, T, sigma, r, df, side);
  const Greeks &g = bundle.greeks;
  const double carry = r - q;
  const double D = g.delta; // dP/dF, the forward delta

  AmericanGreeks out;
  // NO intrinsic floor. See the header: a European premium is entitled to sit
  // below intrinsic, and on the deep-ITM index puts in this population it does.
  out.price = bundle.price;
  out.delta = m * D; // spot-delta convention, matching AmericanGreeks
  out.gamma = m * m * g.gamma;
  out.vega = g.vega;
  // r reaches the price through the discount factor AND through F, so rho
  // carries the through-forward leg the Black-76 rho holds fixed.
  out.rho = g.rho + T * F * D;
  out.theta = g.theta - carry * F * D;
  out.vanna = m * g.vanna;
  out.volga = g.volga;
  // Calendar charm is -d(spot delta)/dT at fixed S; both m(T) and F(T) = S*m(T)
  // contribute carry terms on top of the Black-76 forward charm.
  out.charm = m * (g.charm - carry * (D + F * g.gamma));
  if (dp_dq_out != nullptr) {
    // q enters only through F (dF/dq = -T*F), so dP/dq = (dP/dF)*(-T*F). Same
    // identity the American fixed-carry route uses, with no correction term.
    *dp_dq_out = -T * F * D;
  }
  return Ok(out);
}

Result<ModeAPricing> mode_a_price_row(const OracleRow &row, const ConventionMap &map,
                                      const std::optional<AlOpts> &opts) {
  ModeAPricing out;
  out.inputs = mode_a_inputs(row, map);
  out.style = exercise_style_for(row, map);
  const EnginePricingInputs &in = out.inputs;
  if (out.style == ExerciseStyle::European) {
    // One call: the European jet and its carry sensitivity share d1/d2, so
    // there is no second solve to make and no way for the two to disagree.
    const Result<AmericanGreeks> greeks = european_greeks(
        in.spot, in.strike, in.years, in.sigma, in.rate, in.carry, in.side, &out.dp_dq);
    if (!greeks.has_value()) {
      return Err(greeks.error());
    }
    out.greeks = *greeks;
    return Ok(std::move(out));
  }
  const Result<AmericanGreeks> greeks = american_greeks_al(
      in.spot, in.strike, in.years, in.sigma, in.rate, in.carry, in.side, opts);
  if (!greeks.has_value()) {
    return Err(greeks.error());
  }
  out.greeks = *greeks;
  // A failed carry solve leaves dp_dq non-finite (only the phi metric reads it)
  // rather than discarding the row's other eight Greeks.
  out.dp_dq = std::numeric_limits<double>::quiet_NaN();
  const Result<CarryGreeks> carry = american_carry_greeks_al(
      in.spot, in.strike, in.years, in.sigma, in.rate, in.carry, in.side, opts);
  if (carry.has_value()) {
    out.dp_dq = carry->dP_dq;
  }
  return Ok(std::move(out));
}

Result<double> mode_a_price(const OracleRow &row, const ConventionMap &map,
                            const std::optional<AlOpts> &opts) {
  const EnginePricingInputs in = mode_a_inputs(row, map);
  if (exercise_style_for(row, map) == ExerciseStyle::European) {
    const Result<AmericanGreeks> greeks =
        european_greeks(in.spot, in.strike, in.years, in.sigma, in.rate, in.carry, in.side);
    if (!greeks.has_value()) {
      return Err(greeks.error());
    }
    return Ok(greeks->price);
  }
  return andersen_lake(in.spot, in.strike, in.years, in.sigma, in.rate, in.carry, in.side, opts);
}

double dte_days(double years, const ConventionMap &map) noexcept {
  return years * map.days_per_year;
}

double dte_days(double years) noexcept { return dte_days(years, winning_convention()); }

double price_to_oracle_units(double engine_price, const ConventionMap &map) noexcept {
  return engine_price * map.price_scale;
}

double price_to_oracle_units(double engine_price) noexcept {
  return price_to_oracle_units(engine_price, winning_convention());
}

double price_from_oracle_units(double oracle_price, const ConventionMap &map) noexcept {
  return oracle_price / map.price_scale; // 0 scale -> inf/nan, screened by callers
}

double price_from_oracle_units(double oracle_price) noexcept {
  return price_from_oracle_units(oracle_price, winning_convention());
}

OracleUnitGreeks to_oracle_units(const AmericanGreeks &g, double dp_dq,
                                 const ConventionMap &map) noexcept {
  OracleUnitGreeks out;
  out.de = g.delta * map.delta_scale;
  out.ga = g.gamma * map.gamma_scale;
  out.th = g.theta * map.theta_scale;
  out.ve = g.vega * map.vega_scale;
  out.rh = g.rho * map.rho_scale;
  out.ph = dp_dq * map.phi_scale;
  out.vo = greek_value(g, dp_dq, map.volga_source) * map.volga_scale;
  out.va = greek_value(g, dp_dq, map.vanna_source) * map.vanna_scale;
  out.de_decay = g.charm * map.delta_decay_scale;
  return out;
}

OracleUnitGreeks to_oracle_units(const AmericanGreeks &g, double dp_dq) noexcept {
  return to_oracle_units(g, dp_dq, winning_convention());
}

} // namespace atx::vol::oracle
