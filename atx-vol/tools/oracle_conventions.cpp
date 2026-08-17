#include "oracle_conventions.hpp"

#include <cassert>
#include <cmath>

namespace atx::vol::oracle {

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
