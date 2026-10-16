#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/fitting/american_markov_fit.hpp"
#include "atx/vol/api/fitting/quote_fit_policy.hpp"
#include "atx/vol/api/pricing/american_markov_grid.hpp"

namespace atx::vol {

inline constexpr std::size_t kAmericanMarkovPolicyMaxProfiles = 64;
inline constexpr std::size_t kAmericanMarkovPolicyMaxOverrides = 4096;
inline constexpr std::size_t kAmericanMarkovPolicyMaxNameBytes = 128;
inline constexpr std::size_t kAmericanMarkovPolicyMaxSnapshots = 4096;
inline constexpr std::size_t kAmericanMarkovPolicyMaxRawQuotes = 65536;

// Definitions borrow fit-config spans until create() returns. The catalog
// deep-copies knots and initial/prior sigmas; all other settings are values.
// Model, grid and fit budgets are explicit and independent; a valid profile
// cannot bypass any compiler/fitter no-arbitrage or numerical resource gate.
struct AmericanMarkovPolicyProfile {
  std::string name;
  AmericanMarkovConfig model_config{};
  AmericanMarkovSinhGridConfig grid_config{};
  AmericanMarkovFitConfig fit_config{};
};

struct ResolvedAmericanMarkovPolicy {
  // Shared ownership includes the storage underlying ALL fit-config spans.
  // Copies/moves of this result survive catalog/definition destruction. A
  // separately copied profile/config remains a borrowed view: retain this owner.
  std::shared_ptr<const AmericanMarkovPolicyProfile> profile;
  QuoteFitLiquidityFeatures features;
  QuoteFitPolicySource source{QuoteFitPolicySource::RawLiquidity};
  std::size_t time_groups = 0;
  std::size_t parameter_count = 0;
};

// Uncalibrated starting settings, not empirically optimized configurations.
// Sparse has a single shared sigma with stronger prior regularization;
// ordinary has three shared local-vol knots; dense has five knots per active
// variance interval. Defaults reference immutable static knot arrays.
[[nodiscard]] std::vector<AmericanMarkovPolicyProfile> default_american_markov_policy_profiles();

class AmericanMarkovPolicyCatalog {
public:
  // Exact case-sensitive names/symbols. Refuses empty/duplicate/unknown names,
  // invalid numeric settings and inconsistent resource capacities. Bounds are
  // 64 profiles, 4096 overrides and 128 bytes per name or symbol. Allocates and
  // may throw bad_alloc; failure does not modify caller input.
  [[nodiscard]] static Result<AmericanMarkovPolicyCatalog>
  create(std::span<const AmericanMarkovPolicyProfile> profiles,
         std::span<const QuoteFitSymbolOverride> overrides = {},
         const QuoteFitRegimeConfig &regimes = {});

  // Resolve once for one symbol/root/valuation snapshot; the caller establishes
  // this identity and excludes ambiguous duplicate expiry identities first.
  // Reuses quote_fit_liquidity_features and classify_quote_fit_liquidity, including
  // their conservative duplicate quote handling and locked-quote exclusion.
  // The actual common schedule may include additional cash or clock endpoints.
  // Every raw expiry must match its calendar/variance endpoint and all snapshots
  // must share one exact spot. No schedule or profile is silently changed.
  // Raw extraction is bounded to 4096 snapshots and 65536 total raw quote rows.
  //
  // Counts active variance groups exactly as the fitter does; paused variance
  // consumes no new group. Refuses all-paused variance, excessive parameters,
  // or initial/prior vectors inconsistent with the actual schedule. Overrides
  // select the profile only; every validation remains mandatory.
  [[nodiscard]] Result<ResolvedAmericanMarkovPolicy>
  resolve(std::string_view symbol, std::span<const QuoteFitExpirySnapshot> snapshots,
          std::span<const AmericanMarkovFitInterval> intervals) const;

private:
  AmericanMarkovPolicyCatalog() = default;
  struct Override {
    std::string symbol;
    std::size_t profile = 0;
  };
  std::vector<std::shared_ptr<const AmericanMarkovPolicyProfile>> profiles_;
  std::vector<Override> overrides_;
  QuoteFitRegimeConfig regimes_;
  std::size_t dense_ = 0;
  std::size_t ordinary_ = 0;
  std::size_t sparse_ = 0;
};

} // namespace atx::vol
