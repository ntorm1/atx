#include "fitting/quote_slice_carry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american.hpp" // kTauAtExpiryRelTol, CashDividend

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

// ── One authoritative spot per snapshot (lane C0) ─────────────────────────────

namespace {

bool finite_nonnegative(double value) noexcept { return value >= 0.0 && std::isfinite(value); }

// The dated events fit_quote_slice keeps (prepare_fit): tau in (0, T (1 + rel tol)],
// nonzero amounts, tau clamped to T. Calendar clock throughout.
template <class Visit>
void for_each_event(const QuoteSliceMarket &m, std::span<const CashDividend> dividends,
                    Visit &&visit) {
  const double window = m.calendar_years * (1.0 + kTauAtExpiryRelTol);
  for (const CashDividend &event : dividends) {
    if (!(event.tau > 0.0) || event.tau > window || event.amount == 0.0) continue;
    visit(std::min(event.tau, m.calendar_years), event.amount);
  }
}

// The lattice forward at spot `spot` and calendar yield `q`, exactly as prepare_fit
// builds QuoteSliceDiagnostics::forward for DatedCash.
double dated_forward(const QuoteSliceMarket &m, double spot, double q,
                     std::span<const CashDividend> dividends) noexcept {
  double forward = spot * std::exp((m.rate - q) * m.calendar_years);
  for_each_event(m, dividends, [&](double tau, double amount) {
    forward -= amount * std::exp((m.rate - q) * (m.calendar_years - tau));
  });
  return forward;
}

// The yield shift keeping the dated-cash forward: a bounded safeguarded Newton on
// F(S*, q + shift) = F(S, q), decreasing in the shift while the forward is positive.
// NaN when no positive forward or no finite root exists.
double dated_cash_shift(const QuoteSliceMarket &m, double snapshot_spot,
                        std::span<const CashDividend> dividends) noexcept {
  const double target = dated_forward(m, m.spot, m.continuous_yield, dividends);
  if (!detail::positive(target)) return std::numeric_limits<double>::quiet_NaN();
  const double T = m.calendar_years;
  double shift = std::log1p((snapshot_spot - m.spot) / m.spot) / T;
  for (int step = 0; step < 64; ++step) {
    const double q = m.continuous_yield + shift;
    const double value = dated_forward(m, snapshot_spot, q, dividends) - target;
    if (!std::isfinite(value)) break;
    if (std::abs(value) <= 1.0e-15 * target) return shift;
    double slope = -T * snapshot_spot * std::exp((m.rate - q) * T);
    for_each_event(m, dividends, [&](double tau, double amount) {
      slope += (T - tau) * amount * std::exp((m.rate - q) * (T - tau));
    });
    if (!(slope < 0.0) || !std::isfinite(slope)) break;
    const double next = shift - value / slope;
    if (!std::isfinite(next)) break;
    if (next == shift) return shift;
    shift = next;
  }
  return std::numeric_limits<double>::quiet_NaN();
}

// The forward the model preserves, by the identities fit_quote_slice uses.
double model_forward(const QuoteSliceMarket &m, QuoteSliceDividendModel model,
                     std::span<const CashDividend> dividends) noexcept {
  if (model == QuoteSliceDividendModel::DatedCash) {
    return dated_forward(m, m.spot, m.continuous_yield, dividends);
  }
  return detail::supplied_forward(m, model);
}

} // namespace

Result<QuoteSliceSpotRebase> rebase_quote_slice_spot(const QuoteSliceMarket &market,
                                                     double snapshot_spot,
                                                     QuoteSliceDividendModel model,
                                                     std::span<const CashDividend> dividends) {
  using detail::positive;
  if (!positive(market.spot) || !positive(snapshot_spot) || !positive(market.calendar_years) ||
      !std::isfinite(market.rate) || !std::isfinite(market.continuous_yield) ||
      !finite_nonnegative(market.dividend_pv)) {
    return Err(ErrorCode::InvalidArgument, "spot rebase: invalid market or snapshot spot");
  }
  if (model != QuoteSliceDividendModel::DatedCash && !dividends.empty()) {
    return Err(ErrorCode::InvalidArgument, "spot rebase: dated dividends need DatedCash");
  }
  for (const CashDividend &event : dividends) {
    if (!std::isfinite(event.tau) || !finite_nonnegative(event.amount)) {
      return Err(ErrorCode::InvalidArgument, "spot rebase: invalid dividend event");
    }
  }
  const double T = market.calendar_years;
  // Spot ratios near one: log1p of the exact difference keeps the shift's relative
  // accuracy where a log of the rounded ratio would lose it.
  double shift = std::numeric_limits<double>::quiet_NaN();
  switch (model) {
  case QuoteSliceDividendModel::ContinuousYield:
  case QuoteSliceDividendModel::AggregateFundingPresentValue:
    // F = S e^{(r-q)T} (- PV e^{rT} under funding PV, independent of S and q).
    shift = std::log1p((snapshot_spot - market.spot) / market.spot) / T;
    break;
  case QuoteSliceDividendModel::AggregatePresentValue: {
    // F = (S - PV) e^{(r-q)T}: the escrowed stock carries the shift.
    const double held = market.spot - market.dividend_pv;
    if (!(held > 0.0) || !(snapshot_spot - market.dividend_pv > 0.0)) {
      return Err(ErrorCode::OutOfRange, "spot rebase: spot at or below the escrowed PV");
    }
    shift = std::log1p((snapshot_spot - market.spot) / held) / T;
    break;
  }
  case QuoteSliceDividendModel::DatedCash:
    if (market.dividend_pv != 0.0) {
      return Err(ErrorCode::InvalidArgument, "spot rebase: dated cash with an aggregate PV");
    }
    shift = dated_cash_shift(market, snapshot_spot, dividends);
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "spot rebase: unknown dividend model");
  }
  if (!std::isfinite(shift)) {
    return Err(ErrorCode::OutOfRange, "spot rebase: no finite carry preserves the forward");
  }
  QuoteSliceSpotRebase out;
  out.market = market;
  out.market.spot = snapshot_spot;
  out.market.continuous_yield = market.continuous_yield + shift;
  out.yield_shift = shift;
  out.forward = model_forward(market, model, dividends);
  if (!std::isfinite(out.market.continuous_yield)) {
    return Err(ErrorCode::OutOfRange, "spot rebase: no finite carry preserves the forward");
  }
  return atx::core::Ok(std::move(out));
}

} // namespace atx::vol
