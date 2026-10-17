#include "fitting/quote_slice_carry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace atx::vol {

const char *to_string(QuoteSliceCarryMode value) noexcept {
  switch (value) {
  case QuoteSliceCarryMode::Supplied:
    return "supplied";
  case QuoteSliceCarryMode::ImpliedForward:
    return "implied_forward";
  }
  return "unknown";
}

const char *to_string(QuoteSliceCarryFallback value) noexcept {
  switch (value) {
  case QuoteSliceCarryFallback::None:
    return "none";
  case QuoteSliceCarryFallback::DatedCash:
    return "dated_cash";
  case QuoteSliceCarryFallback::InvalidMarket:
    return "invalid_market";
  case QuoteSliceCarryFallback::EstimateRefused:
    return "estimate_refused";
  case QuoteSliceCarryFallback::NoModelYield:
    return "no_model_yield";
  case QuoteSliceCarryFallback::SliceRefused:
    return "slice_refused";
  }
  return "unknown";
}

namespace detail {
namespace {

bool positive(double value) noexcept { return value > 0.0 && std::isfinite(value); }

// The supplied forward, by the same identities fit_quote_slice uses for the
// non-dated dividend models (diagnostic: it is only reported, never fitted).
double supplied_forward(const QuoteSliceMarket &m, QuoteSliceDividendModel model) noexcept {
  const double carry = (m.rate - m.continuous_yield) * m.calendar_years;
  switch (model) {
  case QuoteSliceDividendModel::ContinuousYield:
    return m.spot * std::exp(carry);
  case QuoteSliceDividendModel::AggregatePresentValue:
    return (m.spot - m.dividend_pv) * std::exp(carry);
  case QuoteSliceDividendModel::AggregateFundingPresentValue:
    return m.spot * std::exp(carry) - m.dividend_pv * std::exp(m.rate * m.calendar_years);
  case QuoteSliceDividendModel::DatedCash:
    break;
  }
  return std::numeric_limits<double>::quiet_NaN();
}

// Calendar continuous yield that makes the slice's own forward equal `forward`
// under `model`, keeping the supplied aggregate PV (quote_slice_carry.hpp).
double model_yield(const QuoteSliceMarket &m, QuoteSliceDividendModel model,
                   double forward) noexcept {
  const double c = m.calendar_years;
  switch (model) {
  case QuoteSliceDividendModel::ContinuousYield:
    return m.rate - std::log(forward / m.spot) / c;
  case QuoteSliceDividendModel::AggregatePresentValue:
    return m.rate - std::log(forward / (m.spot - m.dividend_pv)) / c;
  case QuoteSliceDividendModel::AggregateFundingPresentValue:
    return -std::log((forward * std::exp(-m.rate * c) + m.dividend_pv) / m.spot) / c;
  case QuoteSliceDividendModel::DatedCash:
    break;
  }
  return std::numeric_limits<double>::quiet_NaN();
}

// Pair calls and puts by strike. A strike quoted more than once on one side is
// ambiguous and dropped; invalid contracts are skipped. Bid/ask pass through
// unchanged: the estimator decides which legs are point quotes.
std::vector<ImpliedForwardPair> quote_pairs(std::span<const QuoteSliceQuote> quotes) {
  struct Leg {
    double strike;
    Side side;
    double bid;
    double ask;
  };
  std::vector<Leg> legs;
  legs.reserve(quotes.size());
  for (const QuoteSliceQuote &quote : quotes) {
    if (positive(quote.strike) && (quote.side == Side::Call || quote.side == Side::Put)) {
      legs.push_back({quote.strike, quote.side, quote.bid, quote.ask});
    }
  }
  std::stable_sort(legs.begin(), legs.end(), [](const Leg &a, const Leg &b) {
    return a.strike < b.strike || (a.strike == b.strike && a.side < b.side);
  });
  std::vector<ImpliedForwardPair> pairs;
  pairs.reserve(legs.size() / 2 + 1);
  std::size_t i = 0;
  while (i < legs.size()) {
    std::size_t j = i;
    std::size_t calls = 0;
    std::size_t puts = 0;
    ImpliedForwardPair pair{legs[i].strike, 0.0, 0.0, 0.0, 0.0};
    while (j < legs.size() && legs[j].strike == legs[i].strike) {
      if (legs[j].side == Side::Call) {
        ++calls;
        pair.call_bid = legs[j].bid;
        pair.call_ask = legs[j].ask;
      } else {
        ++puts;
        pair.put_bid = legs[j].bid;
        pair.put_ask = legs[j].ask;
      }
      ++j;
    }
    if (calls == 1 && puts == 1) {
      pairs.push_back(pair);
    }
    i = j;
  }
  return pairs;
}

void record_estimate(QuoteSliceCarryResolution &resolution, const ImpliedForwardResult &estimate) {
  resolution.status = estimate.status;
  resolution.reason = estimate.reason;
  resolution.implied_forward = estimate.forward;
  resolution.implied_total_yield = estimate.implied_yield;
  resolution.parity_forward = estimate.parity_forward;
  resolution.forward_lower = estimate.forward_lower;
  resolution.forward_upper = estimate.forward_upper;
  resolution.dispersion_sd = estimate.dispersion_sd;
  resolution.exercise_shift_sd = estimate.exercise_shift_sd;
  resolution.atm_total_vol = estimate.atm_total_vol;
  resolution.pairs_valid = estimate.pairs_valid;
  resolution.pairs_selected = estimate.pairs_selected;
  resolution.pairs_retained = estimate.pairs_retained;
  resolution.pairs_contaminated = estimate.pairs_contaminated;
  resolution.pairs_unidentified = estimate.pairs_unidentified;
}

Result<QuoteSliceFit> fit_supplied(const QuoteSliceMarket &market,
                                   std::span<const QuoteSliceQuote> quotes,
                                   const QuoteSliceConfig &supplied,
                                   std::span<const CashDividend> dividends,
                                   const QuoteSliceCarryResolution &resolution) {
  auto fit = fit_quote_slice(market, quotes, supplied, dividends);
  if (fit) {
    fit->carry_resolution = resolution;
  }
  return fit;
}

} // namespace

Result<QuoteSliceFit> fit_quote_slice_resolving_carry(const QuoteSliceMarket &market,
                                                      std::span<const QuoteSliceQuote> quotes,
                                                      const QuoteSliceConfig &config,
                                                      std::span<const CashDividend> dividends) {
  QuoteSliceConfig supplied = config;
  supplied.carry.mode = QuoteSliceCarryMode::Supplied;
  QuoteSliceCarryResolution resolution;
  resolution.mode = config.carry.mode;
  resolution.supplied_yield = market.continuous_yield;
  resolution.supplied_forward = supplied_forward(market, config.dividend_model);
  if (config.carry.mode != QuoteSliceCarryMode::ImpliedForward) {
    return Err(ErrorCode::InvalidArgument, "quote slice: unknown carry mode");
  }
  if (config.dividend_model == QuoteSliceDividendModel::DatedCash) {
    resolution.fallback = QuoteSliceCarryFallback::DatedCash;
    resolution.reason = "dated-cash lattice forward is not an implied-forward model";
    return fit_supplied(market, quotes, supplied, dividends, resolution);
  }
  if (!positive(market.spot) || !positive(market.variance_years) ||
      !positive(market.calendar_years) || !std::isfinite(market.rate)) {
    // The Supplied fit reports the malformed market exactly as it always has.
    resolution.fallback = QuoteSliceCarryFallback::InvalidMarket;
    resolution.reason = "invalid market";
    return fit_supplied(market, quotes, supplied, dividends, resolution);
  }

  // Pricer clock: variance years, with the funding rate rescaled so that the
  // discount factor exp(-rate * calendar_years) and the forward are preserved.
  ImpliedForwardMarket estimator_market;
  estimator_market.spot = market.spot;
  estimator_market.years = market.variance_years;
  estimator_market.rate = market.rate * (market.calendar_years / market.variance_years);
  if (positive(resolution.supplied_forward)) {
    estimator_market.reference_yield =
        estimator_market.rate -
        std::log(resolution.supplied_forward / market.spot) / market.variance_years;
  }
  const std::vector<ImpliedForwardPair> pairs = quote_pairs(quotes);
  const ImpliedForwardResult estimate =
      estimate_implied_forward(estimator_market, pairs, config.carry.implied);
  record_estimate(resolution, estimate);
  if (estimate.status != ImpliedForwardStatus::Ok) {
    resolution.fallback = QuoteSliceCarryFallback::EstimateRefused;
    return fit_supplied(market, quotes, supplied, dividends, resolution);
  }

  QuoteSliceMarket implied_market = market;
  implied_market.continuous_yield = model_yield(market, config.dividend_model, estimate.forward);
  resolution.implied_yield = implied_market.continuous_yield;
  if (!std::isfinite(implied_market.continuous_yield)) {
    resolution.fallback = QuoteSliceCarryFallback::NoModelYield;
    resolution.reason = "implied forward has no yield under the dividend model";
    return fit_supplied(market, quotes, supplied, dividends, resolution);
  }
  // One carry per expiry: per-contract supplied yields would re-introduce the
  // carry this mode replaces.
  std::vector<QuoteSliceQuote> implied_quotes(quotes.begin(), quotes.end());
  for (QuoteSliceQuote &quote : implied_quotes) {
    quote.continuous_yield.reset();
  }
  auto fit = fit_quote_slice(implied_market, implied_quotes, supplied, dividends);
  if (!fit) {
    resolution.fallback = QuoteSliceCarryFallback::SliceRefused;
    resolution.reason = "slice refused the implied carry";
    return fit_supplied(market, quotes, supplied, dividends, resolution);
  }
  resolution.applied = true;
  fit->carry_resolution = resolution;
  return fit;
}

} // namespace detail
} // namespace atx::vol
