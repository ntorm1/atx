#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/black76.hpp"
#include "fitting/counters.hpp"
#include "fitting/deam_iv_cache.hpp"
#include "fitting/quote_slice_carry.hpp"
#include "fitting/quote_slice_vol_space.hpp"
#include "fitting/rung_memo.hpp"
#include "fitting/slice_channel.hpp"
#include "pricing/american_iv_premium.hpp"

namespace atx::vol {
namespace {

using atx::core::Err;
using atx::core::Ok;

bool valid_side(Side side) noexcept { return side == Side::Call || side == Side::Put; }
bool positive(double value) noexcept { return value > 0.0 && std::isfinite(value); }
bool nonnegative(double value) noexcept { return value >= 0.0 && std::isfinite(value); }

Status validate(const QuoteSliceMarket &m, std::span<const QuoteSliceQuote> quotes,
                const QuoteSliceConfig &cfg, std::span<const CashDividend> dividends) {
  if (!positive(m.spot) || !positive(m.variance_years) || !positive(m.calendar_years) ||
      !std::isfinite(m.rate) || !std::isfinite(m.continuous_yield) || !nonnegative(m.dividend_pv)) {
    return Err(ErrorCode::InvalidArgument, "quote slice: invalid market or clocks");
  }
  if (quotes.size() > std::numeric_limits<std::uint32_t>::max() || cfg.min_quotes < 2 ||
      !positive(cfg.spread_floor) || !nonnegative(cfg.min_time_value) ||
      !positive(cfg.iv_tolerance) || cfg.max_iv_iterations == 0 || !nonnegative(cfg.atm_log_band) ||
      cfg.discrete_div_steps < 1 || cfg.discrete_div_steps > kDiscreteDivMaxSteps ||
      (cfg.side_policy != QuoteSliceSidePolicy::All &&
       cfg.side_policy != QuoteSliceSidePolicy::OutOfTheMoney) ||
      (cfg.method != QuoteSliceFitMethod::PointCurve &&
       cfg.method != QuoteSliceFitMethod::ChannelSpline)) {
    return Err(ErrorCode::InvalidArgument, "quote slice: invalid fitting configuration");
  }
  if (!discrete_div_scheme_admits(cfg.discrete_div_scheme, cfg.discrete_div_steps)) {
    return Err(ErrorCode::InvalidArgument, "quote slice: invalid lattice scheme");
  }
  if (!nonnegative(cfg.quality.min_bid) || !nonnegative(cfg.quality.max_spread_to_mid) ||
      !nonnegative(cfg.quality.max_spread_vol) || !nonnegative(cfg.quality.min_vega_weight) ||
      !positive(cfg.quality.max_weight)) {
    return Err(ErrorCode::InvalidArgument, "quote slice: invalid quote-quality policy");
  }
  ATX_TRY_VOID(validate_quote_slice_repair(cfg.repair));
  switch (cfg.dividend_model) {
  case QuoteSliceDividendModel::ContinuousYield:
    if (m.dividend_pv != 0.0 || !dividends.empty()) {
      return Err(ErrorCode::InvalidArgument, "continuous-yield model cannot ignore cash dividends");
    }
    break;
  case QuoteSliceDividendModel::AggregatePresentValue:
    if (!(m.dividend_pv < m.spot)) {
      return Err(ErrorCode::InvalidArgument, "escrowed-PV model requires PV below spot");
    }
    [[fallthrough]];
  case QuoteSliceDividendModel::AggregateFundingPresentValue:
    if (!dividends.empty()) {
      return Err(ErrorCode::InvalidArgument, "aggregate-PV model cannot also consume dated cash");
    }
    break;
  case QuoteSliceDividendModel::DatedCash:
    if (m.dividend_pv != 0.0) {
      return Err(ErrorCode::InvalidArgument, "dated-cash model cannot also consume aggregate PV");
    }
    break;
  default:
    return Err(ErrorCode::InvalidArgument, "quote slice: unknown dividend model");
  }
  for (const CashDividend &event : dividends) {
    if (!std::isfinite(event.tau) || !nonnegative(event.amount)) {
      return Err(ErrorCode::InvalidArgument, "quote slice: invalid dividend event");
    }
  }
  for (const QuoteSliceQuote &quote : quotes) {
    if (quote.continuous_yield && !std::isfinite(*quote.continuous_yield)) {
      return Err(ErrorCode::InvalidArgument, "quote slice: non-finite contract yield");
    }
  }
  return Ok();
}

Status build_carry_curve(QuoteSliceFit &fit, std::span<const QuoteSliceQuote> quotes) {
  const bool any_override = std::any_of(quotes.begin(), quotes.end(),
      [](const QuoteSliceQuote &quote) { return quote.continuous_yield.has_value(); });
  if (!any_override) { return Ok(); }
  auto &curve = fit.carry_curve;
  curve.reserve(quotes.size());
  for (const auto &quote : quotes) {
    if (positive(quote.strike) && valid_side(quote.side)) {
      curve.push_back({quote.strike,
                       quote.continuous_yield.value_or(fit.market.continuous_yield)});
    }
  }
  std::sort(curve.begin(), curve.end(),
            [](const QuoteSliceCarryPoint &a, const QuoteSliceCarryPoint &b) {
              return a.strike < b.strike;
            });
  std::size_t count = 0;
  for (const QuoteSliceCarryPoint &point : curve) {
    if (count > 0 && curve[count - 1].strike == point.strike) {
      if (curve[count - 1].continuous_yield != point.continuous_yield) {
        return Err(ErrorCode::InvalidArgument,
                   "quote slice: conflicting contract yields at the same strike");
      }
    } else {
      curve[count++] = point;
    }
  }
  curve.resize(count);
  return Ok();
}

// upper >= lower > 0. log1p preserves a meaningful interval even when two
// adjacent floating-point strikes have indistinguishable rounded logarithms.
double log_strike_distance(double upper, double lower) noexcept {
  const double relative = (upper - lower) / lower;
  return std::isfinite(relative) ? std::log1p(relative) : std::log(upper) - std::log(lower);
}

double calendar_yield_at(const QuoteSliceFit &fit, double strike) noexcept {
  const auto &curve = fit.carry_curve;
  if (curve.empty()) { return fit.market.continuous_yield; }
  const auto hi = std::lower_bound(curve.begin(), curve.end(), strike,
      [](const QuoteSliceCarryPoint &point, double value) { return point.strike < value; });
  if (hi == curve.end()) { return curve.back().continuous_yield; }
  if (hi == curve.begin() || hi->strike == strike) { return hi->continuous_yield; }
  const auto &lo = *(hi - 1);
  const double weight = log_strike_distance(strike, lo.strike) /
                        log_strike_distance(hi->strike, lo.strike);
  return std::lerp(lo.continuous_yield, hi->continuous_yield, weight);
}

double aggregate_log_retention(const QuoteSliceMarket &market, double calendar_yield,
                               QuoteSliceDividendModel model) noexcept {
  if (market.dividend_pv == 0.0) return 0.0;
  double fraction = market.dividend_pv / market.spot;
  if (model == QuoteSliceDividendModel::AggregateFundingPresentValue) {
    // Compute PV / (S*exp(-q*T)) without overflowing exp(q*T) separately.
    fraction = std::exp(std::log(market.dividend_pv) - std::log(market.spot) +
                        calendar_yield * market.calendar_years);
  }
  if (!(fraction < 1.0)) return std::numeric_limits<double>::quiet_NaN();
  return std::log1p(-fraction);
}

double converted_yield(const QuoteSliceMarket &market, double calendar_yield,
                        QuoteSliceDividendModel model) noexcept {
  double value = calendar_yield * (market.calendar_years / market.variance_years);
  if (model == QuoteSliceDividendModel::AggregatePresentValue ||
      model == QuoteSliceDividendModel::AggregateFundingPresentValue) {
    value -= aggregate_log_retention(market, calendar_yield, model) / market.variance_years;
  }
  return value;
}

double effective_yield(const QuoteSliceFit &fit, double calendar_yield) noexcept {
  // Preserve the historical arithmetic exactly on every reference-carry row.
  if (calendar_yield == fit.market.continuous_yield) {
    return fit.diagnostics.effective_yield;
  }
  return converted_yield(fit.market, calendar_yield, fit.dividend_model);
}

double local_forward(const QuoteSliceFit &fit, double calendar_yield, double q_effective) noexcept {
  if (calendar_yield == fit.market.continuous_yield) { return fit.diagnostics.forward; }
  const auto &m = fit.market;
  const double growth = std::exp((m.rate - calendar_yield) * m.calendar_years);
  if (fit.dividend_model == QuoteSliceDividendModel::AggregatePresentValue) {
    return (m.spot - m.dividend_pv) * growth;
  }
  if (fit.dividend_model == QuoteSliceDividendModel::AggregateFundingPresentValue) {
    return std::exp(std::log(m.spot) + (m.rate - calendar_yield) * m.calendar_years +
                    aggregate_log_retention(m, calendar_yield, fit.dividend_model));
  }
  double forward = m.spot * growth;
  if (fit.dividend_model == QuoteSliceDividendModel::DatedCash) {
    for (const auto &event : fit.variance_clock_dividends) {
      forward -= event.amount * std::exp((fit.diagnostics.effective_rate - q_effective) *
                                         (m.variance_years - event.tau));
    }
  }
  return forward;
}

Result<double> model_price(const QuoteSliceFit &fit, double strike, Side side, double sigma,
                           double q_effective) {
  const auto &m = fit.market;
  const auto &d = fit.diagnostics;
  if (!std::isfinite(q_effective)) {
    return Err(ErrorCode::OutOfRange, "quote slice: contract yield clock conversion overflow");
  }
  if (fit.dividend_model == QuoteSliceDividendModel::DatedCash) {
    return american_discrete_div_price(m.spot, strike, m.variance_years, sigma, d.effective_rate,
                                       q_effective, side, fit.variance_clock_dividends,
                                       fit.discrete_div_steps, ExerciseStyle::American,
                                       fit.discrete_div_scheme);
  }
  return andersen_lake(m.spot, strike, m.variance_years, sigma, d.effective_rate, q_effective,
                       side, fit.al_opts);
}

// The dated-cash engine has a different forward map from continuous-carry ALO.
// Use that SAME map throughout the bracket; never substitute a continuous IV.
// The CRR probability imposes sigma > |r-q|*sqrt(dt), so the lower endpoint
// must honor the lattice domain instead of treating its error as a zero price.
// Under BBSR the coarse lattice has the larger dt, so it sets the bound.
Result<double> dated_cash_iv(const QuoteSliceFit &fit, const QuoteSliceQuote &quote, double target,
                             const QuoteSliceConfig &cfg, double q_effective) {
  const auto &d = fit.diagnostics;
  const int bound_steps =
      discrete_div_coarsest_steps(fit.discrete_div_steps, fit.discrete_div_scheme);
  const double min_lattice_sigma =
      std::abs(d.effective_rate - q_effective) *
      std::sqrt(fit.market.variance_years / static_cast<double>(bound_steps));
  double lo = std::max(kIvMin, min_lattice_sigma * (1.0 + 1.0e-6));
  if (!(lo < kIvMax)) {
    return Err(ErrorCode::OutOfRange, "dated-cash IV: lattice has no admissible volatility range");
  }
  const auto lower = model_price(fit, quote.strike, quote.side, lo, q_effective);
  if (!lower) {
    return Err(lower.error());
  }
  double f_lo = *lower - target;
  if (!std::isfinite(f_lo) || f_lo >= 0.0) {
    return Err(ErrorCode::OutOfRange, "dated-cash IV: quote at or below the identifiable range");
  }
  double hi = std::min(kIvMax, std::max(0.5, 2.0 * lo));
  double f_hi = 0.0;
  for (unsigned expansion = 0; expansion < 16; ++expansion) {
    const auto upper = model_price(fit, quote.strike, quote.side, hi, q_effective);
    if (!upper) {
      return Err(upper.error());
    }
    f_hi = *upper - target;
    if (!std::isfinite(f_hi)) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite bracket price");
    }
    if (f_hi >= 0.0) {
      break;
    }
    if (hi == kIvMax) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: price exceeds maximum-volatility price");
    }
    hi = std::min(kIvMax, 2.0 * hi);
  }
  for (std::uint16_t iteration = 0; iteration < cfg.max_iv_iterations; ++iteration) {
    const double width = hi - lo;
    if (width <= 2.0 * cfg.iv_tolerance) {
      return Ok(0.5 * (lo + hi));
    }
    // A safeguarded secant retains a shrinking sign-change bracket, including
    // exercise plateaus where Newton's vega would vanish.
    const double fraction = std::clamp(-f_lo / (f_hi - f_lo), 0.1, 0.9);
    const double sigma = lo + fraction * width;
    const auto value = model_price(fit, quote.strike, quote.side, sigma, q_effective);
    if (!value) {
      return Err(value.error());
    }
    const double residual = *value - target;
    if (!std::isfinite(residual)) {
      return Err(ErrorCode::OutOfRange, "dated-cash IV: non-finite price");
    }
    if (residual == 0.0) {
      return Ok(sigma);
    }
    if (residual < 0.0) {
      lo = sigma;
      f_lo = residual;
    } else {
      hi = sigma;
      f_hi = residual;
    }
  }
  return Err(ErrorCode::Unavailable, "dated-cash IV: iteration budget exhausted");
}

} // namespace

