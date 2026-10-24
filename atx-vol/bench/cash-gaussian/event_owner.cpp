#include "event_owner.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace atx::vol::cash_gaussian {
namespace {
using atx::core::Err;
using atx::core::Ok;

bool nonnegative(double x) noexcept { return std::isfinite(x) && x >= 0.; }
bool side_valid(Side side) noexcept { return side == Side::Call || side == Side::Put; }
double payoff(double x, double strike, Side side) noexcept {
  return std::max(side == Side::Call ? x-strike : strike-x, 0.);
}

Status append(std::vector<double> &x, std::vector<double> &y, double stock,
              double value, const Budget &budget) {
  if (x.size() >= budget.limits.max_knots)
    return Err(ErrorCode::Unavailable, "exercise node budget");
  if (!nonnegative(stock) || !std::isfinite(value) ||
      (!x.empty() && !(stock > x.back())))
    return Err(ErrorCode::OutOfRange, "exercise knot range");
  x.push_back(stock);
  y.push_back(value);
  return Ok();
}

Result<StockPwl> intrinsic_function(double strike, Side side, Budget &budget) {
  const std::array<double, 2> stock{0., strike};
  const std::array<double, 2> value{side == Side::Call ? 0. : strike, 0.};
  return StockPwl::create(stock, value, side == Side::Call ? 1. : 0., budget);
}

Status validate(const Model &m, std::span<const Event> raw, const Options &o) {
  if (!nonnegative(m.spot) || !std::isfinite(m.strike) || !(m.strike > 0.) ||
      !nonnegative(m.maturity) || !nonnegative(m.sigma) || !std::isfinite(m.rate) ||
      !std::isfinite(m.yield) || !side_valid(m.side) ||
      (m.exercise != ExerciseStyle::European && m.exercise != ExerciseStyle::American))
    return Err(ErrorCode::InvalidArgument, "event model");
  if (m.cash_model != CashModel::Liquidator)
    return Err(ErrorCode::Unavailable, "cash model unsupported");
  if (m.exercise == ExerciseStyle::American && m.side == Side::Call &&
      (m.rate < 0. || m.yield != 0.))
    return Err(ErrorCode::Unavailable, "event call domain");
  if (o.positive_intervals < 2 || o.positive_intervals > 8192 || o.time_base == 0 ||
      o.time_base > 4096 || o.time_level > 12 || o.max_events > 64 ||
      o.max_steps > 4096 || o.limits.max_knots == 0 || o.limits.max_knots > 8192)
    return Err(ErrorCode::InvalidArgument, "event limits");
  if (raw.size() > o.max_events) return Err(ErrorCode::Unavailable, "event count budget");
  for (const auto &e : raw) if (!std::isfinite(e.time) || !nonnegative(e.cash))
    return Err(ErrorCode::InvalidArgument, "cash event");
  return Ok();
}

Result<std::vector<Event>> normalize(const Model &m, std::span<const Event> raw) {
  // Independent adapter; the documented relative expiry window is the only
  // convention shared with the original reference. Exact-date stable grouping
  // preserves original input order when summing coincident cash amounts.
  constexpr double expiry_relative_tolerance = 1e-12;
  const double lower = m.maturity*(1.-expiry_relative_tolerance);
  const double upper = m.maturity*(1.+expiry_relative_tolerance);
  if (!std::isfinite(upper)) return Err(ErrorCode::OutOfRange, "expiry window");
  std::vector<Event> sorted;
  sorted.reserve(raw.size());
  for (const auto &e : raw) if (e.time > 0. && e.time <= upper && e.cash > 0.)
    sorted.push_back({e.time >= lower ? m.maturity : e.time, e.cash});
  std::stable_sort(sorted.begin(), sorted.end(),
      [](const Event &a, const Event &b) { return a.time < b.time; });
  std::vector<Event> events;
  events.reserve(sorted.size());
  for (const auto &e : sorted) {
    if (!events.empty() && events.back().time == e.time) {
      events.back().cash += e.cash;
      if (!std::isfinite(events.back().cash))
        return Err(ErrorCode::OutOfRange, "cash sum");
    } else events.push_back(e);
  }
  return events;
}

Result<std::vector<Step>> clock(const Model &m, std::span<const Event> events,
                                const Options &o) {
  std::vector<double> boundaries{0.};
  for (const auto &e : events) if (e.time < m.maturity) boundaries.push_back(e.time);
  if (m.maturity > 0.) boundaries.push_back(m.maturity);
  std::vector<Step> steps;
  for (std::size_t segment = boundaries.size(); segment > 1; --segment) {
    const double lo = boundaries[segment-2], hi = boundaries[segment-1], length = hi-lo;
    std::size_t count = 1;
    if (m.exercise == ExerciseStyle::American && m.side == Side::Put) {
      const double nominal = std::ceil(static_cast<double>(o.time_base)*length/m.maturity);
      if (!std::isfinite(nominal) || nominal > 4096.)
        return Err(ErrorCode::Unavailable, "exercise step budget");
      count = std::max(std::size_t{1}, static_cast<std::size_t>(nominal));
      count *= std::size_t{1} << o.time_level;
    }
    if (steps.size() > o.max_steps || count > o.max_steps-steps.size())
      return Err(ErrorCode::Unavailable, "exercise step budget");
    double to = hi;
    for (std::size_t j = count; j > 0; --j) {
      const double from = j == 1 ? lo :
          std::lerp(lo, hi, static_cast<double>(j-1)/static_cast<double>(count));
      if (!(to > from)) return Err(ErrorCode::OutOfRange, "exercise clock range");
      double cash = 0.;
      if (j == 1) for (const auto &e : events) if (e.time == from) cash = e.cash;
      steps.push_back({from, to, cash});
      to = from;
    }
  }
  return steps;
}

Result<std::vector<double>> grid(const Model &m, std::span<const Event> events,
                                 const Options &o) {
  std::vector<double> stock;
  if (m.spot == 0. || m.sigma == 0. || m.maturity == 0.) return stock;
  double cash_sum = 0.;
  for (const auto &e : events) cash_sum += e.cash;
  const double width = 8.*m.sigma*std::sqrt(m.maturity);
  const double lo = std::min(m.spot, m.strike)*std::exp(-width);
  const double hi = (std::max(m.spot, m.strike)+cash_sum)*std::exp(width);
  if (!std::isfinite(hi) || !std::isfinite(lo) || !(lo > 0.) || !(hi > lo))
    return Err(ErrorCode::OutOfRange, "stock grid domain");
  stock.reserve(o.positive_intervals+4);
  stock.push_back(0.);
  const double log_lo = std::log(lo), log_hi = std::log(hi);
  for (std::size_t i = 0; i <= o.positive_intervals; ++i) {
    const double x = i == 0 ? lo : i == o.positive_intervals ? hi :
        std::exp(std::lerp(log_lo, log_hi,
            static_cast<double>(i)/static_cast<double>(o.positive_intervals)));
    if (!std::isfinite(x) || !(x > stock.back()))
      return Err(ErrorCode::OutOfRange, "stock grid collapse");
    stock.push_back(x);
  }
  stock.push_back(m.spot);
  stock.push_back(m.strike);
  std::sort(stock.begin(), stock.end());
  stock.erase(std::unique(stock.begin(), stock.end()), stock.end());
  if (stock.size() > o.limits.max_knots)
    return Err(ErrorCode::Unavailable, "stock grid budget");
  return stock;
}

Result<StockPwl> diffuse(const StockPwl &f, const Model &m, std::span<const double> grid,
                         double dt, Work &work) {
  if (m.spot == 0.) {
    const auto value = expectation(f, 0., dt, m.sigma, m.rate, m.yield, work.integral);
    if (!value) return Err(value.error());
    const std::array<double, 1> x{0.}, y{*value};
    return StockPwl::create(x, y, 0., work.integral);
  }
  if (m.sigma == 0.) return deterministic_rollback(f, dt, m.rate, m.yield, work);
  std::vector<double> values;
  values.reserve(grid.size());
  for (const double stock : grid) {
    const auto value = expectation(f, stock, dt, m.sigma, m.rate, m.yield, work.integral);
    if (!value) return Err(value.error());
    values.push_back(*value);
  }
  const double slope = f.right_slope()*std::exp(-m.yield*dt);
  if (!std::isfinite(slope)) return Err(ErrorCode::OutOfRange, "diffusion tail");
  return StockPwl::create(grid, values, slope, work.integral);
}

Status exercise(StockPwl &f, const Model &m, Work &work, const CrossingObserver *observer) {
  if (m.exercise == ExerciseStyle::European) return Ok();
  auto result = exercise_max(f, m.strike, m.side, work, observer);
  if (!result) return Err(result.error());
  f = std::move(*result);
  return Ok();
}

Status jump(StockPwl &f, double cash, Work &work) {
  if (cash == 0.) return Ok();
  auto result = liquidator_jump(f, cash, work.integral);
  if (!result) return Err(result.error());
  f = std::move(*result);
  ++work.jumps;
  return Ok();
}

void observe(const CrossingObserver *observer, const StockPwl &f, double strike,
             Side side, bool tail, double lo, double hi, double difference_lo,
             double difference_hi, double slope_difference, double computed) noexcept {
  if (observer == nullptr || observer->callback == nullptr) return;
  const RoundedCrossing view{f.knots(), f.values(), f.right_slope(), strike, side, tail,
      lo, hi, difference_lo, difference_hi, slope_difference, computed};
  observer->callback(view, observer->context);
}
} // namespace

