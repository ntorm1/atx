// Lane C2 (2026-10-22): the served support of a published quote slice and the
// representation-level publication check (quote_slice.hpp, "Served support" and
// "Publication validation").
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/vol/api/fitting/american_price_slice.hpp"
#include "atx/vol/api/fitting/quote_slice.hpp"
#include "fitting/quote_slice_nodes.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::Ok;

namespace detail {

bool serves(const QuoteSliceFit &fit, double strike, Side side) noexcept {
  if (fit.withheld_nodes.empty() || !fit.prices) return true;
  const auto nodes = fit.prices->nodes();
  // A mask that does not describe these nodes serves nothing (fail safe).
  if (fit.withheld_nodes.size() != nodes.size()) return false;
  const unsigned bit = side == Side::Call ? 1u : 2u;
  const auto withheld = [&](std::size_t i) { return (fit.withheld_nodes[i] & bit) != 0u; };
  const auto right = std::lower_bound(nodes.begin(), nodes.end(), strike,
      [](const AmericanPriceNode &node, double k) { return node.strike < k; });
  if (right == nodes.end()) return false;
  const auto index = static_cast<std::size_t>(right - nodes.begin());
  if (right->strike == strike) return !withheld(index);
  if (index == 0u) return false;
  return !withheld(index - 1u) && !withheld(index);
}

void withhold(QuoteSliceFit &fit, std::size_t node, unsigned bit, std::size_t nodes) {
  assert(node < nodes && "withhold: node outside the slice");
  if (fit.withheld_nodes.size() != nodes) fit.withheld_nodes.assign(nodes, 0);
  fit.withheld_nodes[node] = static_cast<std::uint8_t>(fit.withheld_nodes[node] | bit);
}

} // namespace detail

std::optional<double> served_quote_slice_price(const QuoteSliceFit &fit, double strike,
                                               Side side) noexcept {
  if (fit.fit_status != QuoteSliceFitStatus::Success || !fit.prices) return std::nullopt;
  const auto price = fit.prices->price(strike, side);
  if (!price || !detail::serves(fit, strike, side)) return std::nullopt;
  return price;
}

std::vector<QuoteSliceGap> quote_slice_gaps(const QuoteSliceFit &fit) {
  std::vector<QuoteSliceGap> gaps;
  if (!fit.prices || fit.withheld_nodes.empty()) return gaps;
  const auto nodes = fit.prices->nodes();
  const std::size_t n = nodes.size();
  for (const Side side : {Side::Call, Side::Put}) {
    const unsigned bit = side == Side::Call ? 1u : 2u;
    const auto withheld = [&](std::size_t i) {
      return i >= fit.withheld_nodes.size() || (fit.withheld_nodes[i] & bit) != 0u;
    };
    for (std::size_t i = 0; i < n;) {
      if (!withheld(i)) {
        ++i;
        continue;
      }
      std::size_t j = i;
      while (j + 1u < n && withheld(j + 1u)) ++j; // a run of withheld nodes i..j
      QuoteSliceGap gap;
      gap.side = side;
      gap.lo = nodes[i > 0u ? i - 1u : 0u].strike;
      gap.lo_open = i > 0u;
      gap.hi = nodes[j + 1u < n ? j + 1u : j].strike;
      gap.hi_open = j + 1u < n;
      gaps.push_back(gap);
      i = j + 1u;
    }
  }
  return gaps;
}

const char *to_string(QuoteSlicePublicationViolation value) noexcept {
  switch (value) {
  case QuoteSlicePublicationViolation::Certificate:
    return "certificate";
  case QuoteSlicePublicationViolation::Shape:
    return "shape";
  case QuoteSlicePublicationViolation::CrossSide:
    return "cross_side";
  case QuoteSlicePublicationViolation::Nondividend:
    return "nondividend";
  case QuoteSlicePublicationViolation::QuoteBox:
    return "quote_box";
  case QuoteSlicePublicationViolation::Calendar:
    return "calendar";
  case QuoteSlicePublicationViolation::Extrapolation:
    return "extrapolation";
  case QuoteSlicePublicationViolation::Support:
    return "support";
  case QuoteSlicePublicationViolation::Identification:
    return "identification";
  }
  return "unknown";
}

std::size_t QuoteSlicePublicationReport::total() const noexcept {
  std::size_t sum = 0;
  for (const std::size_t v : violations) sum += v;
  return sum;
}

std::size_t QuoteSlicePublicationReport::count(QuoteSlicePublicationViolation kind) const noexcept {
  const auto k = static_cast<std::size_t>(kind);
  return k < violations.size() ? violations[k] : 0u;
}