const char *to_string(QuoteSliceDividendModel value) noexcept {
  switch (value) {
  case QuoteSliceDividendModel::ContinuousYield:
    return "continuous_yield";
  case QuoteSliceDividendModel::AggregatePresentValue:
    return "aggregate_pv_approximation";
  case QuoteSliceDividendModel::AggregateFundingPresentValue:
    return "aggregate_funding_pv_approximation";
  case QuoteSliceDividendModel::DatedCash:
    return "dated_cash_lattice";
  }
  return "unknown";
}
const char *to_string(QuoteSliceQuoteStatus value) noexcept {
  switch (value) {
  case QuoteSliceQuoteStatus::Admitted:
    return "admitted";
  case QuoteSliceQuoteStatus::InvalidContract:
    return "invalid_contract";
  case QuoteSliceQuoteStatus::InvalidQuote:
    return "invalid_quote";
  case QuoteSliceQuoteStatus::ExcludedSide:
    return "excluded_side";
  case QuoteSliceQuoteStatus::UnidentifiableVolatility:
    return "unidentifiable_volatility";
  case QuoteSliceQuoteStatus::ImpliedVolatilityFailure:
    return "implied_volatility_failure";
  case QuoteSliceQuoteStatus::InvalidObservation:
    return "invalid_observation";
  case QuoteSliceQuoteStatus::BidlessQuote:
    return "bidless_quote";
  case QuoteSliceQuoteStatus::BelowMinimumBid:
    return "below_minimum_bid";
  case QuoteSliceQuoteStatus::WideSpreadToMid:
    return "wide_spread_to_mid";
  case QuoteSliceQuoteStatus::WideSpreadVol:
    return "wide_spread_vol";
  case QuoteSliceQuoteStatus::LowVegaWeight:
    return "low_vega_weight";
  }
  return "unknown";
}
const char *to_string(QuoteSliceEvaluationStatus value) noexcept {
  switch (value) {
  case QuoteSliceEvaluationStatus::Success:
    return "success";
  case QuoteSliceEvaluationStatus::InvalidContract:
    return "invalid_contract";
  case QuoteSliceEvaluationStatus::NoFit:
    return "no_fit";
  case QuoteSliceEvaluationStatus::InvalidVolatility:
    return "invalid_volatility";
  case QuoteSliceEvaluationStatus::PricingFailure:
    return "pricing_failure";
  case QuoteSliceEvaluationStatus::PriceOnly:
    return "price_only";
  }
  return "unknown";
}
const char *to_string(QuoteSliceFitStatus value) noexcept {
  switch (value) {
  case QuoteSliceFitStatus::Success:
    return "success";
  case QuoteSliceFitStatus::InsufficientQuotes:
    return "insufficient_quotes";
  case QuoteSliceFitStatus::FitFailure:
    return "fit_failure";
  case QuoteSliceFitStatus::ArbitrageFailure:
    return "arbitrage_failure";
  }
  return "unknown";
}
const char *to_string(QuoteSliceRepairOutcome value) noexcept {
  switch (value) {
  case QuoteSliceRepairOutcome::NotAttempted:
    return "not_attempted";
  case QuoteSliceRepairOutcome::Applied:
    return "applied";
  case QuoteSliceRepairOutcome::NoMove:
    return "no_move";
  case QuoteSliceRepairOutcome::Infeasible:
    return "infeasible";
  case QuoteSliceRepairOutcome::LeftChannel:
    return "left_channel";
  case QuoteSliceRepairOutcome::NodeBudget:
    return "node_budget";
  case QuoteSliceRepairOutcome::VolBudget:
    return "vol_budget";
  case QuoteSliceRepairOutcome::NotCertified:
    return "not_certified";
  }
  return "unknown";
}

Result<QuoteSliceValue> evaluate_quote_slice(const QuoteSliceFit &fit, double strike, Side side) {
  if (!positive(strike) || !valid_side(side)) {
    return Err(ErrorCode::InvalidArgument, "quote slice: invalid query contract");
  }
  if (fit.fit_status != QuoteSliceFitStatus::Success || !fit.curve || !fit.prices) {
    return Err(ErrorCode::Unavailable, "quote slice: no certified price slice");
  }
  const auto price = fit.prices->price(strike, side);
  if (!price) {
    return Err(ErrorCode::OutOfRange, "quote slice: strike outside certified price domain");
  }
  const auto nodes = fit.prices->nodes();
  const auto node = std::lower_bound(nodes.begin(), nodes.end(), strike,
      [](const AmericanPriceNode &value, double k) { return value.strike < k; });
  const auto index = static_cast<std::size_t>(node - nodes.begin());
  const unsigned bit = side == Side::Call ? 1u : 2u;
  const bool at_node = node != nodes.end() && node->strike == strike;
  if (at_node && index < fit.price_only_nodes.size() && (fit.price_only_nodes[index] & bit) != 0u) {
    // A PriceOnly row's node side: its volatility stays refused even when a later
    // calendar repair moved the price (QuoteSliceFit::price_only_nodes).
    return Err(ErrorCode::OutOfRange, "quote slice: node side served price-only (no IV)");
  }
  const bool moved = index < fit.moved_nodes.size() && (fit.moved_nodes[index] & bit) != 0u;
  if (at_node && !moved) {
    const double sigma = fit.curve->iv(std::log(strike / fit.diagnostics.forward));
    if (!positive(sigma)) {
      return Err(ErrorCode::OutOfRange, "quote slice: invalid node volatility");
    }
    return Ok(QuoteSliceValue{sigma, *price});
  }
  const double q = effective_yield(fit, calendar_yield_at(fit, strike));
  QuoteSliceConfig query_config;
  query_config.discrete_div_steps = fit.discrete_div_steps;
  const QuoteSliceQuote query{strike, side, *price, *price, {}};
  const auto sigma = fit.dividend_model == QuoteSliceDividendModel::DatedCash
      ? dated_cash_iv(fit, query, *price, query_config, q)
      : american_implied_vol(*price, fit.market.spot, strike, fit.market.variance_years,
                             fit.diagnostics.effective_rate, q, side,
                             AmericanMethod::AndersenLake, query_config.iv_tolerance,
                             query_config.max_iv_iterations, fit.al_opts);
  if (!sigma || !positive(*sigma)) {
    return Err(ErrorCode::OutOfRange, "quote slice: certified price has no identifiable IV");
  }
  return Ok(QuoteSliceValue{*sigma, *price});
}

