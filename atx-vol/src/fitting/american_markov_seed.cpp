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
  if (c.estimator != AmericanMarkovSeedEstimator::BidMidAsk &&
      c.estimator != AmericanMarkovSeedEstimator::SigmaInterval) {
    return Err(ErrorCode::InvalidArgument, "Markov seeds: unknown estimator");
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

double intrinsic_value(const AmericanMarkovFitQuote &quote, double spot) {
  return std::max(quote.side == Side::Call ? spot - quote.strike : quote.strike - spot, 0.0);
}

double time_value_floor(const AmericanMarkovFitQuote &quote, double spot,
                        const AmericanMarkovSeedConfig &c) {
  return std::max(c.min_time_value_over_spot * spot,
      64 * std::numeric_limits<double>::epsilon() * std::max(spot, quote.strike));
}

// A bid at or below the intrinsic floor carries no lower-bound information.
bool informative_bid(const AmericanMarkovFitQuote &quote, double spot,
                     const AmericanMarkovSeedConfig &c) {
  return quote.bid - intrinsic_value(quote, spot) > time_value_floor(quote, spot, c);
}

QuoteStatus classify(const AmericanMarkovFitQuote &quote, double spot,
                       const AmericanMarkovSeedExpiryDiagnostics &expiry,
                       const AmericanMarkovSeedConfig &c) {
  if (expiry.variance_end <= 0.0) return QuoteStatus::ZeroVariance;
  if (c.estimator == AmericanMarkovSeedEstimator::SigmaInterval) {
    // Bidless and wide quotes stay eligible; only an ask on the intrinsic
    // plateau (no upper-bound information, hence no bid information) is refused.
    if (moneyness(quote, spot, expiry) > c.max_abs_log_forward_moneyness) {
      return QuoteStatus::FarMoneyness;
    }
    if (quote.ask - intrinsic_value(quote, spot) <= time_value_floor(quote, spot, c)) {
      return QuoteStatus::IntrinsicPlateau;
    }
    return QuoteStatus::InversionBudget;
  }
  if (quote.bid == 0.0) return QuoteStatus::Bidless;
  if (relative_spread(quote) > c.max_relative_spread) return QuoteStatus::WideSpread;
  if (moneyness(quote, spot, expiry) > c.max_abs_log_forward_moneyness) {
    return QuoteStatus::FarMoneyness;
  }
  if (!informative_bid(quote, spot, c)) return QuoteStatus::IntrinsicPlateau;
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

// SigmaInterval: ask -> upper bound, informative bid -> lower bound. An IV
// outside the layout range on the uninformative side (ask above max_sigma,
// bid below min_sigma or on the IV floor) is no bound; on the conflicting side
// (ask below min_sigma or on the IV floor, bid above max_sigma) the quote is
// refused, since its other side is then uninformative too. Never clipped.
void invert_interval(const AmericanMarkovFitQuote &q, double spot,
                     const AmericanMarkovSeedLayout &layout,
                     AmericanMarkovSeedQuoteDiagnostics &d, AmericanMarkovQuoteSeeds &out) {
  const auto &c = out.configuration;
  const auto &e = out.expiries[q.expiry_index];
  bool failed = false;
  const auto attempt = [&](std::size_t slot, double price) -> std::optional<double> {
    ++d.inversion_calls;
    ++out.inversion_calls;
    out.reserved_root_iteration_slots += c.max_iv_iterations;
    const auto result = american_implied_vol(price, spot, q.strike, e.variance_end,
        e.integrated_rate / e.variance_end, e.integrated_yield / e.variance_end, q.side,
        AmericanMethod::AndersenLake, c.iv_tolerance, c.max_iv_iterations, c.inversion_options);
    if (!result) {
      if (d.inversion_error.empty()) d.inversion_error = result.error().to_string();
      failed = true;
      return std::nullopt;
    }
    d.equivalent_vols[slot] = *result;
    return *result;
  };
  const auto floor_iv = [&](double iv) {
    return !std::isfinite(iv) || iv <= kIvMin + c.iv_tolerance;
  };
  std::optional<double> lower;
  std::optional<double> upper;
  std::optional<QuoteStatus> conflict;
  if (const auto iv = attempt(2, q.ask)) {
    if (floor_iv(*iv)) conflict = QuoteStatus::IvFloor;
    else if (*iv < layout.min_sigma) conflict = QuoteStatus::SigmaBounds;
    else if (*iv <= layout.max_sigma) upper = *iv;
  }
  if (!conflict && informative_bid(q, spot, c)) {
    if (const auto iv = attempt(0, q.bid)) {
      if (!floor_iv(*iv) && *iv > layout.max_sigma) conflict = QuoteStatus::SigmaBounds;
      else if (!floor_iv(*iv) && *iv >= layout.min_sigma) lower = *iv;
    }
  }
  if (lower && upper && *lower > *upper) {
    d.status = QuoteStatus::InconsistentIvInterval; // Preserved IVs; never reordered.
    return;
  }
  if (conflict) {
    d.status = *conflict;
    return;
  }
  if (!lower && !upper) {
    d.status = failed ? QuoteStatus::InversionFailure
                      : q.bid == 0.0 ? QuoteStatus::Bidless : QuoteStatus::SigmaBounds;
    return;
  }
  d.lower_bound = lower.has_value();
  d.upper_bound = upper.has_value();
  d.status = QuoteStatus::Used;
  ++out.used_quotes;
  ++out.expiries[q.expiry_index].used_quotes;
}

// Running SigmaInterval intersection: max of lower bounds, min of upper bounds.
struct IntervalBounds {
  std::optional<double> lower;
  std::optional<double> upper;
  std::size_t lower_quotes = 0;
  std::size_t upper_quotes = 0;

  void add(const AmericanMarkovSeedQuoteDiagnostics &d) {
    if (d.lower_bound) {
      lower = std::max(lower.value_or(*d.equivalent_vols[0]), *d.equivalent_vols[0]);
      ++lower_quotes;
    }
    if (d.upper_bound) {
      upper = std::min(upper.value_or(*d.equivalent_vols[2]), *d.equivalent_vols[2]);
      ++upper_quotes;
    }
  }
  void merge(const IntervalBounds &other) {
    if (other.lower) lower = std::max(lower.value_or(*other.lower), *other.lower);
    if (other.upper) upper = std::min(upper.value_or(*other.upper), *other.upper);
    lower_quotes += other.lower_quotes;
    upper_quotes += other.upper_quotes;
  }
};

std::optional<AmericanMarkovSeedSigmaInterval> make_sigma_interval(
    const IntervalBounds &b, const AmericanMarkovSeedLayout &layout) {
  if (!b.lower && !b.upper) return std::nullopt; // Unbounded both ways: no estimate.
  AmericanMarkovSeedSigmaInterval out;
  out.lower = b.lower;
  out.upper = b.upper;
  out.effective_lower = b.lower.value_or(layout.min_sigma);
  out.effective_upper = b.upper.value_or(layout.max_sigma);
  out.geometric_midpoint = std::sqrt(out.effective_lower * out.effective_upper);
  out.log_width = std::log(out.effective_upper) - std::log(out.effective_lower);
  out.crossed = out.effective_lower > out.effective_upper;
  out.lower_bound_quotes = b.lower_quotes;
  out.upper_bound_quotes = b.upper_quotes;
  return out;
}

// Descriptive total-variance band {low^2, lower*upper, high^2} * years.
Band interval_band(const AmericanMarkovSeedSigmaInterval &s, double years) {
  const double low = std::min(s.effective_lower, s.effective_upper);
  const double high = std::max(s.effective_lower, s.effective_upper);
  return {low * low * years, s.effective_lower * s.effective_upper * years, high * high * years};
}

void aggregate_interval(std::span<const AmericanMarkovFitQuote> quotes,
                        const AmericanMarkovSeedLayout &layout, AmericanMarkovQuoteSeeds &out) {
  std::vector<IntervalBounds> bounds(out.expiries.size());
  std::vector<std::vector<double>> spreads(out.expiries.size());
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    if (out.quotes[i].status != QuoteStatus::Used) continue;
    bounds[quotes[i].expiry_index].add(out.quotes[i]);
    spreads[quotes[i].expiry_index].push_back(relative_spread(quotes[i]));
  }
  IntervalBounds all;
  for (std::size_t i = 0; i < out.expiries.size(); ++i) {
    auto &e = out.expiries[i];
    e.sigma_interval = make_sigma_interval(bounds[i], layout);
    if (!e.sigma_interval) continue;
    e.total_variance = interval_band(*e.sigma_interval, e.variance_end);
    e.median_relative_spread = median(std::move(spreads[i]));
    all.merge(bounds[i]);
  }
  for (auto &endpoint : out.variance_endpoints) {
    IntervalBounds merged;
    for (const auto expiry : endpoint.expiry_indices) {
      if (!out.expiries[expiry].sigma_interval) continue;
      merged.merge(bounds[expiry]);
      ++endpoint.supported_expiries;
    }
    endpoint.sigma_interval = make_sigma_interval(merged, layout);
    if (endpoint.sigma_interval) {
      endpoint.total_variance = interval_band(*endpoint.sigma_interval, endpoint.variance_end);
    }
  }
  if (layout.time_shape == AmericanMarkovFitTimeShape::Shared) {
    out.sigma_interval = make_sigma_interval(all, layout);
  }
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

AmericanMarkovSeedParameter checked_rate(Band rate, const AmericanMarkovSeedLayout &layout) {
  AmericanMarkovSeedParameter p;
  p.variance_rate = rate;
  if (!std::isfinite(rate.lower) || !std::isfinite(rate.midpoint) ||
      !std::isfinite(rate.upper)) {
    p.status = ParameterStatus::NonfiniteVariance;
    return p;
  }
  if (!(rate.midpoint > 0.0)) {
    p.status = ParameterStatus::NegativeForwardVariance;
    return p;
  }
  const double sigma = std::sqrt(rate.midpoint);
  if (sigma < layout.min_sigma || sigma > layout.max_sigma) {
    p.status = ParameterStatus::SigmaBounds;
    return p;
  }
  p.status = ParameterStatus::Ready;
  p.sigma = sigma;
  return p;
}

// SigmaInterval groups. Shared: geometric midpoint over every used quote.
// PerVarianceInterval: difference geometric-midpoint total variance between
// adjacent endpoints; the band propagates the effective bounds descriptively.
std::vector<AmericanMarkovSeedParameter> interval_groups(const AmericanMarkovSeedLayout &layout,
                                                         const AmericanMarkovQuoteSeeds &out) {
  std::vector<AmericanMarkovSeedParameter> groups(out.time_groups);
  if (layout.time_shape == AmericanMarkovFitTimeShape::Shared) {
    if (out.sigma_interval) {
      const auto &s = *out.sigma_interval;
      groups[0] = checked_rate(interval_band(s, 1.0), layout);
    }
    return groups;
  }
  for (std::size_t i = 0; i < out.variance_endpoints.size(); ++i) {
    const auto &e = out.variance_endpoints[i];
    if (!e.sigma_interval || (i > 0 && !out.variance_endpoints[i - 1].sigma_interval)) continue;
    const Band total = *e.total_variance;
    const Band previous = i == 0 ? Band{} : *out.variance_endpoints[i - 1].total_variance;
    const double dv = e.variance_end - (i == 0 ? 0 : out.variance_endpoints[i - 1].variance_end);
    groups[i] = checked_rate({(total.lower - previous.upper) / dv,
                              (total.midpoint - previous.midpoint) / dv,
                              (total.upper - previous.lower) / dv}, layout);
  }
  return groups;
}

void infer_parameters(const AmericanMarkovSeedLayout &layout, AmericanMarkovQuoteSeeds &out) {
  std::vector<AmericanMarkovSeedParameter> groups(out.time_groups);
  if (out.configuration.estimator == AmericanMarkovSeedEstimator::SigmaInterval) {
    groups = interval_groups(layout, out);
  } else if (layout.time_shape == AmericanMarkovFitTimeShape::Shared) {
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
  const bool interval = config.estimator == AmericanMarkovSeedEstimator::SigmaInterval;
  // Round-robin across chronological expiries: no dense first expiry consumes
  // every inversion slot. Selection only affects seeds, never eventual fit rows.
  // A quote is attempted only when its whole inversion need fits the budget:
  // three calls (bid/mid/ask), or for SigmaInterval one per informative side.
  for (std::size_t rank = 0; rank < config.candidates_per_expiry; ++rank) {
    for (const auto &indices : selected) {
      if (rank >= indices.size()) continue;
      const auto i = indices[rank];
      const std::size_t need = !interval ? 3U
          : informative_bid(quotes[i], market.spot, config) ? 2U : 1U;
      if (config.max_inversions - out.inversion_calls < need ||
          config.max_root_iteration_slots - out.reserved_root_iteration_slots <
              need * config.max_iv_iterations) continue;
      if (interval) invert_interval(quotes[i], market.spot, layout, out.quotes[i], out);
      else invert(quotes[i], market.spot, layout, out.quotes[i], out);
    }
  }
  out.refused_quotes = quotes.size() - out.used_quotes;
  if (interval) aggregate_interval(quotes, layout, out);
  else aggregate(quotes, out);
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
