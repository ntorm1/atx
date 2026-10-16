#include "quote_bench_common_seed.hpp"
#include "quote_bench_policy.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace quote_bench {
namespace {
using namespace atx::vol;
using Json = nlohmann::json;
using quote_bench_policy::keys;
using quote_bench_policy::numeric;

const char *mode_name(CommonInitializationMode mode) {
  return mode == CommonInitializationMode::QuoteSeed ? "quote-seed" : "configured";
}

bool quadrature(std::uint16_t n) {
  return n == 8 || n == 16 || n == 24 || n == 32 || n == 48 || n == 64;
}

Json seed_config_json(const AmericanMarkovSeedConfig &c) {
  const auto &a = c.inversion_options;
  return {{"max_abs_log_forward_moneyness", c.max_abs_log_forward_moneyness},
      {"max_relative_spread", c.max_relative_spread},
      {"min_time_value_over_spot", c.min_time_value_over_spot},
      {"iv_tolerance", c.iv_tolerance}, {"max_iv_iterations", c.max_iv_iterations},
      {"max_input_quotes", c.max_input_quotes}, {"max_expiries", c.max_expiries},
      {"candidates_per_expiry", c.candidates_per_expiry}, {"max_inversions", c.max_inversions},
      {"max_root_iteration_slots", c.max_root_iteration_slots},
      {"inversion_options", {{"n_collocation", a.n_collocation},
          {"n_quadrature", a.n_quadrature}, {"n_quad_price", a.n_quad_price},
          {"max_newton_iter", a.max_newton_iter}, {"tol", a.tol}}}};
}

Json band_json(const std::optional<AmericanMarkovSeedVarianceBand> &band) {
  if (!band) return nullptr;
  return {{"lower", band->lower}, {"midpoint", band->midpoint}, {"upper", band->upper}};
}

void increment(Json &counts, const char *status) {
  counts[status] = counts.value(status, std::size_t{}) + 1U;
}

void effective_vectors(CommonFitInitialization &result) {
  result.diagnostics["effective_initial_sigmas"] = result.initial_sigmas;
  result.diagnostics["effective_prior_sigmas"] = result.prior_sigmas;
}

bool valid_vector(std::span<const double> values, const AmericanMarkovFitConfig &fit,
                  std::size_t count) {
  return values.size() == count && std::all_of(values.begin(), values.end(), [&](double value) {
    return std::isfinite(value) && value >= fit.min_sigma && value <= fit.max_sigma;
  });
}
} // namespace

CommonInitializationConfig parse_common_initialization(const Json &entry) {
  keys(entry, {"mode", "seed"});
  if (!entry.contains("mode")) throw std::runtime_error("Common initialization requires mode");
  const auto mode = quote_bench_policy::text(entry.at("mode"));
  CommonInitializationConfig out;
  if (mode == "quote-seed") out.mode = CommonInitializationMode::QuoteSeed;
  else if (mode != "configured") throw std::runtime_error("Unknown common initialization mode");
  if (entry.contains("seed") && out.mode != CommonInitializationMode::QuoteSeed) {
    throw std::runtime_error("Seed controls require quote-seed initialization mode");
  }
  if (!entry.contains("seed")) return out;
  const auto &j = entry.at("seed");
  keys(j, {"max_abs_log_forward_moneyness", "max_relative_spread", "min_time_value_over_spot",
      "iv_tolerance", "max_iv_iterations", "max_input_quotes", "max_expiries",
      "candidates_per_expiry", "max_inversions", "max_root_iteration_slots", "inversion_options"});
  auto &c = out.seed;
#define FIELD(name, lower, upper) numeric(j, #name, c.name, lower, upper)
  FIELD(max_abs_log_forward_moneyness, 0, 64);
  FIELD(max_relative_spread, 0, 2);
  FIELD(min_time_value_over_spot, 0, 1);
  FIELD(iv_tolerance, 1.0e-12, 1.0e-3);
  FIELD(max_iv_iterations, 1, 128);
  FIELD(max_input_quotes, 1, kAmericanMarkovFitMaxQuotes);
  FIELD(max_expiries, 1, kAmericanMarkovSeedMaxExpiries);
  FIELD(candidates_per_expiry, 1, 32);
  FIELD(max_inversions, 3, kAmericanMarkovSeedMaxInversions);
  FIELD(max_root_iteration_slots, 1, kAmericanMarkovSeedMaxIterationSlots);
#undef FIELD
  if (c.max_root_iteration_slots < 3U * c.max_iv_iterations) {
    throw std::runtime_error("Common seed budget cannot reserve one bid/mid/ask inversion triplet");
  }
  if (j.contains("inversion_options")) {
    const auto &a = j.at("inversion_options");
    keys(a, {"n_collocation", "n_quadrature", "n_quad_price", "max_newton_iter", "tol"});
    auto &o = c.inversion_options;
    numeric(a, "n_collocation", o.n_collocation, 6, 32);
    numeric(a, "n_quadrature", o.n_quadrature, 8, 64);
    numeric(a, "n_quad_price", o.n_quad_price, 8, 64);
    numeric(a, "max_newton_iter", o.max_newton_iter, 2, 16);
    numeric(a, "tol", o.tol, 1.0e-14, 1.0e-3);
    if (!quadrature(o.n_quadrature) || !quadrature(o.n_quad_price)) {
      throw std::runtime_error("Common seed ALO quadrature must be 8,16,24,32,48 or 64");
    }
  }
  return out;
}

