#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "atx/vol/api/pricing/american.hpp"
#include "oracle_convention_sweep.hpp"
#include "oracle_conventions.hpp"
#include "oracle_scorecard.hpp"

namespace {

using namespace atx::vol;
using namespace atx::vol::oracle;

// Synthesizes a row whose oracle columns were produced BY `map`, so a sweep
// over such rows must resolve back to `map`'s input model.
OracleRow make_row(double strike, Side side, const ConventionMap &map, double ddiv = 0.0,
                   double sdiv = 0.0) {
  OracleRow row;
  row.underlier = "SYNTH";
  row.side = side;
  row.strike = strike;
  row.uprc = 100.0;
  row.rate = 0.04;
  row.sdiv = sdiv;
  row.ddiv = ddiv;
  row.years = 45.0 / 365.0;
  row.sr_vol = 0.25;
  row.bid_prc = 1.0;
  row.ask_prc = 1.1;
  const EnginePricingInputs inputs = mode_a_inputs(row, map);
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
  row.sr_prc = price_to_oracle_units(greeks->price, map);
  const OracleUnitGreeks units = to_oracle_units(*greeks, carry->dP_dq, map);
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

OracleRow make_row(double strike, Side side) {
  return make_row(strike, side, baseline_convention());
}

// A cohort authored under a NON-baseline input model, so the sweep's winner arm
// and its baseline arm price different inputs and both must run per row.
ConventionMap distinct_arm_map() {
  ConventionMap map = baseline_convention();
  map.input_model = InputModel::DiscreteDividendPvSdivYield;
  return map;
}

std::vector<OracleRow> distinct_arm_rows(double first_strike, double second_strike) {
  const ConventionMap map = distinct_arm_map();
  return {make_row(first_strike, Side::Call, map, 2.5, 0.01),
          make_row(second_strike, Side::Put, map, 2.5, 0.01)};
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
  EXPECT_DOUBLE_EQ(map.days_per_year, 365.0);
  EXPECT_DOUBLE_EQ(map.theta_days_per_year, 365.25);
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

TEST(OracleConvention, BestScaleRanksOnTheSelectionPopulation) {
  std::vector<ScaleCandidate> candidates = {
      ScaleCandidate{GreekSource::Delta, 1.0, {}},
      ScaleCandidate{GreekSource::Delta, 0.01, {}},
  };
  // Candidate 0 wins the reported floor and loses the selection population.
  // Selection decides, because the reported floor includes the sub-floor rows
  // whose denominator pins.
  candidates[0].error.report.relative(1.0, 1.0);
  candidates[0].error.selection.relative(2.0, 1.0);
  candidates[1].error.report.relative(5.0, 1.0);
  candidates[1].error.selection.relative(1.5, 1.0);
  EXPECT_EQ(best_scale(candidates), 1u);
}

TEST(OracleConvention, BestScaleTieBreaksOnSourceThenNumericScale) {
  std::vector<ScaleCandidate> candidates = {
      ScaleCandidate{GreekSource::Gamma, -1.0, {}},
      ScaleCandidate{GreekSource::Delta, -0.01, {}},
      ScaleCandidate{GreekSource::Delta, -1.0, {}},
  };
  for (ScaleCandidate &candidate : candidates) {
    candidate.error.selection.relative(1.0, 1.0);
  }
  // "delta" < "gamma" on the source ID; within one source the SIGNED scale
  // orders numerically, so -1.0 beats -0.01. A formatted-string comparison
  // would order "-0.010000" first and pick the wrong candidate.
  EXPECT_EQ(best_scale(candidates), 2u);
}

TEST(OracleConvention, BestScaleWithoutSelectionEvidenceUsesCandidateIdentity) {
  const std::vector<ScaleCandidate> candidates = {
      ScaleCandidate{GreekSource::Vanna, 1.0, {}},
      ScaleCandidate{GreekSource::Volga, -1.0, {}},
      ScaleCandidate{GreekSource::Vanna, -1.0, {}},
  };
  EXPECT_EQ(best_scale(candidates), 2u);
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

TEST(OracleConvention, ThetaDayCountNeverRebucketsDteBands) {
  ConventionMap map = baseline_convention();
  map.theta_days_per_year = 360.0;
  map.theta_scale = 1.0 / 360.0;
  EXPECT_NE(convention_map_json(map).find("\"day_count\":\"ACT_360\""), std::string::npos);
  // The banding day count is a separate convention and stays pinned.
  EXPECT_DOUBLE_EQ(dte_days(1.0, map), 365.0);

  const std::vector<OracleRow> smoke = distinct_arm_rows(90.0, 110.0);
  const std::vector<OracleRow> tune = distinct_arm_rows(95.0, 105.0);
  const auto result = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_DOUBLE_EQ(result->winner.days_per_year, baseline_convention().days_per_year);
  EXPECT_DOUBLE_EQ(result->winner.days_per_year, 365.0);
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
  EXPECT_NE(json.find("\"selection_count\":"), std::string::npos);
  EXPECT_NE(json.find("not_evaluated_no_nbbo_gate"), std::string::npos);
  EXPECT_EQ(json.find("holdout"), std::string::npos);
}

TEST(OracleConvention, CandidateAndBaselineFloorsShareOneRowPopulation) {
  const std::vector<OracleRow> smoke = distinct_arm_rows(90.0, 110.0);
  const std::vector<OracleRow> tune = distinct_arm_rows(95.0, 105.0);
  const auto result = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  // Both arms must actually run: a baseline arm that silently degenerates to
  // the winner arm would satisfy the parity check for free.
  ASSERT_NE(result->winner.input_model, baseline_convention().input_model);
  ASSERT_EQ(result->metrics.size(), result->baseline_metrics.size());
  for (std::size_t index = 0; index < result->metrics.size(); ++index) {
    const FloorMetric &candidate = result->metrics[index];
    const FloorMetric &baseline = result->baseline_metrics[index];
    EXPECT_EQ(candidate.metric_id, baseline.metric_id);
    EXPECT_EQ(candidate.count, baseline.count) << candidate.metric_id;
    EXPECT_EQ(candidate.selection_count, baseline.selection_count) << candidate.metric_id;
    EXPECT_LE(candidate.selection_count, candidate.count) << candidate.metric_id;
    EXPECT_GT(candidate.count, 0) << candidate.metric_id;
  }
}

TEST(OracleConvention, SelectionExcludesSubFloorOracleRowsButStillReportsThem) {
  std::vector<OracleRow> smoke = {make_row(90.0, Side::Call), make_row(110.0, Side::Put)};
  const std::vector<OracleRow> tune = {make_row(95.0, Side::Call), make_row(105.0, Side::Put)};
  smoke[0].vo = kGreekAbsFloor / 1000.0;
  const auto result = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const auto volga = std::find_if(result->metrics.begin(), result->metrics.end(),
                                  [](const FloorMetric &metric) {
                                    return metric.metric_id == "mode_a_volga_rel";
                                  });
  ASSERT_NE(volga, result->metrics.end());
  EXPECT_EQ(volga->count, 4);
  EXPECT_EQ(volga->selection_count, 3);
}

TEST(OracleConvention, SweepJsonPublishesTheProductionMapBesideTheWinner) {
  const std::vector<OracleRow> smoke = {make_row(90.0, Side::Call), make_row(110.0, Side::Put)};
  const std::vector<OracleRow> tune = {make_row(95.0, Side::Call), make_row(105.0, Side::Put)};
  const auto result = run_convention_sweep(smoke, tune);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const std::string json = convention_sweep_json(*result, "0123456789abcdef");
  EXPECT_NE(json.find("\"production_conventions\":"), std::string::npos);
  EXPECT_NE(json.find(convention_map_json(winning_convention())), std::string::npos);
}

TEST(OracleConvention, SweepRejectsEmptyCohort) {
  const std::vector<OracleRow> one = {make_row(100.0, Side::Call)};
  EXPECT_FALSE(run_convention_sweep({}, one).has_value());
  EXPECT_FALSE(run_convention_sweep(one, {}).has_value());
}

} // namespace
