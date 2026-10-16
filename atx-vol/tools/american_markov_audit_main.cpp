#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "atx/vol/api/pricing/american_markov_batch.hpp"
#include "atx/vol/api/pricing/american_markov_grid.hpp"
#include "atx/vol/api/pricing/american_markov_pwl.hpp"

namespace {
using namespace atx::vol;
using Json = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;

struct Fixture {
  std::string name;
  double spot = 100.0;
  std::vector<AmericanMarkovInterval> intervals;
};

struct Grid {
  std::string name;
  std::size_t nodes;
  double upper_multiple;
  double max_step;
  bool logarithmic;
  double concentration_scale = 0.0;
  bool first_variance_scale = false;
  bool match_reference_spacing = false;
  double time_refinement = 1.0;
};

double elapsed_us(Clock::time_point start) {
  return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

std::vector<Fixture> fixtures() {
  return {
      {"martingale", 100.0, {{0.25, 0.25, 0.0, 0.0, 0.2, 0.0}}},
      {"funded", 100.0, {{0.25, 0.25, 0.05, 0.0, 0.2, 0.0}}},
      {"separate_clocks", 100.0, {{0.5, 0.25, 0.03, 0.01, 0.2, 0.0}}},
      {"short_low_vol", 100.0, {{7.0 / 365.0, 1.0 / 252.0, 0.04, 0.01, 0.02, 0.0}}},
      {"high_vol", 100.0, {{1.0, 1.0, 0.02, 0.0, 0.8, 0.0}}},
      {"negative_rate", 100.0, {{0.75, 0.5, -0.03, 0.01, 0.3, 0.0}}},
      {"negative_rate_and_yield", 100.0, {{0.75, 0.5, -0.03, -0.06, 0.3, 0.0}}},
      {"dated_cash", 100.0,
       {{0.17, 0.10, 0.04, 0.01, 0.25, 2.0},
        {0.30, 0.20, 0.03, 0.01, 0.30, 0.0},
        {0.60, 0.45, 0.03, 0.01, 0.28, 1.0}}},
      {"near_cash", 100.0,
       {{0.001, 0.0006, 0.03, 0.0, 0.3, 3.0}, {0.04, 0.03, 0.03, 0.0, 0.3, 0.0}}},
      {"clock_pause", 100.0,
       {{0.02, 0.01, 0.03, 0.01, 0.3, 0.0},
        {0.03, 0.01, -0.01, 0.02, 0.3, 0.0},
        {0.10, 0.07, 0.03, 0.01, 0.3, 0.0}}},
      {"deterministic_cash", 100.0,
       {{0.17, 0.10, -0.03, -0.06, 0.0, 2.0},
        {0.30, 0.20, 0.03, 0.01, 0.0, 0.0},
        {0.60, 0.45, -0.02, 0.01, 0.0, 120.0}}}};
}

std::vector<Grid> grids(bool full) {
  std::vector<Grid> result{{"default65", 65u, 4.0, 1.0 / 52.0, false}};
  if (full) {
    result.insert(result.end(), {{"uniform129", 129u, 4.0, 1.0 / 104.0, false},
                                 {"uniform257", 257u, 4.0, 1.0 / 208.0, false},
                                 {"domain2", 33u, 2.0, 1.0 / 52.0, false},
                                 {"domain8", 129u, 8.0, 1.0 / 52.0, false},
                                 {"log65", 65u, 4.0, 1.0 / 52.0, true},
                                 {"log129", 129u, 4.0, 1.0 / 104.0, true},
                                 {"log257", 257u, 4.0, 1.0 / 208.0, true},
                                 {"sinh65-wide", 65u, 4.0, 1.0 / 52.0, false, 0.20},
                                 {"sinh129-wide", 129u, 4.0, 1.0 / 104.0, false, 0.20},
                                 {"sinh65", 65u, 4.0, 1.0 / 52.0, false, 0.05},
                                 {"sinh129", 129u, 4.0, 1.0 / 104.0, false, 0.05},
                                 {"sinh129-tight", 129u, 4.0, 1.0 / 104.0, false, 0.005},
                                 {"sinh65-variance", 65u, 4.0, 1.0 / 52.0, false, 0.05, true},
                                 {"sinh129-variance", 129u, 4.0, 1.0 / 104.0, false, 0.05, true},
                                 {"sinh129-variance-domain8", 129u, 8.0, 1.0 / 104.0, false, 0.05, true},
                                 {"sinh65-variance-domain16", 65u, 16.0, 1.0 / 52.0, false, 0.05, true},
                                 {"sinh129-variance-domain16", 129u, 16.0, 1.0 / 104.0,
                                  false, 0.05, true},
                                 {"sinh-match-domain8", 129u, 8.0, 1.0 / 104.0,
                                  false, 0.05, true, true},
                                 {"sinh-match-domain16", 129u, 16.0, 1.0 / 104.0,
                                  false, 0.05, true, true},
                                 {"sinh129-variance-time2", 129u, 4.0, 1.0 / 104.0,
                                  false, 0.05, true, false, 2.0},
                                 {"sinh129-variance-time4", 129u, 4.0, 1.0 / 104.0,
                                  false, 0.05, true, false, 4.0},
                                 {"sinh161-variance-domain8", 161u, 8.0, 1.0 / 104.0,
                                  false, 0.05, true},
                                 {"sinh193-variance-domain8", 193u, 8.0, 1.0 / 104.0,
                                  false, 0.05, true},
                                 {"sinh129-domain8-time2", 129u, 8.0, 1.0 / 104.0,
                                  false, 0.05, true, false, 2.0},
                                 {"sinh129-domain8-time3", 129u, 8.0, 1.0 / 104.0,
                                  false, 0.05, true, false, 3.0},
                                 {"sinh129-domain8-clock4800", 129u, 8.0, 1.0 / 4800.0,
                                  false, 0.05, true},
                                 {"sinh161-domain8-clock4800", 161u, 8.0, 1.0 / 4800.0,
                                  false, 0.05, true},
                                 {"sinh193-domain8-clock4800", 193u, 8.0, 1.0 / 4800.0,
                                  false, 0.05, true}});
  }
  return result;
}

std::vector<double> log_grid(double spot, const Grid &grid) {
  if (!grid.logarithmic) return {};
  std::vector<double> states{0.0, spot};
  const double upper = std::log(grid.upper_multiple);
  for (std::size_t i = 0u; i + 1u < grid.nodes; ++i) {
    const double weight = static_cast<double>(i) / static_cast<double>(grid.nodes - 2u);
    states.push_back(spot * std::exp(std::lerp(-3.0, upper, weight)));
  }
  std::sort(states.begin(), states.end());
  states.erase(std::unique(states.begin(), states.end()), states.end());
  return states;
}

double intrinsic(double stock, double strike, Side side) {
  return std::max(side == Side::Call ? stock - strike : strike - stock, 0.0);
}

// Independent direct lognormal formula, without any library pricing routine.
// Applicable only when every cash event through this expiry is zero.
double european_reference(const Fixture &fixture, std::size_t expiry, double strike, Side side) {
  double rate_integral = 0.0;
  double yield_integral = 0.0;
  double variance = 0.0;
  double previous_calendar = 0.0;
  double previous_variance = 0.0;
  for (std::size_t i = 0u; i <= expiry; ++i) {
    const auto &interval = fixture.intervals[i];
    const double dt = interval.calendar_end - previous_calendar;
    rate_integral += interval.rate * dt;
    yield_integral += interval.continuous_yield * dt;
    variance += interval.volatility * interval.volatility *
                (interval.variance_end - previous_variance);
    previous_calendar = interval.calendar_end;
    previous_variance = interval.variance_end;
  }
  const double stock_pv = fixture.spot * std::exp(-yield_integral);
  const double strike_pv = strike * std::exp(-rate_integral);
  if (variance == 0.0) return intrinsic(stock_pv, strike_pv, side);
  const double sd = std::sqrt(variance);
  const double d1 = (std::log(fixture.spot / strike) + rate_integral - yield_integral +
                     0.5 * variance) / sd;
  const double d2 = d1 - sd;
  const auto normal = [](double x) { return 0.5 * std::erfc(-x / std::numbers::sqrt2); };
  return side == Side::Call ? stock_pv * normal(d1) - strike_pv * normal(d2)
                            : strike_pv * normal(-d2) - stock_pv * normal(-d1);
}

// Exact continuous-time deterministic stopping value: each interval's optimum
// is at an endpoint or the single stationary point of discounted intrinsic.
// Check both sides of every limited-liability cash event.
double deterministic_reference(const Fixture &fixture, std::size_t expiry,
                               double strike, Side side) {
  double stock = fixture.spot;
  double discount = 1.0;
  double previous = 0.0;
  double best = intrinsic(stock, strike, side);
  for (std::size_t i = 0u; i <= expiry; ++i) {
    const auto &interval = fixture.intervals[i];
    const double dt = interval.calendar_end - previous;
    const auto candidate = [&](double time) {
      const double s = stock * std::exp((interval.rate - interval.continuous_yield) * time);
      return discount * std::exp(-interval.rate * time) * intrinsic(s, strike, side);
    };
    best = std::max({best, candidate(0.0), candidate(dt)});
    const double numerator = interval.rate * strike;
    const double denominator = interval.continuous_yield * stock;
    if (numerator * denominator > 0.0 && interval.rate != interval.continuous_yield) {
      const double stationary = std::log(numerator / denominator) /
                                (interval.rate - interval.continuous_yield);
      if (stationary > 0.0 && stationary < dt) best = std::max(best, candidate(stationary));
    }
    stock = std::max(stock * std::exp((interval.rate - interval.continuous_yield) * dt) -
                       interval.cash_dividend_at_end, 0.0);
    discount *= std::exp(-interval.rate * dt);
    best = std::max(best, discount * intrinsic(stock, strike, side));
    previous = interval.calendar_end;
  }
  return best;
}

std::array<double, 36> strike_samples(double spot) {
  constexpr std::size_t regular_strikes = 33;
  std::array<double, regular_strikes + 3> strikes{};
  for (std::size_t i = 0u; i < regular_strikes; ++i) {
    strikes[i] = spot * (0.4 + 1.4 * static_cast<double>(i) /
                                 static_cast<double>(regular_strikes - 1u));
  }
  // Retain the initial payoff kink and adjacent probes, even on a refused grid.
  strikes[regular_strikes] = spot * (1.0 - 1.0e-6);
  strikes[regular_strikes + 1u] = spot;
  strikes[regular_strikes + 2u] = spot * (1.0 + 1.0e-6);
  std::sort(strikes.begin(), strikes.end());
  return strikes;
}

Json describe(const Fixture &fixture, const Grid &grid) {
  Json intervals = Json::array();
  for (const auto &i : fixture.intervals) {
    intervals.push_back({{"calendar_end", i.calendar_end}, {"variance_end", i.variance_end},
                         {"rate", i.rate}, {"continuous_yield", i.continuous_yield},
                         {"volatility", i.volatility}, {"cash_at_end", i.cash_dividend_at_end}});
  }
  return {{"fixture", fixture.name}, {"spot", fixture.spot}, {"intervals", intervals},
           {"grid", grid.name}, {"requested_nodes", grid.nodes},
           {"upper_multiple", grid.upper_multiple},
           {"configured_max_clock_step", grid.max_step},
           {"max_clock_step", grid.max_step / grid.time_refinement},
           {"logarithmic", grid.logarithmic}, {"configured_concentration_scale", grid.concentration_scale},
           {"requested_time_refinement", grid.time_refinement},
           {"requested_spacing_match", grid.match_reference_spacing},
           {"concentration_rule", grid.first_variance_scale
                ? "first positive cumulative fixture variance standard deviation; fixed fallback"
                : "fixed"},
           {"requested_strikes", strike_samples(fixture.spot)}};
}

Result<AmericanMarkovStateGrid> resolve_sinh_grid(double spot,
    const AmericanMarkovSinhGridConfig &config, bool match_spacing, Json &result) {
  if (!match_spacing) return make_american_markov_sinh_grid(spot, config);
  auto reference_config = config;
  reference_config.grid_nodes = 129;
  reference_config.upper_spot_multiple = 4.0;
  auto &match = result["spacing_match"];
  match = {{"reference_grid", "sinh129-variance"}, {"reference_nodes", 129},
           {"reference_upper_multiple", 4.0}, {"minimum_nodes", config.grid_nodes},
           {"maximum_nodes", config.max_states}, {"candidate_evaluations", 0},
           {"rule", "first ascending node count with both spot gaps <= reference gaps"}};
  const auto reference = make_american_markov_sinh_grid(spot, reference_config);
  if (!reference) {
    match["status"] = "reference_refused";
    return atx::core::Err(reference.error());
  }
  match["reference_lower_gap_over_spot"] = reference->lower_spot_gap_over_spot;
  match["reference_upper_gap_over_spot"] = reference->upper_spot_gap_over_spot;
  match["reference_concentration_scale"] = config.concentration_scale;
  auto trial_config = config;
  // Integer allocation of left/right nodes can make individual spot gaps
  // nonmonotone in node count. Ascending bounded search needs no such assumption.
  for (std::size_t nodes = config.grid_nodes; nodes <= config.max_states; ++nodes) {
    trial_config.grid_nodes = nodes;
    match["candidate_evaluations"] = nodes - config.grid_nodes + 1;
    auto trial = make_american_markov_sinh_grid(spot, trial_config);
    if (!trial) {
      match["status"] = "candidate_refused";
      match["refused_nodes"] = nodes;
      return atx::core::Err(trial.error());
    }
    if (trial->lower_spot_gap_over_spot <= reference->lower_spot_gap_over_spot &&
        trial->upper_spot_gap_over_spot <= reference->upper_spot_gap_over_spot) {
      match["status"] = "matched";
      match["resolved_nodes"] = nodes;
      return trial;
    }
  }
  match["status"] = "search_exhausted";
  return atx::core::Err(ErrorCode::OutOfRange,
                        "audit grid: spacing match exceeds unchanged state budget");
}

const char *method_name(AmericanMarkovDiffusionMethod method) {
  return method == AmericanMarkovDiffusionMethod::ExplicitEuler
             ? "explicit-euler" : "variance-resolvent";
}

Json run(const Fixture &fixture, const Grid &grid, bool pricing_only,
         AmericanMarkovDiffusionMethod method) {
  Json result = describe(fixture, grid);
  result["diffusion_method"] = method_name(method);
  auto states = log_grid(fixture.spot, grid);
  double concentration_scale = grid.concentration_scale;
  if (grid.first_variance_scale) {
    double previous = 0.0;
    double integrated_variance = 0.0;
    for (const auto &interval : fixture.intervals) {
      integrated_variance += interval.volatility * interval.volatility *
                             (interval.variance_end - previous);
      if (integrated_variance > 0.0) {
        concentration_scale = std::sqrt(integrated_variance);
        break;
      }
      previous = interval.variance_end;
    }
  }
  result["concentration_scale"] = concentration_scale;
  if (grid.concentration_scale > 0.0) {
    AmericanMarkovSinhGridConfig grid_config;
    grid_config.grid_nodes = grid.nodes;
    grid_config.upper_spot_multiple = grid.upper_multiple;
    grid_config.concentration_scale = concentration_scale;
    auto generated = resolve_sinh_grid(fixture.spot, grid_config,
                                        grid.match_reference_spacing, result);
    if (!generated) {
      result["model_status"] = "grid_refused";
      result["model_error"] = generated.error().to_string();
      return result;
    }
    result["grid_diagnostics"] = {
        {"lower_spot_gap_over_spot", generated->lower_spot_gap_over_spot},
        {"upper_spot_gap_over_spot", generated->upper_spot_gap_over_spot},
        {"min_gap_over_spot", generated->min_gap_over_spot},
        {"max_unit_volatility_exit_rate", generated->max_unit_volatility_exit_rate}};
    states = std::move(generated->states);
  }
  if (!states.empty()) {
    result["resolved_state_grid"] = states;
    result["resolved_nodes"] = states.size();
  }
  AmericanMarkovConfig config;
  config.diffusion_method = method;
  config.grid_nodes = grid.nodes;
  config.upper_spot_multiple = grid.upper_multiple;
  config.max_calendar_step = grid.max_step / grid.time_refinement;
  config.max_variance_step = grid.max_step / grid.time_refinement;
  config.cfl_fraction /= grid.time_refinement;
  // Keep the public defaults, including their resource limits, for every grid.
  result["model_limits"] = {{"max_states", config.max_states},
                              {"max_transitions", config.max_transitions},
                              {"max_stored_rows", config.max_stored_rows},
                              {"max_transition_work", config.max_transition_work}};
  result["resolved_model_controls"] = {
      {"diffusion_method", method_name(method)},
      {"max_calendar_step", config.max_calendar_step},
      {"max_variance_step", config.max_variance_step}, {"cfl_fraction", config.cfl_fraction},
      {"max_states", config.max_states}, {"max_transitions", config.max_transitions},
      {"max_stored_rows", config.max_stored_rows},
      {"max_transition_work", config.max_transition_work},
      {"max_stored_volatility_values", config.max_stored_volatility_values}};
  auto start = Clock::now();
  auto model = compile_american_markov({fixture.spot, fixture.intervals, states, config});
  result["compile_us"] = elapsed_us(start);
  if (!model) {
    result["model_status"] = "refused";
    result["model_error"] = model.error().to_string();
    return result;
  }
  result["model_status"] = "ok";
  std::vector<std::size_t> interval_diffusion_steps(fixture.intervals.size(), 0u);
  for (const auto &transition : model->transitions()) {
    if (transition.kind == AmericanMarkovStepKind::Diffusion ||
        transition.kind == AmericanMarkovStepKind::VarianceResolvent) {
      ++interval_diffusion_steps[transition.interval_index];
    }
  }
  Json time_intervals = Json::array();
  double previous_calendar = 0.0;
  double previous_variance = 0.0;
  for (std::size_t i = 0u; i < fixture.intervals.size(); ++i) {
    const auto &interval = fixture.intervals[i];
    // A successful compilation already bounded these ceilings by the unchanged
    // transition cap. Counts determine both exact linear clock subdivisions.
    const auto calendar_minimum = static_cast<std::size_t>(
        std::ceil((interval.calendar_end - previous_calendar) / config.max_calendar_step));
    const auto variance_minimum = static_cast<std::size_t>(
        std::ceil((interval.variance_end - previous_variance) / config.max_variance_step));
    const auto clock_minimum = std::max({std::size_t{1}, calendar_minimum, variance_minimum});
    time_intervals.push_back({{"interval_index", i},
                              {"calendar_clock_minimum_steps", calendar_minimum},
                              {"variance_clock_minimum_steps", variance_minimum},
                              {"resolved_diffusion_steps", interval_diffusion_steps[i]},
                              {"exceeds_clock_minimum", interval_diffusion_steps[i] > clock_minimum}});
    previous_calendar = interval.calendar_end;
    previous_variance = interval.variance_end;
  }
  result["resolved_time_intervals"] = std::move(time_intervals);
  const auto initial_states = model->layers().front().states();
  result["resolved_state_grid"] = std::vector<double>(initial_states.begin(), initial_states.end());
  result["resolved_nodes"] = initial_states.size();
  const auto &d = model->diagnostics();
  result["model"] = {{"diffusion_method", method_name(method)},
                       {"states", d.max_states}, {"diffusion_steps", d.diffusion_steps},
                       {"cash_steps", d.cash_steps}, {"stored_rows", d.stored_rows},
                       {"transition_work", d.transition_work},
                       {"resolvent_steps", d.resolvent_steps},
                       {"physical_resolvent_factors", d.physical_resolvent_factors},
                       {"resolvent_factor_blocks", d.resolvent_factor_blocks},
                       {"physical_state_values", d.physical_state_values},
                       {"state_grid_blocks", d.state_grid_blocks},
                       {"physical_transition_rows", d.physical_transition_rows},
                       {"transition_row_blocks", d.transition_row_blocks},
                       {"max_probability_error", d.max_probability_sum_error},
                       {"max_mean_error", d.max_relative_mean_error},
                       {"max_forward_mass_error", d.max_forward_mass_error},
                       {"terminal_zero_probability", d.terminal_zero_probability},
                       {"terminal_upper_probability", d.terminal_upper_probability}};
  const auto strikes = strike_samples(fixture.spot);
  AmericanMarkovWorkspace scalar;
  AmericanMarkovBatchWorkspace batch;
  const auto scalar_ready = scalar.prepare(*model);
  const auto batch_ready = batch.prepare(*model, strikes.size());
  if (!scalar_ready || !batch_ready) {
    result["pricing_status"] = "workspace_refused";
    return result;
  }
  double max_batch_error = 0.0;
  double max_european_error = 0.0;
  double max_deterministic_error = 0.0;
  double scalar_us = 0.0;
  double batch_us = 0.0;
  std::size_t european_count = 0u;
  std::size_t deterministic_count = 0u;
  Json prices = Json::array();
  for (std::size_t expiry = 0u; expiry < fixture.intervals.size(); ++expiry) {
    const auto prefix =
        std::span<const AmericanMarkovInterval>(fixture.intervals).first(expiry + 1u);
    const bool no_cash = std::all_of(prefix.begin(), prefix.end(), [](const auto &i) {
      return i.cash_dividend_at_end == 0.0;
    });
    const bool deterministic = std::all_of(prefix.begin(), prefix.end(), [](const auto &i) {
      return i.volatility == 0.0;
    });
    for (const Side side : {Side::Call, Side::Put}) {
      for (const ExerciseStyle style : {ExerciseStyle::American, ExerciseStyle::European}) {
        std::array<double, strikes.size()> output{};
        start = Clock::now();
        const auto status = price_american_markov_batch(*model, strikes, side, expiry,
                                                       output, batch, style);
        batch_us += elapsed_us(start);
        if (!status) {
          result["pricing_status"] = "batch_refused";
          result["pricing_error"] = status.error().to_string();
          return result;
        }
        start = Clock::now();
        for (std::size_t k = 0u; k < strikes.size(); ++k) {
          const auto value = price_american_markov(*model, strikes[k], side, expiry, scalar, style);
          if (!value) {
            result["pricing_status"] = "scalar_refused";
            result["pricing_error"] = value.error().to_string();
            return result;
          }
          max_batch_error = std::max(max_batch_error, std::abs(output[k] - *value));
        }
        scalar_us += elapsed_us(start);
        for (std::size_t k = 0u; k < strikes.size(); ++k) {
          if (no_cash && style == ExerciseStyle::European) {
            max_european_error = std::max(max_european_error,
                std::abs(output[k] - european_reference(fixture, expiry, strikes[k], side)));
            ++european_count;
          }
          if (deterministic && style == ExerciseStyle::American) {
            max_deterministic_error = std::max(max_deterministic_error,
                std::abs(output[k] - deterministic_reference(fixture, expiry, strikes[k], side)));
            ++deterministic_count;
          }
          prices.push_back({{"expiry", expiry}, {"strike", strikes[k]},
                             {"side", side == Side::Call ? "call" : "put"},
                             {"exercise", style == ExerciseStyle::American
                                              ? "american" : "european"},
                             {"price", output[k]}});
        }
      }
    }
  }
  result["pricing_status"] = "ok";
  result["scalar_us"] = scalar_us;
  result["batch_us"] = batch_us;
  result["max_scalar_batch_error"] = max_batch_error;
  result["european_reference_count"] = european_count;
  result["max_european_reference_error"] =
      european_count ? Json(max_european_error) : Json(nullptr);
  result["deterministic_reference_count"] = deterministic_count;
  result["max_deterministic_reference_error"] = deterministic_count
                                                    ? Json(max_deterministic_error) : Json(nullptr);
  if (pricing_only) {
    result["pwl_status"] = "not_requested";
    result["prices"] = std::move(prices);
    return result;
  }
  start = Clock::now();
  const auto surface =
      compile_american_markov_pwl(std::move(*model), strikes.front(), strikes.back());
  result["pwl_compile_us"] = elapsed_us(start);
  if (!surface) {
    result["pwl_status"] = "refused";
    result["pwl_error"] = surface.error().to_string();
  } else {
    result["pwl_status"] = "ok";
    const auto &pd = surface->diagnostics();
    result["pwl"] = {{"operations", pd.algebra_operations}, {"input_nodes", pd.input_nodes},
                       {"generated_nodes", pd.generated_nodes}, {"output_nodes", pd.output_nodes},
                       {"peak_live_nodes", pd.peak_live_nodes},
                       {"max_function_nodes", pd.max_function_nodes},
                       {"max_output_crossing_resolution_error",
                        pd.max_output_crossing_resolution_error}};
    double max_pwl_error = 0.0;
    start = Clock::now();
    for (const auto &row : prices) {
      if (row["exercise"] != "american") continue;
      const auto side = row["side"] == "call" ? Side::Call : Side::Put;
      const auto value = surface->price(row["strike"].get<double>(), side,
                                        row["expiry"].get<std::size_t>());
      if (!value) {
        result["pwl_status"] = "query_refused";
        break;
      }
      max_pwl_error = std::max(max_pwl_error, std::abs(*value - row["price"].get<double>()));
    }
    result["pwl_query_with_json_us"] = elapsed_us(start);
    result["max_pwl_batch_error"] = max_pwl_error;
  }
  result["prices"] = std::move(prices);
  return result;
}
} // namespace

int main(int argc, char **argv) {
  try {
    bool full = false;
    bool pricing_only = false;
    std::string fixture_filter;
    std::string grid_filter;
    auto method = AmericanMarkovDiffusionMethod::ExplicitEuler;
    bool method_set = false;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg(argv[i]);
      if (arg == "--full" && !full) full = true;
      else if (arg == "--pricing-only" && !pricing_only) pricing_only = true;
      else if (arg == "--case" && i + 1 < argc && fixture_filter.empty()) {
        fixture_filter = argv[++i];
      } else if (arg == "--grid" && i + 1 < argc && grid_filter.empty()) {
        grid_filter = argv[++i];
      } else if (arg == "--diffusion-method" && i + 1 < argc && !method_set) {
        const std::string_view value(argv[++i]);
        if (value == "variance-resolvent") method = AmericanMarkovDiffusionMethod::VarianceResolvent;
        else if (value != "explicit-euler") {
          throw std::invalid_argument("diffusion method must be explicit-euler or variance-resolvent");
        }
        method_set = true;
      } else {
        throw std::invalid_argument(
            "usage: american-markov-audit [--full] [--pricing-only] [--case NAME] [--grid NAME] "
            "[--diffusion-method explicit-euler|variance-resolvent]");
      }
    }
    Json report{{"schema_version", 2},
                 {"pricing_only", pricing_only},
                 {"diffusion_method", method_name(method)},
                 {"diffusion_scope", "explicit Euler uses CFL subdivision; variance resolvent "
                    "randomizes latent variance within deterministic clock steps and ignores CFL subdivision"},
                 {"requested_sides", {"call", "put"}},
                 {"requested_exercises", {"american", "european"}},
                 {"strike_sampling", "33 regular strikes plus spot and spot*(1+/-1e-6)"},
                 {"purpose", "synthetic numerical diagnostics; timings are not a benchmark claim"},
                 {"model_scope",
                  "finite Bermudan; absorbing upper boundary; limited-liability cash"},
                 {"reference_scope",
                  "independent lognormal European and continuous deterministic stopping"},
                 {"pwl_limits", "public defaults; represented knots retained; "
                    "unrepresentable crossings have a propagated fixed error allowance"},
                 {"cases", Json::array()}};
    const auto all_fixtures = fixtures();
    const auto all_grids = grids(full);
    for (const auto &fixture : all_fixtures) {
      if (!fixture_filter.empty() && fixture.name != fixture_filter) continue;
      for (const auto &grid : all_grids) {
        if (!grid_filter.empty() && grid.name != grid_filter) continue;
        report["cases"].push_back(run(fixture, grid, pricing_only, method));
      }
    }
    if (report["cases"].empty()) throw std::invalid_argument("no fixture/grid matched");
    std::cout << report.dump(2) << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << Json{{"fatal_error", error.what()}}.dump() << '\n';
    return 1;
  }
}
