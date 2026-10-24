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

// Diagnostics of the private proposal, separate from the public Search result.
struct IvPriceCandidateStats {
  bool stopped_early{false};
  bool accepted_early{false};
  bool retried_full_search{false};
};

// Opt-in repaired-node seed: an evaluated Fast residual may propose sigma before
// strict IV convergence. Acceptance still requires cold target repricing within
// max_price_error plus the target floor/exercise/slope checks. A refused early
// proposal retries the original complete Fast search before typed fallback.
// Zero/invalid budgets and unsupported Search controls return nullopt. Stats are
// reset on entry, including refusal. No public/default Search behavior changes.
[[nodiscard]] std::optional<double> american_implied_vol_price_candidate(
    double price, double S, double K, double T, double r, double q, Side side,
    const IvOpts &opts, double max_price_error, IvPriceCandidateStats *stats = nullptr) noexcept;

// Checks an externally prepared sigma with the same cold price/exercise/slope
// acceptance as price_candidate, without any inverse iteration or fallback.
// Target range/zero-volatility screens and cold P(kIvMin) preserve floor semantics;
// a nearby seed above the floor cannot turn a sub-floor target into an identified IV.
// A seed is only a proposal; neither a family root nor its residual is trusted.
[[nodiscard]] std::optional<double> american_implied_vol_price_seed(
    double price, double S, double K, double T, double r, double q, Side side,
    const IvOpts &opts, double max_price_error, double sigma) noexcept;

} // namespace atx::vol
