#pragma once

#include "quote_bench_common_seed.hpp"

#include "atx/vol/api/fitting/american_markov_policy.hpp"
#include "atx/vol/api/marketdata/american_markov_market.hpp"

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace quote_bench {

struct EquivalentIvConfig {
  double tolerance = 1.0e-7;
  double min_time_value = 1.0e-8;
  std::uint16_t max_iterations = 64;
};

struct CommonPolicy {
  atx::vol::AmericanMarkovPolicyCatalog catalog;
  atx::vol::AmericanMarkovMarketConfig market;
  EquivalentIvConfig equivalent_iv;
  CommonInitializationConfig initialization;
  nlohmann::json definition;
};

[[nodiscard]] CommonPolicy load_common_policy(const std::string &selection,
                                               const std::string &dividends);
[[nodiscard]] nlohmann::json common_profile_json(
    const atx::vol::AmericanMarkovPolicyProfile &profile);

} // namespace quote_bench
