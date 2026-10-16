#include "atx/vol/api/fitting/american_markov_seed.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>
#include <utility>

#include "atx/vol/api/pricing/american_iv.hpp"

namespace atx::vol {
namespace {
using atx::core::Err;
using atx::core::Ok;
using QuoteStatus = AmericanMarkovSeedQuoteStatus;
using ParameterStatus = AmericanMarkovSeedParameterStatus;
using Band = AmericanMarkovSeedVarianceBand;

bool positive(double x) noexcept { return std::isfinite(x) && x > 0.0; }
bool nonnegative(double x) noexcept { return std::isfinite(x) && x >= 0.0; }

bool quadrature(std::uint16_t n) noexcept {
  return n == 8 || n == 16 || n == 24 || n == 32 || n == 48 || n == 64;
}

Status validate(const AmericanMarkovFitMarket &m, std::span<const AmericanMarkovFitQuote> quotes,
                const AmericanMarkovSeedLayout &l, const AmericanMarkovSeedConfig &c) {
  if (l.time_shape == AmericanMarkovFitTimeShape::VarianceTimeKnots) {
    return Err(ErrorCode::Unavailable, "Markov seeds: variance-time-knot layout is unsupported");
  }
  if (!positive(m.spot) || c.max_input_quotes == 0 ||
      c.max_input_quotes > kAmericanMarkovFitMaxQuotes || quotes.size() > c.max_input_quotes ||
      c.max_expiries == 0 || c.max_expiries > kAmericanMarkovSeedMaxExpiries ||
      m.intervals.empty() || m.intervals.size() > c.max_expiries ||
      c.candidates_per_expiry == 0 || c.candidates_per_expiry > 32 || c.max_inversions < 3 ||
      c.max_inversions > kAmericanMarkovSeedMaxInversions ||
      c.max_root_iteration_slots == 0 ||
      c.max_root_iteration_slots > kAmericanMarkovSeedMaxIterationSlots ||
      c.max_iv_iterations == 0 || c.max_iv_iterations > 128 ||
      c.max_root_iteration_slots < 3U * c.max_iv_iterations ||
      l.max_parameters == 0 || l.max_parameters > kAmericanMarkovFitMaxParameters) {
    return Err(ErrorCode::OutOfRange, "Markov seeds: input or work capacity exceeded");
  }
  if (!positive(l.min_sigma) || !positive(l.max_sigma) || l.min_sigma >= l.max_sigma ||
      l.min_sigma < 1.0e-6 || l.max_sigma > 100.0 ||
      (l.time_shape != AmericanMarkovFitTimeShape::Shared &&
       l.time_shape != AmericanMarkovFitTimeShape::PerVarianceInterval) ||
      !nonnegative(c.max_abs_log_forward_moneyness) || c.max_abs_log_forward_moneyness > 64 ||
      !nonnegative(c.max_relative_spread) || c.max_relative_spread > 2 ||
      !nonnegative(c.min_time_value_over_spot) || c.min_time_value_over_spot > 1 ||
      !positive(c.iv_tolerance) || c.iv_tolerance < 1.0e-12 || c.iv_tolerance > 1.0e-3) {
    return Err(ErrorCode::InvalidArgument, "Markov seeds: invalid layout or selection controls");
  }
  const auto &a = c.inversion_options;
  if (a.n_collocation < 6 || a.n_collocation > 32 || !quadrature(a.n_quadrature) ||
      !quadrature(a.n_quad_price) || a.max_newton_iter < 2 || a.max_newton_iter > 16 ||
      !positive(a.tol) || a.tol < 1.0e-14 || a.tol > 1.0e-3) {
    return Err(ErrorCode::InvalidArgument, "Markov seeds: invalid bounded ALO options");
  }
  if (l.log_state_knots.size() > l.max_parameters) {
    return Err(ErrorCode::OutOfRange, "Markov seeds: knot count exceeds parameter capacity");
  }
  double previous = -std::numeric_limits<double>::infinity();
  for (const auto knot : l.log_state_knots) {
    if (!std::isfinite(knot) || std::abs(knot) > kAmericanMarkovMaxLocalVolLogState ||
        knot <= previous) {
      return Err(ErrorCode::InvalidArgument, "Markov seeds: invalid state knots");
    }
    previous = knot;
  }
  return Ok();
}

Result<bool> prepare_schedule(const AmericanMarkovFitMarket &m,
                              const AmericanMarkovSeedLayout &layout,
                              AmericanMarkovQuoteSeeds &out) {
  double calendar = 0.0;
  double variance = 0.0;
  double rate = 0.0;
  double yield = 0.0;
  bool cash = false;
  out.expiries.reserve(m.intervals.size());
  for (std::size_t i = 0; i < m.intervals.size(); ++i) {
    const auto &in = m.intervals[i];
    if (!positive(in.calendar_end) || in.calendar_end <= calendar ||
        !nonnegative(in.variance_end) || in.variance_end < variance ||
        !std::isfinite(in.rate) || !std::isfinite(in.continuous_yield) ||
        !nonnegative(in.cash_dividend_at_end)) {
      return Err(ErrorCode::InvalidArgument, "Markov seeds: invalid common schedule");
    }
    const double dt = in.calendar_end - calendar;
    rate = std::fma(in.rate, dt, rate);
    yield = std::fma(in.continuous_yield, dt, yield);
    if (!std::isfinite(rate) || !std::isfinite(yield) ||
        (in.variance_end > 0 && (!std::isfinite(rate / in.variance_end) ||
                              !std::isfinite(yield / in.variance_end)))) {
      return Err(ErrorCode::OutOfRange, "Markov seeds: nonfinite cumulative carry");
    }
    AmericanMarkovSeedExpiryDiagnostics expiry;
    expiry.calendar_end = in.calendar_end;
    expiry.variance_end = in.variance_end;
    expiry.integrated_rate = rate;
    expiry.integrated_yield = yield;
    out.expiries.push_back(expiry);
    if (in.variance_end > variance) {
      AmericanMarkovSeedEndpointDiagnostics endpoint;
      endpoint.variance_end = in.variance_end;
      out.variance_endpoints.push_back(std::move(endpoint));
    }
    if (in.variance_end > 0) out.variance_endpoints.back().expiry_indices.push_back(i);
    cash = cash || in.cash_dividend_at_end != 0.0;
    calendar = in.calendar_end;
    variance = in.variance_end;
  }
  out.log_state_knots.assign(layout.log_state_knots.begin(), layout.log_state_knots.end());
  if (out.log_state_knots.empty()) out.log_state_knots.push_back(0.0);
  out.time_groups = layout.time_shape == AmericanMarkovFitTimeShape::Shared
      ? (out.variance_endpoints.empty() ? 0U : 1U) : out.variance_endpoints.size();
  if (out.time_groups == 0 ||
      out.time_groups > layout.max_parameters / out.log_state_knots.size()) {
    return Err(ErrorCode::OutOfRange, "Markov seeds: unidentifiable variance or parameter limit");
  }
  out.parameters.resize(out.time_groups * out.log_state_knots.size());
  return Ok(cash);
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  const auto middle = values.size() / 2U;
  return values.size() % 2U == 0U ? std::midpoint(values[middle - 1U], values[middle])
                                  : values[middle];
}

Band median_band(std::span<const Band> bands) {
  std::vector<double> lower, mid, upper;
  for (const auto &band : bands) {
    lower.push_back(band.lower);
    mid.push_back(band.midpoint);
    upper.push_back(band.upper);
  }
  return {median(std::move(lower)), median(std::move(mid)), median(std::move(upper))};
}

double mad(std::span<const Band> bands, double center) {
  std::vector<double> deviations;
  for (const auto &band : bands) deviations.push_back(std::abs(band.midpoint - center));
  return median(std::move(deviations));
}

double relative_spread(const AmericanMarkovFitQuote &quote) {
  return (quote.ask - quote.bid) / std::midpoint(quote.bid, quote.ask);
}

double moneyness(const AmericanMarkovFitQuote &quote, double spot,
                  const AmericanMarkovSeedExpiryDiagnostics &expiry) {
  return std::abs(std::log(quote.strike) - std::log(spot) - expiry.integrated_rate +
                  expiry.integrated_yield);
}

QuoteStatus classify(const AmericanMarkovFitQuote &quote, double spot,
                       const AmericanMarkovSeedExpiryDiagnostics &expiry,
                       const AmericanMarkovSeedConfig &c) {
  if (expiry.variance_end <= 0.0) return QuoteStatus::ZeroVariance;
  if (quote.bid == 0.0) return QuoteStatus::Bidless;
  if (relative_spread(quote) > c.max_relative_spread) return QuoteStatus::WideSpread;
  if (moneyness(quote, spot, expiry) > c.max_abs_log_forward_moneyness) {
    return QuoteStatus::FarMoneyness;
  }
  const double intrinsic = std::max(quote.side == Side::Call ? spot - quote.strike
                                                           : quote.strike - spot, 0.0);
  const double floor = std::max(c.min_time_value_over_spot * spot,
      64 * std::numeric_limits<double>::epsilon() * std::max(spot, quote.strike));
  if (quote.bid - intrinsic <= floor) return QuoteStatus::IntrinsicPlateau;
  return QuoteStatus::InversionBudget; // Eligible, pending deterministic selection.
}

std::vector<std::vector<std::size_t>> candidates(
    const AmericanMarkovFitMarket &m, std::span<const AmericanMarkovFitQuote> quotes,
    AmericanMarkovQuoteSeeds &out) {
  const auto &c = out.configuration;
  std::vector<std::vector<std::size_t>> selected(m.intervals.size());
  std::map<std::tuple<std::size_t, Side, double>, std::vector<std::size_t>> contracts;
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    const auto &q = quotes[i];
    if (q.expiry_index >= m.intervals.size() || !positive(q.strike) ||
        (q.side != Side::Call && q.side != Side::Put) || !nonnegative(q.bid) ||
        !positive(q.ask) || q.bid > q.ask || !positive(q.weight) || q.weight > 1.0e6) continue;
    contracts[{q.expiry_index, q.side, q.strike}].push_back(i);
  }
  for (const auto &[key, indices] : contracts) {
    static_cast<void>(key);
    if (indices.size() > 1U) {
      for (const auto i : indices) out.quotes[i].status = QuoteStatus::DuplicateContract;
      continue;
    }
    const auto i = indices.front();
    const auto &q = quotes[i];
    auto &diagnostic = out.quotes[i];
    diagnostic.status = classify(q, m.spot, out.expiries[q.expiry_index], c);
    if (diagnostic.status != QuoteStatus::InversionBudget) continue;
    diagnostic.eligible = true;
    ++out.eligible_quotes;
    ++out.expiries[q.expiry_index].eligible_quotes;
    selected[q.expiry_index].push_back(i);
  }
  for (std::size_t expiry = 0; expiry < selected.size(); ++expiry) {
    auto &indices = selected[expiry];
    const auto rank = [&](std::size_t i) {
      const auto &q = quotes[i];
      return std::tuple{moneyness(q, m.spot, out.expiries[expiry]), relative_spread(q),
                        q.strike, q.side};
    };
    std::sort(indices.begin(), indices.end(), [&](auto a, auto b) { return rank(a) < rank(b); });
    for (std::size_t j = c.candidates_per_expiry; j < indices.size(); ++j) {
      out.quotes[indices[j]].status = QuoteStatus::CandidateLimit;
    }
    if (indices.size() > c.candidates_per_expiry) indices.resize(c.candidates_per_expiry);
  }
  return selected;
}

void invert(const AmericanMarkovFitQuote &q, double spot, const AmericanMarkovSeedLayout &layout,
              AmericanMarkovSeedQuoteDiagnostics &d, AmericanMarkovQuoteSeeds &out) {
  const auto &c = out.configuration;
  const auto &e = out.expiries[q.expiry_index];
  const std::array prices{q.bid, std::midpoint(q.bid, q.ask), q.ask};
  std::array<double, 3> vols{};
  for (std::size_t i = 0; i < prices.size(); ++i) {
    ++d.inversion_calls;
    ++out.inversion_calls;
    out.reserved_root_iteration_slots += c.max_iv_iterations;
    const auto result = american_implied_vol(prices[i], spot, q.strike, e.variance_end,
        e.integrated_rate / e.variance_end, e.integrated_yield / e.variance_end, q.side,
        AmericanMethod::AndersenLake, c.iv_tolerance, c.max_iv_iterations, c.inversion_options);
    if (!result) {
      d.status = QuoteStatus::InversionFailure;
      d.inversion_error = result.error().to_string();
      return;
    }
    d.equivalent_vols[i] = *result;
    if (!std::isfinite(*result) || *result <= kIvMin + c.iv_tolerance) {
      d.status = QuoteStatus::IvFloor;
      return;
    }
    if (*result < layout.min_sigma || *result > layout.max_sigma) {
      d.status = QuoteStatus::SigmaBounds;
      return;
    }
    vols[i] = *result;
  }
  // Preserve estimates and refuse numerical interval crossings, never reorder.
  if (vols[0] > vols[1] || vols[1] > vols[2]) {
    d.status = QuoteStatus::InconsistentIvInterval;
    return;
  }
  if (!std::isfinite(vols[2] * vols[2] * e.variance_end)) {
    d.status = QuoteStatus::NonfiniteVariance;
    return;
  }
  d.status = QuoteStatus::Used;
  ++out.used_quotes;
  ++out.expiries[q.expiry_index].used_quotes;
}

void aggregate(std::span<const AmericanMarkovFitQuote> quotes, AmericanMarkovQuoteSeeds &out) {
  std::vector<std::vector<Band>> bands(out.expiries.size());
  std::vector<std::vector<double>> spreads(out.expiries.size());
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    if (out.quotes[i].status != QuoteStatus::Used) continue;
    const auto expiry = quotes[i].expiry_index;
    const auto &v = out.quotes[i].equivalent_vols;
    const double years = out.expiries[expiry].variance_end;
    bands[expiry].push_back({*v[0] * *v[0] * years, *v[1] * *v[1] * years,
                            *v[2] * *v[2] * years});
    spreads[expiry].push_back(relative_spread(quotes[i]));
  }
  for (std::size_t i = 0; i < out.expiries.size(); ++i) {
    if (bands[i].empty()) continue;
    auto &e = out.expiries[i];
    e.total_variance = median_band(bands[i]);
    e.total_variance_mad = mad(bands[i], e.total_variance->midpoint);
    e.median_relative_spread = median(std::move(spreads[i]));
  }
  for (auto &endpoint : out.variance_endpoints) {
    std::vector<Band> estimates;
    for (const auto expiry : endpoint.expiry_indices) {
      if (out.expiries[expiry].total_variance) {
        estimates.push_back(*out.expiries[expiry].total_variance);
      }
    }
    endpoint.supported_expiries = estimates.size();
    if (estimates.empty()) continue;
    endpoint.total_variance = median_band(estimates);
    endpoint.total_variance_mad = mad(estimates, endpoint.total_variance->midpoint);
  }
}

