#include "analytics/historical_projection.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <string>

#include "atx/core/error.hpp"
#include "atx/core/hash.hpp"
#include "core/parallel_for.hpp"

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

Result<PreparedHistoricalProjection>
PreparedHistoricalProjection::create(std::span<const RelativeOptionPosition> positions) {
  if (positions.empty())
    return Err(ErrorCode::InvalidArgument, "historical projection: empty positions");
  std::vector<OptionProjectionSpec> specs;
  specs.reserve(positions.size());
  std::string fingerprint_material;
  fingerprint_material.reserve(positions.size() * sizeof(std::uint64_t) * 2u);
  for (const RelativeOptionPosition &position : positions) {
    if (!std::isfinite(position.quantity))
      return Err(ErrorCode::InvalidArgument, "historical projection: non-finite quantity");
    specs.push_back(position.option);
    const std::uint64_t quantity_bits = std::bit_cast<std::uint64_t>(position.quantity);
    fingerprint_material.append(reinterpret_cast<const char *>(&quantity_bits),
                                sizeof quantity_bits);
  }
  ATX_TRY(PreparedOptionProjection projection, PreparedOptionProjection::create(specs));
  const std::uint64_t projection_fingerprint = projection.fingerprint();
  fingerprint_material.append(reinterpret_cast<const char *>(&projection_fingerprint),
                              sizeof projection_fingerprint);

  PreparedHistoricalProjection result;
  result.positions_.assign(positions.begin(), positions.end());
  result.projection_ = std::move(projection);
  result.fingerprint_ =
      atx::core::hash_bytes(fingerprint_material.data(), fingerprint_material.size());
  if (result.fingerprint_ == 0u)
    result.fingerprint_ = 1u;
  return Ok(std::move(result));
}

Status PreparedHistoricalProjection::evaluate_into(
    std::span<const HistoricalProjectionScenario> scenarios,
    std::span<HistoricalProjectionFrame> frames, std::span<ProjectedOption> leg_output,
    const HistoricalProjectionConfig &config, QueryExecution execution) const {
  if (frames.size() != scenarios.size() ||
      leg_output.size() != scenarios.size() * positions_.size() ||
      !(std::isfinite(config.delta_tolerance) && config.delta_tolerance > 0.0 &&
        config.delta_tolerance <= 1.0e-3)) {
    return Err(ErrorCode::InvalidArgument, "historical projection: invalid output/config");
  }
  for (const HistoricalProjectionScenario &scenario : scenarios) {
    if (scenario.ts_ns <= 0 || scenario.surfaces == nullptr)
      return Err(ErrorCode::InvalidArgument, "historical projection: invalid scenario");
  }

  parallel_for_dynamic(scenarios.size(), config.n_threads, [&](std::size_t scenario_index) {
    const HistoricalProjectionScenario &scenario = scenarios[scenario_index];
    std::span<ProjectedOption> legs =
        leg_output.subspan(scenario_index * positions_.size(), positions_.size());
    OptionProjectionConfig projection_config;
    projection_config.output = OptionProjectionOutput::FullGreeks;
    projection_config.analytic_greeks = config.analytic_greeks;
    projection_config.delta_tolerance = config.delta_tolerance;
    projection_config.n_threads = 1u;
    projection_config.query_execution = execution;
    const Status status = projection_.project_into(*scenario.surfaces, legs, projection_config);

    HistoricalProjectionFrame frame;
    frame.ts_ns = scenario.ts_ns;
    if (!status) {
      frame.n_failed = static_cast<std::uint32_t>(positions_.size());
      frame.value = frame.delta = frame.gamma = frame.vega = frame.theta =
          std::numeric_limits<double>::quiet_NaN();
      frames[scenario_index] = frame;
      return;
    }

    std::size_t aggregate_fingerprint = static_cast<std::size_t>(fingerprint_);
    for (std::size_t leg_index = 0; leg_index < legs.size(); ++leg_index) {
      const ProjectedOption &leg = legs[leg_index];
      if (leg.status != OptionProjectionStatus::Ok ||
          leg.definition.valuation_ts_ns != scenario.ts_ns) {
        ++frame.n_failed;
        continue;
      }
      ++frame.n_ok;
      const double scale = positions_[leg_index].quantity * leg.definition.multiplier;
      frame.value += scale * leg.model_mark;
      frame.delta += scale * leg.greeks.delta;
      frame.gamma += scale * leg.greeks.gamma;
      frame.vega += scale * leg.greeks.vega;
      frame.theta += scale * leg.greeks.theta;
      aggregate_fingerprint =
          atx::core::hash_combine(aggregate_fingerprint, leg.definition.fingerprint);
    }
    if (frame.n_failed != 0u) {
      frame.value = frame.delta = frame.gamma = frame.vega = frame.theta =
          std::numeric_limits<double>::quiet_NaN();
    } else {
      frame.definition_fingerprint = static_cast<std::uint64_t>(aggregate_fingerprint);
      if (frame.definition_fingerprint == 0u)
        frame.definition_fingerprint = 1u;
    }
    frames[scenario_index] = frame;
  });
  return Ok();
}

Result<ProjectedHistoricalVar>
projected_historical_var(std::span<const HistoricalProjectionFrame> frames, double reference_value,
                         double confidence) {
  if (!std::isfinite(reference_value) || !std::isfinite(confidence) || confidence <= 0.0 ||
      confidence >= 1.0) {
    return Err(ErrorCode::InvalidArgument, "historical projection VaR: invalid input");
  }
  std::vector<double> losses;
  losses.reserve(frames.size());
  for (const HistoricalProjectionFrame &frame : frames) {
    if (frame.n_failed == 0u && frame.n_ok != 0u && frame.definition_fingerprint != 0u &&
        std::isfinite(frame.value))
      losses.push_back(reference_value - frame.value);
  }
  if (losses.empty())
    return Err(ErrorCode::Unavailable, "historical projection VaR: no successful scenarios");
  std::sort(losses.begin(), losses.end());
  const std::size_t rank =
      static_cast<std::size_t>(std::ceil(confidence * static_cast<double>(losses.size())));
  const std::size_t index = std::min(losses.size() - 1u, rank == 0u ? 0u : rank - 1u);
  double tail_sum = 0.0;
  for (std::size_t i = index; i < losses.size(); ++i)
    tail_sum += losses[i];

  ProjectedHistoricalVar result;
  result.confidence = confidence;
  result.reference_value = reference_value;
  result.value_at_risk = losses[index];
  result.expected_shortfall = tail_sum / static_cast<double>(losses.size() - index);
  result.n_scenarios = losses.size();
  return Ok(result);
}

} // namespace atx::vol
