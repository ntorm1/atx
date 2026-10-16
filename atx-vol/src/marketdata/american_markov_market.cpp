#include "atx/vol/api/marketdata/american_markov_market.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace atx::vol {
namespace {
using MarketStatus = AmericanMarkovMarketStatus;
using Dividends = AmericanMarkovMarketDividends;

bool positive(double x) noexcept { return std::isfinite(x) && x > 0.0; }
bool nonnegative(double x) noexcept { return std::isfinite(x) && x >= 0.0; }
bool bounded_log(double x) noexcept {
  return std::isfinite(x) && std::abs(x) <= kAmericanMarkovMarketMaxLogFactor;
}

bool valid_config(const AmericanMarkovMarketConfig &c) noexcept {
  if (c.max_quotes == 0 || c.max_quotes > kAmericanMarkovFitMaxQuotes ||
      c.max_expiries == 0 || c.max_expiries > kAmericanMarkovMarketMaxExpiries ||
      !positive(c.min_calendar_increment) ||
      c.min_calendar_increment > kAmericanMarkovMarketMaxYears ||
      !positive(c.max_absolute_interval_rate) ||
      c.max_absolute_interval_rate > kAmericanMarkovMarketMaxAnnualRate ||
      !positive(c.max_absolute_interval_yield) ||
      c.max_absolute_interval_yield > kAmericanMarkovMarketMaxAnnualRate) return false;
  if (c.dividends != Dividends::None && c.dividends != Dividends::FundingPvContinuousYield &&
      c.dividends != Dividends::EscrowedPvContinuousYield) return false;
  for (const double tolerance : {c.max_spot_relative_deviation, c.max_clock_absolute_deviation,
       c.max_rate_absolute_deviation, c.max_yield_absolute_deviation,
       c.max_dividend_pv_over_spot_deviation, c.max_prepaid_over_spot_deviation,
       c.max_forward_over_spot_deviation}) {
    if (!nonnegative(tolerance)) return false;
  }
  return true;
}

bool valid_row(const AmericanMarkovRawQuote &q) noexcept {
  return positive(q.strike) && (q.side == Side::Call || q.side == Side::Put) &&
      nonnegative(q.bid) && positive(q.ask) && q.bid <= q.ask &&
      positive(q.weight) && q.weight <= 1.0e6 && positive(q.underlying_spot) &&
      positive(q.calendar_years) && q.calendar_years <= kAmericanMarkovMarketMaxYears &&
      nonnegative(q.variance_years) && q.variance_years <= kAmericanMarkovMarketMaxYears &&
      std::isfinite(q.rate) && std::abs(q.rate) <= kAmericanMarkovMarketMaxAnnualRate &&
      std::isfinite(q.continuous_yield) &&
      std::abs(q.continuous_yield) <= kAmericanMarkovMarketMaxAnnualRate &&
      nonnegative(q.dividend_pv);
}

double sorted_median(std::span<const double> values) noexcept {
  const auto n = values.size();
  return n % 2 == 0 ? std::midpoint(values[n / 2 - 1], values[n / 2]) : values[n / 2];
}

// Every caller supplies a nonempty finite vector. Reuse the same scratch for
// absolute deviations; all statistics remain independent of input row order.
AmericanMarkovMarketSummary summarize(std::vector<double> &values) {
  std::sort(values.begin(), values.end());
  AmericanMarkovMarketSummary out;
  out.median = sorted_median(values);
  out.minimum = values.front();
  out.maximum = values.back();
  out.maximum_absolute_deviation =
      std::max(out.median - out.minimum, out.maximum - out.median);
  for (double &value : values) value = std::abs(value - out.median);
  std::sort(values.begin(), values.end());
  out.median_absolute_deviation = sorted_median(values);
  return out;
}

struct ExpiryGroup {
  std::size_t begin = 0;
  std::size_t end = 0;
  AmericanMarkovExpiryMarketDiagnostics diag;
};

AmericanMarkovExpiryMarketDiagnostics summarize_expiry(
    std::span<const AmericanMarkovRawQuote> quotes, std::span<const std::size_t> indices,
    std::vector<double> &scratch) {
  AmericanMarkovExpiryMarketDiagnostics d;
  d.expiry_key = quotes[indices.front()].expiry_key;
  d.quote_count = indices.size();
  const auto column = [&](double AmericanMarkovRawQuote::*member) {
    scratch.clear();
    for (const auto i : indices) scratch.push_back(quotes[i].*member);
    return summarize(scratch);
  };
  d.calendar_years = column(&AmericanMarkovRawQuote::calendar_years);
  d.variance_years = column(&AmericanMarkovRawQuote::variance_years);
  d.rate = column(&AmericanMarkovRawQuote::rate);
  d.supplied_yield = column(&AmericanMarkovRawQuote::continuous_yield);
  d.dividend_pv = column(&AmericanMarkovRawQuote::dividend_pv);
  return d;
}

struct Carry {
  double integrated_rate = 0.0;
  double integrated_yield = 0.0;
  double prepaid_ratio = 0.0;
  double forward_ratio = 0.0;
};

MarketStatus resolve_carry(double spot, double years, double rate, double yield, double pv,
                           Dividends dividends, Carry &out) noexcept {
  out.integrated_rate = rate * years;
  out.integrated_yield = yield * years;
  if (!bounded_log(out.integrated_rate) || !bounded_log(out.integrated_yield)) {
    return MarketStatus::NumericLimit;
  }
  if (dividends == Dividends::None && pv != 0.0) return MarketStatus::DividendConvention;
  if (pv != 0.0) {
    const double pv_ratio = pv / spot;
    // Funding PV may exceed spot with negative q, provided prepaid remains
    // positive. Never apply the escrowed PV<spot guard to this convention.
    const double dividend_fraction = dividends == Dividends::FundingPvContinuousYield
        ? pv_ratio * std::exp(out.integrated_yield) : pv_ratio;
    if (!std::isfinite(dividend_fraction)) return MarketStatus::NumericLimit;
    if (!(dividend_fraction < 1.0)) return MarketStatus::NonpositivePrepaid;
    out.integrated_yield -= std::log1p(-dividend_fraction);
  }
  const double log_forward = out.integrated_rate - out.integrated_yield;
  if (!bounded_log(out.integrated_yield) || !bounded_log(log_forward)) {
    return MarketStatus::NumericLimit;
  }
  out.prepaid_ratio = std::exp(-out.integrated_yield);
  out.forward_ratio = std::exp(log_forward);
  // Ratios can be representable while their actual stock values overflow.
  if (!positive(spot * out.prepaid_ratio) || !positive(spot * out.forward_ratio)) {
    return MarketStatus::NumericLimit;
  }
  return MarketStatus::Success;
}

MarketStatus summarize_carry(
    std::span<const AmericanMarkovRawQuote> quotes, std::span<const std::size_t> indices,
    double spot, const AmericanMarkovMarketConfig &c,
    AmericanMarkovExpiryMarketDiagnostics &d, std::vector<double> &scratch,
    std::optional<std::size_t> &refused_row) {
  std::vector<double> forwards;
  forwards.reserve(indices.size());
  scratch.clear();
  for (const auto i : indices) {
    const auto &q = quotes[i];
    Carry carry;
    const auto status = resolve_carry(spot, d.calendar_years.median, q.rate,
                                     q.continuous_yield, q.dividend_pv, c.dividends, carry);
    if (status != MarketStatus::Success) {
      refused_row = i;
      return status;
    }
    scratch.push_back(carry.prepaid_ratio);
    forwards.push_back(carry.forward_ratio);
  }
  d.prepaid_over_spot = summarize(scratch);
  d.forward_over_spot = summarize(forwards);
  Carry resolved;
  const auto status = resolve_carry(spot, d.calendar_years.median, d.rate.median,
      d.supplied_yield.median, d.dividend_pv.median, c.dividends, resolved);
  if (status != MarketStatus::Success) return status;
  d.integrated_rate = resolved.integrated_rate;
  d.integrated_effective_yield = resolved.integrated_yield;
  d.resolved_prepaid_over_spot = resolved.prepaid_ratio;
  d.resolved_forward_over_spot = resolved.forward_ratio;
  if (d.rate.maximum_absolute_deviation > c.max_rate_absolute_deviation ||
      d.supplied_yield.maximum_absolute_deviation > c.max_yield_absolute_deviation ||
      d.dividend_pv.maximum_absolute_deviation / spot > c.max_dividend_pv_over_spot_deviation ||
      d.prepaid_over_spot.maximum_absolute_deviation > c.max_prepaid_over_spot_deviation ||
      d.forward_over_spot.maximum_absolute_deviation > c.max_forward_over_spot_deviation) {
    return MarketStatus::CarryDispersion;
  }
  return MarketStatus::Success;
}

MarketStatus prepare_intervals(
    std::span<const AmericanMarkovRawQuote> quotes, std::span<const std::size_t> order,
    std::vector<ExpiryGroup> &groups, double spot, const AmericanMarkovMarketConfig &c,
    AmericanMarkovMarketDiagnostics &diagnostics, std::vector<double> &scratch) {
  double previous_calendar = 0.0;
  double previous_variance = 0.0;
  double previous_rate = 0.0;
  double previous_yield = 0.0;
  for (auto &group : groups) {
    auto &d = group.diag;
    diagnostics.refused_expiry = d.expiry_key;
    if (d.calendar_years.maximum_absolute_deviation > c.max_clock_absolute_deviation ||
        d.variance_years.maximum_absolute_deviation > c.max_clock_absolute_deviation) {
      diagnostics.expiries.push_back(d);
      return MarketStatus::ClockDispersion;
    }
    const auto indices = order.subspan(group.begin, group.end - group.begin);
    const auto carry_status = summarize_carry(quotes, indices, spot, c, d, scratch,
                                               diagnostics.refused_row);
    if (carry_status != MarketStatus::Success) {
      diagnostics.expiries.push_back(d);
      return carry_status;
    }
    const double dt = d.calendar_years.median - previous_calendar;
    MarketStatus status = MarketStatus::Success;
    if (!(dt >= c.min_calendar_increment)) {
      status = MarketStatus::AmbiguousExpiry;
    } else if (d.variance_years.median < previous_variance) {
      status = MarketStatus::NonmonotoneVarianceClock;
    } else {
      d.interval_rate = (d.integrated_rate - previous_rate) / dt;
      d.interval_yield = (d.integrated_effective_yield - previous_yield) / dt;
      if (!std::isfinite(d.interval_rate) || !std::isfinite(d.interval_yield) ||
          std::abs(d.interval_rate) > c.max_absolute_interval_rate ||
          std::abs(d.interval_yield) > c.max_absolute_interval_yield) {
        status = MarketStatus::NumericLimit;
      }
    }
    diagnostics.expiries.push_back(d);
    if (status != MarketStatus::Success) return status;
    previous_calendar = d.calendar_years.median;
    previous_variance = d.variance_years.median;
    previous_rate = d.integrated_rate;
    previous_yield = d.integrated_effective_yield;
  }
  diagnostics.refused_expiry.reset();
  return MarketStatus::Success;
}
} // namespace

