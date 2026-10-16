#include "atx/vol/api/pricing/american_markov.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace atx::vol {
namespace {
using atx::core::Err;
using atx::core::Ok;

bool positive(double x) noexcept { return x > 0.0 && std::isfinite(x); }
bool nonnegative(double x) noexcept { return x >= 0.0 && std::isfinite(x); }

Status validate_local_volatility(const AmericanMarkovInterval &interval) {
  const auto nodes = interval.local_volatility;
  if (nodes.size() > kAmericanMarkovMaxLocalVolNodes) {
    return Err(ErrorCode::OutOfRange, "American Markov: local volatility node limit exceeded");
  }
  double previous = -std::numeric_limits<double>::infinity();
  for (const auto &node : nodes) {
    if (!std::isfinite(node.log_state_over_spot) ||
        std::abs(node.log_state_over_spot) > kAmericanMarkovMaxLocalVolLogState ||
        !(node.log_state_over_spot > previous) || !nonnegative(node.volatility_multiplier)) {
      return Err(ErrorCode::InvalidArgument, "American Markov: invalid local volatility nodes");
    }
    if (!nonnegative(interval.volatility * node.volatility_multiplier)) {
      return Err(ErrorCode::OutOfRange, "American Markov: local volatility product overflow");
    }
    previous = node.log_state_over_spot;
  }
  return Ok();
}

Status validate(const AmericanMarkovSpec &s) {
  const auto &c = s.config;
  if (!positive(s.spot) || s.intervals.empty() || c.max_states < 3 ||
      c.max_states > kAmericanMarkovMaxStates || c.max_transitions == 0 ||
      c.max_transitions > kAmericanMarkovMaxTransitions || c.max_stored_rows == 0 ||
      c.max_stored_rows > kAmericanMarkovMaxStoredRows || !positive(c.max_calendar_step) ||
      c.max_stored_volatility_values == 0 ||
      c.max_stored_volatility_values > kAmericanMarkovMaxStoredVolatilityValues ||
      !positive(c.max_variance_step) || !positive(c.cfl_fraction) || c.cfl_fraction > 1.0 ||
      s.intervals.size() > c.max_transitions) {
    return Err(ErrorCode::InvalidArgument,
               "American Markov: invalid market or resource configuration");
  }
  if (s.state_grid.empty()) {
    if (c.grid_nodes < 3 || c.grid_nodes > c.max_states || !positive(c.upper_spot_multiple) ||
        c.upper_spot_multiple <= 1.0 || !positive(s.spot * c.upper_spot_multiple)) {
      return Err(ErrorCode::InvalidArgument, "American Markov: invalid generated state grid");
    }
  } else {
    if (s.state_grid.size() < 3 || s.state_grid.size() > c.max_states ||
        s.state_grid.front() != 0.0 || s.state_grid.back() <= s.spot) {
      return Err(ErrorCode::InvalidArgument,
                 "American Markov: grid must include zero, spot and an upper state");
    }
    bool has_spot = false;
    double previous = -1.0;
    for (const double x : s.state_grid) {
      if (!nonnegative(x) || !(x > previous)) {
        return Err(ErrorCode::InvalidArgument,
                   "American Markov: state grid must be finite and strictly increasing");
      }
      has_spot = has_spot || x == s.spot;
      previous = x;
    }
    if (!has_spot) {
      return Err(ErrorCode::InvalidArgument,
                 "American Markov: initial spot must be an exact state");
    }
  }
  double calendar = 0.0;
  double variance = 0.0;
  for (const auto &interval : s.intervals) {
    if (!positive(interval.calendar_end) || !(interval.calendar_end > calendar) ||
        !nonnegative(interval.variance_end) || interval.variance_end < variance ||
        !std::isfinite(interval.rate) || !std::isfinite(interval.continuous_yield) ||
        !nonnegative(interval.volatility) || !nonnegative(interval.cash_dividend_at_end)) {
      return Err(ErrorCode::InvalidArgument,
                 "American Markov: invalid interval, clock or dividend");
    }
    const auto local_valid = validate_local_volatility(interval);
    if (!local_valid) {
      return local_valid;
    }
    calendar = interval.calendar_end;
    variance = interval.variance_end;
  }
  return Ok();
}

double local_volatility(const AmericanMarkovInterval &interval, double state,
                        double log_spot) noexcept {
  const auto nodes = interval.local_volatility;
  if (nodes.empty()) {
    return interval.volatility;
  }
  // Subtract logs to avoid over/underflow in state/spot at extreme stock scales.
  const double coordinate = state == 0.0 ? -std::numeric_limits<double>::infinity()
                                         : std::log(state) - log_spot;
  if (coordinate <= nodes.front().log_state_over_spot) {
    return interval.volatility * nodes.front().volatility_multiplier;
  }
  if (coordinate >= nodes.back().log_state_over_spot) {
    return interval.volatility * nodes.back().volatility_multiplier;
  }
  const auto high = std::lower_bound(
      nodes.begin(), nodes.end(), coordinate,
      [](const AmericanMarkovLocalVolNode &node, double x) {
        return node.log_state_over_spot < x;
      });
  const auto &low = *(high - 1);
  const double fraction = (coordinate - low.log_state_over_spot) /
                          (high->log_state_over_spot - low.log_state_over_spot);
  return interval.volatility *
         std::lerp(low.volatility_multiplier, high->volatility_multiplier, fraction);
}

// Exit probability per unit variance time. Ratios avoid squaring stock levels,
// so an otherwise identical grid has the same transition at any stock scale.
double exit_rate(std::span<const double> x, std::size_t i, double sigma) noexcept {
  if (sigma == 0.0 || i == 0 || i + 1 == x.size()) {
    return 0.0;
  }
  const double lower = x[i] - x[i - 1];
  const double upper = x[i + 1] - x[i];
  return (sigma * (x[i] / lower)) * (sigma * (x[i] / upper));
}

double intrinsic(double stock, double strike, Side side) noexcept {
  return std::max(side == Side::Call ? stock - strike : strike - stock, 0.0);
}
} // namespace