Result<StockPwl> exercise_max(const StockPwl &f, double strike, Side side, Work &work,
                             const CrossingObserver *observer) {
  if (f.knots().empty() || !std::isfinite(strike) || !(strike > 0.) || !side_valid(side))
    return Err(ErrorCode::InvalidArgument, "exercise input");
  if (f.knots().size() > work.integral.limits.max_knots)
    return Err(ErrorCode::Unavailable, "exercise node budget");
  try {
    std::vector<double> knots(f.knots().begin(), f.knots().end());
    const auto where = std::lower_bound(knots.begin(), knots.end(), strike);
    if (where == knots.end() || *where != strike) knots.insert(where, strike);
    std::vector<double> x, y;
    x.reserve(knots.size()); y.reserve(knots.size());
    double prior = 0., prior_difference = 0.;
    for (const double stock : knots) {
      const auto value = f.value(stock);
      if (!value) return Err(value.error());
      const double intrinsic = payoff(stock, strike, side), difference = *value-intrinsic;
      if (!std::isfinite(difference)) return Err(ErrorCode::OutOfRange, "exercise difference");
      const bool crosses = (difference > 0. && prior_difference < 0.) ||
          (difference < 0. && prior_difference > 0.);
      if (crosses) {
        const double a = std::abs(prior_difference), b = std::abs(difference);
        const double scale = std::max(a, b);
        const double crossing = std::lerp(prior, stock, (a/scale)/(a/scale+b/scale));
        if (!(crossing > prior && crossing < stock)) {
          observe(observer, f, strike, side, false, prior, stock, prior_difference,
                  difference, 0., crossing);
          ++work.rounded_intersections;
          return Err(ErrorCode::OutOfRange, "rounded exercise crossing");
        }
        const auto crossed_value = f.value(crossing);
        if (!crossed_value) return Err(crossed_value.error());
        const auto added = append(x, y, crossing,
            std::max(*crossed_value, payoff(crossing, strike, side)), work.integral);
        if (!added) return Err(added.error());
        ++work.intersections;
      }
      const auto added = append(x, y, stock, std::max(*value, intrinsic), work.integral);
      if (!added) return Err(added.error());
      prior = stock; prior_difference = difference;
    }
    const double intrinsic_slope = side == Side::Call ? 1. : 0.;
    const double slope_difference = f.right_slope()-intrinsic_slope;
    if ((prior_difference > 0. && slope_difference < 0.) ||
        (prior_difference < 0. && slope_difference > 0.)) {
      const double crossing = prior-prior_difference/slope_difference;
      if (!std::isfinite(crossing) || !(crossing > prior)) {
        observe(observer, f, strike, side, true, prior,
                std::numeric_limits<double>::infinity(), prior_difference,
                0., slope_difference, crossing);
        ++work.rounded_intersections;
        return Err(ErrorCode::OutOfRange, "rounded exercise tail");
      }
      const auto value = f.value(crossing);
      if (!value) return Err(value.error());
      const auto added = append(x, y, crossing,
          std::max(*value, payoff(crossing, strike, side)), work.integral);
      if (!added) return Err(added.error());
      ++work.intersections;
    }
    auto result = StockPwl::create(x, y, std::max(f.right_slope(), intrinsic_slope),
                                   work.integral);
    if (result) ++work.exercise_projections;
    return result;
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "exercise allocation");
  }
}

