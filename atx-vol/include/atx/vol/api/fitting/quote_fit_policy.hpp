#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/api/fitting/profile.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"

namespace atx::vol {

// Every element describes a distinct expiry of ONE symbol at ONE valuation
// instant. The caller establishes identity/time; this API accepts no fair-price,
// vendor-IV or Greek fields. Input spans are borrowed only during extraction.
struct QuoteFitExpirySnapshot {
  QuoteSliceMarket market;
  std::span<const QuoteSliceQuote> quotes;
};

struct QuoteFitLiquidityFeatures {
  ClassifierInputs classifier_inputs{};
  ProfileVerdict classifier{};
  std::size_t rows_seen{};
  std::size_t invalid_rows{};
  std::size_t unique_contracts{};
  std::size_t two_sided_contracts{};
  std::size_t bidless_contracts{};
  std::size_t near_money_contracts{};
  std::size_t near_money_two_sided_contracts{};
  std::size_t near_money_bidless_contracts{};
  // Fractions are zero when their denominator is zero; missing usable spread
  // evidence is represented separately by an absent median.
  double bidless_fraction{};
  double near_money_bidless_fraction{};
  std::optional<double> median_relative_spread;
  double median_near_money_strikes{};
  std::size_t max_near_money_strikes{};
};

// Counts distinct strike/side contracts within each expiry. Duplicate valid
// quotes are conservatively combined with min(bid), max(ask), independent of
// row order. Invalid/crossed/locked quotes contribute only invalid_rows. A
// strike with two valid sides counts ONCE toward near-money identifiability.
// Relative-spread median uses near-money, strictly two-sided contracts only.
// Duplicate calendar maturities are refused rather than inflating breadth.
[[nodiscard]] Result<QuoteFitLiquidityFeatures>
quote_fit_liquidity_features(std::span<const QuoteFitExpirySnapshot> snapshots,
                             double near_money_log_band = kNearMoneyLogMoneyness);

// Caller definitions can contain a borrowed spline grid. Catalog creation
// copies the complete configuration AND that grid into immutable owned storage.
struct QuoteFitProfile {
  std::string name;
  ProfileKind kind{ProfileKind::OrdinarySingleName};
  QuoteSliceConfig config{};
};

struct QuoteFitSymbolOverride {
  std::string symbol;
  std::string profile;
};

// Uncalibrated starting thresholds, exposed for deployment-specific tuning.
// Routing never reads the spelling of a ticker except an exact override key.
struct QuoteFitRegimeConfig {
  std::string dense_profile{"dense"};
  std::string ordinary_profile{"ordinary"};
  std::string sparse_profile{"sparse"};
  double near_money_log_band{kNearMoneyLogMoneyness};
  std::size_t dense_min_median_strikes{24};
  double dense_max_median_relative_spread{0.08};
  double dense_max_bidless_fraction{0.10}; // near-money contracts only
  std::size_t sparse_min_median_strikes{6};
  double sparse_min_median_relative_spread{0.60};
  double sparse_min_bidless_fraction{0.50};
};

enum class QuoteFitLiquidityRegime { Dense, Ordinary, Sparse };

// Reusable raw-liquidity routing, independent of a volatility parameterization.
// Checks thresholds and finite/ranged routing features; names are resolved by
// the owning catalog. Empty evidence selects Sparse. No ticker heuristics.
[[nodiscard]] Result<QuoteFitLiquidityRegime>
classify_quote_fit_liquidity(const QuoteFitLiquidityFeatures &features,
                             const QuoteFitRegimeConfig &regimes = {});

enum class QuoteFitPolicySource { RawLiquidity, ExactSymbolOverride };

[[nodiscard]] const char *to_string(QuoteFitPolicySource source) noexcept;

struct ResolvedQuoteFitPolicy {
  // Owns both the profile and any spline grid referenced by its config. Copies
  // remain valid after the catalog, definitions and raw snapshots are destroyed.
  std::shared_ptr<const QuoteFitProfile> profile;
  QuoteFitLiquidityFeatures features;
  QuoteFitPolicySource source{QuoteFitPolicySource::RawLiquidity};
};

// Starting profiles, not a claim of empirically optimal settings: dense uses a
// constrained price curve; ordinary/sparse use residual-free three-parameter
// eSSVI with different point-count/uncertainty/robust-loss budgets. No profile
// disables arbitrage admission. Fewer than four useful strikes cannot identify
// even the sparse smile and remain a fit refusal, including under overrides.
[[nodiscard]] std::vector<QuoteFitProfile> default_quote_fit_profiles();

class QuoteFitPolicyCatalog {
public:
  // Exact case-sensitive names and symbols; no whitespace trimming or ticker
  // aliases. Rejects duplicate names/keys, unknown references, invalid thresholds,
  // inadequate model point counts and disabled no-arbitrage controls.
  [[nodiscard]] static Result<QuoteFitPolicyCatalog>
  create(std::span<const QuoteFitProfile> profiles,
         std::span<const QuoteFitSymbolOverride> overrides = {},
         const QuoteFitRegimeConfig &regimes = {});

  // Resolve once per symbol snapshot and reuse the complete profile for its
  // expiries. No unmeasured maturity-specific parameter changes are applied.
  // Overrides select policy, never admission: quote, strike and calendar
  // validation still run in the fitting/publishing pipeline.
  [[nodiscard]] Result<ResolvedQuoteFitPolicy>
  resolve(std::string_view symbol, std::span<const QuoteFitExpirySnapshot> snapshots) const;

private:
  QuoteFitPolicyCatalog() = default;
  struct Override {
    std::string symbol;
    std::size_t profile{};
  };
  std::vector<std::shared_ptr<const QuoteFitProfile>> profiles_;
  std::vector<Override> overrides_;
  QuoteFitRegimeConfig regimes_;
  std::size_t dense_{};
  std::size_t ordinary_{};
  std::size_t sparse_{};
};

} // namespace atx::vol
