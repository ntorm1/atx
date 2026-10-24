#include "atx/vol/api/pricing/american_markov.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <utility>

#include "pricing/american_markov_transition.hpp"

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
      c.max_transition_work == 0 || c.max_transition_work > kAmericanMarkovMaxTransitionWork ||
      (c.diffusion_method != AmericanMarkovDiffusionMethod::ExplicitEuler &&
       c.diffusion_method != AmericanMarkovDiffusionMethod::VarianceResolvent) ||
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

Result<std::vector<AmericanMarkovResolventFactor>> make_resolvent_factors(
    std::span<const double> states, std::span<const double> exits, double h, double spot,
    AmericanMarkovDiagnostics &diagnostics) {
  std::vector<AmericanMarkovResolventFactor> factors(states.size());
  double previous_beta = 1.0;
  for (std::size_t i = 0; i < states.size(); ++i) {
    double down = 0.0;
    double up = 0.0;
    if (i > 0 && i + 1 < states.size()) {
      const double lower = states[i] - states[i - 1];
      const double upper = states[i + 1] - states[i];
      down = h * (exits[i] * (upper / (lower + upper)));
      up = h * (exits[i] * (lower / (lower + upper)));
    }
    const double s = 1.0 + down * previous_beta;
    const double pivot = s + up;
    const double alpha = 1.0 / s;
    const double beta = s / pivot;
    if (!nonnegative(down) || !nonnegative(up) || !positive(s) || !positive(pivot) ||
        !positive(alpha) || alpha > 1.0 || !positive(beta) || beta > 1.0) {
      return Err(ErrorCode::OutOfRange, "American Markov: resolvent factors lose numerical range");
    }
    factors[i] = {alpha, beta};
    previous_beta = beta;
  }
  std::vector<double> input(states.size(), 1.0);
  std::vector<double> output(states.size());
  if (!detail::resolvent_backward(factors, input, output)) {
    return Err(ErrorCode::OutOfRange, "American Markov: nonfinite resolvent constant action");
  }
  for (const double value : output) {
    const double error = std::abs(value - 1.0);
    if (error > kAmericanMarkovResolventMaxActionError) {
      return Err(ErrorCode::OutOfRange, "American Markov: resolvent probability residual exceeded");
    }
    diagnostics.max_probability_sum_error = std::max(diagnostics.max_probability_sum_error, error);
  }
  // Probe represented stock levels directly. Global normalization can round a
  // small positive state to a subnormal and conceal material relative drift.
  // Convex lerps keep these finite nonnegative inputs within their input range.
  std::copy(states.begin(), states.end(), input.begin());
  if (!detail::resolvent_backward(factors, input, output)) {
    return Err(ErrorCode::OutOfRange, "American Markov: nonfinite resolvent stock action");
  }
  for (std::size_t i = 0; i < states.size(); ++i) {
    const double error = std::abs(output[i] - input[i]) / std::max(input[i], spot);
    if (!nonnegative(error) || error > kAmericanMarkovResolventMaxActionError) {
      return Err(ErrorCode::OutOfRange, "American Markov: resolvent martingale residual exceeded");
    }
    diagnostics.max_relative_mean_error = std::max(diagnostics.max_relative_mean_error, error);
  }
  return Ok(std::move(factors));
}
} // namespace

