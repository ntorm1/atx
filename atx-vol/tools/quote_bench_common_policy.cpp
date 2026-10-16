#include "quote_bench_common_policy.hpp"
#include "quote_bench_policy.hpp"

#include "atx/vol/api/pricing/american_markov_batch.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace quote_bench {
namespace {
using namespace atx::vol;
using quote_bench_policy::Json;
using quote_bench_policy::keys;
using quote_bench_policy::numeric;
using quote_bench_policy::text;

Json model_json(const AmericanMarkovConfig &c) {
  return {{"grid_nodes", c.grid_nodes}, {"upper_spot_multiple", c.upper_spot_multiple},
          {"max_calendar_step", c.max_calendar_step}, {"max_variance_step", c.max_variance_step},
          {"cfl_fraction", c.cfl_fraction}, {"max_states", c.max_states},
          {"max_transitions", c.max_transitions}, {"max_stored_rows", c.max_stored_rows},
          {"max_stored_volatility_values", c.max_stored_volatility_values}};
}

Json grid_json(const AmericanMarkovSinhGridConfig &c) {
  return {{"grid_nodes", c.grid_nodes}, {"upper_spot_multiple", c.upper_spot_multiple},
          {"concentration_scale", c.concentration_scale}, {"max_states", c.max_states}};
}

Json fit_json(const AmericanMarkovFitConfig &c) {
  const auto vector = [](std::span<const double> values) {
    return std::vector<double>(values.begin(), values.end());
  };
  return {{"log_state_knots", vector(c.log_state_knots)},
          {"initial_sigmas", vector(c.initial_sigmas)}, {"prior_sigmas", vector(c.prior_sigmas)},
          {"time_shape", c.time_shape == AmericanMarkovFitTimeShape::Shared
              ? "shared" : "per-variance-interval"},
          {"initial_sigma", c.initial_sigma}, {"min_sigma", c.min_sigma},
          {"max_sigma", c.max_sigma}, {"price_scale_floor", c.price_scale_floor},
          {"midpoint_weight", c.midpoint_weight}, {"prior_weight", c.prior_weight},
          {"strike_roughness_weight", c.strike_roughness_weight},
          {"time_roughness_weight", c.time_roughness_weight},
          {"initial_log_step", c.initial_log_step}, {"min_log_step", c.min_log_step},
          {"max_parameters", c.max_parameters}, {"max_quotes", c.max_quotes},
          {"max_evaluations", c.max_evaluations}, {"max_sweeps", c.max_sweeps},
          {"batch_options", c.batch_options}, {"max_batch_values", c.max_batch_values},
          {"max_rollback_work", c.max_rollback_work}};
}

void parse_model(const Json &j, AmericanMarkovConfig &c) {
  keys(j, {"grid_nodes", "upper_spot_multiple", "max_calendar_step", "max_variance_step",
           "cfl_fraction", "max_states", "max_transitions", "max_stored_rows",
           "max_stored_volatility_values"});
#define FIELD(name, lower, upper) numeric(j, #name, c.name, lower, upper)
  FIELD(grid_nodes, 3, kAmericanMarkovMaxStates);
  FIELD(upper_spot_multiple, 1, 1.0e6);
  FIELD(max_calendar_step, 1.0e-12, 100);
  FIELD(max_variance_step, 1.0e-12, 100);
  FIELD(cfl_fraction, 1.0e-12, 1);
  FIELD(max_states, 3, kAmericanMarkovMaxStates);
  FIELD(max_transitions, 1, kAmericanMarkovMaxTransitions);
  FIELD(max_stored_rows, 1, kAmericanMarkovMaxStoredRows);
  FIELD(max_stored_volatility_values, 1, kAmericanMarkovMaxStoredVolatilityValues);
#undef FIELD
}

void parse_grid(const Json &j, AmericanMarkovSinhGridConfig &c) {
  keys(j, {"grid_nodes", "upper_spot_multiple", "concentration_scale", "max_states"});
  numeric(j, "grid_nodes", c.grid_nodes, 3, kAmericanMarkovMaxStates);
  numeric(j, "upper_spot_multiple", c.upper_spot_multiple, 1, 1.0e6);
  numeric(j, "concentration_scale", c.concentration_scale, 1.0e-12, 1.0e6);
  numeric(j, "max_states", c.max_states, 3, kAmericanMarkovMaxStates);
}

void parse_fit(const Json &j, AmericanMarkovFitConfig &c,
                std::array<std::vector<double>, 3> &storage) {
  keys(j, {"log_state_knots", "initial_sigmas", "prior_sigmas", "time_shape",
           "initial_sigma", "min_sigma", "max_sigma", "price_scale_floor", "midpoint_weight",
           "prior_weight", "strike_roughness_weight", "time_roughness_weight", "initial_log_step",
           "min_log_step", "max_parameters", "max_quotes", "max_evaluations", "max_sweeps",
           "batch_options", "max_batch_values", "max_rollback_work"});
  if (j.contains("time_shape")) {
    const auto value = text(j.at("time_shape"));
    if (value == "shared") c.time_shape = AmericanMarkovFitTimeShape::Shared;
    else if (value == "per-variance-interval") {
      c.time_shape = AmericanMarkovFitTimeShape::PerVarianceInterval;
    } else throw std::runtime_error("Common fit time_shape is invalid");
  }
  const std::array names{"log_state_knots", "initial_sigmas", "prior_sigmas"};
  const std::array spans{&AmericanMarkovFitConfig::log_state_knots,
                        &AmericanMarkovFitConfig::initial_sigmas,
                        &AmericanMarkovFitConfig::prior_sigmas};
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (!j.contains(names[index])) continue;
    const auto &values = j.at(names[index]);
    if (!values.is_array() || values.size() > kAmericanMarkovFitMaxParameters) {
      throw std::runtime_error("Common fit vector exceeds parameter capacity");
    }
    for (const auto &value : values) {
      if (!value.is_number()) throw std::runtime_error("Common fit vector must be numeric");
      const auto x = value.get<double>();
      if (!std::isfinite(x)) throw std::runtime_error("Common fit vector must be finite");
      storage[index].push_back(x);
    }
    c.*spans[index] = storage[index];
  }
#define FIELD(name, lower, upper) numeric(j, #name, c.name, lower, upper)
  FIELD(initial_sigma, 1.0e-6, 100);
  FIELD(min_sigma, 1.0e-6, 100);
  FIELD(max_sigma, 1.0e-6, 100);
  FIELD(price_scale_floor, 1.0e-12, 1.0e6);
  FIELD(midpoint_weight, 0, 1.0e6);
  FIELD(prior_weight, 0, 1.0e6);
  FIELD(strike_roughness_weight, 0, 1.0e6);
  FIELD(time_roughness_weight, 0, 1.0e6);
  FIELD(initial_log_step, 1.0e-12, 10);
  FIELD(min_log_step, 1.0e-12, 10);
  FIELD(max_parameters, 1, kAmericanMarkovFitMaxParameters);
  FIELD(max_quotes, 1, kAmericanMarkovFitMaxQuotes);
  FIELD(max_evaluations, 1, kAmericanMarkovFitMaxEvaluations);
  FIELD(max_sweeps, 1, kAmericanMarkovFitMaxSweeps);
  FIELD(batch_options, 1, kAmericanMarkovBatchMaxOptions);
  FIELD(max_batch_values, 1, kAmericanMarkovBatchMaxValues);
  FIELD(max_rollback_work, 1, kAmericanMarkovFitMaxRollbackWork);
#undef FIELD
}

