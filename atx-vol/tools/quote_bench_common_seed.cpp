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

const char *estimator_name(const CommonInitializationConfig &config) {
  if (config.interval_fallback) return "bid-mid-ask-then-sigma-interval";
  return config.seed.estimator == AmericanMarkovSeedEstimator::SigmaInterval
      ? "sigma-interval" : "bid-mid-ask";
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

Json interval_json(const std::optional<AmericanMarkovSeedSigmaInterval> &s) {
  if (!s) return nullptr;
  const auto optional = [](const std::optional<double> &x) { return x ? Json(*x) : Json(nullptr); };
  return {{"lower", optional(s->lower)}, {"upper", optional(s->upper)},
          {"effective_lower", s->effective_lower}, {"effective_upper", s->effective_upper},
          {"geometric_midpoint", s->geometric_midpoint}, {"log_width", s->log_width},
          {"crossed", s->crossed}, {"lower_bound_quotes", s->lower_bound_quotes},
          {"upper_bound_quotes", s->upper_bound_quotes}};
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

const char *common_time_shape_name(AmericanMarkovFitTimeShape shape) {
  switch (shape) {
  case AmericanMarkovFitTimeShape::Shared: return "shared";
  case AmericanMarkovFitTimeShape::PerVarianceInterval: return "per-variance-interval";
  case AmericanMarkovFitTimeShape::VarianceTimeKnots: return "variance-time-knots";
  }
  // Policy validation refuses out-of-range enumerators before any report.
  throw std::runtime_error("Common fit time_shape is invalid");
}

const char *common_search_name(AmericanMarkovFitSearch search) {
  switch (search) {
  case AmericanMarkovFitSearch::CoordinatePattern: return "coordinate-pattern";
  case AmericanMarkovFitSearch::LevenbergMarquardt: return "levenberg-marquardt";
  }
  throw std::runtime_error("Common fit search is invalid");
}

CommonInitializationConfig parse_common_initialization(const Json &entry) {
  keys(entry, {"mode", "seed", "prior"});
  if (!entry.contains("mode")) throw std::runtime_error("Common initialization requires mode");
  const auto mode = quote_bench_policy::text(entry.at("mode"));
  CommonInitializationConfig out;
  if (mode == "quote-seed") out.mode = CommonInitializationMode::QuoteSeed;
  else if (mode != "configured") throw std::runtime_error("Unknown common initialization mode");
  if ((entry.contains("seed") || entry.contains("prior")) &&
      out.mode != CommonInitializationMode::QuoteSeed) {
    throw std::runtime_error("Seed controls require quote-seed initialization mode");
  }
  if (entry.contains("prior")) {
    const auto prior = quote_bench_policy::text(entry.at("prior"));
    if (prior == "seed") out.prior_from_seed = true;
    else if (prior != "configured") {
      throw std::runtime_error("Common initialization prior must be configured or seed");
    }
  }
  if (!entry.contains("seed")) return out;
  const auto &j = entry.at("seed");
  keys(j, {"max_abs_log_forward_moneyness", "max_relative_spread", "min_time_value_over_spot",
      "iv_tolerance", "max_iv_iterations", "max_input_quotes", "max_expiries",
      "candidates_per_expiry", "max_inversions", "max_root_iteration_slots", "inversion_options",
      "estimator"});
  auto &c = out.seed;
  if (j.contains("estimator")) {
    const auto name = quote_bench_policy::text(j.at("estimator"));
    if (name == "sigma-interval") {
      c.estimator = AmericanMarkovSeedEstimator::SigmaInterval;
    } else if (name == "bid-mid-ask-then-sigma-interval") {
      out.interval_fallback = true;
    } else if (name != "bid-mid-ask") {
      throw std::runtime_error("Common seed estimator must be bid-mid-ask, sigma-interval or "
                               "bid-mid-ask-then-sigma-interval");
    }
  }
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
  Json result{{"mode", mode_name(config.mode)}, {"seed", seed_config_json(config.seed)},
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
  // Added only when opted in, so the default expanded definition keeps its bytes.
  if (config.interval_fallback ||
      config.seed.estimator != AmericanMarkovSeedEstimator::BidMidAsk) {
    result["seed"]["estimator"] = estimator_name(config);
    result["interval_estimator"] = "near-forward quotes incl. bidless/wide; ask-implied sigma is "
        "an upper bound, bid-implied sigma (bid above intrinsic floor) a lower bound; feasible "
        "interval = [max lower, min upper], missing side = layout sigma bound; seed = geometric "
        "midpoint (shared: all used quotes; per-interval: endpoint midpoints differenced); "
        "refused only when unbounded both ways; crossed intervals reported, never repaired; "
        "fallback runs only after an incomplete bid-mid-ask seed, within the remaining budget";
  }
  if (config.prior_from_seed) {
    result["prior"] = "seed";
    result["prior_application"] = "an applied quote seed (any estimator) also replaces the "
        "regularization prior centre; refused/incomplete seeds keep the configured prior";
  }
  return result;
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
  d["layout"] = {{"time_shape", common_time_shape_name(fit.time_shape)},
      {"log_state_knots", fit.log_state_knots.empty() ? std::vector<double>{0.0}
          : std::vector<double>{fit.log_state_knots.begin(), fit.log_state_knots.end()}},
      {"parameter_count", parameter_count}, {"max_parameters", fit.max_parameters},
      {"min_sigma", fit.min_sigma}, {"max_sigma", fit.max_sigma}};
  // Legacy layouts keep their former key set; only the knot mode adds coordinates.
  if (fit.time_shape == AmericanMarkovFitTimeShape::VarianceTimeKnots) {
    d["layout"]["variance_time_knots"] = std::vector<double>(fit.variance_time_knots.begin(),
                                                             fit.variance_time_knots.end());
  }
  effective_vectors(result);
  return result;
}

namespace {
// The configured prior was materialized before seeding; only prior "seed"
// replaces it, and only with a complete applied seed vector.
void use_seed(const std::vector<double> &seed, const CommonInitializationConfig &config,
              CommonFitInitialization &result) {
  result.initial_sigmas = seed;
  if (config.prior_from_seed) {
    result.prior_sigmas = seed;
    result.diagnostics["effective_prior_source"] = "quote_seed";
  }
  effective_vectors(result);
}

// Every count, refusal, parameter and endpoint of one estimator call. Interval
// provenance (which quotes bounded sigma, interval widths) only for SigmaInterval.
void record_seeds(const AmericanMarkovQuoteSeeds &seeds, Json &d) {
  // The pending record pre-creates both counters; a fallback record does not.
  for (const auto *key : {"quote_status_counts", "parameter_status_counts"}) {
    if (!d[key].is_object()) d[key] = Json::object();
  }
  d["eligible_quotes"] = seeds.eligible_quotes;
  d["used_quotes"] = seeds.used_quotes;
  d["refused_quotes"] = seeds.refused_quotes;
  d["inversion_calls"] = seeds.inversion_calls;
  d["reserved_root_iteration_slots"] = seeds.reserved_root_iteration_slots;
  d["inversion_error_samples"] = Json::array();
  d["inversion_errors_not_sampled"] = 0;
  d["inversion_error_sample_limit"] = 32;
  for (std::size_t index = 0; index < seeds.quotes.size(); ++index) {
    const auto &quote = seeds.quotes[index];
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
  for (const auto &parameter : seeds.parameters) {
    increment(d["parameter_status_counts"], to_string(parameter.status));
    d["parameters"].push_back({{"status", to_string(parameter.status)},
        {"sigma", parameter.sigma ? Json(*parameter.sigma) : Json(nullptr)},
        {"variance_rate", band_json(parameter.variance_rate)}});
  }
  const bool interval = seeds.configuration.estimator == AmericanMarkovSeedEstimator::SigmaInterval;
  d["expiries"] = Json::array();
  for (const auto &expiry : seeds.expiries) {
    d["expiries"].push_back({{"calendar_end", expiry.calendar_end},
        {"variance_end", expiry.variance_end}, {"integrated_rate", expiry.integrated_rate},
        {"integrated_yield", expiry.integrated_yield}, {"eligible_quotes", expiry.eligible_quotes},
        {"used_quotes", expiry.used_quotes}, {"total_variance", band_json(expiry.total_variance)},
        {"total_variance_mad", expiry.total_variance_mad},
        {"median_relative_spread", expiry.median_relative_spread}});
    if (interval) d["expiries"].back()["sigma_interval"] = interval_json(expiry.sigma_interval);
  }
  d["variance_endpoints"] = Json::array();
  for (const auto &endpoint : seeds.variance_endpoints) {
    d["variance_endpoints"].push_back({{"variance_end", endpoint.variance_end},
        {"expiry_indices", endpoint.expiry_indices}, {"supported_expiries", endpoint.supported_expiries},
        {"total_variance", band_json(endpoint.total_variance)},
        {"total_variance_mad", endpoint.total_variance_mad}});
    if (interval) {
      d["variance_endpoints"].back()["sigma_interval"] = interval_json(endpoint.sigma_interval);
    }
  }
  if (!interval) return;
  d["estimator"] = "sigma-interval";
  d["sigma_interval"] = interval_json(seeds.sigma_interval);
  d["interval_quotes"] = Json::array();
  for (std::size_t index = 0; index < seeds.quotes.size(); ++index) {
    const auto &quote = seeds.quotes[index];
    if (quote.status != AmericanMarkovSeedQuoteStatus::Used) continue;
    d["interval_quotes"].push_back({{"quote_index", index},
        {"lower_sigma", quote.lower_bound ? Json(*quote.equivalent_vols[0]) : Json(nullptr)},
        {"upper_sigma", quote.upper_bound ? Json(*quote.equivalent_vols[2]) : Json(nullptr)}});
  }
}

// SigmaInterval retry after an incomplete BidMidAsk seed, inside the same
// configured inversion/slot caps: it receives only what the primary left.
void apply_interval_fallback(const AmericanMarkovFitMarket &market,
    std::span<const AmericanMarkovFitQuote> quotes, const AmericanMarkovFitConfig &fit,
    const AmericanMarkovSeedLayout &layout, const CommonInitializationConfig &config,
    const AmericanMarkovQuoteSeeds &primary, CommonFitInitialization &result) {
  auto &d = result.diagnostics;
  Json f{{"attempted", false}, {"applied", false}};
  auto seed = config.seed;
  seed.estimator = AmericanMarkovSeedEstimator::SigmaInterval;
  seed.max_inversions = config.seed.max_inversions - primary.inversion_calls;
  seed.max_root_iteration_slots =
      config.seed.max_root_iteration_slots - primary.reserved_root_iteration_slots;
  f["remaining_max_inversions"] = seed.max_inversions;
  f["remaining_max_root_iteration_slots"] = seed.max_root_iteration_slots;
  if (seed.max_inversions < 3U || seed.max_root_iteration_slots < 3U * seed.max_iv_iterations) {
    f["state"] = "budget_exhausted";
    d["interval_fallback"] = std::move(f);
    return;
  }
  f["attempted"] = true;
  const auto seeds = estimate_american_markov_quote_seeds(market, quotes, layout, seed);
  if (!seeds) {
    f["state"] = "error";
    f["reason"] = seeds.error().to_string();
    f["error_code"] = atx::core::to_string(seeds.error().code());
    d["interval_fallback"] = std::move(f);
    return;
  }
  record_seeds(*seeds, f);
  // Top-level work is the total over both estimator calls.
  d["primary_inversion_calls"] = primary.inversion_calls;
  d["primary_reserved_root_iteration_slots"] = primary.reserved_root_iteration_slots;
  d["inversion_calls"] = primary.inversion_calls + seeds->inversion_calls;
  d["reserved_root_iteration_slots"] =
      primary.reserved_root_iteration_slots + seeds->reserved_root_iteration_slots;
  if (seeds->initial_sigmas.empty()) {
    f["state"] = "incomplete";
  } else if (!valid_vector(seeds->initial_sigmas, fit, result.initial_sigmas.size())) {
    f["state"] = "layout_mismatch";
  } else {
    f["state"] = "applied";
    f["applied"] = true;
    d["state"] = "applied_interval_fallback";
    d["applied"] = true;
    d["reason"] = config.prior_from_seed
        ? "Bid-mid-ask seed incomplete; sigma-interval seed applied as initial and prior"
        : "Bid-mid-ask seed incomplete; sigma-interval seed applied; configured prior retained";
    use_seed(seeds->initial_sigmas, config, result);
  }
  d["interval_fallback"] = std::move(f);
}
} // namespace

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
    // Its refusals are layout/schedule validation, which a fallback shares.
    d["inversion_calls"] = nullptr;
    d["reserved_root_iteration_slots"] = nullptr;
    return;
  }
  record_seeds(*seeds, d);
  if (seeds->initial_sigmas.empty()) {
    d["state"] = "fallback_incomplete";
    d["reason"] = "At least one requested seed parameter is unready; configured initial retained";
    if (config.interval_fallback) {
      apply_interval_fallback(market, quotes, fit, layout, config, *seeds, result);
    }
    return;
  }
  if (!valid_vector(seeds->initial_sigmas, fit, result.initial_sigmas.size())) {
    d["state"] = "fallback_error";
    d["reason"] = "Seed vector does not match resolved parameter count or sigma bounds";
    d["error_code"] = "seed_layout_mismatch";
    return;
  }
  d["state"] = "applied";
  d["applied"] = true;
  d["reason"] = config.prior_from_seed ? "Complete quote seed applied as initial and prior"
                                       : "Complete quote seed applied; configured prior retained";
  use_seed(seeds->initial_sigmas, config, result);
}
} // namespace quote_bench
