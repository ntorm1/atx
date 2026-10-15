#include "atx/vol/api/fitting/quote_slice.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "atx/vol/api/pricing/american_iv.hpp"
#include "atx/vol/api/pricing/black76.hpp"

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
      !std::isfinite(m.rate) || !std::isfinite(m.continuous_yield) || !nonnegative(m.dividend_pv) ||
      !(m.dividend_pv < m.spot)) {
    return Err(ErrorCode::InvalidArgument, "quote slice: invalid market or clocks");
  }
  if (quotes.size() > std::numeric_limits<std::uint32_t>::max() || cfg.min_quotes < 2 ||
      !positive(cfg.spread_floor) || !nonnegative(cfg.min_time_value) ||
      !positive(cfg.iv_tolerance) || cfg.max_iv_iterations == 0 || !nonnegative(cfg.atm_log_band) ||
      cfg.discrete_div_steps < 1 || cfg.discrete_div_steps > kDiscreteDivMaxSteps ||
      (cfg.side_policy != QuoteSliceSidePolicy::All &&
       cfg.side_policy != QuoteSliceSidePolicy::OutOfTheMoney)) {
    return Err(ErrorCode::InvalidArgument, "quote slice: invalid fitting configuration");
  }
  switch (cfg.dividend_model) {
  case QuoteSliceDividendModel::ContinuousYield:
    if (m.dividend_pv != 0.0 || !dividends.empty()) {
      return Err(ErrorCode::InvalidArgument, "continuous-yield model cannot ignore cash dividends");
    }
    break;
  case QuoteSliceDividendModel::AggregatePresentValue:
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
  return Ok();
}

Result<double> model_price(const QuoteSliceFit &fit, double strike, Side side, double sigma) {
  const auto &m = fit.market;
  const auto &d = fit.diagnostics;
  if (fit.dividend_model == QuoteSliceDividendModel::DatedCash) {
    return american_discrete_div_price(m.spot, strike, m.variance_years, sigma, d.effective_rate,
                                       d.effective_yield, side, fit.variance_clock_dividends,
                                       fit.discrete_div_steps);
  }
  return andersen_lake(m.spot, strike, m.variance_years, sigma, d.effective_rate, d.effective_yield,
                       side, fit.al_opts);
}

// The dated-cash engine has a different forward map from continuous-carry ALO.
// Use that SAME map throughout the bracket; never substitute a continuous IV.
// The CRR probability imposes sigma > |r-q|*sqrt(dt), so the lower endpoint
// must honor the lattice domain instead of treating its error as a zero price.
Result<double> dated_cash_iv(const QuoteSliceFit &fit, const QuoteSliceQuote &quote, double target,
                             const QuoteSliceConfig &cfg) {
  const auto &d = fit.diagnostics;
  const double min_lattice_sigma =
      std::abs(d.effective_rate - d.effective_yield) *
      std::sqrt(fit.market.variance_years / static_cast<double>(fit.discrete_div_steps));
  double lo = std::max(kIvMin, min_lattice_sigma * (1.0 + 1.0e-6));
  if (!(lo < kIvMax)) {
    return Err(ErrorCode::OutOfRange, "dated-cash IV: lattice has no admissible volatility range");
  }
  const auto lower = model_price(fit, quote.strike, quote.side, lo);
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
    const auto upper = model_price(fit, quote.strike, quote.side, hi);
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
    const auto value = model_price(fit, quote.strike, quote.side, sigma);
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
  }
  return "unknown";
}

Result<QuoteSliceValue> evaluate_quote_slice(const QuoteSliceFit &fit, double strike, Side side) {
  if (!positive(strike) || !valid_side(side)) {
    return Err(ErrorCode::InvalidArgument, "quote slice: invalid query contract");
  }
  if (fit.fit_status != QuoteSliceFitStatus::Success || !fit.curve) {
    return Err(ErrorCode::Unavailable, "quote slice: no fitted curve");
  }
  const double sigma = fit.curve->iv(std::log(strike / fit.diagnostics.forward));
  if (!positive(sigma)) {
    return Err(ErrorCode::OutOfRange, "quote slice: curve returned invalid volatility");
  }
  const auto price = model_price(fit, strike, side, sigma);
  if (!price) {
    return Err(price.error());
  }
  if (!nonnegative(*price)) {
    return Err(ErrorCode::OutOfRange, "quote slice: model returned invalid price");
  }
  return Ok(QuoteSliceValue{sigma, *price});
}

