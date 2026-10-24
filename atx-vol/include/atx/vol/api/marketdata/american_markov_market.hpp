#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/fitting/american_markov_fit.hpp"

namespace atx::vol {

inline constexpr std::size_t kAmericanMarkovMarketMaxExpiries = 1024;
inline constexpr double kAmericanMarkovMarketMaxYears = 100.0;
inline constexpr double kAmericanMarkovMarketMaxAnnualRate = 100.0;
inline constexpr double kAmericanMarkovMarketMaxLogFactor = 64.0;

// One underlying, deliverable and EXACT valuation snapshot per preparation call.
// expiry_key is an opaque caller identity, not a date or an ordering key.
// underlying_spot must come from raw underlying quotes/trades, never synSpot.
// rate and continuous_yield are zero-to-expiry continuously compounded rates on
// calendar_years; yield excludes separately supplied dividend_pv. Clocks share
// a valuation origin. No vendor fair prices/vols/Greeks/model flags are inputs.
struct AmericanMarkovRawQuote {
  std::uint64_t expiry_key = 0;
  double strike = 0.0;
  Side side = Side::Call;
  double bid = 0.0;
  double ask = 0.0;
  double weight = 1.0;
  double underlying_spot = 0.0;
  double calendar_years = 0.0;
  double variance_years = 0.0;
  double rate = 0.0;
  double continuous_yield = 0.0;
  double dividend_pv = 0.0;
};

enum class AmericanMarkovMarketDividends : std::uint8_t {
  None,
  // Missing ex-dates: replace aggregate PV by continuous yield. Preserves
  // prepaid=S*exp(-q*T)-PV, NOT American exercise around unknown cash dates.
  FundingPvContinuousYield,
  // Missing ex-dates: preserves prepaid=(S-PV)*exp(-q*T), same limitation.
  EscrowedPvContinuousYield
};

enum class AmericanMarkovMarketStatus : std::uint8_t {
  Success, InvalidConfig, ResourceLimit, InvalidRow, DuplicateContract,
  SpotDispersion, ClockDispersion, CarryDispersion, DividendConvention,
  NonpositivePrepaid, AmbiguousExpiry, NonmonotoneVarianceClock, NumericLimit
};

[[nodiscard]] const char *to_string(AmericanMarkovMarketDividends value) noexcept;
[[nodiscard]] const char *to_string(AmericanMarkovMarketStatus value) noexcept;

struct AmericanMarkovMarketConfig {
  AmericanMarkovMarketDividends dividends = AmericanMarkovMarketDividends::None;
  std::size_t max_quotes = 4096;
  std::size_t max_expiries = 128;
  // All dispersion thresholds are inclusive and mandatory; zero requires exact
  // agreement. Equal-weight coordinate medians resolve inputs, independently
  // of bid/ask/fit weights. All rows remain in diagnostics; no outlier trimming.
  double max_spot_relative_deviation = 0.005;
  double max_clock_absolute_deviation = 1.0e-10; // years, separate from carry
  double max_rate_absolute_deviation = 0.0025; // annualized rate
  double max_yield_absolute_deviation = 0.05; // annualized supplied yield
  double max_dividend_pv_over_spot_deviation = 0.005;
  double max_prepaid_over_spot_deviation = 0.05;
  double max_forward_over_spot_deviation = 0.05;
  // Protect differencing of cumulative term inputs at nearly equal expiries.
  double min_calendar_increment = 1.0e-10;
  double max_absolute_interval_rate = 10.0;
  double max_absolute_interval_yield = 10.0;
};

struct AmericanMarkovMarketSummary {
  double median = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;
  double median_absolute_deviation = 0.0;
  double maximum_absolute_deviation = 0.0;
};

struct AmericanMarkovExpiryMarketDiagnostics {
  std::uint64_t expiry_key = 0;
  std::size_t quote_count = 0;
  AmericanMarkovMarketSummary calendar_years;
  AmericanMarkovMarketSummary variance_years;
  AmericanMarkovMarketSummary rate;
  AmericanMarkovMarketSummary supplied_yield;
  AmericanMarkovMarketSummary dividend_pv;
  // Ratios use the resolved COMMON spot and median clock; dispersion therefore
  // measures raw carry differences, not inconsistent underlying quotes/clocks.
  AmericanMarkovMarketSummary prepaid_over_spot;
  AmericanMarkovMarketSummary forward_over_spot;
  double integrated_rate = 0.0;
  double integrated_effective_yield = 0.0;
  double resolved_prepaid_over_spot = 0.0;
  double resolved_forward_over_spot = 0.0;
  double interval_rate = 0.0;
  double interval_yield = 0.0;
};

struct AmericanMarkovMarketDiagnostics {
  std::size_t quote_count = 0;
  AmericanMarkovMarketSummary spot;
  // Same ordering as prepared intervals on success, partial on refusal. The
  // complete failing expiry summary is retained for dispersion refusals.
  std::vector<AmericanMarkovExpiryMarketDiagnostics> expiries;
  std::optional<std::size_t> refused_row; // original input index, when applicable
  std::optional<std::uint64_t> refused_expiry;
};

class AmericanMarkovPreparedMarket;
struct AmericanMarkovMarketPreparation;

// Cold bounded O(N log N) preparation, allocates O(N + expiry_count). Invalid
// inputs refuse the whole snapshot; never silently drop or reweight contracts.
// Borrowed input spans do not escape. bad_alloc propagates; inputs are unchanged.
// Cumulative rate/effective yield and both clocks interpolate linearly between
// expiry endpoints: each resulting interval has one constant rate and yield.
// Success establishes a common market schedule, NOT quote-fit admission,
// continuous-American accuracy or a fitted-volatility certificate.
[[nodiscard]] AmericanMarkovMarketPreparation prepare_american_markov_market(
    std::span<const AmericanMarkovRawQuote> quotes,
    const AmericanMarkovMarketConfig &config = {});

class AmericanMarkovPreparedMarket {
public:
  [[nodiscard]] double spot() const noexcept { return spot_; }
  [[nodiscard]] std::span<const AmericanMarkovFitInterval> intervals() const noexcept {
    return intervals_;
  }
  // Original input row order. Each expiry_index identifies the shared prefix.
  [[nodiscard]] std::span<const AmericanMarkovFitQuote> quotes() const noexcept { return quotes_; }
  [[nodiscard]] std::span<const std::uint64_t> expiry_keys() const noexcept { return keys_; }
  // Returned spans borrow *this and the supplied state grid. Keep both alive
  // through fitting. The adapter does not choose volatility or numerical grids.
  [[nodiscard]] AmericanMarkovFitMarket fit_market(
      const AmericanMarkovConfig &model_config = {},
      std::span<const double> state_grid = {}) const noexcept {
    return {spot_, intervals_, state_grid, model_config};
  }

private:
  friend AmericanMarkovMarketPreparation prepare_american_markov_market(
      std::span<const AmericanMarkovRawQuote>, const AmericanMarkovMarketConfig &);
  AmericanMarkovPreparedMarket() = default;
  double spot_ = 0.0;
  std::vector<AmericanMarkovFitInterval> intervals_;
  std::vector<AmericanMarkovFitQuote> quotes_;
  std::vector<std::uint64_t> keys_;
};

struct AmericanMarkovMarketPreparation {
  AmericanMarkovMarketStatus status = AmericanMarkovMarketStatus::InvalidConfig;
  AmericanMarkovMarketDiagnostics diagnostics;
  // Present exactly when status == Success; owns all prepared inputs.
  std::optional<AmericanMarkovPreparedMarket> market;
};

} // namespace atx::vol