AmericanMarkovSeedParameter parameter(Band variance, const AmericanMarkovSeedLayout &layout) {
  AmericanMarkovSeedParameter p;
  p.variance_rate = variance;
  if (!std::isfinite(variance.midpoint) || !std::isfinite(variance.lower) ||
      !std::isfinite(variance.upper)) {
    p.status = ParameterStatus::NonfiniteVariance;
  } else if (variance.midpoint < 0) {
    p.status = ParameterStatus::NegativeForwardVariance;
  } else if (variance.lower <= 0 || variance.upper < variance.midpoint ||
             variance.midpoint < variance.lower) {
    p.status = ParameterStatus::UncertainForwardVariance;
  } else {
    const double sigma = std::sqrt(variance.midpoint);
    if (sigma < layout.min_sigma || sigma > layout.max_sigma) {
      p.status = ParameterStatus::SigmaBounds;
    } else {
      p.status = ParameterStatus::Ready;
      p.sigma = sigma;
    }
  }
  return p;
}

void infer_parameters(const AmericanMarkovSeedLayout &layout, AmericanMarkovQuoteSeeds &out) {
  std::vector<AmericanMarkovSeedParameter> groups(out.time_groups);
  if (layout.time_shape == AmericanMarkovFitTimeShape::Shared) {
    std::vector<Band> rates;
    for (const auto &e : out.variance_endpoints) {
      if (!e.total_variance) continue;
      const auto &w = *e.total_variance;
      rates.push_back({w.lower / e.variance_end, w.midpoint / e.variance_end,
                       w.upper / e.variance_end});
    }
    if (!rates.empty()) groups[0] = parameter(median_band(rates), layout);
  } else {
    for (std::size_t i = 0; i < out.variance_endpoints.size(); ++i) {
      const auto &e = out.variance_endpoints[i];
      if (!e.total_variance || (i > 0 && !out.variance_endpoints[i - 1].total_variance)) continue;
      const auto &w = *e.total_variance;
      const Band previous = i == 0 ? Band{} : *out.variance_endpoints[i - 1].total_variance;
      const double dv = e.variance_end - (i == 0 ? 0 : out.variance_endpoints[i - 1].variance_end);
      groups[i] = parameter({(w.lower - previous.upper) / dv,
                            (w.midpoint - previous.midpoint) / dv,
                            (w.upper - previous.lower) / dv}, layout);
    }
  }
  bool complete = true;
  for (std::size_t i = 0; i < out.parameters.size(); ++i) {
    out.parameters[i] = groups[i / out.log_state_knots.size()];
    complete = complete && out.parameters[i].sigma.has_value();
  }
  if (complete) {
    for (const auto &p : out.parameters) out.initial_sigmas.push_back(*p.sigma);
  }
}
} // namespace