Json market_json(const AmericanMarkovMarketConfig &c) {
  return {{"dividends", to_string(c.dividends)}, {"max_quotes", c.max_quotes},
          {"max_expiries", c.max_expiries},
          {"max_spot_relative_deviation", c.max_spot_relative_deviation},
          {"max_clock_absolute_deviation", c.max_clock_absolute_deviation},
          {"max_rate_absolute_deviation", c.max_rate_absolute_deviation},
          {"max_yield_absolute_deviation", c.max_yield_absolute_deviation},
          {"max_dividend_pv_over_spot_deviation", c.max_dividend_pv_over_spot_deviation},
          {"max_prepaid_over_spot_deviation", c.max_prepaid_over_spot_deviation},
          {"max_forward_over_spot_deviation", c.max_forward_over_spot_deviation},
          {"min_calendar_increment", c.min_calendar_increment},
          {"max_absolute_interval_rate", c.max_absolute_interval_rate},
          {"max_absolute_interval_yield", c.max_absolute_interval_yield}};
}

void parse_market(const Json &j, AmericanMarkovMarketConfig &c) {
  keys(j, {"max_quotes", "max_expiries", "max_spot_relative_deviation",
           "max_clock_absolute_deviation", "max_rate_absolute_deviation",
           "max_yield_absolute_deviation", "max_dividend_pv_over_spot_deviation",
           "max_prepaid_over_spot_deviation", "max_forward_over_spot_deviation",
           "min_calendar_increment", "max_absolute_interval_rate", "max_absolute_interval_yield"});
#define FIELD(name, lower, upper) numeric(j, #name, c.name, lower, upper)
  FIELD(max_quotes, 1, kAmericanMarkovFitMaxQuotes);
  FIELD(max_expiries, 1, kAmericanMarkovMarketMaxExpiries);
  FIELD(max_spot_relative_deviation, 0, 1.0e6);
  FIELD(max_clock_absolute_deviation, 0, 100);
  FIELD(max_rate_absolute_deviation, 0, 100);
  FIELD(max_yield_absolute_deviation, 0, 100);
  FIELD(max_dividend_pv_over_spot_deviation, 0, 1.0e6);
  FIELD(max_prepaid_over_spot_deviation, 0, 1.0e6);
  FIELD(max_forward_over_spot_deviation, 0, 1.0e6);
  FIELD(min_calendar_increment, 1.0e-12, kAmericanMarkovMarketMaxYears);
  FIELD(max_absolute_interval_rate, 1.0e-12, kAmericanMarkovMarketMaxAnnualRate);
  FIELD(max_absolute_interval_yield, 1.0e-12, kAmericanMarkovMarketMaxAnnualRate);
#undef FIELD
}

