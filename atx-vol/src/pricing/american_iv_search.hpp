#pragma once

#include <optional>

#include "atx/vol/api/pricing/american_iv.hpp"

namespace atx::vol {

// Internal Search result for PriceOnly classification. A rejected candidate is the
// exact plain Search value discarded by the non-monotone gate, never an accepted IV.
// It is populated only for NonMonotoneRegion; answer retains the public NaN/status
// contract. No pricing state or borrowed input survives the call.
struct IvSearchResult {
  IvResult answer;
  std::optional<double> rejected_candidate;
};

// Runs Search (regardless of opts.strategy) with all its original controls and
// bad_alloc containment. Reuses the candidate solely to classify a typed refusal;
// callers must not promote it to Success or interpret it as a cold-map root.
[[nodiscard]] IvSearchResult american_implied_vol_search(
    double price, double S, double K, double T, double r, double q, Side side,
    const IvOpts &opts) noexcept;

// Opt-in repaired-node seed: Fast arithmetic on the target AL scheme, accepted
// only after cold target repricing within max_price_error. Exercise/flat candidates
// are never identified. A refusal requests the caller's unchanged typed fallback.
// Zero/invalid budgets and unsupported Search controls return nullopt.
[[nodiscard]] std::optional<double> american_implied_vol_price_candidate(
    double price, double S, double K, double T, double r, double q, Side side,
    const IvOpts &opts, double max_price_error) noexcept;

} // namespace atx::vol