Result<AmericanMarkovModel> compile_american_markov(const AmericanMarkovSpec &spec) {
  const auto valid = validate(spec);
  if (!valid) {
    return Err(valid.error());
  }
  const auto &cfg = spec.config;
  AmericanMarkovModel model;
  model.spot_ = spec.spot;
  AmericanMarkovLayer initial;
  if (spec.state_grid.empty()) {
    initial.states.reserve(cfg.grid_nodes + 1);
    const double upper = spec.spot * cfg.upper_spot_multiple;
    for (std::size_t i = 0; i < cfg.grid_nodes; ++i) {
      initial.states.push_back(upper *
                               (static_cast<double>(i) / static_cast<double>(cfg.grid_nodes - 1)));
    }
    const auto spot = std::lower_bound(initial.states.begin(), initial.states.end(), spec.spot);
    if (spot == initial.states.end() || *spot != spec.spot) {
      initial.states.insert(spot, spec.spot);
    }
    if (initial.states.size() > cfg.max_states) {
      return Err(ErrorCode::OutOfRange, "American Markov: inserting spot exceeds the state limit");
    }
    for (std::size_t i = 1; i < initial.states.size(); ++i) {
      if (!(initial.states[i] > initial.states[i - 1])) {
        return Err(ErrorCode::OutOfRange,
                   "American Markov: generated grid loses floating-point resolution");
      }
    }
  } else {
    initial.states.assign(spec.state_grid.begin(), spec.state_grid.end());
  }
  model.initial_state_index_ = static_cast<std::size_t>(
      std::lower_bound(initial.states.begin(), initial.states.end(), spec.spot) -
      initial.states.begin());
  model.diagnostics_.max_states = initial.states.size();
  model.layers_.push_back(std::move(initial));
  model.expiry_layers_.reserve(spec.intervals.size());
  model.interval_data_.reserve(spec.intervals.size());
  const double log_spot = std::log(spec.spot);
  double log_growth = 0.0;

  for (const auto &interval : spec.intervals) {
    const auto &start = model.layers_.back();
    const double calendar_start = start.calendar_time;
    const double variance_start = start.variance_time;
    const double calendar_delta = interval.calendar_end - calendar_start;
    const double variance_delta = interval.variance_end - variance_start;
    const double carry = interval.rate - interval.continuous_yield;
    const double interval_log_growth = carry * calendar_delta;
    const double next_log_growth = log_growth + interval_log_growth;
    if (!std::isfinite(carry) || !std::isfinite(next_log_growth) ||
        !positive(std::exp(next_log_growth)) ||
        !positive(std::exp(-interval.rate * calendar_delta))) {
      return Err(ErrorCode::OutOfRange, "American Markov: carry or discount overflow");
    }
    const std::size_t volatility_values = start.states.size() + interval.local_volatility.size();
    if (volatility_values >
        cfg.max_stored_volatility_values - model.diagnostics_.stored_volatility_values) {
      return Err(ErrorCode::OutOfRange, "American Markov: stored volatility value limit exceeded");
    }
    AmericanMarkovIntervalData interval_data;
    interval_data.first_layer = model.layers_.size() - 1;
    interval_data.volatility = interval.volatility;
    interval_data.local_volatility.assign(interval.local_volatility.begin(),
                                          interval.local_volatility.end());
    interval_data.state_volatilities.reserve(start.states.size());
    std::vector<double> state_exit_rates;
    state_exit_rates.reserve(start.states.size());
    double max_exit = 0.0;
    for (std::size_t i = 0; i < start.states.size(); ++i) {
      const double sigma = local_volatility(interval, start.states[i], log_spot);
      if (!nonnegative(sigma)) {
        return Err(ErrorCode::OutOfRange, "American Markov: local volatility sampling overflow");
      }
      const double value =
          variance_delta == 0.0 ? 0.0 : exit_rate(start.states, i, sigma);
      if (!nonnegative(value)) {
        return Err(ErrorCode::OutOfRange, "American Markov: state grid cannot resolve diffusion");
      }
      interval_data.state_volatilities.push_back(sigma);
      state_exit_rates.push_back(value);
      max_exit = std::max(max_exit, value);
    }
    // A small arithmetic margin prevents cfl_fraction==1 from manufacturing a
    // negative stay probability through rounded division at the boundary.
    const double cfl = cfg.cfl_fraction * (1.0 - 32.0 * std::numeric_limits<double>::epsilon());
    const double required = std::max({1.0, std::ceil(calendar_delta / cfg.max_calendar_step),
                                      std::ceil(variance_delta / cfg.max_variance_step),
                                      std::ceil(variance_delta * max_exit / cfl)});
    const std::size_t cash_steps = interval.cash_dividend_at_end > 0.0 ? 1U : 0U;
    const std::size_t remaining_steps = cfg.max_transitions - model.transitions_.size();
    if (!std::isfinite(required) || remaining_steps < cash_steps ||
        required > static_cast<double>(remaining_steps - cash_steps)) {
      return Err(ErrorCode::OutOfRange, "American Markov: expanded transition limit exceeded");
    }
    const auto count = static_cast<std::size_t>(required);
    const std::size_t state_count = start.states.size();
    const std::size_t remaining_rows = cfg.max_stored_rows - model.diagnostics_.stored_rows;
    if (count + cash_steps > remaining_rows / state_count) {
      return Err(ErrorCode::OutOfRange, "American Markov: stored transition row limit exceeded");
    }
    model.interval_data_.push_back(std::move(interval_data));
    model.diagnostics_.stored_volatility_values += volatility_values;
    const std::size_t interval_index = model.interval_data_.size() - 1;

    for (std::size_t step = 1; step <= count; ++step) {
      const auto &before = model.layers_.back();
      const double fraction = static_cast<double>(step) / static_cast<double>(count);
      AmericanMarkovLayer after;
      after.calendar_time = step == count
                                ? interval.calendar_end
                                : std::lerp(calendar_start, interval.calendar_end, fraction);
      after.variance_time = step == count
                                ? interval.variance_end
                                : std::lerp(variance_start, interval.variance_end, fraction);
      after.growth = std::exp(log_growth + carry * (after.calendar_time - calendar_start));
      after.states = before.states;
      AmericanMarkovTransition transition;
      transition.interval_index = interval_index;
      transition.calendar_step = after.calendar_time - before.calendar_time;
      transition.variance_step = after.variance_time - before.variance_time;
      transition.volatility = interval.volatility;
      transition.discount = std::exp(-interval.rate * transition.calendar_step);
      if (!positive(transition.calendar_step) || !nonnegative(transition.variance_step) ||
          !positive(after.growth) || !positive(transition.discount) ||
          !nonnegative(after.growth * after.states.back())) {
        return Err(ErrorCode::OutOfRange,
                   "American Markov: time or stock grid loses numerical range");
      }
      transition.rows.resize(state_count);
      for (std::size_t i = 0; i < state_count; ++i) {
        auto &row = transition.rows[i];
        row.destination[0] = static_cast<std::uint32_t>(i);
        row.probability[0] = 1.0;
        if (i > 0 && i + 1 < state_count) {
          const double lower = before.states[i] - before.states[i - 1];
          const double upper = before.states[i + 1] - before.states[i];
          const double exit =
              transition.variance_step == 0.0
                  ? 0.0
                  : transition.variance_step * state_exit_rates[i];
          const double down = exit * (upper / (lower + upper));
          const double up = exit * (lower / (lower + upper));
          const double stay = 1.0 - (down + up);
          if (!nonnegative(down) || !nonnegative(up) || !nonnegative(stay)) {
            return Err(
                ErrorCode::OutOfRange,
                "American Markov: rounded time step violates positive transition constraint");
          }
          row.destination = {static_cast<std::uint32_t>(i - 1), static_cast<std::uint32_t>(i),
                             static_cast<std::uint32_t>(i + 1)};
          row.probability = {down, stay, up};
        }
        double sum = 0.0;
        double mean = 0.0;
        const double mean_scale = std::max(before.states[i], spec.spot);
        for (std::size_t j = 0; j < 3; ++j) {
          sum += row.probability[j];
          mean += (row.probability[j] * after.states[row.destination[j]]) / mean_scale;
        }
        model.diagnostics_.max_probability_sum_error =
            std::max(model.diagnostics_.max_probability_sum_error, std::abs(sum - 1.0));
        model.diagnostics_.max_relative_mean_error =
            std::max(model.diagnostics_.max_relative_mean_error,
                     std::abs(mean - before.states[i] / mean_scale));
      }
      model.diagnostics_.stored_rows += state_count;
      ++model.diagnostics_.diffusion_steps;
      model.transitions_.push_back(std::move(transition));
      model.layers_.push_back(std::move(after));
    }
    log_growth = next_log_growth;

    if (cash_steps != 0U) {
      const auto &before = model.layers_.back();
      AmericanMarkovLayer after;
      after.calendar_time = before.calendar_time;
      after.variance_time = before.variance_time;
      after.growth = before.growth;
      after.states.reserve(state_count);
      AmericanMarkovTransition transition;
      transition.interval_index = interval_index;
      transition.kind = AmericanMarkovStepKind::CashDividend;
      transition.cash_dividend = interval.cash_dividend_at_end;
      transition.rows.resize(state_count);
      const bool wipes_grid = interval.cash_dividend_at_end >= before.growth * before.states.back();
      const double cash_x = wipes_grid ? 0.0 : interval.cash_dividend_at_end / before.growth;
      if (!nonnegative(cash_x)) {
        return Err(ErrorCode::OutOfRange, "American Markov: dividend conversion overflow");
      }
      for (std::size_t i = 0; i < state_count; ++i) {
        const double post = wipes_grid ? 0.0 : std::max(before.states[i] - cash_x, 0.0);
        if (after.states.empty() || post != after.states.back()) {
          after.states.push_back(post);
        }
        transition.rows[i].destination[0] = static_cast<std::uint32_t>(after.states.size() - 1);
        transition.rows[i].probability[0] = 1.0;
      }
      model.diagnostics_.stored_rows += state_count;
      ++model.diagnostics_.cash_steps;
      model.transitions_.push_back(std::move(transition));
      model.layers_.push_back(std::move(after));
    }
    model.expiry_layers_.push_back(model.layers_.size() - 1);
  }

  // Forward probability propagation reports numerical boundary exposure using
  // the SAME rows as pricing. This is compilation work, not serving work.
  std::vector<double> mass(model.diagnostics_.max_states, 0.0);
  std::vector<double> next_mass(model.diagnostics_.max_states, 0.0);
  mass[model.initial_state_index_] = 1.0;
  for (const auto &transition : model.transitions_) {
    std::fill(next_mass.begin(), next_mass.end(), 0.0);
    for (std::size_t i = 0; i < transition.rows.size(); ++i) {
      const auto &row = transition.rows[i];
      for (std::size_t j = 0; j < 3; ++j) {
        next_mass[row.destination[j]] += mass[i] * row.probability[j];
      }
    }
    mass.swap(next_mass);
  }
  model.diagnostics_.terminal_zero_probability = mass.front();
  model.diagnostics_.terminal_upper_probability =
      model.layers_.back().states.size() > 1 ? mass[model.layers_.back().states.size() - 1] : 0.0;
  return Ok(std::move(model));
}