Json common_initialization_json(const CommonInitializationConfig &config) {
  return {{"mode", mode_name(config.mode)}, {"seed", seed_config_json(config.seed)},
      {"application", "complete seeds only; initializer error/incomplete falls back to configured "
          "initial; no fitter retry; configured prior and full quote set preserved"},
      {"iv_convention", "Andersen-Lake flat-to-expiry American IV: variance time T, "
          "r=integral(r dt)/T, q=integral(q dt)/T; aggregate PV continuous-yield approximation; "
          "dated cash refused"},
      {"aggregation", "equal quote medians within expiry; equal expiries within positive variance "
          "endpoint; equal endpoints for shared; conservative adjacent support for interval shape; "
          "flat across state knots"},
      {"limits", "support/MAD are heuristic, not statistical confidence or arbitrage bounds; "
          "root iteration slots reserve root-loop capacity, not total ALO pricing work"}};
}

Json common_initialization_pending(const CommonInitializationConfig &config) {
  return {{"mode", mode_name(config.mode)}, {"state", "not_attempted"},
      {"attempted", false}, {"applied", false}, {"reason", "before_initialization"},
      {"eligible_quotes", nullptr}, {"used_quotes", nullptr}, {"refused_quotes", nullptr},
      {"inversion_calls", 0}, {"reserved_root_iteration_slots", 0},
      {"quote_status_counts", Json::object()}, {"parameter_status_counts", Json::object()}};
}

Json common_initialization_compact(const Json &diagnostics) {
  Json result = Json::object();
  for (const auto *key : {"mode", "state", "attempted", "applied", "reason", "error_code",
       "configured_initial_source", "configured_prior_source", "effective_initial_sigmas",
       "effective_prior_sigmas", "eligible_quotes", "used_quotes", "refused_quotes",
       "inversion_calls", "reserved_root_iteration_slots", "quote_status_counts",
       "parameter_status_counts"}) {
    if (diagnostics.contains(key)) result[key] = diagnostics.at(key);
  }
  return result;
}

CommonFitInitialization configured_common_fit_initialization(
    const AmericanMarkovFitConfig &fit, std::size_t parameter_count,
    const CommonInitializationConfig &config) {
  if (parameter_count == 0 || parameter_count > fit.max_parameters ||
      parameter_count > kAmericanMarkovFitMaxParameters) {
    throw std::runtime_error("Common initialization parameter count exceeds resolved capacity");
  }
  CommonFitInitialization result;
  if (fit.initial_sigmas.empty()) result.initial_sigmas.assign(parameter_count, fit.initial_sigma);
  else result.initial_sigmas.assign(fit.initial_sigmas.begin(), fit.initial_sigmas.end());
  // Materialize BEFORE applying quotes: empty prior means the old configured
  // initial vector, not the later quote-derived initialization.
  if (fit.prior_sigmas.empty()) result.prior_sigmas = result.initial_sigmas;
  else result.prior_sigmas.assign(fit.prior_sigmas.begin(), fit.prior_sigmas.end());
  if (!valid_vector(result.initial_sigmas, fit, parameter_count) ||
      !valid_vector(result.prior_sigmas, fit, parameter_count)) {
    throw std::runtime_error("Common initialization invalid resolved configured sigma vector");
  }
  auto &d = result.diagnostics;
  d = common_initialization_pending(config);
  d["state"] = "configured";
  d["reason"] = "configured_initialization";
  d["configured_initial_source"] = fit.initial_sigmas.empty() ? "initial_sigma" : "initial_sigmas";
  d["configured_prior_source"] = fit.prior_sigmas.empty() ? "configured_initial" : "prior_sigmas";
  d["configuration"] = common_initialization_json(config);
  d["layout"] = {{"time_shape", fit.time_shape == AmericanMarkovFitTimeShape::Shared
          ? "shared" : "per-variance-interval"},
      {"log_state_knots", fit.log_state_knots.empty() ? std::vector<double>{0.0}
          : std::vector<double>{fit.log_state_knots.begin(), fit.log_state_knots.end()}},
      {"parameter_count", parameter_count}, {"max_parameters", fit.max_parameters},
      {"min_sigma", fit.min_sigma}, {"max_sigma", fit.max_sigma}};
  effective_vectors(result);
  return result;
}

