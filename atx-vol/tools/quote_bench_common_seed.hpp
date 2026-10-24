#pragma once

#include "atx/vol/api/fitting/american_markov_seed.hpp"

#include <cstddef>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

namespace quote_bench {

enum class CommonInitializationMode { Configured, QuoteSeed };

struct CommonInitializationConfig {
  CommonInitializationMode mode = CommonInitializationMode::Configured;
  // seed.estimator selects the primary estimator. interval_fallback (policy
  // spelling "bid-mid-ask-then-sigma-interval") runs SigmaInterval only when the
  // BidMidAsk seed is incomplete (state fallback_incomplete), within the SAME
  // configured inversion/slot budgets (the remainder after the primary
  // attempt). A primary estimator ERROR never runs it: errors are layout or
  // schedule refusals that both estimators share, reported as fallback_error.
  atx::vol::AmericanMarkovSeedConfig seed;
  bool interval_fallback = false;
  // Policy "prior": "seed". With wide/bidless quotes the hinge interval loss is
  // flat, so only the prior acts and a fit returns to the configured prior
  // centre whatever the seed. This opt-in makes an applied seed vector the
  // prior centre too; incomplete/refused seeds keep the configured prior. A
  // one-sided sigma interval is a refusal (state fallback_one_sided_interval),
  // so a layout sigma bound never becomes the prior centre.
  bool prior_from_seed = false;
};

// Owns every vector later borrowed by the synchronous fitter. No fit-config
// spans are stored here, so moving this result cannot leave dangling spans.
struct CommonFitInitialization {
  std::vector<double> initial_sigmas;
  std::vector<double> prior_sigmas;
  nlohmann::json diagnostics;
};

// Closed native JSON spelling shared by policy parsing, expanded definitions,
// seed layouts and fit reports: shared, per-variance-interval, variance-time-knots.
[[nodiscard]] const char *common_time_shape_name(atx::vol::AmericanMarkovFitTimeShape shape);
// coordinate-pattern, levenberg-marquardt.
[[nodiscard]] const char *common_search_name(atx::vol::AmericanMarkovFitSearch search);

[[nodiscard]] CommonInitializationConfig parse_common_initialization(const nlohmann::json &entry);
[[nodiscard]] nlohmann::json common_initialization_json(const CommonInitializationConfig &config);
[[nodiscard]] nlohmann::json common_initialization_pending(const CommonInitializationConfig &config);
[[nodiscard]] nlohmann::json common_initialization_compact(const nlohmann::json &diagnostics);
[[nodiscard]] CommonFitInitialization configured_common_fit_initialization(
    const atx::vol::AmericanMarkovFitConfig &fit, std::size_t parameter_count,
    const CommonInitializationConfig &config);

// Only the initial vector can change. Incomplete/error results retain the full
// configured vector. The original configured prior is always materialized first.
// This never retries fitting, changes model caps, or selects the fitter's quotes.
// With interval_fallback, a second (SigmaInterval) estimator call is a seed
// retry within the remaining seed budget, never a fit retry.
void apply_common_quote_seeds(const atx::vol::AmericanMarkovFitMarket &market,
    std::span<const atx::vol::AmericanMarkovFitQuote> quotes,
    const atx::vol::AmericanMarkovFitConfig &fit,
    const CommonInitializationConfig &config, CommonFitInitialization &result);

} // namespace quote_bench