Status AmericanMarkovWorkspace::prepare(const AmericanMarkovModel &model) {
  if (model.layers().empty()) {
    return Err(ErrorCode::InvalidArgument, "American Markov: moved-from model");
  }
  try {
    first_.resize(model.diagnostics().max_states);
    second_.resize(model.diagnostics().max_states);
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::Unavailable, "American Markov: workspace allocation failed");
  }
  return Ok();
}

Result<double> price_american_markov(const AmericanMarkovModel &model, double strike, Side side,
                                     std::size_t expiry_index, AmericanMarkovWorkspace &workspace,
                                     ExerciseStyle exercise) {
  if (!nonnegative(strike) || (side != Side::Call && side != Side::Put) ||
      (exercise != ExerciseStyle::American && exercise != ExerciseStyle::European) ||
      model.layers().empty() || expiry_index >= model.expiry_layers().size()) {
    return Err(ErrorCode::InvalidArgument, "American Markov: invalid option or expiry prefix");
  }
  if (workspace.first_.size() < model.diagnostics().max_states ||
      workspace.second_.size() < model.diagnostics().max_states) {
    return Err(ErrorCode::InvalidArgument,
               "American Markov: workspace must be prepared before pricing");
  }
  const auto terminal = model.expiry_layers()[expiry_index];
  const auto &end = model.layers()[terminal];
  auto *values = &workspace.first_;
  auto *scratch = &workspace.second_;
  for (std::size_t i = 0; i < end.states.size(); ++i) {
    (*values)[i] = intrinsic(end.growth * end.states[i], strike, side);
  }
  for (std::size_t n = terminal; n > 0; --n) {
    const auto &transition = model.transitions()[n - 1];
    const auto &layer = model.layers()[n - 1];
    for (std::size_t i = 0; i < transition.rows.size(); ++i) {
      const auto &row = transition.rows[i];
      double continuation = 0.0;
      for (std::size_t j = 0; j < 3; ++j) {
        continuation += row.probability[j] * (*values)[row.destination[j]];
      }
      continuation *= transition.discount;
      if (!nonnegative(continuation)) {
        return Err(ErrorCode::OutOfRange, "American Markov: option value overflow during rollback");
      }
      (*scratch)[i] =
          exercise == ExerciseStyle::American
              ? std::max(continuation, intrinsic(layer.growth * layer.states[i], strike, side))
              : continuation;
    }
    std::swap(values, scratch);
  }
  return Ok((*values)[model.initial_state_index()]);
}

} // namespace atx::vol