Result<AmericanMarkovQuoteSeeds> estimate_american_markov_quote_seeds(
    const AmericanMarkovFitMarket &market, std::span<const AmericanMarkovFitQuote> quotes,
    const AmericanMarkovSeedLayout &layout, const AmericanMarkovSeedConfig &config) {
  ATX_TRY_VOID(validate(market, quotes, layout, config));
  AmericanMarkovQuoteSeeds out;
  out.configuration = config;
  out.time_shape = layout.time_shape;
  out.min_sigma = layout.min_sigma;
  out.max_sigma = layout.max_sigma;
  out.parameter_capacity = layout.max_parameters;
  ATX_TRY(const bool cash, prepare_schedule(market, layout, out));
  out.quotes.resize(quotes.size());
  if (cash) {
    for (auto &quote : out.quotes) quote.status = QuoteStatus::UnsupportedCashSchedule;
    for (auto &p : out.parameters) p.status = ParameterStatus::UnsupportedCashSchedule;
    out.refused_quotes = quotes.size();
    return Ok(std::move(out));
  }
  const auto selected = candidates(market, quotes, out);
  // Round-robin across chronological expiries: no dense first expiry consumes
  // every inversion slot. Selection only affects seeds, never eventual fit rows.
  for (std::size_t rank = 0; rank < config.candidates_per_expiry; ++rank) {
    for (const auto &indices : selected) {
      if (rank >= indices.size() || config.max_inversions - out.inversion_calls < 3U ||
          config.max_root_iteration_slots - out.reserved_root_iteration_slots <
              3U * config.max_iv_iterations) continue;
      const auto i = indices[rank];
      invert(quotes[i], market.spot, layout, out.quotes[i], out);
    }
  }
  out.refused_quotes = quotes.size() - out.used_quotes;
  aggregate(quotes, out);
  infer_parameters(layout, out);
  return Ok(std::move(out));
}