namespace {

// Stage 1, shared by every fit method: validation, clocks, forward and carry.
Status prepare_fit(QuoteSliceFit &out, const QuoteSliceMarket &market,
                   std::span<const QuoteSliceQuote> quotes, const QuoteSliceConfig &config,
                   std::span<const CashDividend> calendar_dividends) {
  const Status valid = validate(market, quotes, config, calendar_dividends);
  if (!valid) {
    return Err(valid.error());
  }

  out.market = market;
  out.dividend_model = config.dividend_model;
  out.al_opts = config.al_opts;
  out.discrete_div_steps = config.discrete_div_steps;
  out.discrete_div_scheme = config.discrete_div_scheme;
  auto &diag = out.diagnostics;
  diag.quotes_seen = quotes.size();
  const double ratio = market.calendar_years / market.variance_years;
  diag.effective_rate = market.rate * ratio;
  diag.effective_yield = converted_yield(market, market.continuous_yield, config.dividend_model);
  diag.discount = std::exp(-market.rate * market.calendar_years);
  const double growth = std::exp((market.rate - market.continuous_yield) * market.calendar_years);
  diag.forward = market.spot * growth;
  if (config.dividend_model == QuoteSliceDividendModel::AggregatePresentValue) {
    diag.forward = (market.spot - market.dividend_pv) * growth;
  } else if (config.dividend_model == QuoteSliceDividendModel::AggregateFundingPresentValue) {
    diag.forward = std::exp(std::log(market.spot) +
        (market.rate - market.continuous_yield) * market.calendar_years +
        aggregate_log_retention(market, market.continuous_yield, config.dividend_model));
  } else if (config.dividend_model == QuoteSliceDividendModel::DatedCash) {
    out.variance_clock_dividends.reserve(calendar_dividends.size());
    const double window = market.calendar_years * (1.0 + kTauAtExpiryRelTol);
    for (const CashDividend &event : calendar_dividends) {
      if (!(event.tau > 0.0) || event.tau > window || event.amount == 0.0) {
        continue;
      }
      const double tau = std::min(event.tau, market.calendar_years);
      const double variance_tau = tau / ratio;
      if (!positive(variance_tau)) {
        return Err(ErrorCode::OutOfRange, "quote slice: dividend clock conversion overflow");
      }
      out.variance_clock_dividends.push_back(CashDividend{variance_tau, event.amount});
      diag.forward -= event.amount * std::exp((market.rate - market.continuous_yield) *
                                              (market.calendar_years - tau));
    }
  }
  if (!positive(diag.forward) || !positive(diag.discount) || !std::isfinite(diag.effective_rate) ||
      !std::isfinite(diag.effective_yield)) {
    return Err(ErrorCode::OutOfRange, "quote slice: invalid derived carry or forward");
  }
  const auto carry_status = build_carry_curve(out, quotes);
  if (!carry_status) { return Err(carry_status.error()); }
  return Ok();
}

// Stage 2: point-quote admission (quality gates, mid IV, inverse-uncertainty
// weights). Records every row's status and returns the admitted observations
// sorted by strike, with diagnostics.distinct_strikes counted. A vol-space fit
// reads each mid IV from its chain's cache (`deam`).
std::vector<FitObs> admit_point_observations(QuoteSliceFit &out,
                                             std::span<const QuoteSliceQuote> quotes,
                                             const QuoteSliceConfig &config,
                                             DeAmIvView *deam = nullptr) {
  const counters::fit_timing::BucketScope timing(counters::fit_timing::Bucket::Deam);
  const QuoteSliceMarket &market = out.market;
  auto &diag = out.diagnostics;
  out.rows.resize(quotes.size());
  std::vector<FitObs> observations;
  observations.reserve(quotes.size());
  const double min_sensitivity = std::sqrt(config.quality.min_vega_weight);
  const double max_weight_root = std::sqrt(config.quality.max_weight);
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const QuoteSliceQuote &quote = quotes[i];
    QuoteSliceRow &row = out.rows[i];
    if (!positive(quote.strike) || !valid_side(quote.side)) {
      row.quote_status = QuoteSliceQuoteStatus::InvalidContract;
      row.evaluation_status = QuoteSliceEvaluationStatus::InvalidContract;
      continue;
    }
    if (!nonnegative(quote.bid) || !positive(quote.ask) || quote.bid > quote.ask) {
      continue;
    }
    if (quote.bid == 0.0) {
      row.quote_status = QuoteSliceQuoteStatus::BidlessQuote;
      continue;
    }
    if (quote.bid == quote.ask) {
      continue;
    }
    if (quote.bid < config.quality.min_bid) {
      row.quote_status = QuoteSliceQuoteStatus::BelowMinimumBid;
      continue;
    }
    const double calendar_yield = quote.continuous_yield.value_or(market.continuous_yield);
    const double q_effective = effective_yield(out, calendar_yield);
    const double forward = local_forward(out, calendar_yield, q_effective);
    if (!positive(forward) || !std::isfinite(q_effective)) {
      row.quote_status = QuoteSliceQuoteStatus::InvalidObservation;
      continue;
    }
    const double k = std::log(quote.strike / diag.forward);
    const double local_k = forward == diag.forward ? k : std::log(quote.strike / forward);
    if (config.side_policy == QuoteSliceSidePolicy::OutOfTheMoney &&
        std::abs(local_k) > config.atm_log_band &&
        ((local_k < 0.0 && quote.side == Side::Call) ||
         (local_k > 0.0 && quote.side == Side::Put))) {
      row.quote_status = QuoteSliceQuoteStatus::ExcludedSide;
      continue;
    }
    const double mid = 0.5 * quote.bid + 0.5 * quote.ask;
    const double raw_spread = quote.ask - quote.bid;
    if (config.quality.max_spread_to_mid > 0.0 &&
        raw_spread / mid > config.quality.max_spread_to_mid) {
      row.quote_status = QuoteSliceQuoteStatus::WideSpreadToMid;
      continue;
    }
    const double intrinsic = std::max(
        quote.side == Side::Call ? market.spot - quote.strike : quote.strike - market.spot, 0.0);
    if (!(mid - intrinsic > config.min_time_value)) {
      row.quote_status = QuoteSliceQuoteStatus::UnidentifiableVolatility;
      continue;
    }
    ++diag.iv_attempted;
    const auto iv =
        deam != nullptr ? deam->mid_iv(i)
        : config.dividend_model == QuoteSliceDividendModel::DatedCash
            ? dated_cash_iv(out, quote, mid, config, q_effective)
            : american_implied_vol(mid, market.spot, quote.strike, market.variance_years,
                                   diag.effective_rate, q_effective, quote.side,
                                   AmericanMethod::AndersenLake, config.iv_tolerance,
                                   config.max_iv_iterations, config.al_opts);
    if (!iv || !positive(*iv)) {
      row.quote_status = QuoteSliceQuoteStatus::ImpliedVolatilityFailure;
      continue;
    }
    row.market_iv = *iv;
    if (*iv <= kIvMin || *iv >= kIvMax) {
      row.quote_status = QuoteSliceQuoteStatus::UnidentifiableVolatility;
      continue;
    }
    const auto euro = black76_value_and_vega(diag.forward, quote.strike, market.variance_years, *iv,
                                             diag.discount, quote.side);
    const double spread = std::max(raw_spread, config.spread_floor);
    const double local_vega = forward == diag.forward ? euro.vega :
        black76_value_and_vega(forward, quote.strike, market.variance_years, *iv,
                               diag.discount, quote.side).vega;
    const double variance = *iv * *iv * market.variance_years;
    if (!positive(euro.vega) || !positive(local_vega) || !nonnegative(euro.price) ||
        !positive(variance) || !std::isfinite(k)) {
      row.quote_status = QuoteSliceQuoteStatus::InvalidObservation;
      continue;
    }
    row.spread_vol = spread / local_vega;
    if (config.quality.max_spread_vol > 0.0 &&
        row.spread_vol > config.quality.max_spread_vol) {
      row.quote_status = QuoteSliceQuoteStatus::WideSpreadVol;
      continue;
    }
    // Compare square roots to avoid overflow in a high-sensitivity quote's
    // inverse variance. The filter and cap use the local quote's uncertainty,
    // independently of the reference-forward coordinate used by the curve.
    const double sensitivity = local_vega / spread;
    if (sensitivity < min_sensitivity) {
      row.quote_status = QuoteSliceQuoteStatus::LowVegaWeight;
      continue;
    }
    // IV uncertainty belongs to the actual quote's carry. Price-space fitters
    // receive an equivalent spread in the common reference-forward coordinate.
    const double normalized_spread = local_vega == euro.vega ? spread :
        spread * (euro.vega / local_vega);
    const double weight_root = sensitivity / (2.0 * *iv * market.variance_years);
    const bool capped = weight_root >= max_weight_root;
    const double weight = capped ? config.quality.max_weight : weight_root * weight_root;
    if (!positive(weight) || !positive(normalized_spread) || !positive(row.spread_vol)) {
      row.quote_status = QuoteSliceQuoteStatus::InvalidObservation;
      continue;
    }
    FitObs obs;
    obs.k = k;
    obs.sigma_mkt = *iv;
    obs.score_sigma_mkt = *iv;
    obs.w_mkt = variance;
    obs.weight_w = weight;
    obs.active_weight_w = weight;
    obs.K = quote.strike;
    obs.F = diag.forward;
    obs.df = diag.discount;
    obs.mid = euro.price;
    obs.spread = normalized_spread;
    obs.vega = euro.vega;
    obs.noise_sigma = row.spread_vol;
    obs.side = quote.side;
    obs.source_strike_index = static_cast<std::uint32_t>(i);
    observations.push_back(obs);
    row.quote_status = QuoteSliceQuoteStatus::Admitted;
    row.admitted = true;
    row.weight_w = weight;
    row.weight_capped = capped;
    if (capped) { ++diag.weights_capped; }
    ++diag.quotes_admitted;
  }
  std::stable_sort(observations.begin(), observations.end(),
                   [](const FitObs &a, const FitObs &b) { return a.K < b.K; });
  double previous_strike = -1.0;
  for (const FitObs &obs : observations) {
    if (obs.K != previous_strike) {
      ++diag.distinct_strikes;
      previous_strike = obs.K;
    }
  }
  return observations;
}

// American IV of `target` at the contract's own carry, with the same forward
// map as point admission. Err when the solver cannot identify a volatility.
Result<double> contract_iv(const QuoteSliceFit &fit, const QuoteSliceQuote &quote, double target,
                           const QuoteSliceConfig &config, double q_effective) {
  if (fit.dividend_model == QuoteSliceDividendModel::DatedCash) {
    return dated_cash_iv(fit, quote, target, config, q_effective);
  }
  return american_implied_vol(target, fit.market.spot, quote.strike, fit.market.variance_years,
                              fit.diagnostics.effective_rate, q_effective, quote.side,
                              AmericanMethod::AndersenLake, config.iv_tolerance,
                              config.max_iv_iterations, fit.al_opts);
}

// The slice's American model for the opt-in vol-space cache (deam_iv_cache.hpp):
// the Accurate kernel IS model_price / contract_iv, the Fast kernel the same
// Andersen-Lake scheme on AlProfile::Fast. Reads only fields prepare_fit fixed.
class SliceDeAmModel final : public DeAmModel {
public:
  SliceDeAmModel(const QuoteSliceFit &fit, const QuoteSliceConfig &config)
      : fit_(fit), config_(config), fast_(al_fast_profile(fit.al_opts)) {}

  double effective_yield_of(double calendar_yield) const override {
    return effective_yield(fit_, calendar_yield);
  }
  double local_forward_of(double calendar_yield, double q_effective) const override {
    return local_forward(fit_, calendar_yield, q_effective);
  }
  double reference_yield() const override { return fit_.market.continuous_yield; }
  bool has_fast_kernel() const override {
    return fit_.dividend_model != QuoteSliceDividendModel::DatedCash;
  }
  Result<double> price(const QuoteSliceQuote &quote, double sigma, double q_effective,
                       DeAmKernel kernel) const override {
    if (kernel == DeAmKernel::Accurate || !has_fast_kernel() || !std::isfinite(q_effective)) {
      return model_price(fit_, quote.strike, quote.side, sigma, q_effective);
    }
    return andersen_lake(fit_.market.spot, quote.strike, fit_.market.variance_years, sigma,
                         fit_.diagnostics.effective_rate, q_effective, quote.side, fast_);
  }
  Result<double> implied_vol(const QuoteSliceQuote &quote, double target, double q_effective,
                             DeAmKernel kernel) const override {
    if (kernel == DeAmKernel::Accurate || !has_fast_kernel()) {
      return contract_iv(fit_, quote, target, config_, q_effective);
    }
    return american_implied_vol(target, fit_.market.spot, quote.strike,
                                fit_.market.variance_years, fit_.diagnostics.effective_rate,
                                q_effective, quote.side, AmericanMethod::AndersenLake,
                                config_.iv_tolerance, config_.max_iv_iterations, fast_);
  }
  // Premium (L2e): contract_iv's own forward map and controls; a fallback returns
  // contract_iv's value bit for bit (american_iv_premium.hpp). The dated-cash lattice
  // has no Premium route: contract_iv itself.
  Result<double> implied_vol_premium(const QuoteSliceQuote &quote, double target,
                                     double q_effective, IvPremiumMemory *memory,
                                     IvPremiumStats &stats) const override {
    if (fit_.dividend_model == QuoteSliceDividendModel::DatedCash) {
      stats = IvPremiumStats{};
      return contract_iv(fit_, quote, target, config_, q_effective);
    }
    IvOpts opts;
    opts.tol = config_.iv_tolerance;
    opts.max_iter = config_.max_iv_iterations;
    opts.al_opts = fit_.al_opts;
    const IvResult iv = american_implied_vol_premium(
        target, fit_.market.spot, quote.strike, fit_.market.variance_years,
        fit_.diagnostics.effective_rate, q_effective, quote.side, opts, memory, stats);
    if (iv.status != IvStatus::Ok) {
      return Err(ErrorCode::Unavailable, "quote slice: no American implied volatility");
    }
    return Ok(iv.iv);
  }
  double black_vega(const QuoteSliceQuote &quote, double sigma, double forward) const override {
    return black76_value_and_vega(forward, quote.strike, fit_.market.variance_years, sigma,
                                  fit_.diagnostics.discount, quote.side)
        .vega;
  }

private:
  const QuoteSliceFit &fit_;
  const QuoteSliceConfig &config_;
  std::optional<AlOpts> fast_;
};

// The chain cache an opt-in vol-space fit reads (QuoteSliceFitSpace::VolChannel):
// when deam_cache is set, the expiry's entry for the exact inputs in the rung memo
// installed by the surface fitter (QuoteSurfaceConfig::rung_memo, rung_memo.hpp), else
// in this thread's memo; without deam_cache a cache this fit owns. Construct after
// prepare_fit; not movable (the view holds references to the members).
class VolSpaceCache {
public:
  VolSpaceCache(QuoteSliceFit &fit, std::span<const QuoteSliceQuote> quotes,
                const QuoteSliceConfig &config)
      : model_(fit, config) {
    QuoteSliceDeamDiagnostics &work = fit.diagnostics.deam;
    work.used = true;
    if (config.channel.deam_cache) {
      DeAmIvKey key = deam_iv_key(fit, quotes, config);
      detail::ExpiryRungMemo *const memo = detail::current_rung_memo();
      cache_ = memo != nullptr ? memo->deam_cache(std::move(key), quotes.size(), work.reused)
                               : deam_iv_memo(std::move(key), quotes.size(), work.reused);
    } else {
      cache_ = std::make_shared<DeAmIvCache>();
      cache_->ivs.assign(quotes.size(), DeAmQuoteIv{});
    }
    view_.emplace(*cache_, model_, quotes, config.channel.deam_profile, work,
                  config.channel.deam_inversion);
  }
  VolSpaceCache(const VolSpaceCache &) = delete;
  VolSpaceCache &operator=(const VolSpaceCache &) = delete;
  VolSpaceCache(VolSpaceCache &&) = delete;
  VolSpaceCache &operator=(VolSpaceCache &&) = delete;
  ~VolSpaceCache() = default;

  [[nodiscard]] DeAmIvView &view() noexcept { return *view_; }

private:
  SliceDeAmModel model_;
  std::shared_ptr<DeAmIvCache> cache_; // never null after construction
  std::optional<DeAmIvView> view_;
};