const char *to_string(AmericanMarkovMarketDividends value) noexcept {
  switch (value) {
  case Dividends::None: return "none";
  case Dividends::FundingPvContinuousYield: return "funding_pv_continuous_yield_approximation";
  case Dividends::EscrowedPvContinuousYield: return "escrowed_pv_continuous_yield_approximation";
  }
  return "invalid";
}

const char *to_string(AmericanMarkovMarketStatus value) noexcept {
  switch (value) {
  case MarketStatus::Success: return "success";
  case MarketStatus::InvalidConfig: return "invalid_config";
  case MarketStatus::ResourceLimit: return "resource_limit";
  case MarketStatus::InvalidRow: return "invalid_row";
  case MarketStatus::DuplicateContract: return "duplicate_contract";
  case MarketStatus::SpotDispersion: return "spot_dispersion";
  case MarketStatus::ClockDispersion: return "clock_dispersion";
  case MarketStatus::CarryDispersion: return "carry_dispersion";
  case MarketStatus::DividendConvention: return "dividend_convention";
  case MarketStatus::NonpositivePrepaid: return "nonpositive_prepaid";
  case MarketStatus::AmbiguousExpiry: return "ambiguous_expiry";
  case MarketStatus::NonmonotoneVarianceClock: return "nonmonotone_variance_clock";
  case MarketStatus::NumericLimit: return "numeric_limit";
  }
  return "invalid";
}