namespace {

[[nodiscard]] double allowance(double spot, double strike) noexcept {
  return 1.0e-8 + 1.0e-9 * std::max(spot, strike);
}

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

// Lane C0's one answer per contract (quote_slice.hpp, Identification): the served row's
// labels are query_quote_slice's typed answer for its contract, bit for bit.
[[nodiscard]] bool agrees_with_query(const QuoteSliceFit &fit, const QuoteSliceQuote &quote,
                                     const QuoteSliceRow &row) {
  const auto answer = query_quote_slice(fit, quote.strike, quote.side);
  if (!answer || !same_bits(answer->price, row.fitted_price)) return false;
  if (answer->status == QuoteSliceEvaluationStatus::Success) {
    return row.evaluation_status == QuoteSliceEvaluationStatus::Success &&
           vol_convention(row) == answer->vol_convention &&
           same_bits(row.fitted_iv, answer->volatility) &&
           row.price_only_reason == QuoteSlicePriceOnlyReason::None &&
           std::isnan(row.iv_lower) && std::isnan(row.iv_upper);
  }
  return answer->status == QuoteSliceEvaluationStatus::PriceOnly &&
         vol_convention(row) == QuoteSliceVolConvention::None &&
         row.price_only_reason == answer->reason && same_bits(row.iv_lower, answer->iv_lower) &&
         same_bits(row.iv_upper, answer->iv_upper) &&
         (fit.floor_policy != QuoteSliceFloorPolicy::PriceOnly ||
          row.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly);
}

class Checker {
public:
  Checker(std::span<const QuoteSlicePublicationSlice> slices,
          const QuoteSlicePublicationConfig &config, QuoteSlicePublicationReport &report)
      : slices_(slices), config_(config), report_(report) {}

  void slice(std::size_t s) {
    const QuoteSliceFit &fit = *slices_[s].fit;
    ++report_.slices;
    certificate(s, fit);
    const std::vector<double> points = shape_points(fit);
    for (const Side side : {Side::Call, Side::Put}) shape(s, fit, side, points);
    cross_side(s, fit, points);
    extrapolation(s, fit);
    rows(s, fit);
  }

  void pair(std::size_t a, std::size_t b) {
    // a earlier, b later (the caller orders them).
    const QuoteSliceFit &early = *slices_[a].fit;
    const QuoteSliceFit &late = *slices_[b].fit;
    const auto first = early.prices->nodes();
    const auto second = late.prices->nodes();
    const double lo = std::max(first.front().strike, second.front().strike);
    const double hi = std::min(first.back().strike, second.back().strike);
    if (hi < lo) return;
    ++report_.calendar_pairs;
    std::vector<double> knots;
    knots.reserve(first.size() + second.size());
    for (const AmericanPriceNode &node : first) {
      if (node.strike >= lo && node.strike <= hi) knots.push_back(node.strike);
    }
    for (const AmericanPriceNode &node : second) {
      if (node.strike >= lo && node.strike <= hi) knots.push_back(node.strike);
    }
    std::sort(knots.begin(), knots.end());
    knots.erase(std::unique(knots.begin(), knots.end()), knots.end());
    const double spot = std::max(early.prices->bounds().spot, late.prices->bounds().spot);
    for (std::size_t i = 0; i < knots.size(); ++i) {
      calendar_point(a, b, knots[i], allowance(spot, knots[i]));
      if (i + 1u < knots.size()) {
        // The spread is affine between union knots; the midpoint tests that the served
        // interpolation is too, at the tolerance of the segment's larger strike.
        calendar_point(a, b, 0.5 * (knots[i] + knots[i + 1u]), allowance(spot, knots[i + 1u]));
      }
    }
  }

private:
  void add(QuoteSlicePublicationViolation kind, std::size_t expiry, Side side, double strike,
           double excess, std::size_t row = kQuoteSlicePublicationAll,
           std::size_t other = kQuoteSlicePublicationAll) {
    ++report_.violations[static_cast<std::size_t>(kind)];
    if (report_.findings.size() < kQuoteSlicePublicationMaxFindings) {
      report_.findings.push_back({kind, expiry, other, row, side, strike, excess});
    }
  }

  void certificate(std::size_t s, const QuoteSliceFit &fit) {
    const auto nodes = fit.prices->nodes();
    if (!AmericanPriceSlice::create(nodes, fit.prices->bounds())) {
      add(QuoteSlicePublicationViolation::Certificate, s, Side::Call, nodes.front().strike, 0.0);
    }
    const std::optional<double> df = detail::nondividend_discount(fit);
    if (df && !check_nondividend_american(*fit.prices, *df)) {
      add(QuoteSlicePublicationViolation::Nondividend, s, Side::Call, nodes.front().strike, 0.0);
    }
  }