// Half-spread of each node's own quote per side (bidless: half the ask). A side
// without a quote borrows the other side's; a strike without quotes gets zero,
// which forbids moving it.
void node_half_spreads(std::span<const AmericanPriceNode> nodes,
                       std::span<const QuoteSliceQuote> quotes, std::vector<double> &call,
                       std::vector<double> &put) {
  constexpr double kMissing = std::numeric_limits<double>::infinity();
  call.assign(nodes.size(), kMissing);
  put.assign(nodes.size(), kMissing);
  for (const QuoteSliceQuote &quote : quotes) {
    if (!positive(quote.strike) || !valid_side(quote.side) || !nonnegative(quote.bid) ||
        !positive(quote.ask) || !(quote.ask > quote.bid)) {
      continue;
    }
    const auto it = std::lower_bound(nodes.begin(), nodes.end(), quote.strike,
        [](const AmericanPriceNode &node, double k) { return node.strike < k; });
    if (it == nodes.end() || it->strike != quote.strike) continue;
    const auto index = static_cast<std::size_t>(it - nodes.begin());
    double &slot = quote.side == Side::Call ? call[index] : put[index];
    slot = std::min(slot, 0.5 * (quote.ask - quote.bid));
  }
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (!std::isfinite(call[i])) call[i] = std::isfinite(put[i]) ? put[i] : 0.0;
    if (!std::isfinite(put[i])) put[i] = call[i];
  }
}

// Greatest convex minorant (lower hull) of one side's node prices.
void lower_hull(std::span<const AmericanPriceNode> nodes, Side side, std::vector<double> &target) {
  const auto value = [side](const AmericanPriceNode &node) {
    return side == Side::Call ? node.call : node.put;
  };
  std::vector<std::size_t> hull;
  hull.reserve(nodes.size());
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    while (hull.size() >= 2) {
      const auto &left = nodes[hull[hull.size() - 2]];
      const auto &middle = nodes[hull.back()];
      const double fraction = (middle.strike - left.strike) / (nodes[i].strike - left.strike);
      if (value(middle) <= std::lerp(value(left), value(nodes[i]), fraction)) break;
      hull.pop_back();
    }
    hull.push_back(i);
  }
  target.resize(nodes.size());
  for (std::size_t segment = 0; segment < hull.size(); ++segment) {
    target[hull[segment]] = value(nodes[hull[segment]]);
    if (segment == 0) continue;
    const auto &left = nodes[hull[segment - 1]];
    const auto &right = nodes[hull[segment]];
    for (std::size_t i = hull[segment - 1] + 1; i < hull[segment]; ++i) {
      const double fraction = (nodes[i].strike - left.strike) / (right.strike - left.strike);
      target[i] = std::min(value(nodes[i]), std::lerp(value(left), value(right), fraction));
    }
  }
}

// Bounded-move repair: lower both sides to their convex minorants when every
// required shift from the fitted price is within `half_spreads` of the node's
// own half-spread (a bound on the move, not containment in [bid, ask]). Records
// the required shift either way; returns whether nodes were changed.
bool repair_convexity(std::vector<AmericanPriceNode> &nodes,
                      std::span<const QuoteSliceQuote> quotes, double half_spreads,
                      QuoteSliceChannelDiagnostics &diag, std::vector<std::uint8_t> &repaired) {
  std::vector<double> call_spread;
  std::vector<double> put_spread;
  node_half_spreads(nodes, quotes, call_spread, put_spread);
  std::vector<double> call_target;
  std::vector<double> put_target;
  lower_hull(nodes, Side::Call, call_target);
  lower_hull(nodes, Side::Put, put_target);
  bool within = true;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    for (const Side side : {Side::Call, Side::Put}) {
      const bool call = side == Side::Call;
      const double shift = call ? nodes[i].call - call_target[i] : nodes[i].put - put_target[i];
      if (!(shift > 0.0)) continue;
      const double spread = call ? call_spread[i] : put_spread[i];
      const double ratio =
          spread > 0.0 ? shift / spread : std::numeric_limits<double>::infinity();
      diag.repair_max_price = std::max(diag.repair_max_price, shift);
      diag.repair_max_half_spreads = std::max(diag.repair_max_half_spreads, ratio);
      within = within && ratio <= half_spreads;
    }
  }
  if (!within) return false;
  repaired.assign(nodes.size(), 0);
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (call_target[i] < nodes[i].call) {
      nodes[i].call = call_target[i];
      repaired[i] |= 1u;
    }
    if (put_target[i] < nodes[i].put) {
      nodes[i].put = put_target[i];
      repaired[i] |= 2u;
    }
    diag.repaired_nodes += repaired[i] != 0 ? 1u : 0u;
  }
  return true;
}

// ── Channel repair (QuoteSliceRepairConfig) ─────────────────────────────────

// The certificate's fixed numerical allowance (american_price_slice.cpp).
double certificate_tolerance(double spot, double strike) noexcept {
  return 1.0e-8 + 1.0e-9 * std::max(spot, strike);
}

// Lowers, per side, only the nodes more than the certificate allowance above the
// side's lower convex envelope, onto that envelope (the same hull construction
// and interpolation as the certificate). Hull vertices never move, so the
// envelope of the untouched nodes is unchanged. ORs 1 (call) / 2 (put) into
// `moved` (resized to the node count) for every node it lowers.
void lower_violators(std::vector<AmericanPriceNode> &nodes, double spot,
                     std::vector<std::uint8_t> &moved) {
  moved.resize(nodes.size(), 0);
  std::vector<double> target;
  for (const Side side : {Side::Call, Side::Put}) {
    const bool call = side == Side::Call;
    lower_hull(nodes, side, target);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      double &value = call ? nodes[i].call : nodes[i].put;
      if (value - target[i] > certificate_tolerance(spot, nodes[i].strike)) {
        value = target[i];
        moved[i] |= call ? 1u : 2u;
      }
    }
  }
}

// The tightest own-quote channel of every node side: [max bid, min ask] over the
// valid quotes at that strike and side (bidless: [0, ask]). A side without a
// valid quote has the unbounded channel, i.e. no containment constraint.
struct NodeChannels {
  std::vector<double> call_lo, call_hi, put_lo, put_hi;
};

NodeChannels node_channels(std::span<const AmericanPriceNode> nodes,
                           std::span<const QuoteSliceQuote> quotes) {
  constexpr double kInf = std::numeric_limits<double>::infinity();
  NodeChannels c{std::vector<double>(nodes.size(), -kInf), std::vector<double>(nodes.size(), kInf),
                 std::vector<double>(nodes.size(), -kInf), std::vector<double>(nodes.size(), kInf)};
  for (const QuoteSliceQuote &quote : quotes) {
    if (!positive(quote.strike) || !valid_side(quote.side) || !nonnegative(quote.bid) ||
        !positive(quote.ask) || !(quote.ask > quote.bid)) {
      continue;
    }
    const auto it = std::lower_bound(nodes.begin(), nodes.end(), quote.strike,
        [](const AmericanPriceNode &node, double k) { return node.strike < k; });
    if (it == nodes.end() || it->strike != quote.strike) continue;
    const auto i = static_cast<std::size_t>(it - nodes.begin());
    auto &lo = quote.side == Side::Call ? c.call_lo[i] : c.put_lo[i];
    auto &hi = quote.side == Side::Call ? c.call_hi[i] : c.put_hi[i];
    lo = std::max(lo, quote.bid);
    hi = std::min(hi, quote.ask);
  }
  return c;
}

double channel_distance(double price, double lo, double hi) noexcept {
  return std::max({lo - price, price - hi, 0.0});
}

// Weighted least-squares projection (weights `w`) of one side's prices `p` (in place)
// onto { convex in strike; first slope >= slope_min; last slope <= slope_max;
// lo <= p <= hi }: argmin sum w_i (p_i - p0_i)^2 by Dykstra's algorithm, in
// Hildreth's form for the half-spaces (one nonnegative multiplier each) plus a
// full increment for the box. Convex + end slopes covers every vertical-spread
// bound of the certificate. Nodes whose constraints never bind are untouched
// bit for bit. Bounded: at most kMaxSweeps sweeps of O(n). False when the box
// is empty or the sweeps do not settle; the caller then refuses the repair.
bool project_convex_box(std::span<const double> strikes, std::vector<double> &p,
                        std::span<const double> lo, std::span<const double> hi,
                        std::span<const double> w, double slope_min, double slope_max) {
  constexpr int kMaxSweeps = 20000;
  const std::size_t n = p.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (!(lo[i] <= hi[i])) return false;
  }
  if (n < 2) return true;
  // Half-space j: sum_k a_jk p_k >= b_j over three (convexity) or two (slope)
  // consecutive nodes starting at first[j].
  struct HalfSpace {
    std::size_t first;
    std::size_t count;
    std::array<double, 3> a;
    double b;
    double norm; // a W^-1 a
    double mu;   // Dykstra multiplier, >= 0
  };
  std::vector<HalfSpace> spaces;
  spaces.reserve(n);
  const auto inverse_norm = [&](HalfSpace &h) {
    h.norm = 0.0;
    for (std::size_t k = 0; k < h.count; ++k) h.norm += h.a[k] * h.a[k] / w[h.first + k];
  };
  for (std::size_t i = 1; i + 1 < n; ++i) {
    const double left = 1.0 / (strikes[i] - strikes[i - 1]);
    const double right = 1.0 / (strikes[i + 1] - strikes[i]);
    HalfSpace h{i - 1, 3, {left, -(left + right), right}, 0.0, 0.0, 0.0};
    inverse_norm(h);
    spaces.push_back(h);
  }
  const double first_width = 1.0 / (strikes[1] - strikes[0]);
  const double last_width = 1.0 / (strikes[n - 1] - strikes[n - 2]);
  HalfSpace first{0, 2, {-first_width, first_width, 0.0}, slope_min, 0.0, 0.0};
  HalfSpace last{n - 2, 2, {last_width, -last_width, 0.0}, -slope_max, 0.0, 0.0};
  inverse_norm(first);
  inverse_norm(last);
  spaces.push_back(first);
  spaces.push_back(last);
  std::vector<double> box(n, 0.0);
  double scale = 1.0;
  for (const double value : p) scale = std::max(scale, std::abs(value));
  const double tolerance = 1.0e-13 * scale;
  for (int sweep = 0; sweep < kMaxSweeps; ++sweep) {
    double change = 0.0;
    for (HalfSpace &h : spaces) {
      double dot = 0.0;
      for (std::size_t k = 0; k < h.count; ++k) dot += h.a[k] * p[h.first + k];
      const double mu = std::max(0.0, h.mu + (h.b - dot) / h.norm);
      const double step = mu - h.mu;
      if (step == 0.0) continue;
      h.mu = mu;
      for (std::size_t k = 0; k < h.count; ++k) {
        const double delta = step * h.a[k] / w[h.first + k];
        p[h.first + k] += delta;
        change = std::max(change, std::abs(delta));
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      const double z = p[i] + box[i];
      const double clamped = std::clamp(z, lo[i], hi[i]);
      change = std::max(change, std::abs(clamped - p[i]));
      box[i] = z - clamped;
      p[i] = clamped;
    }
    if (change <= tolerance) return true;
  }
  return false;
}

// Per-node box of one side for the channel projection: the node's own quote
// channel widened to contain the starting price (so no node can end farther
// outside its channel than it started), intersected with the certificate's
// per-node price bounds; a side without its own quote is held fixed. The
// least-squares weights are uniform: the box already carries each node's
// channel, and spread-scaled weights (ratios up to ~1e5 across a chain) make
// the cyclic projection converge far too slowly.
void channel_box(std::span<const AmericanPriceNode> nodes, Side side, const NodeChannels &channel,
                 const AmericanPriceBounds &bounds, std::vector<double> &lo,
                 std::vector<double> &hi, std::vector<double> &w) {
  const bool call = side == Side::Call;
  lo.resize(nodes.size());
  hi.resize(nodes.size());
  w.assign(nodes.size(), 1.0);
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const double k = nodes[i].strike;
    const double price = call ? nodes[i].call : nodes[i].put;
    const double bid = call ? channel.call_lo[i] : channel.put_lo[i];
    const double ask = call ? channel.call_hi[i] : channel.put_hi[i];
    if (!std::isfinite(ask)) {
      lo[i] = price;
      hi[i] = price;
      continue;
    }
    // The certificate's own per-node bounds are hard (an empty box refuses).
    const double intrinsic = std::max(call ? bounds.spot - k : k - bounds.spot, 0.0);
    const double upper = call ? bounds.prepaid_max : k * bounds.discount_max;
    lo[i] = std::max(std::min(bid, price), intrinsic);
    hi[i] = std::min(std::max(ask, price), upper);
  }
}

