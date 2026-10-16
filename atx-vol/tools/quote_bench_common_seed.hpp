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
  atx::vol::AmericanMarkovSeedConfig seed;
};

// Owns every vector later borrowed by the synchronous fitter. No fit-config
// spans are stored here, so moving this result cannot leave dangling spans.
struct CommonFitInitialization {
  std::vector<double> initial_sigmas;
  std::vector<double> prior_sigmas;
  nlohmann::json diagnostics;
};

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
void apply_common_quote_seeds(const atx::vol::AmericanMarkovFitMarket &market,
    std::span<const atx::vol::AmericanMarkovFitQuote> quotes,
    const atx::vol::AmericanMarkovFitConfig &fit,
    const CommonInitializationConfig &config, CommonFitInitialization &result);

} // namespace quote_bench