AmericanMarkovMarketPreparation prepare_american_markov_market(
    std::span<const AmericanMarkovRawQuote> quotes, const AmericanMarkovMarketConfig &config) {
  AmericanMarkovMarketPreparation out;
  out.diagnostics.quote_count = quotes.size();
  if (!valid_config(config)) return out;
  if (quotes.empty() || quotes.size() > config.max_quotes) {
    out.status = MarketStatus::ResourceLimit;
    return out;
  }
  std::vector<double> scratch;
  scratch.reserve(quotes.size());
  for (std::size_t i = 0; i < quotes.size(); ++i) {
    if (!valid_row(quotes[i])) {
      out.status = MarketStatus::InvalidRow;
      out.diagnostics.refused_row = i;
      out.diagnostics.refused_expiry = quotes[i].expiry_key;
      return out;
    }
    scratch.push_back(quotes[i].underlying_spot);
  }
  out.diagnostics.spot = summarize(scratch);
  const double spot = out.diagnostics.spot.median;
  if (out.diagnostics.spot.maximum_absolute_deviation / spot >
      config.max_spot_relative_deviation) {
    out.status = MarketStatus::SpotDispersion;
    return out;
  }
  std::vector<std::size_t> order(quotes.size());
  std::iota(order.begin(), order.end(), std::size_t{});
  const auto less = [&](std::size_t a, std::size_t b) {
    const auto &x = quotes[a];
    const auto &y = quotes[b];
    if (x.expiry_key != y.expiry_key) return x.expiry_key < y.expiry_key;
    if (x.strike != y.strike) return x.strike < y.strike;
    return x.side < y.side;
  };
  std::sort(order.begin(), order.end(), less);
  std::vector<ExpiryGroup> groups;
  groups.reserve(std::min(quotes.size(), config.max_expiries));
  std::size_t begin = 0;
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (i > 0 && !less(order[i - 1], order[i])) {
      out.status = MarketStatus::DuplicateContract;
      out.diagnostics.refused_row = order[i];
      out.diagnostics.refused_expiry = quotes[order[i]].expiry_key;
      return out;
    }
    if (i + 1 == order.size() ||
        quotes[order[i + 1]].expiry_key != quotes[order[i]].expiry_key) {
      if (groups.size() >= config.max_expiries) {
        out.status = MarketStatus::ResourceLimit;
        return out;
      }
      const auto indices = std::span<const std::size_t>(order).subspan(begin, i + 1 - begin);
      groups.push_back({begin, i + 1, summarize_expiry(quotes, indices, scratch)});
      begin = i + 1;
    }
  }
  std::sort(groups.begin(), groups.end(), [](const auto &a, const auto &b) {
    if (a.diag.calendar_years.median != b.diag.calendar_years.median) {
      return a.diag.calendar_years.median < b.diag.calendar_years.median;
    }
    return a.diag.expiry_key < b.diag.expiry_key;
  });
  out.diagnostics.expiries.reserve(groups.size());
  out.status = prepare_intervals(quotes, order, groups, spot, config, out.diagnostics, scratch);
  if (out.status != MarketStatus::Success) return out;

  AmericanMarkovPreparedMarket market;
  market.spot_ = spot;
  market.intervals_.reserve(groups.size());
  market.keys_.reserve(groups.size());
  market.quotes_.resize(quotes.size());
  for (std::size_t expiry = 0; expiry < groups.size(); ++expiry) {
    const auto &group = groups[expiry];
    const auto &d = group.diag;
    market.keys_.push_back(d.expiry_key);
    market.intervals_.push_back({d.calendar_years.median, d.variance_years.median,
                                d.interval_rate, d.interval_yield, 0.0});
    for (std::size_t j = group.begin; j < group.end; ++j) {
      const auto i = order[j];
      const auto &q = quotes[i];
      market.quotes_[i] = {expiry, q.strike, q.side, q.bid, q.ask, q.weight};
    }
  }
  out.market = std::move(market);
  return out;
}
} // namespace atx::vol