// Calendar clamp of a slice's nodes against accepted slices: floors from the
// earlier ones, caps from the later ones, wherever a node lies in their domain.
struct CalendarClamp {
  std::vector<double> floor_call, floor_put, cap_call, cap_put;
};

// Projects both sides of `nodes` inside their channels (and, with `clamp`,
// inside the calendar floors/caps), then lowers any residual certificate
// violation onto its envelope. False when a box is empty or a projection does
// not settle.
bool project_in_channel(std::vector<AmericanPriceNode> &nodes,
                        std::span<const QuoteSliceQuote> quotes,
                        const AmericanPriceBounds &bounds, const CalendarClamp *clamp) {
  const counters::fit_timing::BucketScope timing(counters::fit_timing::Bucket::Project);
  counters::fit_timing::count(&counters::fit_timing::FitTimingCounters::projections);
  const NodeChannels channel = node_channels(nodes, quotes);
  std::vector<double> strikes(nodes.size());
  for (std::size_t i = 0; i < nodes.size(); ++i) strikes[i] = nodes[i].strike;
  std::vector<double> lo;
  std::vector<double> hi;
  std::vector<double> w;
  std::vector<double> p(nodes.size());
  for (const Side side : {Side::Call, Side::Put}) {
    const bool call = side == Side::Call;
    channel_box(nodes, side, channel, bounds, lo, hi, w);
    if (clamp != nullptr) {
      for (std::size_t i = 0; i < nodes.size(); ++i) {
        lo[i] = std::max(lo[i], call ? clamp->floor_call[i] : clamp->floor_put[i]);
        hi[i] = std::min(hi[i], call ? clamp->cap_call[i] : clamp->cap_put[i]);
      }
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) p[i] = call ? nodes[i].call : nodes[i].put;
    // Calls fall with slope >= -discount_max; puts rise with slope <= discount_max.
    const double slope_min = call ? -bounds.discount_max : 0.0;
    const double slope_max = call ? 0.0 : bounds.discount_max;
    if (!project_convex_box(strikes, p, lo, hi, w, slope_min, slope_max)) return false;
    for (std::size_t i = 0; i < nodes.size(); ++i) (call ? nodes[i].call : nodes[i].put) = p[i];
  }
  std::vector<std::uint8_t> lowered;
  lower_violators(nodes, bounds.spot, lowered);
  return true;
}

// Budget check of a proposed move `before` -> `after` (same strikes). Fills
// `moved` (1 call / 2 put per node) and `record` (nodes, largest price and
// volatility moves against the fitted curve, unidentified node sides) and
// returns Applied when the move satisfies QuoteSliceRepairConfig; the caller
// still has to certify the result.
QuoteSliceRepairOutcome check_channel_move(const QuoteSliceFit &fit,
                                           std::span<const AmericanPriceNode> before,
                                           std::span<const AmericanPriceNode> after,
                                           std::span<const QuoteSliceQuote> quotes,
                                           const QuoteSliceRepairConfig &budget,
                                           std::vector<std::uint8_t> &moved,
                                           QuoteSliceRepairRecord &record) {
  moved.assign(after.size(), 0);
  record.nodes = 0;
  record.material_nodes = 0;
  record.max_price_move = 0.0;
  record.max_vol_move = 0.0;
  record.max_unquoted_vol_move = 0.0;
  record.unidentified = 0;
  for (std::size_t i = 0; i < after.size(); ++i) {
    const double call = std::abs(after[i].call - before[i].call);
    const double put = std::abs(after[i].put - before[i].put);
    if (call > 0.0) moved[i] |= 1u;
    if (put > 0.0) moved[i] |= 2u;
    if (moved[i] == 0u) continue;
    ++record.nodes;
    if (std::max(call, put) > budget.immaterial_price_move) ++record.material_nodes;
    record.max_price_move = std::max({record.max_price_move, call, put});
  }
  if (record.nodes == 0) return QuoteSliceRepairOutcome::NoMove;
  if (record.material_nodes > budget.max_nodes) return QuoteSliceRepairOutcome::NodeBudget;
  const NodeChannels channel = node_channels(after, quotes);
  for (std::size_t i = 0; i < after.size(); ++i) {
    for (const Side side : {Side::Call, Side::Put}) {
      const bool call = side == Side::Call;
      if ((moved[i] & (call ? 1u : 2u)) == 0u) continue;
      const double lo = call ? channel.call_lo[i] : channel.put_lo[i];
      const double hi = call ? channel.call_hi[i] : channel.put_hi[i];
      const double old_price = call ? before[i].call : before[i].put;
      const double new_price = call ? after[i].call : after[i].put;
      const double slack = 1.0e-12 * std::max(1.0, std::abs(old_price));
      if (channel_distance(new_price, lo, hi) > channel_distance(old_price, lo, hi) + slack) {
        return QuoteSliceRepairOutcome::LeftChannel;
      }
    }
  }
  QuoteSliceConfig query;
  query.discrete_div_steps = fit.discrete_div_steps;
  for (std::size_t i = 0; i < after.size(); ++i) {
    if (moved[i] == 0u) continue;
    const double strike = after[i].strike;
    const double curve_vol = fit.curve->iv(std::log(strike / fit.diagnostics.forward));
    const double q = effective_yield(fit, calendar_yield_at(fit, strike));
    for (const Side side : {Side::Call, Side::Put}) {
      const bool call = side == Side::Call;
      if ((moved[i] & (call ? 1u : 2u)) == 0u) continue;
      const double price = call ? after[i].call : after[i].put;
      const QuoteSliceQuote at{strike, side, price, price, {}};
      const auto iv = contract_iv(fit, at, price, query, q);
      if (!iv || !positive(*iv) || !positive(curve_vol)) {
        ++record.unidentified;
        continue;
      }
      const double change = std::abs(*iv - curve_vol);
      record.max_vol_move = std::max(record.max_vol_move, change);
      if (!std::isfinite(call ? channel.call_hi[i] : channel.put_hi[i])) {
        record.max_unquoted_vol_move = std::max(record.max_unquoted_vol_move, change);
      }
    }
  }
  return record.max_unquoted_vol_move > budget.max_vol_move ? QuoteSliceRepairOutcome::VolBudget
                                                            : QuoteSliceRepairOutcome::Applied;
}

// Channel repair of a failed slice certificate, in two steps on the FITTED
// prices: (1) lower only the rejected nodes onto their envelope (exact, cheap;
// enough for isolated concave kinks), else (2) the full projection inside the
// channels (project_in_channel; also restores monotonicity and raises nodes).
// Each step must pass the budget and the unchanged certificate; `record` keeps
// the outcome of the last step that produced a candidate (Infeasible only when
// the lowering moved nothing and the projection has no candidate). On success
// replaces `prices`/`repaired` and returns the certified slice.
std::optional<AmericanPriceSlice> channel_repair_slice(
    const QuoteSliceFit &fit, std::span<const QuoteSliceQuote> quotes,
    std::vector<AmericanPriceNode> &prices, const AmericanPriceBounds &bounds,
    const QuoteSliceRepairConfig &budget, std::vector<std::uint8_t> &repaired,
    QuoteSliceRepairRecord &record) {
  for (int step = 0; step < 2; ++step) {
    std::vector<AmericanPriceNode> trial = prices;
    if (step == 0) {
      std::vector<std::uint8_t> lowered;
      lower_violators(trial, fit.market.spot, lowered);
    } else if (!project_in_channel(trial, quotes, bounds, nullptr)) {
      // No projected candidate: keep why the lowering was refused, unless it
      // did not move anything (then the slice simply has no feasible repair).
      if (record.outcome == QuoteSliceRepairOutcome::NoMove) {
        record.outcome = QuoteSliceRepairOutcome::Infeasible;
      }
      return std::nullopt;
    }
    std::vector<std::uint8_t> moved;
    record.outcome = check_channel_move(fit, prices, trial, quotes, budget, moved, record);
    if (record.outcome != QuoteSliceRepairOutcome::Applied) continue;
    auto certified = AmericanPriceSlice::create(trial, bounds);
    if (!certified) {
      record.outcome = QuoteSliceRepairOutcome::NotCertified;
      continue;
    }
    prices = std::move(trial);
    repaired = std::move(moved);
    return std::move(*certified);
  }
  return std::nullopt;
}

// Flags node side `bit` (1 call, 2 put) of node `node` among `nodes` as served
// price-only (QuoteSliceFit::price_only_nodes).
void mark_price_only(QuoteSliceFit &fit, std::size_t node, unsigned bit, std::size_t nodes) {
  if (fit.price_only_nodes.size() != nodes) fit.price_only_nodes.assign(nodes, 0);
  fit.price_only_nodes[node] = static_cast<std::uint8_t>(fit.price_only_nodes[node] | bit);
}

// Rung memo key of a certification candidate (rung_memo.hpp): the fit-level words of
// the American model (deam_iv_key of the prepared fit with no quotes and fixed
// inverter controls, so only the market, derived carry and clocks, dividend model and
// dated dividends, AlOpts and lattice controls vary), then per strike the strike, the
// fitted volatility and the contract yield, bit for bit. Also folds every yield into
// [q_min, q_max] exactly as the grid loop does. Empty when a volatility is not
// positive: the caller then runs the historical loop, which reports that refusal.
std::vector<std::uint64_t> certification_grid_key(const QuoteSliceFit &out,
                                                  std::span<const double> strikes,
                                                  double &q_min, double &q_max) {
  std::vector<std::uint64_t> key = deam_iv_key(out, {}, QuoteSliceConfig{}).words;
  key.reserve(key.size() + 3 * strikes.size());
  for (const double strike : strikes) {
    const double q = effective_yield(out, calendar_yield_at(out, strike));
    q_min = std::min(q_min, q);
    q_max = std::max(q_max, q);
    const double sigma = out.curve->iv(std::log(strike / out.diagnostics.forward));
    if (!positive(sigma)) return {};
    key.push_back(std::bit_cast<std::uint64_t>(strike));
    key.push_back(std::bit_cast<std::uint64_t>(sigma));
    key.push_back(std::bit_cast<std::uint64_t>(q));
  }
  return key;
}