  // The slice's own evaluation points: every node and dense_points interior points per
  // node segment.
  [[nodiscard]] std::vector<double> shape_points(const QuoteSliceFit &fit) const {
    const auto nodes = fit.prices->nodes();
    std::vector<double> points;
    points.reserve(nodes.size() * (config_.dense_points + 1u));
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      points.push_back(nodes[i].strike);
      if (i + 1u == nodes.size()) break;
      const double left = nodes[i].strike;
      const double width = nodes[i + 1u].strike - left;
      for (std::size_t j = 1; j <= config_.dense_points; ++j) {
        const double t = static_cast<double>(j) / static_cast<double>(config_.dense_points + 1u);
        const double k = left + t * width;
        if (k > left && k < nodes[i + 1u].strike) points.push_back(k);
      }
    }
    return points;
  }

  // Slopes, convexity and the exercise / upper bounds of the served function of one side
  // at its served evaluation points (in strike order, across any gap: a subset of a
  // convex piecewise-linear function's points is convex).
  void shape(std::size_t s, const QuoteSliceFit &fit, Side side,
             const std::vector<double> &points) {
    const AmericanPriceBounds &b = fit.prices->bounds();
    const bool call = side == Side::Call;
    const double slope_min = call ? -b.discount_max : 0.0;
    const double slope_max = call ? 0.0 : b.discount_max;
    double k0 = 0.0;
    double p0 = 0.0;
    double k1 = 0.0;
    double p1 = 0.0;
    std::size_t seen = 0;
    for (const double k : points) {
      const auto served = served_quote_slice_price(fit, k, side);
      if (!served) continue;
      ++report_.points;
      const double p = *served;
      const double tol = allowance(b.spot, k);
      const double floor = std::max(call ? b.spot - k : k - b.spot, 0.0);
      const double cap = call ? b.prepaid_max : k * b.discount_max;
      if (p < floor - tol || p > cap + tol) {
        add(QuoteSlicePublicationViolation::Shape, s, side, k, std::max(floor - p, p - cap) - tol);
      }
      if (seen >= 1u) {
        const double rise = p - p1;
        const double width = k - k1;
        if (rise < slope_min * width - tol || rise > slope_max * width + tol) {
          add(QuoteSlicePublicationViolation::Shape, s, side, k,
              std::max(slope_min * width - rise, rise - slope_max * width) - tol);
        }
      }
      if (seen >= 2u) {
        const double t = (k1 - k0) / (k - k0);
        const double chord = std::lerp(p0, p, t);
        const double bend = p1 - chord - allowance(b.spot, k1);
        if (bend > 0.0) add(QuoteSlicePublicationViolation::Shape, s, side, k1, bend);
      }
      k0 = k1;
      p0 = p1;
      k1 = k;
      p1 = p;
      ++seen;
    }
  }

  void cross_side(std::size_t s, const QuoteSliceFit &fit, const std::vector<double> &points) {
    const AmericanPriceBounds &b = fit.prices->bounds();
    for (const double k : points) {
      const auto c = served_quote_slice_price(fit, k, Side::Call);
      const auto p = served_quote_slice_price(fit, k, Side::Put);
      if (!c || !p) continue;
      const double difference = *c - *p;
      const double lower = b.prepaid_min - k * b.discount_max;
      const double upper = b.prepaid_max - k * b.discount_min;
      const double tol = allowance(b.spot, k);
      if (difference < lower - tol || difference > upper + tol) {
        add(QuoteSlicePublicationViolation::CrossSide, s, Side::Call, k,
            std::max(lower - difference, difference - upper) - tol);
      }
    }
  }

  void extrapolation(std::size_t s, const QuoteSliceFit &fit) {
    const auto nodes = fit.prices->nodes();
    const double below = std::nextafter(nodes.front().strike, 0.0);
    const double above =
        std::nextafter(nodes.back().strike, std::numeric_limits<double>::infinity());
    for (const Side side : {Side::Call, Side::Put}) {
      for (const double k : {below, above}) {
        if (served_quote_slice_price(fit, k, side)) {
          add(QuoteSlicePublicationViolation::Extrapolation, s, side, k, 0.0);
        }
      }
    }
  }