void parse_regimes(const Json &j, QuoteFitRegimeConfig &c) {
  keys(j, {"dense_profile", "ordinary_profile", "sparse_profile", "near_money_log_band",
           "dense_min_median_strikes", "dense_max_median_relative_spread",
           "dense_max_bidless_fraction", "sparse_min_median_strikes",
           "sparse_min_median_relative_spread", "sparse_min_bidless_fraction"});
  if (j.contains("dense_profile")) c.dense_profile = text(j.at("dense_profile"), true);
  if (j.contains("ordinary_profile")) c.ordinary_profile = text(j.at("ordinary_profile"), true);
  if (j.contains("sparse_profile")) c.sparse_profile = text(j.at("sparse_profile"), true);
#define FIELD(name, lower, upper) numeric(j, #name, c.name, lower, upper)
  FIELD(near_money_log_band, 1.0e-6, 5);
  FIELD(dense_min_median_strikes, 4, 100000);
  FIELD(sparse_min_median_strikes, 4, 100000);
  FIELD(dense_max_median_relative_spread, 1.0e-6, 2);
  FIELD(sparse_min_median_relative_spread, 1.0e-6, 2);
  FIELD(dense_max_bidless_fraction, 0, 1);
  FIELD(sparse_min_bidless_fraction, 0, 1);
#undef FIELD
}
} // namespace

nlohmann::json common_profile_json(const AmericanMarkovPolicyProfile &p) {
  return {{"name", p.name}, {"model", model_json(p.model_config)},
          {"grid", grid_json(p.grid_config)}, {"fit", fit_json(p.fit_config)}};
}