// Stage 3: reprice every valid strike from out.curve, certify the final
// American prices and fill the rows. Sets fit_status/fit_error on refusal.
// A positive `repair_half_spreads` enables the bounded-move convexity repair;
// a Channel `channel_repair` then gets the remaining failures. Nodes either
// repair moved are flagged in out.moved_nodes. Under an installed rung memo
// (QuoteSurfaceConfig::rung_memo) an unchanged candidate reuses its memoised
// grid prices (bit-identical to a reprice) and certifies them as below.
void certify_and_serve(QuoteSliceFit &out, std::span<const QuoteSliceQuote> quotes,
                       double repair_half_spreads = 0.0,
                       const QuoteSliceRepairConfig *channel_repair = nullptr) {
  const counters::fit_timing::BucketScope price_timing(counters::fit_timing::Bucket::Price);
  counters::fit_timing::count(&counters::fit_timing::FitTimingCounters::certification_passes);
  const QuoteSliceMarket &market = out.market;
  auto &diag = out.diagnostics;
  // Certify the FINAL American prices. A no-arbitrage European IV curve is
  // insufficient after early exercise and especially after strike-varying
  // carry. Keep the serving function piecewise linear in PRICE so the checked
  // affine bounds and convexity hold between nodes as well as at the nodes.
  std::vector<double> strikes;
  strikes.reserve(quotes.size());
  for (const auto &quote : quotes) {
    if (positive(quote.strike) && valid_side(quote.side)) strikes.push_back(quote.strike);
  }
  std::sort(strikes.begin(), strikes.end());
  strikes.erase(std::unique(strikes.begin(), strikes.end()), strikes.end());
  std::vector<AmericanPriceNode> prices;
  prices.reserve(strikes.size());
  double q_min = diag.effective_yield;
  double q_max = diag.effective_yield;
  detail::ExpiryRungMemo *const memo = detail::current_rung_memo();
  std::vector<std::uint64_t> grid_key;
  if (memo != nullptr) grid_key = certification_grid_key(out, strikes, q_min, q_max);
  const std::vector<AmericanPriceNode> *const memoised =
      grid_key.empty() ? nullptr : memo->grid(grid_key);
  if (memoised != nullptr) prices = *memoised;
  for (std::size_t s = 0; memoised == nullptr && s < strikes.size(); ++s) {
    const double strike = strikes[s];
    const double q = effective_yield(out, calendar_yield_at(out, strike));
    q_min = std::min(q_min, q);
    q_max = std::max(q_max, q);
    const double sigma = out.curve->iv(std::log(strike / diag.forward));
    if (!positive(sigma)) {
      out.fit_status = QuoteSliceFitStatus::ArbitrageFailure;
      out.fit_error = "American price certification: invalid grid volatility";
      return;
    }
    const auto call = model_price(out, strike, Side::Call, sigma, q);
    const auto put = model_price(out, strike, Side::Put, sigma, q);
    if (!call || !put) {
      out.fit_status = QuoteSliceFitStatus::ArbitrageFailure;
      out.fit_error = "American price certification: grid repricing failed";
      return;
    }
    prices.push_back({strike, *call, *put});
  }
  if (memoised == nullptr && !grid_key.empty()) memo->remember_grid(std::move(grid_key), prices);
  const counters::fit_timing::BucketScope certify_timing(counters::fit_timing::Bucket::Certify);
  AmericanPriceBounds bounds{market.spot, std::min(1.0, diag.discount),
                             std::max(1.0, diag.discount), 0.0, 0.0};
  bounds.prepaid_max = market.spot * std::exp(std::max(-q_min, 0.0) * market.variance_years);
  bounds.prepaid_min = market.spot * std::exp(-std::max(q_max, 0.0) * market.variance_years);
  for (const auto &event : out.variance_clock_dividends) {
    bounds.prepaid_min -= event.amount * std::exp(-diag.effective_rate * event.tau +
        std::max(-q_min, 0.0) * (market.variance_years - event.tau));
  }
  bounds.prepaid_min = std::max(bounds.prepaid_min, 0.0);
  auto certified = AmericanPriceSlice::create(prices, bounds);
  const bool channel_mode =
      channel_repair != nullptr && channel_repair->mode == QuoteSliceRepairMode::Channel;
  // The channel repair starts from the fitted prices, never from a bounded-move
  // repair that moved them and still failed (only copied when it can run).
  std::vector<AmericanPriceNode> fitted;
  if (!certified && channel_mode) fitted = prices;
  std::vector<std::uint8_t> repaired;
  if (!certified && repair_half_spreads > 0.0 &&
      repair_convexity(prices, quotes, repair_half_spreads, diag.channel, repaired)) {
    certified = AmericanPriceSlice::create(prices, bounds);
  }
  std::optional<AmericanPriceSlice> channel_certified;
  if (!certified && channel_mode) {
    // A bounded attempt that moved nodes and still failed is discarded: the
    // served moves (if any) are the channel repair's, recorded in diag.repair.
    if (!repaired.empty()) diag.channel.repaired_nodes = 0;
    channel_certified =
        channel_repair_slice(out, quotes, fitted, bounds, *channel_repair, repaired, diag.repair);
    if (channel_certified) prices = std::move(fitted);
  }
  if (!certified && !channel_certified) {
    out.fit_status = QuoteSliceFitStatus::ArbitrageFailure;
    out.fit_error = certified.error().to_string();
    return;
  }
  AmericanPriceSlice &accepted = channel_certified ? *channel_certified : *certified;
  if (q_min == 0.0 && q_max == 0.0 && out.variance_clock_dividends.empty() &&
      diag.effective_rate >= 0.0) {
    const auto joint = check_nondividend_american(accepted, diag.discount);
    if (!joint) {
      if (channel_certified) diag.repair.outcome = QuoteSliceRepairOutcome::NotCertified;
      out.fit_status = QuoteSliceFitStatus::ArbitrageFailure;
      out.fit_error = joint.error().to_string();
      return;
    }
  }
  out.prices = std::move(accepted);
  out.fit_status = QuoteSliceFitStatus::Success;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &quote = quotes[i];
    auto &row = out.rows[i];
    if (row.evaluation_status == QuoteSliceEvaluationStatus::InvalidContract) {
      continue;
    }
    row.fitted_iv = out.curve->iv(std::log(quote.strike / diag.forward));
    if (!positive(row.fitted_iv)) {
      row.evaluation_status = QuoteSliceEvaluationStatus::InvalidVolatility;
      continue;
    }
    const auto price = out.prices->price(quote.strike, quote.side);
    if (!price || !nonnegative(*price)) {
      row.evaluation_status = QuoteSliceEvaluationStatus::PricingFailure;
      continue;
    }
    row.fitted_price = *price;
    row.evaluation_status = QuoteSliceEvaluationStatus::Success;
    ++diag.prices_produced;
    if (!repaired.empty()) {
      // A repaired node's served price is no longer the curve's: report the
      // volatility of the certified price, and never the curve's pre-repair
      // volatility when the served price has none (PriceOnly).
      const auto at = std::lower_bound(strikes.begin(), strikes.end(), quote.strike);
      const auto node = static_cast<std::size_t>(at - strikes.begin());
      const unsigned bit = quote.side == Side::Call ? 1u : 2u;
      if (at != strikes.end() && *at == quote.strike && (repaired[node] & bit) != 0u) {
        const double q = effective_yield(out, calendar_yield_at(out, quote.strike));
        QuoteSliceConfig query;
        query.discrete_div_steps = out.discrete_div_steps;
        const auto iv = contract_iv(out, quote, *price, query, q);
        if (iv && positive(*iv)) {
          row.fitted_iv = *iv;
        } else {
          row.fitted_iv = std::numeric_limits<double>::quiet_NaN();
          row.evaluation_status = QuoteSliceEvaluationStatus::PriceOnly;
          mark_price_only(out, node, bit, strikes.size());
        }
      }
    }
  }
  if (std::any_of(repaired.begin(), repaired.end(), [](std::uint8_t m) { return m != 0u; })) {
    out.moved_nodes = std::move(repaired);
  }
}

Status validate_channel(const QuoteSliceChannelConfig &c) {
  const auto finite_nonnegative = [](double x) { return std::isfinite(x) && x >= 0.0; };
  if (!positive(c.channel_weight) || !finite_nonnegative(c.mid_weight) ||
      !finite_nonnegative(c.itm_mid_weight) || !finite_nonnegative(c.default_shape_weight) ||
      !positive(c.prior_scale) || !finite_nonnegative(c.roughness) ||
      !positive(c.min_half_width_vol) || !positive(c.mult_floor) || !positive(c.mult_ceil) ||
      !(c.mult_ceil > c.mult_floor) || c.max_iterations == 0 || c.min_channel_quotes < 1 ||
      !finite_nonnegative(c.repair_half_spreads) || c.family_iterations == 0 ||
      (c.point_max_outside_fraction &&
       !(*c.point_max_outside_fraction >= 0.0 && *c.point_max_outside_fraction <= 1.0)) ||
      c.point_min_outside_quotes < 1 ||
      (c.fit_space != QuoteSliceFitSpace::PriceChannel &&
       c.fit_space != QuoteSliceFitSpace::VolChannel) ||
      (c.deam_profile != QuoteSliceDeamProfile::Accurate &&
       c.deam_profile != QuoteSliceDeamProfile::Fast) ||
      (c.deam_inversion != QuoteSliceDeamInversion::Search &&
       c.deam_inversion != QuoteSliceDeamInversion::Premium) ||
      (c.deam_inversion == QuoteSliceDeamInversion::Premium &&
       c.deam_profile != QuoteSliceDeamProfile::Accurate)) {
    return Err(ErrorCode::InvalidArgument, "quote slice: invalid channel configuration");
  }
  if (c.family) {
    switch (*c.family) {
    case VolCurveKind::ConvexDense:
    case VolCurveKind::Essvi:
    case VolCurveKind::Svi:
    case VolCurveKind::C8:
    case VolCurveKind::SplineVol:
      break;
    default: // LinearVariance interpolates its targets: no channel solve
      return Err(ErrorCode::InvalidArgument, "quote slice: unsupported channel family");
    }
  }
  return Ok();
}

ChannelFitConfig channel_fit_config(const QuoteSliceChannelConfig &c) {
  ChannelFitConfig out;
  out.channel_weight = c.channel_weight;
  out.mid_weight = c.mid_weight;
  out.default_shape_weight = c.default_shape_weight;
  out.prior_scale = c.prior_scale;
  out.roughness = c.roughness;
  out.min_half_width = c.min_half_width_vol;
  out.mult_floor = c.mult_floor;
  out.mult_ceil = c.mult_ceil;
  out.max_iterations = c.max_iterations;
  out.min_observations = c.min_channel_quotes;
  return out;
}

// One bid/ask volatility channel per valid contract (both sides; bidless rows
// are upper bounds). A bid at or below the minimum-volatility price carries no
// lower information; an ask at or above the maximum-volatility price carries no
// upper information. A channel entirely outside the model's price range is
// unusable and skipped. Admitted rows reuse their point-path mid IV.
std::vector<ChannelObservation> build_channel_observations(QuoteSliceFit &out,
                                                           std::span<const QuoteSliceQuote> quotes,
                                                           const QuoteSliceConfig &config) {
  const counters::fit_timing::BucketScope timing(counters::fit_timing::Bucket::Deam);
  const auto &diag = out.diagnostics;
  std::vector<ChannelObservation> channel;
  channel.reserve(quotes.size());
  std::vector<double> mid_strikes;
  mid_strikes.reserve(quotes.size());
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const QuoteSliceQuote &quote = quotes[i];
    if (!positive(quote.strike) || !valid_side(quote.side) || !nonnegative(quote.bid) ||
        !positive(quote.ask) || !(quote.ask > quote.bid)) {
      continue;
    }
    const double calendar_yield = quote.continuous_yield.value_or(out.market.continuous_yield);
    const double q_effective = effective_yield(out, calendar_yield);
    const double forward = local_forward(out, calendar_yield, q_effective);
    if (!positive(forward) || !std::isfinite(q_effective)) {
      continue;
    }
    const auto floor_price = model_price(out, quote.strike, quote.side, kIvMin, q_effective);
    const auto cap_price = model_price(out, quote.strike, quote.side, kIvMax, q_effective);
    if (!floor_price || !cap_price || !(quote.bid < *cap_price) || !(quote.ask > *floor_price)) {
      continue;
    }
    ChannelObservation obs;
    obs.k = std::log(quote.strike / diag.forward);
    if (quote.bid > *floor_price) {
      const auto lo = contract_iv(out, quote, quote.bid, config, q_effective);
      obs.sigma_lo = lo && positive(*lo) ? *lo : 0.0;
    }
    if (quote.ask < *cap_price) {
      const auto hi = contract_iv(out, quote, quote.ask, config, q_effective);
      if (hi && positive(*hi)) obs.sigma_hi = *hi;
    }
    const QuoteSliceRow &row = out.rows[i];
    if (row.admitted && positive(row.market_iv)) {
      obs.sigma_mid = row.market_iv;
    } else if (quote.bid > 0.0) {
      const double mid = 0.5 * quote.bid + 0.5 * quote.ask;
      if (mid > *floor_price && mid < *cap_price) {
        const auto iv = contract_iv(out, quote, mid, config, q_effective);
        if (iv && positive(*iv)) obs.sigma_mid = *iv;
      }
    }
    const double local_k = std::log(quote.strike / forward);
    const bool in_the_money = (local_k < 0.0 && quote.side == Side::Call) ||
                              (local_k > 0.0 && quote.side == Side::Put);
    obs.mid_scale = in_the_money ? config.channel.itm_mid_weight : 1.0;
    if (!(obs.sigma_hi > obs.sigma_lo) ||
        (!(obs.sigma_lo > 0.0) && !std::isfinite(obs.sigma_hi))) {
      continue;
    }
    if (std::isfinite(obs.sigma_mid)) mid_strikes.push_back(quote.strike);
    channel.push_back(obs);
  }
  std::sort(mid_strikes.begin(), mid_strikes.end());
  auto &channel_diag = out.diagnostics.channel;
  channel_diag.used = true;
  channel_diag.channel_quotes = channel.size();
  channel_diag.mid_quotes = mid_strikes.size();
  channel_diag.mid_strikes = static_cast<std::size_t>(
      std::unique(mid_strikes.begin(), mid_strikes.end()) - mid_strikes.begin());
  return channel;
}