Status apply_american_markov_transition(const AmericanMarkovTransition &transition,
                                        std::span<const double> input, std::span<double> output,
                                        AmericanMarkovTransitionAction action) {
  const auto source = transition.source_state_count();
  const auto target = transition.target_state_count();
  const bool forward = action == AmericanMarkovTransitionAction::Forward;
  if (source == 0 || target == 0 ||
      (action != AmericanMarkovTransitionAction::Backward && !forward) ||
      (transition.kind != AmericanMarkovStepKind::Diffusion &&
       transition.kind != AmericanMarkovStepKind::CashDividend &&
       transition.kind != AmericanMarkovStepKind::VarianceResolvent) ||
      input.size() != (forward ? source : target) ||
      output.size() != (forward ? target : source)) {
    return Err(ErrorCode::InvalidArgument, "American Markov: invalid transition action shape");
  }
  const std::less<const double *> before;
  if (before(input.data(), output.data() + output.size()) &&
      before(output.data(), input.data() + input.size())) {
    return Err(ErrorCode::InvalidArgument, "American Markov: transition action buffers overlap");
  }
  if (!std::all_of(input.begin(), input.end(), [](double x) { return std::isfinite(x); })) {
    return Err(ErrorCode::InvalidArgument, "American Markov: nonfinite transition action input");
  }
  bool finite = true;
  if (transition.kind == AmericanMarkovStepKind::VarianceResolvent) {
    if (source != target) {
      return Err(ErrorCode::InvalidArgument, "American Markov: invalid resolvent action shape");
    }
    finite = forward ? detail::resolvent_transpose(transition.resolvent_factors(), input, output)
                     : detail::resolvent_backward(transition.resolvent_factors(), input, output);
  } else {
    const auto rows = transition.rows();
    std::fill(output.begin(), output.end(), 0.0);
    for (std::size_t i = 0; i < rows.size(); ++i) {
      for (std::size_t j = 0; j < 3; ++j) {
        const auto destination = rows[i].destination[j];
        if (forward) output[destination] += input[i] * rows[i].probability[j];
        else output[i] += rows[i].probability[j] * input[destination];
      }
    }
    finite = std::all_of(output.begin(), output.end(), [](double x) { return std::isfinite(x); });
  }
  if (!finite) return Err(ErrorCode::OutOfRange, "American Markov: transition action overflow");
  return Ok();
}