  void rows(std::size_t s, const QuoteSliceFit &fit) {
    const std::span<const QuoteSliceQuote> quotes = slices_[s].quotes;
    const auto nodes = fit.prices->nodes();
    for (const Side side : {Side::Call, Side::Put}) {
      for (std::size_t i = 0; i + 1u < nodes.size(); ++i) {
        if (!detail::serves(fit, 0.5 * (nodes[i].strike + nodes[i + 1u].strike), side)) {
          ++report_.gap_segments;
        }
      }
    }
    for (std::size_t r = 0; r < quotes.size(); ++r) {
      const QuoteSliceQuote &q = quotes[r];
      const QuoteSliceRow &row = fit.rows[r];
      if (!(q.side == Side::Call || q.side == Side::Put)) continue;
      const auto served = served_quote_slice_price(fit, q.strike, q.side);
      const bool priced = row.evaluation_status == QuoteSliceEvaluationStatus::Success ||
                          row.evaluation_status == QuoteSliceEvaluationStatus::PriceOnly;
      if (row.evaluation_status == QuoteSliceEvaluationStatus::MarketDistanceFailure) {
        ++report_.withheld_rows;
        if (served) add(QuoteSlicePublicationViolation::Support, s, q.side, q.strike, 0.0, r);
        continue;
      }
      if (!priced) continue;
      if (!served || *served != row.fitted_price) {
        add(QuoteSlicePublicationViolation::Support, s, q.side, q.strike,
            served ? std::abs(*served - row.fitted_price) : 0.0, r);
        continue;
      }
      const double d = market_distance_spreads(*served, q.bid, q.ask, config_.box_tick);
      if (std::isfinite(config_.market_distance_spreads) && d > config_.market_distance_spreads +
                                                                    1.0e-9) {
        add(QuoteSlicePublicationViolation::QuoteBox, s, q.side, q.strike,
            d - config_.market_distance_spreads, r);
      }
      if (config_.identification && !agrees_with_query(fit, q, row)) {
        add(QuoteSlicePublicationViolation::Identification, s, q.side, q.strike, 0.0, r);
      }
    }
  }

  void calendar_point(std::size_t a, std::size_t b, double k, double tol) {
    const QuoteSliceFit &early = *slices_[a].fit;
    const QuoteSliceFit &late = *slices_[b].fit;
    for (const Side side : {Side::Call, Side::Put}) {
      const auto near = served_quote_slice_price(early, k, side);
      const auto far = served_quote_slice_price(late, k, side);
      if (!near || !far) continue;
      const double crossing = *near - *far - tol;
      if (crossing > 0.0) {
        add(QuoteSlicePublicationViolation::Calendar, a, side, k, crossing,
            kQuoteSlicePublicationAll, b);
      }
    }
  }

  std::span<const QuoteSlicePublicationSlice> slices_;
  const QuoteSlicePublicationConfig &config_;
  QuoteSlicePublicationReport &report_;
};

[[nodiscard]] Status validate_inputs(std::span<const QuoteSlicePublicationSlice> slices,
                                     const QuoteSlicePublicationConfig &config) {
  if (std::isnan(config.market_distance_spreads) || config.market_distance_spreads < 0.0 ||
      !std::isfinite(config.box_tick) || !(config.box_tick > 0.0) || config.dense_points > 64u ||
      (config.focus != kQuoteSlicePublicationAll && config.focus >= slices.size())) {
    return Err(ErrorCode::InvalidArgument, "publication check: invalid configuration");
  }
  std::vector<double> maturities;
  maturities.reserve(slices.size());
  for (const QuoteSlicePublicationSlice &s : slices) {
    if (s.fit == nullptr || !s.fit->prices || s.fit->prices->nodes().size() < 2u ||
        s.fit->rows.size() != s.quotes.size()) {
      return Err(ErrorCode::InvalidArgument,
                 "publication check: every slice needs certified prices and one row per quote");
    }
    maturities.push_back(s.fit->market.calendar_years);
  }
  std::sort(maturities.begin(), maturities.end());
  if (std::adjacent_find(maturities.begin(), maturities.end()) != maturities.end()) {
    return Err(ErrorCode::InvalidArgument, "publication check: distinct maturities required");
  }
  return Ok();
}

} // namespace

Result<QuoteSlicePublicationReport>
validate_published_representation(std::span<const QuoteSlicePublicationSlice> slices,
                                  const QuoteSlicePublicationConfig &config) {
  ATX_TRY_VOID(validate_inputs(slices, config));
  QuoteSlicePublicationReport report;
  Checker check(slices, config, report);
  const bool all = config.focus == kQuoteSlicePublicationAll;
  for (std::size_t s = 0; s < slices.size(); ++s) {
    if (all || s == config.focus) check.slice(s);
  }
  for (std::size_t a = 0; a < slices.size(); ++a) {
    for (std::size_t b = 0; b < slices.size(); ++b) {
      if (a == b || (!all && a != config.focus && b != config.focus)) continue;
      if (slices[a].fit->market.calendar_years < slices[b].fit->market.calendar_years) {
        check.pair(a, b);
      }
    }
  }
  return Ok(std::move(report));
}

} // namespace atx::vol