// Pre-solve screen (QuoteSlicePreScreen, lane L2c): necessary conditions of a served
// channel fit read from its usable bid/ask bands alone. `channels` counts the quotes
// whose band is usable (non-empty, inside the model's identifiable price range, with
// at least one finite edge); under VolChannel they come from the de-Am IV cache, so the
// screen costs no American work beyond the one inversion pass. Exactly today's
// pre-solve refusal, typed: it can never refuse a slice the solve would serve.
QuoteSlicePreScreen prescreen_slice(std::size_t channels,
                                    const QuoteSliceChannelConfig &config) noexcept {
  if (channels == 0) return QuoteSlicePreScreen::EmptyBand;
  if (channels < config.min_channel_quotes) return QuoteSlicePreScreen::TooFewChannels;
  return QuoteSlicePreScreen::Pass;
}

// The native knot-spline solve of the channel objective, served as SplineVol.
Result<ChannelFitDiagnostics> solve_channel_native(QuoteSliceFit &out,
                                                   std::span<const ChannelObservation> channel,
                                                   const ChannelPrior *prior,
                                                   const QuoteSliceConfig &config) {
  ATX_TRY(ChannelFit fitted, fit_slice_channel(channel, out.market.variance_years, prior,
                                               channel_fit_config(config.channel)));
  out.curve = std::make_unique<SplineVolCurve>(fitted.params, out.market.variance_years,
                                               out.diagnostics.forward, out.diagnostics.discount);
  return Ok(fitted.diagnostics);
}

// The same objective solved by `channel.family` (fit_slice_channel_family). Each
// step's targets become European point observations on the reference forward
// (out-of-the-money side). Weights keep the objective's ratios in total
// variance, scaled so a unit mid anchor at the half-width floor and the
// reference ATM volatility carries the point path's weight cap (an active bound
// therefore weighs channel_weight times the cap). A family error is a fit
// failure of this expiry, never a configuration error.
Result<ChannelFitDiagnostics> solve_channel_family(QuoteSliceFit &out,
                                                   std::span<const ChannelObservation> channel,
                                                   const ChannelPrior *prior,
                                                   const QuoteSliceConfig &config) {
  const double years = out.market.variance_years;
  const double forward = out.diagnostics.forward;
  const double discount = out.diagnostics.discount;
  CurveConfig curve = config.curve;
  curve.kind = *config.channel.family;
  const double unit = config.channel.min_half_width_vol;
  std::vector<std::unique_ptr<IVolCurve>> curves;
  std::vector<FitObs> observations;
  const ChannelFamilyStep step = [&](double sigma_ref, std::span<const ChannelTarget> targets,
                                     std::span<const double> k_eval,
                                     std::span<double> sigma) -> Status {
    const double reference = 2.0 * sigma_ref * years;
    const double scale = config.quality.max_weight * unit * unit * reference * reference;
    observations.clear();
    observations.reserve(targets.size());
    for (std::size_t i = 0; i < targets.size(); ++i) {
      const ChannelTarget &t = targets[i];
      const double strike = forward * std::exp(t.k);
      const Side side = t.k < 0.0 ? Side::Put : Side::Call;
      const auto euro = black76_value_and_vega(forward, strike, years, t.sigma, discount, side);
      const double sensitivity = 2.0 * t.sigma * years;
      const double weight = scale * t.weight / (sensitivity * sensitivity);
      if (!positive(strike) || !positive(euro.vega) || !nonnegative(euro.price) ||
          !positive(weight)) {
        continue; // no price sensitivity at this target: it cannot inform the fit
      }
      FitObs obs;
      obs.k = t.k;
      obs.sigma_mkt = t.sigma;
      obs.score_sigma_mkt = t.sigma;
      obs.w_mkt = t.sigma * t.sigma * years;
      obs.weight_w = weight;
      obs.active_weight_w = weight;
      obs.K = strike;
      obs.F = forward;
      obs.df = discount;
      obs.mid = euro.price;
      obs.spread = t.unit * euro.vega;
      obs.vega = euro.vega;
      obs.noise_sigma = t.unit;
      obs.side = side;
      obs.source_strike_index = static_cast<std::uint32_t>(i);
      observations.push_back(obs);
    }
    auto fitted = fit_slice_curve(
        curve, observations, forward, years, discount, {}, {},
        {-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()});
    if (!fitted) {
      return Err(ErrorCode::Internal, std::string("channel family ") + to_string(curve.kind) +
                                          ": " + fitted.error().to_string());
    }
    for (std::size_t i = 0; i < k_eval.size(); ++i) {
      sigma[i] = (*fitted)->iv(k_eval[i]);
    }
    curves.push_back(std::move(*fitted));
    return Ok();
  };
  ATX_TRY(ChannelFamilyFit solved,
          fit_slice_channel_family(channel, years, prior, channel_fit_config(config.channel),
                                   config.channel.family_iterations, step));
  out.curve = std::move(curves[solved.step]);
  return Ok(solved.diagnostics);
}

} // namespace

Status validate_quote_slice_channel(const QuoteSliceChannelConfig &config) {
  return validate_channel(config);
}

Status validate_quote_slice_repair(const QuoteSliceRepairConfig &config) {
  if ((config.mode != QuoteSliceRepairMode::Off && config.mode != QuoteSliceRepairMode::Channel) ||
      config.max_nodes < 1 || !positive(config.max_vol_move) ||
      !nonnegative(config.immaterial_price_move)) {
    return Err(ErrorCode::InvalidArgument, "quote slice: invalid channel repair configuration");
  }
  return Ok();
}

Result<QuoteSliceFit> fit_quote_slice(const QuoteSliceMarket &market,
                                      std::span<const QuoteSliceQuote> quotes,
                                      const QuoteSliceConfig &config,
                                      std::span<const CashDividend> calendar_dividends) {
  if (config.carry.mode != QuoteSliceCarryMode::Supplied) {
    // Validate the caller's inputs before carry resolution: the implied-carry
    // path replaces every contract yield, so a non-finite supplied yield (or
    // any other malformed input) would otherwise never reach validate().
    const Status valid = validate(market, quotes, config, calendar_dividends);
    if (!valid) {
      return Err(valid.error());
    }
    return detail::fit_quote_slice_resolving_carry(market, quotes, config, calendar_dividends);
  }
  if (config.method == QuoteSliceFitMethod::ChannelSpline) {
    return fit_quote_slice_channel(market, quotes, config, nullptr, calendar_dividends);
  }
  QuoteSliceFit out;
  const Status prepared = prepare_fit(out, market, quotes, config, calendar_dividends);
  if (!prepared) {
    return Err(prepared.error());
  }
  // Opt-in vol-space channel with the chain memo: admission shares its mids.
  std::optional<VolSpaceCache> deam;
  if (config.channel.fit_space == QuoteSliceFitSpace::VolChannel && config.channel.deam_cache) {
    deam.emplace(out, quotes, config);
  }
  const std::vector<FitObs> observations =
      admit_point_observations(out, quotes, config, deam ? &deam->view() : nullptr);
  auto &diag = out.diagnostics;
  if (diag.quotes_admitted < config.min_quotes || diag.distinct_strikes < config.min_quotes) {
    out.fit_error = "too few distinct admitted strikes";
    return Ok(std::move(out));
  }
  const counters::fit_timing::BucketScope solve_timing(counters::fit_timing::Bucket::Solve);
  auto curve = fit_slice_curve(
      config.curve, observations, diag.forward, market.variance_years, diag.discount, {}, {},
      {-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()},
      &diag.curve_fit);
  if (!curve) {
    out.fit_status = QuoteSliceFitStatus::FitFailure;
    out.fit_error = curve.error().to_string();
    return Ok(std::move(out));
  }
  out.curve = std::move(*curve);
  certify_and_serve(out, quotes, 0.0, &config.repair);
  return Ok(std::move(out));
}

Result<QuoteSliceFit> fit_quote_slice_channel(const QuoteSliceMarket &market,
                                              std::span<const QuoteSliceQuote> quotes,
                                              const QuoteSliceConfig &config,
                                              const QuoteSlicePrior *prior,
                                              std::span<const CashDividend> calendar_dividends) {
  ATX_TRY_VOID(validate_channel(config.channel));
  if (config.carry.mode != QuoteSliceCarryMode::Supplied) {
    // This entry point has no carry-resolution seam yet: fit_quote_slice
    // resolves carry first (prior-free), the graded surface requires Supplied.
    return Err(ErrorCode::InvalidArgument,
               "quote slice: fit_quote_slice_channel requires supplied carry");
  }
  QuoteSliceFit out;
  const Status prepared = prepare_fit(out, market, quotes, config, calendar_dividends);
  if (!prepared) {
    return Err(prepared.error());
  }
  // Point admission still runs: it reports every row's quote status and
  // provides the admitted mid volatilities reused below. The opt-in vol-space
  // channel reads every quote volatility from the chain's de-Am IV cache.
  std::optional<VolSpaceCache> deam;
  if (config.channel.fit_space == QuoteSliceFitSpace::VolChannel) deam.emplace(out, quotes, config);
  static_cast<void>(
      admit_point_observations(out, quotes, config, deam ? &deam->view() : nullptr));
  const std::vector<ChannelObservation> channel = [&] {
    // The cache's band fills are this fit's de-Am work (fit_timing Deam bucket).
    const counters::fit_timing::BucketScope timing(counters::fit_timing::Bucket::Deam);
    return deam ? vol_space_channel_observations(out, quotes, deam->view(), config.channel)
                : build_channel_observations(out, quotes, config);
  }();
  auto &channel_diag = out.diagnostics.channel;
  channel_diag.prescreen = prescreen_slice(channel.size(), config.channel);
  if (channel_diag.prescreen != QuoteSlicePreScreen::Pass) {
    out.fit_error = "too few usable bid/ask channels";
    return Ok(std::move(out));
  }
  ChannelPrior applied;
  if (prior != nullptr) {
    applied.atm_vol = prior->atm_vol;
    applied.z = prior->z;
    applied.multiples = prior->multiples;
    applied.strength = prior->strength;
  }
  const ChannelPrior *used_prior = prior != nullptr ? &applied : nullptr;
  const counters::fit_timing::BucketScope solve_timing(counters::fit_timing::Bucket::Solve);
  counters::fit_timing::count(&counters::fit_timing::FitTimingCounters::channel_solves);
  const auto solve_start = std::chrono::steady_clock::now();
  const auto solved = config.channel.family
                          ? solve_channel_family(out, channel, used_prior, config)
                          : solve_channel_native(out, channel, used_prior, config);
  channel_diag.solve_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - solve_start).count();
  if (!solved) {
    if (solved.error().code() == ErrorCode::InvalidArgument) {
      return Err(solved.error());
    }
    out.fit_status = solved.error().code() == ErrorCode::Unavailable
                         ? QuoteSliceFitStatus::InsufficientQuotes
                         : QuoteSliceFitStatus::FitFailure;
    out.fit_error = solved.error().to_string();
    return Ok(std::move(out));
  }
  const ChannelFitDiagnostics &d = *solved;
  channel_diag.violations = d.violations;
  channel_diag.max_violation_half_widths = d.max_violation_half_widths;
  channel_diag.prior_strength = d.prior_strength;
  channel_diag.term_prior = d.used_term_prior;
  channel_diag.converged = d.converged;
  channel_diag.iterations = d.iterations;
  counters::fit_timing::count(&counters::fit_timing::FitTimingCounters::jacobian_evals,
                              counters::fit_timing::detail::saturate(d.iterations));
  certify_and_serve(out, quotes, config.channel.repair_half_spreads, &config.repair);
  return Ok(std::move(out));
}