Result<AmericanMarkovModel> compile_american_markov(const AmericanMarkovSpec &spec) {
  const auto valid = validate(spec);
  if (!valid) {
    return Err(valid.error());
  }
  const auto &cfg = spec.config;
  const bool resolvent = cfg.diffusion_method == AmericanMarkovDiffusionMethod::VarianceResolvent;
  AmericanMarkovModel model;
  model.spot_ = spec.spot;
  model.method_ = cfg.diffusion_method;
  AmericanMarkovLayer initial;
  std::vector<double> initial_states;
  if (spec.state_grid.empty()) {
    initial_states.reserve(cfg.grid_nodes + 1);
    const double upper = spec.spot * cfg.upper_spot_multiple;
    for (std::size_t i = 0; i < cfg.grid_nodes; ++i) {
      initial_states.push_back(upper *
                               (static_cast<double>(i) / static_cast<double>(cfg.grid_nodes - 1)));
    }
    const auto spot = std::lower_bound(initial_states.begin(), initial_states.end(), spec.spot);
    if (spot == initial_states.end() || *spot != spec.spot) {
      initial_states.insert(spot, spec.spot);
    }
    if (initial_states.size() > cfg.max_states) {
      return Err(ErrorCode::OutOfRange, "American Markov: inserting spot exceeds the state limit");
    }
    for (std::size_t i = 1; i < initial_states.size(); ++i) {
      if (!(initial_states[i] > initial_states[i - 1])) {
        return Err(ErrorCode::OutOfRange,
                   "American Markov: generated grid loses floating-point resolution");
      }
    }
  } else {
    initial_states.assign(spec.state_grid.begin(), spec.state_grid.end());
  }
  model.initial_state_index_ = static_cast<std::size_t>(
      std::lower_bound(initial_states.begin(), initial_states.end(), spec.spot) -
      initial_states.begin());
  model.diagnostics_.max_states = initial_states.size();
  model.diagnostics_.physical_state_values = initial_states.size();
  model.diagnostics_.state_grid_blocks = 1;
  initial.states_ = std::make_shared<const std::vector<double>>(std::move(initial_states));
  model.layers_.push_back(std::move(initial));
  model.expiry_layers_.reserve(spec.intervals.size());
  model.interval_data_.reserve(spec.intervals.size());
  const double log_spot = std::log(spec.spot);
  double log_growth = 0.0;

  for (const auto &interval : spec.intervals) {
    const auto &start = model.layers_.back();
    const auto states = start.states();
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
    const std::size_t volatility_values = states.size() + interval.local_volatility.size();
    if (volatility_values >
        cfg.max_stored_volatility_values - model.diagnostics_.stored_volatility_values) {
      return Err(ErrorCode::OutOfRange, "American Markov: stored volatility value limit exceeded");
    }
    AmericanMarkovIntervalData interval_data;
    interval_data.first_layer = model.layers_.size() - 1;
    interval_data.volatility = interval.volatility;
    interval_data.local_volatility.assign(interval.local_volatility.begin(),
                                          interval.local_volatility.end());
    interval_data.state_volatilities.reserve(states.size());
    std::vector<double> state_exit_rates;
    state_exit_rates.reserve(states.size());
    double max_exit = 0.0;
    for (std::size_t i = 0; i < states.size(); ++i) {
      const double sigma = local_volatility(interval, states[i], log_spot);
      if (!nonnegative(sigma)) {
        return Err(ErrorCode::OutOfRange, "American Markov: local volatility sampling overflow");
      }
      const double value =
          variance_delta == 0.0 ? 0.0 : exit_rate(states, i, sigma);
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
                                      resolvent ? 0.0
                                                : std::ceil(variance_delta * max_exit / cfl)});
    const std::size_t cash_steps = interval.cash_dividend_at_end > 0.0 ? 1U : 0U;
    const std::size_t remaining_steps = cfg.max_transitions - model.transitions_.size();
    if (!std::isfinite(required) || remaining_steps < cash_steps ||
        required > static_cast<double>(remaining_steps - cash_steps)) {
      return Err(ErrorCode::OutOfRange, "American Markov: expanded transition limit exceeded");
    }
    const auto count = static_cast<std::size_t>(required);
    const std::size_t state_count = states.size();
    const std::size_t remaining_rows = cfg.max_stored_rows - model.diagnostics_.stored_rows;
    if (count + cash_steps > remaining_rows / state_count) {
      return Err(ErrorCode::OutOfRange, "American Markov: stored transition row limit exceeded");
    }
    const std::size_t step_work = state_count * (resolvent ? 2U : 1U);
    const std::size_t cash_work = cash_steps * state_count;
    const std::size_t remaining_work = cfg.max_transition_work - model.diagnostics_.transition_work;
    if (cash_work > remaining_work || count > (remaining_work - cash_work) / step_work) {
      return Err(ErrorCode::OutOfRange, "American Markov: transition action work limit exceeded");
    }
    model.interval_data_.push_back(std::move(interval_data));
    model.diagnostics_.stored_volatility_values += volatility_values;
    const std::size_t interval_index = model.interval_data_.size() - 1;
    // Row probabilities depend only on this interval's fixed grid/local-vol
    // samples and the EXACT represented variance step. Calendar discounts and
    // growth stay per-transition. Never quantize steps or share across intervals.
    std::map<std::uint64_t, std::shared_ptr<const std::vector<AmericanMarkovTransitionRow>>>
        row_cache;
    std::map<std::uint64_t, std::shared_ptr<const std::vector<AmericanMarkovResolventFactor>>>
        factor_cache;

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
      after.states_ = before.states_;
      AmericanMarkovTransition transition;
      transition.interval_index = interval_index;
      transition.target_states_ = state_count;
      transition.calendar_step = after.calendar_time - before.calendar_time;
      transition.variance_step = after.variance_time - before.variance_time;
      transition.volatility = interval.volatility;
      transition.discount = std::exp(-interval.rate * transition.calendar_step);
      if (!positive(transition.calendar_step) || !nonnegative(transition.variance_step) ||
          !positive(after.growth) || !positive(transition.discount) ||
          !nonnegative(after.growth * states.back())) {
        return Err(ErrorCode::OutOfRange,
                   "American Markov: time or stock grid loses numerical range");
      }
      const auto key = std::bit_cast<std::uint64_t>(transition.variance_step);
      const auto cached = row_cache.find(key);
      if (resolvent) {
        transition.kind = AmericanMarkovStepKind::VarianceResolvent;
        const auto prior = factor_cache.find(key);
        if (prior != factor_cache.end()) {
          transition.factors_ = prior->second;
        } else {
          auto factors = make_resolvent_factors(states, state_exit_rates, transition.variance_step,
                                                spec.spot, model.diagnostics_);
          if (!factors) return Err(factors.error());
          transition.factors_ = std::make_shared<
              const std::vector<AmericanMarkovResolventFactor>>(std::move(*factors));
          factor_cache.emplace(key, transition.factors_);
          model.diagnostics_.physical_resolvent_factors += state_count;
          ++model.diagnostics_.resolvent_factor_blocks;
        }
        ++model.diagnostics_.resolvent_steps;
      } else if (cached != row_cache.end()) {
        transition.rows_ = cached->second;
      } else {
        std::vector<AmericanMarkovTransitionRow> rows(state_count);
        for (std::size_t i = 0; i < state_count; ++i) {
          auto &row = rows[i];
          row.destination[0] = static_cast<std::uint32_t>(i);
          row.probability[0] = 1.0;
          if (i > 0 && i + 1 < state_count) {
            const double lower = states[i] - states[i - 1];
            const double upper = states[i + 1] - states[i];
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
          const double mean_scale = std::max(states[i], spec.spot);
          for (std::size_t j = 0; j < 3; ++j) {
            sum += row.probability[j];
            mean += (row.probability[j] * states[row.destination[j]]) / mean_scale;
          }
          model.diagnostics_.max_probability_sum_error =
              std::max(model.diagnostics_.max_probability_sum_error, std::abs(sum - 1.0));
          model.diagnostics_.max_relative_mean_error =
              std::max(model.diagnostics_.max_relative_mean_error,
                       std::abs(mean - states[i] / mean_scale));
        }
        transition.rows_ =
            std::make_shared<const std::vector<AmericanMarkovTransitionRow>>(std::move(rows));
        row_cache.emplace(key, transition.rows_);
        model.diagnostics_.physical_transition_rows += state_count;
        ++model.diagnostics_.transition_row_blocks;
      }
      model.diagnostics_.stored_rows += state_count;
      model.diagnostics_.transition_work += step_work;
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
      std::vector<double> after_states;
      after_states.reserve(state_count);
      AmericanMarkovTransition transition;
      transition.interval_index = interval_index;
      transition.kind = AmericanMarkovStepKind::CashDividend;
      transition.cash_dividend = interval.cash_dividend_at_end;
      std::vector<AmericanMarkovTransitionRow> rows(state_count);
      const bool wipes_grid = interval.cash_dividend_at_end >= before.growth * states.back();
      const double cash_x = wipes_grid ? 0.0 : interval.cash_dividend_at_end / before.growth;
      if (!nonnegative(cash_x)) {
        return Err(ErrorCode::OutOfRange, "American Markov: dividend conversion overflow");
      }
      for (std::size_t i = 0; i < state_count; ++i) {
        const double post = wipes_grid ? 0.0 : std::max(states[i] - cash_x, 0.0);
        if (after_states.empty() || post != after_states.back()) {
          after_states.push_back(post);
        }
        rows[i].destination[0] = static_cast<std::uint32_t>(after_states.size() - 1);
        rows[i].probability[0] = 1.0;
      }
      model.diagnostics_.physical_state_values += after_states.size();
      transition.target_states_ = after_states.size();
      ++model.diagnostics_.state_grid_blocks;
      model.diagnostics_.physical_transition_rows += state_count;
      ++model.diagnostics_.transition_row_blocks;
      after.states_ = std::make_shared<const std::vector<double>>(std::move(after_states));
      transition.rows_ =
          std::make_shared<const std::vector<AmericanMarkovTransitionRow>>(std::move(rows));
      model.diagnostics_.stored_rows += state_count;
      model.diagnostics_.transition_work += state_count;
      ++model.diagnostics_.cash_steps;
      model.transitions_.push_back(std::move(transition));
      model.layers_.push_back(std::move(after));
    }
    model.expiry_layers_.push_back(model.layers_.size() - 1);
  }

  // Forward probability propagation reports numerical boundary exposure using
  // the SAME operator as pricing. This is compilation work, not serving work.
  std::vector<double> mass(model.diagnostics_.max_states, 0.0);
  std::vector<double> next_mass(model.diagnostics_.max_states, 0.0);
  mass[model.initial_state_index_] = 1.0;
  for (const auto &transition : model.transitions_) {
    std::fill(next_mass.begin(), next_mass.end(), 0.0);
    const auto rows = transition.rows();
    if (transition.kind == AmericanMarkovStepKind::VarianceResolvent) {
      const auto count = transition.source_state_count();
      if (!detail::resolvent_transpose(transition.resolvent_factors(),
                                       std::span<const double>(mass).first(count),
                                       std::span<double>(next_mass).first(count))) {
        return Err(ErrorCode::OutOfRange, "American Markov: resolvent forward action overflow");
      }
    } else {
      for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        for (std::size_t j = 0; j < 3; ++j) {
          next_mass[row.destination[j]] += mass[i] * row.probability[j];
        }
      }
    }
    double total_mass = 0.0;
    for (std::size_t i = 0; i < transition.target_state_count(); ++i) {
      if (resolvent && !nonnegative(next_mass[i])) {
        return Err(ErrorCode::OutOfRange, "American Markov: invalid resolvent forward probability");
      }
      total_mass += next_mass[i];
    }
    const double mass_error = std::abs(total_mass - 1.0);
    if (resolvent &&
        (!nonnegative(mass_error) || mass_error > kAmericanMarkovResolventMaxActionError)) {
      return Err(ErrorCode::OutOfRange,
                 "American Markov: resolvent forward mass residual exceeded");
    }
    model.diagnostics_.max_forward_mass_error =
        std::max(model.diagnostics_.max_forward_mass_error, mass_error);
    mass.swap(next_mass);
  }
  model.diagnostics_.terminal_zero_probability = mass.front();
  model.diagnostics_.terminal_upper_probability =
      model.layers_.back().states().size() > 1 ? mass[model.layers_.back().states().size() - 1] : 0.0;
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
  const auto end_states = end.states();
  auto *values = &workspace.first_;
  auto *scratch = &workspace.second_;
  for (std::size_t i = 0; i < end_states.size(); ++i) {
    (*values)[i] = intrinsic(end.growth * end_states[i], strike, side);
  }
  for (std::size_t n = terminal; n > 0; --n) {
    const auto &transition = model.transitions()[n - 1];
    const auto &layer = model.layers()[n - 1];
    const auto rows = transition.rows();
    const auto states = layer.states();
    if (transition.kind == AmericanMarkovStepKind::VarianceResolvent) {
      const bool finite = detail::resolvent_rollback(
          transition.resolvent_factors(), values->data(), scratch->data(), 1, 1,
          [&](double continuation, std::size_t i, std::size_t) noexcept {
            const double discounted = continuation * transition.discount;
            return exercise == ExerciseStyle::American
                       ? std::max(discounted, intrinsic(layer.growth * states[i], strike, side))
                       : discounted;
          });
      if (!finite) return Err(ErrorCode::OutOfRange, "American Markov: option value overflow");
      std::swap(values, scratch);
      continue;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const auto &row = rows[i];
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
              ? std::max(continuation, intrinsic(layer.growth * states[i], strike, side))
              : continuation;
    }
    std::swap(values, scratch);
  }
  return Ok((*values)[model.initial_state_index()]);
}

} // namespace atx::vol