Result<StockPwl> deterministic_rollback(const StockPwl &f, double dt, double rate,
                                       double yield, Work &work) {
  if (f.knots().empty() || !nonnegative(dt) || !std::isfinite(rate) || !std::isfinite(yield))
    return Err(ErrorCode::InvalidArgument, "deterministic input");
  if (dt == 0.) return StockPwl::create(f.knots(), f.values(), f.right_slope(), work.integral);
  if (f.knots().size() > work.integral.limits.max_knots)
    return Err(ErrorCode::Unavailable, "deterministic node budget");
  const double growth = std::exp((rate-yield)*dt), discount = std::exp(-rate*dt);
  if (!std::isfinite(growth) || !(growth > 0.) ||
      !std::isfinite(discount) || !(discount > 0.))
    return Err(ErrorCode::OutOfRange, "deterministic transition");
  try {
    std::vector<double> stock, value;
    stock.reserve(f.knots().size()); value.reserve(f.knots().size());
    for (std::size_t i = 0; i < f.knots().size(); ++i) {
      const double x = f.knots()[i]/growth, y = discount*f.values()[i];
      if (!nonnegative(x) || !std::isfinite(y) ||
          (!stock.empty() && !(x > stock.back())))
        return Err(ErrorCode::OutOfRange, "deterministic knot range");
      stock.push_back(x); value.push_back(y);
    }
    const double slope = discount*growth*f.right_slope();
    if (!std::isfinite(slope)) return Err(ErrorCode::OutOfRange, "deterministic tail");
    auto result = StockPwl::create(stock, value, slope, work.integral);
    if (result) ++work.deterministic_rollbacks;
    return result;
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "deterministic allocation");
  }
}