const char *to_string(AmericanMarkovSeedQuoteStatus status) noexcept {
  switch (status) {
  case QuoteStatus::Used: return "used";
  case QuoteStatus::InvalidQuote: return "invalid_quote";
  case QuoteStatus::DuplicateContract: return "duplicate_contract";
  case QuoteStatus::UnsupportedCashSchedule: return "unsupported_cash_schedule";
  case QuoteStatus::ZeroVariance: return "zero_variance";
  case QuoteStatus::Bidless: return "bidless";
  case QuoteStatus::WideSpread: return "wide_spread";
  case QuoteStatus::FarMoneyness: return "far_moneyness";
  case QuoteStatus::IntrinsicPlateau: return "intrinsic_plateau";
  case QuoteStatus::CandidateLimit: return "candidate_limit";
  case QuoteStatus::InversionBudget: return "inversion_budget";
  case QuoteStatus::InversionFailure: return "inversion_failure";
  case QuoteStatus::IvFloor: return "iv_floor";
  case QuoteStatus::SigmaBounds: return "sigma_bounds";
  case QuoteStatus::InconsistentIvInterval: return "inconsistent_iv_interval";
  case QuoteStatus::NonfiniteVariance: return "nonfinite_variance";
  }
  return "unknown";
}

const char *to_string(AmericanMarkovSeedParameterStatus status) noexcept {
  switch (status) {
  case ParameterStatus::Ready: return "ready";
  case ParameterStatus::MissingEndpoint: return "missing_endpoint";
  case ParameterStatus::NegativeForwardVariance: return "negative_forward_variance";
  case ParameterStatus::UncertainForwardVariance: return "uncertain_forward_variance";
  case ParameterStatus::SigmaBounds: return "sigma_bounds";
  case ParameterStatus::UnsupportedCashSchedule: return "unsupported_cash_schedule";
  case ParameterStatus::NonfiniteVariance: return "nonfinite_variance";
  }
  return "unknown";
}

} // namespace atx::vol