Result<QuoteSliceFit> fit_quote_slice(const QuoteSliceMarket &market,
                                      std::span<const QuoteSliceQuote> quotes,
                                      const QuoteSliceConfig &config,
                                      std::span<const CashDividend> calendar_dividends) {
  const Status valid = validate(market, quotes, config, calendar_dividends);
  if (!valid) {
    return Err(valid.error());
  }

  QuoteSliceFit out;
  out.market = market;
  out.dividend_model = config.dividend_model;
  out.al_opts = config.al_opts;
  out.discrete_div_steps = config.discrete_div_steps;
  auto &diag = out.diagnostics;
  diag.quotes_seen = quotes.size();
  const double ratio = market.calendar_years / market.variance_years;
  diag.effective_rate = market.rate * ratio;
  diag.effective_yield = market.continuous_yield * ratio;
  diag.discount = std::exp(-market.rate * market.calendar_years);
  const double growth = std::exp((market.rate - market.continuous_yield) * market.calendar_years);
  diag.forward = market.spot * growth;
  if (config.dividend_model == QuoteSliceDividendModel::AggregatePresentValue) {
    diag.effective_yield -= std::log1p(-market.dividend_pv / market.spot) / market.variance_years;
    diag.forward = (market.spot - market.dividend_pv) * growth;
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

  out.rows.resize(quotes.size());
  std::vector<FitObs> observations;
  observations.reserve(quotes.size());
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
    const double k = std::log(quote.strike / diag.forward);
    if (config.side_policy == QuoteSliceSidePolicy::OutOfTheMoney &&
        std::abs(k) > config.atm_log_band &&
        ((k < 0.0 && quote.side == Side::Call) || (k > 0.0 && quote.side == Side::Put))) {
      row.quote_status = QuoteSliceQuoteStatus::ExcludedSide;
      continue;
    }
    const double mid = 0.5 * quote.bid + 0.5 * quote.ask;
    const double intrinsic = std::max(
        quote.side == Side::Call ? market.spot - quote.strike : quote.strike - market.spot, 0.0);
    if (!(mid - intrinsic > config.min_time_value)) {
      row.quote_status = QuoteSliceQuoteStatus::UnidentifiableVolatility;
      continue;
    }
    ++diag.iv_attempted;
    const auto iv =
        config.dividend_model == QuoteSliceDividendModel::DatedCash
            ? dated_cash_iv(out, quote, mid, config)
            : american_implied_vol(mid, market.spot, quote.strike, market.variance_years,
                                   diag.effective_rate, diag.effective_yield, quote.side,
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
    const double spread = std::max(quote.ask - quote.bid, config.spread_floor);
    const double weight_root = euro.vega / spread / (2.0 * *iv * market.variance_years);
    const double weight = std::min(weight_root * weight_root, 1.0e24);
    const double variance = *iv * *iv * market.variance_years;
    if (!positive(weight) || !positive(euro.vega) || !nonnegative(euro.price) ||
        !positive(variance) || !std::isfinite(k)) {
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
    obs.spread = spread;
    obs.vega = euro.vega;
    obs.noise_sigma = spread / euro.vega;
    obs.side = quote.side;
    obs.source_strike_index = static_cast<std::uint32_t>(i);
    observations.push_back(obs);
    row.quote_status = QuoteSliceQuoteStatus::Admitted;
    row.admitted = true;
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
  if (diag.quotes_admitted < config.min_quotes || diag.distinct_strikes < config.min_quotes) {
    out.fit_error = "too few distinct admitted strikes";
    return Ok(std::move(out));
  }
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
    const auto price = model_price(out, quote.strike, quote.side, row.fitted_iv);
    if (!price || !nonnegative(*price)) {
      row.evaluation_status = QuoteSliceEvaluationStatus::PricingFailure;
      continue;
    }
    row.fitted_price = *price;
    row.evaluation_status = QuoteSliceEvaluationStatus::Success;
    ++diag.prices_produced;
  }
  return Ok(std::move(out));
}

} // namespace atx::vol
