#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "oracle_convention_sweep.hpp"
#include "oracle_conventions.hpp"

namespace {

using namespace atx::vol;
using namespace atx::vol::oracle;

OracleRow make_row(double strike, Side side) {
  OracleRow row;
  row.underlier = "SYNTH";
  row.side = side;
  row.strike = strike;
  row.uprc = 100.0;
  row.rate = 0.04;
  row.sdiv = 0.0;
  row.ddiv = 0.0;
  row.years = 45.0 / 365.0;
  row.sr_vol = 0.25;
  row.bid_prc = 1.0;
  row.ask_prc = 1.1;
  const EnginePricingInputs inputs = mode_a_inputs(row, baseline_convention());
  const auto greeks = american_greeks_al(inputs.spot, inputs.strike, inputs.years, inputs.sigma,
                                         inputs.rate, inputs.carry, inputs.side, al_fast_opts());
  EXPECT_TRUE(greeks.has_value());
  if (!greeks.has_value()) {
    return row;
  }
  const auto carry =
      american_carry_greeks_al(inputs.spot, inputs.strike, inputs.years, inputs.sigma, inputs.rate,
                               inputs.carry, inputs.side, al_fast_opts());
  EXPECT_TRUE(carry.has_value());
  if (!carry.has_value()) {
    return row;
  }
  row.sr_prc = greeks->price;
  const OracleUnitGreeks units = to_oracle_units(*greeks, carry->dP_dq, baseline_convention());
  row.de = units.de;
  row.ga = units.ga;
  row.th = units.th;
  row.ve = units.ve;
  row.rh = units.rh;
  row.ph = units.ph;
  row.vo = units.vo;
  row.va = units.va;
  row.de_decay = units.de_decay;
  return row;
}

TEST(OracleConvention, DiscreteDividendForwardIsAppliedExactly) {
  OracleRow row;
  row.uprc = 123.0;
  row.rate = 0.037;
  row.sdiv = 0.012;
  row.ddiv = 4.25;
  row.years = 1.75;
  ConventionMap map = baseline_convention();
  map.input_model = InputModel::DiscreteDividendPvSdivYield;
  const EnginePricingInputs inputs = mode_a_inputs(row, map);
  const double documented_forward = row.uprc * std::exp(row.rate * row.years) - row.ddiv;
  EXPECT_DOUBLE_EQ(inputs.spot, documented_forward * std::exp(-row.rate * row.years));
  EXPECT_DOUBLE_EQ(inputs.rate, row.rate);
  EXPECT_DOUBLE_EQ(inputs.carry, row.sdiv);
}

TEST(OracleConvention, ProductionMapIsTheResolvedHardCut) {
  const ConventionMap &map = winning_convention();
  EXPECT_EQ(map.input_model, InputModel::DiscreteDividendPvSdivYield);
  EXPECT_DOUBLE_EQ(map.price_scale, 1.0);
  EXPECT_DOUBLE_EQ(map.days_per_year, 365.25);
  EXPECT_DOUBLE_EQ(map.delta_scale, 1.0);
  EXPECT_DOUBLE_EQ(map.gamma_scale, 1.0);
  EXPECT_DOUBLE_EQ(map.theta_scale, 1.0 / 365.25);
  EXPECT_DOUBLE_EQ(map.vega_scale, 0.01);
  EXPECT_DOUBLE_EQ(map.rho_scale, 0.01);
  EXPECT_DOUBLE_EQ(map.phi_scale, 0.0001);
  EXPECT_EQ(map.volga_source, GreekSource::Volga);
  EXPECT_DOUBLE_EQ(map.volga_scale, 0.0001);
  EXPECT_EQ(map.vanna_source, GreekSource::Vanna);
  EXPECT_DOUBLE_EQ(map.vanna_scale, 0.01);
  EXPECT_DOUBLE_EQ(map.delta_decay_scale, 1.0 / 365.25);
}

TEST(OracleConvention, RelativeScaleSelectionUsesStableNumericTieBreak) {
  const std::array<ScaleObservation, 2> observations = {ScaleObservation{100.0, -1.0},
                                                        ScaleObservation{200.0, -2.0}};
  const std::array<double, 4> candidates = {1.0, 0.01, -0.01, -1.0};
  EXPECT_DOUBLE_EQ(select_relative_scale(observations, candidates), -0.01);

  const std::array<ScaleObservation, 1> zero = {ScaleObservation{0.0, 0.0}};
  const std::array<double, 2> tie = {1.0, -1.0};
  EXPECT_DOUBLE_EQ(select_relative_scale(zero, tie), -1.0);
}

TEST(OracleConvention, CompleteMapNamesEveryGreekSignAndScale) {
  const std::string json = convention_map_json(baseline_convention());
  for (const char *token :
       {"input_model",     "forward_formula", "rate_model",        "carry_model",
        "dividend_model",  "day_count",       "price_scale",       "vol_scale",
        "delta_scale",     "delta_sign",      "gamma_scale",       "gamma_sign",
        "theta_basis",     "theta_sign",      "vega_scale",        "vega_sign",
        "rho_scale",       "rho_sign",        "phi_scale",         "phi_sign",
        "volga_source",    "volga_scale",     "volga_sign",        "vanna_source",
        "vanna_scale",     "vanna_sign",      "delta_decay_basis", "delta_decay_day_count",
        "delta_decay_sign"}) {
    EXPECT_NE(json.find(std::string{"\""} + token + "\""), std::string::npos) << token;
  }
}

TEST(OracleConvention, SweepIsClosedDeterministicAndCoversElevenMetrics) {
  const std::vector<OracleRow> smoke = {make_row(90.0, Side::Call), make_row(110.0, Side::Put)};
  const std::vector<OracleRow> tune = {make_row(95.0, Side::Call), make_row(105.0, Side::Put)};
  const auto first = run_convention_sweep(smoke, tune);
  const auto second = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(first.has_value()) << first.error().to_string();
  ASSERT_TRUE(second.has_value()) << second.error().to_string();
  ASSERT_EQ(first->metrics.size(), 11u);
  ASSERT_EQ(first->baseline_metrics.size(), 11u);
  ASSERT_EQ(first->candidate_prices.size(), 8u);
  EXPECT_EQ(std::count_if(first->candidate_prices.begin(), first->candidate_prices.end(),
                          [](const CandidatePriceMetric &candidate) {
                            return candidate.tune_sample_count > 0;
                          }),
            2);
  EXPECT_EQ(convention_map_json(first->winner), convention_map_json(second->winner));
  const std::string json = convention_sweep_json(*first, "0123456789abcdef");
  EXPECT_NE(json.find("\"cohorts\":[\"smoke\",\"tune\"]"), std::string::npos);
  EXPECT_NE(json.find("\"oracle_suspect_candidates\":[]"), std::string::npos);
  EXPECT_NE(json.find("not_evaluated_no_nbbo_gate"), std::string::npos);
  EXPECT_EQ(json.find("holdout"), std::string::npos);
}

TEST(OracleConvention, SweepRejectsEmptyCohort) {
  const std::vector<OracleRow> one = {make_row(100.0, Side::Call)};
  EXPECT_FALSE(run_convention_sweep({}, one).has_value());
  EXPECT_FALSE(run_convention_sweep(one, {}).has_value());
}

} // namespace