Result<EventOwner> EventOwner::prepare(const Model &m, std::span<const Event> raw,
                                      const Options &o) {
  if (const auto valid = validate(m, raw, o); !valid) return Err(valid.error());
  try {
    auto events = normalize(m, raw);
    if (!events) return Err(events.error());
    auto steps = clock(m, *events, o);
    if (!steps) return Err(steps.error());
    auto stock = grid(m, *events, o);
    if (!stock) return Err(stock.error());
    EventOwner owner;
    owner.model_ = m; owner.options_ = o;
    owner.raw_.assign(raw.begin(), raw.end());
    owner.events_ = std::move(*events); owner.steps_ = std::move(*steps);
    owner.stock_ = std::move(*stock); owner.prepared_ = true;
    return owner;
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "event allocation");
  }
}

Result<double> EventOwner::price(Work &work, const CrossingObserver *observer) const {
  work = Work{};
  work.integral.limits = options_.limits;
  work.planned_steps = steps_.size(); work.grid_nodes = stock_.size();
  if (!prepared_ || (model_.maturity > 0. && steps_.empty()) ||
      (model_.maturity > 0. && model_.spot > 0. && model_.sigma > 0. && stock_.empty()))
    return Err(ErrorCode::InvalidArgument, "unprepared owner");
  try {
    auto result = intrinsic_function(model_.strike, model_.side, work.integral);
    if (!result) return Err(result.error());
    StockPwl value = std::move(*result);
    if (!events_.empty() && events_.back().time == model_.maturity) {
      if (const auto status = jump(value, events_.back().cash, work); !status)
        return Err(status.error());
      if (const auto status = exercise(value, model_, work, observer); !status)
        return Err(status.error());
    }
    for (const auto &step : steps_) {
      auto prior = diffuse(value, model_, stock_, step.to-step.from, work);
      if (!prior) return Err(prior.error());
      value = std::move(*prior);
      if (const auto status = exercise(value, model_, work, observer); !status)
        return Err(status.error());
      if (step.cash_at_from > 0.) {
        if (const auto status = jump(value, step.cash_at_from, work); !status)
          return Err(status.error());
        if (const auto status = exercise(value, model_, work, observer); !status)
          return Err(status.error());
      }
      ++work.completed_steps;
    }
    return value.value(model_.spot);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "rollback allocation");
  }
}
} // namespace atx::vol::cash_gaussian