namespace {

// Fills the calendar clamp of `nodes` against accepted slices: floors from
// `earlier`, caps from `later`, wherever a node lies inside their domains.
// False when a floor exceeds its cap beyond the certificate allowance.
bool calendar_clamp(const QuoteSliceFit &fit, std::span<const AmericanPriceNode> nodes,
                    std::span<const AmericanPriceSlice *const> earlier,
                    std::span<const AmericanPriceSlice *const> later, CalendarClamp &c) {
  c.floor_call.assign(nodes.size(), 0.0);
  c.floor_put.assign(nodes.size(), 0.0);
  c.cap_call.assign(nodes.size(), std::numeric_limits<double>::infinity());
  c.cap_put.assign(nodes.size(), std::numeric_limits<double>::infinity());
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const double k = nodes[i].strike;
    for (const AmericanPriceSlice *slice : earlier) {
      const auto call = slice->price(k, Side::Call);
      const auto put = slice->price(k, Side::Put);
      if (call) c.floor_call[i] = std::max(c.floor_call[i], *call);
      if (put) c.floor_put[i] = std::max(c.floor_put[i], *put);
    }
    for (const AmericanPriceSlice *slice : later) {
      const auto call = slice->price(k, Side::Call);
      const auto put = slice->price(k, Side::Put);
      if (call) c.cap_call[i] = std::min(c.cap_call[i], *call);
      if (put) c.cap_put[i] = std::min(c.cap_put[i], *put);
    }
    // Accepted slices agree only to the certificate's fixed allowance.
    const double tol = 1.0e-8 + 1.0e-9 * std::max(fit.market.spot, nodes[i].strike);
    if (c.floor_call[i] > c.cap_call[i] + tol || c.floor_put[i] > c.cap_put[i] + tol) return false;
    c.cap_call[i] = std::max(c.cap_call[i], c.floor_call[i]);
    c.cap_put[i] = std::max(c.cap_put[i], c.floor_put[i]);
  }
  return true;
}

void clamp_nodes(std::vector<AmericanPriceNode> &nodes, const CalendarClamp &c) {
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    nodes[i].call = std::clamp(nodes[i].call, c.floor_call[i], c.cap_call[i]);
    nodes[i].put = std::clamp(nodes[i].put, c.floor_put[i], c.cap_put[i]);
  }
}

bool above_floors(const QuoteSliceFit &fit, std::span<const AmericanPriceNode> nodes,
                  const CalendarClamp &c) {
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const double tol = 1.0e-8 + 1.0e-9 * std::max(fit.market.spot, nodes[i].strike);
    if (!(nodes[i].call >= c.floor_call[i] - tol && nodes[i].put >= c.floor_put[i] - tol)) {
      return false;
    }
  }
  return true;
}

// Publishes a certified calendar repair: replaces fit.prices, refreshes the
// prices of the rows carrying one (Success and PriceOnly), re-implies a moved
// node of a Success row (no identifiable volatility -> PriceOnly with the
// certified price and NaN volatility, flagged in fit.price_only_nodes) and ORs
// `moved` into fit.moved_nodes. A PriceOnly row is never re-implied: it stays
// PriceOnly, so a price-only outcome can never become a volatility row.
void commit_calendar_repair(QuoteSliceFit &fit, std::span<const QuoteSliceQuote> quotes,
                            AmericanPriceSlice certified,
                            std::span<const AmericanPriceNode> nodes,
                            const std::vector<std::uint8_t> &moved) {
  fit.prices = std::move(certified);
  std::vector<double> strikes;
  strikes.reserve(nodes.size());
  for (const auto &node : nodes) strikes.push_back(node.strike);
  auto &produced = fit.diagnostics.prices_produced;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    auto &row = fit.rows[i];
    if (row.evaluation_status != QuoteSliceEvaluationStatus::Success &&
        row.evaluation_status != QuoteSliceEvaluationStatus::PriceOnly) {
      continue;
    }
    const QuoteSliceQuote &quote = quotes[i];
    const auto price = fit.prices->price(quote.strike, quote.side);
    if (!price || !nonnegative(*price)) {
      row.evaluation_status = QuoteSliceEvaluationStatus::PricingFailure;
      row.fitted_price = std::numeric_limits<double>::quiet_NaN();
      row.fitted_iv = std::numeric_limits<double>::quiet_NaN();
      if (produced > 0) --produced; // the row no longer carries a price
      continue;
    }
    row.fitted_price = *price;
    if (row.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly) continue; // final
    const auto at = std::lower_bound(strikes.begin(), strikes.end(), quote.strike);
    const auto node = static_cast<std::size_t>(at - strikes.begin());
    const unsigned bit = quote.side == Side::Call ? 1u : 2u;
    if (at != strikes.end() && *at == quote.strike && (moved[node] & bit) != 0u) {
      const double q = effective_yield(fit, calendar_yield_at(fit, quote.strike));
      QuoteSliceConfig query;
      query.discrete_div_steps = fit.discrete_div_steps;
      const auto iv = contract_iv(fit, quote, *price, query, q);
      if (iv && positive(*iv)) {
        row.fitted_iv = *iv;
      } else {
        // The served price has no identifiable volatility: never pair it with
        // the curve's pre-repair volatility.
        row.fitted_iv = std::numeric_limits<double>::quiet_NaN();
        row.evaluation_status = QuoteSliceEvaluationStatus::PriceOnly;
        mark_price_only(fit, node, bit, nodes.size());
      }
    }
  }
  // Union with any earlier (convexity) repair: both leave nodes off the curve.
  if (std::any_of(moved.begin(), moved.end(), [](std::uint8_t m) { return m != 0u; })) {
    if (fit.moved_nodes.size() != moved.size()) fit.moved_nodes.assign(moved.size(), 0);
    for (std::size_t i = 0; i < moved.size(); ++i) fit.moved_nodes[i] |= moved[i];
  }
}

} // namespace

bool repair_quote_slice_calendar(QuoteSliceFit &fit, std::span<const QuoteSliceQuote> quotes,
                                 std::span<const AmericanPriceSlice *const> earlier,
                                 std::span<const AmericanPriceSlice *const> later,
                                 double half_spreads, double *required) {
  const counters::fit_timing::BucketScope timing(counters::fit_timing::Bucket::Calendar);
  if (required != nullptr) *required = 0.0;
  if (!fit.prices || !fit.curve || !(half_spreads > 0.0) || !std::isfinite(half_spreads) ||
      fit.rows.size() != quotes.size()) {
    return false;
  }
  const auto original = fit.prices->nodes();
  std::vector<AmericanPriceNode> nodes(original.begin(), original.end());
  CalendarClamp clamp;
  if (!calendar_clamp(fit, nodes, earlier, later, clamp)) return false;
  // Alternate the calendar clamp and the convex minorant (which can undercut a
  // floor where earlier domains end); at most three rounds, then verify.
  std::vector<double> call_target;
  std::vector<double> put_target;
  bool within = false;
  for (int round = 0; round < 3 && !within; ++round) {
    clamp_nodes(nodes, clamp);
    lower_hull(nodes, Side::Call, call_target);
    lower_hull(nodes, Side::Put, put_target);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      nodes[i].call = call_target[i];
      nodes[i].put = put_target[i];
    }
    within = above_floors(fit, nodes, clamp);
  }
  std::vector<double> call_spread;
  std::vector<double> put_spread;
  node_half_spreads(nodes, quotes, call_spread, put_spread);
  std::vector<std::uint8_t> moved(nodes.size(), 0);
  double worst = 0.0;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    for (const Side side : {Side::Call, Side::Put}) {
      const bool call = side == Side::Call;
      const double shift = std::abs(call ? nodes[i].call - original[i].call
                                         : nodes[i].put - original[i].put);
      if (!(shift > 0.0)) continue;
      moved[i] |= call ? 1u : 2u;
      const double spread = call ? call_spread[i] : put_spread[i];
      const double ratio =
          spread > 0.0 ? shift / spread : std::numeric_limits<double>::infinity();
      worst = std::max(worst, ratio);
    }
  }
  if (required != nullptr) *required = worst;
  if (!within || worst > half_spreads) return false;
  auto certified = AmericanPriceSlice::create(nodes, fit.prices->bounds());
  if (!certified) return false;
  commit_calendar_repair(fit, quotes, std::move(*certified), nodes, moved);
  return true;
}

bool repair_quote_slice_calendar_in_channel(QuoteSliceFit &fit,
                                            std::span<const QuoteSliceQuote> quotes,
                                            std::span<const AmericanPriceSlice *const> earlier,
                                            std::span<const AmericanPriceSlice *const> later,
                                            const QuoteSliceRepairConfig &budget,
                                            QuoteSliceRepairRecord *record) {
  const counters::fit_timing::BucketScope timing(counters::fit_timing::Bucket::Calendar);
  QuoteSliceRepairRecord local;
  QuoteSliceRepairRecord &out = record != nullptr ? *record : local;
  out = QuoteSliceRepairRecord{};
  if (!fit.prices || !fit.curve || fit.rows.size() != quotes.size() || budget.max_nodes < 1 ||
      !positive(budget.max_vol_move) || !nonnegative(budget.immaterial_price_move)) {
    return false;
  }
  const auto original = fit.prices->nodes();
  std::vector<AmericanPriceNode> nodes(original.begin(), original.end());
  CalendarClamp clamp;
  if (!calendar_clamp(fit, nodes, earlier, later, clamp) ||
      !project_in_channel(nodes, quotes, fit.prices->bounds(), &clamp)) {
    out.outcome = QuoteSliceRepairOutcome::Infeasible;
    return false;
  }
  // The projection honours the floors exactly; the residual lowering may not.
  const bool within = above_floors(fit, nodes, clamp);
  std::vector<std::uint8_t> moved;
  out.outcome = check_channel_move(fit, original, nodes, quotes, budget, moved, out);
  if (out.outcome != QuoteSliceRepairOutcome::Applied) return false;
  auto certified = AmericanPriceSlice::create(nodes, fit.prices->bounds());
  // The clamp acts at this slice's nodes only: a neighbour's knot between two
  // of them can still cross, so the full calendar check runs on the result.
  const auto calendar_clean = [&](const AmericanPriceSlice &repaired) {
    for (const AmericanPriceSlice *slice : earlier) {
      if (!check_american_calendar(*slice, repaired)) return false;
    }
    for (const AmericanPriceSlice *slice : later) {
      if (!check_american_calendar(repaired, *slice)) return false;
    }
    return true;
  };
  if (!within || !certified || !calendar_clean(*certified)) {
    out.outcome = QuoteSliceRepairOutcome::NotCertified;
    return false;
  }
  commit_calendar_repair(fit, quotes, std::move(*certified), nodes, moved);
  return true;
}

const char *to_string(QuoteSliceGrade value) noexcept {
  switch (value) {
  case QuoteSliceGrade::A:
    return "A";
  case QuoteSliceGrade::B:
    return "B";
  case QuoteSliceGrade::C:
    return "C";
  case QuoteSliceGrade::Refused:
    return "refused";
  }
  return "unknown";
}

const char *to_string(QuoteSlicePreScreen value) noexcept {
  switch (value) {
  case QuoteSlicePreScreen::Pass:
    return "pass";
  case QuoteSlicePreScreen::EmptyBand:
    return "empty_band";
  case QuoteSlicePreScreen::TooFewChannels:
    return "too_few_channels";
  }
  return "unknown";
}

} // namespace atx::vol