void apply_common_quote_seeds(const AmericanMarkovFitMarket &market,
    std::span<const AmericanMarkovFitQuote> quotes, const AmericanMarkovFitConfig &fit,
    const CommonInitializationConfig &config, CommonFitInitialization &result) {
  if (config.mode != CommonInitializationMode::QuoteSeed) {
    throw std::runtime_error("Quote seeding must be explicitly enabled");
  }
  auto &d = result.diagnostics;
  d["attempted"] = true;
  d["input_quotes"] = quotes.size();
  const AmericanMarkovSeedLayout layout{fit.time_shape, fit.log_state_knots,
      fit.min_sigma, fit.max_sigma, fit.max_parameters};
  const auto seeds = estimate_american_markov_quote_seeds(market, quotes, layout, config.seed);
  if (!seeds) {
    d["state"] = "fallback_error";
    d["reason"] = seeds.error().to_string();
    d["error_code"] = atx::core::to_string(seeds.error().code());
    // A failed estimator Result has no work/selection ledger. Do not invent it.
    d["inversion_calls"] = nullptr;
    d["reserved_root_iteration_slots"] = nullptr;
    return;
  }
  d["eligible_quotes"] = seeds->eligible_quotes;
  d["used_quotes"] = seeds->used_quotes;
  d["refused_quotes"] = seeds->refused_quotes;
  d["inversion_calls"] = seeds->inversion_calls;
  d["reserved_root_iteration_slots"] = seeds->reserved_root_iteration_slots;
  d["inversion_error_samples"] = Json::array();
  d["inversion_errors_not_sampled"] = 0;
  d["inversion_error_sample_limit"] = 32;
  for (std::size_t index = 0; index < seeds->quotes.size(); ++index) {
    const auto &quote = seeds->quotes[index];
    increment(d["quote_status_counts"], to_string(quote.status));
    if (quote.inversion_error.empty()) continue;
    // Every refusal remains counted. Error text is a bounded diagnostic sample,
    // not a quote-selection rule; the fitter still receives all original rows.
    if (d["inversion_error_samples"].size() < 32U) {
      d["inversion_error_samples"].push_back({{"quote_index", index},
          {"error", quote.inversion_error.substr(0, 1024)},
          {"error_text_truncated", quote.inversion_error.size() > 1024U}});
    } else {
      d["inversion_errors_not_sampled"] = d["inversion_errors_not_sampled"].get<std::size_t>() + 1U;
    }
  }
  d["parameters"] = Json::array();
  for (const auto &parameter : seeds->parameters) {
    increment(d["parameter_status_counts"], to_string(parameter.status));
    d["parameters"].push_back({{"status", to_string(parameter.status)},
        {"sigma", parameter.sigma ? Json(*parameter.sigma) : Json(nullptr)},
        {"variance_rate", band_json(parameter.variance_rate)}});
  }
  d["expiries"] = Json::array();
  for (const auto &expiry : seeds->expiries) {
    d["expiries"].push_back({{"calendar_end", expiry.calendar_end},
        {"variance_end", expiry.variance_end}, {"integrated_rate", expiry.integrated_rate},
        {"integrated_yield", expiry.integrated_yield}, {"eligible_quotes", expiry.eligible_quotes},
        {"used_quotes", expiry.used_quotes}, {"total_variance", band_json(expiry.total_variance)},
        {"total_variance_mad", expiry.total_variance_mad},
        {"median_relative_spread", expiry.median_relative_spread}});
  }
  d["variance_endpoints"] = Json::array();
  for (const auto &endpoint : seeds->variance_endpoints) {
    d["variance_endpoints"].push_back({{"variance_end", endpoint.variance_end},
        {"expiry_indices", endpoint.expiry_indices}, {"supported_expiries", endpoint.supported_expiries},
        {"total_variance", band_json(endpoint.total_variance)},
        {"total_variance_mad", endpoint.total_variance_mad}});
  }
  if (seeds->initial_sigmas.empty()) {
    d["state"] = "fallback_incomplete";
    d["reason"] = "At least one requested seed parameter is unready; configured initial retained";
    return;
  }
  if (!valid_vector(seeds->initial_sigmas, fit, result.initial_sigmas.size())) {
    d["state"] = "fallback_error";
    d["reason"] = "Seed vector does not match resolved parameter count or sigma bounds";
    d["error_code"] = "seed_layout_mismatch";
    return;
  }
  result.initial_sigmas = seeds->initial_sigmas;
  d["state"] = "applied";
  d["applied"] = true;
  d["reason"] = "Complete quote seed applied; configured prior retained";
  effective_vectors(result);
}
} // namespace quote_bench