CommonPolicy load_common_policy(const std::string &selection, const std::string &dividends) {
  const Json document = selection == "auto"
      ? Json{{"schema_version", 1}, {"model", "common-markov"}}
      : quote_bench_policy::read_document(selection);
  keys(document, {"schema_version", "model", "profiles", "symbol_overrides", "regimes",
                  "market", "equivalent_iv"});
  if (!document.contains("schema_version") || !document.at("schema_version").is_number_integer() ||
      document.at("schema_version") != 1 ||
      document.value("model", std::string{}) != "common-markov") {
    throw std::runtime_error("Common policy requires schema_version 1 and model common-markov");
  }
  const auto defaults = default_american_markov_policy_profiles();
  auto profiles = defaults;
  std::vector<std::array<std::vector<double>, 3>> storage;
  if (document.contains("profiles")) {
    const auto &list = document.at("profiles");
    if (!list.is_array() || list.empty() || list.size() > kAmericanMarkovPolicyMaxProfiles) {
      throw std::runtime_error("Common policy profiles must contain 1..64 definitions");
    }
    profiles.clear();
    profiles.reserve(list.size());
    storage.resize(list.size());
    for (std::size_t index = 0; index < list.size(); ++index) {
      const auto &entry = list[index];
      keys(entry, {"name", "base", "model", "grid", "fit"});
      const auto base = text(entry.at("base"), true);
      const auto found = std::find_if(defaults.begin(), defaults.end(),
                                     [&](const auto &profile) { return profile.name == base; });
      if (found == defaults.end()) throw std::runtime_error("Unknown common policy base: " + base);
      auto profile = *found;
      profile.name = text(entry.at("name"), true);
      if (entry.contains("model")) parse_model(entry.at("model"), profile.model_config);
      if (entry.contains("grid")) parse_grid(entry.at("grid"), profile.grid_config);
      if (entry.contains("fit")) parse_fit(entry.at("fit"), profile.fit_config, storage[index]);
      profiles.push_back(std::move(profile));
    }
  }
  std::vector<QuoteFitSymbolOverride> overrides;
  if (document.contains("symbol_overrides")) {
    const auto &list = document.at("symbol_overrides");
    if (!list.is_array() || list.size() > kAmericanMarkovPolicyMaxOverrides) {
      throw std::runtime_error("Common policy permits at most 4096 overrides");
    }
    for (const auto &entry : list) {
      keys(entry, {"symbol", "profile"});
      overrides.push_back({text(entry.at("symbol")), text(entry.at("profile"), true)});
    }
  }
  QuoteFitRegimeConfig regimes;
  if (document.contains("regimes")) parse_regimes(document.at("regimes"), regimes);
  AmericanMarkovMarketConfig market;
  market.dividends = dividends == "funding-pv"
      ? AmericanMarkovMarketDividends::FundingPvContinuousYield
      : dividends == "escrowed-pv" ? AmericanMarkovMarketDividends::EscrowedPvContinuousYield
                                  : AmericanMarkovMarketDividends::None;
  if (document.contains("market")) parse_market(document.at("market"), market);
  EquivalentIvConfig iv;
  if (document.contains("equivalent_iv")) {
    const auto &entry = document.at("equivalent_iv");
    keys(entry, {"tolerance", "min_time_value", "max_iterations"});
    numeric(entry, "tolerance", iv.tolerance, 1.0e-12, 1.0e-3);
    numeric(entry, "min_time_value", iv.min_time_value, 0, 1.0e6);
    numeric(entry, "max_iterations", iv.max_iterations, 1, 512);
  }
  auto catalog = AmericanMarkovPolicyCatalog::create(profiles, overrides, regimes);
  if (!catalog) throw std::runtime_error(catalog.error().to_string());
  Json definition{{"schema_version", 1}, {"model", "common-markov"},
      {"profiles", Json::array()}, {"symbol_overrides", Json::array()},
      {"regimes", quote_bench_policy::regimes_json(regimes)}, {"market", market_json(market)},
      {"equivalent_iv", {{"tolerance", iv.tolerance}, {"min_time_value", iv.min_time_value},
                          {"max_iterations", iv.max_iterations}}}};
  for (const auto &profile : profiles) {
    definition["profiles"].push_back(common_profile_json(profile));
  }
  for (const auto &entry : overrides) {
    definition["symbol_overrides"].push_back(
        {{"symbol", entry.symbol}, {"profile", entry.profile}});
  }
  return {std::move(*catalog), market, iv, std::move(definition)};
}
} // namespace quote_bench
